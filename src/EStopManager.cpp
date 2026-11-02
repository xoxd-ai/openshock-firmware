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

const uint32_t k_estopHoldToClearTime = 5000;
const uint32_t k_estopUpdateRate      = 5;   // 200 Hz
const uint32_t k_estopCheckCount      = 13;  // 65 ms at 200 Hz
const uint16_t k_estopCheckMask       = 0xFFFF >> ((sizeof(uint16_t) * 8) - k_estopCheckCount);

// Grace period after deactivation (prevents immediate re-trigger on release bounce/EMI)
const uint32_t k_estopRearmGraceTime = 250;  // tune as needed

// The checker counts as dead when it has not sampled for this long, so a stalled task cannot read as armed.
const uint32_t k_estopSampleStaleTime = 1000;

static OpenShock::SimpleMutex s_estopMutex = {};
static gpio_num_t s_estopPin               = GPIO_NUM_NC;
static TaskHandle_t s_estopTask            = nullptr;

static EStopState s_lastPublishedState      = EStopState::Idle;
static int64_t s_estopActivatedAt           = 0;
static std::atomic<uint32_t> s_lastSampleMs = {0};

static volatile bool s_externallyTriggered = false;

static bool s_estopInitialized = false;

// The latch and what set it. Only `estopmanager_writelatch` writes it, so RAM and the retained copy never disagree.
// Survives a warm restart. `Init` trusts it only after an allow-listed reset. It starts engaged, so a hub stays refused
// if anything reads it before `Init`.
static std::atomic<Softproxy::EStopLatchSource> s_latch = {Softproxy::EStopLatchSource::ColdBoot};
static RTC_NOINIT_ATTR Softproxy::RetainedEStop s_retainedLatch;
static portMUX_TYPE s_latchMux = portMUX_INITIALIZER_UNLOCKED;

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

// Records that the press which engaged the E-Stop was a full window of low samples. It changes only the latch that
// press set, so a press during a held latch restates nothing.
static void estopmanager_confirmpress()
{
  portENTER_CRITICAL(&s_latchMux);
  if (s_latch.load() == Softproxy::EStopLatchSource::Unconfirmed) {
    estopmanager_writelatch(Softproxy::EStopLatchSource::Button);
  }
  portEXIT_CRITICAL(&s_latchMux);
}

static void estopmanager_release()
{
  portENTER_CRITICAL(&s_latchMux);
  estopmanager_writelatch(Softproxy::EStopLatchSource::Released);
  portEXIT_CRITICAL(&s_latchMux);
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

  s_lastPublishedState = state;

  OS_LOGW(TAG, "softproxy-estop state=%s", estopmanager_statename(state));

  // Post the current state as the event payload
  ESP_ERROR_CHECK(esp_event_post(OPENSHOCK_EVENTS, OPENSHOCK_EVENT_ESTOP_STATE_CHANGED, &state, sizeof(state), portMAX_DELAY));
}

// Samples the estop at a fixed rate and updates internal state + events
static void estopmgr_checkertask(void* pvParameters)
{
  (void)pvParameters;

  // The latch outlives this task. Starting the task reads it and never writes it, so a restart of the task cannot
  // clear an engaged E-Stop. Only the hold-to-clear below releases it.
  const bool startedEngaged = EStopManager::IsEStopped();

  uint16_t history = 0xFFFF;  // Bit history of samples, 0 is pressed

  EStopState state      = startedEngaged ? EStopState::Active : EStopState::Idle;
  int64_t deactivatesAt = 0;

  // Rearm grace state
  int64_t rearmAt   = 0;
  bool rearmBlocked = false;

  // Debounced button state: true == pressed, false == released
  // Started engaged, a button held across the restart is not a new press, so it cannot start the hold-to-clear.
  bool lastBtnState = startedEngaged;

  // `history` starts all high, which nothing measured. Only a full window of high samples read by this task shows a
  // released button, so a press is an edge only after one. A pin already low when the task starts, as after `estop pin`
  // onto a low pin, still engages, but it proves no press and cannot reach `Button` (SP-156).
  uint32_t samplesRead = 0;
  bool sawRelease      = false;

  // Whether the press now down began after a release while engaged, so the hold-to-clear may use it. A press that
  // engaged the E-Stop or that a gateway stop interrupted does not count.
  bool clearPress = false;

  // Whether the press that engaged the E-Stop is still down, so a full window of low samples can confirm it.
  bool confirming = false;

  for (;;) {
    // Sleep for the update rate
    vTaskDelay(pdMS_TO_TICKS(k_estopUpdateRate));

    // Get current time
    int64_t now = OpenShock::millis();
    s_lastSampleMs.store(static_cast<uint32_t>(now));

    // Handle external trigger: forcibly set the E-Stop active.
    if (s_externallyTriggered) {
      s_externallyTriggered = false;

      estopmanager_engage(Softproxy::EStopLatchSource::Gateway);
      state        = EStopState::Active;
      rearmBlocked = false;
      clearPress   = false;  // Stops a hold-to-clear in progress. The next one needs a new press.

      estopmanager_updateexternals(state);

      // Do not modify history/lastBtnState here; allow hardware to take over
      // on subsequent iterations.
      continue;
    }

    // Sample the EStop input
    history = static_cast<uint16_t>((history << 1) | gpio_get_level(s_estopPin));
    if (samplesRead < k_estopCheckCount) {
      ++samplesRead;
    }

    // Debounce:
    // If all recent bits are 1 -> fully released.
    // If any bit is 0 -> pressed (or bouncing toward pressed).
    // If all recent bits are 0 -> a solid press. One low sample is enough to engage, but only a solid press shows that
    // a hand is on the button, so only a solid press confirms an engagement or counts toward the hold-to-clear.
    bool btnState    = (history & k_estopCheckMask) != k_estopCheckMask;  // true == pressed
    bool btnSolid    = (history & k_estopCheckMask) == 0;
    sawRelease       = sawRelease || (!btnState && samplesRead == k_estopCheckCount);
    bool pressedEdge = (btnState && !lastBtnState && sawRelease);
    lastBtnState     = btnState;

    if (!btnState) {
      clearPress = false;
      confirming = false;
    } else if (pressedEdge) {
      clearPress = true;
    }

    if (confirming && btnSolid) {
      confirming = false;
      estopmanager_confirmpress();
    }

    switch (state) {
      case EStopState::Idle:
        // Rearm grace: after clearing, ignore presses for a short window.
        // After the window ends, require a released state before re-arming.
        if (rearmBlocked) {
          if (now < rearmAt) {
            // Still in grace window: ignore any press. Track input to avoid phantom edges later.
            break;
          }

          // Grace window ended: only re-arm once we see released.
          rearmBlocked = false;
        }

        if (btnState) {
          state              = EStopState::Active;
          s_estopActivatedAt = now;
          estopmanager_engage(Softproxy::EStopLatchSource::Unconfirmed);
          clearPress = false;
          confirming = sawRelease;
        }
        break;

      case EStopState::Active:
        // Once active, if a new press turns solid, start hold-to-clear timing.
        if (clearPress && btnSolid) {
          state         = EStopState::ActiveClearing;
          deactivatesAt = now + k_estopHoldToClearTime;
        }
        break;

      case EStopState::ActiveClearing:
        if (!btnSolid) {  // released or noisy before hold time -> go back to Active
          state = EStopState::Active;
        } else if (now >= deactivatesAt) {
          // Hold complete -> now wait for release edge to fully clear
          state = EStopState::AwaitingRelease;
        }
        break;

      case EStopState::AwaitingRelease:
        if (!btnState) {  // fully released -> clear E-Stop
          state = EStopState::Idle;
          estopmanager_release();

          // Start grace period to prevent immediate re-trigger.
          rearmBlocked = true;
          rearmAt      = now + k_estopRearmGraceTime;
        }
        break;

      default:
        // Should never happen
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
  }

  return true;
}

// Whether the E-Stop settings are locked: they stay put while a press or the gateway holds it engaged, or while any
// cap can let a command through.
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
    OS_LOGE(TAG, "EStop pin %hhi conflicts with the RF TX pin, a board LED or has no pull-up", static_cast<int8_t>(pin));
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

  // Configure the new pin
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

  // Restore the latch before anything can send a command. An allow-listed reset with a valid seal comes back as it was.
  // Anything else counts as a cold boot, which starts engaged, so the hub refuses every command until the hold-to-clear
  // (SP-158).
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
