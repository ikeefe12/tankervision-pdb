#include "Protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
  std::exit(1); } } while (0)

using Protocol::Command;
using Protocol::Framer;
using Protocol::ReplayCache;
using Protocol::Request;

Request parsed(const std::string &json) {
  Request request;
  const char *error = Protocol::parse(json.c_str(), request);
  if (error) std::fprintf(stderr, "Unexpected %s for %s\n", error, json.c_str());
  CHECK(error == nullptr);
  return request;
}

void rejected(const std::string &json, const char *expected = nullptr) {
  Request request;
  const char *error = Protocol::parse(json.c_str(), request);
  if (!error) std::fprintf(stderr, "Unexpected acceptance: %s\n", json.c_str());
  CHECK(error != nullptr);
  if (expected) CHECK(std::strcmp(error, expected) == 0);
}

void all_commands_have_expected_fields() {
  auto r = parsed(R"({"v":1,"id":1,"cmd":"status"})");
  CHECK(r.command == Command::Status && r.id == 1);
  r = parsed(R"({"v":1,"id":2,"cmd":"pong","boot_id":"12ABCDEF","ping_id":123})");
  CHECK(r.command == Command::Pong && r.pingId == 123);
  CHECK(std::strcmp(r.bootId, "12ABCDEF") == 0);
  r = parsed(R"({"v":1,"id":3,"cmd":"shutdown_ack","boot_id":"12ABCDEF","shutdown_id":456})");
  CHECK(r.command == Command::ShutdownAck && r.shutdownId == 456);
  r = parsed(R"({"v":1,"id":4,"cmd":"shutdown_ready","boot_id":"12ABCDEF","shutdown_id":456})");
  CHECK(r.command == Command::ShutdownReady && r.shutdownId == 456);
  r = parsed(R"({"v":1,"id":5,"cmd":"port_set","boot_id":"12ABCDEF","port":"supervised","enabled":true})");
  CHECK(r.command == Command::PortSet && r.enabled);
  CHECK(std::strcmp(r.port, "supervised") == 0);
  r = parsed(R"({"v":1,"id":6,"cmd":"reboot","boot_id":"12ABCDEF"})");
  CHECK(r.command == Command::Reboot);
}

void reordered_keys_and_whitespace_have_identical_fingerprints() {
  const auto a = parsed(R"({"v":1,"id":6,"cmd":"port_set","boot_id":"1234ABCD","port":"supervised","enabled":false})");
  const auto b = parsed(" \t{ \"enabled\" : false, \"port\":\"supervised\",\r"
                        "\"boot_id\":\"1234ABCD\",\"cmd\":\"port_set\",\"id\":6,\"v\":1 }\r");
  CHECK(Protocol::fingerprint(a) == Protocol::fingerprint(b));
  CHECK(!b.enabled);
}

void schemas_reject_missing_extra_duplicate_and_unknown_keys() {
  for (const auto &json : std::vector<std::string>{
      R"({"id":1,"cmd":"status"})",
      R"({"v":1,"cmd":"status"})",
      R"({"v":1,"id":1})",
      R"({"v":1,"id":1,"cmd":"reboot"})",
      R"({"v":1,"id":1,"cmd":"pong","boot_id":"12ABCDEF"})",
      R"({"v":1,"id":1,"cmd":"shutdown_ack","boot_id":"12ABCDEF"})",
      R"({"v":1,"id":1,"cmd":"shutdown_ready","boot_id":"12ABCDEF"})",
      R"({"v":1,"id":1,"cmd":"port_set","boot_id":"12ABCDEF","port":"supervised"})",
      R"({"v":1,"id":1,"cmd":"port_set","boot_id":"12ABCDEF","enabled":false})",
      R"({"v":1,"id":1,"cmd":"status","boot_id":"12ABCDEF"})",
      R"({"v":1,"id":1,"cmd":"reboot","boot_id":"12ABCDEF","ping_id":1})",
      R"({"v":1,"id":1,"cmd":"status","other":true})",
      R"({"v":1,"id":1,"id":2,"cmd":"status"})",
      R"({"v":1,"v":1,"id":1,"cmd":"status"})"
    }) rejected(json, "BAD_REQUEST");
  rejected(R"({"v":2,"id":1,"cmd":"status"})", "UNSUPPORTED_VERSION");
  rejected(R"({"v":1,"id":1,"cmd":"STATUS"})", "UNKNOWN_COMMAND");
}

void integer_and_boolean_types_are_strict() {
  auto r = parsed(R"({"v":1,"id":4294967295,"cmd":"pong","boot_id":"12ABCDEF","ping_id":4294967295})");
  CHECK(r.id == UINT32_MAX && r.pingId == UINT32_MAX);
  for (const auto &value : std::vector<std::string>{
      "-1", "+1", "1.0", "1e0", "01", "4294967296", "99999999999999999999",
      "true", "null", "\"1\"", "[]", "{}"})
    rejected("{\"v\":1,\"id\":" + value + ",\"cmd\":\"status\"}", "BAD_JSON");
  rejected(R"({"v":1,"id":0,"cmd":"status"})", "BAD_REQUEST");
  for (const auto &value : std::vector<std::string>{"1", "0", "null", "TRUE", "falsex", "\"true\""})
    rejected("{\"v\":1,\"id\":1,\"cmd\":\"port_set\",\"boot_id\":\"1234ABCD\","
             "\"port\":\"supervised\",\"enabled\":" + value + "}", "BAD_JSON");
}

void boot_identifier_requires_exact_uppercase_hex() {
  parsed(R"({"v":1,"id":1,"cmd":"reboot","boot_id":"00000000"})");
  parsed(R"({"v":1,"id":1,"cmd":"reboot","boot_id":"FFFFFFFF"})");
  for (const auto &boot : std::vector<std::string>{"", "1234567", "123456789", "1234abcd", "G1234567", "1234 ABC"})
    rejected("{\"v\":1,\"id\":1,\"cmd\":\"reboot\",\"boot_id\":\"" + boot + "\"}");
}

void malformed_json_and_escapes_are_rejected() {
  for (const auto &json : std::vector<std::string>{
      "", " ", "[]", "null", "{", "{\"v\"", "{\"v\":", "{\"v\":1,",
      R"({"v":1,"id":1,"cmd":"status",})",
      R"({"v":1 "id":1,"cmd":"status"})",
      R"({"v":1,"id":1,"cmd":"status"} trailing)",
      R"({"v":1,"id":1,"cmd":"status"}{})",
      R"({'v':1,"id":1,"cmd":"status"})",
      R"({"v":1,"id":1,"cmd":"sta\tus"})",
      R"({"v":1,"id":1,"cmd":"\u0073tatus"})",
      "{\"v\":1,\"id\":1,\"cmd\":\"sta\ttus\"}",
      "{\"v\":1,\"id\":1,\"cmd\":\"sta\xC3\xA9tus\"}"
    }) rejected(json, "BAD_JSON");
}

void truncated_requests_never_parse_or_read_past_the_buffer() {
  const std::string valid = R"({"v":1,"id":1,"cmd":"port_set","boot_id":"1234ABCD","port":"supervised","enabled":false})";
  for (size_t n = 0; n < valid.size(); ++n) rejected(valid.substr(0, n));
  Request request;
  CHECK(std::strcmp(Protocol::parse(nullptr, request), "BAD_JSON") == 0);
}

void parser_and_framer_accept_exact_limit_and_reject_overflow() {
  std::string json = R"({"v":1,"id":1,"cmd":"status"})";
  json.resize(Protocol::kMaxRequest, ' ');
  parsed(json);
  Framer framer;
  for (char c : json) CHECK(framer.push(c) == Framer::Result::None);
  CHECK(framer.push('\n') == Framer::Result::Line);
  CHECK(std::strlen(framer.line()) == Protocol::kMaxRequest);
  parsed(framer.line());
  json += ' ';
  rejected(json, "BAD_JSON");
  for (char c : json) CHECK(framer.push(c) == Framer::Result::None);
  CHECK(framer.push('\n') == Framer::Result::TooLong);
  for (char c : std::string(R"({"v":1,"id":2,"cmd":"status"})"))
    CHECK(framer.push(c) == Framer::Result::None);
  CHECK(framer.push('\n') == Framer::Result::Line);
  CHECK(parsed(framer.line()).id == 2);
}

void nul_discards_entire_frame_and_newline_recovers() {
  Framer framer;
  for (char c : std::string("prefix")) framer.push(c);
  framer.push('\0');
  for (char c : std::string(R"({"v":1,"id":1,"cmd":"status"})")) framer.push(c);
  CHECK(framer.push('\n') == Framer::Result::Invalid);
  for (char c : std::string(R"({"v":1,"id":2,"cmd":"status"})")) framer.push(c);
  framer.push('\r');
  CHECK(framer.push('\n') == Framer::Result::Line);
  CHECK(parsed(framer.line()).id == 2);
  framer.push('x');
  framer.reset();
  CHECK(framer.push('\n') == Framer::Result::Line);
  CHECK(std::strlen(framer.line()) == 0);
}

void replay_recognizes_same_content_and_conflicting_id() {
  ReplayCache cache;
  ReplayCache::Reply reply;
  const auto original = parsed(R"({"v":1,"id":1,"cmd":"reboot","boot_id":"1234ABCD"})");
  CHECK(cache.lookup(original, reply) == ReplayCache::Match::New);
  cache.remember(original, {true, "REBOOT_STARTED"});
  const auto reordered = parsed(R"({"boot_id":"1234ABCD","cmd":"reboot","id":1,"v":1})");
  CHECK(cache.lookup(reordered, reply) == ReplayCache::Match::Same);
  CHECK(reply.ok && std::strcmp(reply.code, "REBOOT_STARTED") == 0);
  const auto changed = parsed(R"({"v":1,"id":1,"cmd":"status"})");
  CHECK(cache.lookup(changed, reply) == ReplayCache::Match::Conflict);
  CHECK(cache.lookup(original, reply) == ReplayCache::Match::Same);
}

void replay_distinguishes_port_values_and_keeps_sixteen_entries() {
  ReplayCache cache;
  ReplayCache::Reply reply;
  auto original = parsed(R"({"v":1,"id":1,"cmd":"port_set","boot_id":"1234ABCD","port":"supervised","enabled":false})");
  cache.remember(original, {false, "PORT_FAILED"});
  auto changed = original;
  changed.enabled = true;
  CHECK(cache.lookup(changed, reply) == ReplayCache::Match::Conflict);
  changed = original;
  std::strcpy(changed.port, "vbus");
  CHECK(cache.lookup(changed, reply) == ReplayCache::Match::Conflict);
  changed = original;
  std::strcpy(changed.bootId, "ABCD1234");
  CHECK(cache.lookup(changed, reply) == ReplayCache::Match::Conflict);
  for (uint32_t id = 2; id <= 16; ++id) {
    changed = original;
    changed.id = id;
    cache.remember(changed, {true, "OK"});
  }
  CHECK(cache.lookup(original, reply) == ReplayCache::Match::Same);
  CHECK(!reply.ok && std::strcmp(reply.code, "PORT_FAILED") == 0);
  changed.id = 17;
  cache.remember(changed, {true, "OK"});
  CHECK(cache.lookup(original, reply) == ReplayCache::Match::New);
}

void bounded_random_streams_do_not_corrupt_framing() {
  uint32_t random = 0x71835492;
  Framer framer;
  Request request;
  for (unsigned test = 0; test < 20000; ++test) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    const unsigned length = random % 1024;
    for (unsigned i = 0; i < length; ++i) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      const auto result = framer.push(char(random & 255));
      if (result == Framer::Result::Line) Protocol::parse(framer.line(), request);
    }
    if (framer.push('\n') == Framer::Result::Line)
      Protocol::parse(framer.line(), request);
  }
}

int main() {
  void (*tests[])() = {
    all_commands_have_expected_fields,
    reordered_keys_and_whitespace_have_identical_fingerprints,
    schemas_reject_missing_extra_duplicate_and_unknown_keys,
    integer_and_boolean_types_are_strict,
    boot_identifier_requires_exact_uppercase_hex,
    malformed_json_and_escapes_are_rejected,
    truncated_requests_never_parse_or_read_past_the_buffer,
    parser_and_framer_accept_exact_limit_and_reject_overflow,
    nul_discards_entire_frame_and_newline_recovers,
    replay_recognizes_same_content_and_conflicting_id,
    replay_distinguishes_port_values_and_keeps_sixteen_entries,
    bounded_random_streams_do_not_corrupt_framing
  };
  for (auto test : tests) test();
  std::printf("Protocol: %zu scenarios passed (including 20,000 random streams)\n",
              sizeof(tests) / sizeof(tests[0]));
}
