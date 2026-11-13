#include <freertos/FreeRTOS.h>

#include "estop/EStopManager.h"

const char* const TAG = "EStopManager";

#include "Chipset.h"
#include "config/Config.h"
#include "Core.h"
#include "events/Events.h"
#include "Logging.h"
#include "SimpleMutex.h"
#include "softproxy/KillPath.h"
#include "softproxy/SafetyCaps.h"
#include "util/TaskUtils.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include <atomic>
#include <cstdint>

using namespace OpenShock;

// The E-Stop is a latching mushroom with a normally closed contact to ground, and the input is pulled up
// (R-SOFTPROXY-ESTOP-NC, SP-174). Closed reads low and means run. Open reads high and means stop: a pressed mushroom, a cut
// or unplugged cable and an unwired pin all read high. The task reads it as a level. A high sample engages at any time,
// and only the mushroom measured down, then twisted up and left up, closed and low, for the clear time releases.
const int k_estopRunLevel        = 0;
const uint32_t k_estopClearTime  = 5000;
const uint32_t k_estopUpdateRate = 5;   // 200 Hz
const uint32_t k_estopCheckCount = 13;  // 65 ms at 200 Hz
const uint16_t k_estopCheckMask  = 0xFFFF >> ((sizeof(uint16_t) * 8) - k_estopCheckCount);

// The clear counts low samples, not wall time, so a checker that stalls during the clear cannot skip it: the contact
// still has to read closed on this many samples in a row.
const uint32_t k_estopClearSamples = k_estopClearTime / k_estopUpdateRate;

// The checker counts as dead when it has not sampled for this long, so a stalled task cannot read as armed.
const uint32_t k_estopSampleStaleTime = 1000;

// Longest wait to post the state event. A full event queue must not stop the checker task from sampling (review H2).
const uint32_t k_estopEventPostWaitMs = k_estopUpdateRate;

static OpenShock::SimpleMutex s_estopMutex = {};
static gpio_num_t s_estopPin               = GPIO_NUM_NC;
static TaskHandle_t s_estopTask            = nullptr;

static EStopState s_lastPublishedState      = EStopState::Idle;
static int64_t s_estopActivatedAt           = 0;
static std::atomic<uint32_t> s_lastSampleMs = {0};

// Whether the running checker task has sampled since it started. `s_lastSampleMs` starts at 0 and a restarted task
// inherits the old task's value, so without this `IsArmed` could read true before the first sample (review H5).
static std::atomic<bool> s_sampledSinceStart = {false};

// The state last logged, so a post that fails and is retried on every pass logs the state line once.
static EStopState s_lastLoggedState = EStopState::Idle;
static bool s_postFailureLogged     = false;

static volatile bool s_externallyTriggered = false;

static bool s_estopInitialized = false;

// The latch and what set it. Only `estopmanager_writelatch` writes it, so RAM and the retained copy never disagree.
// Survives a warm restart. `Init` trusts it only after an allow-listed reset. It starts engaged, so a hub stays refused
// if anything reads it before `Init`.
static std::atomic<Softproxy::EStopLatchSource> s_latch = {Softproxy::EStopLatchSource::ColdBoot};
static RTC_NOINIT_ATTR Softproxy::RetainedEStop s_retainedLatch;
static portMUX_TYPE s_latchMux = portMUX_INITIALIZER_UNLOCKED;

// Gateway stops since boot, read and written only under `s_latchMux`. `Trigger` counts a stop before it engages, and the
// checker task records the count when it takes the stop, so a clear can tell that a stop came in that it has not taken.
static uint32_t s_gatewayStops = 0;

static void estopmanager_writelatch(Softproxy::EStopLatchSource source)
{
  s_latch.store(source);
  Softproxy::SealEStop(s_retainedLatch, source);
}

// Engages, keeping the source that already holds the latch.
static void estopmanager_engage(Softproxy::EStopLatchSource source)
{
  portENTER_CRITICAL(&s_latchMux);
  if (s_latch.load() == Softproxy::EStopLatchSource::Released) {
    estopmanager_writelatch(source);
  }
  portEXIT_CRITICAL(&s_latchMux);
}

// Records that the press which engaged the E-Stop was a full window of high samples. It changes only the latch that
// press set, so a press during a held latch restates nothing.
static void estopmanager_confirmpress()
{
  portENTER_CRITICAL(&s_latchMux);
  if (s_latch.load() == Softproxy::EStopLatchSource::Unconfirmed) {
    estopmanager_writelatch(Softproxy::EStopLatchSource::Button);
  }
  portEXIT_CRITICAL(&s_latchMux);
}

static void estopmanager_countgatewaystop()
{
  portENTER_CRITICAL(&s_latchMux);
  ++s_gatewayStops;
  portEXIT_CRITICAL(&s_latchMux);
}

static uint32_t estopmanager_gatewaystops()
{
  portENTER_CRITICAL(&s_latchMux);
  const uint32_t stops = s_gatewayStops;
  portEXIT_CRITICAL(&s_latchMux);
  return stops;
}

// Releases the latch unless a gateway stop came in that the checker task has not taken. `Trigger` counts the stop before
// it engages. A stop that races the clear therefore either finds the latch released and engages it, or moves the count
// and this refuses, so `IsEStopped` never reads false while a gateway stop is pending.
static bool estopmanager_release(uint32_t gatewayStopsTaken)
{
  portENTER_CRITICAL(&s_latchMux);
  const bool released = s_gatewayStops == gatewayStopsTaken;
  if (released) {
    estopmanager_writelatch(Softproxy::EStopLatchSource::Released);
  }
  portEXIT_CRITICAL(&s_latchMux);
  return released;
}

// Whether this boot is one that keeps the latch. A valid seal cannot show that: a brownout, a short supply dropout or
// an EN reset can keep RTC memory too, and a retained `Released` record would then bring the hub back released. Every
// reason off this list counts as a cold boot (SP-158).
static bool estopmanager_warmrestart()
{
  switch (esp_reset_reason()) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

static const char* estopmanager_statename(EStopState state)
{
  switch (state) {
    case EStopState::Idle:
      return "idle";
    case EStopState::Active:
      return "active";
    case EStopState::ActiveClearing:
      return "active-clearing";
    case EStopState::AwaitingRelease:
      return "awaiting-release";
    default:
      return "unknown";
  }
}

static void estopmanager_updateexternals(EStopState state)
{
  if (state == s_lastPublishedState) {
    return;  // No state change -> no event
  }

  if (state != s_lastLoggedState) {
    s_lastLoggedState = state;
    OS_LOGW(TAG, "softproxy-estop state=%s", estopmanager_statename(state));
  }

  // Post the current state as the event payload. The wait is finite, so a full event queue cannot hold the checker task
  // here while the mushroom goes unread (review H2). A failed post leaves the published state as it was, so the next
  // pass posts again. The latch never waits on this: it is written before the post.
  const esp_err_t err = esp_event_post(OPENSHOCK_EVENTS, OPENSHOCK_EVENT_ESTOP_STATE_CHANGED, &state, sizeof(state), pdMS_TO_TICKS(k_estopEventPostWaitMs));
  if (err != ESP_OK) {
    if (!s_postFailureLogged) {
      s_postFailureLogged = true;
      OS_LOGE(TAG, "Failed to post the EStop state, retrying on the next pass: %s", esp_err_to_name(err));
    }
    return;
  }
  s_postFailureLogged  = false;
  s_lastPublishedState = state;
}

// Samples the mushroom at a fixed rate and updates internal state and events.
static void estopmgr_checkertask(void* pvParameters)
{
  (void)pvParameters;

  // The latch outlives this task. Starting the task reads it and never writes it, so a restart of the task cannot
  // clear an engaged E-Stop. Only the clear below releases it.
  const bool startedEngaged = EStopManager::IsEStopped();

  // The gateway stops this task has taken. `Trigger` itself engages the latch for a stop counted before the task starts,
  // so the task has nothing to take for it.
  uint32_t gatewayStopsTaken = estopmanager_gatewaystops();

  // Bit history of samples, 1 is stop (open, high). It starts with no stop bit, so a released latch restored by a warm
  // restart stays released on a closed contact. Nothing in it is measured until this task has read a full window.
  uint16_t history = 0;

  EStopState state          = startedEngaged ? EStopState::Active : EStopState::Idle;
  uint32_t clearSamplesLeft = 0;

  // Only a full window of low samples read by this task shows the contact closed, so an engagement is a real one only
  // after one (SP-156). A cold boot, an unwired pin or `estop pin` onto an open pin reads high from the start. It
  // engages or stays engaged, but it proves no press and cannot reach `Button`.
  uint32_t samplesRead = 0;
  bool sawRun          = false;

  // Whether the press that engaged the E-Stop is still down, so a full window of high samples can confirm it.
  bool confirming = false;

  // Whether this task measured the mushroom down, a full window of high samples, while engaged and since the last
  // gateway stop. The clear starts only when the contact closes after that, so a cold boot, a warm restart or a gateway
  // stop with the mushroom already up stays engaged until it goes down and up again.
  bool sawDown = false;

  for (;;) {
    // Sleep for the update rate
    vTaskDelay(pdMS_TO_TICKS(k_estopUpdateRate));

    // Get current time
    int64_t now = OpenShock::millis();
    s_lastSampleMs.store(static_cast<uint32_t>(now));
    s_sampledSinceStart.store(true);

    // Handle external trigger: forcibly set the E-Stop active.
    if (s_externallyTriggered) {
      // Clear the flag before reading the count. `Trigger` counts first and sets the flag last, so a stop that comes
      // in between sets the flag again and the next pass takes it.
      s_externallyTriggered = false;
      gatewayStopsTaken     = estopmanager_gatewaystops();

      estopmanager_engage(Softproxy::EStopLatchSource::Gateway);
      state   = EStopState::Active;
      sawDown = false;  // Stops a clear in progress. The next clear needs the mushroom measured down again.

      estopmanager_updateexternals(state);

      // Do not modify history here; allow hardware to take over on subsequent iterations.
      continue;
    }

    // Sample the E-Stop input. Any level but the run level reads as stop.
    const uint16_t stopBit = gpio_get_level(s_estopPin) == k_estopRunLevel ? 0 : 1;
    history                = static_cast<uint16_t>((history << 1) | stopBit);
    if (samplesRead < k_estopCheckCount) {
      ++samplesRead;
    }

    // Debounce. Any high sample in the window reads as stop, so one high sample engages. A full window of high samples
    // is the mushroom measured down. A full window of low samples that this task read is the contact measured closed.
    const uint16_t window = history & k_estopCheckMask;
    bool stopSeen         = window != 0;
    bool solidStop        = window == k_estopCheckMask;
    bool solidRun         = window == 0 && samplesRead == k_estopCheckCount;
    sawRun                = sawRun || solidRun;

    if (!stopSeen) {
      confirming = false;
    }

    if (confirming && solidStop) {
      confirming = false;
      estopmanager_confirmpress();
    }

    switch (state) {
      case EStopState::Idle:
        // A high level at any time engages. There is no rearm window.
        if (stopSeen) {
          state              = EStopState::Active;
          s_estopActivatedAt = now;
          estopmanager_engage(Softproxy::EStopLatchSource::Unconfirmed);
          confirming = sawRun;
        }
        break;

      case EStopState::Active:
        // The mushroom measured down allows a clear. Twisting it up from there starts the clear once the contact reads
        // closed for a full window.
        if (solidStop) {
          sawDown = true;
        } else if (sawDown && solidRun) {
          state            = EStopState::ActiveClearing;
          clearSamplesLeft = k_estopClearSamples;
        }
        break;

      case EStopState::ActiveClearing:
        // Any high sample, a push, a bounce or a cable fault, stops the clear. It starts again from the next full window
        // of low samples. A gateway stop that has not reached this task yet refuses the release and stops the clear as
        // well, so the next clear needs the mushroom down and up again.
        if (stopSeen) {
          state = EStopState::Active;
        } else if (--clearSamplesLeft == 0) {
          state   = estopmanager_release(gatewayStopsTaken) ? EStopState::Idle : EStopState::Active;
          sawDown = false;
        }
        break;

      default:
        // `AwaitingRelease` belonged to the momentary hold-to-clear (SP-167), which SP-174 replaced. Nothing enters it.
        break;
    }

    estopmanager_updateexternals(state);
  }
}

static bool estopmgr_setestopenabled(bool enabled)
{
  if (enabled) {
    if (!OpenShock::IsValidInputPin(s_estopPin)) {
      OS_LOGE(TAG, "Refusing to start the EStop with no valid pin: %hhi", static_cast<int8_t>(s_estopPin));
      return false;
    }
    if (s_estopTask == nullptr) {
      s_sampledSinceStart.store(false);
      if (TaskUtils::TaskCreateUniversal(estopmgr_checkertask, TAG, 4096, nullptr, 5, &s_estopTask, 1) != pdPASS) {  // TODO: Profile stack size and set priority
        OS_LOGE(TAG, "Failed to create EStop event handler task");
        s_estopTask = nullptr;
        return false;
      }
    }
  } else {
    if (s_estopTask != nullptr) {
      vTaskDelete(s_estopTask);
      s_estopTask = nullptr;
    }
    s_sampledSinceStart.store(false);
  }

  return true;
}

// Whether the E-Stop settings are locked: they stay put while a press, an open contact or the gateway holds it engaged,
// or while any cap can let a command through.
static bool estopmgr_settingslocked()
{
  return Softproxy::EStopSettingsLocked(s_latch.load(), Softproxy::SafetyCaps::AnySet());
}

// Whether `pin` may be the E-Stop pin, against the stored RF TX pin. A failed read refuses.
static bool estopmgr_pinallowed(gpio_num_t pin)
{
  gpio_num_t txPin;
  if (!OpenShock::Config::GetRFConfigTxPin(txPin)) {
    OS_LOGE(TAG, "Failed to get RF TX pin from config");
    return false;
  }
  if (Softproxy::EStopPinConflicts(pin, txPin)) {
    OS_LOGE(TAG, "EStop pin %hhi conflicts with the RF TX pin, a board LED, a strapping or flash pin or has no pull-up", static_cast<int8_t>(pin));
    return false;
  }
  return true;
}

static bool estopmgr_set_pin_impl(gpio_num_t pin)
{
  esp_err_t err;

  if (s_estopPin == pin) {
    return true;
  }

  if (!OpenShock::IsValidInputPin(pin)) {
    OS_LOGE(TAG, "Invalid EStop pin: %hhi", static_cast<int8_t>(pin));
    return false;
  }

  bool wasRunning = s_estopTask != nullptr;
  if (wasRunning) {
    if (!estopmgr_setestopenabled(false)) {
      OS_LOGE(TAG, "Failed to disable EStop event handler task");
      return false;
    }
  }

  // Configure the new pin. The pull-up makes an open contact, a cut cable or an unwired pin read high, which is stop.
  gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << pin,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
  };

  err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to configure EStop pin");
    return false;
  }

  gpio_num_t oldPin = s_estopPin;

  // Set the new pin
  s_estopPin = pin;

  if (oldPin != GPIO_NUM_NC) {
    // Reset the old pin
    err = gpio_reset_pin(oldPin);
    if (err != ESP_OK) {
      OS_LOGE(TAG, "Failed to reset old EStop pin");
      return false;
    }
  }

  if (wasRunning) {
    if (!estopmgr_setestopenabled(true)) {
      OS_LOGE(TAG, "Failed to re-enable EStop event handler task");
      return false;
    }
  }

  return true;
}

bool EStopManager::Init()
{
  if (s_estopInitialized) {
    return true;
  }
  s_estopInitialized = true;

  // Restore the latch before anything can send a command. An allow-listed reset with a valid seal comes back as it was,
  // engaged or released, so a crash loop never forces a stop (SP-172). Anything else counts as a cold boot, which starts
  // engaged, so the hub refuses every command until the mushroom is pushed down and twisted up to clear it (SP-158,
  // SP-174).
  auto source = Softproxy::EStopLatchSource::ColdBoot;
  if (!estopmanager_warmrestart() || !Softproxy::RestoreEStop(s_retainedLatch, source)) {
    OS_LOGW(TAG, "Cold boot, the EStop starts engaged");
  } else if (source != Softproxy::EStopLatchSource::Released) {
    OS_LOGW(TAG, "Restored an engaged EStop after a warm restart, source=%s", Softproxy::EStopLatchName(source));
  }
  portENTER_CRITICAL(&s_latchMux);
  estopmanager_writelatch(source);
  portEXIT_CRITICAL(&s_latchMux);

  Config::EStopConfig cfg;
  if (!OpenShock::Config::GetEStop(cfg)) {
    OS_LOGE(TAG, "Failed to get EStop pin from config");
    return false;
  }

  OpenShock::ScopedLock lock__(&s_estopMutex);

  // A stored config can hold a bad pin. The E-Stop then stays off, which is not armed, so shock and vibrate refuse.
  // Returning false here would restart the hub in a loop and take the serial port with it.
  if (!estopmgr_pinallowed(cfg.gpioPin) || !estopmgr_set_pin_impl(cfg.gpioPin)) {
    OS_LOGE(TAG, "EStop stays off, shock and vibrate commands will refuse");
    return true;
  }

  if (cfg.enabled && !estopmgr_setestopenabled(true)) {
    OS_LOGE(TAG, "EStop stays off, shock and vibrate commands will refuse");
  }

  return true;
}

bool EStopManager::SetEStopEnabled(bool enabled)
{
  OpenShock::ScopedLock lock__(&s_estopMutex);

  if (!enabled && estopmgr_settingslocked()) {
    OS_LOGW(TAG, "Refusing to disable the EStop while a press or the gateway holds it engaged or a cap is set");
    return false;
  }

  if (enabled && s_estopPin == GPIO_NUM_NC) {
    gpio_num_t pin;
    if (!OpenShock::Config::GetEStopGpioPin(pin)) {
      OS_LOGE(TAG, "Failed to get EStop pin from config");
      return false;
    }
    if (!estopmgr_pinallowed(pin) || !estopmgr_set_pin_impl(pin)) {
      OS_LOGE(TAG, "Failed to set EStop pin");
      return false;
    }
  }

  return estopmgr_setestopenabled(enabled);
}

bool EStopManager::SetEStopPin(gpio_num_t pin)
{
  OpenShock::ScopedLock lock__(&s_estopMutex);

  if (pin == s_estopPin) {
    return true;
  }

  if (estopmgr_settingslocked()) {
    OS_LOGW(TAG, "Refusing to move the EStop pin while a press or the gateway holds it engaged or a cap is set");
    return false;
  }

  if (!estopmgr_pinallowed(pin)) {
    return false;
  }

  return estopmgr_set_pin_impl(pin);
}

bool EStopManager::IsEStopped()
{
  return s_latch.load() != Softproxy::EStopLatchSource::Released;
}

Softproxy::EStopLatchSource EStopManager::GetLatchSource()
{
  return s_latch.load();
}

bool EStopManager::IsArmed()
{
  if (s_estopTask == nullptr || !OpenShock::IsValidInputPin(s_estopPin)) {
    return false;
  }

  // A task that has not sampled since it started is not armed, whatever the boot value or an older task left in
  // `s_lastSampleMs` (review H5). Read before the sample: the task stores its sample, then the flag.
  if (!s_sampledSinceStart.load()) {
    return false;
  }

  // Read the sample first. In the other order a sample between the two reads would wrap the difference.
  const uint32_t lastSampleMs = s_lastSampleMs.load();
  return static_cast<uint32_t>(OpenShock::millis()) - lastSampleMs <= k_estopSampleStaleTime;
}

gpio_num_t EStopManager::GetRunningPin()
{
  return s_estopPin;
}

int64_t EStopManager::LastEStopped()
{
  return s_estopActivatedAt;
}

void EStopManager::Trigger()
{
  // Engage here, not in the checker task, because a hub with no pin has no task. The latch must hold from this call.
  // Count the stop first, so a clear that has not taken it refuses to release (see `estopmanager_release`).
  estopmanager_countgatewaystop();
  if (!IsEStopped()) {
    s_estopActivatedAt = OpenShock::millis();
  }
  estopmanager_engage(Softproxy::EStopLatchSource::Gateway);

  OpenShock::ScopedLock lock__(&s_estopMutex);

  if (s_estopTask != nullptr) {
    // The checker task moves its state to Active and publishes it.
    s_externallyTriggered = true;
    return;
  }

  estopmanager_updateexternals(EStopState::Active);
}
