#include "UsbRecoveryDiagnostics.h"

#include <HWCDC.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <soc/usb_serial_jtag_struct.h>
#include <string.h>

namespace {
using Trace = UsbRecoveryDiagnostics;
constexpr uint32_t kMagic = 0x55425354, kVersion = 1, kSampleMs = 250;
struct Record {
  uint32_t magic, version, bytes, generation, checksum;
  Trace::Snapshot snapshot;
};
static_assert(sizeof(Record) == Trace::kBlobBytes, "USB trace blob layout changed");
// Double slots retain the previous committed record if reset interrupts a write.
// Retention is checked, never assumed, and is rejected after power/brownout reset.
RTC_NOINIT_ATTR Record journal[2];
portMUX_TYPE traceMux = portMUX_INITIALIZER_UNLOCKED;
Trace *owner = nullptr;
Trace::Snapshot live = {};
int currentSlot = -1;
uint32_t generation = 0;

uint32_t hashBytes(uint32_t hash, const void *data, size_t bytes) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (bytes--) { hash ^= *p++; hash *= 16777619u; }
  return hash;
}

uint32_t checksum(const Record &r) {
  uint32_t hash = hashBytes(2166136261u, &r.version, 3 * sizeof(uint32_t));
  return hashBytes(hash, &r.snapshot, sizeof(r.snapshot));
}

bool valid(const Record &r) {
  if (r.magic != kMagic || r.version != kVersion || r.bytes != sizeof(Record) ||
      r.snapshot.head >= Trace::kEventCapacity || r.snapshot.count > Trace::kEventCapacity ||
      r.snapshot.sofKnown > 1 || r.snapshot.sofConnected > 1 ||
      r.snapshot.cdcKnown > 1 || r.snapshot.cdcConnected > 1 ||
      r.snapshot.availableRx < -1 || r.checksum != checksum(r)) return false;
  for (uint32_t i = 0; i < r.snapshot.count; ++i) {
    const uint32_t index = (r.snapshot.head + Trace::kEventCapacity - r.snapshot.count + i) % Trace::kEventCapacity;
    const uint32_t kind = r.snapshot.events[index].kind;
    if (kind < uint32_t(Trace::Kind::Boot) || kind > uint32_t(Trace::Kind::ApplicationLine)) return false;
  }
  return true;
}

void append(Trace::Kind kind, uint32_t value, uint32_t now) {
  live.events[live.head] = {now, uint32_t(kind), value};
  live.head = (live.head + 1) % Trace::kEventCapacity;
  if (live.count < Trace::kEventCapacity) ++live.count;
}

// All callers hold traceMux. No USB calls, flash, allocation, or logging here.
void commit() {
  const int next = currentSlot == 0 ? 1 : 0;
  Record &r = journal[next];
  __atomic_store_n(&r.magic, 0u, __ATOMIC_RELEASE);
  r.version = kVersion;
  r.bytes = sizeof(Record);
  r.generation = ++generation;
  r.snapshot = live;
  r.checksum = checksum(r);
  __atomic_store_n(&r.magic, kMagic, __ATOMIC_RELEASE);
  currentSlot = next;
}

const char *kindName(uint32_t kind) {
  switch (Trace::Kind(kind)) {
    case Trace::Kind::Boot: return "BOOT";
    case Trace::Kind::BusReset: return "BUS_RESET";
    case Trace::Kind::Rx: return "RX";
    case Trace::Kind::SofConnection: return "SOF_CONNECTION";
    case Trace::Kind::CdcConnection: return "CDC_GATE";
    case Trace::Kind::ApplicationLine: return "APPLICATION_LINE";
  }
  return "INVALID";
}
}

bool UsbRecoveryDiagnostics::begin(uint32_t session) {
  if (started_) return true;
  if (owner && owner != this) return false;
  const esp_reset_reason_t reset = esp_reset_reason();
  const bool permitRtc = reset != ESP_RST_POWERON && reset != ESP_RST_BROWNOUT &&
      reset != ESP_RST_PWR_GLITCH && reset != ESP_RST_UNKNOWN;
  portENTER_CRITICAL(&traceMux);
  const bool a = valid(journal[0]), b = valid(journal[1]);
  int latest = -1;
  if (a) latest = 0;
  if (b && (!a || int32_t(journal[1].generation - journal[0].generation) > 0)) latest = 1;
  if (permitRtc && latest >= 0) {
    prior_ = journal[latest].snapshot;
    priorAvailable_ = true;
  }
  // On a power/brownout reset, discard even a coincidentally valid stale record.
  currentSlot = permitRtc ? latest : -1;
  generation = currentSlot >= 0 ? journal[currentSlot].generation : 0;
  if (!permitRtc) {
    journal[0].magic = 0;
    journal[1].magic = 0;
  }
  live = {};
  live.session = session;
  live.resetReason = uint32_t(reset);
  live.timeMs = millis();
  live.availableRx = -1;
  append(Kind::Boot, uint32_t(reset), live.timeMs);
  commit();
  owner = this;
  started_ = true;
  portEXIT_CRITICAL(&traceMux);
  // onEvent() returns void: registration was requested, but only received events
  // establish that it succeeded. Core's event queue can also drop events.
  HWCDCSerial.onEvent(eventCallback);
  lastSampleMs_ = millis() - kSampleMs;
  service();
  return true;
}

void UsbRecoveryDiagnostics::eventCallback(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base != ARDUINO_HW_CDC_EVENTS || !owner) return;
  const uint32_t now = millis();  // Task delivery time, not the USB wire timestamp.
  const auto *event = static_cast<const arduino_hw_cdc_event_data_t *>(data);
  portENTER_CRITICAL(&traceMux);
  live.timeMs = now;
  switch (id) {
    case ARDUINO_HW_CDC_BUS_RESET_EVENT:
      ++live.busResets;
      append(Kind::BusReset, live.busResets, now);
      break;
    case ARDUINO_HW_CDC_RX_EVENT:
      ++live.rxEvents;
      if (event) live.rxBytes += uint32_t(event->rx.len);
      append(Kind::Rx, event ? uint32_t(event->rx.len) : 0, now);
      break;
    case ARDUINO_HW_CDC_TX_EVENT:
      ++live.txEvents;
      if (event) live.txBytes += uint32_t(event->tx.len);
      // Core posts TX when bytes enter the hardware FIFO, not host receipt.
      // Count it, but do not flood the transition ring with diagnostic output.
      break;
    default:
      // CONNECTED_EVENT is declared but not posted by Arduino-ESP32 3.3.3.
      portEXIT_CRITICAL(&traceMux);
      return;
  }
  commit();
  portEXIT_CRITICAL(&traceMux);
}

void UsbRecoveryDiagnostics::service() {
  if (!started_) return;
  const uint32_t now = millis();
  if (now - lastSampleMs_ < kSampleMs) return;
  lastSampleMs_ = now;
  // isPlugged() is a read of the driver's SOF-based connection detector. Unlike
  // isConnected()/operator bool(), it does not kick TX or change CDC state.
  const bool sof = HWCDC::isPlugged();
  const int rx = HWCDCSerial.available();
  const uint32_t raw = USB_SERIAL_JTAG.int_raw.val;
  const uint32_t enabled = USB_SERIAL_JTAG.int_ena.val;
  const uint32_t status = USB_SERIAL_JTAG.int_st.val;
  portENTER_CRITICAL(&traceMux);
  live.timeMs = now;
  if (!live.sofKnown || live.sofConnected != uint32_t(sof)) append(Kind::SofConnection, sof, now);
  live.sofKnown = 1;
  live.sofConnected = sof;
  live.availableRx = rx;
  live.interruptRaw = raw;
  live.interruptEnabled = enabled;
  live.interruptStatus = status;
  commit();
  portEXIT_CRITICAL(&traceMux);
}

void UsbRecoveryDiagnostics::observeConnection(bool connected) {
  if (!started_) return;
  const uint32_t now = millis();
  portENTER_CRITICAL(&traceMux);
  ++live.connectionObservations;
  if (!live.cdcKnown || live.cdcConnected != uint32_t(connected)) {
    live.timeMs = now;
    live.cdcKnown = 1;
    live.cdcConnected = connected;
    append(Kind::CdcConnection, connected, now);
    commit();
  }
  portEXIT_CRITICAL(&traceMux);
}

void UsbRecoveryDiagnostics::observeApplicationRx(int value, bool commandComplete) {
  if (!started_) return;
  portENTER_CRITICAL(&traceMux);
  if (value >= 0) ++live.applicationRxBytes;
  if (commandComplete) {
    ++live.applicationLines;
    live.timeMs = millis();
    append(Kind::ApplicationLine, live.applicationLines, live.timeMs);
    commit();
  }
  portEXIT_CRITICAL(&traceMux);
}

bool UsbRecoveryDiagnostics::snapshot(Snapshot &out, bool previous) const {
  portENTER_CRITICAL(&traceMux);
  const bool available = previous ? priorAvailable_ : started_;
  if (available) out = previous ? prior_ : live;
  portEXIT_CRITICAL(&traceMux);
  return available;
}

const char *UsbRecoveryDiagnostics::priorSource() const {
  if (!priorAvailable_) return "NONE";
  return priorFromNvs_ ? "NVS_IMPORTED" : "RTC_VALIDATED";
}

size_t UsbRecoveryDiagnostics::exportBlob(void *destination, size_t capacity) const {
  if (!destination || capacity < sizeof(Record) || !started_) return 0;
  portENTER_CRITICAL(&traceMux);
  // Include observations since the last periodic journal commit.
  commit();
  memcpy(destination, &journal[currentSlot], sizeof(Record));
  portEXIT_CRITICAL(&traceMux);
  return sizeof(Record);
}

bool UsbRecoveryDiagnostics::importPriorBlob(const void *blob, size_t length) {
  if (!blob || length != sizeof(Record)) return false;
  Record saved;
  memcpy(&saved, blob, sizeof(saved));
  if (!valid(saved)) return false;
  portENTER_CRITICAL(&traceMux);
  const bool accepted = !priorAvailable_;
  if (accepted) { prior_ = saved.snapshot; priorAvailable_ = priorFromNvs_ = true; }
  portEXIT_CRITICAL(&traceMux);
  return accepted;
}

void UsbRecoveryDiagnostics::printSnapshot(Stream &out, const char *label, const Snapshot &s,
                                          BetweenRows betweenRows) {
  out.printf("USBTRACE %s session=%08lX reset=%lu t=%lu resets=%lu rx_events=%lu rx_bytes=%lu tx_fifo_events=%lu tx_fifo_bytes=%lu\n",
             label, (unsigned long)s.session, (unsigned long)s.resetReason, (unsigned long)s.timeMs,
             (unsigned long)s.busResets, (unsigned long)s.rxEvents, (unsigned long)s.rxBytes,
             (unsigned long)s.txEvents, (unsigned long)s.txBytes);
  if (betweenRows) betweenRows();
  out.printf("USBTRACE %s application_rx_bytes=%lu application_lines=%lu\n", label,
             (unsigned long)s.applicationRxBytes, (unsigned long)s.applicationLines);
  if (betweenRows) betweenRows();
  out.printf("USBTRACE %s sof_known=%lu sof=%lu cdc_known=%lu cdc_gate=%lu observations=%lu rx_available=%ld raw=%08lX ena=%08lX status=%08lX events=%lu\n",
             label, (unsigned long)s.sofKnown, (unsigned long)s.sofConnected,
             (unsigned long)s.cdcKnown, (unsigned long)s.cdcConnected,
             (unsigned long)s.connectionObservations, (long)s.availableRx,
             (unsigned long)s.interruptRaw, (unsigned long)s.interruptEnabled,
             (unsigned long)s.interruptStatus, (unsigned long)s.count);
  if (betweenRows) betweenRows();
  for (uint32_t i = 0; i < s.count; ++i) {
    const Event &event = s.events[(s.head + kEventCapacity - s.count + i) % kEventCapacity];
    out.printf("USBTRACE_EVENT %s session=%08lX t=%lu kind=%s value=%lu\n", label,
               (unsigned long)s.session, (unsigned long)event.timeMs,
               kindName(event.kind), (unsigned long)event.value);
    if (betweenRows) betweenRows();
  }
}

void UsbRecoveryDiagnostics::dump(Stream &out, BetweenRows betweenRows) const {
  Snapshot current, previous;
  const bool haveCurrent = snapshot(current), havePrevious = snapshot(previous, true);
  // Freeze both records before writing: callbacks and service() may mutate live
  // state while the caller services supervision between these bounded rows.
  if (havePrevious) printSnapshot(out, priorSource(), previous, betweenRows);
  if (haveCurrent) printSnapshot(out, "CURRENT", current, betweenRows);
}

bool UsbRecoveryDiagnostics::dumpBlob(Stream &out, const void *blob, size_t length, BetweenRows betweenRows) {
  if (!blob || length != sizeof(Record)) return false;
  Record saved;
  memcpy(&saved, blob, sizeof(saved));
  if (!valid(saved)) return false;
  printSnapshot(out, "NVS_SAVED", saved.snapshot, betweenRows);
  return true;
}
