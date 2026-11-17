// Host test of the radio path (TIN-7545). It links the real `HandleCommand`, `RFTransmitter` and its
// encoders and the real `WebSocketDeFragger`. Fakes: RMT, the `Config` store, FreeRTOS, the number parser in
// `Convert` and `Chipset`, whose pin checks accept 0 to 39 instead of the board's own unsafe and bypassed pins.
//
// The FreeRTOS fake runs each task on its own thread with one thread awake at a time, and `advance` steps every task
// in 5 millisecond ticks, so a run is deterministic.
//
// radio_stubs holds symlinks to the real TinyVec.h and WebSocketDeFragger.h. A quoted include looks beside the including
// file first, so through include/ they would reach the real Logging.h and Common.h next to them instead of the stubs.
//
// Run from the repository root after `pio pkg install -e Pishock-2023`. The second line sets a PiShock layout: an ESP32
// with the LED on GPIO 2. Build it once per PiShock RF TX default, TX=12 for Pishock-2023 and TX=15 for
// Pishock-Lite-2021.
//
//   FB=.pio/libdeps/Pishock-2023/flatbuffers/include
//   FLAGS="-std=c++20 -O1 -pthread -DSOFTPROXY_RADIO_TEST -DOPENSHOCK_FW_CHIP_ESP32 -DOPENSHOCK_LED_GPIO=2"
//   FLAGS="$FLAGS -DOPENSHOCK_RF_TX_GPIO=$TX"
//   INC="-Itest/native/radio_stubs -Itest/native/stubs -Iinclude -I$FB"
//   SRC="src/CommandHandler.cpp src/WebSocketDeFragger.cpp src/radio/RFTransmitter.cpp"
//   SRC="$SRC src/radio/rmt/Sequence.cpp src/radio/rmt/*Encoder.cpp"
//   g++ $FLAGS $INC -Wl,--wrap=malloc test/native/test_radio_path.cpp $SRC -o radio_path_test && ./radio_path_test
//
// The test has no model of the receiver or of the board.

#include "CommandHandler.h"
#include "radio/RFTransmitter.h"
#include "radio/rmt/Sequence.h"
#include "softproxy/Lockout.h"
#include "util/TaskUtils.h"
#include "WebSocketDeFragger.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace OpenShock;
using namespace OpenShock::Softproxy;

// Fake state declared in host_fakes.h.
std::string g_lastLog;
std::string g_serialOut;
esp_event_base_t OPENSHOCK_EVENTS = "fake";
int g_restarts                    = 0;
int g_panics                      = 0;
namespace OpenShock {
  int64_t g_nowMs = 1'000'000'000;
  namespace Serialization::Local {
    std::string g_lastError;
  }
  namespace Serialization::JsonSerial {
    ShockerCommand g_serialCommand;
  }
}  // namespace OpenShock

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

  void stepTasks()
  {
    const std::vector<HostTask*> tasks = s_tasks;
    for (HostTask* task : tasks) {
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
BaseType_t TaskUtils::TaskCreateExpensive(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t, TaskHandle_t* out)
{
  *out = startTask(fn, arg);
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
      return 0;
    }
    hostYield();
  }
  std::memcpy(out, q->items.front().data(), q->itemSize);
  q->items.pop_front();
  return pdTRUE;
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

esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, void (*)(void*, esp_event_base_t, int32_t, void*), void*)
{
  return ESP_OK;
}

// `Config` store.
static gpio_num_t s_cfgTx = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
bool Config::GetRFConfig(Config::RFConfig& out)
{
  out = {.txPin = s_cfgTx, .keepAliveEnabled = false};
  return true;
}
bool Config::GetRFConfigKeepAliveEnabled(bool& out)
{
  out = false;
  return true;
}
bool Config::GetRFConfigTxPin(gpio_num_t& out)
{
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

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      ++s_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

static const auto kModel = ShockerModelType::CaiXianlin;

// The radio, started once. A later run reuses it 5 seconds on, after the last command's terminator.
static void hubWithRadio()
{
  static bool started = false;
  if (!started) {
    CHECK(CommandHandler::Init());
    started = true;
  }
  advance(5000);
  s_air.clear();
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

// A command through the real `HandleCommand` reaches the air through the real RF task: its payload frame repeats for the
// command's duration and then stops.
static void commandReachesTheAir()
{
  hubWithRadio();

  Rmt::Sequence expected(kModel, 7, 0);
  CHECK(expected.fill(ShockerCommandType::Vibrate, 20));
  const std::vector<uint32_t> payload(reinterpret_cast<const uint32_t*>(expected.payload()), reinterpret_cast<const uint32_t*>(expected.payload()) + expected.size());

  const int64_t start = g_nowMs;
  CHECK(CommandHandler::HandleCommand(kModel, 7, ShockerCommandType::Vibrate, 20, 1000));
  advance(1500);

  std::size_t frames = 0;
  int64_t lastMs     = 0;
  for (const auto& frame : s_air) {
    if (frame.words == payload) {
      ++frames;
      lastMs = frame.atMs;
    }
  }
  CHECK(frames > 10);
  CHECK(lastMs < start + 1000 + 100);
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

  SCENARIO(flashPinIsUnsafe);
  SCENARIO(fragmentStreamIsBounded);
  SCENARIO(fragmentBoundIsExact);
  SCENARIO(allocationFailureDropsTheMessage);
  SCENARIO(commandReachesTheAir);

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    std::_Exit(EXIT_FAILURE);
  }
  std::printf("radio path: all checks passed\n");
  std::_Exit(EXIT_SUCCESS);
}
