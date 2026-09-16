#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Protocol {
constexpr size_t kMaxRequest = 512, kMaxMessage = 8192;
enum class Command { Status, Pong, ShutdownAck, ShutdownReady, PortSet, Reboot, Unknown };
struct Request {
  Command command = Command::Unknown;
  uint32_t id = 0, pingId = 0, shutdownId = 0;
  char bootId[9] = {}, port[24] = {};
  bool enabled = false;
};
// Strict flat JSON API: unescaped ASCII identifiers, unsigned integer IDs,
// boolean enabled. Reject duplicate/unknown keys, missing fields and trailing data.
const char *parse(const char *line, Request &request); // nullptr = success.
uint32_t fingerprint(const Request &request);

class Framer {
 public:
  enum class Result { None, Line, TooLong, Invalid };
  Result push(char byte);
  const char *line() const { return data_; }
  void reset() { used_ = 0; discard_ = false; invalid_ = false; }
 private:
  char data_[kMaxRequest + 1] = {};
  size_t used_ = 0;
  bool discard_ = false, invalid_ = false;
};

class ReplayCache {
 public:
  enum class Match { New, Same, Conflict };
  struct Reply { bool ok = false; const char *code = "BAD_REQUEST"; };
  Match lookup(const Request &, Reply &) const;
  void remember(const Request &, Reply);
 private:
  struct Entry { uint32_t id = 0, hash = 0; Reply reply; } entries_[16];
  unsigned next_ = 0;
};
} // namespace Protocol
