#include "serial/command_handlers/common.h"

#include "Convert.h"
#include "softproxy/SafetyCaps.h"
#include "util/StringUtils.h"

// `safetycaps` reads and sets the Softproxy caps (include/softproxy/CapLimiter.h) over the local
// serial port only. The operator sets them at an attended session. The fork ships no cap value.

using OpenShock::Softproxy::ChannelCaps;

static void printChannel(const char* name, const ChannelCaps& c, OpenShock::ShockerCommandType type)
{
  uint32_t lastHour = 0;
  uint32_t lastDay  = 0;
  OpenShock::Softproxy::SafetyCaps::GetCounts(type, lastHour, lastDay);

  SERPR_RESPONSE(
    "SafetyCaps|%s|maxIntensityPct=%u,maxPulseMs=%u,maxPulsesPerStage=%u,minGapSec=%u,maxPerHour=%u,maxPerDay=%u,stageJoinMs=%u|set=%s,stagesLastHour=%u,stagesLastDay=%u",
    name,
    static_cast<unsigned>(c.maxIntensityPct),
    static_cast<unsigned>(c.maxPulseMs),
    static_cast<unsigned>(c.maxPulsesPerStage),
    static_cast<unsigned>(c.minGapSec),
    static_cast<unsigned>(c.maxPerHour),
    static_cast<unsigned>(c.maxPerDay),
    static_cast<unsigned>(c.stageJoinMs),
    OpenShock::Softproxy::CapsSet(c) && OpenShock::Softproxy::CapsCoherent(c) ? "true" : "false",
    static_cast<unsigned>(lastHour),
    static_cast<unsigned>(lastDay)
  );
}

static void _handleSafetyCapsGetCommand(std::string_view arg, bool isAutomated)
{
  (void)isAutomated;

  if (!arg.empty()) {
    SERPR_ERROR("Invalid argument (takes no argument)");
    return;
  }

  ChannelCaps shock;
  ChannelCaps vibrate;
  OpenShock::Softproxy::SafetyCaps::Get(shock, vibrate);

  printChannel("shock", shock, OpenShock::ShockerCommandType::Shock);
  printChannel("vibrate", vibrate, OpenShock::ShockerCommandType::Vibrate);
}

static void _handleSafetyCapsSet(std::string_view arg, OpenShock::ShockerCommandType type)
{
  auto parts = OpenShock::StringSplitWhiteSpace(OpenShock::StringTrim(arg));
  if (parts.size() != 6 && parts.size() != 7) {
    SERPR_ERROR("Invalid arguments (expected maxIntensityPct maxPulseMs maxPulsesPerStage minGapSec maxPerHour maxPerDay [stageJoinMs])");
    return;
  }

  ChannelCaps caps = {};
  bool ok          = OpenShock::Convert::ToUint8(parts[0], caps.maxIntensityPct)     //
         && OpenShock::Convert::ToUint16(parts[1], caps.maxPulseMs)         //
         && OpenShock::Convert::ToUint16(parts[2], caps.maxPulsesPerStage)  //
         && OpenShock::Convert::ToUint32(parts[3], caps.minGapSec)          //
         && OpenShock::Convert::ToUint16(parts[4], caps.maxPerHour)         //
         && OpenShock::Convert::ToUint16(parts[5], caps.maxPerDay);
  if (ok && parts.size() == 7) {
    ok = OpenShock::Convert::ToUint32(parts[6], caps.stageJoinMs);
  }
  if (!ok) {
    SERPR_ERROR("Invalid argument (every value must be a whole number in range)");
    return;
  }
  if (!OpenShock::Softproxy::CapsSet(caps)) {
    SERPR_ERROR("Invalid caps (every cap except stageJoinMs must be above 0; use `safetycaps clear` to refuse every command)");
    return;
  }
  if (!OpenShock::Softproxy::CapsCoherent(caps)) {
    SERPR_ERROR("Invalid caps (need maxIntensityPct <= 100, maxPerHour <= maxPerDay and minGapSec * maxPerHour <= 3600)");
    return;
  }

  if (!OpenShock::Softproxy::SafetyCaps::Set(type, caps)) {
    SERPR_ERROR("Failed to store caps");
    return;
  }

  SERPR_SUCCESS("Saved caps");
}

static void _handleSafetyCapsShockCommand(std::string_view arg, bool isAutomated)
{
  (void)isAutomated;
  _handleSafetyCapsSet(arg, OpenShock::ShockerCommandType::Shock);
}

static void _handleSafetyCapsVibrateCommand(std::string_view arg, bool isAutomated)
{
  (void)isAutomated;
  _handleSafetyCapsSet(arg, OpenShock::ShockerCommandType::Vibrate);
}

static void _handleSafetyCapsClearCommand(std::string_view arg, bool isAutomated)
{
  (void)isAutomated;

  if (!arg.empty()) {
    SERPR_ERROR("Invalid argument (takes no argument)");
    return;
  }

  if (!OpenShock::Softproxy::SafetyCaps::Clear()) {
    SERPR_ERROR("Caps are 0 until restart, but erasing the stored caps failed");
    return;
  }

  SERPR_SUCCESS("Cleared caps");
}

OpenShock::Serial::CommandGroup OpenShock::Serial::CommandHandlers::SafetyCapsHandler()
{
  auto group = OpenShock::Serial::CommandGroup("safetycaps"sv);

  group.addCommand("Get the Softproxy caps for shock and vibrate, and the stages counted since boot"sv, _handleSafetyCapsGetCommand);

  auto& shockCommand = group.addCommand("shock"sv, "Set the Softproxy shock caps"sv, _handleSafetyCapsShockCommand);
  shockCommand.addArgument("maxIntensityPct"sv, "1 to 100"sv, "<operator value>"sv);
  shockCommand.addArgument("maxPulseMs"sv, "above 0"sv, "<operator value>"sv);
  shockCommand.addArgument("maxPulsesPerStage"sv, "above 0"sv, "<operator value>"sv);
  shockCommand.addArgument("minGapSec"sv, "above 0"sv, "<operator value>"sv);
  shockCommand.addArgument("maxPerHour"sv, "above 0, at most maxPerDay"sv, "<operator value>"sv);
  shockCommand.addArgument("maxPerDay"sv, "above 0"sv, "<operator value>"sv);
  shockCommand.addArgument("stageJoinMs"sv, "optional, 0 makes every command its own stage"sv, "0"sv);

  auto& vibrateCommand = group.addCommand("vibrate"sv, "Set the Softproxy vibrate caps"sv, _handleSafetyCapsVibrateCommand);
  vibrateCommand.addArgument("maxIntensityPct"sv, "1 to 100"sv, "<operator value>"sv);
  vibrateCommand.addArgument("maxPulseMs"sv, "above 0"sv, "<operator value>"sv);
  vibrateCommand.addArgument("maxPulsesPerStage"sv, "above 0"sv, "<operator value>"sv);
  vibrateCommand.addArgument("minGapSec"sv, "above 0"sv, "<operator value>"sv);
  vibrateCommand.addArgument("maxPerHour"sv, "above 0, at most maxPerDay"sv, "<operator value>"sv);
  vibrateCommand.addArgument("maxPerDay"sv, "above 0"sv, "<operator value>"sv);
  vibrateCommand.addArgument("stageJoinMs"sv, "optional, 0 makes every command its own stage"sv, "0"sv);

  group.addCommand("clear"sv, "Set every Softproxy cap to 0, so shock and vibrate refuse"sv, _handleSafetyCapsClearCommand);

  return group;
}
