// Host test of the hub E-Stop as a kill path. It compiles the real src/EStopManager.cpp into this file, so a scenario
// can wipe that file's RAM state as a restart would and leave the retained latch in place. It links the real
// `HandleCommand`, `SafetyCaps`, `RFTransmitter` and its encoders, the real serial `estop`, `rftxpin` and `safetycaps`
// commands, the real gateway `Trigger` and the real `WebSocketDeFragger`. Fakes: GPIO, RMT, the `Config` store, NVS,
// FreeRTOS, the reset reason, the number parser in `Convert` and `Chipset`, whose pin checks accept 0 to 39 instead of
// the board's own unsafe and bypassed pins. src/serial/command_handlers/safetycaps.cpp is compiled in here too, because
// its handlers are static.
//
// The FreeRTOS fake runs each task on its own thread with one thread awake at a time, and `advance` steps every task
// in 5 millisecond ticks, so a run is deterministic. The E-Stop checker runs its real loop with its real local state.
//
// kill_stubs holds symlinks to the real TinyVec.h and WebSocketDeFragger.h. A quoted include looks beside the including
// file first, so through include/ they would reach the real Logging.h and Common.h next to them instead of the stubs.
//
// Run from the repository root after `pio pkg install -e Pishock-2023`. The second and third lines set a PiShock
// layout: an ESP32 with the LED on GPIO 2. Build it once per PiShock RF TX default, TX=12 for Pishock-2023 and TX=15
// for Pishock-Lite-2021, because the E-Stop pin rule must not depend on either.
//
//   FB=.pio/libdeps/Pishock-2023/flatbuffers/include
//   FLAGS="-std=c++20 -O1 -pthread -DSOFTPROXY_KILL_TEST -DOPENSHOCK_FW_CHIP_ESP32 -DOPENSHOCK_LED_GPIO=2 -DSOFTPROXY_PISHOCK_BUILD"
//   FLAGS="$FLAGS -DOPENSHOCK_RF_TX_GPIO=$TX"
//   INC="-Itest/native/kill_stubs -Itest/native/stubs -Iinclude -I$FB"
//   SRC="src/CommandHandler.cpp src/softproxy/SafetyCaps.cpp src/WebSocketDeFragger.cpp src/radio/RFTransmitter.cpp"
//   SRC="$SRC src/radio/rmt/Sequence.cpp src/radio/rmt/*Encoder.cpp src/serial/command_handlers/estop.cpp"
//   SRC="$SRC src/serial/command_handlers/rftxpin.cpp src/message_handlers/websocket/gateway/Trigger.cpp"
//   SRC="$SRC src/util/StringUtils.cpp"
//   g++ $FLAGS $INC -Wl,--wrap=malloc test/native/test_kill_path.cpp $SRC -o kill_path_test && ./kill_path_test
//
// Every cap below is a synthetic test value chosen for the arithmetic. None is a body-safety value. The test has no
// model of the receiver or of the switch, the cable or the board. Those are for the attended session.

#include "../../src/EStopManager.cpp"
#include "../../src/serial/command_handlers/safetycaps.cpp"

#include "CommandHandler.h"
#include "message_handlers/impl/WSGateway.h"
#include "radio/RFTransmitter.h"
#include "radio/rmt/Sequence.h"
#include "softproxy/Lockout.h"
#include "softproxy/SafetyCaps.h"
#include "WebSocketDeFragger.h"

#include "serialization/_fbs/GatewayToHubMessage_generated.h"
#include "serialization/_fbs/HubToGatewayMessage_generated.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace GatewaySchema = OpenShock::Serialization::Gateway;
using namespace OpenShock::Softproxy;
using SafetyCaps::SetResult;

// Fake state declared in host_fakes.h.
FakeNvs g_nvs;
std::string g_lastLog;
std::string g_serialOut;
esp_event_base_t OPENSHOCK_EVENTS = "fake";
int g_restarts                    = 0;
int g_panics                      = 0;
namespace OpenShock {
  int64_t g_nowMs = 1'000'000'000;  // Past every cold boot hold of the synthetic caps.
  namespace Serialization::Local {
    std::string g_lastError;
  }
  namespace Serialization::JsonSerial {
    ShockerCommand g_serialCommand;
  }
}  // namespace OpenShock

void _handleEStopEnabledCommand(std::string_view arg, bool isAutomated);
void _handleEStopPinCommand(std::string_view arg, bool isAutomated);
void _handleEStopStateCommand(std::string_view arg, bool isAutomated);
void _handleRfTxPinCommand(std::string_view arg, bool isAutomated);

static std::size_t s_failMallocAtOrAbove = 0;  // 0 never fails.
extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __wrap_malloc(std::size_t size)
{
  if (s_failMallocAtOrAbove != 0 && size >= s_failMallocAtOrAbove) {
    return nullptr;
  }
  return __real_malloc(size);
}

// One thread awake at a time. `resume` hands the token to a task and waits for it back.
namespace {
  struct HostTaskExit { };
  struct HostTask {
    TaskFunction_t fn;
    void* arg;
    std::thread thread;
    bool killed   = false;
    bool finished = false;
  };

  std::mutex s_schedMutex;
  std::condition_variable s_schedCv;
  HostTask* s_awake = nullptr;  // nullptr is the test thread.
  std::vector<HostTask*> s_tasks;
  thread_local HostTask* t_self = nullptr;

  void resume(HostTask* task)
  {
    std::unique_lock<std::mutex> lock(s_schedMutex);
    s_awake = task;
    s_schedCv.notify_all();
    s_schedCv.wait(lock, [] { return s_awake == nullptr; });
  }

  void hostYield()
  {
    HostTask* self = t_self;
    std::unique_lock<std::mutex> lock(s_schedMutex);
    s_awake = nullptr;
    s_schedCv.notify_all();
    s_schedCv.wait(lock, [self] { return s_awake == self; });
    if (self->killed) {
      throw HostTaskExit {};
    }
  }

  TaskHandle_t startTask(TaskFunction_t fn, void* arg)
  {
    auto* task   = new HostTask;
    task->fn     = fn;
    task->arg    = arg;
    task->thread = std::thread([task] {
      t_self = task;
      {
        std::unique_lock<std::mutex> lock(s_schedMutex);
        s_schedCv.wait(lock, [task] { return s_awake == task; });
      }
      try {
        if (!task->killed) {
          task->fn(task->arg);
        }
      }
      catch (const HostTaskExit&) {
      }
      std::unique_lock<std::mutex> lock(s_schedMutex);
      task->finished = true;
      s_awake        = nullptr;
      s_schedCv.notify_all();
    });
    s_tasks.push_back(task);
    return task;
  }

  void reap(HostTask* task)
  {
    task->thread.join();
    s_tasks.erase(std::find(s_tasks.begin(), s_tasks.end(), task));
    delete task;
  }

  bool s_stallChecker = false;  // A stalled checker task gets no time.

  void stepTasks()
  {
    const std::vector<HostTask*> tasks = s_tasks;
    for (HostTask* task : tasks) {
      if (s_stallChecker && task == s_estopTask) {
        continue;
      }
      resume(task);
      if (task->finished) {
        reap(task);
      }
    }
  }

  void advance(int64_t ms)
  {
    for (int64_t elapsed = 0; elapsed < ms; elapsed += 5) {
      OpenShock::g_nowMs += 5;
      stepTasks();
    }
  }

  struct HostQueue {
    std::size_t itemSize;
    std::deque<std::vector<uint8_t>> items;
  };
}  // namespace

BaseType_t TaskUtils::TaskCreateUniversal(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t, TaskHandle_t* out, BaseType_t)
{
  *out = startTask(fn, arg);
  return pdPASS;
}
// The keep-alive task and each `RFTransmitter` start here.
static int s_keepAliveStarts        = 0;
static TaskHandle_t s_keepAliveTask = nullptr;
BaseType_t TaskUtils::TaskCreateExpensive(TaskFunction_t fn, const char* name, uint32_t, void* arg, UBaseType_t, TaskHandle_t* out)
{
  *out = startTask(fn, arg);
  if (std::string_view(name) == "KeepAliveTask") {
    ++s_keepAliveStarts;
    s_keepAliveTask = *out;
  }
  return pdPASS;
}
void vTaskDelay(TickType_t)
{
  if (t_self != nullptr) {
    hostYield();
  } else {
    stepTasks();
  }
}
void vTaskDelete(TaskHandle_t handle)
{
  if (handle == nullptr || handle == t_self) {
    throw HostTaskExit {};
  }
  auto* task   = static_cast<HostTask*>(handle);
  task->killed = true;
  resume(task);
  reap(task);
}
eTaskState eTaskGetState(TaskHandle_t handle)
{
  return std::find(s_tasks.begin(), s_tasks.end(), handle) == s_tasks.end() ? eDeleted : eRunning;
}
// Runs at an edge of an outermost critical section on the thread `s_criticalThread` names, nullptr for the test thread,
// until it returns true. At `Enter` the code it runs takes the lock first. At `Exit` it waited for the lock and runs
// just after the section. Those are the two orders a contended `s_latchMux` allows. Nothing runs inside a section.
enum class CriticalEdge {
  Enter,
  Exit,
};
static std::function<bool(CriticalEdge)> s_onCritical;
static TaskHandle_t s_criticalThread = nullptr;
static int s_criticalDepth           = 0;
static void runCriticalHook(CriticalEdge edge)
{
  if (!s_onCritical || static_cast<TaskHandle_t>(t_self) != s_criticalThread) {
    return;
  }
  auto hook    = std::move(s_onCritical);
  s_onCritical = nullptr;
  if (!hook(edge) && !s_onCritical) {
    s_onCritical = std::move(hook);
  }
}
void hostEnterCritical(portMUX_TYPE*)
{
  if (s_criticalDepth == 0) {
    runCriticalHook(CriticalEdge::Enter);
  }
  ++s_criticalDepth;
}
void hostExitCritical(portMUX_TYPE*)
{
  if (--s_criticalDepth == 0) {
    runCriticalHook(CriticalEdge::Exit);
  }
}
// Runs once when a task finds its queue empty with no wait: for the transmit task, the point after a pop and before
// it reads the E-Stop for that pass.
static std::function<void()> s_onQueueDrained;
QueueHandle_t xQueueCreate(int, std::size_t itemSize)
{
  return new HostQueue {itemSize, {}};
}
void vQueueDelete(QueueHandle_t queue)
{
  delete static_cast<HostQueue*>(queue);
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t)
{
  auto* q        = static_cast<HostQueue*>(queue);
  const auto* in = static_cast<const uint8_t*>(item);
  q->items.emplace_back(in, in + q->itemSize);
  return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* out, TickType_t wait)
{
  auto* q = static_cast<HostQueue*>(queue);
  while (q->items.empty()) {
    if (wait == 0 || t_self == nullptr) {
      if (s_onQueueDrained) {
        std::exchange(s_onQueueDrained, nullptr)();
      }
      return 0;
    }
    hostYield();
  }
  std::memcpy(out, q->items.front().data(), q->itemSize);
  q->items.pop_front();
  return pdTRUE;
}

// GPIO. The E-Stop is a latching mushroom with a normally closed contact to ground (R-SOFTPROXY-ESTOP-NC, SP-174). Up
// closes the contact, which reads 0 and means run. Down opens it, which reads 1 through the pull-up and means stop,
// until it is twisted up. A pin with nothing wired reads 1 through the pull-up, as a cut or unplugged cable does. With
// no pull-up it floats, and the fake reads that as 0, run, the unsafe case.
static std::map<int, int> s_gpioLevel;
static std::vector<int> s_gpioConfigured;
static std::set<int> s_gpioPullUp;
esp_err_t gpio_config(const gpio_config_t* config)
{
  for (int pin = 0; pin < 64; ++pin) {
    if ((config->pin_bit_mask >> pin) & 1) {
      s_gpioConfigured.push_back(pin);
      if (config->pull_up_en == GPIO_PULLUP_ENABLE) {
        s_gpioPullUp.insert(pin);
      } else {
        s_gpioPullUp.erase(pin);
      }
    }
  }
  return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t pin)
{
  s_gpioPullUp.erase(pin);
  return ESP_OK;
}
// Runs on each E-Stop sample until it returns true. The checker task reads the pin after its trigger check and before
// its state update, so this is where a `Trigger` from another core can land inside a pass.
static std::function<bool()> s_onEStopSample;
int gpio_get_level(gpio_num_t pin)
{
  if (s_onEStopSample && s_onEStopSample()) {
    s_onEStopSample = nullptr;
  }
  auto it = s_gpioLevel.find(pin);
  if (it != s_gpioLevel.end()) {
    return it->second;
  }
  return s_gpioPullUp.count(pin) != 0 ? 1 : 0;
}

// RMT. Each write is one frame on the air, and it costs the transmit task one tick.
struct rmt_obj_t { };
struct AirFrame {
  std::vector<uint32_t> words;
  int64_t atMs;
};
static std::vector<AirFrame> s_air;
static rmt_obj_t s_rmt;
rmt_obj_t* rmtInit(int, int, int)
{
  return &s_rmt;
}
float rmtSetTick(rmt_obj_t*, float tick)
{
  return tick;
}
bool rmtDeinit(rmt_obj_t*)
{
  return true;
}
bool rmtWriteBlocking(rmt_obj_t*, const rmt_data_t* data, std::size_t size)
{
  AirFrame frame {{}, OpenShock::g_nowMs};
  for (std::size_t i = 0; i < size; ++i) {
    frame.words.push_back(data[i].val);
  }
  s_air.push_back(std::move(frame));
  vTaskDelay(pdMS_TO_TICKS(5));
  return true;
}

// Events run at once on the posting thread.
static void (*s_estopHandler)(void*, esp_event_base_t, int32_t, void*) = nullptr;
static std::vector<EStopState> s_published;
// A post can fail, as a full event queue would after its wait. Each post records the wait it asked for.
static esp_err_t s_eventPostResult = ESP_OK;
static std::vector<TickType_t> s_eventPostWaits;
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, void (*handler)(void*, esp_event_base_t, int32_t, void*), void*)
{
  s_estopHandler = handler;
  return ESP_OK;
}
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void* data, std::size_t, TickType_t wait)
{
  s_eventPostWaits.push_back(wait);
  if (s_eventPostResult != ESP_OK) {
    return s_eventPostResult;
  }
  s_published.push_back(*static_cast<const EStopState*>(data));
  if (s_estopHandler != nullptr) {
    s_estopHandler(nullptr, base, id, const_cast<void*>(data));
  }
  return ESP_OK;
}

// `Config` store.
static bool s_cfgEnabled   = false;
static gpio_num_t s_cfgPin = GPIO_NUM_NC;
static gpio_num_t s_cfgTx  = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
static bool s_cfgKeepAlive = false;

// `GetRFConfigTxPin` fails, as a store that cannot be read would.
static bool s_cfgTxReadFails = false;
bool Config::GetRFConfig(Config::RFConfig& out)
{
  out = {.txPin = s_cfgTx, .keepAliveEnabled = s_cfgKeepAlive};
  return true;
}
bool Config::GetRFConfigKeepAliveEnabled(bool& out)
{
  out = s_cfgKeepAlive;
  return true;
}
bool Config::GetRFConfigTxPin(gpio_num_t& out)
{
  if (s_cfgTxReadFails) {
    return false;
  }
  out = s_cfgTx;
  return true;
}
bool Config::SetRFConfigTxPin(gpio_num_t pin)
{
  s_cfgTx = pin;
  return true;
}
bool Config::SetRFConfigKeepAliveEnabled(bool)
{
  return true;
}
bool Config::GetEStop(Config::EStopConfig& out)
{
  out = {.enabled = s_cfgEnabled, .gpioPin = s_cfgPin};
  return true;
}
bool Config::GetEStopEnabled(bool& out)
{
  out = s_cfgEnabled;
  return true;
}
bool Config::SetEStopEnabled(bool enabled)
{
  s_cfgEnabled = enabled;
  return true;
}
bool Config::GetEStopGpioPin(gpio_num_t& out)
{
  out = s_cfgPin;
  return true;
}
bool Config::SetEStopGpioPin(gpio_num_t pin)
{
  s_cfgPin = pin;
  return true;
}
static int s_portalCalls = 0;
void CaptivePortal::SetAlwaysEnabled(bool)
{
  ++s_portalCalls;
}

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      ++s_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

static const gpio_num_t kButton = static_cast<gpio_num_t>(13);
static const int kRunLevel       = 0;
static const int kStopLevel      = 1;
static const auto kModel        = ShockerModelType::CaiXianlin;

// Synthetic caps. `high` is above `low` in every field.
static ChannelCaps lowCaps()
{
  ChannelCaps c {};
  c.maxIntensityPct   = 10;
  c.maxPulseMs        = 100;
  c.maxPulsesPerStage = 1;
  c.minGapSec         = 2;
  c.maxPerHour        = 10;
  c.maxPerDay         = 20;
  return c;
}
static ChannelCaps highCaps()
{
  ChannelCaps c       = lowCaps();
  c.maxIntensityPct   = 20;
  c.maxPulseMs        = 2000;
  c.maxPulsesPerStage = 2;
  c.maxPerHour        = 20;
  c.maxPerDay         = 40;
  return c;
}

// Everything a boot wipes: this file's RAM state. `s_retainedLatch` stays unless `loseRetained` fills it with noise.
static void bootEStopManager(esp_reset_reason_t reason, bool loseRetained)
{
  if (s_estopTask != nullptr) {
    vTaskDelete(s_estopTask);
    s_estopTask = nullptr;
  }
  s_estopPin            = GPIO_NUM_NC;
  s_lastPublishedState  = EStopState::Idle;
  s_latch               = EStopLatchSource::ColdBoot;
  s_estopActivatedAt    = 0;
  s_lastSampleMs        = 0;
  s_sampledSinceStart   = false;
  s_lastLoggedState     = EStopState::Idle;
  s_postFailureLogged   = false;
  s_eventPostResult     = ESP_OK;
  s_externallyTriggered = false;
  s_gatewayStops        = 0;
  s_estopInitialized    = false;
  g_resetReason         = reason;

  s_failedStatePosts      = 0;
  s_postFailureLoggedAtMs = 0;
  s_maxSampleIntervalMs   = 0;
  if (loseRetained) {
    std::memset(static_cast<void*>(&s_retainedLatch), 0xA5, sizeof(s_retainedLatch));
  }
  CHECK(EStopManager::Init());
}

// A software restart, or a power cycle that loses RTC memory.
static void restartEStopManager(bool coldBoot)
{
  bootEStopManager(coldBoot ? ESP_RST_POWERON : ESP_RST_SW, coldBoot);
}

// A clean hub: cold boot, the button wired with the mushroom up, caps cleared. It starts engaged with source cold-boot
// (SP-158). Every other pin has nothing wired, so it reads open.
static void freshHub(bool enabled = false, gpio_num_t pin = GPIO_NUM_NC)
{
  s_gpioLevel.clear();
  s_gpioLevel[kButton] = kRunLevel;
  s_gpioConfigured.clear();
  s_stallChecker = false;
  s_cfgEnabled   = enabled;
  s_cfgPin       = pin;
  s_cfgTx        = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
  s_cfgKeepAlive = false;
  (void)SafetyCaps::Clear();
  s_cfgTxReadFails = false;
  s_onEStopSample  = nullptr;
  s_onCritical     = nullptr;
  s_criticalThread = nullptr;
  restartEStopManager(true);
  CHECK(CommandHandler::Init());
  // A stored E-Stop pin on the transmitter is a scenario of its own, and the rule refuses this call.
  if (pin != s_cfgTx) {
    CHECK(CommandHandler::SetRfTxPin(s_cfgTx) == SetGPIOResultCode::Success);
  }
  advance(50);
  s_air.clear();
  s_published.clear();
  g_serialOut.clear();
}

// The mushroom stays where it is put. Down opens the contact, and up closes it.
static void pushDown(gpio_num_t pin = kButton)
{
  s_gpioLevel[pin] = kStopLevel;
}
static void twistUp(gpio_num_t pin = kButton)
{
  s_gpioLevel[pin] = kRunLevel;
}
// The cable is cut or unplugged: nothing drives the pin, and the pull-up reads it open.
static void cutCable(gpio_num_t pin = kButton)
{
  s_gpioLevel.erase(pin);
}

// The only way that releases the E-Stop (SP-174): the mushroom measured down for a full 65 millisecond window, then
// twisted up and left up for the 5 second clear time. The clear starts once a full window reads low, so the last wait
// is 65 milliseconds plus the clear time plus margin. Pushing it down first changes nothing on an engaged hub.
static void twistToClear(gpio_num_t pin = kButton)
{
  pushDown(pin);
  advance(100);
  twistUp(pin);
  advance(5200);
}

// An armed hub with the button wired and released: the cold boot latch cleared by hand, as SP-158 requires.
static void armedHub()
{
  freshHub(true, kButton);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  twistToClear();
  CHECK(EStopManager::IsArmed());
  CHECK(!EStopManager::IsEStopped());
  s_published.clear();
}

// A released hub with no checker task, reached by a warm restart after the stored E-Stop went away.
static void releasedHubWithNoTask()
{
  armedHub();
  s_cfgEnabled = false;
  s_cfgPin     = GPIO_NUM_NC;
  restartEStopManager(false);
  CHECK(s_estopTask == nullptr);
  CHECK(!EStopManager::IsEStopped());
  s_published.clear();
}

// A real press: one high sample engages, and the mushroom stays down past the 65 millisecond window that confirms it.
// The task must have read a full window of the mushroom up, the contact closed, first. The mushroom latches, so it stays down after this.
static void engageByButton(gpio_num_t pin = kButton)
{
  pushDown(pin);
  advance(20);
  CHECK(EStopManager::IsEStopped());
  advance(80);
}

static void sendTrigger(GatewaySchema::TriggerType type)
{
  flatbuffers::FlatBufferBuilder builder;
  auto trigger = GatewaySchema::CreateTrigger(builder, type);
  auto root    = GatewaySchema::CreateGatewayToHubMessage(builder, GatewaySchema::GatewayToHubMessagePayload::Trigger, trigger.Union());
  builder.Finish(root);
  OpenShock::MessageHandlers::Server::_Private::HandleTrigger(flatbuffers::GetRoot<GatewaySchema::GatewayToHubMessage>(builder.GetBufferPointer()));
}

static bool lastRefusalIs(Verdict verdict)
{
  return SafetyCaps::GetRefusals().lastCode == static_cast<uint8_t>(verdict);
}

// An unreadable store counts as set, so the E-Stop settings stay locked. It must run before anything loads the caps,
// because a load that succeeded is never repeated in a process.
static void unreadableCapsLockTheEStop()
{
  s_cfgEnabled = true;
  s_cfgPin     = kButton;
  restartEStopManager(true);
  advance(50);
  CHECK(EStopManager::IsArmed());

  // A record exists and cannot be read. With no record at all NVS reports not found, which is a clean state.
  g_nvs.present   = true;
  g_nvs.readError = ESP_FAIL;
  CHECK(!SafetyCaps::Init());
  CHECK(SafetyCaps::AnySet());
  CHECK(!EStopManager::SetEStopEnabled(false));
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(EStopManager::IsArmed());
  g_nvs.readError = ESP_OK;
  g_nvs.present   = false;
  CHECK(SafetyCaps::Init());
  CHECK(!SafetyCaps::AnySet());
}

// SP-113 rule 1 at boot. It must run before `freshHub`, because `CommandHandler::Init` runs once in a process.
static void bootWithTxOnEStopLeavesRadioOff()
{
  s_cfgEnabled = true;
  s_cfgPin     = kButton;
  s_cfgTx      = kButton;
  restartEStopManager(true);
  CHECK(!EStopManager::IsArmed());
  CHECK(CommandHandler::Init());
  CHECK(!CommandHandler::Ok());
  CHECK(s_estopHandler != nullptr);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));

  // Serial `rftxpin` is the repair, and it still refuses the E-Stop pin.
  s_cfgTx = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
  CHECK(CommandHandler::SetRfTxPin(kButton) == SetGPIOResultCode::InvalidPin);
  CHECK(!CommandHandler::Ok());
  CHECK(CommandHandler::SetRfTxPin(s_cfgTx) == SetGPIOResultCode::Success);
  CHECK(CommandHandler::Ok());
}

// SP-113 and finding 4: with no live E-Stop, every shock and vibrate command refuses at any cap, and the refusal shows.
static void noEStopRefuses()
{
  freshHub();
  CHECK(!EStopManager::IsArmed());

  // Caps can only be there from before the E-Stop went away, so plant them in the store, as `Config::Init` defaults do
  // when a config load fails and the separate caps namespace survives.
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Shock, lowCaps()) == SetResult::Stored);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  s_cfgEnabled = false;
  s_cfgPin     = GPIO_NUM_NC;
  restartEStopManager(false);
  CHECK(!EStopManager::IsArmed());
  CHECK(SafetyCaps::AnySet());

  const uint32_t before = SafetyCaps::GetRefusals().count;
  for (auto type : {ShockerCommandType::Shock, ShockerCommandType::Vibrate}) {
    CHECK(!CommandHandler::HandleCommand(kModel, 7, type, 1, 50));
    CHECK(lastRefusalIs(Verdict::EStopNotArmed));
  }
  CHECK(SafetyCaps::GetRefusals().count == before + 2);
  CHECK(g_lastLog.find("verdict=estop-not-armed") != std::string::npos);
  advance(20);
  CHECK(s_air.empty());

  // A stop still reaches the radio with no E-Stop, and it carries no energy (encoders in test_command_paths.cpp).
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 0));
  advance(20);
  CHECK(!s_air.empty());
  (void)SafetyCaps::Clear();
  s_air.clear();
}

static void armedHubAdmitsWithinCaps()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopEngaged));
  CHECK(g_lastLog.find("verdict=estop-engaged") != std::string::npos);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 0));
  CHECK(g_lastLog.find("channel=other") != std::string::npos);
  advance(20);
  CHECK(s_air.empty());

  twistToClear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  advance(20);
  CHECK(!s_air.empty());
}

// Finding: the gateway trigger was a no-op with no checker task, and the flag fired later.
static void gatewayTriggerLatchesWithNoTask()
{
  releasedHubWithNoTask();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  CHECK(s_published.size() == 1 && s_published.back() == EStopState::Active);
  CHECK(!s_externallyTriggered);

  // No button can release it, so it holds through a warm restart. A power cycle swaps it for the cold boot latch.
  restartEStopManager(false);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  restartEStopManager(true);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
}

static void gatewayTriggerLatchesWithTask()
{
  armedHub();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  CHECK(EStopManager::IsEStopped());
  advance(10);
  CHECK(s_published.size() == 1 && s_published.back() == EStopState::Active);

  // The mushroom was up when the stop came, so leaving it up clears nothing. It has to go down and up again.
  advance(6000);
  CHECK(EStopManager::IsEStopped());
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::Idle);
}

static void twistToClearNeedsTheFullTime()
{
  armedHub();

  // A latched mushroom held down for minutes never clears. Down is stopped, read as a level, always (SP-174).
  pushDown();
  advance(3 * 60 * 1000);
  CHECK(EStopManager::IsEStopped());
  CHECK(s_published.size() == 1 && s_published.back() == EStopState::Active);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));

  // Twisted up, it clears after a full low window and the clear time, and not before.
  twistUp();
  advance(65 + 4900);
  CHECK(EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::ActiveClearing);
  advance(200);
  CHECK(!EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::Idle);

  // A high level engages at once, with no rearm window after the clear.
  pushDown();
  advance(5);
  CHECK(EStopManager::IsEStopped());
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// A push during the clear stops it: the hub stays engaged, the source does not change, and no command passes. Only a
// new twist up clears. During the clear the latch still refuses a rise, whatever set it.
static void aPushDuringTheClearStopsIt()
{
  armedHub();
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistUp();
  advance(2000);
  CHECK(s_published.back() == EStopState::ActiveClearing);
  pushDown();
  for (int i = 0; i < 1000; ++i) {
    advance(5);
    CHECK(EStopManager::IsEStopped());
  }
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(s_published.back() == EStopState::Active);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));
  twistUp();
  advance(5200);
  CHECK(!EStopManager::IsEStopped());

  // On the cold boot latch, a clear under way neither confirms a press nor allows a rise.
  freshHub(true, kButton);
  pushDown();
  advance(100);
  twistUp();
  advance(1000);
  CHECK(s_published.back() == EStopState::ActiveClearing);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  pushDown();
  advance(6000);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(!SafetyCaps::AnySet());
}

// Findings: a restart, panic or watchdog cleared an engaged E-Stop with no hold.
static void latchSurvivesWarmRestart()
{
  armedHub();
  engageByButton();
  restartEStopManager(false);
  CHECK(EStopManager::IsEStopped());
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(s_published.back() == EStopState::Active);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));

  // A second restart still comes back engaged, since the first sealed it again.
  restartEStopManager(false);
  CHECK(EStopManager::IsEStopped());

  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  restartEStopManager(false);
  CHECK(!EStopManager::IsEStopped());
}

// SP-158: a cold boot refuses every command, Stop included, until the clear. Nothing else releases it, so a power cycle
// with nobody at the hub leaves every command refused until the operator clears it by hand. A mushroom already up at
// the boot proves no twist, so it has to go down and up again (SP-174).
static void coldBootStartsEngaged()
{
  // No pin stored: nothing can clear it, and no command reaches the radio.
  freshHub();
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  for (auto type : {ShockerCommandType::Shock, ShockerCommandType::Vibrate, ShockerCommandType::Stop, ShockerCommandType::Sound, ShockerCommandType::Light}) {
    CHECK(!CommandHandler::HandleCommand(kModel, 7, type, 1, 50));
    CHECK(lastRefusalIs(Verdict::EStopEngaged));
  }
  advance(6000);
  CHECK(EStopManager::IsEStopped());
  CHECK(s_air.empty());

  // A hub in service, caps stored and pin wired, loses power with the mushroom up. Time alone releases nothing.
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  advance(3000);
  restartEStopManager(true);
  s_air.clear();
  advance(10'000);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopEngaged));
  CHECK(s_air.empty());

  // The caps keep the settings locked through the power cycle. The clear is what brings commands back.
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopEnabled(false));
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  advance(20);
  CHECK(!s_air.empty());
  (void)SafetyCaps::Clear();
  s_air.clear();
}

// A fresh hub starts on the cold boot latch, so the first `estop pin` has to work. No setting may release it.
static void coldBootLatchLeavesSettingsOpen()
{
  freshHub();
  g_serialOut.clear();
  _handleEStopPinCommand("13"sv, false);
  _handleEStopEnabledCommand("true"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  advance(50);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);

  // Move the pin, turn the task off and on, restart. Each one keeps the latch and its source.
  g_serialOut.clear();
  _handleEStopPinCommand("14"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  advance(50);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  g_serialOut.clear();
  _handleEStopEnabledCommand("false"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  _handleEStopEnabledCommand("true"sv, false);
  restartEStopManager(false);
  advance(50);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);

  // A twist on the old pin does nothing. The clear runs on the pin the checker reads.
  CHECK(EStopManager::GetRunningPin() == static_cast<gpio_num_t>(14));
  twistToClear(kButton);
  CHECK(EStopManager::IsEStopped());
}

// Review finding 1: a gateway Trigger on an armed hub must not stand in for the button, because an armed pin with
// nothing wired never reads a press.
static void armedTriggerDoesNotAllowARise()
{
  armedHub();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());

  // Restarting does not launder it into a press, and neither does a power cycle.
  restartEStopManager(false);
  advance(20);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  restartEStopManager(true);
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());

  // Pushing the mushroom down during a held latch is no engagement, whatever set the latch.
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  twistToClear();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);

  // Only a clear and then a press from released counts.
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  (void)SafetyCaps::Clear();
}

// The first source holds. A trigger on a pressed hub changes nothing, and one that lost the race against a clear
// engages as the gateway, not as a press.
static void latchKeepsItsFirstSource()
{
  armedHub();
  engageByButton();
  EStopManager::Trigger();
  advance(20);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();

  s_externallyTriggered = true;
  advance(10);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
}

// Review finding: no mutant covered the guard in `estopmanager_confirmpress`. A gateway stop that lands in a released
// checker's pass, after its trigger check, latches as the gateway. The same pass then reads the mushroom down from Idle
// and starts to confirm a press. The full window of high samples that follows must leave the gateway latch in place.
static void gatewayLatchThenPressStaysGateway()
{
  armedHub();
  pushDown();
  s_onEStopSample = [] {
    EStopManager::Trigger();
    return true;
  };
  advance(5);
  CHECK(!s_onEStopSample);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  advance(200);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  // With no stop in the pass, the same press reads as a button.
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// Review mutant X10: the pass that takes a gateway stop leaves the press it found in progress confirming. The press
// engaged first, so the latch keeps its source, and the full window of high samples that follows confirms it as a
// button. The stop is taken, so the clear after it releases.
static void pressThenGatewayStopStillConfirms()
{
  armedHub();
  pushDown();
  advance(5);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(5);
  CHECK(!s_externallyTriggered);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  advance(100);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  (void)SafetyCaps::Clear();
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// Review finding: a gateway stop that landed in the checker's last clear pass, after its trigger check and before the
// release, found the latch still engaged and engaged nothing. The release then cleared the latch, and `IsEStopped` read
// false until the next pass took the stop. The release now refuses a stop the task has not taken, so the hub stays
// engaged on every pass, the source stays the press, and the next clear needs the mushroom down and up again.
static void gatewayStopRacingTheReleaseHolds()
{
  armedHub();
  engageByButton();
  twistUp();
  uint32_t clearPasses = 0;
  bool fired           = false;
  s_onEStopSample      = [&clearPasses, &fired] {
    if (s_published.empty() || s_published.back() != EStopState::ActiveClearing || ++clearPasses < k_estopClearSamples) {
      return false;
    }
    EStopManager::Trigger();
    fired = true;
    return true;
  };
  for (int pass = 0; pass < 1200; ++pass) {
    advance(5);
    CHECK(EStopManager::IsEStopped());
  }
  CHECK(fired);
  s_onEStopSample = nullptr;
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(std::find(s_published.begin(), s_published.end(), EStopState::Idle) == s_published.end());
  CHECK(s_published.back() == EStopState::Active);

  // The mushroom stayed up through the stop, so time alone clears nothing.
  advance(6000);
  CHECK(EStopManager::IsEStopped());
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::Idle);
}

// An engaged hub with the mushroom twisted up, stepped until the checker's next pass is the last clear pass, the one
// that calls `estopmanager_release`. It counts passes as gatewayStopRacingTheReleaseHolds does, and leaves the count
// running in `clearPasses`.
static void stepToTheLastClearPass(uint32_t& clearPasses)
{
  armedHub();
  engageByButton();
  twistUp();
  clearPasses     = 0;
  s_onEStopSample = [&clearPasses] {
    if (!s_published.empty() && s_published.back() == EStopState::ActiveClearing) {
      ++clearPasses;
    }
    return false;
  };
  for (int pass = 0; pass < 1200 && clearPasses < k_estopClearSamples - 1; ++pass) {
    advance(5);
  }
  CHECK(clearPasses == k_estopClearSamples - 1);
  CHECK(EStopManager::IsEStopped());
}

// Review survivors X1, X8, X2 and X6. A gateway stop contends with the checker task for `s_latchMux`, and the critical
// section hook runs it at an edge of a latch section, in each order the lock allows. The hub must read engaged after
// every pass, and a clear after the stop must still release.
static void gatewayStopContendingForTheLatchHolds()
{
  uint32_t clearPasses = 0;

  // X1: the release pass runs between `Trigger`'s first latch section and the rest of `Trigger`. The count comes first,
  // so the release refuses.
  stepToTheLastClearPass(clearPasses);
  s_criticalThread = nullptr;
  s_onCritical     = [](CriticalEdge edge) {
    if (edge != CriticalEdge::Exit) {
      return false;
    }
    OpenShock::g_nowMs += 5;
    resume(static_cast<HostTask*>(s_estopTask.load()));
    return true;
  };
  EStopManager::Trigger();
  CHECK(!s_onCritical);
  CHECK(clearPasses == k_estopClearSamples);
  CHECK(EStopManager::IsEStopped());
  CHECK(std::find(s_published.begin(), s_published.end(), EStopState::Idle) == s_published.end());
  s_onEStopSample = nullptr;
  advance(5);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  // X8: a `Trigger` lands at each edge of the checker's first latch section in the last clear pass. Before it, the
  // release sees the count moved and refuses. After it, the release has written the latch, so `Trigger` engages it again.
  for (CriticalEdge at : {CriticalEdge::Enter, CriticalEdge::Exit}) {
    stepToTheLastClearPass(clearPasses);
    s_criticalThread = s_estopTask;
    s_onCritical     = [at](CriticalEdge edge) {
      if (edge != at) {
        return false;
      }
      EStopManager::Trigger();
      return true;
    };
    advance(5);
    CHECK(!s_onCritical);
    CHECK(clearPasses == k_estopClearSamples);
    CHECK(EStopManager::IsEStopped());
    CHECK(EStopManager::GetLatchSource() == (at == CriticalEdge::Enter ? EStopLatchSource::Button : EStopLatchSource::Gateway));
    s_onEStopSample = nullptr;
    advance(5);
    CHECK(EStopManager::IsEStopped());
    twistToClear();
    CHECK(!EStopManager::IsEStopped());
  }

  // X2: a second `Trigger` lands just after the checker reads the count for the first. The flag is already clear, so the
  // next pass takes the second stop, and a clear after it releases.
  armedHub();
  engageByButton();
  EStopManager::Trigger();
  s_criticalThread = s_estopTask;
  s_onCritical     = [](CriticalEdge edge) {
    if (edge != CriticalEdge::Exit) {
      return false;
    }
    EStopManager::Trigger();
    return true;
  };
  advance(5);
  CHECK(!s_onCritical);
  advance(5);
  CHECK(EStopManager::IsEStopped());
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  // X6: a new checker task takes the stops counted before it started, so its first clear releases.
  armedHub();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Gateway);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(estopmgr_setestopenabled(false));
  CHECK(estopmgr_setestopenabled(true));
  advance(100);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// A gateway stop reaches a clear in progress, so the hub does not release after it. The mushroom measured down before the
// stop then counts for nothing, and the clear needs it down and up again. The other triggers stay ignored while engaged.
static void emergencyStopInterruptsAClear()
{
  armedHub();
  engageByButton();

  // During the clear.
  twistUp();
  advance(1000);
  CHECK(s_published.back() == EStopState::ActiveClearing);
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(5200);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  advance(6000);
  CHECK(EStopManager::IsEStopped());

  // The same holds on the cold boot latch.
  freshHub(true, kButton);
  pushDown();
  advance(100);
  twistUp();
  advance(1000);
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(5200);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);

  const int portalCalls = s_portalCalls;
  sendTrigger(GatewaySchema::TriggerType::CaptivePortalDisable);
  CHECK(s_portalCalls == portalCalls);

  // A new push and twist still clears.
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// The source survives a warm restart, so a restart neither grants nor loses the rise. The settings stay locked through
// it, because a retained `Button` latch on a pin moved after the restart would allow a rise on an unproved pin.
// `factoryreset` is the exception: see buttonLatchWithADeadCheckerAllowsNoRise.
static void sourceSurvivesWarmRestart()
{
  armedHub();
  engageByButton();
  restartEStopManager(false);
  advance(20);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopEnabled(false));
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  (void)SafetyCaps::Clear();
  twistToClear();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopEnabled(false));
}

// Finding: the seal alone decided a warm restart, so a power loss that kept RTC memory came back released, as a brownout,
// a short supply dropout or an EN reset can. A valid record counts only after an allow-listed reset.
static void onlyAnAllowedResetKeepsTheLatch()
{
  armedHub();
  for (esp_reset_reason_t reason : {ESP_RST_POWERON, ESP_RST_BROWNOUT, ESP_RST_EXT, ESP_RST_UNKNOWN, ESP_RST_DEEPSLEEP, ESP_RST_SDIO}) {
    // A sealed `Released`.
    CHECK(!EStopManager::IsEStopped());
    bootEStopManager(reason, false);
    advance(20);
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
    CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));
    CHECK(lastRefusalIs(Verdict::EStopEngaged));
    twistToClear();

    // A sealed `Button`, which must not carry its rise across a power loss.
    engageByButton();
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
    bootEStopManager(reason, false);
    advance(20);
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
    CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
    twistToClear();
  }

  for (esp_reset_reason_t reason : {ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT}) {
    CHECK(!EStopManager::IsEStopped());
    bootEStopManager(reason, false);
    advance(100);
    CHECK(!EStopManager::IsEStopped());
    engageByButton();
    bootEStopManager(reason, false);
    advance(20);
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
    twistToClear();
  }
}

// SP-172: a panic or watchdog reset keeps the latch as it was, engaged or released, with its source. So a crash loop
// never forces a stop: a released hub that restarts over and over comes back released each time.
static void crashLoopNeverForcesAStop()
{
  armedHub();
  for (int i = 0; i < 20; ++i) {
    bootEStopManager(i % 2 == 0 ? ESP_RST_PANIC : ESP_RST_TASK_WDT, false);
    advance(50);
    CHECK(!EStopManager::IsEStopped());
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Released);
  }
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));

  // An engaged hub stays engaged with its source through the same loop, and the mushroom still clears it. The press
  // comes after the restarted task has measured a full run window, so it counts as a button.
  advance(100);
  engageByButton();
  for (int i = 0; i < 20; ++i) {
    bootEStopManager(i % 2 == 0 ? ESP_RST_INT_WDT : ESP_RST_WDT, false);
    advance(50);
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  }
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// The armed half of the rise rule through the real `Set`: a button latch with no live checker behind it allows no rise.
static void buttonLatchWithADeadCheckerAllowsNoRise()
{
  // The checker stalls.
  armedHub();
  engageByButton();
  s_stallChecker = true;
  advance(k_estopSampleStaleTime + 50);
  CHECK(!EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  g_serialOut.clear();
  _handleSafetyCapsVibrateCommand("10 100 1 2 10 20"sv, false);
  CHECK(g_serialOut.find("Refused (raising a cap needs the E-Stop armed and engaged") != std::string::npos);
  CHECK(!SafetyCaps::AnySet());
  s_stallChecker = false;
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  (void)SafetyCaps::Clear();

  // `factoryreset` writes the default config with no lock check, and a retained latch survives it. The hub comes back
  // with a button latch and no task, so it fails closed: no rise, the settings stay locked, and a power cycle ends it.
  s_cfgEnabled = false;
  s_cfgPin     = GPIO_NUM_NC;
  restartEStopManager(false);
  advance(20);
  CHECK(s_estopTask == nullptr);
  CHECK(!EStopManager::IsArmed());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());
  CHECK(!EStopManager::SetEStopPin(kButton));
  restartEStopManager(true);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(EStopManager::SetEStopPin(kButton));
}

// One high sample engages, which is the safe side, but it can be noise on the line. It proves no press (SP-156).
static void oneHighSampleProvesNoPress()
{
  armedHub();
  pushDown();
  advance(5);
  twistUp();
  advance(200);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopEnabled(false));
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));

  // The mushroom was never measured down, so leaving it up clears nothing.
  advance(6000);
  CHECK(EStopManager::IsEStopped());

  // A real press on the same task after the glitch is a press during a held latch, so it restates nothing.
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);

  // A warm restart keeps it, and a press during the held latch restates nothing.
  restartEStopManager(false);
  advance(20);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  // A real press reads as a button at the 13th high sample, and a press that bounces first does too.
  pushDown();
  advance(60);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  advance(10);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();
  pushDown();
  advance(5);
  twistUp();
  advance(5);
  pushDown();
  advance(100);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();
}

// Only a stable low level for the clear time releases. A noisy line on a latched mushroom must not read as a twist up.
static void pulseTrainClearsNothing()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);

  // A low sample in every window on a mushroom held down, for longer than the clear time. The caps are set, so a
  // release here would admit the next command with nobody at the hub.
  for (int i = 0; i < 100; ++i) {
    twistUp();
    advance(5);
    pushDown();
    advance(55);
  }
  CHECK(EStopManager::IsEStopped());

  // Mostly low, and still never a full window.
  for (int i = 0; i < 100; ++i) {
    twistUp();
    advance(55);
    pushDown();
    advance(5);
  }
  CHECK(EStopManager::IsEStopped());

  // Long low stretches cut by one high sample before each would end the clear.
  for (int i = 0; i < 10; ++i) {
    twistUp();
    advance(4000);
    pushDown();
    advance(5);
  }
  twistUp();
  advance(400);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopEngaged));
  (void)SafetyCaps::Clear();
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  // A high sample in every window on a released hub engages, and it never reads as a button.
  for (int i = 0; i < 100; ++i) {
    pushDown();
    advance(5);
    twistUp();
    advance(55);
  }
  advance(400);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
}

// Contact bounce on the push still counts once, as a button. A bounce during the twist up restarts the clear, which is
// timed from the first full low window after the last high sample.
static void bounceCountsAsOnePress()
{
  const auto bounce = [] {
    for (int i = 0; i < 6; ++i) {
      s_gpioLevel[kButton] = i % 2 == 0 ? kRunLevel : kStopLevel;
      advance(5);
    }
  };

  armedHub();
  bounce();
  pushDown();
  advance(70);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  CHECK(s_published.size() == 1 && s_published.back() == EStopState::Active);

  // A bounce late in the twist costs the time already up.
  twistUp();
  advance(3000);
  pushDown();
  advance(5);
  twistUp();
  advance(2200);
  CHECK(EStopManager::IsEStopped());
  advance(3000);
  CHECK(!EStopManager::IsEStopped());

  // Bounce at both ends: the push engages once, and the twist clears once, timed from after the bounce.
  bounce();
  pushDown();
  advance(100);
  CHECK(EStopManager::IsEStopped());
  bounce();
  twistUp();
  advance(4900);
  CHECK(EStopManager::IsEStopped());
  advance(300);
  CHECK(!EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::Idle);
}

// Random samples at each density, with a fixed seed. None holds a low level for the clear time, so a latch never
// releases, and a released hub engages but never reads a press. A burst of 13 high samples would read as a press, which
// is why the released part stops at 20 percent.
static void noiseProvesNothing()
{
  std::mt19937 rng(7545);
  const auto noise = [&](uint32_t stopPercent) {
    for (int i = 0; i < 1200; ++i) {
      s_gpioLevel[kButton] = rng() % 100 < stopPercent ? kStopLevel : kRunLevel;
      advance(5);
    }
    twistUp();
    advance(400);
  };

  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  for (uint32_t stopPercent : {1u, 5u, 20u, 50u, 80u, 95u}) {
    noise(stopPercent);
    CHECK(EStopManager::IsEStopped());
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
    CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  }
  CHECK(SafetyCaps::Clear());

  twistToClear();
  for (uint32_t stopPercent : {5u, 20u}) {
    CHECK(!EStopManager::IsEStopped());
    noise(stopPercent);
    CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
    CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
    twistToClear();
  }
}

// SP-156 with the E-Stop settings open, at released with every cap 0. A pin already open when the task starts, as an
// unwired pin is, shows no edge, however long it stays open, so it engages and proves nothing.
static void anOpenPinProvesNoPress()
{
  // `estop pin` onto a pin with nothing wired.
  armedHub();
  g_serialOut.clear();
  _handleEStopPinCommand("14"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  advance(200);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  advance(6000);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);

  // The latch locks the settings. A pin with nothing wired never gives a clear, so a power cycle is the way out: the
  // cold boot latch opens them again.
  CHECK(!EStopManager::SetEStopPin(kButton));
  restartEStopManager(true);
  advance(200);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(EStopManager::SetEStopPin(kButton));

  // `estop enabled true` onto a pushed mushroom. Twisting up and pushing down again restates nothing, and a rise then needs a
  // clear and a new press.
  armedHub();
  CHECK(EStopManager::SetEStopEnabled(false));
  pushDown();
  CHECK(EStopManager::SetEStopEnabled(true));
  advance(200);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  twistUp();
  advance(100);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();

  // A warm restart of a released hub onto a pushed mushroom.
  pushDown();
  restartEStopManager(false);
  advance(200);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  twistUp();
  advance(100);
  twistToClear();

  // A press before the task has read a full window with the contact closed proves nothing. After one it does.
  CHECK(EStopManager::SetEStopEnabled(false));
  CHECK(EStopManager::SetEStopEnabled(true));
  advance(30);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  twistToClear();
  CHECK(EStopManager::SetEStopEnabled(false));
  CHECK(EStopManager::SetEStopEnabled(true));
  advance(100);
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistToClear();

  // The clear needs no measured run before the push: a hub that boots with the mushroom down clears by the twist. A
  // rise still needs a press from a measured run after that.
  freshHub();
  pushDown();
  _handleEStopPinCommand("13"sv, false);
  _handleEStopEnabledCommand("true"sv, false);
  advance(200);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  twistUp();
  advance(5200);
  CHECK(!EStopManager::IsEStopped());
  engageByButton();
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
}

// R-SOFTPROXY-ESTOP-NC: an open circuit means stop. A cable cut or unplugged on a running hub engages at once and holds
// for as long as it stays cut. The firmware cannot tell a cut from a press, or a reconnected cable from a twist up.
static void cableCutStops()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(100);

  s_air.clear();
  const int64_t cutAt = g_nowMs;
  cutCable();
  advance(5);
  CHECK(EStopManager::IsEStopped());
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopEngaged));
  advance(3 * 60 * 1000);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::IsArmed());
  CHECK(!s_air.empty());
  CHECK(s_air.back().atMs <= cutAt + 5 + 300 + 15);

  // The cut follows a measured run, so it reads as a press. A reconnected cable held closed for the clear time clears,
  // as a twist up would, and a shorter one does not.
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Button);
  twistUp();
  advance(2000);
  cutCable();
  advance(5);
  twistUp();
  advance(65 + 4900);
  CHECK(EStopManager::IsEStopped());
  advance(200);
  CHECK(!EStopManager::IsEStopped());

  // A cut on a released hub engages too. One shorter than a window is no measured down, so reconnecting clears nothing.
  CHECK(SafetyCaps::Clear());
  cutCable();
  advance(5);
  CHECK(EStopManager::IsEStopped());
  twistUp();
  advance(6000);
  CHECK(EStopManager::IsEStopped());
}

// An unwired pin reads open, which is stop. A hub that boots onto one stays engaged and never shows a button: no run
// level was ever measured, so no press can follow one (SP-156, SP-158).
static void unwiredPinAtBootStops()
{
  freshHub(true, kButton);
  cutCable();
  restartEStopManager(true);
  advance(3 * 60 * 1000);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 50));

  // A warm restart of a released hub onto an unwired pin engages with no button source, however long it stays open.
  armedHub();
  cutCable();
  restartEStopManager(false);
  advance(3 * 60 * 1000);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::Unconfirmed);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());
}

// Contact bounce as the mushroom is twisted up does not clear. The clear is timed from the first full low window after
// the last bounce, so the hub stays engaged until a stable closed contact has lasted the clear time.
static void bounceOnCloseDoesNotClear()
{
  armedHub();
  engageByButton();
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 8; ++i) {
      s_gpioLevel[kButton] = i % 2 == 0 ? kRunLevel : kStopLevel;
      advance(5);
    }
    twistUp();
    advance(1500);
  }
  for (int i = 0; i < 8; ++i) {
    s_gpioLevel[kButton] = i % 2 == 0 ? kRunLevel : kStopLevel;
    advance(5);
  }
  const int64_t lastBounce = g_nowMs;
  twistUp();
  advance(5000);
  CHECK(g_nowMs - lastBounce == 5000);
  CHECK(EStopManager::IsEStopped());
  advance(100);
  CHECK(!EStopManager::IsEStopped());
}

// Upstream enabled keep-alive on every Idle state, so a stored `false` came back after each clear.
static void keepAliveFollowsTheStoredSetting()
{
  armedHub();
  const int starts = s_keepAliveStarts;
  engageByButton();
  twistToClear();
  CHECK(s_keepAliveStarts == starts);

  s_cfgKeepAlive = true;
  engageByButton();
  CHECK(s_keepAliveStarts == starts);
  twistToClear();
  CHECK(s_keepAliveStarts == starts + 1);
  CHECK(eTaskGetState(s_keepAliveTask) != eDeleted);

  // An engaged E-Stop stops the task, whatever the setting.
  engageByButton();
  CHECK(eTaskGetState(s_keepAliveTask) == eDeleted);
  s_cfgKeepAlive = false;
  twistToClear();
  CHECK(s_keepAliveStarts == starts + 1);
}

// `CommandHandler::Init` runs once in a process, so a boot scenario runs in a child forked before any task exists.
static void inChildProcess(void (*scenario)())
{
  const pid_t child = fork();
  if (child == 0) {
    scenario();
    std::_Exit(s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
  }
  int status = 0;
  CHECK(child > 0 && waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
}

// A boot on an engaged latch starts no keep-alive, whatever the stored setting. The clear starts it.
static void bootKeepAliveWaitsForTheClear()
{
  inChildProcess([] {
    s_cfgEnabled   = true;
    s_cfgPin       = kButton;
    s_cfgKeepAlive = true;
    restartEStopManager(true);
    CHECK(EStopManager::IsEStopped());
    CHECK(CommandHandler::Init());
    CHECK(CommandHandler::Ok());
    advance(50);
    CHECK(s_keepAliveStarts == 0);
    twistToClear();
    CHECK(!EStopManager::IsEStopped());
    CHECK(s_keepAliveStarts == 1);
  });
}

// A warm restart that comes back released keeps the stored setting at boot.
static void bootKeepAliveStartsWhenReleased()
{
  inChildProcess([] {
    s_cfgEnabled   = true;
    s_cfgPin       = kButton;
    s_cfgKeepAlive = true;
    restartEStopManager(true);
    advance(50);
    twistToClear();
    CHECK(!EStopManager::IsEStopped());
    restartEStopManager(false);
    CHECK(!EStopManager::IsEStopped());
    CHECK(CommandHandler::Init());
    CHECK(s_keepAliveStarts == 1);
  });
}

static void corruptLatchReadsAsColdBoot()
{
  const EStopLatchSource all[] = {EStopLatchSource::Released, EStopLatchSource::ColdBoot, EStopLatchSource::Gateway, EStopLatchSource::Button, EStopLatchSource::Unconfirmed};
  RetainedEStop r {};
  EStopLatchSource source = EStopLatchSource::Released;
  for (EStopLatchSource value : all) {
    SealEStop(r, value);
    CHECK(RestoreEStop(r, source) && source == value);
  }
  for (uint32_t bit = 0; bit < 32; ++bit) {
    for (EStopLatchSource value : all) {
      SealEStop(r, value);
      r.latch ^= 1u << bit;
      CHECK(!RestoreEStop(r, source));
      SealEStop(r, value);
      r.magic ^= 1u << bit;
      CHECK(!RestoreEStop(r, source));
      SealEStop(r, value);
      r.check ^= 1u << bit;
      CHECK(!RestoreEStop(r, source));
    }
  }

  // A record of an older image is well formed under its own magic. Only the magic rejects it.
  for (EStopLatchSource value : all) {
    SealEStop(r, value);
    r.magic = kRetainedEStopMagic - 1;
    r.check = RetainedEStopCheck(r.magic, r.latch);
    CHECK(!RestoreEStop(r, source));
  }

  // Corruption on a real boot reads as the cold boot case, which stays engaged, whatever the latch held before.
  armedHub();
  s_retainedLatch.check ^= 1u;
  restartEStopManager(false);
  CHECK(EStopManager::GetLatchSource() == EStopLatchSource::ColdBoot);
}

// A mushroom down across a warm restart keeps the hub stopped for as long as it stays down, and the restarted task
// measures it down again, so the twist up clears it as usual.
static void heldButtonAcrossRestartStaysEngaged()
{
  armedHub();
  pushDown();
  advance(20);
  CHECK(EStopManager::IsEStopped());
  restartEStopManager(false);
  advance(6000);
  CHECK(EStopManager::IsEStopped());
  twistUp();
  advance(4900);
  CHECK(EStopManager::IsEStopped());
  advance(300);
  CHECK(!EStopManager::IsEStopped());
}

// Finding: `estop pin` and `estop enabled` restarted the task, which cleared the latch.
static void serialCannotClearTheLatch()
{
  armedHub();
  engageByButton();

  _handleEStopPinCommand("14"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(EStopManager::GetRunningPin() == kButton);
  CHECK(s_cfgPin == kButton);
  CHECK(EStopManager::IsEStopped());

  g_serialOut.clear();
  _handleEStopEnabledCommand("false"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(s_estopTask != nullptr);
  CHECK(s_cfgEnabled);
  advance(20);
  CHECK(EStopManager::IsEStopped());
}

// The latch outlives the task on any route, including one that gets past the serial refusals.
static void taskRestartKeepsTheLatch()
{
  armedHub();
  engageByButton();
  CHECK(estopmgr_setestopenabled(false));
  CHECK(!EStopManager::IsArmed());
  CHECK(EStopManager::IsEStopped());
  CHECK(estopmgr_setestopenabled(true));
  advance(20);
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::IsArmed());
}

// Review finding F5: a checker task that starts on an engaged latch starts in `Active`, so it never publishes `Idle`
// while the hub is engaged. First a gateway stop with the mushroom up and a task restart, then a cold boot latch that a
// task starts on later.
static void restartedTaskPublishesEngaged()
{
  armedHub();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  CHECK(EStopManager::IsEStopped());
  CHECK(!s_published.empty() && s_published.back() == EStopState::Active);
  CHECK(estopmgr_setestopenabled(false));
  CHECK(estopmgr_setestopenabled(true));
  s_published.clear();
  advance(200);
  CHECK(EStopManager::IsEStopped());
  CHECK(std::find(s_published.begin(), s_published.end(), EStopState::Idle) == s_published.end());

  // With no task the cold boot latch has published nothing. The task that starts on it publishes `Active` once.
  freshHub();
  CHECK(s_estopTask == nullptr);
  _handleEStopPinCommand("13"sv, false);
  _handleEStopEnabledCommand("true"sv, false);
  advance(50);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::IsEStopped());
  CHECK(s_published.size() == 1 && s_published.front() == EStopState::Active);
}

static void capsLockTheEStopSettings()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopEnabled(false));
  CHECK(EStopManager::IsArmed());

  // Clearing the caps unlocks them, and the same-pin write stays a no-op that succeeds. The new pin has a closed
  // contact wired, so the move does not engage.
  CHECK(SafetyCaps::Clear());
  twistUp(static_cast<gpio_num_t>(14));
  CHECK(EStopManager::SetEStopPin(kButton));
  CHECK(EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(EStopManager::GetRunningPin() == static_cast<gpio_num_t>(14));
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(!EStopManager::IsEStopped());
  CHECK(EStopManager::SetEStopEnabled(false));
  CHECK(!EStopManager::IsArmed());
}

// Review D4: a clear whose erase fails left `AnySet` false for that boot, so `estop pin` and `estop enabled false`
// opened while the stored caps waited for the next boot. The settings now stay locked until an erase commits. Every
// command still refuses, because the caps in RAM are 0.
static void failedClearKeepsTheEStopLocked()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(SafetyCaps::AnySet());

  g_nvs.eraseError = ESP_FAIL;
  g_serialOut.clear();
  _handleSafetyCapsClearCommand(""sv, false);
  CHECK(g_serialOut.find("Caps are 0 until restart, but erasing the stored caps failed") != std::string::npos);
  CHECK(g_nvs.present);
  CHECK(SafetyCaps::AnySet());
  CHECK(!EStopManager::SetEStopEnabled(false));
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::CapsNotSet));

  // `safetycaps` still reads 0 with set=false, as the recovery text in SOFTPROXY.md says.
  g_serialOut.clear();
  _handleSafetyCapsGetCommand(""sv, false);
  CHECK(g_serialOut.find("SafetyCaps|vibrate|maxIntensityPct=0,") != std::string::npos);

  // A failed open and a failed commit lock them the same way (review 2 C1). Each starts from caps stored again, which
  // also drops the flag the failed erase left, so a clear that unlocked on either failure would open the settings here.
  g_nvs.eraseError = ESP_OK;
  for (esp_err_t FakeNvs::*fault : {&FakeNvs::openError, &FakeNvs::commitError}) {
    engageByButton();
    CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
    twistToClear();
    CHECK(!EStopManager::IsEStopped());
    CHECK(SafetyCaps::AnySet());

    g_nvs.*fault = ESP_FAIL;
    g_serialOut.clear();
    _handleSafetyCapsClearCommand(""sv, false);
    g_nvs.*fault = ESP_OK;
    CHECK(g_serialOut.find("Caps are 0 until restart, but erasing the stored caps failed") != std::string::npos);
    CHECK(SafetyCaps::AnySet());
    CHECK(!EStopManager::SetEStopEnabled(false));
    CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
    CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
    CHECK(lastRefusalIs(Verdict::CapsNotSet));
  }

  // A clear that commits opens them again.
  CHECK(SafetyCaps::Clear());
  CHECK(!g_nvs.present);
  CHECK(!SafetyCaps::AnySet());
  CHECK(EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
}

static void enableNeedsAValidPin()
{
  freshHub();
  _handleEStopEnabledCommand("true"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(!s_cfgEnabled);
  CHECK(s_estopTask == nullptr);
  CHECK(!EStopManager::IsArmed());

  // A stored config that asks for it at boot gets the same answer, and the hub still boots.
  freshHub(true, GPIO_NUM_NC);
  CHECK(s_estopTask == nullptr);
  CHECK(!EStopManager::IsArmed());

  g_serialOut.clear();
  _handleEStopPinCommand("13"sv, false);
  _handleEStopEnabledCommand("true"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  advance(20);
  CHECK(EStopManager::IsArmed());
}

// Review finding F1: `estop enabled true` applies the pin rule to the stored pin, as `Init` does. A stored pin that the
// rule refuses leaves the E-Stop off at boot, and enabling it afterwards refuses too: the transmitter, the board LED, a
// pin with no pull-up and a strapping pin.
static void enableRefusesAStoredConflictingPin()
{
  const auto tx = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
  for (gpio_num_t pin : {tx, static_cast<gpio_num_t>(kBoardLedPin), static_cast<gpio_num_t>(36), static_cast<gpio_num_t>(0)}) {
    freshHub(true, pin);
    CHECK(s_estopTask == nullptr);
    CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);

    g_serialOut.clear();
    _handleEStopEnabledCommand("true"sv, false);
    CHECK(g_serialOut.find("Error|") != std::string::npos);
    CHECK(s_estopTask == nullptr);
    CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
    CHECK(std::find(s_gpioConfigured.begin(), s_gpioConfigured.end(), static_cast<int>(pin)) == s_gpioConfigured.end());
    advance(20);
    CHECK(!EStopManager::IsArmed());
    CHECK(EStopManager::IsEStopped());
  }
}

// Review finding F6: the pin rule reads the stored RF TX pin, and a failed read refuses the pin on every route, serial
// `estop pin`, the stored pin at boot and `estop enabled true`. It passes again once the read works.
static void failedTxReadRefusesThePin()
{
  freshHub();
  s_cfgTxReadFails = true;
  CHECK(!EStopManager::SetEStopPin(kButton));
  g_serialOut.clear();
  _handleEStopPinCommand("13"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
  CHECK(s_cfgPin == GPIO_NUM_NC);

  s_cfgEnabled = true;
  s_cfgPin     = kButton;
  restartEStopManager(true);
  CHECK(s_estopTask == nullptr);
  CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
  g_serialOut.clear();
  _handleEStopEnabledCommand("true"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(s_estopTask == nullptr);
  advance(20);
  CHECK(!EStopManager::IsArmed());

  s_cfgTxReadFails = false;
  g_serialOut.clear();
  _handleEStopEnabledCommand("true"sv, false);
  CHECK(g_serialOut.find("Error|") == std::string::npos);
  advance(20);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::GetRunningPin() == kButton);
}

// A stalled or dead checker reads as not armed, and it recovers when it samples again.
static void stalledCheckerIsNotArmed()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();
  s_stallChecker = true;
  advance(k_estopSampleStaleTime + 50);
  CHECK(!EStopManager::IsArmed());
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopNotArmed));
  s_stallChecker = false;
  advance(20);
  CHECK(EStopManager::IsArmed());

  // A live task on a pin that is no valid input is not armed. No public path reaches this state, so the file's own
  // variable sets it up.
  const gpio_num_t pin = s_estopPin;
  s_estopPin           = GPIO_NUM_NC;
  CHECK(!EStopManager::IsArmed());
  s_estopPin = pin;
  CHECK(EStopManager::IsArmed());
  (void)SafetyCaps::Clear();
}

// Review finding: the clear ran on wall time, so a checker that stalled past the clear time released on its first low
// sample after the stall. The clear now counts samples, and every one of them must read the contact closed.
static void stalledCheckerCannotSkipTheClear()
{
  armedHub();
  engageByButton();
  twistUp();
  advance(1000);
  CHECK(s_published.back() == EStopState::ActiveClearing);
  s_stallChecker = true;
  advance(k_estopClearTime + 1000);
  s_stallChecker = false;
  advance(5);
  CHECK(EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::ActiveClearing);

  // The clear started 65 milliseconds after the twist, so 187 of its 1000 samples ran before the stall. The other 813
  // take 4065 milliseconds.
  advance(4000);
  CHECK(EStopManager::IsEStopped());
  advance(100);
  CHECK(!EStopManager::IsEStopped());
  CHECK(s_published.back() == EStopState::Idle);
}

// Review H2: a checker that stalls cannot latch a press, so the transmit task ends output once the hub is not armed,
// about 1000 milliseconds after the last sample, instead of running the admitted pulse to its end.
static void stalledCheckerEndsAPulseInFlight()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();

  Rmt::Sequence expected(kModel, 7, 0);
  CHECK(expected.fill(ShockerCommandType::Vibrate, 20));
  const std::vector<uint32_t> payload(reinterpret_cast<const uint32_t*>(expected.payload()), reinterpret_cast<const uint32_t*>(expected.payload()) + expected.size());
  const std::vector<uint32_t> terminator(reinterpret_cast<const uint32_t*>(expected.terminator()), reinterpret_cast<const uint32_t*>(expected.terminator()) + expected.size());

  s_air.clear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(100);
  s_stallChecker        = true;
  const int64_t stalled = g_nowMs;
  advance(1500);
  CHECK(!EStopManager::IsArmed());
  CHECK(!EStopManager::IsEStopped());

  std::size_t payloadFrames = 0;
  for (const auto& frame : s_air) {
    if (frame.words == payload) {
      ++payloadFrames;
      CHECK(frame.atMs <= stalled + static_cast<int64_t>(k_estopSampleStaleTime) + 15);
    } else {
      CHECK(frame.words == terminator);
    }
  }
  CHECK(payloadFrames > 5);
  CHECK(s_air.back().words == terminator);
  const std::size_t frames = s_air.size();
  advance(500);
  CHECK(s_air.size() == frames);

  s_stallChecker = false;
  advance(20);
  CHECK(EStopManager::IsArmed());
  (void)SafetyCaps::Clear();
}

// Review H2, the other side: a hub that is not armed still sends a stop frame, a keep-alive and Sound for their full
// length. A vibrate above 0 and a shock end before any payload frame. A transmitter of the test's own sends those,
// since `HandleCommand` refuses shock and vibrate first.
static void notArmedKeepsTheStopFrameAndTheKeepAlive()
{
  releasedHubWithNoTask();
  CHECK(!EStopManager::IsArmed());
  CHECK(!EStopManager::IsEStopped());

  Rmt::Sequence stop(kModel, 7, 0);
  CHECK(stop.fill(ShockerCommandType::Vibrate, 0));
  const std::vector<uint32_t> stopFrame(reinterpret_cast<const uint32_t*>(stop.payload()), reinterpret_cast<const uint32_t*>(stop.payload()) + stop.size());

  // A Stop through the real `HandleCommand`: 300 milliseconds of stop frames, then the 300 millisecond terminator, which
  // is the same frame. Ended at once it would last only the terminator.
  s_air.clear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Stop, 0, 0));
  advance(1000);
  CHECK(!s_air.empty());
  for (const auto& frame : s_air) {
    CHECK(frame.words == stopFrame);
  }
  CHECK(!s_air.empty() && s_air.back().atMs - s_air.front().atMs >= 550);

  {
    OpenShock::RFTransmitter radio(static_cast<gpio_num_t>(27));
    CHECK(radio.ok());

    // The keep-alive: vibrate at intensity 0 for 300 milliseconds, not overwriting.
    s_air.clear();
    CHECK(radio.SendCommand(kModel, 7, ShockerCommandType::Vibrate, 0, 300, false));
    advance(1000);
    CHECK(!s_air.empty() && s_air.back().atMs - s_air.front().atMs >= 550);

    // Sound carries no cap and passes `HandleCommand` when not armed, so it runs its length.
    Rmt::Sequence sound(kModel, 7, 0);
    CHECK(sound.fill(ShockerCommandType::Sound, 0));
    const std::vector<uint32_t> soundFrame(reinterpret_cast<const uint32_t*>(sound.payload()), reinterpret_cast<const uint32_t*>(sound.payload()) + sound.size());
    s_air.clear();
    CHECK(radio.SendCommand(kModel, 7, ShockerCommandType::Sound, 0, 1000));
    advance(1500);
    std::size_t soundFrames = 0;
    for (const auto& frame : s_air) {
      soundFrames += frame.words == soundFrame ? 1 : 0;
    }
    CHECK(soundFrames > 150);

    // Output ends on the pass that takes it: no payload frame reaches the air.
    for (ShockerCommandType type : {ShockerCommandType::Vibrate, ShockerCommandType::Shock}) {
      Rmt::Sequence out(kModel, 7, 0);
      CHECK(out.fill(type, 20));
      const std::vector<uint32_t> outFrame(reinterpret_cast<const uint32_t*>(out.payload()), reinterpret_cast<const uint32_t*>(out.payload()) + out.size());
      s_air.clear();
      CHECK(radio.SendCommand(kModel, 7, type, 20, 2000));
      advance(1000);
      CHECK(!s_air.empty());
      for (const auto& frame : s_air) {
        CHECK(frame.words != outFrame);
      }
    }
  }

  // The decision itself, for every type and intensity.
  for (int raw = 0; raw <= static_cast<int>(ShockerCommandType::Light); ++raw) {
    const auto type = static_cast<ShockerCommandType>(raw);
    for (int intensity = 0; intensity <= 255; ++intensity) {
      const auto i        = static_cast<uint8_t>(intensity);
      const bool carrying = type == ShockerCommandType::Shock || (type == ShockerCommandType::Vibrate && i > 0);
      CHECK(TransmitEndsSequence(true, true, type, i));
      CHECK(TransmitEndsSequence(true, false, type, i));
      CHECK(!TransmitEndsSequence(false, true, type, i));
      CHECK(TransmitEndsSequence(false, false, type, i) == carrying);
    }
  }
}

// Review H2: the checker posted its state event with no time limit, so a full event queue held it there and the
// mushroom went unread. The wait is now bounded, the checker keeps sampling, and the state posts on a later pass.
static void eventPostCannotHoldTheChecker()
{
  armedHub();
  s_eventPostWaits.clear();
  s_eventPostResult = ESP_FAIL;
  engageByButton();
  CHECK(EStopManager::IsEStopped());
  CHECK(!s_eventPostWaits.empty());
  for (TickType_t wait : s_eventPostWaits) {
    CHECK(wait != portMAX_DELAY);
    CHECK(wait <= pdMS_TO_TICKS(k_estopUpdateRate));
  }
  advance(k_estopSampleStaleTime + 500);
  CHECK(EStopManager::IsArmed());
  CHECK(s_published.empty());

  s_eventPostResult = ESP_OK;
  advance(10);
  CHECK(!s_published.empty() && s_published.back() == EStopState::Active);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(!s_published.empty() && s_published.back() == EStopState::Idle);
}

// Review T2: a post that keeps failing logged once and then stayed silent. Each failure now counts, `estop state`
// prints the count, and the failure logs again at most every 10 seconds while it lasts.
static void failedStatePostsCountAndLogAgain()
{
  armedHub();
  s_eventPostResult = ESP_FAIL;
  engageByButton();
  CHECK(EStopManager::IsEStopped());
  CHECK(EStopManager::FailedStatePosts() > 0);
  CHECK(g_lastLog.find("Failed to post the EStop state") != std::string::npos);

  g_lastLog.clear();
  advance(k_estopPostFailureLogIntervalMs - 1000);
  CHECK(g_lastLog.find("Failed to post the EStop state") == std::string::npos);
  advance(1000);
  CHECK(g_lastLog.find("Failed to post the EStop state") != std::string::npos);
  CHECK(g_lastLog.find(" failed since boot") != std::string::npos);
  CHECK(EStopManager::FailedStatePosts() > 1000);  // one a pass, every 5 ms, for 10 s

  g_serialOut.clear();
  const uint32_t failed = EStopManager::FailedStatePosts();
  _handleEStopStateCommand(""sv, false);
  CHECK(g_serialOut.find("failedStatePosts=" + std::to_string(failed) + "\n") != std::string::npos);

  // The post succeeds again: the state reaches the event loop and the count stays as a record.
  s_eventPostResult = ESP_OK;
  advance(10);
  CHECK(!s_published.empty() && s_published.back() == EStopState::Active);
  CHECK(EStopManager::FailedStatePosts() == failed);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
}

// Review T1: the checker's longest sample interval is kept, so the bench can read the real worst gap. A stall of 400 ms
// shows as at least 400. `estop state` prints it on a plain line before the reply line, which stays as Softproxy reads it.
static void sampleIntervalHighWaterIsReadable()
{
  armedHub();
  CHECK(EStopManager::MaxSampleIntervalMs() >= k_estopUpdateRate);
  CHECK(EStopManager::MaxSampleIntervalMs() < 100);
  s_stallChecker = true;
  advance(400);
  s_stallChecker = false;
  advance(20);
  CHECK(EStopManager::MaxSampleIntervalMs() >= 400);
  CHECK(EStopManager::MaxSampleIntervalMs() <= 410);
  advance(200);
  CHECK(EStopManager::MaxSampleIntervalMs() >= 400);  // a high-water mark: shorter intervals leave it

  g_serialOut.clear();
  _handleEStopStateCommand(""sv, false);
  const std::string health   = "plain|softproxy-estop-health maxSampleIntervalMs=" + std::to_string(EStopManager::MaxSampleIntervalMs()) + " failedStatePosts=0\n";
  const std::size_t healthAt = g_serialOut.find(health);
  const std::size_t stateAt  = g_serialOut.find("Response|EStopState|armed=true,engaged=false,pin=13,source=released\n");
  CHECK(healthAt != std::string::npos);
  CHECK(stateAt != std::string::npos);
  CHECK(healthAt < stateAt);
}

// Review T3: `IsArmed` reads the task handle and the pin on the RF task without the mutex, so both are atomic.
static_assert(std::is_same_v<decltype(s_estopTask), std::atomic<TaskHandle_t>>, "the RF task reads the handle without the mutex");
static_assert(std::is_same_v<decltype(s_estopPin), std::atomic<gpio_num_t>>, "the RF task reads the pin without the mutex");

// Review H5: a task restarted by a pin move is not armed until it samples, whatever the old task left behind.
static void restartedTaskIsNotArmedBeforeItsFirstSample()
{
  armedHub();
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(s_estopTask != nullptr);
  CHECK(!EStopManager::IsArmed());
  advance(5);  // The task's first pass sleeps one sample period before it reads the pin.
  CHECK(!EStopManager::IsArmed());
  advance(5);
  CHECK(EStopManager::IsArmed());
}

// Review H5 at boot: within the first second of uptime the sample time 0 used to pass the staleness test.
static void bootIsNotArmedBeforeTheFirstSample()
{
  inChildProcess([] {
    s_cfgEnabled         = true;
    s_cfgPin             = kButton;
    s_gpioLevel[kButton] = kRunLevel;
    OpenShock::g_nowMs   = 200;
    restartEStopManager(true);
    CHECK(s_estopTask != nullptr);
    CHECK(!EStopManager::IsArmed());
    advance(10);
    CHECK(EStopManager::IsArmed());
  });
}

// SP-113 rule 2.
static void capRiseNeedsAnEngagedEStop()
{
  freshHub();
  CHECK(SafetyCaps::Set(ShockerCommandType::Shock, lowCaps()) == SetResult::NeedsEngagedEStop);

  // Engaged by the gateway with no checker task: engaged, but nothing behind it.
  releasedHubWithNoTask();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  CHECK(EStopManager::IsEStopped());
  CHECK(SafetyCaps::Set(ShockerCommandType::Shock, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());

  armedHub();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);
  CHECK(!SafetyCaps::AnySet());

  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  twistToClear();

  // Lowering, rewriting and clearing take no engaged E-Stop. Each field that rises refuses.
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::NeedsEngagedEStop);
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::Stored);

  ChannelCaps sooner = lowCaps();
  sooner.minGapSec   = 1;
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, sooner) == SetResult::NeedsEngagedEStop);
  ChannelCaps joined = lowCaps();
  joined.stageJoinMs = 1;
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, joined) == SetResult::NeedsEngagedEStop);

  // A shorter join window needs the E-Stop too: it can turn a refused `over-pulses` into a new stage.
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, joined) == SetResult::Stored);
  twistToClear();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, lowCaps()) == SetResult::NeedsEngagedEStop);

  CHECK(SafetyCaps::Clear());
  CHECK(!SafetyCaps::AnySet());
}

static void capsRiseCoversEveryField()
{
  const ChannelCaps base = lowCaps();
  CHECK(!CapsRise(base, base));
  CHECK(CapsRise(ChannelCaps {}, base));
  CHECK(!CapsRise(base, ChannelCaps {}));
  CHECK(!CapsRise(highCaps(), base));
  auto bumped = [&](auto field, auto value) {
    ChannelCaps c = base;
    c.*field      = value;
    return CapsRise(base, c);
  };
  CHECK(bumped(&ChannelCaps::maxIntensityPct, uint8_t {11}));
  CHECK(bumped(&ChannelCaps::maxPulseMs, uint16_t {101}));
  CHECK(bumped(&ChannelCaps::maxPulsesPerStage, uint16_t {2}));
  CHECK(bumped(&ChannelCaps::minGapSec, uint32_t {1}));
  CHECK(bumped(&ChannelCaps::maxPerHour, uint16_t {11}));
  CHECK(bumped(&ChannelCaps::maxPerDay, uint16_t {21}));
  CHECK(bumped(&ChannelCaps::stageJoinMs, uint32_t {1}));
  ChannelCaps joined = base;
  joined.stageJoinMs = 500;
  CHECK(CapsRise(joined, base));
  for (EStopLatchSource source : {EStopLatchSource::Released, EStopLatchSource::ColdBoot, EStopLatchSource::Gateway, EStopLatchSource::Button, EStopLatchSource::Unconfirmed}) {
    CHECK(!CapRiseAllowed(false, source));
    CHECK(CapRiseAllowed(true, source) == (source == EStopLatchSource::Button));
    CHECK(EStopSettingsLocked(source, true));
    CHECK(EStopSettingsLocked(source, false) == (source == EStopLatchSource::Button || source == EStopLatchSource::Gateway || source == EStopLatchSource::Unconfirmed));
  }
}

// SP-113 rule 1, in both directions and at boot.
static void pinsStaySeparate()
{
  const auto tx  = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
  const auto led = static_cast<gpio_num_t>(kBoardLedPin);
  CHECK(kBoardRfTxPin == OPENSHOCK_RF_TX_GPIO);
  CHECK(kBoardLedPin == 2);

  freshHub();
  CHECK(!EStopManager::SetEStopPin(tx));
  CHECK(!EStopManager::SetEStopPin(led));
  CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
  for (int pin = 34; pin <= 39; ++pin) {
    CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(pin)));
  }

  // The strapping pins and the flash pins, on both PiShock builds. The fake `Chipset` accepts all of them, so only the
  // E-Stop rule refuses here. Each pin is refused by `IsEStopReservedPin` itself, also where the LED or this env's
  // transmitter would refuse it anyway, so both envs refuse 12 and 15.
  for (int pin : {0, 2, 5, 12, 15, 6, 7, 8, 9, 10, 11}) {
    CHECK(IsEStopReservedPin(pin));
    CHECK(EStopPinConflicts(pin, 27));
    CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(pin)));
    CHECK(!KillConfigChangeAllowed(KillConfig {false, -1}, KillConfig {true, pin}, 27, -1, false));
    g_serialOut.clear();
    _handleEStopPinCommand(std::to_string(pin), false);
    CHECK(g_serialOut.find("Error|") != std::string::npos);
    CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
    CHECK(s_cfgPin == GPIO_NUM_NC);
  }
  for (int pin : {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}) {
    CHECK(!IsEStopReservedPin(pin));
  }

  // The transmitter was moved off the default: the default stays out of bounds, since a bad config falls back to it.
  CHECK(CommandHandler::SetRfTxPin(static_cast<gpio_num_t>(14)) == SetGPIOResultCode::Success);
  CHECK(!EStopManager::SetEStopPin(static_cast<gpio_num_t>(14)));
  CHECK(!EStopManager::SetEStopPin(tx));

  // The other direction, against the running pin, the stored pin and the LED.
  armedHub();
  CHECK(CommandHandler::SetRfTxPin(kButton) == SetGPIOResultCode::InvalidPin);
  CHECK(CommandHandler::SetRfTxPin(led) == SetGPIOResultCode::InvalidPin);
  CHECK(CommandHandler::GetRfTxPin() == tx);
  s_cfgPin = static_cast<gpio_num_t>(27);
  CHECK(CommandHandler::SetRfTxPin(static_cast<gpio_num_t>(27)) == SetGPIOResultCode::InvalidPin);

  g_serialOut.clear();
  _handleRfTxPinCommand("13"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(CommandHandler::GetRfTxPin() == tx);

  // A bad argument stops at the parser, with no transmitter rebuilt on a garbage pin.
  g_serialOut.clear();
  _handleRfTxPinCommand("twelve"sv, false);
  CHECK(g_serialOut.find("Invalid argument (number invalid") != std::string::npos);
  CHECK(g_serialOut.find("Saved config") == std::string::npos);
  CHECK(CommandHandler::GetRfTxPin() == tx);

  // A stored config with the E-Stop on the transmitter: the hub boots, the task stays off and commands refuse.
  freshHub(true, tx);
  CHECK(s_estopTask == nullptr);
  CHECK(std::find(s_gpioConfigured.begin(), s_gpioConfigured.end(), static_cast<int>(tx)) == s_gpioConfigured.end());
  CHECK(!EStopManager::IsArmed());
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 1, 50));
  CHECK(lastRefusalIs(Verdict::EStopEngaged));

  // The same for a pin with no pull-up, a strapping pin and a stored transmitter on the E-Stop.
  freshHub(true, static_cast<gpio_num_t>(36));
  CHECK(!EStopManager::IsArmed());
  for (int pin : {0, 5, 12, 15}) {
    freshHub(true, static_cast<gpio_num_t>(pin));
    CHECK(s_estopTask == nullptr);
    CHECK(!EStopManager::IsArmed());
    CHECK(EStopManager::IsEStopped());
  }
  freshHub(true, kButton);
  s_cfgTx = kButton;
  restartEStopManager(true);
  CHECK(!EStopManager::IsArmed());
  s_cfgTx = tx;
}

// SP-113 rule 1 keys on the E-Stop pin set at run time. No PiShock env defines OPENSHOCK_ESTOP_PIN and the two envs
// differ on RF TX, so a compile-time comparison would miss every real hub. The transmitter of the other PiShock env is
// a strapping pin on both builds, so the E-Stop refuses it (pinsStaySeparate) and ordinary GPIOs stand in here.
static void pinRuleKeysOnTheRunTimePin()
{
  const auto tx = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);

  for (int number : {13, 14, 25}) {
    const auto pin           = static_cast<gpio_num_t>(number);
    const std::string digits = std::to_string(number);

    // No E-Stop pin set: nothing claims this GPIO, so the radio can go there and come back.
    freshHub();
    CHECK(s_cfgPin == GPIO_NUM_NC);
    CHECK(CommandHandler::SetRfTxPin(pin) == SetGPIOResultCode::Success);
    CHECK(CommandHandler::SetRfTxPin(tx) == SetGPIOResultCode::Success);

    // Serial `estop pin` sets it, stored and running. Serial `rftxpin` now refuses that GPIO.
    g_serialOut.clear();
    _handleEStopPinCommand(digits, false);
    CHECK(g_serialOut.find("Error|") == std::string::npos);
    CHECK(s_cfgPin == pin);
    CHECK(EStopManager::GetRunningPin() == pin);
    g_serialOut.clear();
    _handleRfTxPinCommand(digits, false);
    CHECK(g_serialOut.find("Error|") != std::string::npos);
    CHECK(CommandHandler::GetRfTxPin() == tx);
    CHECK(s_cfgTx == tx);

    // Stored but not running yet, as after a config write that waits for a restart.
    freshHub();
    s_cfgPin = pin;
    CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
    CHECK(CommandHandler::SetRfTxPin(pin) == SetGPIOResultCode::InvalidPin);

    // Running with a different stored pin, as after the stored pin moved away.
    freshHub(true, pin);
    s_cfgPin = static_cast<gpio_num_t>(27);
    CHECK(EStopManager::GetRunningPin() == pin);
    CHECK(CommandHandler::SetRfTxPin(pin) == SetGPIOResultCode::InvalidPin);
    CHECK(CommandHandler::GetRfTxPin() == tx);
  }

  // The other direction on this env's own transmitter, through the serial command.
  freshHub();
  g_serialOut.clear();
  _handleEStopPinCommand(std::to_string(OPENSHOCK_RF_TX_GPIO), false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
  CHECK(s_cfgPin == GPIO_NUM_NC);
}

// SP-113 rule 2 through serial `safetycaps`, the only path that writes a cap. A hub starts here on either PiShock env,
// with no E-Stop pin, so the button cannot engage it and every rise refuses.
static void serialCapRiseNeedsTheEStop()
{
  constexpr std::string_view kLow  = "10 100 1 2 10 20";
  constexpr std::string_view kHigh = "20 100 1 2 10 20";
  constexpr std::string_view kLess = "5 100 1 2 10 20";
  const char* const kRefused       = "Refused (raising a cap needs the E-Stop armed and engaged";

  const auto refused = [&](void (*handler)(std::string_view, bool), std::string_view args) {
    g_serialOut.clear();
    handler(args, false);
    return g_serialOut.find(kRefused) != std::string::npos;
  };
  const auto saved = [&](void (*handler)(std::string_view, bool), std::string_view args) {
    g_serialOut.clear();
    handler(args, false);
    return g_serialOut.find("Saved caps") != std::string::npos;
  };

  const auto storedVibrateIntensity = [] {
    ChannelCaps shock {};
    ChannelCaps vibrate {};
    SafetyCaps::Get(shock, vibrate);
    return vibrate.maxIntensityPct;
  };

  // No pin configured.
  freshHub();
  CHECK(s_cfgPin == GPIO_NUM_NC);
  CHECK(EStopManager::GetRunningPin() == GPIO_NUM_NC);
  CHECK(refused(_handleSafetyCapsShockCommand, kLow));
  CHECK(refused(_handleSafetyCapsVibrateCommand, kLow));
  CHECK(!SafetyCaps::AnySet());

  // Still no pin, engaged from the gateway. No button stands behind it, so it does not count.
  releasedHubWithNoTask();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  CHECK(EStopManager::IsEStopped());
  CHECK(refused(_handleSafetyCapsShockCommand, kLow));
  CHECK(refused(_handleSafetyCapsVibrateCommand, kLow));
  CHECK(!SafetyCaps::AnySet());

  // The attended order from SOFTPROXY.md, on the real serial commands. Armed on the cold boot latch refuses, and so
  // does armed and released.
  freshHub();
  _handleEStopPinCommand("13"sv, false);
  _handleEStopEnabledCommand("true"sv, false);
  advance(50);
  CHECK(EStopManager::IsArmed());
  CHECK(EStopManager::IsEStopped());
  CHECK(refused(_handleSafetyCapsVibrateCommand, kLow));
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(refused(_handleSafetyCapsVibrateCommand, kLow));
  CHECK(!SafetyCaps::AnySet());

  engageByButton();
  CHECK(saved(_handleSafetyCapsVibrateCommand, kLow));
  CHECK(SafetyCaps::AnySet());

  // Released again: a rise refuses and leaves the stored value, and a lower value needs no E-Stop.
  twistToClear();
  CHECK(!EStopManager::IsEStopped());
  CHECK(refused(_handleSafetyCapsVibrateCommand, kHigh));
  CHECK(storedVibrateIntensity() == 10);
  CHECK(saved(_handleSafetyCapsVibrateCommand, kLess));
  CHECK(storedVibrateIntensity() == 5);

  // A malformed request stops before any write, and clear always works.
  g_serialOut.clear();
  _handleSafetyCapsShockCommand("10 100"sv, false);
  CHECK(g_serialOut.find("Error|") != std::string::npos);
  g_serialOut.clear();
  _handleSafetyCapsClearCommand(""sv, false);
  CHECK(g_serialOut.find("Cleared caps") != std::string::npos);
  CHECK(!SafetyCaps::AnySet());
}

// The fake `Chipset` accepts 0 to 39, so a scan pins the real ESP32 list, which the PiShock envs compile.
static void flashPinIsUnsafe()
{
  std::ifstream in("include/Chipset.h");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::size_t begin = text.find("#ifdef OPENSHOCK_FW_CHIP_ESP32\n");
  CHECK(begin != std::string::npos);
  const std::string list = text.substr(begin, text.find("#endif", begin) - begin);
  CHECK(list.find("(pin) == GPIO_NUM_10 ") != std::string::npos);
}

// The decision that `Config::SaveFromJSON`, `SetRaw` and `SaveFromFlatBuffer` share. Those writers need the real
// filesystem, so test/native/test_config_writers.cpp links them on a fake one and runs each against this rule.
static void configWritesAreGated()
{
  const KillConfig on {true, 13};
  CHECK(KillConfigChangeAllowed(on, on, 12, 13, false));
  CHECK(KillConfigChangeAllowed(on, on, 12, 13, true));
  CHECK(KillConfigChangeAllowed(on, KillConfig {true, 14}, 12, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 14}, 12, 13, true));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {false, 13}, 12, 13, true));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, -1}, 12, 13, true));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 13}, 13, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 2}, 15, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, OPENSHOCK_RF_TX_GPIO}, 14, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 36}, 12, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 0}, 14, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 5}, 14, 13, false));
  CHECK(KillConfigChangeAllowed(KillConfig {false, -1}, KillConfig {false, -1}, 12, -1, false));
  // The transmitter side: the LED, and the pin the checker reads now while the stored pin moves away from it.
  CHECK(!KillConfigChangeAllowed(on, on, 2, 13, false));
  CHECK(!KillConfigChangeAllowed(on, KillConfig {true, 14}, 13, 13, false));
}

// Port 81: an endless fragment stream from the open access point.
struct DefragHarness {
  std::vector<std::vector<uint8_t>> delivered;
  std::vector<uint8_t> dropped;
  WebSocketDeFragger defrag;
  DefragHarness()
    : defrag([this](uint8_t, WebSocketMessageType, tcb::span<const uint8_t> data) { delivered.emplace_back(data.begin(), data.end()); }, kLocalSocketMaxMessageBytes, [this](uint8_t id) { dropped.push_back(id); })
  {
  }
};

static void fragmentStreamIsBounded()
{
  DefragHarness h;
  const std::vector<uint8_t> fragment(16, 0xAB);
  h.defrag.handler(1, WStype_FRAGMENT_BIN_START, fragment.data(), fragment.size());
  for (int i = 0; i < 2000; ++i) {
    h.defrag.handler(1, WStype_FRAGMENT, fragment.data(), fragment.size());
  }
  h.defrag.handler(1, WStype_FRAGMENT_FIN, fragment.data(), fragment.size());
  CHECK(h.dropped.size() == 1 && h.dropped[0] == 1);
  CHECK(h.delivered.empty());
  CHECK(g_panics == 0);
}

static void fragmentBoundIsExact()
{
  const std::vector<uint8_t> half(kLocalSocketMaxMessageBytes / 2, 1);
  const uint8_t one = 1;

  DefragHarness fits;
  fits.defrag.handler(1, WStype_FRAGMENT_BIN_START, half.data(), half.size());
  fits.defrag.handler(1, WStype_FRAGMENT_FIN, half.data(), half.size());
  CHECK(fits.dropped.empty());
  CHECK(fits.delivered.size() == 1 && fits.delivered[0].size() == kLocalSocketMaxMessageBytes);

  DefragHarness over;
  over.defrag.handler(1, WStype_FRAGMENT_BIN_START, half.data(), half.size());
  over.defrag.handler(1, WStype_FRAGMENT, half.data(), half.size());
  over.defrag.handler(1, WStype_FRAGMENT_FIN, &one, 1);
  CHECK(over.dropped.size() == 1);
  CHECK(over.delivered.empty());

  // One big first fragment, which the library bounds only at 15 kilobytes.
  DefragHarness big;
  const std::vector<uint8_t> tooBig(kLocalSocketMaxMessageBytes + 1, 1);
  big.defrag.handler(2, WStype_FRAGMENT_BIN_START, tooBig.data(), tooBig.size());
  CHECK(big.dropped.size() == 1 && big.dropped[0] == 2);

  // A dropped client does not touch another's message, and its later fragments do nothing.
  DefragHarness two;
  two.defrag.handler(1, WStype_FRAGMENT_BIN_START, tooBig.data(), tooBig.size());
  two.defrag.handler(3, WStype_FRAGMENT_BIN_START, &one, 1);
  two.defrag.handler(1, WStype_FRAGMENT, &one, 1);
  two.defrag.handler(1, WStype_FRAGMENT_FIN, &one, 1);
  two.defrag.handler(3, WStype_FRAGMENT_FIN, &one, 1);
  CHECK(two.dropped.size() == 1);
  CHECK(two.delivered.size() == 1 && two.delivered[0].size() == 2);
}

// A failed allocation drops the message instead of reaching the TinyVec panic, which restarts the hub.
static void allocationFailureDropsTheMessage()
{
  DefragHarness h;
  const std::vector<uint8_t> fragment(600, 1);
  h.defrag.handler(1, WStype_FRAGMENT_BIN_START, fragment.data(), fragment.size());
  s_failMallocAtOrAbove = 800;
  h.defrag.handler(1, WStype_FRAGMENT, fragment.data(), fragment.size());
  s_failMallocAtOrAbove = 0;
  CHECK(h.dropped.size() == 1);
  CHECK(g_panics == 0);

  TinyVec<uint8_t> vec;
  const uint8_t byte    = 1;
  s_failMallocAtOrAbove = 1;
  CHECK(!vec.try_append(&byte, 1));
  s_failMallocAtOrAbove = 0;
  CHECK(vec.size() == 0);
  CHECK(vec.try_append(&byte, 1));
  CHECK(vec.size() == 1);
  CHECK(vec.try_append(nullptr, 1));

  TinyVec<uint8_t, uint8_t> tiny;
  uint8_t block[200] = {};
  CHECK(tiny.try_append(block, 200));
  CHECK(!tiny.try_append(block, 100));
  CHECK(tiny.size() == 200);
}

// The real transmit task: an E-Stop ends a pulse in flight with its terminator, and nothing follows.
static void estopEndsAPulseInFlight()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();

  Rmt::Sequence expected(kModel, 7, 0);
  CHECK(expected.fill(ShockerCommandType::Vibrate, 20));
  const std::vector<uint32_t> payload(reinterpret_cast<const uint32_t*>(expected.payload()), reinterpret_cast<const uint32_t*>(expected.payload()) + expected.size());
  const std::vector<uint32_t> terminator(reinterpret_cast<const uint32_t*>(expected.terminator()), reinterpret_cast<const uint32_t*>(expected.terminator()) + expected.size());
  CHECK(payload != terminator);

  s_air.clear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(200);
  CHECK(s_air.size() > 5);
  for (const auto& frame : s_air) {
    CHECK(frame.words == payload);
  }

  // The pulse has 1800 milliseconds left when the button goes down.
  s_air.clear();
  const int64_t pressedAt = g_nowMs;
  pushDown();
  advance(10);
  CHECK(EStopManager::IsEStopped());
  CHECK(!CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(1000);

  std::size_t payloadFrames = 0;
  for (const auto& frame : s_air) {
    if (frame.words == payload) {
      ++payloadFrames;
      CHECK(frame.atMs <= pressedAt + 15);
    } else {
      CHECK(frame.words == terminator);
    }
  }
  CHECK(payloadFrames <= 3);
  CHECK(s_air.back().words == terminator);
  CHECK(s_air.back().atMs <= pressedAt + 10 + 300 + 15);

  // Quiet for good, and a command queued before the latch cannot start after it.
  const std::size_t frames = s_air.size();
  advance(500);
  CHECK(s_air.size() == frames);
  (void)SafetyCaps::Clear();
}

static void gatewayTriggerEndsAPulseInFlightWithNoTask()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(100);

  // The checker stops running while the pulse is on the air. The trigger must still end it.
  s_stallChecker = true;
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(500);
  s_air.clear();
  advance(100);
  CHECK(s_air.empty());
  s_stallChecker = false;
  (void)SafetyCaps::Clear();
}

// The last gate. `HandleCommand` refuses first, so only a command that got past it can reach this one. The queue has
// no check of its own, and a transmitter of the test's own stands in for that race.
static void radioDropsACommandQueuedWhileEngaged()
{
  armedHub();
  engageByButton();
  {
    OpenShock::RFTransmitter radio(static_cast<gpio_num_t>(27));
    CHECK(radio.ok());
    s_air.clear();
    for (int i = 0; i < 3; ++i) {
      CHECK(radio.SendCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000, false));
      advance(50);
    }
    CHECK(s_air.empty());
  }
}

// Review finding: the task forced terminators only when it saw the state change. A sequence added after a release,
// with `wasEstopped` still true from the engagement before, kept its full length if a new engagement landed before
// the compare. The transmit task sleeps with nothing to send, so it can hold that old value across a whole clear.
static void engagementAfterAPopEndsTheSequence()
{
  armedHub();
  engageByButton();
  CHECK(SafetyCaps::Set(ShockerCommandType::Vibrate, highCaps()) == SetResult::Stored);
  twistToClear();

  // The transmit task sees an engagement, ends this pulse, and goes to sleep.
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(100);
  pushDown();
  advance(500);
  twistUp();
  advance(100);
  twistToClear();
  CHECK(!EStopManager::IsEStopped());

  Rmt::Sequence expected(kModel, 7, 0);
  CHECK(expected.fill(ShockerCommandType::Vibrate, 20));
  const std::vector<uint32_t> payload(reinterpret_cast<const uint32_t*>(expected.payload()), reinterpret_cast<const uint32_t*>(expected.payload()) + expected.size());

  s_air.clear();
  s_onQueueDrained = [] {
    EStopManager::Trigger();
  };
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 2000));
  advance(1000);
  CHECK(!s_onQueueDrained);
  CHECK(EStopManager::IsEStopped());
  CHECK(!s_air.empty());
  for (const auto& frame : s_air) {
    CHECK(frame.words != payload);
  }
  const std::size_t frames = s_air.size();
  advance(500);
  CHECK(s_air.size() == frames);
  (void)SafetyCaps::Clear();
}

static void stateIsReadable()
{
  CHECK(EStopWireCode(false, false) == 0);
  CHECK(EStopWireCode(true, false) == 1);
  CHECK(EStopWireCode(false, true) == 2);
  CHECK(EStopWireCode(true, true) == 3);

  flatbuffers::FlatBufferBuilder builder;
  auto pong = GatewaySchema::CreatePong(builder, 1, 2, 3, 4, EStopWireCode(true, true));
  builder.Finish(pong);
  CHECK(flatbuffers::GetRoot<GatewaySchema::Pong>(builder.GetBufferPointer())->softproxy_estop() == 3);

  armedHub();
  g_serialOut.clear();
  _handleEStopStateCommand(""sv, false);
  CHECK(g_serialOut.find("EStopState|armed=true,engaged=false,pin=13,source=released") != std::string::npos);
  engageByButton();
  g_serialOut.clear();
  _handleEStopStateCommand(""sv, false);
  CHECK(g_serialOut.find("EStopState|armed=true,engaged=true,pin=13,source=button") != std::string::npos);
  twistToClear();
  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  advance(20);
  g_serialOut.clear();
  _handleEStopStateCommand(""sv, false);
  CHECK(g_serialOut.find("EStopState|armed=true,engaged=true,pin=13,source=gateway") != std::string::npos);

  freshHub();
  g_serialOut.clear();
  _handleEStopStateCommand(""sv, false);
  CHECK(g_serialOut.find("EStopState|armed=false,engaged=true,pin=-1,source=cold-boot") != std::string::npos);
}

#define SCENARIO(name)                             \
  do {                                             \
    const int failuresBefore = s_failures;         \
    name();                                        \
    if (s_failures != failuresBefore) {            \
      std::printf("scenario failed: %s\n", #name); \
    }                                              \
  } while (0)

int main()
{
  // `_Exit` below skips the stdio flush, because the task threads never join. A buffered failure line would be lost.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // These fork, so they run before any task thread exists.
  SCENARIO(bootKeepAliveWaitsForTheClear);
  SCENARIO(bootKeepAliveStartsWhenReleased);
  SCENARIO(bootIsNotArmedBeforeTheFirstSample);

  SCENARIO(unreadableCapsLockTheEStop);
  SCENARIO(bootWithTxOnEStopLeavesRadioOff);
  SCENARIO(noEStopRefuses);
  SCENARIO(armedHubAdmitsWithinCaps);
  SCENARIO(gatewayTriggerLatchesWithNoTask);
  SCENARIO(gatewayTriggerLatchesWithTask);
  SCENARIO(twistToClearNeedsTheFullTime);
  SCENARIO(aPushDuringTheClearStopsIt);
  SCENARIO(latchSurvivesWarmRestart);
  SCENARIO(coldBootStartsEngaged);
  SCENARIO(coldBootLatchLeavesSettingsOpen);
  SCENARIO(armedTriggerDoesNotAllowARise);
  SCENARIO(latchKeepsItsFirstSource);
  SCENARIO(gatewayLatchThenPressStaysGateway);
  SCENARIO(pressThenGatewayStopStillConfirms);
  SCENARIO(gatewayStopRacingTheReleaseHolds);
  SCENARIO(gatewayStopContendingForTheLatchHolds);
  SCENARIO(emergencyStopInterruptsAClear);
  SCENARIO(sourceSurvivesWarmRestart);
  SCENARIO(onlyAnAllowedResetKeepsTheLatch);
  SCENARIO(crashLoopNeverForcesAStop);
  SCENARIO(buttonLatchWithADeadCheckerAllowsNoRise);
  SCENARIO(oneHighSampleProvesNoPress);
  SCENARIO(pulseTrainClearsNothing);
  SCENARIO(bounceCountsAsOnePress);
  SCENARIO(noiseProvesNothing);
  SCENARIO(anOpenPinProvesNoPress);
  SCENARIO(cableCutStops);
  SCENARIO(unwiredPinAtBootStops);
  SCENARIO(bounceOnCloseDoesNotClear);
  SCENARIO(keepAliveFollowsTheStoredSetting);
  SCENARIO(corruptLatchReadsAsColdBoot);
  SCENARIO(heldButtonAcrossRestartStaysEngaged);
  SCENARIO(serialCannotClearTheLatch);
  SCENARIO(taskRestartKeepsTheLatch);
  SCENARIO(restartedTaskPublishesEngaged);
  SCENARIO(capsLockTheEStopSettings);
  SCENARIO(failedClearKeepsTheEStopLocked);
  SCENARIO(enableNeedsAValidPin);
  SCENARIO(enableRefusesAStoredConflictingPin);
  SCENARIO(failedTxReadRefusesThePin);
  SCENARIO(stalledCheckerIsNotArmed);
  SCENARIO(stalledCheckerCannotSkipTheClear);
  SCENARIO(stalledCheckerEndsAPulseInFlight);
  SCENARIO(notArmedKeepsTheStopFrameAndTheKeepAlive);
  SCENARIO(eventPostCannotHoldTheChecker);
  SCENARIO(failedStatePostsCountAndLogAgain);
  SCENARIO(sampleIntervalHighWaterIsReadable);
  SCENARIO(restartedTaskIsNotArmedBeforeItsFirstSample);
  SCENARIO(capRiseNeedsAnEngagedEStop);
  SCENARIO(capsRiseCoversEveryField);
  SCENARIO(pinsStaySeparate);
  SCENARIO(pinRuleKeysOnTheRunTimePin);
  SCENARIO(serialCapRiseNeedsTheEStop);
  SCENARIO(flashPinIsUnsafe);
  SCENARIO(configWritesAreGated);
  SCENARIO(fragmentStreamIsBounded);
  SCENARIO(fragmentBoundIsExact);
  SCENARIO(allocationFailureDropsTheMessage);
  SCENARIO(estopEndsAPulseInFlight);
  SCENARIO(gatewayTriggerEndsAPulseInFlightWithNoTask);
  SCENARIO(radioDropsACommandQueuedWhileEngaged);
  SCENARIO(engagementAfterAPopEndsTheSequence);
  SCENARIO(stateIsReadable);

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    std::_Exit(EXIT_FAILURE);
  }
  std::printf("kill path: all checks passed\n");
  std::_Exit(EXIT_SUCCESS);
}
