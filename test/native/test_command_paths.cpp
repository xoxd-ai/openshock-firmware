// Host test of the real command paths: gateway, port 81 socket, serial and radio encoders. It links the firmware's
// own dispatchers, `HandleCommand`, `SafetyCaps` and the encoders. test/native/stubs fakes the hardware edge and also
// three pieces of firmware logic: `EStopManager` (one boolean), `Config` and `CaptivePortal`, plus `RFTransmitter`.
// So nothing here runs the E-Stop or the radio task. test/native/test_kill_path.cpp runs both for real.
// `softproxy-host-test.yml` runs it. Run it from the repository root after `pio pkg install -e Pishock-2023`:
//
//   FB=.pio/libdeps/Pishock-2023/flatbuffers/include
//   WS=src/message_handlers/websocket
//   SRC="src/CommandHandler.cpp src/softproxy/SafetyCaps.cpp $WS/Local.cpp $WS/Gateway.cpp"
//   SRC="$SRC $WS/gateway/ShockerCommandList.cpp $WS/gateway/Trigger.cpp $WS/local/SetRfTxPinCommand.cpp"
//   SRC="$SRC src/serial/command_handlers/rftransmit.cpp src/serial/command_handlers/rftxpin.cpp src/radio/rmt/*Encoder.cpp"
//   g++ -std=c++20 -O1 -Itest/native/stubs -Iinclude -I"$FB" test/native/test_command_paths.cpp $SRC -o command_paths_test
//   ./command_paths_test
//
// Every cap below is a synthetic test value chosen for the arithmetic. None is a body-safety value.

#include "CommandHandler.h"
#include "host_fakes.h"
#include "message_handlers/impl/WSGateway.h"
#include "message_handlers/impl/WSLocal.h"
#include "message_handlers/WebSocket.h"
#include "radio/rmt/CaiXianlinEncoder.h"
#include "radio/rmt/D80Encoder.h"
#include "radio/rmt/Petrainer998DREncoder.h"
#include "radio/rmt/PetrainerEncoder.h"
#include "radio/rmt/T330Encoder.h"
#include "softproxy/SafetyCaps.h"

#include "serialization/_fbs/LocalToHubMessage_generated.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace OpenShock;
using namespace OpenShock::Softproxy;

namespace GatewaySchema = OpenShock::Serialization::Gateway;
namespace LocalSchema   = OpenShock::Serialization::Local;
namespace TypesSchema   = OpenShock::Serialization::Types;

// Fake state declared in host_fakes.h.
FakeNvs g_nvs;
std::string g_lastLog;
std::string g_serialOut;
esp_event_base_t OPENSHOCK_EVENTS = "fake";
int g_restarts                    = 0;
namespace OpenShock {
  int64_t g_nowMs = 0;
  std::vector<RadioSend> g_radio;
  std::vector<gpio_num_t> g_transmitterPins;
  namespace Serialization::Local {
    std::string g_lastError;
  }
  namespace Serialization::JsonSerial {
    ShockerCommand g_serialCommand;
  }
}  // namespace OpenShock

int g_panics                    = 0;
static bool s_estopped          = false;
static int s_estopTriggers      = 0;
static gpio_num_t s_configTxPin = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
static int s_configTxPinWrites  = 0;
static std::vector<bool> s_alwaysOpen;  // Each `SetAlwaysEnabled` argument, in order.
static int s_socketFrames = 0;
static int s_localHandlerRuns;

bool EStopManager::IsEStopped()
{
  return s_estopped;
}
bool EStopManager::IsArmed()
{
  return true;
}
Softproxy::EStopLatchSource EStopManager::GetLatchSource()
{
  return s_estopped ? Softproxy::EStopLatchSource::Button : Softproxy::EStopLatchSource::Released;
}
gpio_num_t EStopManager::GetRunningPin()
{
  return GPIO_NUM_NC;
}
void EStopManager::Trigger()
{
  ++s_estopTriggers;
}
bool Config::GetRFConfig(Config::RFConfig& out)
{
  out = {.txPin = s_configTxPin, .keepAliveEnabled = false};
  return true;
}
bool Config::GetRFConfigTxPin(gpio_num_t& out)
{
  out = s_configTxPin;
  return true;
}
bool Config::SetRFConfigTxPin(gpio_num_t txPin)
{
  s_configTxPin = txPin;
  ++s_configTxPinWrites;
  return true;
}
bool Config::GetRFConfigKeepAliveEnabled(bool& out)
{
  out = false;
  return true;
}
bool Config::SetRFConfigKeepAliveEnabled(bool)
{
  return true;
}
bool Config::GetEStop(Config::EStopConfig& out)
{
  out = {.enabled = false, .gpioPin = GPIO_NUM_NC};
  return true;
}
bool Config::GetEStopGpioPin(gpio_num_t& out)
{
  out = GPIO_NUM_NC;
  return true;
}
void CaptivePortal::SetAlwaysEnabled(bool alwaysEnabled)
{
  s_alwaysOpen.push_back(alwaysEnabled);
}
bool CaptivePortal::SendMessageBIN(uint8_t, tcb::span<const uint8_t>)
{
  ++s_socketFrames;
  return true;
}

// Every local handler but the real `HandleSetRfTxPinCommand`, which local/SetRfTxPinCommand.cpp supplies. A run of
// any of them on a refused command is a failure.
namespace LocalHandlers = OpenShock::MessageHandlers::Local::_Private;
#define FAKE_LOCAL_HANDLER(NAME)                                                   \
  void LocalHandlers::Handle##NAME(uint8_t, const LocalSchema::LocalToHubMessage*) \
  {                                                                                \
    ++s_localHandlerRuns;                                                          \
  }
FAKE_LOCAL_HANDLER(WifiScanCommand)
FAKE_LOCAL_HANDLER(WifiNetworkSaveCommand)
FAKE_LOCAL_HANDLER(WifiNetworkForgetCommand)
FAKE_LOCAL_HANDLER(WifiNetworkConnectCommand)
FAKE_LOCAL_HANDLER(WifiNetworkDisconnectCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetIsEnabledCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetDomainCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetUpdateChannelCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetCheckIntervalCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetAllowBackendManagementCommand)
FAKE_LOCAL_HANDLER(OtaUpdateSetRequireManualApprovalCommand)
FAKE_LOCAL_HANDLER(OtaUpdateHandleUpdateRequestCommand)
FAKE_LOCAL_HANDLER(OtaUpdateCheckForUpdatesCommand)
FAKE_LOCAL_HANDLER(OtaUpdateStartUpdateCommand)
FAKE_LOCAL_HANDLER(AccountLinkCommand)
FAKE_LOCAL_HANDLER(AccountUnlinkCommand)
FAKE_LOCAL_HANDLER(SetEstopEnabledCommand)
FAKE_LOCAL_HANDLER(SetEstopPinCommand)
FAKE_LOCAL_HANDLER(InvalidMessage)

namespace GatewayHandlers = OpenShock::MessageHandlers::Server::_Private;
void GatewayHandlers::HandlePing(const GatewaySchema::GatewayToHubMessage*)
{
}
void GatewayHandlers::HandleOtaUpdateRequest(const GatewaySchema::GatewayToHubMessage*)
{
}
void GatewayHandlers::HandleInvalidMessage(const GatewaySchema::GatewayToHubMessage*)
{
}

void _handleRFTransmitCommand(std::string_view arg, bool isAutomated);
void _handleRfTxPinCommand(std::string_view arg, bool isAutomated);

static int s_failures = 0;

// Prints the first few failures only, since a broken gate fails once per command in the sweeps below.
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      if (s_failures < 20) {                                        \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      }                                                             \
      ++s_failures;                                                 \
    }                                                               \
  } while (0)

static const int64_t kDayMs = 86'400'000;

// A hub clock past every cold boot hold the fixtures produce.
static const int64_t kWarmMs = 2 * kDayMs;

// Synthetic caps: 10 percent, 100 ms, 1 pulse, 1 s gap, 3 per hour, 5 per day.
static ChannelCaps fixture()
{
  return ChannelCaps {.maxIntensityPct = 10, .maxPulseMs = 100, .maxPulsesPerStage = 1, .minGapSec = 1, .maxPerHour = 3, .maxPerDay = 5, .stageJoinMs = 0};
}

// The layout of `StoredRecord` in src/softproxy/SafetyCaps.cpp, so a test can plant a bad record.
struct PlantedRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  ChannelCaps shock;
  ChannelCaps vibrate;
};

static void plantRecord(const ChannelCaps& shock, const ChannelCaps& vibrate, uint32_t magic = 0x53504341, uint16_t version = 1)
{
  PlantedRecord record;
  std::memset(&record, 0, sizeof(record));
  record.magic   = magic;
  record.version = version;
  record.size    = sizeof(record);
  record.shock   = shock;
  record.vibrate = vibrate;

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  g_nvs.blob.assign(bytes, bytes + sizeof(record));
  g_nvs.present = true;
}

static TypesSchema::ShockerModelType toFbs(ShockerModelType model)
{
  switch (model) {
    case ShockerModelType::CaiXianlin:
      return TypesSchema::ShockerModelType::CaiXianlin;
    case ShockerModelType::Petrainer:
      return TypesSchema::ShockerModelType::Petrainer;
    case ShockerModelType::Petrainer998DR:
      return TypesSchema::ShockerModelType::Petrainer998DR;
    default:
      return TypesSchema::ShockerModelType::WellturnT330;
  }
}

static TypesSchema::ShockerCommandType toFbs(ShockerCommandType type)
{
  switch (type) {
    case ShockerCommandType::Shock:
      return TypesSchema::ShockerCommandType::Shock;
    case ShockerCommandType::Vibrate:
      return TypesSchema::ShockerCommandType::Vibrate;
    case ShockerCommandType::Sound:
      return TypesSchema::ShockerCommandType::Sound;
    default:
      return TypesSchema::ShockerCommandType::Stop;
  }
}

// One verified gateway frame, through the real `HandleGatewayBinary`.
static void sendGateway(ShockerModelType model, uint16_t id, ShockerCommandType type, uint8_t intensity, uint16_t durationMs)
{
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<GatewaySchema::ShockerCommand>> commands;
  commands.push_back(GatewaySchema::CreateShockerCommand(builder, toFbs(model), id, toFbs(type), intensity, durationMs));
  auto list = GatewaySchema::CreateShockerCommandList(builder, builder.CreateVector(commands));
  auto root = GatewaySchema::CreateGatewayToHubMessage(builder, GatewaySchema::GatewayToHubMessagePayload::ShockerCommandList, list.Union());
  builder.Finish(root);
  MessageHandlers::WebSocket::HandleGatewayBinary(tcb::span<const uint8_t>(builder.GetBufferPointer(), builder.GetSize()));
}

static void sendTrigger(GatewaySchema::TriggerType type)
{
  flatbuffers::FlatBufferBuilder builder;
  auto trigger = GatewaySchema::CreateTrigger(builder, type);
  auto root    = GatewaySchema::CreateGatewayToHubMessage(builder, GatewaySchema::GatewayToHubMessagePayload::Trigger, trigger.Union());
  builder.Finish(root);
  MessageHandlers::WebSocket::HandleGatewayBinary(tcb::span<const uint8_t>(builder.GetBufferPointer(), builder.GetSize()));
}

// One verified local frame with an empty table of this payload type, through the real `HandleLocalBinary`.
static void sendLocalEmpty(LocalSchema::LocalToHubMessagePayload payload)
{
  flatbuffers::FlatBufferBuilder builder;
  flatbuffers::Offset<void> table = 0;
  if (payload != LocalSchema::LocalToHubMessagePayload::NONE) {
    table = flatbuffers::Offset<void>(builder.EndTable(builder.StartTable()));
  }
  auto root = LocalSchema::CreateLocalToHubMessage(builder, payload, table);
  builder.Finish(root);
  MessageHandlers::WebSocket::HandleLocalBinary(1, tcb::span<const uint8_t>(builder.GetBufferPointer(), builder.GetSize()));
}

static void sendLocalSetRfTxPin(int8_t pin)
{
  flatbuffers::FlatBufferBuilder builder;
  auto command = LocalSchema::CreateSetRfTxPinCommand(builder, pin);
  auto root    = LocalSchema::CreateLocalToHubMessage(builder, LocalSchema::LocalToHubMessagePayload::SetRfTxPinCommand, command.Union());
  builder.Finish(root);
  MessageHandlers::WebSocket::HandleLocalBinary(1, tcb::span<const uint8_t>(builder.GetBufferPointer(), builder.GetSize()));
}

static void sendSerial(ShockerModelType model, uint16_t id, ShockerCommandType type, uint8_t intensity, uint16_t durationMs)
{
  Serialization::JsonSerial::g_serialCommand = {.model = model, .id = id, .command = type, .intensity = intensity, .durationMs = durationMs};
  _handleRFTransmitCommand("{}"sv, false);
}

static void bootHub()
{
  CHECK(CommandHandler::Init());
}

static const ShockerCommandType kCapped[]      = {ShockerCommandType::Shock, ShockerCommandType::Vibrate};
static const uint16_t kDurations[]             = {0, 1, 2, 99, 100, 101, 300, 1000, 30'000, 65'535};
static const uint16_t kIds[]                   = {0, 1, 12'345, 65'535};
static const ShockerModelType kGatewayModels[] = {ShockerModelType::CaiXianlin, ShockerModelType::Petrainer, ShockerModelType::Petrainer998DR, ShockerModelType::WellturnT330};
static const ShockerModelType kSerialModels[]  = {ShockerModelType::CaiXianlin, ShockerModelType::Petrainer, ShockerModelType::Petrainer998DR, ShockerModelType::WellturnT330, ShockerModelType::D80};

// Every shock and vibrate command, on the gateway and serial paths and at `HandleCommand`, reaches no radio.
static void refusesEveryCommandOnEveryPath()
{
  const std::size_t radioBefore = g_radio.size();

  for (ShockerCommandType type : kCapped) {
    for (int intensity = 0; intensity <= 255; ++intensity) {
      const uint8_t i = static_cast<uint8_t>(intensity);
      for (uint16_t durationMs : kDurations) {
        for (uint16_t id : kIds) {
          for (ShockerModelType model : kGatewayModels) {
            sendGateway(model, id, type, i, durationMs);
          }
          for (ShockerModelType model : kSerialModels) {
            sendSerial(model, id, type, i, durationMs);
            CHECK(!CommandHandler::HandleCommand(model, id, type, i, durationMs));
          }
        }
      }
    }
  }

  CHECK(g_radio.size() == radioBefore);
}

static void zeroCapsAtBoot()
{
  bootHub();
  for (int64_t now : {int64_t {0}, int64_t {1}, kDayMs, kWarmMs}) {
    g_nowMs = now;
    refusesEveryCommandOnEveryPath();
  }

  RefusalSnapshot refusals = SafetyCaps::GetRefusals();
  CHECK(refusals.count > 0);
  CHECK(refusals.lastCode == static_cast<uint8_t>(Verdict::CapsNotSet));
  CHECK(g_serialOut.find("Failed to send command") != std::string::npos);
  CHECK(g_serialOut.find("Command sent") == std::string::npos);
}

// Each single cap at 0 in a stored record refuses that channel, on either channel.
static void storedRecord(int channel, int field)
{
  ChannelCaps bad = fixture();
  switch (field) {
    case 0:
      bad.maxIntensityPct = 0;
      break;
    case 1:
      bad.maxPulseMs = 0;
      break;
    case 2:
      bad.maxPulsesPerStage = 0;
      break;
    case 3:
      bad.minGapSec = 0;
      break;
    case 4:
      bad.maxPerHour = 0;
      break;
    default:
      bad.maxPerDay = 0;
      break;
  }
  plantRecord(channel == 0 ? bad : ChannelCaps {}, channel == 0 ? ChannelCaps {} : bad);
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void incoherentStoredCaps()
{
  ChannelCaps overHundred     = fixture();
  overHundred.maxIntensityPct = 101;
  plantRecord(overHundred, overHundred);
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void unknownStoredLayout()
{
  plantRecord(fixture(), fixture(), 0x12345678);
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void otherStoredVersion()
{
  plantRecord(fixture(), fixture(), 0x53504341, 2);
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void truncatedStoredRecord()
{
  plantRecord(fixture(), fixture());
  g_nvs.blob.resize(g_nvs.blob.size() - 1);
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void storageOpenFails()
{
  plantRecord(fixture(), fixture());
  g_nvs.openError = ESP_FAIL;
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

static void storageReadFails()
{
  plantRecord(fixture(), fixture());
  g_nvs.readError = ESP_FAIL;
  bootHub();
  g_nowMs = kWarmMs;
  refusesEveryCommandOnEveryPath();
}

// The positive control: the same path does reach the radio, so a refusal above comes from the caps and the harness
// is able to see a send.
static void withinCapsReachesRadio()
{
  plantRecord(fixture(), fixture());
  bootHub();
  g_nowMs = kWarmMs;

  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100));
  CHECK(g_radio.size() == 1);
  CHECK(g_radio.back().type == ShockerCommandType::Vibrate);
  CHECK(g_radio.back().intensity == 10);
  CHECK(g_radio.back().durationMs == 100);

  g_nowMs += 1000;
  sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 100);
  CHECK(g_radio.size() == 2);
  CHECK(g_radio.back().type == ShockerCommandType::Shock);

  g_nowMs += 1000;
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100);
  CHECK(g_radio.size() == 3);

  // One over the caps on each path, past the gap, refuses.
  g_nowMs += 1000;
  sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 11, 100);
  sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 101);
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 11, 100);
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 101);
  CHECK(g_radio.size() == 3);
}

// The bench state after a vibrate calibration: the vibrate channel set and every shock cap still 0.
static void vibrateSetLeavesShockRefused()
{
  plantRecord(ChannelCaps {}, fixture());
  bootHub();
  g_nowMs = kWarmMs;

  sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100);
  CHECK(g_radio.size() == 1);

  const std::size_t sent = g_radio.size();
  for (int intensity = 0; intensity <= 255; ++intensity) {
    for (uint16_t durationMs : kDurations) {
      g_nowMs += 1000;
      sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, static_cast<uint8_t>(intensity), durationMs);
      sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, static_cast<uint8_t>(intensity), durationMs);
    }
  }
  CHECK(g_radio.size() == sent);
}

static void clearRefusesAgain()
{
  plantRecord(fixture(), fixture());
  bootHub();
  g_nowMs = kWarmMs;
  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100));
  CHECK(g_radio.size() == 1);

  CHECK(SafetyCaps::Clear());
  g_radio.clear();
  g_nowMs += kDayMs;
  refusesEveryCommandOnEveryPath();
}

// A hub just past power-on holds even with valid caps: the cold boot hold of the fixture is 86400 s / 5.
static void coldBootHolds()
{
  plantRecord(fixture(), fixture());
  bootHub();
  g_nowMs = ColdBootHoldMs(fixture()) - 1;
  CHECK(!CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100));
  CHECK(SafetyCaps::GetRefusals().lastCode == static_cast<uint8_t>(Verdict::BootHold));
  CHECK(g_radio.empty());

  g_nowMs += 1;
  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100));
  CHECK(g_radio.size() == 1);
}

// Review H1 through the real `HandleCommand` and serial `rftransmit`: pulses as long as the gap, one stage apart, abut,
// so the second would join the first on the radio. It refuses as `over-on-time`, wire code 12, and never reaches the
// radio. The run's output counts one radio frame past each command's end (review N2), and once the output has stayed
// off for the 300 ms terminator after that a new run starts.
static void onTimeCeilingRefusesAJoinedPulse()
{
  ChannelCaps abutting  = fixture();
  abutting.maxPulseMs   = 1000;  // equal to the 1 s gap, the most the coherence rule allows
  abutting.maxPerHour   = 3;
  abutting.maxPerDay    = 5;
  CHECK(CapsCoherent(abutting));
  plantRecord(abutting, abutting);
  bootHub();
  g_nowMs = kWarmMs;

  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 1000));
  CHECK(g_radio.size() == 1);

  g_nowMs += 1000;
  CHECK(!CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 1000));
  CHECK(SafetyCaps::GetRefusals().lastCode == 12);
  CHECK(g_lastLog.find("verdict=over-on-time") != std::string::npos);
  g_nowMs += kMaxRadioFrameMs + kOutputRunBreakMs - 1;
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 1000);
  CHECK(g_radio.size() == 1);

  g_nowMs += 1;
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Shock, 10, 1000);
  CHECK(g_radio.size() == 2);
}

// The run break in CapLimiter.h is the RF task's terminator time. The constant there is file-local, so read it.
static void runBreakIsTheTerminator()
{
  std::ifstream in("src/radio/RFTransmitter.cpp");
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::smatch match;
  CHECK(std::regex_search(source, match, std::regex(R"(const int64_t kTerminatorDurationMs\s*=\s*(\d+);)")));
  CHECK(match.size() == 2 && std::stoll(match[1].str()) == kOutputRunBreakMs);
}

static void estopRefusesWithCapsSet()
{
  plantRecord(fixture(), fixture());
  bootHub();
  g_nowMs    = kWarmMs;
  s_estopped = true;
  sendGateway(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100);
  sendSerial(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Vibrate, 10, 100);
  CHECK(!CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Sound, 0, 100));
  CHECK(!CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Stop, 0, 100));
  CHECK(g_radio.empty());
}

// Stop, Sound and Light carry no cap, so they pass at every cap 0, and the radio gets exactly what was asked.
// `uncappedTypesNeverEncodeAsShock` covers what those types become on the air.
static void uncappedTypesPassAtZeroCaps()
{
  bootHub();
  g_nowMs = kWarmMs;

  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Stop, 0, 0));
  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Sound, 0, 300));
  CHECK(CommandHandler::HandleCommand(ShockerModelType::CaiXianlin, 7, ShockerCommandType::Light, 0, 300));
  CHECK(g_radio.size() == 3);
  CHECK(SafetyCaps::GetRefusals().count == 0);
}

template<typename Fill>
static std::vector<uint32_t> encode(std::size_t size, Fill fill, bool& ok)
{
  std::vector<rmt_data_t> buffer(size);
  std::memset(buffer.data(), 0, size * sizeof(rmt_data_t));
  ok = fill(buffer.data());
  std::vector<uint32_t> words;
  for (const rmt_data_t& d : buffer) {
    words.push_back(d.val);
  }
  return words;
}

struct Encoder {
  const char* name;
  std::size_t size;
  std::function<bool(rmt_data_t*, uint16_t, ShockerCommandType, uint8_t)> fill;
};

static std::vector<Encoder> encoders()
{
  return {
    {"CaiXianlin",
     Rmt::CaiXianlinEncoder::GetBufferSize(),
     [](rmt_data_t* s, uint16_t id, ShockerCommandType t, uint8_t i) {
    return Rmt::CaiXianlinEncoder::FillBuffer(s, id, 0, t, i);
  }},
    {"Petrainer", Rmt::PetrainerEncoder::GetBufferSize(), Rmt::PetrainerEncoder::FillBuffer},
    {"Petrainer998DR", Rmt::Petrainer998DREncoder::GetBufferSize(), Rmt::Petrainer998DREncoder::FillBuffer},
    {"WellturnT330", Rmt::WellturnT330Encoder::GetBufferSize(), Rmt::WellturnT330Encoder::FillBuffer},
    {"D80", Rmt::D80Encoder::GetBufferSize(), Rmt::D80Encoder::FillBuffer},
  };
}

// `SafetyCaps::Admit` answers `NotCapped` for anything but Shock and Vibrate, so the encoders are what stands
// between such a type and the air. Each must refuse a type it does not know, and Sound and Light must differ from
// Shock and Vibrate at the same id and intensity.
static void uncappedTypesNeverEncodeAsShock()
{
  for (const Encoder& e : encoders()) {
    for (int raw = 0; raw <= 255; ++raw) {
      const ShockerCommandType type = static_cast<ShockerCommandType>(raw);
      if (type == ShockerCommandType::Shock || type == ShockerCommandType::Vibrate || type == ShockerCommandType::Sound || type == ShockerCommandType::Light) {
        continue;
      }
      bool ok = true;
      encode(e.size, [&](rmt_data_t* s) { return e.fill(s, 7, type, 50); }, ok);
      if (ok) {
        std::printf("FAIL %s:%d: %s encodes type %d\n", __FILE__, __LINE__, e.name, raw);
        ++s_failures;
      }
    }

    for (int intensity = 0; intensity <= 255; ++intensity) {
      const uint8_t i = static_cast<uint8_t>(intensity);
      bool shockOk    = false;
      bool vibrateOk  = false;
      auto shock      = encode(e.size, [&](rmt_data_t* s) { return e.fill(s, 7, ShockerCommandType::Shock, i); }, shockOk);
      auto vibrate    = encode(e.size, [&](rmt_data_t* s) { return e.fill(s, 7, ShockerCommandType::Vibrate, i); }, vibrateOk);
      CHECK(shockOk);
      CHECK(vibrateOk);
      for (ShockerCommandType type : {ShockerCommandType::Sound, ShockerCommandType::Light}) {
        bool ok    = false;
        auto words = encode(e.size, [&](rmt_data_t* s) { return e.fill(s, 7, type, i); }, ok);
        if (ok && (words == shock || words == vibrate)) {
          std::printf("FAIL %s:%d: %s encodes an uncapped type as a capped one\n", __FILE__, __LINE__, e.name);
          ++s_failures;
        }
      }
    }
  }
}

// Review N2: `kMaxRadioFrameMs` in CapLimiter.h is the longest frame any encoder can send, rounded up to whole ms. For
// each model, every item position takes the longest value it shows over ids 0, 0xFFFF and each single bit, every type
// and intensities 0 to 255, which sets each id, type and intensity bit both ways. An item is a one or a zero, so the sum
// bounds every frame that model can send. The RF task's tick, read from its source since the constant is file-local,
// turns ticks into time. The constant must equal the largest bound rounded up, so an encoder or tick change that
// lengthens a frame fails here, and so does one that shortens the longest.
static void maxRadioFrameCoversEveryEncoder()
{
  std::ifstream in("src/radio/RFTransmitter.cpp");
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::smatch match;
  CHECK(std::regex_search(source, match, std::regex(R"(const float kTickrateNs\s*=\s*(\d+);)")));
  const int64_t tickNs = match.size() == 2 ? std::stoll(match[1].str()) : 0;
  CHECK(tickNs == 1000);

  std::vector<uint16_t> ids = {0, 0xFFFF};
  for (int bit = 0; bit < 16; ++bit) {
    ids.push_back(static_cast<uint16_t>(1u << bit));
  }
  int64_t longestNs = 0;
  for (const Encoder& e : encoders()) {
    std::vector<int64_t> itemMax(e.size, 0);
    int frames = 0;
    for (uint16_t id : ids) {
      for (ShockerCommandType type : {ShockerCommandType::Shock, ShockerCommandType::Vibrate, ShockerCommandType::Sound, ShockerCommandType::Light}) {
        for (int intensity = 0; intensity <= 255; ++intensity) {
          std::vector<rmt_data_t> buffer(e.size);
          std::memset(buffer.data(), 0, e.size * sizeof(rmt_data_t));
          if (!e.fill(buffer.data(), id, type, static_cast<uint8_t>(intensity))) {
            continue;
          }
          ++frames;
          for (std::size_t i = 0; i < e.size; ++i) {
            itemMax[i] = std::max<int64_t>(itemMax[i], buffer[i].duration0 + buffer[i].duration1);
          }
        }
      }
    }
    CHECK(frames > 0);
    int64_t boundNs = 0;
    for (int64_t ticks : itemMax) {
      boundNs += ticks * tickNs;
    }
    longestNs = std::max(longestNs, boundNs);
  }
  CHECK(longestNs > 0);
  CHECK(kMaxRadioFrameMs == (longestNs + 999'999) / 1'000'000);
  if (kMaxRadioFrameMs != (longestNs + 999'999) / 1'000'000) {
    std::printf("  longest frame bound %lld ns, kMaxRadioFrameMs %lld\n", static_cast<long long>(longestNs), static_cast<long long>(kMaxRadioFrameMs));
  }
}

// Every payload the port 81 socket can carry refuses before any handler runs. Finding 5dc07581: this socket must not
// move the transmitter.
static void localSocketRefusesEveryPayload()
{
  bootHub();
  const gpio_num_t pinBefore = CommandHandler::GetRfTxPin();
  const std::size_t built    = g_transmitterPins.size();

  for (int raw = static_cast<int>(LocalSchema::LocalToHubMessagePayload::MIN); raw <= static_cast<int>(LocalSchema::LocalToHubMessagePayload::MAX); ++raw) {
    const auto payload = static_cast<LocalSchema::LocalToHubMessagePayload>(raw);
    Serialization::Local::g_lastError.clear();
    const int frames = s_socketFrames;
    if (payload == LocalSchema::LocalToHubMessagePayload::SetRfTxPinCommand) {
      continue;  // Sent with a real pin below.
    }
    sendLocalEmpty(payload);
    CHECK(s_localHandlerRuns == 0);
    CHECK(Serialization::Local::g_lastError == "refused_local_command");
    CHECK(s_socketFrames == frames + 1);
    CHECK(g_lastLog.find("Refusing local socket command") != std::string::npos);
  }

  CHECK(CommandHandler::GetRfTxPin() == pinBefore);
  CHECK(g_transmitterPins.size() == built);
}

// SPR-75: the real `HandleSetRfTxPinCommand` is linked, and it never runs for a frame from the socket, for any pin.
static void localSocketCannotMoveTransmitter()
{
  bootHub();
  const gpio_num_t pinBefore = CommandHandler::GetRfTxPin();
  const std::size_t built    = g_transmitterPins.size();

  for (int pin = -128; pin <= 127; ++pin) {
    Serialization::Local::g_lastError.clear();
    sendLocalSetRfTxPin(static_cast<int8_t>(pin));
    CHECK(Serialization::Local::g_lastError == "refused_local_command");
    CHECK(g_lastLog.find("SetRfTxPinCommand") != std::string::npos);
  }

  CHECK(CommandHandler::GetRfTxPin() == pinBefore);
  CHECK(s_configTxPinWrites == 0);
  CHECK(g_transmitterPins.size() == built);
}

// Serial `rftxpin` stays the one path (SPR-75). This is also the positive control of the test above: the same real
// `SetRfTxPin` does move the pin when serial asks.
static void serialStillMovesTransmitter()
{
  bootHub();
  _handleRfTxPinCommand("14"sv, false);
  CHECK(CommandHandler::GetRfTxPin() == static_cast<gpio_num_t>(14));
  CHECK(s_configTxPinWrites == 1);
  CHECK(g_serialOut.find("Saved config") != std::string::npos);
}

// The real `HandleTrigger`, finding 3 of 5dc07581: the gateway cannot hold the portal open or reboot the hub.
static void gatewayTriggers()
{
  sendTrigger(GatewaySchema::TriggerType::CaptivePortalEnable);
  CHECK(s_alwaysOpen.empty());

  sendTrigger(GatewaySchema::TriggerType::Restart);
  CHECK(g_restarts == 0);

  // Closing the portal still works, and the handler reaches `Trigger`. What `Trigger` does is test_kill_path.cpp's.
  sendTrigger(GatewaySchema::TriggerType::CaptivePortalDisable);
  CHECK(s_alwaysOpen.size() == 1);
  CHECK(!s_alwaysOpen.back());

  sendTrigger(GatewaySchema::TriggerType::EmergencyStop);
  CHECK(s_estopTriggers == 1);
}

static std::string readFile(const std::filesystem::path& path)
{
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int countMatches(const std::string& text, const std::regex& pattern)
{
  return static_cast<int>(std::distance(std::sregex_iterator(text.begin(), text.end(), pattern), std::sregex_iterator()));
}

// Every file under src and include holding a match, as `path=count`, sorted.
static std::string sitesOf(const char* pattern)
{
  const std::regex re(pattern);
  std::vector<std::string> sites;
  for (const char* root : {"src", "include"}) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || entry.path().string().find("_fbs") != std::string::npos) {
        continue;
      }
      const int n = countMatches(readFile(entry.path()), re);
      if (n > 0) {
        sites.push_back(entry.path().generic_string() + "=" + std::to_string(n));
      }
    }
  }
  std::sort(sites.begin(), sites.end());
  std::string joined;
  for (const std::string& site : sites) {
    joined += site + " ";
  }
  return joined;
}

#define CHECK_SITES(pattern, expected)                                                                             \
  do {                                                                                                             \
    std::string got__ = sitesOf(pattern);                                                                          \
    if (got__ != (expected)) {                                                                                     \
      std::printf("FAIL %s:%d: %s\n  got  %s\n  want %s\n", __FILE__, __LINE__, pattern, got__.c_str(), expected); \
      ++s_failures;                                                                                                \
    }                                                                                                              \
  } while (0)

// The runs above prove the paths they reach. This pins the list of paths, so a new caller of the radio has to change
// this test.
static void radioHasNoOtherCaller()
{
  CHECK_SITES(R"(->SendCommand\()", "src/CommandHandler.cpp=2 ");
  CHECK_SITES(R"(make_unique<RFTransmitter>|new RFTransmitter)", "src/CommandHandler.cpp=2 ");
  CHECK_SITES(R"(rmtWriteBlocking\()", "src/RGBPatternManager.cpp=1 src/radio/RFTransmitter.cpp=2 ");
  CHECK_SITES(
    R"(FillBuffer\()",
    "include/radio/rmt/CaiXianlinEncoder.h=1 include/radio/rmt/D80Encoder.h=1 include/radio/rmt/Petrainer998DREncoder.h=1 include/radio/rmt/PetrainerEncoder.h=1 include/radio/rmt/T330Encoder.h=1 src/radio/rmt/CaiXianlinEncoder.cpp=1 "
    "src/radio/rmt/D80Encoder.cpp=1 src/radio/rmt/Petrainer998DREncoder.cpp=1 src/radio/rmt/PetrainerEncoder.cpp=1 src/radio/rmt/Sequence.cpp=5 src/radio/rmt/T330Encoder.cpp=1 "
  );
  CHECK_SITES(R"(CommandHandler::HandleCommand\()", "src/CommandHandler.cpp=1 src/message_handlers/websocket/gateway/ShockerCommandList.cpp=1 src/serial/command_handlers/rftransmit.cpp=1 ");
  CHECK_SITES(R"(CommandHandler::SetRfTxPin\()", "src/CommandHandler.cpp=1 src/message_handlers/websocket/local/SetRfTxPinCommand.cpp=1 src/serial/command_handlers/rftxpin.cpp=1 ");
  CHECK_SITES(R"(HandleLocalBinary\()", "include/message_handlers/WebSocket.h=1 src/captiveportal/CaptivePortalInstance.cpp=1 src/message_handlers/websocket/Local.cpp=1 ");

  const std::string handler = readFile("src/CommandHandler.cpp");

  // The keep-alive task is the one send outside `HandleCommand`, and it is a vibrate at intensity 0.
  CHECK(handler.find("s_rfTransmitter->SendCommand(cmdRef.model, cmdRef.shockerId, ShockerCommandType::Vibrate, 0, KEEP_ALIVE_DURATION, false)") != std::string::npos);

  // Inside `HandleCommand` the E-Stop check and the cap gate come before the send.
  const std::size_t entry = handler.find("bool CommandHandler::HandleCommand(");
  const std::size_t estop = handler.find("EStopManager::IsEStopped()", entry);
  const std::size_t admit = handler.find("SafetyCaps::Admit(", entry);
  const std::size_t send  = handler.find("s_rfTransmitter->SendCommand(", entry);
  CHECK(entry != std::string::npos && estop != std::string::npos && admit != std::string::npos && send != std::string::npos);
  CHECK(entry < estop && estop < admit && admit < send);
}

typedef void (*Scenario)();

// Runs a scenario in a forked child, so the static state of the real `SafetyCaps` and `CommandHandler` starts fresh.
static void run(const char* name, Scenario scenario)
{
  std::fflush(stdout);
  const pid_t child = fork();
  if (child == 0) {
    s_failures = 0;  // A child reports only its own scenario.
    scenario();
    std::fflush(stdout);
    _exit(s_failures == 0 ? 0 : 1);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::printf("FAIL scenario %s\n", name);
    ++s_failures;
  }
}

template<int Channel, int Field>
static void storedRecordCase()
{
  storedRecord(Channel, Field);
}

#define SCENARIO(fn) run(#fn, fn)

int main()
{
  SCENARIO(zeroCapsAtBoot);
  SCENARIO((storedRecordCase<0, 0>));
  SCENARIO((storedRecordCase<0, 1>));
  SCENARIO((storedRecordCase<0, 2>));
  SCENARIO((storedRecordCase<0, 3>));
  SCENARIO((storedRecordCase<0, 4>));
  SCENARIO((storedRecordCase<0, 5>));
  SCENARIO((storedRecordCase<1, 0>));
  SCENARIO((storedRecordCase<1, 1>));
  SCENARIO((storedRecordCase<1, 2>));
  SCENARIO((storedRecordCase<1, 3>));
  SCENARIO((storedRecordCase<1, 4>));
  SCENARIO((storedRecordCase<1, 5>));
  SCENARIO(incoherentStoredCaps);
  SCENARIO(unknownStoredLayout);
  SCENARIO(otherStoredVersion);
  SCENARIO(truncatedStoredRecord);
  SCENARIO(storageOpenFails);
  SCENARIO(storageReadFails);
  SCENARIO(withinCapsReachesRadio);
  SCENARIO(vibrateSetLeavesShockRefused);
  SCENARIO(clearRefusesAgain);
  SCENARIO(coldBootHolds);
  SCENARIO(onTimeCeilingRefusesAJoinedPulse);
  SCENARIO(runBreakIsTheTerminator);
  SCENARIO(estopRefusesWithCapsSet);
  SCENARIO(uncappedTypesPassAtZeroCaps);
  SCENARIO(uncappedTypesNeverEncodeAsShock);
  SCENARIO(maxRadioFrameCoversEveryEncoder);
  SCENARIO(localSocketRefusesEveryPayload);
  SCENARIO(localSocketCannotMoveTransmitter);
  SCENARIO(serialStillMovesTransmitter);
  SCENARIO(gatewayTriggers);
  SCENARIO(radioHasNoOtherCaller);

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return EXIT_FAILURE;
  }
  std::printf("command paths: all checks passed\n");
  return EXIT_SUCCESS;
}
