#pragma once

// Fakes for test/native/test_command_paths.cpp and test/native/test_kill_path.cpp. The firmware's own code compiles
// against these on a host: every header in this directory forwards here. Fakes in both tests: the NVS store, the
// mutexes, the `Config` store and the serial and socket transports. test_command_paths also fakes `RFTransmitter`
// and `EStopManager`, as firmware logic, not hardware. With SOFTPROXY_KILL_TEST set those two are real, and
// test_kill_path.cpp supplies FreeRTOS tasks and queues, GPIO, RMT and events.

#include "ShockerCommandType.h"
#include "ShockerModelType.h"
#include "softproxy/KillPath.h"
#include "span.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// `int64_t` is `long long` on the hub and `long` on a 64-bit host, which breaks one `std::clamp` call in
// src/CommandHandler.cpp. This overload takes that call on the host.
namespace std {
  inline int64_t clamp(int64_t value, long long low, const int64_t& high)
  {
    return std::min<int64_t>(std::max<int64_t>(value, low), high);
  }
}  // namespace std

// FreeRTOS and ESP-IDF names.
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void* QueueHandle_t;
typedef void* TaskHandle_t;
typedef int esp_err_t;
typedef const char* esp_event_base_t;
typedef uint32_t nvs_handle_t;
typedef void (*TaskFunction_t)(void*);
typedef unsigned int UBaseType_t;
typedef int portMUX_TYPE;

enum gpio_num_t : int8_t {
  GPIO_NUM_NC = -1,
};
enum eTaskState {
  eRunning,
  eDeleted,
};
enum nvs_open_mode_t {
  NVS_READONLY,
  NVS_READWRITE,
};

#define portMAX_DELAY         0xFFFFFFFFu
#define pdMS_TO_TICKS(ms)     static_cast<TickType_t>(ms)
#define pdTRUE                1
#define pdPASS                1
#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define RTC_NOINIT_ATTR
#define OPENSHOCK_GPIO_INVALID -1
#ifndef OPENSHOCK_RF_TX_GPIO
#define OPENSHOCK_RF_TX_GPIO 12
#endif
#define DISABLE_COPY(TypeName)              \
  TypeName(const TypeName&)       = delete; \
  void operator=(const TypeName&) = delete
#define DISABLE_MOVE(TypeName)         \
  TypeName(TypeName&&)       = delete; \
  void operator=(TypeName&&) = delete
#define portMUX_INITIALIZER_UNLOCKED 0
#ifndef SOFTPROXY_KILL_TEST
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux)  (void)(mux)
#else
// One thread is awake at a time, so a critical section excludes nothing on the host. test_kill_path.cpp hooks the edges
// of each outermost section instead: there another core's code can run, just before the section or just after it.
void hostEnterCritical(portMUX_TYPE* mux);
void hostExitCritical(portMUX_TYPE* mux);
#define portENTER_CRITICAL(mux) hostEnterCritical(mux)
#define portEXIT_CRITICAL(mux)  hostExitCritical(mux)
#endif

inline const char* esp_err_to_name(esp_err_t)
{
  return "fake";
}

#ifndef SOFTPROXY_KILL_TEST
inline QueueHandle_t xQueueCreate(int, std::size_t)
{
  return nullptr;
}
inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t)
{
  return 0;
}
inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t)
{
  return 0;
}
inline void vQueueDelete(QueueHandle_t)
{
}
inline void vTaskDelay(TickType_t)
{
}
inline void vTaskDelete(TaskHandle_t)
{
}
inline eTaskState eTaskGetState(TaskHandle_t)
{
  return eDeleted;
}
#else
QueueHandle_t xQueueCreate(int, std::size_t);
BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t);
BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t);
void vQueueDelete(QueueHandle_t);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
eTaskState eTaskGetState(TaskHandle_t);
inline int xPortGetCoreID()
{
  return 0;
}
#endif

// In-memory NVS: one blob, with injectable errors.
struct FakeNvs {
  bool present        = false;
  esp_err_t openError = ESP_OK;
  esp_err_t readError = ESP_OK;
  std::vector<uint8_t> blob;
};
extern FakeNvs g_nvs;

inline esp_err_t nvs_open(const char*, nvs_open_mode_t mode, nvs_handle_t* handle)
{
  *handle = 1;
  if (g_nvs.openError != ESP_OK) {
    return g_nvs.openError;
  }
  return mode == NVS_READONLY && !g_nvs.present ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
inline void nvs_close(nvs_handle_t)
{
}
inline esp_err_t nvs_commit(nvs_handle_t)
{
  return ESP_OK;
}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* out, std::size_t* size)
{
  if (g_nvs.readError != ESP_OK) {
    return g_nvs.readError;
  }
  if (!g_nvs.present) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  std::memcpy(out, g_nvs.blob.data(), std::min(*size, g_nvs.blob.size()));
  *size = g_nvs.blob.size();
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* in, std::size_t size)
{
  const uint8_t* bytes = static_cast<const uint8_t*>(in);
  g_nvs.blob.assign(bytes, bytes + size);
  g_nvs.present = true;
  return ESP_OK;
}
inline esp_err_t nvs_erase_key(nvs_handle_t, const char*)
{
  g_nvs.present = false;
  g_nvs.blob.clear();
  return ESP_OK;
}

// Logging keeps the last line, so a test can read what the firmware wrote.
extern std::string g_lastLog;
inline void hostLog(const char* format, ...)
{
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  g_lastLog = line;
}
#define OS_LOGV(TAG, format, ...) hostLog(format, ##__VA_ARGS__)
#define OS_LOGD(TAG, format, ...) hostLog(format, ##__VA_ARGS__)
#define OS_LOGI(TAG, format, ...) hostLog(format, ##__VA_ARGS__)
#define OS_LOGW(TAG, format, ...) hostLog(format, ##__VA_ARGS__)
#define OS_LOGE(TAG, format, ...) hostLog(format, ##__VA_ARGS__)
extern int g_panics;
#define OS_PANIC_INSTANT(TAG, format, ...) (hostLog(format, ##__VA_ARGS__), ++g_panics, std::abort())

extern std::string g_serialOut;
#define SERPR_SYS(format, ...)      hostSerial(format, ##__VA_ARGS__)
#define SERPR_RESPONSE(format, ...) SERPR_SYS("Response|" format, ##__VA_ARGS__)
#define SERPR_SUCCESS(format, ...)  SERPR_SYS("Success|" format, ##__VA_ARGS__)
#define SERPR_ERROR(format, ...)    SERPR_SYS("Error|" format, ##__VA_ARGS__)
inline void hostSerial(const char* format, ...)
{
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  g_serialOut += line;
  g_serialOut += '\n';
}

using namespace std::string_view_literals;

// Just enough cJSON for rftransmit.cpp. The parsed command comes from `g_serialCommand` below.
struct cJSON { };
inline cJSON* cJSON_ParseWithLength(const char*, std::size_t)
{
  static cJSON root;
  return &root;
}
inline void cJSON_Delete(cJSON*)
{
}
inline const char* cJSON_GetErrorPtr()
{
  return "";
}

extern "C" {
typedef union {
  struct {
    uint32_t duration0 : 15;
    uint32_t level0    : 1;
    uint32_t duration1 : 15;
    uint32_t level1    : 1;
  };
  uint32_t val;
} rmt_data_t;
}

namespace OpenShock {
  extern int64_t g_nowMs;
  inline int64_t millis()
  {
    return g_nowMs;
  }

  constexpr bool IsValidOutputPin(int pin)
  {
    return pin >= 0 && pin < 40;
  }
  constexpr bool IsValidInputPin(int pin)
  {
    return pin >= 0 && pin < 40;
  }

  // Mutexes always lock: the host test runs on one thread.
  class SimpleMutex { };
  class ScopedLock {
  public:
    ScopedLock(SimpleMutex*, TickType_t = portMAX_DELAY) { }
    bool isLocked() const { return true; }
  };
  class ReadWriteMutex { };
  class ScopedReadLock {
  public:
    ScopedReadLock(ReadWriteMutex*, TickType_t = portMAX_DELAY) { }
    bool unlock() { return true; }
  };
  class ScopedWriteLock {
  public:
    ScopedWriteLock(ReadWriteMutex*, TickType_t = portMAX_DELAY) { }
  };

#ifndef SOFTPROXY_KILL_TEST
  enum class EStopState : uint8_t {
    Idle,
    Active,
  };

  namespace EStopManager {
    bool IsEStopped();
    bool IsArmed();
    Softproxy::EStopLatchSource GetLatchSource();
    gpio_num_t GetRunningPin();
    void Trigger();
  }  // namespace EStopManager
#endif

  namespace Config {
    struct RFConfig {
      gpio_num_t txPin;
      bool keepAliveEnabled;
    };
    struct EStopConfig {
      bool enabled;
      gpio_num_t gpioPin;
    };
    bool GetRFConfig(RFConfig& out);
    bool GetRFConfigKeepAliveEnabled(bool& out);
    bool GetRFConfigTxPin(gpio_num_t& out);
    bool SetRFConfigTxPin(gpio_num_t txPin);
    bool SetRFConfigKeepAliveEnabled(bool enabled);
    bool GetEStop(EStopConfig& out);
    bool GetEStopEnabled(bool& out);
    bool SetEStopEnabled(bool enabled);
    bool GetEStopGpioPin(gpio_num_t& out);
    bool SetEStopGpioPin(gpio_num_t gpioPin);
  }  // namespace Config

#ifndef SOFTPROXY_KILL_TEST
  namespace TaskUtils {
    inline BaseType_t TaskCreateExpensive(TaskFunction_t, const char*, uint32_t, void*, int, TaskHandle_t*)
    {
      return pdPASS;
    }
  }  // namespace TaskUtils
#endif

  namespace Convert {
    inline bool ToGpioNum(std::string_view text, gpio_num_t& out)
    {
      if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
      }
      out = static_cast<gpio_num_t>(std::atoi(std::string(text).c_str()));
      return true;
    }
    template<typename T>
    inline bool ToUnsignedForTest(std::string_view text, T& out)
    {
      uint64_t value = 0;
      if (text.empty() || text.size() > 19) {
        return false;
      }
      for (char c : text) {
        if (c < '0' || c > '9') {
          return false;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
      }
      if (value > static_cast<uint64_t>(static_cast<T>(~T {0}))) {
        return false;
      }
      out = static_cast<T>(value);
      return true;
    }
    inline bool ToUint8(std::string_view text, uint8_t& out)
    {
      return ToUnsignedForTest(text, out);
    }
    inline bool ToUint16(std::string_view text, uint16_t& out)
    {
      return ToUnsignedForTest(text, out);
    }
    inline bool ToUint32(std::string_view text, uint32_t& out)
    {
      return ToUnsignedForTest(text, out);
    }
    inline bool ToBool(std::string_view text, bool& out)
    {
      if (text != "true"sv && text != "false"sv) {
        return false;
      }
      out = text == "true"sv;
      return true;
    }
  }  // namespace Convert

  /// One command that reached the fake radio.
  struct RadioSend {
    ShockerModelType model;
    uint16_t id;
    ShockerCommandType type;
    uint8_t intensity;
    uint16_t durationMs;
  };
  extern std::vector<RadioSend> g_radio;
  extern std::vector<gpio_num_t> g_transmitterPins;  // One entry per transmitter built, in order.

#ifndef SOFTPROXY_KILL_TEST
  class RFTransmitter {
  public:
    explicit RFTransmitter(gpio_num_t pin)
      : m_pin(pin)
    {
      g_transmitterPins.push_back(pin);
    }
    bool ok() const { return true; }
    gpio_num_t GetTxPin() const { return m_pin; }
    bool SendCommand(ShockerModelType model, uint16_t id, ShockerCommandType type, uint8_t intensity, uint16_t durationMs, bool = true)
    {
      g_radio.push_back(RadioSend {model, id, type, intensity, durationMs});
      return true;
    }

  private:
    gpio_num_t m_pin;
  };
#endif

  namespace CaptivePortal {
    void SetAlwaysEnabled(bool alwaysEnabled);
    bool SendMessageBIN(uint8_t socketId, tcb::span<const uint8_t> data);
  }  // namespace CaptivePortal

  namespace Serialization::Local {
    extern std::string g_lastError;
    template<typename Callback>
    bool SerializeErrorMessage(std::string_view message, Callback callback)
    {
      g_lastError                    = std::string(message);
      static const uint8_t kFrame[1] = {0};
      return callback(tcb::span<const uint8_t>(kFrame, 1));
    }
  }  // namespace Serialization::Local

  namespace Serialization::JsonSerial {
    struct ShockerCommand {
      ShockerModelType model;
      uint16_t id;
      ShockerCommandType command;
      uint8_t intensity;
      uint16_t durationMs;
    };
    extern ShockerCommand g_serialCommand;
    inline bool ParseShockerCommand(const cJSON*, ShockerCommand& out)
    {
      out = g_serialCommand;
      return true;
    }
  }  // namespace Serialization::JsonSerial

  namespace Serial {
    class CommandEntry {
    public:
      void addArgument(std::string_view, std::string_view, std::string_view, std::vector<std::string_view> = {}) { }
    };
    class CommandGroup {
    public:
      explicit CommandGroup(std::string_view) { }
      template<typename Fn>
      CommandEntry& addCommand(std::string_view, Fn)
      {
        m_entries.emplace_back();
        return m_entries.back();
      }
      template<typename Fn>
      CommandEntry& addCommand(std::string_view, std::string_view, Fn)
      {
        m_entries.emplace_back();
        return m_entries.back();
      }

    private:
      std::vector<CommandEntry> m_entries;
    };
    namespace CommandHandlers {
      CommandGroup RfTransmitHandler();
      CommandGroup RfTxPinHandler();
      CommandGroup EStopHandler();
      CommandGroup SafetyCapsHandler();
    }  // namespace CommandHandlers
  }  // namespace Serial
}  // namespace OpenShock

// The WebSocket event types src/WebSocketDeFragger.cpp handles.
enum WStype_t {
  WStype_ERROR,
  WStype_DISCONNECTED,
  WStype_CONNECTED,
  WStype_TEXT,
  WStype_BIN,
  WStype_FRAGMENT_TEXT_START,
  WStype_FRAGMENT_BIN_START,
  WStype_FRAGMENT,
  WStype_FRAGMENT_FIN,
  WStype_PING,
  WStype_PONG,
};

// esp_event and esp_system.
extern esp_event_base_t OPENSHOCK_EVENTS;
#define OPENSHOCK_EVENT_ESTOP_STATE_CHANGED 1
#ifndef SOFTPROXY_KILL_TEST
inline esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, void (*)(void*, esp_event_base_t, int32_t, void*), void*)
{
  return ESP_OK;
}
#else
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, void (*)(void*, esp_event_base_t, int32_t, void*), void*);
esp_err_t esp_event_post(esp_event_base_t, int32_t, const void*, std::size_t, TickType_t);
#define ESP_ERROR_CHECK(x) (void)(x)

// GPIO and RMT, which test_kill_path.cpp supplies.
enum gpio_mode_t {
  GPIO_MODE_INPUT,
};
enum gpio_pullup_t {
  GPIO_PULLUP_DISABLE = 0,
  GPIO_PULLUP_ENABLE  = 1,
};
enum gpio_pulldown_t {
  GPIO_PULLDOWN_DISABLE,
};
enum gpio_int_type_t {
  GPIO_INTR_DISABLE,
};
struct gpio_config_t {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
};
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_reset_pin(gpio_num_t);
int gpio_get_level(gpio_num_t);

struct rmt_obj_t;
#define RMT_TX_MODE        1
#define RMT_MEM_64         64
rmt_obj_t* rmtInit(int, int, int);
float rmtSetTick(rmt_obj_t*, float);
bool rmtDeinit(rmt_obj_t*);
bool rmtWriteBlocking(rmt_obj_t*, const rmt_data_t*, std::size_t);
#endif
extern int g_restarts;
inline void esp_restart()
{
  ++g_restarts;
}
enum esp_reset_reason_t {
  ESP_RST_UNKNOWN,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
};
inline esp_reset_reason_t g_resetReason = ESP_RST_POWERON;
inline esp_reset_reason_t esp_reset_reason()
{
  return g_resetReason;
}
