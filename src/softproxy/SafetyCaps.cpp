#include "softproxy/SafetyCaps.h"

const char* const TAG = "SafetyCaps";

#include "Core.h"
#include "estop/EStopManager.h"
#include "Logging.h"
#include "SimpleMutex.h"
#include "softproxy/KillPath.h"

#include <esp_attr.h>
#include <nvs.h>

#include <cstring>

using namespace OpenShock;
using namespace OpenShock::Softproxy;

namespace {
  const char* const kNvsNamespace = "spcaps";
  const char* const kNvsKey       = "caps";

  const uint32_t kRecordMagic   = 0x53504341;  // "SPCA"
  const uint16_t kRecordVersion = 1;

  struct StoredRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    ChannelCaps shock;
    ChannelCaps vibrate;
  };

  // Accept a stored channel only when it is cleared or fully set and coherent. Anything else loads as 0.
  ChannelCaps sanitize(const ChannelCaps& c)
  {
    if (CapsSet(c) && CapsCoherent(c)) {
      return c;
    }
    if (!CapsCleared(c)) {
      OS_LOGW(TAG, "Stored caps are incomplete or incoherent, loading them as 0");
    }
    return ChannelCaps {};
  }
}  // namespace

static SimpleMutex s_capsMutex = {};
static bool s_loaded           = false;
static ChannelCaps s_shock     = {};
static ChannelCaps s_vibrate   = {};
static ChannelLimiter s_shockLimiter;
static ChannelLimiter s_vibrateLimiter;
static RefusalReport s_refusals;

// A clear zeroed the caps in RAM, but its erase has not committed, so the stored record may still hold caps that load at
// the next boot. `AnySet` reads true meanwhile, so the E-Stop settings stay locked (review D4).
static bool s_eraseUncommitted = false;

// Survives a warm restart. Its content is garbage after a cold boot, which the checksum rejects.
static RTC_NOINIT_ATTR RetainedLimiters s_retained;
static bool s_restored       = false;
static int64_t s_clockBaseMs = 0;  // Hub clock at boot. 0 after a cold boot.

// Hub clock: the restored clock plus the time since boot. It never runs ahead of real time.
static int64_t hubNowMs()
{
  return s_clockBaseMs + OpenShock::millis();
}

static void ensureRestored()
{
  if (s_restored) {
    return;
  }
  s_restored = true;

  int64_t clockMs = 0;
  if (RestoreRetained(s_retained, clockMs, s_shockLimiter, s_vibrateLimiter)) {
    s_clockBaseMs = clockMs;
    OS_LOGI(TAG, "Restored the cap counters after a warm restart");
  } else {
    s_clockBaseMs = 0;
    OS_LOGI(TAG, "Cold boot, cap counters start empty and each channel holds before its first stage");
  }
}

static void sealRetained(int64_t now)
{
  SealRetained(s_retained, now, s_shockLimiter, s_vibrateLimiter);
}

static bool loadRecord()
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    OS_LOGI(TAG, "No stored caps, every shock and vibrate command refuses until the operator sets them");
    return true;
  }
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to open caps storage: %s", esp_err_to_name(err));
    return false;
  }

  StoredRecord record;
  size_t size = sizeof(record);
  err         = nvs_get_blob(handle, kNvsKey, &record, &size);
  nvs_close(handle);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    OS_LOGI(TAG, "No stored caps, every shock and vibrate command refuses until the operator sets them");
    return true;
  }
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to read stored caps: %s", esp_err_to_name(err));
    return false;
  }
  if (size != sizeof(record) || record.magic != kRecordMagic || record.version != kRecordVersion || record.size != sizeof(record)) {
    OS_LOGE(TAG, "Stored caps record has an unknown layout, loading every cap as 0");
    return false;
  }

  s_shock   = sanitize(record.shock);
  s_vibrate = sanitize(record.vibrate);
  return true;
}

static bool saveRecord()
{
  StoredRecord record;
  memset(&record, 0, sizeof(record));
  record.magic   = kRecordMagic;
  record.version = kRecordVersion;
  record.size    = sizeof(record);
  record.shock   = s_shock;
  record.vibrate = s_vibrate;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to open caps storage for writing: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_set_blob(handle, kNvsKey, &record, sizeof(record));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to store caps: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static bool ensureLoaded()
{
  if (s_loaded) {
    return true;
  }
  s_shock   = {};
  s_vibrate = {};
  if (!loadRecord()) {
    s_shock   = {};
    s_vibrate = {};
    return false;
  }
  s_loaded = true;
  return true;
}

bool SafetyCaps::Init()
{
  ScopedLock lock__(&s_capsMutex);
  ensureRestored();
  sealRetained(hubNowMs());
  return ensureLoaded();
}

void SafetyCaps::Get(ChannelCaps& shock, ChannelCaps& vibrate)
{
  ScopedLock lock__(&s_capsMutex);
  ensureLoaded();
  shock   = s_shock;
  vibrate = s_vibrate;
}

bool SafetyCaps::GetCounts(ShockerCommandType type, uint32_t& lastHour, uint32_t& lastDay)
{
  ScopedLock lock__(&s_capsMutex);
  ensureRestored();
  const int64_t now = hubNowMs();
  sealRetained(now);
  switch (type) {
    case ShockerCommandType::Shock:
      lastHour = s_shockLimiter.StagesLastHour(now);
      lastDay  = s_shockLimiter.StagesLastDay(now);
      return true;
    case ShockerCommandType::Vibrate:
      lastHour = s_vibrateLimiter.StagesLastHour(now);
      lastDay  = s_vibrateLimiter.StagesLastDay(now);
      return true;
    default:
      return false;
  }
}

SafetyCaps::SetResult SafetyCaps::Set(ShockerCommandType type, const ChannelCaps& caps)
{
  if (!CapsSet(caps) || !CapsCoherent(caps)) {
    return SetResult::Invalid;
  }
  if (type != ShockerCommandType::Shock && type != ShockerCommandType::Vibrate) {
    return SetResult::Invalid;
  }

  ScopedLock lock__(&s_capsMutex);
  if (!ensureLoaded()) {
    // An unreadable record must not be overwritten with one channel set and the other silently 0.
    OS_LOGE(TAG, "Caps storage is unreadable, run `safetycaps clear` before setting caps");
    return SetResult::Unreadable;
  }

  ChannelCaps& slot          = type == ShockerCommandType::Shock ? s_shock : s_vibrate;
  const ChannelCaps previous = slot;

  if (CapsRise(previous, caps) && !CapRiseAllowed(EStopManager::IsArmed(), EStopManager::GetLatchSource())) {
    OS_LOGW(TAG, "Refusing to raise a cap, the E-Stop must be armed and engaged by its button (SP-113, SP-156)");
    return SetResult::NeedsEngagedEStop;
  }

  slot = caps;
  if (!saveRecord()) {
    slot = previous;
    return SetResult::StoreFailed;
  }
  s_eraseUncommitted = false;  // The store now holds what RAM holds.

  OS_LOGI(TAG, "Stored %s caps", type == ShockerCommandType::Shock ? "shock" : "vibrate");
  return SetResult::Stored;
}

bool SafetyCaps::AnySet()
{
  ScopedLock lock__(&s_capsMutex);
  if (!lock__.isLocked() || s_eraseUncommitted || !ensureLoaded()) {
    return true;
  }
  return !CapsCleared(s_shock) || !CapsCleared(s_vibrate);
}

bool SafetyCaps::Clear()
{
  ScopedLock lock__(&s_capsMutex);

  // Refuse from now on, whether or not the erase below succeeds. Until it commits the settings lock still counts the
  // caps as set, because the stored record would load them again at the next boot.
  s_shock            = {};
  s_vibrate          = {};
  s_loaded           = true;
  s_eraseUncommitted = true;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to open caps storage for clearing: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_erase_key(handle, kNvsKey);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to erase stored caps: %s", esp_err_to_name(err));
    return false;
  }

  s_eraseUncommitted = false;
  OS_LOGI(TAG, "Cleared every cap, shock and vibrate commands refuse");
  return true;
}

// Checks a shock or vibrate command under the caps mutex. `admittedAtMs` receives the boot clock reading behind the hub
// clock the limiter counted from.
static Verdict admitCapped(ShockerCommandType type, uint8_t intensity, uint16_t durationMs, int64_t& admittedAtMs)
{
  ScopedLock lock__(&s_capsMutex);
  if (!lock__.isLocked()) {
    return Verdict::CapsNotSet;
  }
  if (!ensureLoaded()) {
    return Verdict::CapsNotSet;
  }

  ensureRestored();
  admittedAtMs      = OpenShock::millis();
  const int64_t now = s_clockBaseMs + admittedAtMs;  // hubNowMs() at the same reading.
  Verdict verdict   = type == ShockerCommandType::Shock ? s_shockLimiter.Admit(s_shock, intensity, durationMs, now) : s_vibrateLimiter.Admit(s_vibrate, intensity, durationMs, now);
  sealRetained(now);
  return verdict;
}

void SafetyCaps::RecordRefusal(ShockerCommandType type, Verdict verdict)
{
  // Count and log every refusal with no lock held, so the log never holds the cap lock.
  uint32_t seq = s_refusals.Record(verdict);
  if (seq != 0) {
    const char* channel = type == ShockerCommandType::Shock ? "shock" : type == ShockerCommandType::Vibrate ? "vibrate" : "other";
    char line[kRefusalLineSize];
    FormatRefusalLine(line, sizeof(line), seq, channel, verdict);
    OS_LOGW(TAG, "%s", line);
  }
}

Verdict SafetyCaps::Admit(ShockerCommandType type, uint8_t intensity, uint16_t durationMs, int64_t& admittedAtMs)
{
  admittedAtMs = OpenShock::millis();
  if (type != ShockerCommandType::Shock && type != ShockerCommandType::Vibrate) {
    return Verdict::NotCapped;
  }

  Verdict verdict = admitCapped(type, intensity, durationMs, admittedAtMs);
  RecordRefusal(type, verdict);
  return verdict;
}

RefusalSnapshot SafetyCaps::GetRefusals()
{
  return s_refusals.Snapshot();
}
