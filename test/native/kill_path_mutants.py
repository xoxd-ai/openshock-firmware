#!/usr/bin/env python3
# Mutation run for test/native/test_kill_path.cpp and test/native/test_config_writers.cpp (TIN-7545). Each mutant breaks
# one line of the kill path in a copy of the tree. The tests must then go red: a mutant is red when either test fails.
# A mutant that stays green is a gap in the tests, and the run fails.
#
# From the repository root, after `pio pkg install -e Pishock-2023`:
#
#   python3 test/native/kill_path_mutants.py                  # every mutant, in turn
#   python3 test/native/kill_path_mutants.py --only K11 R4    # a few
#   python3 test/native/kill_path_mutants.py --budget 540     # stop after 540 seconds, a later run resumes
#
# Results sit in the work directory with the tree they belong to, so a run after an edit starts over. The run has no
# load stop of its own: a caller that shares a host runs it in a slice that caps it.
#
# Exit codes: 0 every mutant red, 1 a mutant survived, 2 the tree or a mutant is broken, 75 stopped early, run it again.
# A mutant whose text no longer matches the tree counts as broken, so the list cannot drift unseen.

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

TEST = 'test/native/test_kill_path.cpp'
CH = 'src/CommandHandler.cpp'
SC = 'src/softproxy/SafetyCaps.cpp'
DF = 'src/WebSocketDeFragger.cpp'
RF = 'src/radio/RFTransmitter.cpp'
ES = 'src/EStopManager.cpp'
KP = 'include/softproxy/KillPath.h'
TR = 'src/message_handlers/websocket/gateway/Trigger.cpp'
RFTXPIN = 'src/serial/command_handlers/rftxpin.cpp'
SERIAL_CAPS = 'src/serial/command_handlers/safetycaps.cpp'
CONFIG = 'src/config/Config.cpp'
CHIPSET = 'include/Chipset.h'
CONFIG_TEST = 'test/native/test_config_writers.cpp'
ESTOP_CONFIG = 'src/config/EStopConfig.cpp'
RF_CONFIG = 'src/config/RFConfig.cpp'
CONFIG_UTILS = 'src/config/internal/utils.cpp'
CONFIG_UNITS = [CONFIG_TEST, CONFIG, 'src/config/RootConfig.cpp', RF_CONFIG, ESTOP_CONFIG, CONFIG_UTILS]
ENCODERS = ['src/radio/rmt/%sEncoder.cpp' % name for name in ('CaiXianlin', 'D80', 'Petrainer998DR', 'Petrainer', 'T330')]
UNITS = [TEST, CH, SC, DF, RF, 'src/radio/rmt/Sequence.cpp'] + ENCODERS + ['src/serial/command_handlers/estop.cpp', RFTXPIN, TR, 'src/util/StringUtils.cpp']

# A header reaches these units. EStopManager.cpp and safetycaps.cpp compile inside the test.
HEADER = [TEST, CH, SC]

RISE = 'return estopArmed && source == EStopLatchSource::Button;'
LOCK = 'return anyCapSet || source == EStopLatchSource::Button || source == EStopLatchSource::Gateway || source == EStopLatchSource::Unconfirmed;'
WARM = '    case ESP_RST_SW:\n'
RESERVED = 'return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15 || (pin >= 6 && pin <= 11);'
GATE = 'if (EStopManager::IsEStopped() && triggerType != TriggerType::EmergencyStop) {'
NEGATIVE_PIN = 'if (pin < 0 && pin != OPENSHOCK_GPIO_INVALID) {'

# The clear before the sample count: it ends at the wall time 5 seconds after it started, however few samples ran.
CLEAR_ON_WALL_TIME = ('} else if ([&] { static int64_t endsAt; if (clearSamplesLeft == k_estopClearSamples) endsAt = now + k_estopClearTime'
                      ' - k_estopUpdateRate; --clearSamplesLeft; return now >= endsAt; }()) {')

# Equivalent mutants, left out of the run. K29 drops `estopPin == kBoardRfTxPin` from `EStopPinConflicts`. On both
# PiShock builds the board RF TX pin is 12 or 15, and `IsEStopReservedPin` refuses both, so no input tells the two apart.
# pinsStaySeparate checks that both pins are reserved, and a static_assert in KillPath.h fails every build that defines
# SOFTPROXY_PISHOCK_BUILD on an RF TX pin that is not reserved. Such a build needs K29 back in the run.
# X7, X9 and X11 came from the review of `ee79e67`. Each changes no latch value and no `IsEStopped` reading:
# X7: a refused release means a counted stop, and the pass that takes its flag clears the measured down anyway.
# X9: a refused release means `Trigger` counted a stop, and it sets the flag right after the count. Until the pass that
# takes the flag, the task reads and publishes `active-clearing` where the code has `active`. On the host that is the
# next pass. On the hub it lasts as long as `Trigger` takes from its count to its flag, which nothing bounds. The clear
# count has run out, so the next pass wraps it and the clear needs 2^32 more samples: the latch cannot release first.
# X11: with no task nothing compares the count, and a task that starts takes the count as it then stands.
# X15 came from the review of `25f1449`. The pass that takes a gateway stop also samples the pin. That pass has already
# set Active and cleared the measured down, so its sample is the one the next pass would take 5 milliseconds later. It
# moves every later window one sample earlier and changes no rule, so it is equivalent up to a one-sample shift. Only a
# contact that changes on that one sample could end with another source, which no test can place apart from bounce.
EQUIVALENT = [
  ('K29 estop rule ignores the board tx pin', KP, 'estopPin == txPin || estopPin == kBoardRfTxPin', 'estopPin == txPin', HEADER),
  ('X7 refused release keeps the measured down', ES, '          state   = estopmanager_release(gatewayStopsTaken) ? EStopState::Idle : EStopState::Active;\n          sawDown = false;\n', '          if (estopmanager_release(gatewayStopsTaken)) { state = EStopState::Idle; sawDown = false; } else { state = EStopState::Active; }\n', [TEST]),
  ('X9 refused release keeps clearing', ES, '? EStopState::Idle : EStopState::Active;', '? EStopState::Idle : EStopState::ActiveClearing;', [TEST]),
  ('X11 Trigger skips the count when no task', ES, '  estopmanager_countgatewaystop();\n  if (!IsEStopped()) {', '  if (s_estopTask != nullptr) estopmanager_countgatewaystop();\n  if (!IsEStopped()) {', [TEST]),
  ('X15 the trigger pass samples the pin', ES, '      // Do not modify history here; allow hardware to take over on subsequent iterations.\n      continue;\n', '      // Do not modify history here; allow hardware to take over on subsequent iterations.\n', [TEST]),
]

# Name, file, text to replace, replacement, units to rebuild. An empty unit list means the test reads the file itself.
MUTANTS = [
  ('K1 HandleCommand ignores not armed', CH, '!EStopManager::IsArmed())', 'false)', [CH]),
  ('K2 HandleCommand ignores engaged', CH, 'if (EStopManager::IsEStopped()) {\n    Softproxy::SafetyCaps::RecordRefusal', 'if (false) {\n    Softproxy::SafetyCaps::RecordRefusal', [CH]),
  ('K3 task start clears the latch', ES, 'const bool startedEngaged = EStopManager::IsEStopped();', '(void)estopmanager_release(s_gatewayStops); const bool startedEngaged = false;', [TEST]),
  ('K4 Init ignores the retained latch', ES, 'if (!estopmanager_warmrestart() || !Softproxy::RestoreEStop(s_retainedLatch, source)) {', 'if (true) {', [TEST]),
  ('K5 Trigger only sets the flag', ES, '  if (!IsEStopped()) {\n    s_estopActivatedAt = OpenShock::millis();\n  }\n  estopmanager_engage(Softproxy::EStopLatchSource::Gateway);\n\n  OpenShock::ScopedLock', '  s_externallyTriggered = true;\n  return;\n  OpenShock::ScopedLock', [TEST]),
  ('K6 estop pin ignores the lock', ES, '  if (estopmgr_settingslocked()) {\n    OS_LOGW(TAG, "Refusing to move', '  if (false) {\n    OS_LOGW(TAG, "Refusing to move', [TEST]),
  ('K7 estop enabled false ignores the lock', ES, 'if (!enabled && estopmgr_settingslocked()) {', 'if (false) {', [TEST]),
  ('K8 lock ignores caps', KP, 'return anyCapSet || source == EStopLatchSource::Button', 'return source == EStopLatchSource::Button', HEADER),
  ('K9 lock ignores the latch', KP, LOCK, 'return anyCapSet;', HEADER),
  ('K10 cap rise allowed always', KP, RISE, 'return true;', HEADER),
  ('K11 cap rise skips armed', KP, RISE, 'return source == EStopLatchSource::Button;', HEADER),
  ('K12 lower gap is no rise', KP, '|| to.minGapSec < from.minGapSec', '', HEADER),
  ('K13 defrag has no bound', DF, 'length > m_maxMessageBytes - message.data.size() || ', '', [DF]),
  ('K14 defrag uses the panic append', DF, '!message.data.try_append(data, length)', '(message.data.append(data, length), false)', [DF]),
  ('K15 SetRfTxPin skips the rule', CH, 'if (Softproxy::TxPinConflicts(txPin, estopPin, EStopManager::GetRunningPin())) {', 'if (false) {', [CH]),
  ('K16 boot skips the tx rule', CH, 'if (Softproxy::TxPinConflicts(txPin, estopConfig.gpioPin, EStopManager::GetRunningPin())) {', 'if (false) {', [CH]),
  ('K17 estop pin skips the conflict check', ES, 'if (Softproxy::EStopPinConflicts(pin, txPin)) {', 'if (false) {', [TEST]),
  ('K18 history starts as stop, so a warm restart engages', ES, 'uint16_t history = 0;', 'uint16_t history = 0xFFFF;', [TEST]),
  ('K19 stale sample still armed', ES, '<= k_estopSampleStaleTime;', '<= 0xFFFFFFFFu;', [TEST]),
  ('K20 start with no valid pin', ES, 'if (!OpenShock::IsValidInputPin(s_estopPin)) {\n      OS_LOGE(TAG, "Refusing to start', 'if (false) {\n      OS_LOGE(TAG, "Refusing to start', [TEST]),
  ('K21 engaged refusal not recorded', CH, 'Softproxy::SafetyCaps::RecordRefusal(type, Softproxy::Verdict::EStopEngaged);', '', [CH]),
  ('K22 transmit task ignores the E-Stop end', RF, 'seq->setTransmitEnd(now);', '', [RF]),
  ('K23 no pull-up pin allowed', KP, '|| !HasInternalPullUp(estopPin)', '', HEADER),
  ('K24 tx rule ignores the running pin', KP, 'txPin == configuredEStopPin || txPin == runningEStopPin ||', 'txPin == configuredEStopPin ||', HEADER),
  ('K25 config rule skips the tx side', KP, 'if (EStopPinConflicts(next.pin, nextTxPin) || TxPinConflicts(nextTxPin, next.pin, runningEStopPin)) {', 'if (EStopPinConflicts(next.pin, nextTxPin)) {', HEADER),
  ('K26 unreadable caps count as clear', SC, 'if (!lock__.isLocked() || !ensureLoaded()) {\n    return true;', 'if (!lock__.isLocked() || !ensureLoaded()) {\n    return false;', [SC]),
  ('K27 tx rule ignores the stored pin', KP, 'return txPin == configuredEStopPin || txPin == runningEStopPin', 'return txPin == runningEStopPin', HEADER),
  ('K28 estop rule ignores the stored tx pin', KP, 'return estopPin == txPin || estopPin == kBoardRfTxPin', 'return estopPin == kBoardRfTxPin', HEADER),
  ('K30 Set has no rise check', SC, 'if (CapsRise(previous, caps) && !CapRiseAllowed(EStopManager::IsArmed(), EStopManager::GetLatchSource())) {', 'if (false) {', [SC]),
  ('K31 armed with no pin or task', ES, 'if (s_estopTask == nullptr || !OpenShock::IsValidInputPin(s_estopPin)) {\n    return false;\n  }\n\n  // Read the sample', 'if (false) {\n    return false;\n  }\n  return true;\n  // Read the sample', [TEST]),
  ('K32 tx rule keyed on no run-time pin', CH, 'Softproxy::TxPinConflicts(txPin, estopPin, EStopManager::GetRunningPin())', 'Softproxy::TxPinConflicts(txPin, GPIO_NUM_NC, GPIO_NUM_NC)', [CH]),
  ('K33 rftxpin falls through on a bad argument', RFTXPIN, 'SERPR_ERROR("Invalid argument (number invalid or out of range)");\n    return;', 'SERPR_ERROR("Invalid argument (number invalid or out of range)");', [RFTXPIN]),
  ('K34 serial refusal reads as saved', SERIAL_CAPS, 'SERPR_ERROR("Refused (raising a cap needs the E-Stop armed and engaged by its button, see `estop state`)");', 'SERPR_SUCCESS("Saved caps");', [TEST]),
  ('K35 any armed engagement allows a rise', KP, RISE, 'return estopArmed && source != EStopLatchSource::Released;', HEADER),
  ('K36 gateway counts as a press', KP, RISE, 'return estopArmed && (source == EStopLatchSource::Button || source == EStopLatchSource::Gateway);', HEADER),
  ('K37 cold boot counts as a press', KP, RISE, 'return estopArmed && (source == EStopLatchSource::Button || source == EStopLatchSource::ColdBoot);', HEADER),
  ('K38 Trigger records a button', ES, '  estopmanager_engage(Softproxy::EStopLatchSource::Gateway);\n\n  OpenShock::ScopedLock', '  estopmanager_engage(Softproxy::EStopLatchSource::Button);\n\n  OpenShock::ScopedLock', [TEST]),
  ('K39 raced trigger records a button', ES, '      estopmanager_engage(Softproxy::EStopLatchSource::Gateway);\n      state   = EStopState::Active;', '      estopmanager_engage(Softproxy::EStopLatchSource::Button);\n      state   = EStopState::Active;', [TEST]),
  ('K40 cold boot starts released', ES, 'auto source = Softproxy::EStopLatchSource::ColdBoot;', 'auto source = Softproxy::EStopLatchSource::Released;', [TEST]),
  ('K41 restart reads every latch as a press', KP, '        source = candidate;\n', '        source = candidate == EStopLatchSource::Released ? candidate : EStopLatchSource::Button;\n', HEADER),
  ('K42 lock skips the gateway latch', KP, LOCK, 'return anyCapSet || source == EStopLatchSource::Button || source == EStopLatchSource::Unconfirmed;', HEADER),
  ('K43 lock skips the button latch', KP, LOCK, 'return anyCapSet || source == EStopLatchSource::Gateway || source == EStopLatchSource::Unconfirmed;', HEADER),
  ('K44 lock takes the cold boot latch, so a fresh hub gets no pin', KP, LOCK, 'return anyCapSet || source != EStopLatchSource::Released;', HEADER),
  ('K45 a clear under way records a button', ES, '          state            = EStopState::ActiveClearing;\n', '          state            = EStopState::ActiveClearing;\n          s_latch.store(Softproxy::EStopLatchSource::Button);\n', [TEST]),
  ('K46 engage restates the source', ES, '  if (s_latch.load() == Softproxy::EStopLatchSource::Released) {\n    estopmanager_writelatch(source);\n  }', '  estopmanager_writelatch(source);', [TEST]),
  ('K47 latch not sealed on engage', ES, '  s_latch.store(source);\n  Softproxy::SealEStop(s_retainedLatch, source);', '  s_latch.store(source);\n  if (source == Softproxy::EStopLatchSource::Released) Softproxy::SealEStop(s_retainedLatch, source);', [TEST]),
  ('K48 shorter join window is no rise', KP, 'to.stageJoinMs != from.stageJoinMs', 'to.stageJoinMs > from.stageJoinMs', HEADER),
  ('K49 Config lock reads engaged only', CONFIG, 'OpenShock::Softproxy::EStopSettingsLocked(OpenShock::EStopManager::GetLatchSource(), OpenShock::Softproxy::SafetyCaps::AnySet())', 'OpenShock::EStopManager::IsEStopped() || OpenShock::Softproxy::SafetyCaps::AnySet()', [CONFIG]),
  ('K50 Init drops the reset reason check', ES, '!estopmanager_warmrestart() || ', '', [TEST]),
  ('K51 brownout counts as warm', ES, WARM, '    case ESP_RST_BROWNOUT:\n' + WARM, [TEST]),
  ('K52 power-on counts as warm', ES, WARM, '    case ESP_RST_POWERON:\n' + WARM, [TEST]),
  ('K53 EN counts as warm', ES, WARM, '    case ESP_RST_EXT:\n' + WARM, [TEST]),
  ('K54 unknown reasons count as warm', ES, '    default:\n      return false;\n  }\n}\n\nstatic const char* estopmanager_statename', '    default:\n      return true;\n  }\n}\n\nstatic const char* estopmanager_statename', [TEST]),
  ('K55 panic drops off the warm list', ES, '    case ESP_RST_PANIC:\n', '', [TEST]),
  ('K56 Set passes armed true at the call site', SC, 'CapRiseAllowed(EStopManager::IsArmed(), EStopManager::GetLatchSource())', 'CapRiseAllowed(true, EStopManager::GetLatchSource())', [SC]),
  ('K57 one high sample records a button', ES, 'estopmanager_engage(Softproxy::EStopLatchSource::Unconfirmed);', 'estopmanager_engage(Softproxy::EStopLatchSource::Button);', [TEST]),
  ('K58 confirm needs no full window', ES, 'if (confirming && solidStop) {', 'if (confirming) {', [TEST]),
  ('K59 a later press confirms a glitch', ES, '    if (!stopSeen) {\n      confirming = false;\n    }\n', '', [TEST]),
  ('K61 one high sample keeps the clear running', ES, '        if (stopSeen) {\n          state = EStopState::Active;\n        } else if (--clearSamplesLeft == 0) {', '        if (solidStop) {\n          state = EStopState::Active;\n        } else if (--clearSamplesLeft == 0) {', [TEST]),
  ('K62 a push never stops the clear', ES, '        if (stopSeen) {\n          state = EStopState::Active;\n        } else if (--clearSamplesLeft == 0) {', '        if (false) {\n          state = EStopState::Active;\n        } else if (--clearSamplesLeft == 0) {', [TEST]),
  ('K63 clear needs no measured down', ES, '} else if (sawDown && solidRun) {', '} else if (solidRun) {', [TEST]),
  ('K64 clear skips the clear time', ES, '} else if (--clearSamplesLeft == 0) {', '} else {', [TEST]),
  ('K65 engaged hub drops a gateway stop', TR, ' && triggerType != TriggerType::EmergencyStop', '', [TR]),
  ('K67 gateway stop keeps the measured down', ES, '      sawDown = false;  // Stops a clear in progress. The next clear needs the mushroom measured down again.\n', '', [TEST]),
  ('K68 Idle ignores the stored keep-alive', CH, 'if (keepAlive && !Config::GetRFConfigKeepAliveEnabled(keepAlive)) {', 'if (false) {', [CH]),
  ('K69 lock skips an unconfirmed latch', KP, LOCK, 'return anyCapSet || source == EStopLatchSource::Button || source == EStopLatchSource::Gateway;', HEADER),
  ('K70 unconfirmed allows a rise', KP, RISE, 'return estopArmed && (source == EStopLatchSource::Button || source == EStopLatchSource::Unconfirmed);', HEADER),
  ('K71 restore drops an unconfirmed latch', KP, ', EStopLatchSource::Unconfirmed}) {', '}) {', HEADER),
  ('K72 an open pin at task start confirms', ES, 'confirming = sawRun;', 'confirming = true;', [TEST]),
  ('K73 the unmeasured start counts as a run', ES, 'bool solidRun         = window == 0 && samplesRead == k_estopCheckCount;', 'bool solidRun         = window == 0;', [TEST]),
  ('K74 a confirmed press needs no measured run', ES, 'sawRun                = sawRun || solidRun;', 'sawRun                = true;', [TEST]),
  ('K75 terminators forced only on the change', RF, '    wasEstopped     = isEstopped;\n\n    if (isEstopped) {', '    const bool changed = isEstopped != wasEstopped;\n    wasEstopped     = isEstopped;\n\n    if (isEstopped && changed) {', [RF]),
  ('K76 a forced end restarts a running terminator', RF, 'if (seq->transmitEnd() > now) {', 'if (true) {', [RF]),
  ('K77 GPIO 10 stays on the safe list', CHIPSET, '(pin) == GPIO_NUM_9 || (pin) == GPIO_NUM_10 \\\n   || (pin) == GPIO_NUM_11', '(pin) == GPIO_NUM_9 \\\n   || (pin) == GPIO_NUM_11', []),
  ('K79 the contact reads as normally open', ES, '== k_estopRunLevel ? 0 : 1;', '== k_estopRunLevel ? 1 : 0;', [TEST]),
  ('K80 one high sample engages nothing', ES, '        // A high level at any time engages. There is no rearm window.\n        if (stopSeen) {', '        // A high level at any time engages. There is no rearm window.\n        if (solidStop) {', [TEST]),
  ('K81 one high sample counts as the mushroom down', ES, 'bool solidStop        = window == k_estopCheckMask;', 'bool solidStop        = window != 0;', [TEST]),
  ('K82 release keeps the measured down', ES, ' : EStopState::Active;\n          sawDown = false;\n', ' : EStopState::Active;\n', [TEST]),
  ('K84 estop rule skips the reserved pins', KP, ' || IsEStopReservedPin(estopPin)', '', HEADER),
  ('K85 reserved pins only off a plain ESP32', KP, '    if (!kChipIsEsp32) {\n      return false;', '    if (kChipIsEsp32) {\n      return false;', HEADER),
  ('K86 GPIO 0 allowed', KP, RESERVED, RESERVED.replace('pin == 0 || ', ''), HEADER),
  ('K87 GPIO 2 allowed', KP, RESERVED, RESERVED.replace('pin == 2 || ', ''), HEADER),
  ('K88 GPIO 5 allowed', KP, RESERVED, RESERVED.replace('pin == 5 || ', ''), HEADER),
  ('K89 GPIO 12 allowed', KP, RESERVED, RESERVED.replace('pin == 12 || ', ''), HEADER),
  ('K90 GPIO 15 allowed', KP, RESERVED, RESERVED.replace('pin == 15 || ', ''), HEADER),
  ('K91 flash pins allowed', KP, RESERVED, RESERVED.replace(' || (pin >= 6 && pin <= 11)', ''), HEADER),
  ('K92 GPIO 6 allowed', KP, RESERVED, RESERVED.replace('pin >= 6 &&', 'pin >= 7 &&'), HEADER),
  ('K93 GPIO 11 allowed', KP, RESERVED, RESERVED.replace('pin <= 11', 'pin <= 10'), HEADER),
  ('K94 the pull-up comes off the E-Stop pin', ES, '.pull_up_en   = GPIO_PULLUP_ENABLE,', '.pull_up_en   = GPIO_PULLUP_DISABLE,', [TEST]),
  ('K95 confirm skips the unconfirmed guard', ES, '  if (s_latch.load() == Softproxy::EStopLatchSource::Unconfirmed) {', '  if (true) {', [TEST]),
  ('K96 release ignores an untaken gateway stop', ES, 'const bool released = s_gatewayStops == gatewayStopsTaken;', 'const bool released = true;', [TEST]),
  ('K97 Trigger does not count the stop', ES, '  estopmanager_countgatewaystop();\n  if (!IsEStopped()) {', '  if (!IsEStopped()) {', [TEST]),
  ('K98 the task never takes a gateway stop', ES, '      gatewayStopsTaken     = estopmanager_gatewaystops();\n', '', [TEST]),
  ('K99 a refused release reads as idle', ES, '? EStopState::Idle : EStopState::Active;', '? EStopState::Idle : EStopState::Idle;', [TEST]),
  ('K100 the clear ends on wall time', ES, '} else if (--clearSamplesLeft == 0) {', CLEAR_ON_WALL_TIME, [TEST]),
  ('X1 Trigger counts after it engages', ES, '  estopmanager_countgatewaystop();\n  if (!IsEStopped()) {\n    s_estopActivatedAt = OpenShock::millis();\n  }\n  estopmanager_engage(Softproxy::EStopLatchSource::Gateway);\n', '  if (!IsEStopped()) {\n    s_estopActivatedAt = OpenShock::millis();\n  }\n  estopmanager_engage(Softproxy::EStopLatchSource::Gateway);\n  estopmanager_countgatewaystop();\n', [TEST]),
  ('X2 task reads the count before it clears the flag', ES, '      s_externallyTriggered = false;\n      gatewayStopsTaken     = estopmanager_gatewaystops();\n', '      gatewayStopsTaken     = estopmanager_gatewaystops();\n      s_externallyTriggered = false;\n', [TEST]),
  ('X6 task start takes no stop', ES, '  uint32_t gatewayStopsTaken = estopmanager_gatewaystops();', '  uint32_t gatewayStopsTaken = 0;', [TEST]),
  ('X8 release reads the count outside the lock', ES, '  portENTER_CRITICAL(&s_latchMux);\n  const bool released = s_gatewayStops == gatewayStopsTaken;', '  const bool released = estopmanager_gatewaystops() == gatewayStopsTaken;\n  portENTER_CRITICAL(&s_latchMux);', [TEST]),
  ('X10 taking a stop keeps confirming', ES, '      sawDown = false;  // Stops a clear in progress.', '      sawDown = false; confirming = false;  // Stops a clear in progress.', [TEST]),
  ('X12 estop enabled skips the pin rule', ES, 'if (!estopmgr_pinallowed(pin) || !estopmgr_set_pin_impl(pin)) {', 'if (!estopmgr_set_pin_impl(pin)) {', [TEST]),
  ('X13 a restarted task starts idle', ES, 'state          = startedEngaged ? EStopState::Active : EStopState::Idle;', 'state          = EStopState::Idle;', [TEST]),
  ('X14 a failed tx read allows the pin', ES, '    OS_LOGE(TAG, "Failed to get RF TX pin from config");\n    return false;', '    OS_LOGE(TAG, "Failed to get RF TX pin from config");\n    return true;', [TEST]),
  ('C1 SaveFromJSON skips the kill gate', CONFIG, 'if (!candidate.FromJSON(root) || !_killConfigChangeAllowed(candidate)) {', 'if (!candidate.FromJSON(root)) {', [CONFIG]),
  ('C2 SaveFromFlatBuffer skips the kill gate', CONFIG, 'if (!candidate.FromFlatbuffers(config) || !_killConfigChangeAllowed(candidate)) {', 'if (!candidate.FromFlatbuffers(config)) {', [CONFIG]),
  ('C3 SetRaw skips the kill gate', CONFIG, '  if (!_killConfigChangeAllowed(config)) {\n    return false;\n  }\n', '', [CONFIG]),
  ('P1 E-Stop parser takes a negative pin', ESTOP_CONFIG, NEGATIVE_PIN, 'if (false) {', [ESTOP_CONFIG]),
  ('P2 E-Stop parser refuses no pin', ESTOP_CONFIG, NEGATIVE_PIN, 'if (pin < 0) {', [ESTOP_CONFIG]),
  ('P3 E-Stop parser takes the lowest negative pins', ESTOP_CONFIG, NEGATIVE_PIN, 'if (pin < 0 && pin != OPENSHOCK_GPIO_INVALID && pin > -64) {', [ESTOP_CONFIG]),
  ('P4 E-Stop parser enables an invalid pin', ESTOP_CONFIG, 'if (OpenShock::IsValidInputPin(static_cast<int8_t>(gpioPin))) {', 'if (true) {', [ESTOP_CONFIG]),
  ('P5 RF parser drops the flatbuffer pin', RF_CONFIG, 'Internal::Utils::FromU8GpioNum(txPin, config->tx_pin(), static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO));', '', [RF_CONFIG]),
  ('P6 RF parser drops the JSON pin', RF_CONFIG, 'Internal::Utils::FromJsonGpioNum(txPin, json, "txPin", static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO));', '', [RF_CONFIG]),
  ('P7 E-Stop parser drops the JSON pin', ESTOP_CONFIG, 'Internal::Utils::FromJsonGpioNum(gpioPin, json, "gpioPin", static_cast<gpio_num_t>(OPENSHOCK_ESTOP_PIN));', '', [ESTOP_CONFIG]),
  ('P8 E-Stop parser takes an enabled of the wrong type', ESTOP_CONFIG, 'OS_LOGE(TAG, "Failed to parse enabled");\n    return false;', 'OS_LOGE(TAG, "Failed to parse enabled");', [ESTOP_CONFIG]),
  ('P9 a pin outside the ESP32 mask passes', CONFIG_UTILS, 'if (u8Val >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(u8Val)) {', 'if (u8Val >= GPIO_NUM_MAX) {', [CONFIG_UTILS]),
  ('K78 boot never starts the keep-alive', CH, 'if (rfConfig.keepAliveEnabled && !EStopManager::IsEStopped()) {', 'if (false) {', [CH]),
  ('R4 RF task stops discarding while engaged', RF, 'if (OpenShock::EStopManager::IsEStopped()) {\n        // Immediately', 'if (false) {\n        // Immediately', [RF]),
  ('R4b RF task adds what it should discard', RF, '        // Discard next item in queue\n        continue;\n', '', [RF]),
  ('R9 restore ignores the magic', KP, 'in.magic != kRetainedEStopMagic || ', '', HEADER),
  ('R32 IsArmed drops the valid pin check', ES, 'if (s_estopTask == nullptr || !OpenShock::IsValidInputPin(s_estopPin)) {\n    return false;\n  }\n\n  // Read the sample', 'if (s_estopTask == nullptr) {\n    return false;\n  }\n\n  // Read the sample', [TEST]),
  ('R37 boot keep-alive ignores engaged', CH, 'if (rfConfig.keepAliveEnabled && !EStopManager::IsEStopped()) {', 'if (rfConfig.keepAliveEnabled) {', [CH]),
  ('R38 keep-alive stays on while engaged', CH, 'bool keepAlive = state == EStopState::Idle;', 'bool keepAlive = true;', [CH]),
  ('R52 engaged hub takes every trigger', TR, GATE, 'if (false) {', [TR]),
]


def parse_args():
  parser = argparse.ArgumentParser(description='Mutation run for the kill path host test.')
  parser.add_argument('--only', nargs='*', default=[], help='mutant ids to run, such as K11 R4')
  parser.add_argument('--budget', type=int, default=0, help='stop after this many seconds, 0 for no limit')
  parser.add_argument('--work', default='.pio/kill-path-mutants', help='work directory, outside the tracked tree')
  parser.add_argument('--flatbuffers', default='.pio/libdeps/Pishock-2023/flatbuffers/include', help='flatbuffers include directory')
  parser.add_argument('--tx', type=int, default=12, help='RF TX pin to build for')
  return parser.parse_args()


class Run:
  def __init__(self, args):
    self.args = args
    self.work = os.path.abspath(args.work)
    self.results_path = os.path.join(self.work, 'results.json')
    board = ['-DOPENSHOCK_FW_CHIP_ESP32', '-DOPENSHOCK_LED_GPIO=2', '-DSOFTPROXY_PISHOCK_BUILD', '-DOPENSHOCK_RF_TX_GPIO=%d' % args.tx, '-w']
    self.flags = ['-std=c++20', '-O1', '-pthread', '-DSOFTPROXY_KILL_TEST'] + board
    self.includes = ['-Itest/native/kill_stubs', '-Itest/native/stubs', '-Iinclude', '-Iflatbuffers']
    self.config_flags = ['-std=c++20', '-O1'] + board
    self.config_includes = ['-Itest/native/config_stubs', '-Iinclude', '-Iflatbuffers']

  def object_path(self, unit):
    return 'obj/%s.o' % unit.replace('/', '_')

  def compile(self, unit):
    flags, includes = (self.config_flags, self.config_includes) if unit in CONFIG_UNITS else (self.flags, self.includes)
    return subprocess.run(['g++'] + flags + includes + ['-c', unit, '-o', self.object_path(unit)], cwd=self.work, capture_output=True, text=True)

  def link(self):
    kill = subprocess.run(['g++', '-pthread', '-Wl,--wrap=malloc'] + [self.object_path(unit) for unit in UNITS] + ['-o', 'kill_path_test'], cwd=self.work, capture_output=True, text=True)
    if kill.returncode:
      return kill
    return subprocess.run(['g++'] + [self.object_path(unit) for unit in CONFIG_UNITS] + ['-o', 'config_writers_test'], cwd=self.work, capture_output=True, text=True)

  def test(self):
    states, details = [], []
    for binary in ('./kill_path_test', './config_writers_test'):
      try:
        result = subprocess.run([binary], cwd=self.work, capture_output=True, text=True, timeout=120)
      except subprocess.TimeoutExpired:
        states.append('red-hang')
        details.append('%s hangs' % binary[2:])
        continue
      states.append('green' if result.returncode == 0 else 'red')
      failed = [line.split(': ', 1)[1] for line in result.stdout.splitlines() if line.startswith('scenario failed')]
      if result.returncode:
        details.append(', '.join(failed) or '%s rc=%d' % (binary[2:], result.returncode))
    state = 'red-hang' if 'red-hang' in states else ('red' if 'red' in states else 'green')
    return state, ', '.join(details)[:200]

  def tree_stamp(self):
    digest = hashlib.sha256()
    for path in sorted({TEST, CONFIG, CHIPSET, KP, ES, SERIAL_CAPS, *UNITS, *CONFIG_UNITS}):
      with open(path, 'rb') as handle:
        digest.update(handle.read())
    return digest.hexdigest()[:16]

  def load_results(self):
    stamp = self.tree_stamp()
    if os.path.exists(self.results_path):
      with open(self.results_path) as handle:
        results = json.load(handle)
      if results.get('_tree') == stamp and os.path.isfile(os.path.join(self.work, 'kill_path_test')):
        return results
    return self.prepare(stamp)

  def save(self, results):
    with open(self.results_path, 'w') as handle:
      json.dump(results, handle, indent=1)

  def prepare(self, stamp):
    shutil.rmtree(self.work, ignore_errors=True)
    os.makedirs(os.path.join(self.work, 'obj'))
    for directory in ('src', 'include', 'test'):
      shutil.copytree(directory, os.path.join(self.work, directory), symlinks=True)
    shutil.copytree(self.args.flatbuffers, os.path.join(self.work, 'flatbuffers'))
    for unit in UNITS + CONFIG_UNITS:
      result = self.compile(unit)
      if result.returncode:
        print('control does not compile: %s\n%s' % (unit, result.stderr[:1500]))
        sys.exit(2)
    result = self.link()
    if result.returncode:
      print('control does not link\n%s' % result.stderr[:1500])
      sys.exit(2)
    state, detail = self.test()
    if state != 'green':
      print('control is not green: %s %s' % (state, detail))
      sys.exit(2)
    print('control green', flush=True)
    results = {'_tree': stamp}
    self.save(results)
    return results

  def mutate(self, mutant):
    name, path, old, new, units = mutant
    full = os.path.join(self.work, path)
    with open(full) as handle:
      original = handle.read()
    if old not in original:
      return 'no-match', ''
    try:
      with open(full, 'w') as handle:
        handle.write(original.replace(old, new, 1))
      failed = [(unit, result) for unit, result in ((unit, self.compile(unit)) for unit in units) if result.returncode]
      # A static_assert that refuses the mutant, such as the K29 guard in KillPath.h, refuses it in the firmware build
      # as well, so that counts as red. Any other compile error means the mutant is broken.
      if failed and all('static assertion failed' in result.stderr for _, result in failed):
        return 'red-static-assert', ', '.join(unit for unit, _ in failed)
      if failed:
        return 'compile-error', ', '.join(unit for unit, _ in failed)
      if self.link().returncode:
        return 'link-error', ''
      return self.test()
    finally:
      with open(full, 'w') as handle:
        handle.write(original)
      for unit in units:
        self.compile(unit)


def main():
  args = parse_args()
  chosen = [mutant for mutant in MUTANTS if not args.only or mutant[0].split()[0] in args.only]
  unknown = set(args.only) - {mutant[0].split()[0] for mutant in MUTANTS}
  if unknown:
    print('unknown mutant ids: %s' % ', '.join(sorted(unknown)))
    return 2

  run = Run(args)
  started = time.time()
  results = run.load_results()

  stopped = False
  for mutant in chosen:
    name = mutant[0]
    if name in results:
      continue
    if args.budget and time.time() - started > args.budget:
      print('stopped: budget reached, run it again')
      stopped = True
      break
    state, detail = run.mutate(mutant)
    results[name] = [state, detail]
    run.save(results)
    print('%s => %s %s' % (name, state, detail[:90]), flush=True)

  finished = [mutant[0] for mutant in chosen if mutant[0] in results]
  red = [name for name in finished if results[name][0].startswith('red')]
  survived = [name for name in finished if results[name][0] == 'green']
  broken = [name for name in finished if name not in red and name not in survived]
  print('%d of %d chosen mutants ran: %d red, %d survived, %d broken' % (len(finished), len(chosen), len(red), len(survived), len(broken)))
  for name in survived + broken:
    print('  %s: %s' % (name, results[name][0]))
  if broken:
    return 2
  if survived:
    return 1
  return 75 if stopped else 0


if __name__ == '__main__':
  sys.exit(main())
