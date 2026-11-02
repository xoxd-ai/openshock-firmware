#!/usr/bin/env python3
# Mutation run for test/native/test_kill_path.cpp (TIN-7545). Each mutant breaks one line of the kill path in a copy of
# the tree. The test must then go red. A mutant that stays green is a gap in the test, and the run fails.
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
ENCODERS = ['src/radio/rmt/%sEncoder.cpp' % name for name in ('CaiXianlin', 'D80', 'Petrainer998DR', 'Petrainer', 'T330')]
UNITS = [TEST, CH, SC, DF, RF, 'src/radio/rmt/Sequence.cpp'] + ENCODERS + ['src/serial/command_handlers/estop.cpp', RFTXPIN, TR, 'src/util/StringUtils.cpp']

# A header reaches these units. EStopManager.cpp and safetycaps.cpp compile inside the test.
HEADER = [TEST, CH, SC]

RISE = 'return estopArmed && source == EStopLatchSource::Button;'
LOCK = 'return anyCapSet || source == EStopLatchSource::Button || source == EStopLatchSource::Gateway || source == EStopLatchSource::Unconfirmed;'
WARM = '    case ESP_RST_SW:\n'
RESERVED = 'return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15 || (pin >= 6 && pin <= 11);'
GATE = 'if (EStopManager::IsEStopped() && triggerType != TriggerType::EmergencyStop) {'

# Equivalent mutants, left out of the run. K29 drops `estopPin == kBoardRfTxPin` from `EStopPinConflicts`. On both
# PiShock builds the board RF TX pin is 12 or 15, and `IsEStopReservedPin` refuses both, so no input tells the two apart.
# pinsStaySeparate checks that both pins are reserved. A build whose board RF TX pin is not reserved needs K29 back.
EQUIVALENT = [
  ('K29 estop rule ignores the board tx pin', KP, 'estopPin == txPin || estopPin == kBoardRfTxPin', 'estopPin == txPin', HEADER),
]

# Name, file, text to replace, replacement, units to rebuild. An empty unit list means the test reads the file itself.
MUTANTS = [
  ('K1 HandleCommand ignores not armed', CH, '!EStopManager::IsArmed())', 'false)', [CH]),
  ('K2 HandleCommand ignores engaged', CH, 'if (EStopManager::IsEStopped()) {\n    Softproxy::SafetyCaps::RecordRefusal', 'if (false) {\n    Softproxy::SafetyCaps::RecordRefusal', [CH]),
  ('K3 task start clears the latch', ES, 'const bool startedEngaged = EStopManager::IsEStopped();', 'estopmanager_release(); const bool startedEngaged = false;', [TEST]),
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
  ('K45 a clear under way records a button', ES, '          state    = EStopState::ActiveClearing;\n', '          state    = EStopState::ActiveClearing;\n          s_latch.store(Softproxy::EStopLatchSource::Button);\n', [TEST]),
  ('K46 engage restates the source', ES, '  if (s_latch.load() == Softproxy::EStopLatchSource::Released) {\n    estopmanager_writelatch(source);\n  }', '  estopmanager_writelatch(source);', [TEST]),
  ('K47 latch not sealed on engage', ES, '  s_latch.store(source);\n  Softproxy::SealEStop(s_retainedLatch, source);', '  s_latch.store(source);\n  if (source == Softproxy::EStopLatchSource::Released) Softproxy::SealEStop(s_retainedLatch, source);', [TEST]),
  ('K48 shorter join window is no rise', KP, 'to.stageJoinMs != from.stageJoinMs', 'to.stageJoinMs > from.stageJoinMs', HEADER),
  ('K49 Config lock reads engaged only', CONFIG, 'OpenShock::Softproxy::EStopSettingsLocked(OpenShock::EStopManager::GetLatchSource(), OpenShock::Softproxy::SafetyCaps::AnySet())', 'OpenShock::EStopManager::IsEStopped() || OpenShock::Softproxy::SafetyCaps::AnySet()', []),
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
  ('K61 one high sample keeps the clear running', ES, '        if (stopSeen) {\n          state = EStopState::Active;\n        } else if (now >= clearsAt) {', '        if (solidStop) {\n          state = EStopState::Active;\n        } else if (now >= clearsAt) {', [TEST]),
  ('K62 a push never stops the clear', ES, '        if (stopSeen) {\n          state = EStopState::Active;\n        } else if (now >= clearsAt) {', '        if (false) {\n          state = EStopState::Active;\n        } else if (now >= clearsAt) {', [TEST]),
  ('K63 clear needs no measured down', ES, '} else if (sawDown && solidRun) {', '} else if (solidRun) {', [TEST]),
  ('K64 clear skips the clear time', ES, '} else if (now >= clearsAt) {', '} else {', [TEST]),
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
  ('K82 release keeps the measured down', ES, '          sawDown = false;\n          estopmanager_release();', '          estopmanager_release();', [TEST]),
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
    self.flags = ['-std=c++20', '-O1', '-pthread', '-DSOFTPROXY_KILL_TEST', '-DOPENSHOCK_FW_CHIP_ESP32', '-DOPENSHOCK_LED_GPIO=2', '-DOPENSHOCK_RF_TX_GPIO=%d' % args.tx, '-w']
    self.includes = ['-Itest/native/kill_stubs', '-Itest/native/stubs', '-Iinclude', '-Iflatbuffers']

  def object_path(self, unit):
    return 'obj/%s.o' % unit.replace('/', '_')

  def compile(self, unit):
    return subprocess.run(['g++'] + self.flags + self.includes + ['-c', unit, '-o', self.object_path(unit)], cwd=self.work, capture_output=True, text=True)

  def link(self):
    return subprocess.run(['g++', '-pthread', '-Wl,--wrap=malloc'] + [self.object_path(unit) for unit in UNITS] + ['-o', 'kill_path_test'], cwd=self.work, capture_output=True, text=True)

  def test(self):
    try:
      result = subprocess.run(['./kill_path_test'], cwd=self.work, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
      return 'red-hang', ''
    failed = [line.split(': ', 1)[1] for line in result.stdout.splitlines() if line.startswith('scenario failed')]
    return ('green' if result.returncode == 0 else 'red'), (', '.join(failed)[:200] or 'rc=%d' % result.returncode)

  def tree_stamp(self):
    digest = hashlib.sha256()
    for path in sorted({TEST, CONFIG, CHIPSET, KP, ES, SERIAL_CAPS, *UNITS}):
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
    for unit in UNITS:
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
      failed = [unit for unit in units if self.compile(unit).returncode]
      if failed:
        return 'compile-error', ', '.join(failed)
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
