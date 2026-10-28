#include "softproxy/SafetyCaps.h"

const char* const TAG = "SafetyCaps";

#include "Core.h"
#include "Logging.h"
#include "SimpleMutex.h"

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
  const int64_t now = OpenShock::millis();
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

bool SafetyCaps::Set(ShockerCommandType type, const ChannelCaps& caps)
{
  if (!CapsSet(caps) || !CapsCoherent(caps)) {
    return false;
  }

  ScopedLock lock__(&s_capsMutex);
  if (!ensureLoaded()) {
    // An unreadable record must not be overwritten with one channel set and the other silently 0.
    OS_LOGE(TAG, "Caps storage is unreadable, run `safetycaps clear` before setting caps");
    return false;
  }

  ChannelCaps previous = {};
  switch (type) {
    case ShockerCommandType::Shock:
      previous = s_shock;
      s_shock  = caps;
      break;
    case ShockerCommandType::Vibrate:
      previous  = s_vibrate;
      s_vibrate = caps;
      break;
    default:
      return false;
  }

  if (!saveRecord()) {
    if (type == ShockerCommandType::Shock) {
      s_shock = previous;
    } else {
      s_vibrate = previous;
    }
    return false;
  }

  OS_LOGI(TAG, "Stored %s caps", type == ShockerCommandType::Shock ? "shock" : "vibrate");
  return true;
}

bool SafetyCaps::Clear()
{
  ScopedLock lock__(&s_capsMutex);

  // Refuse from now on, whether or not the erase below succeeds.
  s_shock   = {};
  s_vibrate = {};
  s_loaded  = true;

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

  OS_LOGI(TAG, "Cleared every cap, shock and vibrate commands refuse");
  return true;
}

Verdict SafetyCaps::Admit(ShockerCommandType type, uint8_t intensity, uint16_t durationMs)
{
  if (type != ShockerCommandType::Shock && type != ShockerCommandType::Vibrate) {
    return Verdict::NotCapped;
  }

  ScopedLock lock__(&s_capsMutex);
  if (!lock__.isLocked()) {
    return Verdict::CapsNotSet;
  }
  if (!ensureLoaded()) {
    return Verdict::CapsNotSet;
  }

  const int64_t now = OpenShock::millis();
  if (type == ShockerCommandType::Shock) {
    return s_shockLimiter.Admit(s_shock, intensity, durationMs, now);
  }
  return s_vibrateLimiter.Admit(s_vibrate, intensity, durationMs, now);
}
