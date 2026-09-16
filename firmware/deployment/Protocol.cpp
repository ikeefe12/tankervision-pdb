#include "Protocol.h"
#include <string.h>
#include <limits.h>

namespace Protocol {
namespace {
void space(const char *&p) { while (*p == ' ' || *p == '\t' || *p == '\r') ++p; }
bool string(const char *&p, char *out, size_t capacity) {
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"') {
    const unsigned char c = *p++;
    if (c < 32 || c > 126 || c == '\\' || n + 1 >= capacity) return false;
    out[n++] = char(c);
  }
  if (*p != '"') return false;
  ++p; out[n] = 0; return true;
}
bool number(const char *&p, uint32_t &value) {
  if (*p < '0' || *p > '9') return false;
  if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;
  value = 0;
  while (*p >= '0' && *p <= '9') {
    const uint32_t digit = uint32_t(*p++ - '0');
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}
}
const char *parse(const char *line, Request &out) {
  out = {};
  if (!line || strlen(line) > kMaxRequest) return "BAD_JSON";
  const char *p = line; space(p);
  if (*p++ != '{') return "BAD_JSON";
  uint32_t fields = 0, version = 0; char command[32] = {};
  for (;;) {
    space(p); if (*p == '}') { ++p; break; }
    if (!*p) return "BAD_JSON";
    char key[32]; if (!string(p, key, sizeof(key))) return "BAD_JSON";
    space(p); if (*p++ != ':') return "BAD_JSON"; space(p);
    int index = -1;
    const char *keys[] = {"v", "id", "cmd", "boot_id", "ping_id", "shutdown_id", "port", "enabled"};
    for (int i = 0; i < 8; ++i) if (!strcmp(key, keys[i])) index = i;
    if (index < 0 || (fields & (1u << index))) return "BAD_REQUEST";
    fields |= 1u << index;
    bool ok = false;
    switch (index) {
      case 0: ok = number(p, version); break;
      case 1: ok = number(p, out.id); break;
      case 2: ok = string(p, command, sizeof(command)); break;
      case 3: ok = string(p, out.bootId, sizeof(out.bootId)); break;
      case 4: ok = number(p, out.pingId); break;
      case 5: ok = number(p, out.shutdownId); break;
      case 6: ok = string(p, out.port, sizeof(out.port)); break;
      case 7:
        if (!strncmp(p, "true", 4)) { out.enabled = true; p += 4; ok = true; }
        else if (!strncmp(p, "false", 5)) { out.enabled = false; p += 5; ok = true; }
        break;
    }
    if (!ok) return "BAD_JSON";
    space(p);
    if (*p == '}') { ++p; break; }
    if (*p++ != ',') return "BAD_JSON";
    space(p); if (*p == '}') return "BAD_JSON";
  }
  space(p); if (*p) return "BAD_JSON";
  if ((fields & 7) != 7 || !out.id) return "BAD_REQUEST";
  if (version != 1) return "UNSUPPORTED_VERSION";
  const char *names[] = {"status", "pong", "shutdown_ack", "shutdown_ready", "port_set", "reboot"};
  for (unsigned i = 0; i < 6; ++i) if (!strcmp(command, names[i])) out.command = Command(i);
  if (out.command == Command::Unknown) return "UNKNOWN_COMMAND";
  uint32_t required = 7;
  if (out.command != Command::Status) required |= 8;
  if (out.command == Command::Pong) required |= 16;
  if (out.command == Command::ShutdownAck || out.command == Command::ShutdownReady) required |= 32;
  if (out.command == Command::PortSet) required |= 64 | 128;
  if (fields != required) return "BAD_REQUEST";
  if (required & 8) {
    if (strlen(out.bootId) != 8) return "BAD_REQUEST";
    for (char c : out.bootId) if (c && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return "BAD_REQUEST";
  }
  return nullptr;
}
uint32_t fingerprint(const Request &r) {
  uint32_t hash = 2166136261u;
  const auto mix = [&](uint32_t v) { for (unsigned i = 0; i < 4; ++i) { hash ^= uint8_t(v >> (i * 8)); hash *= 16777619u; } };
  mix(uint32_t(r.command)); mix(r.id); mix(r.pingId); mix(r.shutdownId); mix(r.enabled);
  for (char c : r.bootId) { hash ^= uint8_t(c); hash *= 16777619u; }
  for (char c : r.port) { hash ^= uint8_t(c); hash *= 16777619u; }
  return hash;
}
Framer::Result Framer::push(char byte) {
  if (byte == '\n') {
    const Result result = invalid_ ? Result::Invalid : discard_ ? Result::TooLong : Result::Line;
    data_[used_] = 0; used_ = 0; discard_ = false; invalid_ = false; return result;
  }
  if (byte == 0) { invalid_ = true; discard_ = true; }
  if (discard_) return Result::None;
  if (used_ == kMaxRequest) { discard_ = true; return Result::None; }
  data_[used_++] = byte; return Result::None;
}
ReplayCache::Match ReplayCache::lookup(const Request &r, Reply &reply) const {
  for (const Entry &entry : entries_) if (entry.id && entry.id == r.id) {
    reply = entry.reply; return entry.hash == fingerprint(r) ? Match::Same : Match::Conflict;
  }
  return Match::New;
}
void ReplayCache::remember(const Request &r, Reply reply) {
  entries_[next_++ % 16] = {r.id, fingerprint(r), reply};
}
} // namespace Protocol
