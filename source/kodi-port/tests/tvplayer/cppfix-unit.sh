#!/bin/bash
# Host test: compiles the real source text of three changed pieces against mocks
#  1. feeder eviction loop (TVPlayerHttpFeeder.cpp NotifyConnection)
#  2. HasDvBaseLayer (TVPlayerProbe.cpp)
#  3. CTVBridgeIO::IsSwitchedOff + marker path (TVBridgeIO.cpp)
set -eu
T=~/flex/kodi-port/kodi-tree/xbmc/cores/TVPlayer
W=$(mktemp -d /tmp/cppfix-unit.XXXXXX)
trap 'rm -rf "$W"' EXIT

# 1. from "size_t live = 0;" up to the closing brace of the while loop
awk '/size_t live = 0;/{on=1} on{print} on&&/--live;/{getline; print; exit}' \
  "$T/TVPlayerHttpFeeder.cpp" > "$W/evict.inc"
# 2. the function
awk '/^bool HasDvBaseLayer/{on=1} on{print} on&&/^}/{exit}' "$T/TVPlayerProbe.cpp" > "$W/dv.inc"
# 3. the marker path lines of the constructor and the function
awk '/const std::string key\(keyFile\);/{on=1} on{print} on&&/bridge-off";/{exit}' \
  "$T/TVBridgeIO.cpp" > "$W/marker.inc"
awk '/^bool CTVBridgeIO::IsSwitchedOff/{on=1} on{print} on&&/^}/{exit}' \
  "$T/TVBridgeIO.cpp" | sed 's/CTVBridgeIO::IsSwitchedOff() const/IsSwitchedOff()/' > "$W/off.inc"
wc -l "$W"/*.inc

cat > "$W/t.cpp" <<'EOF'
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#define LOGINFO 0
#define LOGWARNING 1
namespace CLog { template <typename... A> void Log(int, const char*, A&&...) {} }
static std::vector<int> g_shut;
static int shutdown(int fd, int) { g_shut.push_back(fd); return 0; }
#define SHUT_RDWR 2
static int64_t g_now = 1000;
static int64_t SteadyMs() { return g_now; }
struct Connection { int fd = -1; std::atomic<int64_t> lastActivityMs{0}; std::atomic<bool> evicted{false}; };
struct Cfg { unsigned int connections; };
struct Self { Cfg m_config; std::map<int, std::shared_ptr<Connection>> m_connections; };

static size_t Notify(Self* self, int id, int fd)
{
  auto entry = std::make_shared<Connection>();
  entry->fd = fd;
  entry->lastActivityMs = SteadyMs();
#include "evict.inc"
  self->m_connections[id] = std::move(entry);
  return live + 1;
}

struct AVDOVIDecoderConfigurationRecord { uint8_t dv_version_major, dv_version_minor, dv_profile, dv_level, rpu_present_flag, el_present_flag, bl_present_flag, dv_bl_signal_compatibility_id; };
#include "dv.inc"

static std::string Marker(const char* keyFile) { std::string m_offMarker;
#include "marker.inc"
  return m_offMarker; }
static std::string m_offMarker;
static std::atomic<bool> m_offLogged{false};
#include "off.inc"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main()
{
  // eviction: limit 16, 16 live, then a 17th: oldest live closed, live stays 16
  Self s{{16}, {}};
  for (int i = 0; i < 16; ++i) { g_now = 1000 + i; Notify(&s, i, 100 + i); }
  CHECK(g_shut.empty());
  g_now = 2000; size_t live = Notify(&s, 16, 116);
  CHECK(live == 16); CHECK(g_shut.size() == 1 && g_shut[0] == 100);
  CHECK(s.m_connections[0]->evicted && s.m_connections[0]->fd == -1);
  // the evicted one stays in the map (blocked read) with the oldest timestamp: 18th must close
  // the next live one (101), not pick 0 again
  g_now = 2001; live = Notify(&s, 17, 117);
  CHECK(live == 16); CHECK(g_shut.size() == 2 && g_shut[1] == 101);
  // burst of 10 more while the evicted entries never close: live never above 16
  for (int i = 18; i < 28; ++i) { g_now = 3000 + i; live = Notify(&s, i, 100 + i); CHECK(live == 16); }
  size_t liveNow = 0; for (auto& e : s.m_connections) liveNow += !e.second->evicted;
  CHECK(liveNow == 16); CHECK(g_shut.size() == 12);
  // an entry without fd (info failed) is still evicted (marked), nothing shut down for it
  Self u{{2}, {}}; g_shut.clear();
  g_now = 1; Notify(&u, 1, -1); g_now = 2; Notify(&u, 2, 50); g_now = 3; live = Notify(&u, 3, 51);
  CHECK(live == 2); CHECK(u.m_connections[1]->evicted); CHECK(g_shut.empty());

  // Dolby Vision: refuse profile 5 / no or reserved base; 7, 8.1, 8.2, 8.4, 4, 9 play
  auto dv = [](int p, int id) { AVDOVIDecoderConfigurationRecord r{}; r.dv_profile = (uint8_t)p; r.dv_bl_signal_compatibility_id = (uint8_t)id; return HasDvBaseLayer(r); };
  CHECK(!dv(5, 0)); CHECK(!dv(5, 1)); CHECK(dv(8, 1)); CHECK(dv(8, 2)); CHECK(dv(8, 4));
  CHECK(dv(7, 6)); CHECK(dv(4, 2)); CHECK(dv(9, 2)); CHECK(!dv(8, 0)); CHECK(!dv(8, 3));
  CHECK(!dv(8, 5)); CHECK(!dv(8, 7));

  // marker path next to the key file
  CHECK(Marker("/tmp/flexkodi.d/key") == "/tmp/flexkodi.d/bridge-off");
  CHECK(Marker("/key") == "/bridge-off");
  char dir[] = "/tmp/cppfix-off.XXXXXX"; CHECK(mkdtemp(dir) != nullptr);
  m_offMarker = std::string(dir) + "/bridge-off";
  CHECK(!IsSwitchedOff());
  FILE* f = std::fopen(m_offMarker.c_str(), "w"); std::fclose(f);
  CHECK(IsSwitchedOff());
  std::remove(m_offMarker.c_str()); CHECK(!IsSwitchedOff());
  CHECK(symlink("/nonexistent", m_offMarker.c_str()) == 0); CHECK(IsSwitchedOff()); // dangling link counts
  std::remove(m_offMarker.c_str()); rmdir(dir);
  m_offMarker.clear(); CHECK(!IsSwitchedOff());

  std::printf("%s (%d failures)\n", fails ? "FAILED" : "all checks passed", fails);
  return fails ? 1 : 0;
}
EOF
cd "$W" && g++ -std=c++17 -Wall -Wextra -Werror -o t t.cpp -include unistd.h && ./t
