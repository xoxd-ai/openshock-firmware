// Host test of the whole-config writers (TIN-7545). It links the real src/config/Config.cpp and
// src/config/RootConfig.cpp, so `SaveFromJSON` (serial `jsonconfig`), `SetRaw` (serial `rawconfig`) and
// `SaveFromFlatBuffer` run their real bodies, with the real `_killConfigChangeAllowed` gate and the real
// `KillConfigChangeAllowed` rule behind it. The RF and E-Stop parsers are the real RFConfig.cpp, EStopConfig.cpp and
// internal/utils.cpp, on the real Chipset.h. Fakes, in test/native/config_stubs/ and below: the LittleFS file, cJSON, the
// mutex, logging, the GPIO driver header, the E-Stop latch and running pin, `SafetyCaps::AnySet` and the other leaf
// configs, which read and write nothing.
//
// Run from the repository root, once per PiShock RF TX default (TX=12 for Pishock-2023, TX=15 for Pishock-Lite-2021):
//
//   FB=.pio/libdeps/Pishock-2023/flatbuffers/include
//   FLAGS="-std=c++20 -O1 -fsanitize=shift -fsanitize-undefined-trap-on-error -DOPENSHOCK_FW_CHIP_ESP32 -DOPENSHOCK_LED_GPIO=2 -DSOFTPROXY_PISHOCK_BUILD -DOPENSHOCK_RF_TX_GPIO=$TX"
//   SRC="test/native/test_config_writers.cpp src/config/Config.cpp src/config/RootConfig.cpp src/config/RFConfig.cpp"
//   SRC="$SRC src/config/EStopConfig.cpp src/config/internal/utils.cpp"
//   g++ $FLAGS -Itest/native/config_stubs -Iinclude -I$FB $SRC -o config_writers_test && ./config_writers_test

#include "config/Config.h"
#include "config/RootConfig.h"
#include "estop/EStopManager.h"
#include "softproxy/KillPath.h"
#include "softproxy/SafetyCaps.h"
#include "util/IPAddressUtils.h"

#include "serialization/_fbs/HubConfig_generated.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace OpenShock;
using Softproxy::EStopLatchSource;
namespace Fbs = OpenShock::Serialization::Configuration;

// The E-Stop and caps state the gate reads.
static EStopLatchSource s_latch = EStopLatchSource::ColdBoot;
static gpio_num_t s_runningPin  = GPIO_NUM_NC;
static bool s_anyCapSet         = false;

Softproxy::EStopLatchSource EStopManager::GetLatchSource()
{
  return s_latch;
}
bool EStopManager::IsEStopped()
{
  return s_latch != EStopLatchSource::Released;
}
gpio_num_t EStopManager::GetRunningPin()
{
  return s_runningPin;
}
bool Softproxy::SafetyCaps::AnySet()
{
  return s_anyCapSet;
}
bool Softproxy::SafetyCaps::Clear()
{
  s_anyCapSet = false;
  return true;
}

// `internal/utils.cpp` names this for the backend fields, and no leaf this test links parses one.
bool OpenShock::IPV4AddressFromStringView(IPAddress&, std::string_view)
{
  return false;
}

// The other leaf configs read and write nothing. RF and E-Stop are the real parsers.
namespace OpenShock::Config {
#define SOFTPROXY_EMPTY_LEAF(Type)                                                                \
  void Type::ToDefault()                                                                          \
  {                                                                                               \
  }                                                                                               \
  bool Type::FromFlatbuffers(const Fbs::Type*)                                                    \
  {                                                                                               \
    return true;                                                                                  \
  }                                                                                               \
  flatbuffers::Offset<Fbs::Type> Type::ToFlatbuffers(flatbuffers::FlatBufferBuilder&, bool) const \
  {                                                                                               \
    return {};                                                                                    \
  }                                                                                               \
  bool Type::FromJSON(const cJSON*)                                                               \
  {                                                                                               \
    return true;                                                                                  \
  }                                                                                               \
  cJSON* Type::ToJSON(bool) const                                                                 \
  {                                                                                               \
    return cJSON_CreateObject();                                                                  \
  }

  WiFiCredentials::WiFiCredentials()
    : id(0)
  {
  }
  WiFiCredentials::WiFiCredentials(uint8_t id, std::string_view ssid, std::string_view password)
    : id(id)
    , ssid(ssid)
    , password(password)
  {
  }
  SOFTPROXY_EMPTY_LEAF(WiFiCredentials)
  WiFiConfig::WiFiConfig()
  {
  }
  SOFTPROXY_EMPTY_LEAF(WiFiConfig)
  CaptivePortalConfig::CaptivePortalConfig()
    : alwaysEnabled(false)
  {
  }
  SOFTPROXY_EMPTY_LEAF(CaptivePortalConfig)
  BackendConfig::BackendConfig()
  {
  }
  SOFTPROXY_EMPTY_LEAF(BackendConfig)
  SerialInputConfig::SerialInputConfig()
    : echoEnabled(true)
  {
  }
  SOFTPROXY_EMPTY_LEAF(SerialInputConfig)
  OtaUpdateConfig::OtaUpdateConfig()
    : isEnabled(false)
    , cdnDomain()
    , updateChannel()
    , checkOnStartup(false)
    , checkPeriodically(false)
    , checkInterval(0)
    , allowBackendManagement(false)
    , requireManualApproval(false)
    , updateId(0)
    , updateStep()
  {
  }
  SOFTPROXY_EMPTY_LEAF(OtaUpdateConfig)
#undef SOFTPROXY_EMPTY_LEAF
}  // namespace OpenShock::Config

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      ++s_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

static const int kTx       = OPENSHOCK_RF_TX_GPIO;
static const int kStopPin  = 13;
static const int kOtherPin = 14;
static const int kLedPin   = Softproxy::kBoardLedPin;

// What a writer gets: the E-Stop and RF fields of a whole config.
struct Candidate {
  bool estopEnabled;
  int estopPin;
  int txPin;
  bool keepAlive;
};

static std::string asJson(const Candidate& c)
{
  char text[160];
  std::snprintf(text, sizeof(text), R"({"rf":{"txPin":%d,"keepAliveEnabled":%s},"estop":{"enabled":%s,"gpioPin":%d}})", c.txPin, c.keepAlive ? "true" : "false", c.estopEnabled ? "true" : "false", c.estopPin);
  return text;
}

static std::vector<uint8_t> asFlatBuffer(const Candidate& c)
{
  flatbuffers::FlatBufferBuilder builder;
  const auto rf    = Fbs::CreateRFConfig(builder, static_cast<int8_t>(c.txPin), c.keepAlive);
  const auto estop = Fbs::CreateEStopConfig(builder, c.estopEnabled, static_cast<int8_t>(c.estopPin));
  Fbs::FinishHubConfigBuffer(builder, Fbs::CreateHubConfig(builder, rf, 0, 0, 0, 0, 0, estop));
  return std::vector<uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
}

enum class Writer {
  Json,        // serial `jsonconfig`
  FlatBuffer,  // `SaveFromFlatBuffer`
  Raw,         // serial `rawconfig`
};

static const char* writerName(Writer writer)
{
  switch (writer) {
    case Writer::Json:
      return "SaveFromJSON";
    case Writer::FlatBuffer:
      return "SaveFromFlatBuffer";
    default:
      return "SetRaw";
  }
}

static bool write(Writer writer, const Candidate& c)
{
  const std::vector<uint8_t> buffer = asFlatBuffer(c);
  switch (writer) {
    case Writer::Json:
      return Config::SaveFromJSON(asJson(c));
    case Writer::FlatBuffer:
      return Config::SaveFromFlatBuffer(flatbuffers::GetRoot<Fbs::HubConfig>(buffer.data()));
    default:
      return Config::SetRaw(buffer.data(), buffer.size());
  }
}

// The stored config the writers start from: the E-Stop enabled on 13 and the RF transmitter on this env's default.
static std::vector<uint8_t> s_baseline;
static void seed(EStopLatchSource latch, bool anyCapSet, gpio_num_t runningPin)
{
  s_latch      = EStopLatchSource::ColdBoot;
  s_anyCapSet  = false;
  s_runningPin = GPIO_NUM_NC;
  CHECK(Config::SetEStop(Config::EStopConfig(true, static_cast<gpio_num_t>(kStopPin))));
  CHECK(Config::SetRFConfig(Config::RFConfig(static_cast<gpio_num_t>(kTx), false)));
  s_latch            = latch;
  s_anyCapSet        = anyCapSet;
  s_runningPin       = runningPin;
  s_baseline         = g_configFile;
  g_configFileWrites = 0;
}

static bool storedIs(int estopPin, bool estopEnabled, int txPin)
{
  Config::EStopConfig estop;
  Config::RFConfig rf;
  return Config::GetEStop(estop) && Config::GetRFConfig(rf) && estop.gpioPin == estopPin && estop.enabled == estopEnabled && rf.txPin == txPin;
}

// A refused write reaches neither the file nor the config in RAM.
static bool leftAlone()
{
  return g_configFileWrites == 0 && g_configFile == s_baseline && storedIs(kStopPin, true, kTx);
}

// An allowed write reaches the file. `SetRaw` stores the bytes for the next boot and leaves the config in RAM.
static bool stored(Writer writer, const Candidate& c)
{
  if (g_configFileWrites != 1) {
    return false;
  }
  if (writer == Writer::Raw) {
    return g_configFile == asFlatBuffer(c) && storedIs(kStopPin, true, kTx);
  }
  return storedIs(c.estopPin, c.estopEnabled, c.txPin);
}

static void writerAppliesTheKillRule(Writer writer)
{
  const int failuresBefore = s_failures;

  // Open: the cold boot latch, no cap set, no running pin. Moving the E-Stop pin is allowed, which K49 must not refuse.
  const Candidate moved {true, kOtherPin, kTx, false};
  seed(EStopLatchSource::ColdBoot, false, GPIO_NUM_NC);
  CHECK(write(writer, moved));
  CHECK(stored(writer, moved));

  // Locked by each latch that locks serial `estop`, and by a set cap with the latch released. A change of the E-Stop
  // pin or of its enable is refused. A write that keeps both passes.
  const Candidate disabled {false, kStopPin, kTx, false};
  const Candidate kept {true, kStopPin, kTx, true};
  struct Lock {
    EStopLatchSource latch;
    bool anyCapSet;
  };
  const std::vector<Lock> locks {
    {     EStopLatchSource::Button, false},
    {    EStopLatchSource::Gateway, false},
    {EStopLatchSource::Unconfirmed, false},
    {   EStopLatchSource::Released,  true},
  };
  for (const Lock& lock : locks) {
    seed(lock.latch, lock.anyCapSet, static_cast<gpio_num_t>(kStopPin));
    CHECK(!write(writer, moved));
    CHECK(leftAlone());
    CHECK(!write(writer, disabled));
    CHECK(leftAlone());
    CHECK(write(writer, kept));
    CHECK(stored(writer, kept));
  }

  // Open, but the pins must stay apart: the transmitter on the E-Stop pin, the transmitter on the pin the checker reads
  // now while the stored pin moves away, the transmitter on the board LED and the E-Stop on the board transmitter pin.
  const std::vector<Candidate> clashes {
    {true,  kStopPin, kStopPin, false},
    {true, kOtherPin, kStopPin, false},
    {true,  kStopPin,  kLedPin, false},
    {true,       kTx,      kTx, false},
  };
  for (const Candidate& refused : clashes) {
    seed(EStopLatchSource::Released, false, static_cast<gpio_num_t>(kStopPin));
    CHECK(!write(writer, refused));
    CHECK(leftAlone());
  }

  if (s_failures != failuresBefore) {
    std::printf("scenario failed: writerAppliesTheKillRule(%s)\n", writerName(writer));
  }
}

// The E-Stop parser's refusal of a negative pin. `GPIO_IS_VALID_GPIO` has no sign check, so the parser must stop every
// negative byte but -1 before the pin reaches it.
static void negativeEStopPinsAreRefused()
{
  const int failuresBefore = s_failures;

  for (int pin = -128; pin <= -2; ++pin) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Fbs::CreateEStopConfig(builder, true, static_cast<int8_t>(pin)));
    Config::EStopConfig parsed(true, static_cast<gpio_num_t>(kStopPin));
    CHECK(!parsed.FromFlatbuffers(flatbuffers::GetRoot<Fbs::EStopConfig>(builder.GetBufferPointer())));
    CHECK(parsed.enabled && parsed.gpioPin == kStopPin);

    const Candidate candidate {true, pin, kTx, false};
    for (Writer writer : {Writer::FlatBuffer, Writer::Raw}) {
      seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
      CHECK(!write(writer, candidate));
      CHECK(leftAlone());
    }
  }

  // -1 is the stored form of no pin, so it parses, and it can never read as enabled.
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Fbs::CreateEStopConfig(builder, true, -1));
  Config::EStopConfig parsed(true, static_cast<gpio_num_t>(kStopPin));
  CHECK(parsed.FromFlatbuffers(flatbuffers::GetRoot<Fbs::EStopConfig>(builder.GetBufferPointer())));
  CHECK(!parsed.enabled && parsed.gpioPin == GPIO_NUM_NC);

  const Candidate noPin {true, -1, kTx, false};
  for (Writer writer : {Writer::FlatBuffer, Writer::Raw}) {
    seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
    CHECK(write(writer, noPin));
    CHECK(g_configFileWrites == 1);
  }
  CHECK(Config::SaveFromFlatBuffer(flatbuffers::GetRoot<Fbs::HubConfig>(asFlatBuffer(noPin).data())));
  CHECK(storedIs(-1, false, kTx));

  // JSON reads the pin as an unsigned byte, so a negative number falls back to the build default and never reaches the
  // macro. The default is -1 on both PiShock envs. The same write is a pin change, so a lock refuses it.
  const Candidate negative {true, -2, kTx, false};
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  CHECK(write(Writer::Json, negative));
  CHECK(storedIs(OPENSHOCK_ESTOP_PIN, true, kTx));
  seed(EStopLatchSource::Button, false, static_cast<gpio_num_t>(kStopPin));
  CHECK(!write(Writer::Json, negative));
  CHECK(leftAlone());

  // A stored file that holds such a byte does not load, so boot writes the defaults over it: no E-Stop pin, not enabled.
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  g_configFile = asFlatBuffer({true, -2, kStopPin + 1, false});
  Config::Init();
  CHECK(g_configFileWrites == 1);
  CHECK(storedIs(OPENSHOCK_ESTOP_PIN, false, kTx));

  if (s_failures != failuresBefore) {
    std::printf("scenario failed: negativeEStopPinsAreRefused\n");
  }
}

// The real parsers differ from what the fakes they replaced did, and each difference here pins the real one.
static void realLeafParsersRun()
{
  const int failuresBefore = s_failures;

  auto parseEStop = [](bool enabled, int pin) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Fbs::CreateEStopConfig(builder, enabled, static_cast<int8_t>(pin)));
    Config::EStopConfig parsed;
    CHECK(parsed.FromFlatbuffers(flatbuffers::GetRoot<Fbs::EStopConfig>(builder.GetBufferPointer())));
    return parsed;
  };
  auto parseRf = [](int pin) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Fbs::CreateRFConfig(builder, static_cast<int8_t>(pin), false));
    Config::RFConfig parsed(static_cast<gpio_num_t>(kStopPin), true);
    CHECK(parsed.FromFlatbuffers(flatbuffers::GetRoot<Fbs::RFConfig>(builder.GetBufferPointer())));
    CHECK(!parsed.keepAliveEnabled);
    return parsed;
  };

  // A pin Chipset.h refuses keeps its number and reads as not enabled: past the last pin, a hole in the ESP32 pin mask
  // and a pin on the chip's unsafe list.
  for (int pin : {100, 30, 16}) {
    const Config::EStopConfig parsed = parseEStop(true, pin);
    CHECK(!parsed.enabled && parsed.gpioPin == pin);
  }
  const Config::EStopConfig usable = parseEStop(true, kStopPin);
  CHECK(usable.enabled && usable.gpioPin == kStopPin);
  CHECK(!parseEStop(false, kStopPin).enabled);

  // The RF parser reads the byte as unsigned, and a pin the mask refuses, or any negative one, falls back to the default.
  for (int pin : {-2, -1, -128, 30, 100}) {
    CHECK(parseRf(pin).txPin == kTx);
  }
  CHECK(parseRf(kOtherPin).txPin == kOtherPin);

  // JSON: a pin that fails falls back to the default, and an `enabled` of the wrong type refuses the whole write.
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  CHECK(Config::SaveFromJSON(R"({"rf":{"txPin":300,"keepAliveEnabled":false},"estop":{"enabled":false,"gpioPin":300}})"));
  CHECK(storedIs(OPENSHOCK_ESTOP_PIN, false, kTx));
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  CHECK(!Config::SaveFromJSON(R"({"rf":{"txPin":14},"estop":{"enabled":1,"gpioPin":13}})"));
  CHECK(leftAlone());

  // A missing `enabled` takes the validity of the pin.
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  CHECK(Config::SaveFromJSON(R"({"rf":{"txPin":14},"estop":{"gpioPin":13}})"));
  CHECK(storedIs(kStopPin, true, kOtherPin));

  if (s_failures != failuresBefore) {
    std::printf("scenario failed: realLeafParsersRun\n");
  }
}

static void malformedJsonWritesNothing()
{
  const int failuresBefore = s_failures;
  seed(EStopLatchSource::Released, false, GPIO_NUM_NC);
  CHECK(!Config::SaveFromJSON(R"({"estop":{"enabled":true,"gpioPin":)"));
  CHECK(leftAlone());
  if (s_failures != failuresBefore) {
    std::printf("scenario failed: malformedJsonWritesNothing\n");
  }
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // No file yet, so `Init` writes the defaults.
  Config::Init();
  CHECK(g_configFileWrites == 1);

  for (Writer writer : {Writer::Json, Writer::FlatBuffer, Writer::Raw}) {
    writerAppliesTheKillRule(writer);
  }
  malformedJsonWritesNothing();
  negativeEStopPinsAreRefused();
  realLeafParsersRun();

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return EXIT_FAILURE;
  }
  std::printf("config writers: all checks passed\n");
  return EXIT_SUCCESS;
}
