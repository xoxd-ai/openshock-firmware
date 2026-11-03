#pragma once

// Fakes for test/native/test_config_writers.cpp, which links the real src/config/Config.cpp and
// src/config/RootConfig.cpp. Every header in this directory forwards here. Fakes: the LittleFS file, the mutex, logging,
// `Chipset` (pins 0 to 39) and a small cJSON that parses objects of numbers and booleans. The config headers, the
// generated HubConfig schema, flatbuffers, KillPath.h and the E-Stop and caps headers are the real ones.

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

typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu

enum gpio_num_t : int {
  GPIO_NUM_NC = -1,
};

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
  constexpr bool IsValidInputPin(int pin)
  {
    return pin >= 0 && pin < 40;
  }
  constexpr bool IsValidOutputPin(int pin)
  {
    return pin >= 0 && pin < 40;
  }

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
  Kind kind     = Kind::Object;
  double number = 0;
  bool boolean  = false;
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
    item->kind   = cJSON::Kind::Number;
    item->number = number;
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
inline const char* cJSON_GetErrorPtr()
{
  return "";
}
inline int cJSON_IsObject(const cJSON* item)
{
  return item != nullptr && item->kind == cJSON::Kind::Object ? 1 : 0;
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
