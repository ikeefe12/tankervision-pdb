#include "BoardHardware.h"
#include "PowerDelivery.h"
#include "USBCDC.h"
#include "USB.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d at%u ms: %s\n", __func__, __LINE__, millis(), #condition); \
  std::exit(1); } } while (0)

namespace AppMock {
uint32_t now = 0, ceAt = 0, releaseAt = 0, restartAt = 0;
bool connected = true, rebootEnabled = true, main = true;
bool boost = false, ce = false, backup = false, led = false, buck = false;
bool released = false, restoreCompleted = false;
bool temperatureFault = false, bootPressed = false;
unsigned ports = 0, restartCalls = 0, hardwareCalls = 0, pdCalls = 0;
unsigned reconciles = 0, prepares = 0, enables = 0, disables = 0, budgetRequests = 0;
float cap = 5.0f;
std::deque<uint8_t> rx;
std::string tx;
std::vector<std::pair<uint32_t, bool>> leds;
std::vector<std::pair<uint32_t, std::string>> operations;
struct PortRequest { uint32_t at; Port port; bool enabled; };
std::vector<PortRequest> portRequests;
void access(const char *name, bool pd = false) {
  if (released) { std::fprintf(stderr, "Access after release: %s\n", name); std::exit(1); }
  if (pd) ++pdCalls; else ++hardwareCalls;
}
void operation(const char *name) {
  access(name); operations.emplace_back(now, name);
  CHECK(now >= 10000); // During settling, only monitoring/LED reads are allowed.
}
}

USBMock USB;
uint32_t millis() { return AppMock::now; }
void delay(uint32_t milliseconds) { AppMock::now += milliseconds; }
uint32_t esp_random() { return 0xA1B2C3D4; }
unsigned esp_reset_reason() { return 1; }
void esp_restart() { ++AppMock::restartCalls; AppMock::restartAt = millis(); }
int esp_read_mac(uint8_t *mac, unsigned) {
  const uint8_t value[6] = {0x68, 0xEE, 0x8F, 0x58, 0xB0, 0x10};
  std::memcpy(mac, value, sizeof(value)); return 0;
}

bool BoardHardware::beginMonitoring() {
  AppMock::access("beginMonitoring"); monitoring_ = true;
  AppMock::backup = false; AppMock::led = true;
  snapshot_.internal.config[0] = snapshot_.internal.config[1] = 0xff;
  snapshot_.external.config[0] = snapshot_.external.config[1] = 0xff;
  return true;
}
const char *BoardHardware::adcName(unsigned index) {
  static const char *names[] = {"ts", "vcap", "dc", "j8", "j7", "j9", "j10", "usb", "backup"};
  return index < 9 ? names[index] : "unknown";
}
bool BoardHardware::poll() {
  AppMock::access("board.poll");
  if (AppMock::ce) AppMock::cap = std::min(20.4f, 5.0f + float(millis() - AppMock::ceAt) * 0.02f);
  snapshot_.atMs = snapshot_.internal.atMs = snapshot_.external.atMs = millis();
  snapshot_.valid = snapshot_.internal.valid = snapshot_.external.valid = true;
  snapshot_.mainSelected = snapshot_.usbPg = AppMock::main;
  snapshot_.backupEnabled = snapshot_.backupPg = AppMock::backup;
  snapshot_.ssBuckPg = AppMock::main || AppMock::backup;
  snapshot_.boostPg = snapshot_.chargerPg = AppMock::boost && AppMock::main;
  snapshot_.chargerStat = true; snapshot_.buckPg = AppMock::buck;
  snapshot_.statusLed = AppMock::led; snapshot_.vcapV = AppMock::cap;
  snapshot_.tsMv = AppMock::boost && AppMock::main ? (AppMock::temperatureFault ? 1200 : 1935) : 0;
  snapshot_.bootButtonLow = AppMock::bootPressed;
  snapshot_.extVbusPg = AppMock::ports & 2;
  snapshot_.ext5vVbusPg = AppMock::ports & 1;
  snapshot_.extSsPg = snapshot_.jetsonOn = AppMock::ports & 4;
  snapshot_.ext5vSsPg = AppMock::ports & 8;
  const unsigned control = (AppMock::boost ? 1 : 0) | (AppMock::ce ? 2 : 0) | (AppMock::buck ? 16 : 0);
  snapshot_.internal.physicalInputs = uint16_t(control << 8);
  snapshot_.external.physicalInputs = uint16_t(AppMock::ports << 8);
  if (!AppMock::restoreCompleted) {
    snapshot_.internal.output[1] = control;
    snapshot_.external.output[1] = AppMock::ports;
  }
  snapshot_.internal.input[1] = control;
  snapshot_.external.input[1] = AppMock::ports;
  for (unsigned i = 0; i < 9; ++i) {
    snapshot_.adc[i].gpio = i + 1; snapshot_.adc[i].raw = 100 + i;
    snapshot_.adc[i].mv = i == 0 ? snapshot_.tsMv : 1200;
    snapshot_.adc[i].currentA = i < 2 ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
  }
  return true;
}
bool BoardHardware::beginOperation(Operation op, bool) {
  operation_ = op; operationAt_ = millis(); result_ = Result::Running; return true;
}
bool BoardHardware::startReconcile() {
  AppMock::operation("reconcile"); ++AppMock::reconciles;
  return beginOperation(Operation::Reconcile);
}
bool BoardHardware::startPrepareCharge() {
  AppMock::operation("prepare"); ++AppMock::prepares;
  CHECK(reconciled_); CHECK(AppMock::budgetRequests > 0);
  return beginOperation(Operation::PrepareCharge);
}
bool BoardHardware::startEnableCharge() {
  AppMock::operation("enable_charge"); ++AppMock::enables;
  CHECK(prepared_); return beginOperation(Operation::EnableCharge);
}
bool BoardHardware::startDisableCharging() {
  AppMock::operation("disable_charge"); ++AppMock::disables;
  AppMock::ce = false; return beginOperation(Operation::DisableCharging, true);
}
bool BoardHardware::startPort(Port port, bool enabled) {
  AppMock::operation("port"); CHECK(reconciled_);
  AppMock::portRequests.push_back({millis(), port, enabled});
  port_ = port; portEnabled_ = enabled; return beginOperation(Operation::PortChange);
}
bool BoardHardware::startRestorePor() {
  AppMock::operation("restore_por");
  AppMock::ce = false; return beginOperation(Operation::RestorePor, true);
}
void BoardHardware::service() {
  AppMock::access("board.service");
  if (!busy()) return;
  const uint32_t duration = operation_ == Operation::PrepareCharge ? 600
      : operation_ == Operation::DisableCharging || operation_ == Operation::RestorePor ? 100 : 20;
  if (millis() - operationAt_ < duration) return;
  switch (operation_) {
    case Operation::Reconcile:
      AppMock::ce = AppMock::boost = AppMock::buck = false; AppMock::ports = 0;
      snapshot_.internal.config[0] = snapshot_.external.config[0] = 0xff;
      snapshot_.internal.config[1] = snapshot_.external.config[1] = 0xe0;
      reconciled_ = true; break;
    case Operation::PrepareCharge:
      AppMock::boost = true; prepared_ = true;
      // The real preparation job samples PG/TS to verify its completion.
      poll(); break;
    case Operation::EnableCharge: AppMock::ce = true; AppMock::ceAt = millis(); break;
    case Operation::DisableCharging: AppMock::ce = AppMock::boost = false; break;
    case Operation::PortChange: {
      const unsigned masks[] = {2, 1, 4, 8};
      if (portEnabled_) AppMock::ports |= masks[unsigned(port_)];
      else AppMock::ports &= ~masks[unsigned(port_)];
      if (port_ == Port::FiveVoltVbus) AppMock::buck = portEnabled_;
      break;
    }
    case Operation::RestorePor:
      AppMock::ports = 0; AppMock::boost = AppMock::ce = AppMock::buck = false;
      snapshot_.internal.config[0] = snapshot_.internal.config[1] = 0xff;
      snapshot_.external.config[0] = snapshot_.external.config[1] = 0xff;
      snapshot_.internal.output[0] = snapshot_.internal.output[1] = 0xff;
      snapshot_.external.output[0] = snapshot_.external.output[1] = 0xff;
      AppMock::restoreCompleted = true; break;
    case Operation::None: break;
  }
  result_ = Result::Succeeded;
}
bool BoardHardware::setBackup(bool enabled) {
  AppMock::operation("backup"); CHECK(AppMock::main);
  AppMock::backup = enabled; return true;
}
bool BoardHardware::setLed(bool enabled) {
  AppMock::access("led");
  if (AppMock::led != enabled) AppMock::leds.emplace_back(millis(), enabled);
  if (millis() < 10000) CHECK(enabled);
  AppMock::led = enabled; return true;
}
bool BoardHardware::releaseBackupForShutdown() {
  AppMock::operation("release"); CHECK(AppMock::restoreCompleted);
  CHECK(!AppMock::ports && !AppMock::ce && !AppMock::boost && !AppMock::buck);
  AppMock::backup = false; AppMock::released = true; AppMock::releaseAt = millis(); return true;
}

bool PowerDelivery::begin() { AppMock::access("pd.begin", true); begun_ = true; return true; }
bool PowerDelivery::poll(bool mainPresent) {
  AppMock::access("pd.poll", true);
  reading_.valid = mainPresent; reading_.atMs = millis(); reading_.typeC = mainPresent ? 1 : 0;
  reading_.pdo = (400u << 10) | 300u; reading_.rdo = (300u << 10) | 300u;
  return mainPresent;
}
bool PowerDelivery::startBudget() {
  AppMock::access("pd.startBudget", true); CHECK(millis() >= 10000);
  ++AppMock::budgetRequests; result_ = Result::Running; startAt_ = millis(); return true;
}
void PowerDelivery::service() {
  AppMock::access("pd.service", true);
  if (result_ == Result::Running && millis() - startAt_ >= 30) {
    reading_.atMs = millis(); result_ = Result::Succeeded;
  }
}
bool PowerDelivery::matches(const PdReading &reading) const {
  return reading.attached() && reading.millivolts() == 20000 && reading.operatingMa() >= kRequestedMa;
}

void setupDeployment();
void loopDeployment();
void runUntil(uint32_t until) {
  while (millis() < until && !AppMock::restartCalls) loopDeployment();
}
void send(const std::string &request) {
  for (unsigned char byte : request + "\n") AppMock::rx.push_back(byte);
  runUntil(millis() + 100);
}
void response(unsigned id, bool ok, const char *code) {
  const std::string expected = "\"id\":" + std::to_string(id) + ",\"ok\":" + (ok ? "true" : "false") + ",\"code\":\"" + code + "\"";
  CHECK(AppMock::tx.find(expected) != std::string::npos);
}
uint32_t lastNumber(const char *key) {
  const size_t start = AppMock::tx.rfind(key); CHECK(start != std::string::npos);
  return uint32_t(std::stoul(AppMock::tx.substr(start + std::strlen(key))));
}
std::string request(unsigned id, const char *command, const std::string &extra = "") {
  return "{\"v\":1,\"id\":" + std::to_string(id) + ",\"cmd\":\"" + command + "\",\"boot_id\":\"A1B2C3D4\"" + extra + "}";
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  const std::string scenario = argv[1];
  setupDeployment(); CHECK(!AppMock::rebootEnabled);
  runUntil(9999);
  CHECK(AppMock::operations.empty()); CHECK(!AppMock::budgetRequests); CHECK(AppMock::led);
  runUntil(12500);
  if (AppMock::reconciles != 1 || AppMock::prepares != 1 || AppMock::enables != 1) {
    for (const auto &op : AppMock::operations)
      std::fprintf(stderr, "operation@%u: %s\n", op.first, op.second.c_str());
    std::fprintf(stderr, "counts reconcile=%u prepare=%u enable=%u budget=%u\n",
                 AppMock::reconciles, AppMock::prepares, AppMock::enables, AppMock::budgetRequests);
  }
  CHECK(AppMock::reconciles == 1 && AppMock::prepares == 1 && AppMock::enables == 1);
  CHECK(AppMock::boost && AppMock::ce && AppMock::backup && (AppMock::ports & 4));
  CHECK(AppMock::disables == 0);
  CHECK(AppMock::tx.find("\"state\":\"RUNNING\"") != std::string::npos);
  CHECK(!AppMock::leds.empty());

  send(R"({"v":1,"id":100,"cmd":"status"})"); response(100, true, "OK");
  const uint32_t ping = lastNumber("\"event\":\"ping\",\"ping_id\":");
  send(request(101, "pong", ",\"ping_id\":" + std::to_string(ping))); response(101, true, "OK");
  send(request(102, "pong", ",\"ping_id\":0")); response(102, false, "STALE_PING");
  send(request(103, "port_set", ",\"port\":\"vbus_ss\",\"enabled\":false")); response(103, false, "RESERVED_PORT");
  send(request(104, "port_set", ",\"port\":\"5v_vbus\",\"enabled\":true")); response(104, true, "ACCEPTED");
  CHECK(AppMock::ports & 1); CHECK(AppMock::buck);
  CHECK(AppMock::tx.find("\"request_id\":104,\"port\":\"5v_vbus\",\"enabled\":true,\"ok\":true") != std::string::npos);
  send(request(106, "port_set", ",\"port\":\"vbus\",\"enabled\":true")); response(106, true, "ACCEPTED");
  send(request(107, "port_set", ",\"port\":\"5v_ss\",\"enabled\":true")); response(107, true, "ACCEPTED");
  CHECK(AppMock::ports == 15); // J7/J8 unbacked, J9/J10 backed.

  if (scenario == "reconnect") {
    const unsigned beforeEnable = AppMock::enables;
    AppMock::connected = false; runUntil(millis() + 1500);
    AppMock::connected = true; runUntil(millis() + 1500);
    CHECK(!AppMock::restartCalls && !AppMock::released);
    CHECK(AppMock::enables == beforeEnable && AppMock::ce && AppMock::boost);
    send(R"({"v":1,"id":105,"cmd":"status"})"); response(105, true, "OK");
    // BOOT_N is observational while running, and pressing it is not a reset API.
    AppMock::bootPressed = true; runUntil(millis() + 1200);
    CHECK(!AppMock::restartCalls);
    CHECK(AppMock::tx.find("\"BOOT_N\":false") != std::string::npos);
    AppMock::bootPressed = false;
    // A real-TS fault during maintenance must disable only charging and leave
    // the running Jetson, optional outputs, backup, CDC and loop alive.
    AppMock::temperatureFault = true;
    const uint32_t faultAt = millis();
    runUntil(faultAt + 300);
    CHECK(!AppMock::ce && !AppMock::boost);
    CHECK(AppMock::disables == 1 && AppMock::ports == 15 && AppMock::backup);
    CHECK(!AppMock::released && !AppMock::restartCalls);
    send(R"({"v":1,"id":108,"cmd":"status"})"); response(108, true, "OK");
    CHECK(AppMock::tx.find("CHARGER_TEMPERATURE_INVALID") != std::string::npos);
    CHECK(AppMock::tx.find("\"enabled_expected\":false,\"maintenance\":false") != std::string::npos);
    runUntil(millis() + 1500);
    CHECK(AppMock::ports == 15 && AppMock::backup && AppMock::enables == beforeEnable);
  } else {
    uint32_t lossAt = 0;
    if (scenario == "reboot") {
      send(request(110, "reboot")); response(110, true, "ACCEPTED");
    } else {
      CHECK(scenario == "loss");
      AppMock::main = false;
      // The next 100 ms poll recognizes loss. Return main immediately after
      // that loop, before the 100 ms CE/boost cleanup or port jobs finish.
      runUntil(((millis() + 99) / 100) * 100 + 1);
      lossAt = millis() - 1;
      CHECK(!AppMock::ce && AppMock::ports == 15);
      AppMock::main = true;
      runUntil(millis() + 300);
      CHECK((AppMock::ports & 3) == 0 && (AppMock::ports & 12) == 12);
      CHECK(!AppMock::buck);
      std::vector<AppMock::PortRequest> shed;
      for (const auto &port : AppMock::portRequests)
        if (port.at >= lossAt && !port.enabled) shed.push_back(port);
      CHECK(shed.size() == 2);
      CHECK(shed[0].port == Port::Vbus && shed[1].port == Port::FiveVoltVbus);
      CHECK(shed[0].at >= lossAt + 100 && shed[1].at > shed[0].at);
      CHECK(shed[1].at < lossAt + 300);
    }
    const uint32_t deadline = lastNumber("\"deadline_ms\":");
    CHECK(deadline > millis());
    CHECK(AppMock::disables >= 1 && !AppMock::ce);
    if (scenario == "reboot") CHECK(AppMock::ports == 15 && AppMock::buck);
    send(request(111, "shutdown_ack", ",\"shutdown_id\":1")); response(111, true, "OK");
    send(request(112, "shutdown_ready", ",\"shutdown_id\":1")); response(112, true, "OK");
    send(request(113, "port_set", ",\"port\":\"vbus\",\"enabled\":true")); response(113, false, "SHUTTING_DOWN");
    AppMock::main = true;
    runUntil(deadline - 1);
    CHECK(!AppMock::released && !AppMock::restartCalls);
    CHECK(AppMock::ports & 4); CHECK(AppMock::backup);
    CHECK(!AppMock::ce && !AppMock::boost);
    if (scenario == "loss") {
      CHECK(AppMock::ports == 12 && !AppMock::buck);
      CHECK(deadline == lossAt + 60000);
    } else CHECK(AppMock::ports == 15 && AppMock::buck);
    runUntil(deadline + 200);
    CHECK(AppMock::released && AppMock::releaseAt >= deadline);
    CHECK(AppMock::releaseAt <= deadline + 101);
    CHECK(!AppMock::restartCalls);
    const unsigned calls = AppMock::hardwareCalls + AppMock::pdCalls;
    runUntil(AppMock::releaseAt + 250);
    CHECK(!AppMock::restartCalls);
    runUntil(AppMock::releaseAt + 251);
    CHECK(AppMock::restartCalls == 1 && AppMock::restartAt == AppMock::releaseAt + 250);
    CHECK(AppMock::hardwareCalls + AppMock::pdCalls == calls);
  }
  std::ofstream out(argv[2]); out << AppMock::tx; out.close();
  CHECK(bool(out));
  std::printf("Application scenario %s passed (%zu output bytes)\n", scenario.c_str(), AppMock::tx.size());
}
