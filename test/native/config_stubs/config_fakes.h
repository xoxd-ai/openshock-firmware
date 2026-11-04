#pragma once

// Fakes for test/native/test_config_writers.cpp, which links the real src/config/Config.cpp, RootConfig.cpp,
// RFConfig.cpp, EStopConfig.cpp and internal/utils.cpp. Every header in this directory forwards here. Fakes: the LittleFS
// file, the mutex, logging, the GPIO driver header and a small cJSON that parses objects of numbers and booleans. The
// config headers, Chipset.h, the generated HubConfig schema, flatbuffers, KillPath.h and the E-Stop and caps headers are
// the real ones.

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The real Chipset.h pulls in the real Common.h beside it, which needs these and raises two #warning lines on a host.
#pragma GCC diagnostic ignored "-Wcpp"
#define OPENSHOCK_API_DOMAIN    "api.invalid"
#define OPENSHOCK_FW_CDN_DOMAIN "cdn.invalid"
#define OPENSHOCK_FW_VERSION    "0.0.0"
#define OPENSHOCK_FW_USERAGENT  "config-writers-test"

typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu

// The ESP32 pin list and validity macros, adapted from the ESP-IDF headers that arduino-esp32 2.0.17 ships (Espressif
// Systems, Apache-2.0): hal/gpio_types.h, soc/soc_caps.h and driver/gpio.h. The enum keeps a fixed underlying type here.
// Neither macro checks the sign of its argument, and a negative shift count is undefined.
enum gpio_num_t : int {
  GPIO_NUM_NC = -1,
  GPIO_NUM_0 = 0,
  GPIO_NUM_1 = 1,
  GPIO_NUM_2 = 2,
  GPIO_NUM_3 = 3,
  GPIO_NUM_4 = 4,
  GPIO_NUM_5 = 5,
  GPIO_NUM_6 = 6,
  GPIO_NUM_7 = 7,
  GPIO_NUM_8 = 8,
  GPIO_NUM_9 = 9,
  GPIO_NUM_10 = 10,
  GPIO_NUM_11 = 11,
  GPIO_NUM_12 = 12,
  GPIO_NUM_13 = 13,
  GPIO_NUM_14 = 14,
  GPIO_NUM_15 = 15,
  GPIO_NUM_16 = 16,
  GPIO_NUM_17 = 17,
  GPIO_NUM_18 = 18,
  GPIO_NUM_19 = 19,
  GPIO_NUM_20 = 20,
  GPIO_NUM_21 = 21,
  GPIO_NUM_22 = 22,
  GPIO_NUM_23 = 23,
  GPIO_NUM_25 = 25,
  GPIO_NUM_26 = 26,
  GPIO_NUM_27 = 27,
  GPIO_NUM_28 = 28,
  GPIO_NUM_29 = 29,
  GPIO_NUM_30 = 30,
  GPIO_NUM_31 = 31,
  GPIO_NUM_32 = 32,
  GPIO_NUM_33 = 33,
  GPIO_NUM_34 = 34,
  GPIO_NUM_35 = 35,
  GPIO_NUM_36 = 36,
  GPIO_NUM_37 = 37,
  GPIO_NUM_38 = 38,
  GPIO_NUM_39 = 39,
  GPIO_NUM_MAX,
};
#define BIT24 0x01000000
#define BIT28 0x10000000
#define BIT29 0x20000000
#define BIT30 0x40000000
#define BIT31 0x80000000
#define BIT34 (1ULL << 34)
#define BIT35 (1ULL << 35)
#define BIT36 (1ULL << 36)
#define BIT37 (1ULL << 37)
#define BIT38 (1ULL << 38)
#define BIT39 (1ULL << 39)
#define SOC_GPIO_VALID_GPIO_MASK            (0xFFFFFFFFFFULL & ~(0ULL | BIT24 | BIT28 | BIT29 | BIT30 | BIT31))
#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK     (SOC_GPIO_VALID_GPIO_MASK & ~(0ULL | BIT34 | BIT35 | BIT36 | BIT37 | BIT38 | BIT39))
#define GPIO_IS_VALID_GPIO(gpio_num)        (((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0)
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) (((1ULL << (gpio_num)) & SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0)

#define DISABLE_DEFAULT(TypeName) TypeName() = delete
#define DISABLE_COPY(TypeName)                   \
  TypeName(const TypeName&)            = delete; \
  TypeName& operator=(const TypeName&) = delete
#define DISABLE_MOVE(TypeName)              \
  TypeName(TypeName&&)            = delete; \
  TypeName& operator=(TypeName&&) = delete
#define OPENSHOCK_GPIO_INVALID -1
#ifndef OPENSHOCK_RF_TX_GPIO
#define OPENSHOCK_RF_TX_GPIO 12
#endif
#ifndef OPENSHOCK_ESTOP_PIN
#define OPENSHOCK_ESTOP_PIN OPENSHOCK_GPIO_INVALID
#endif

// Logging keeps the last line. A panic aborts, so a test that reaches one fails.
inline std::string g_configLastLog;
inline void configHostLog(const char* format, ...)
{
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  g_configLastLog = line;
}
#define OS_LOGV(TAG, format, ...)          configHostLog(format, ##__VA_ARGS__)
#define OS_LOGD(TAG, format, ...)          configHostLog(format, ##__VA_ARGS__)
#define OS_LOGI(TAG, format, ...)          configHostLog(format, ##__VA_ARGS__)
#define OS_LOGW(TAG, format, ...)          configHostLog(format, ##__VA_ARGS__)
#define OS_LOGE(TAG, format, ...)          configHostLog(format, ##__VA_ARGS__)
#define OS_PANIC(TAG, format, ...)         (configHostLog(format, ##__VA_ARGS__), std::abort())
#define OS_PANIC_INSTANT(TAG, format, ...) (configHostLog(format, ##__VA_ARGS__), std::abort())

namespace OpenShock {
  // The lock always succeeds: the test runs on one thread.
  class ReadWriteMutex { };
  class ScopedReadLock {
  public:
    ScopedReadLock(ReadWriteMutex*, TickType_t = portMAX_DELAY) { }
    bool isLocked() const { return true; }
  };
  class ScopedWriteLock {
  public:
    ScopedWriteLock(ReadWriteMutex*, TickType_t = portMAX_DELAY) { }
    bool isLocked() const { return true; }
  };
}  // namespace OpenShock

// One LittleFS file, `/config`. `g_configFileWrites` counts the opens for writing, which is every write that reached
// the file.
inline bool g_configFileExists = false;
inline std::vector<uint8_t> g_configFile;
inline int g_configFileWrites = 0;

namespace fs {
  class File {
  public:
    File() = default;
    explicit File(bool forWrite)
      : m_open(true)
      , m_forWrite(forWrite)
    {
    }
    explicit operator bool() const { return m_open; }
    std::size_t size() const { return g_configFile.size(); }
    std::size_t read(uint8_t* out, std::size_t length)
    {
      const std::size_t count = std::min(length, g_configFile.size() - std::min(m_offset, g_configFile.size()));
      std::memcpy(out, g_configFile.data() + m_offset, count);
      m_offset += count;
      return count;
    }
    std::size_t write(const uint8_t* data, std::size_t length)
    {
      if (!m_forWrite) {
        return 0;
      }
      g_configFile.insert(g_configFile.end(), data, data + length);
      return length;
    }
    void close() { m_open = false; }

  private:
    bool m_open          = false;
    bool m_forWrite      = false;
    std::size_t m_offset = 0;
  };

  class LittleFSFS {
  public:
    bool begin(bool, const char*, uint8_t, const char*) { return true; }
    File open(const char*, const char* mode)
    {
      if (mode[0] == 'w') {
        g_configFileExists = true;
        g_configFile.clear();
        ++g_configFileWrites;
        return File(true);
      }
      return g_configFileExists ? File(false) : File();
    }
    bool remove(const char*)
    {
      g_configFileExists = false;
      g_configFile.clear();
      return true;
    }
    bool exists(const char*) { return g_configFileExists; }
  };
}  // namespace fs

// The Arduino FS.h brings `File` into the global namespace.
using fs::File;

// cJSON: objects whose values are numbers, booleans or objects. That covers the E-Stop and RF fields the test writes.
struct cJSON {
  enum class Kind {
    Object,
    Number,
    Bool,
  };
  Kind kind         = Kind::Object;
  int valueint      = 0;
  char* valuestring = nullptr;  // Always null: the fake parses no strings.
  bool boolean      = false;
  std::vector<std::pair<std::string, cJSON*>> children;
};

inline void cJSON_Delete(cJSON* item)
{
  if (item == nullptr) {
    return;
  }
  for (auto& child : item->children) {
    cJSON_Delete(child.second);
  }
  delete item;
}

namespace ConfigFakeJson {
  inline void skipSpace(const char*& at, const char* end)
  {
    while (at < end && (*at == ' ' || *at == '\n' || *at == '\t' || *at == '\r')) {
      ++at;
    }
  }
  inline bool take(const char*& at, const char* end, std::string_view token)
  {
    skipSpace(at, end);
    if (static_cast<std::size_t>(end - at) < token.size() || std::string_view(at, token.size()) != token) {
      return false;
    }
    at += token.size();
    return true;
  }
  inline cJSON* parseValue(const char*& at, const char* end);
  inline cJSON* parseObject(const char*& at, const char* end)
  {
    if (!take(at, end, "{")) {
      return nullptr;
    }
    auto* object = new cJSON;
    if (take(at, end, "}")) {
      return object;
    }
    do {
      skipSpace(at, end);
      if (at >= end || *at != '"') {
        cJSON_Delete(object);
        return nullptr;
      }
      const char* nameEnd = static_cast<const char*>(std::memchr(at + 1, '"', static_cast<std::size_t>(end - at - 1)));
      if (nameEnd == nullptr) {
        cJSON_Delete(object);
        return nullptr;
      }
      std::string name(at + 1, nameEnd);
      at           = nameEnd + 1;
      cJSON* value = nullptr;
      if (!take(at, end, ":") || (value = parseValue(at, end)) == nullptr) {
        cJSON_Delete(object);
        return nullptr;
      }
      object->children.emplace_back(std::move(name), value);
    } while (take(at, end, ","));
    if (!take(at, end, "}")) {
      cJSON_Delete(object);
      return nullptr;
    }
    return object;
  }
  inline cJSON* parseValue(const char*& at, const char* end)
  {
    skipSpace(at, end);
    if (at < end && *at == '{') {
      return parseObject(at, end);
    }
    for (bool value : {true, false}) {
      if (take(at, end, value ? "true" : "false")) {
        auto* item    = new cJSON;
        item->kind    = cJSON::Kind::Bool;
        item->boolean = value;
        return item;
      }
    }
    const std::string rest(at, end);
    char* parsedEnd     = nullptr;
    const double number = std::strtod(rest.c_str(), &parsedEnd);
    if (parsedEnd == rest.c_str()) {
      return nullptr;
    }
    at += parsedEnd - rest.c_str();
    auto* item   = new cJSON;
    item->kind     = cJSON::Kind::Number;
    item->valueint = static_cast<int>(number);
    return item;
  }
}  // namespace ConfigFakeJson

inline cJSON* cJSON_ParseWithLength(const char* text, std::size_t length)
{
  const char* at  = text;
  const char* end = text + length;
  cJSON* root     = ConfigFakeJson::parseObject(at, end);
  ConfigFakeJson::skipSpace(at, end);
  if (root != nullptr && at != end) {
    cJSON_Delete(root);
    return nullptr;
  }
  return root;
}
// The fake parses no arrays, so this walks nothing. Only the leaf configs this test fakes read one.
#define cJSON_ArrayForEach(element, array) for (element = nullptr; element != nullptr;)
inline const char* cJSON_GetErrorPtr()
{
  return "";
}
inline int cJSON_IsObject(const cJSON* item)
{
  return item != nullptr && item->kind == cJSON::Kind::Object ? 1 : 0;
}
inline int cJSON_IsNumber(const cJSON* item)
{
  return item != nullptr && item->kind == cJSON::Kind::Number ? 1 : 0;
}
inline int cJSON_IsBool(const cJSON* item)
{
  return item != nullptr && item->kind == cJSON::Kind::Bool ? 1 : 0;
}
inline int cJSON_IsTrue(const cJSON* item)
{
  return cJSON_IsBool(item) != 0 && item->boolean ? 1 : 0;
}
inline int cJSON_IsString(const cJSON*)
{
  return 0;
}
inline cJSON* cJSON_GetObjectItemCaseSensitive(const cJSON* object, const char* name)
{
  if (object == nullptr) {
    return nullptr;
  }
  for (const auto& child : object->children) {
    if (child.first == name) {
      return child.second;
    }
  }
  return nullptr;
}
inline cJSON* cJSON_CreateObject()
{
  return new cJSON;
}
inline int cJSON_AddItemToObject(cJSON* object, const char* name, cJSON* item)
{
  if (object == nullptr || item == nullptr) {
    return 0;
  }
  object->children.emplace_back(name, item);
  return 1;
}
inline cJSON* cJSON_AddBoolToObject(cJSON* object, const char* name, bool value)
{
  auto* item    = new cJSON;
  item->kind    = cJSON::Kind::Bool;
  item->boolean = value;
  cJSON_AddItemToObject(object, name, item);
  return item;
}
inline cJSON* cJSON_AddNumberToObject(cJSON* object, const char* name, double value)
{
  auto* item     = new cJSON;
  item->kind     = cJSON::Kind::Number;
  item->valueint = static_cast<int>(value);
  cJSON_AddItemToObject(object, name, item);
  return item;
}
inline int cJSON_AddItemToArray(cJSON* array, cJSON* item)
{
  return cJSON_AddItemToObject(array, "", item);
}
inline char* cJSON_PrintUnformatted(const cJSON*)
{
  char* text = static_cast<char*>(std::malloc(3));
  std::memcpy(text, "{}", 3);
  return text;
}
