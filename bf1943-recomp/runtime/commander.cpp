// COMMANDER SEATS, ORDERS and the LIVE MAP (commander.h; the drops are in
// commander_world.cpp).
#include "commander.h"

#include "bot_minimap.h"
#include "guest.h"
#include "kernel.h"
#include "lan.h"
#include "replay.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include "posix_net.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image_write.h"

namespace rt::commander {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kExpireS = 20.0;     // a seat with no keep-alive (or page poll) this long is free
constexpr double kGraceS = 90.0;      // a new seat lasts this long anyway (to open the page)
constexpr int kKeepEveryMs = 5000;    // a joiner's keep-alive period
constexpr int kEyeEveryMs = 5000;     // a joiner's eye packet period
constexpr int kLivePortBase = 19440;  // TCP; + the port offset
constexpr double kOrderSeconds = 180;  // an order lasts this long
constexpr double kOrderCooldownS = 10;  // orders: their own short cooldown
constexpr float kFlagPickM = 60;      // a click this near a flag means the flag

double Since(Clock::time_point t, Clock::time_point now) { return std::chrono::duration<double>(now - t).count(); }
bool VehicleDropOn() {
  static const bool on = std::getenv("BF_VEHICLE_DROP") && std::strcmp(std::getenv("BF_VEHICLE_DROP"), "0") != 0;
  return on;
}

struct Seat {
  bool held = false;
  uint32_t ip = 0;     // network order; 0 and port 0: this game's own player (the host or the console)
  uint16_t port = 0;   // the chat port
  std::string name;
  std::string token;   // the page's secret
  Clock::time_point seen, taken, web_seen;
  bool web = false;    // the page has polled: the page keeps the seat from now on
  bool Local() const { return ip == 0 && port == 0; }
};

// A player found under its eye (host): its soldier's id in the live sample, its team.
struct Known {
  uint16_t id = 0;
  int team = 0;
  Clock::time_point seen;
};

struct OrderState {
  Order o;
  uint16_t point = 0;
  Clock::time_point at;
};

std::mutex g_mu;
Seat g_seat[3];  // by team
std::map<std::pair<int, std::string>, Clock::time_point> g_last_act;  // (team, action): last allowed
std::atomic<int> g_cooldown{60};  // seconds: the drops' base
std::map<std::pair<uint32_t, uint16_t>, Known> g_known;  // (ip, chat port) -> its soldier
Known g_local;                                           // the host's own player
OrderState g_order[3];
uint32_t g_order_serial = 0;
std::deque<std::string> g_notes[3];  // what the page tells its commander (the last few)
std::atomic<int> g_joiner_team{0};  // a joiner: the seat this game holds (0 none)
std::string g_joiner_token;         // (its token: the link is shown when it changes)
std::string g_joiner_link;          // (and its page's link)
Clock::time_point g_last_keep, g_last_eye;  // (a joiner's last keep-alive, eye)
std::once_flag g_once;
std::function<void(const std::string&)> g_announce;
std::function<void(uint32_t, uint16_t, const std::string&)> g_tell;

// Seconds since the last keyboard or mouse input (Windows; 0 elsewhere: a
// local seat is then kept for good).
double IdleSeconds() {
#ifdef _WIN32
  LASTINPUTINFO li{sizeof(LASTINPUTINFO), 0};
  if (!GetLastInputInfo(&li)) return 0;
  return double(GetTickCount() - li.dwTime) / 1000.0;
#else
  return 0;
#endif
}

// A seat still held (under g_mu). Once its page has polled, the page keeps it;
// before that a local seat is kept by the host's input, a joiner's by its
// keep-alives, and every seat lasts kGraceS from when it was taken.
bool Fresh(Seat& s, Clock::time_point now) {
  if (!s.held) return false;
  if (s.web) {
    if (Since(s.web_seen, now) <= kExpireS) return true;
  } else {
    if (s.Local() && IdleSeconds() < kExpireS) s.seen = now;
    if (Since(s.seen, now) <= kExpireS || Since(s.taken, now) <= kGraceS) return true;
  }
  s = Seat{};
  return false;
}

bool SameHolder(const Seat& s, uint32_t ip, uint16_t port) { return s.held && s.ip == ip && s.port == port; }

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
  return s;
}

std::string NewToken() {
  std::random_device rd;
  char b[20];
  snprintf(b, sizeof b, "%08x%08x", unsigned(rd()), unsigned(rd()));
  return b;
}

bool ValidToken(const std::string& t) {
  if (t.size() != 16) return false;
  for (char c : t)
    if (!isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// The seat a token is for (under g_mu): its team, else 0.
int TeamOfToken(const std::string& token) {
  if (!ValidToken(token)) return 0;
  const auto now = Clock::now();
  for (int t = kUS; t <= kJapan; ++t)
    if (g_seat[t].held && g_seat[t].token == token && Fresh(g_seat[t], now)) return t;
  return 0;
}

void Note(int team, const std::string& text) {  // (under g_mu)
  if (team < kUS || team > kJapan) return;
  g_notes[team].push_back(text);
  while (g_notes[team].size() > 6) g_notes[team].pop_front();
}

// An action's cooldown check (under g_mu): true starts it.
bool ActLocked(int team, const std::string& action, double* wait_seconds) {
  const auto now = Clock::now();
  auto key = std::make_pair(team, action);
  auto it = g_last_act.find(key);
  const double len = CooldownFor(action);
  if (it != g_last_act.end()) {
    const double since = Since(it->second, now);
    if (since < len) {
      if (wait_seconds) *wait_seconds = len - since;
      return false;
    }
  }
  g_last_act[key] = now;
  if (wait_seconds) *wait_seconds = 0;
  return true;
}
double WaitLocked(int team, const std::string& action) {
  auto it = g_last_act.find(std::make_pair(team, action));
  if (it == g_last_act.end()) return 0;
  return std::max(0.0, CooldownFor(action) - Since(it->second, Clock::now()));
}

// ---- where things are ----------------------------------------------------------

// The level's picture and which way it lies (bot_minimap.h); the frame of the
// page and of the directions told in chat (up on the picture is "north").
struct Frame {
  float cx = 0, cz = 0, half = 1024;
  bool mx = false, mz = false, picture = false;
};
Frame FrameFor(const std::string& map) {
  Frame f;
  if (map.empty()) return f;
  if (auto img = botllm::minimap::Get(map, false)) {
    f.cx = img->cx, f.cz = img->cz, f.half = img->half, f.mx = img->mirror_x, f.mz = img->mirror_z;
    f.picture = img->w > 0;
  }
  return f;
}

// "230 m north-east" of (fx, fz), as the level's picture shows it (up north).
std::string Direction(const Frame& f, float fx, float fz, float tx, float tz) {
  static const char* dirs[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
  float right = (tx - fx) * (f.mx ? -1.0f : 1.0f), up = (tz - fz) * (f.mz ? -1.0f : 1.0f);
  const float d = std::hypot(right, up);
  if (d < 15) return "right where you are";
  int o = int(std::lround(std::atan2(right, up) / (3.14159265f / 4))) & 7;
  char b[48];
  snprintf(b, sizeof b, "%.0f m %s of you", std::round(d / 10) * 10, dirs[o]);
  return b;
}

// The soldier under an eye in the live sample: on foot, its feet 0 to 2.5 m
// below the eye and within 2 m across; none if soldiers of both teams are there.
bool SoldierUnder(const replay::LiveSnapshot& s, const float eye[3], uint16_t* id, int* team) {
  float best = 2.0f;
  int found = -1, teams = 0;
  for (size_t i = 0; i < s.soldiers.size(); ++i) {
    const auto& p = s.soldiers[i];
    if (p.seated) continue;
    const float across = std::hypot(p.x - eye[0], p.z - eye[2]), up = eye[1] - p.y;
    if (across > 2.0f || up < -0.3f || up > 2.5f) continue;
    teams |= 1 << p.team;
    if (across < best) best = across, found = int(i);
  }
  if (found < 0 || teams == ((1 << kUS) | (1 << kJapan))) return false;
  *id = s.soldiers[size_t(found)].id, *team = s.soldiers[size_t(found)].team;
  return true;
}

// A player's team as the host knows it (under g_mu): 0 not known.
int KnownTeamLocked(uint32_t ip, uint16_t port) {
  if (ip == 0 && port == 0) return g_local.team;
  auto it = g_known.find({ip, port});
  return it == g_known.end() ? 0 : it->second.team;
}

// Tells a team's players something about a place: each known one with the way
// from where it is ("... 230 m north-east of you"), from the live sample.
void TellTeam(int team, const std::string& head, float x, float z, const std::string& tail) {
  replay::LiveSnapshot s;
  const bool live = replay::LiveCopy(s);
  const Frame f = FrameFor(live ? s.map : "");
  auto where = [&](uint16_t id) -> std::string {
    if (live)
      for (const auto& p : s.soldiers)
        if (p.id == id) return Direction(f, p.x, p.z, x, z);
    return "on the live map";
  };
  std::vector<std::pair<std::pair<uint32_t, uint16_t>, std::string>> lines;
  {
    std::lock_guard l(g_mu);
    for (auto& [k, kn] : g_known)
      if (kn.team == team) lines.push_back({k, where(kn.id)});
    if (g_local.team == team && !g_dedicated) lines.push_back({{0, 0}, where(g_local.id)});
  }
  for (auto& [k, w] : lines)
    if (g_tell) g_tell(k.first, k.second, "[commander] " + head + " " + w + tail);
  RT_INFO("commander: %s team: %s at %.0f,%.0f%s (told %zu players)", TeamName(team), head.c_str(), x, z, tail.c_str(),
          lines.size());
}

// ---- the live map (Windows: the game's own HTTP server) -------------------------

std::string Num(double v) {
  char b[32];
  snprintf(b, sizeof b, "%.2f", v);
  return b;
}
std::string JsonText(const std::string& s) {  // (ASCII, no quotes or controls)
  std::string o = "\"";
  for (char c : s)
    if (c >= ' ' && c < 127 && c != '"' && c != '\\') o += c;
  return o + "\"";
}

std::string OrdersJson() {  // (under g_mu)
  std::string o = "[";
  const auto now = Clock::now();
  bool first = true;
  for (int t = kUS; t <= kJapan; ++t) {
    const OrderState& s = g_order[t];
    if (s.o.kind == kNoOrder) continue;
    o += std::string(first ? "" : ",") + "{\"team\":" + std::to_string(t) + ",\"kind\":\"" +
         (s.o.kind == kAttack ? "attack" : "defend") + "\",\"point\":" + std::to_string(s.point) +
         ",\"x\":" + Num(s.o.x) + ",\"z\":" + Num(s.o.z) + ",\"left\":" + Num(kOrderSeconds - Since(s.at, now)) + "}";
    first = false;
  }
  return o + "]";
}

std::string LiveJson() {
  static std::mutex m;
  static std::string cached;
  static Clock::time_point cached_at;
  std::lock_guard l(m);
  const auto now = Clock::now();
  if (!cached.empty() && Since(cached_at, now) < 0.2) return cached;  // (rate-limited)
  replay::LiveSnapshot s;
  if (!replay::LiveCopy(s)) {
    cached = R"({"ok":false,"error":"no match is running on this server"})", cached_at = now;
    return cached;
  }
  const Frame f = FrameFor(s.map);
  std::ostringstream o;
  o << "{\"ok\":true,\"map\":" << JsonText(s.map) << ",\"t\":" << s.t;
  o << ",\"frame\":{\"cx\":" << Num(f.cx) << ",\"cz\":" << Num(f.cz) << ",\"half\":" << Num(f.half)
    << ",\"mx\":" << (f.mx ? "true" : "false") << ",\"mz\":" << (f.mz ? "true" : "false")
    << ",\"picture\":" << (f.picture ? "true" : "false") << "}";
  {
    std::lock_guard g(g_mu);
    o << ",\"commander\":{";
    for (int t = kUS; t <= kJapan; ++t) {
      const bool held = Fresh(g_seat[t], now);
      o << (t == kUS ? "\"us\":" : ",\"japan\":") << (held ? "true" : "false") << (t == kUS ? ",\"us_name\":" : ",\"japan_name\":")
        << JsonText(held ? g_seat[t].name : "");
    }
    o << "},\"orders\":" << OrdersJson();
  }
  o << ",\"supply\":[";
  bool first = true;
  for (const SupplyZone& z : SupplyZones()) {
    o << (first ? "" : ",") << "{\"team\":" << z.team << ",\"x\":" << Num(z.x) << ",\"z\":" << Num(z.z) << ",\"left\":" << Num(z.left) << "}";
    first = false;
  }
  o << "],\"players\":[";
  for (size_t i = 0; i < s.soldiers.size(); ++i) {
    const auto& p = s.soldiers[i];
    o << (i ? "," : "") << "{\"id\":" << p.id << ",\"team\":" << int(p.team) << ",\"x\":" << Num(p.x) << ",\"z\":" << Num(p.z)
      << ",\"yaw\":" << Num(p.yaw) << ",\"hp\":" << int(p.health) << ",\"seated\":" << int(p.seated) << "}";
  }
  o << "],\"vehicles\":[";
  for (size_t i = 0; i < s.vehicles.size(); ++i) {
    const auto& v = s.vehicles[i];
    o << (i ? "," : "") << "{\"id\":" << v.id << ",\"x\":" << Num(v.x) << ",\"z\":" << Num(v.z) << ",\"yaw\":" << Num(v.yaw) << "}";
  }
  o << "],\"points\":[";
  for (size_t i = 0; i < s.points.size(); ++i) {
    const auto& p = s.points[i];
    o << (i ? "," : "") << "{\"id\":" << p.id << ",\"owner\":" << int(p.owner) << ",\"x\":" << Num(p.x) << ",\"z\":" << Num(p.z) << "}";
  }
  o << "]}";
  cached = o.str(), cached_at = now;
  return cached;
}

// The level's picture as a PNG (made once a map, from the host's own game files).
std::string MinimapPng(const std::string& map) {
  static std::mutex m;
  static std::string for_map, png;
  std::lock_guard l(m);
  if (for_map == map && !png.empty()) return png;
  auto img = botllm::minimap::Get(map, false);
  if (!img || img->w <= 0 || img->rgb.size() < size_t(img->w) * img->h * 3) return "";
  std::string out;
  stbi_write_png_to_func([](void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size));
  }, &out, img->w, img->h, 3, img->rgb.data(), img->w * 3);
  for_map = map, png = std::move(out);
  return png;
}

// The commander's view for the page (and its poll keeps the seat).
std::string StateJson(const std::string& token) {
  std::lock_guard l(g_mu);
  const int team = TeamOfToken(token);
  if (!team) return R"({"ok":false,"error":"this link holds no commander seat (now): take one in the game with /commander us or /commander japan"})";
  Seat& s = g_seat[team];
  s.web = true, s.web_seen = Clock::now();
  // The holder's own soldier (the page marks it), when the host knows it.
  int me = -1;
  if (s.Local() && g_local.team) me = g_local.id;
  else if (auto it = g_known.find({s.ip, s.port}); it != g_known.end()) me = it->second.id;
  std::ostringstream o;
  o << "{\"ok\":true,\"team\":" << team << ",\"name\":" << JsonText(s.name) << ",\"me\":" << me << ",\"wait\":{";
  const char* actions[] = {"order", "ammo", "vehicle"};
  for (int i = 0; i < 3; ++i)
    o << (i ? "," : "") << "\"" << actions[i] << "\":" << Num(WaitLocked(team, actions[i]));
  o << "},\"cooldown\":{";
  for (int i = 0; i < 3; ++i) o << (i ? "," : "") << "\"" << actions[i] << "\":" << CooldownFor(actions[i]);
  o << "},\"vehicle_drop\":" << (VehicleDropOn() ? "true" : "false") << ",\"notes\":[";
  for (size_t i = 0; i < g_notes[team].size(); ++i) o << (i ? "," : "") << JsonText(g_notes[team][i]);
  o << "]}";
  return o.str();
}

std::string FailJson(const std::string& why, double wait = 0) {
  return "{\"ok\":false,\"error\":" + JsonText(why) + (wait > 0 ? ",\"wait\":" + Num(wait) : "") + "}";
}

// A number from the page: all of it a finite number.
bool ParseNum(const std::string& s, float* out) {
  if (s.empty() || s.size() > 16) return false;
  char* end = nullptr;
  const double v = strtod(s.c_str(), &end);
  if (!end || *end || !std::isfinite(v)) return false;
  *out = float(v);
  return true;
}

// The page's action: a = attack, defend, cancel (orders), ammo, vehicle (drops);
// x, z where it clicked.
std::string Act(const std::string& token, const std::string& action, const std::string& xs, const std::string& zs) {
  static const char* kActions[] = {"attack", "defend", "cancel", "ammo", "vehicle"};
  if (std::find_if(std::begin(kActions), std::end(kActions), [&](const char* a) { return action == a; }) == std::end(kActions))
    return FailJson("unknown action");
  int team;
  std::string name;
  {
    std::lock_guard l(g_mu);
    team = TeamOfToken(token);
    if (team) name = g_seat[team].name, g_seat[team].web = true, g_seat[team].web_seen = Clock::now();
  }
  if (!team) return FailJson("this link holds no commander seat (now)");
  if (action == "cancel") {
    std::lock_guard l(g_mu);
    if (g_order[team].o.kind == kNoOrder) return FailJson("no order to cancel");
    g_order[team] = OrderState{};
    g_order[team].o.serial = ++g_order_serial;
    Note(team, "order cancelled");
    return R"({"ok":true,"text":"order cancelled"})";
  }
  float x, z;
  if (!ParseNum(xs, &x) || !ParseNum(zs, &z)) return FailJson("bad coordinates");
  replay::LiveSnapshot s;
  if (!replay::LiveCopy(s)) return FailJson("no match is running");
  const Frame f = FrameFor(s.map);
  if (std::fabs(x - f.cx) > f.half || std::fabs(z - f.cz) > f.half) return FailJson("that is off the map");
  if (action == "attack" || action == "defend") {
    const replay::LivePoint* best = nullptr;
    float d = kFlagPickM;
    for (const auto& p : s.points)
      if (float e = std::hypot(p.x - x, p.z - z); e < d) d = e, best = &p;
    if (!best) return FailJson("click on a flag for an order");
    const int kind = action == "attack" ? kAttack : kDefend;
    double wait = 0;
    {
      std::lock_guard l(g_mu);
      if (!ActLocked(team, "order", &wait)) return FailJson("orders wait", wait);
      OrderState& o = g_order[team];
      o.o.kind = kind, o.o.x = best->x, o.o.z = best->z, o.o.serial = ++g_order_serial, o.point = best->id, o.at = Clock::now();
      Note(team, std::string(kind == kAttack ? "attack" : "defend") + " order given");
    }
    const int enemy = team == kUS ? kJapan : kUS;
    const std::string flag = best->owner == team ? "our flag" : best->owner == enemy ? (enemy == kUS ? "the US flag" : "the Japanese flag")
                                                                                      : "the neutral flag";
    TellTeam(team, std::string(kind == kAttack ? "ATTACK " : "DEFEND ") + flag, best->x, best->z, "");
    return std::string("{\"ok\":true,\"text\":") + JsonText(std::string(kind == kAttack ? "attack " : "defend ") + flag) + "}";
  }
  // The vehicle drop moves the server's jeep but the players' games keep
  // drawing it where it stood (docs/COMMANDER.md): off unless testing.
  if (action == "vehicle" && !VehicleDropOn()) return FailJson("vehicle drops are not working yet (BF_VEHICLE_DROP=1 to test)");
  // A drop: its cooldown now (given back if it can't be done).
  double wait = 0;
  {
    std::lock_guard l(g_mu);
    if (!ActLocked(team, action, &wait)) return FailJson(action == "ammo" ? "supply drops wait" : "vehicle drops wait", wait);
    Note(team, std::string(action == "ammo" ? "supply" : "vehicle") + " drop called");
  }
  QueueDrop(team, action, x, z, [team, action, x, z](bool ok, const std::string& text) {
    {
      std::lock_guard l(g_mu);
      Note(team, text);
      if (!ok) g_last_act.erase(std::make_pair(team, action));  // (nothing dropped: no wait)
    }
    if (ok) TellTeam(team, action == "ammo" ? "SUPPLY DROP:" : "VEHICLE DROP:", x, z, action == "ammo" ? " (heals for 30 s)" : "");
    RT_INFO("commander: %s team's %s drop at %.0f,%.0f: %s", TeamName(team), action.c_str(), x, z, text.c_str());
  });
  return R"({"ok":true,"text":"the drop is on its way"})";
}

// A small page that draws LiveJson, with the commander's controls when the
// link carries a token (?k=...). ASCII only: no page encoding to set.
const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Live map</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{margin:0;background:#0b0e14;color:#e8edf7;font:14px system-ui,sans-serif}
#top{padding:8px 14px;color:#8a96ad}#cmd{padding:6px 14px;display:none;background:#141b29;border-bottom:1px solid #232d42}
#cmd b{color:#fff}.chip{display:inline-block;margin:0 8px 0 0;padding:2px 8px;border-radius:9px;background:#232d42}
.ready{background:#1f5130}#notes{color:#8a96ad;font-size:12px;margin-top:4px}
#wrap{position:relative;width:900px;margin:8px auto}canvas{display:block;background:#131824;border:1px solid #232d42}
#menu{position:absolute;display:none;background:#1b2334;border:1px solid #3a4866;border-radius:6px;padding:4px;min-width:150px}
#menu button{display:block;width:100%;margin:2px 0;padding:6px 8px;background:#26314a;color:#e8edf7;border:0;border-radius:4px;text-align:left;cursor:pointer}
#menu button:disabled{opacity:.4;cursor:default}#menu button:hover:enabled{background:#34446a}
#msg{min-height:18px;color:#ffd27a}</style>
</head><body><div id="top">Live map: connecting...</div>
<div id="cmd"><div>Commander of <b id="team"></b> (<span id="who"></span>). Click a flag for an order, any place for a drop.
<span id="msg"></span></div><div id="chips"></div><div id="notes"></div></div>
<div id="wrap"><canvas id="c" width="900" height="900"></canvas><div id="menu"></div></div>
<script>
var c=document.getElementById('c'),g=c.getContext('2d'),hdr=document.getElementById('top'),menu=document.getElementById('menu');
var key=(location.search.match(/[?&]k=([0-9a-fA-F]{16})/)||[])[1]||'';
var data=null,state=null,img=null,imgMap='',view=null,flash=null;
var col=['#8a96ad','#6eafff','#ff645a'],tname=['','US','Japan'];
function uv(x,z){var f=data.frame,fx=(x-(f.cx-f.half))/(2*f.half),fz=((f.cz+f.half)-z)/(2*f.half);return [f.mx?1-fx:fx,f.mz?1-fz:fz];}
function world(u,v){var f=data.frame,fx=f.mx?1-u:u,fz=f.mz?1-v:v;return [f.cx-f.half+fx*2*f.half,f.cz+f.half-fz*2*f.half];}
function P(x,z){var q=uv(x,z);return [(q[0]-view.u0)/view.s*c.width,(q[1]-view.v0)/view.s*c.height];}
function fit(){
  var u0=1,u1=0,v0=1,v1=0,n=0;
  data.points.forEach(function(p){var q=uv(p.x,p.z);u0=Math.min(u0,q[0]);u1=Math.max(u1,q[0]);v0=Math.min(v0,q[1]);v1=Math.max(v1,q[1]);n++;});
  if(!n){view={u0:0,v0:0,s:1};return;}
  var m=250/(2*data.frame.half),s=Math.max(u1-u0,v1-v0)+2*m,cu=(u0+u1)/2,cv=(v0+v1)/2;s=Math.min(1,Math.max(s,0.1));
  view={u0:Math.min(Math.max(cu-s/2,0),1-s),v0:Math.min(Math.max(cv-s/2,0),1-s),s:s};
}
function draw(){
  g.clearRect(0,0,c.width,c.height);
  if(!data)return;
  if(!data.ok){hdr.textContent=data.error;return;}
  if(!view||view.map!==data.map){fit();view.map=data.map;}
  if(data.frame.picture&&imgMap!==data.map){imgMap=data.map;img=new Image();img.src='/minimap.png?m='+encodeURIComponent(data.map);}
  if(img&&img.complete&&img.naturalWidth){g.globalAlpha=0.75;g.drawImage(img,view.u0*img.naturalWidth,view.v0*img.naturalHeight,view.s*img.naturalWidth,view.s*img.naturalHeight,0,0,c.width,c.height);g.globalAlpha=1;}
  var k=c.width/(view.s*2*data.frame.half);
  (data.supply||[]).forEach(function(z){var p=P(z.x,z.z);g.beginPath();g.arc(p[0],p[1],15*k,0,6.3);g.fillStyle='rgba(80,220,120,0.25)';g.fill();g.strokeStyle='#50dc78';g.stroke();});
  data.points.forEach(function(p){var q=P(p.x,p.z);
    g.beginPath();g.arc(q[0],q[1],12,0,6.3);g.fillStyle=col[p.owner]||col[0];g.globalAlpha=0.55;g.fill();
    g.globalAlpha=1;g.strokeStyle=col[p.owner]||col[0];g.lineWidth=2;g.stroke();});
  (data.orders||[]).forEach(function(o){var q=P(o.x,o.z);g.beginPath();g.arc(q[0],q[1],22,0,6.3);g.setLineDash([6,4]);g.strokeStyle=col[o.team];g.lineWidth=3;g.stroke();g.setLineDash([]);
    g.font='bold 13px system-ui';g.fillStyle=col[o.team];g.fillText(tname[o.team]+' '+o.kind.toUpperCase(),q[0]+26,q[1]-10);g.font='14px system-ui';});
  data.vehicles.forEach(function(v){var q=P(v.x,v.z);g.fillStyle='#e8edf7';g.fillRect(q[0]-4,q[1]-4,8,8);});
  data.players.forEach(function(p){var q=P(p.x,p.z);g.beginPath();g.arc(q[0],q[1],4,0,6.3);g.fillStyle=col[p.team]||col[0];g.fill();
    if(state&&state.ok&&state.me===p.id){g.beginPath();g.arc(q[0],q[1],8,0,6.3);g.strokeStyle='#fff';g.lineWidth=2;g.stroke();g.fillStyle='#fff';g.fillText('you',q[0]+10,q[1]+4);}});
  if(flash&&Date.now()<flash.until){var q=P(flash.x,flash.z);g.beginPath();g.arc(q[0],q[1],8,0,6.3);g.strokeStyle='#ffd27a';g.lineWidth=2;g.stroke();}
  var cm=data.commander;
  hdr.textContent=data.map+' - US commander: '+(cm.us?cm.us_name:'free')+' - Japan commander: '+(cm.japan?cm.japan_name:'free')+(key?'':' - take a seat in the game: /commander us or /commander japan');
}
function chips(){
  var box=document.getElementById('cmd');
  if(!key){box.style.display='none';return;}
  box.style.display='block';
  if(!state||!state.ok){document.getElementById('team').textContent='nobody';document.getElementById('chips').textContent=state?state.error:'';return;}
  document.getElementById('team').textContent=tname[state.team];document.getElementById('who').textContent=state.name;
  var names={order:'Orders',ammo:'Supply drop',vehicle:'Vehicle drop'},h='';
  for(var a in names){var w=state.wait[a];h+='<span class="chip'+(w>0?'':' ready')+'">'+names[a]+': '+(w>0?Math.ceil(w)+' s':'ready')+'</span>';}
  document.getElementById('chips').innerHTML=h;
  document.getElementById('notes').textContent=state.notes.slice(-3).join(' / ');
}
function say(t){document.getElementById('msg').textContent=t;}
function act(a,x,z){menu.style.display='none';
  fetch('/api/act?k='+key+'&a='+a+'&x='+x.toFixed(1)+'&z='+z.toFixed(1),{cache:'no-store'}).then(function(r){return r.json();})
  .then(function(r){say(r.ok?r.text:(r.error+(r.wait?' '+Math.ceil(r.wait)+' s':'')));poll();}).catch(function(){say('no answer');});}
c.addEventListener('click',function(e){
  if(!key||!data||!data.ok||!state||!state.ok)return;
  var r=c.getBoundingClientRect(),px=(e.clientX-r.left)*c.width/r.width,py=(e.clientY-r.top)*c.height/r.height;
  var w=world(view.u0+px/c.width*view.s,view.v0+py/c.height*view.s),x=w[0],z=w[1];flash={x:x,z:z,until:Date.now()+4000};
  var near=null,d=60;data.points.forEach(function(p){var e2=Math.hypot(p.x-x,p.z-z);if(e2<d){d=e2;near=p;}});
  var ow=state.wait.order>0,h='';
  if(near){h+='<button data-a="attack"'+(ow?' disabled':'')+'>Attack this flag</button><button data-a="defend"'+(ow?' disabled':'')+'>Defend this flag</button>';}
  h+='<button data-a="ammo"'+(state.wait.ammo>0?' disabled':'')+'>Supply drop here</button><button data-a="vehicle"'+(state.wait.vehicle>0||!state.vehicle_drop?' disabled':'')+'>Vehicle drop here'+(state.vehicle_drop?'':' (not yet)')+'</button>';
  if((data.orders||[]).some(function(o){return o.team===state.team;}))h+='<button data-a="cancel">Cancel our order</button>';
  h+='<button data-a="close">Close</button>';
  menu.innerHTML=h;menu.style.left=Math.min(px+8,c.width-170)+'px';menu.style.top=Math.min(py+8,c.height-230)+'px';menu.style.display='block';
  var tx=near?near.x:x,tz=near?near.z:z;
  Array.prototype.forEach.call(menu.querySelectorAll('button'),function(b){b.onclick=function(ev){ev.stopPropagation();var a=b.getAttribute('data-a');
    if(a==='close'){menu.style.display='none';return;}act(a,(a==='attack'||a==='defend')?tx:x,(a==='attack'||a==='defend')?tz:z);};});
  draw();
});
function poll(){if(!key)return;fetch('/api/state?k='+key,{cache:'no-store'}).then(function(r){return r.json();}).then(function(s){state=s;chips();}).catch(function(){});}
function tick(){fetch('/livemap.json',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){data=d;draw();}).catch(function(){hdr.textContent='no answer from the server';});poll();}
tick();setInterval(tick,1000);
</script></body></html>
)HTML";

#ifdef _WIN32
bool LiveDisabled() { return std::getenv("BF_NO_LIVEMAP") != nullptr; }

// Requests from one address: at most 60 in 10 s (the page asks twice a
// second), and an action at most every 0.3 s. Kept for 256 addresses.
bool Allowed(uint32_t ip, bool action) {
  struct Rate {
    Clock::time_point window, last_act;
    int n = 0;
  };
  static std::map<uint32_t, Rate> rates;
  const auto now = Clock::now();
  if (rates.size() > 256)
    for (auto it = rates.begin(); it != rates.end();)
      it = Since(it->second.window, now) > 60 ? rates.erase(it) : std::next(it);
  if (rates.size() > 256) return false;
  Rate& r = rates[ip];
  if (Since(r.window, now) > 10) r.window = now, r.n = 0;
  if (++r.n > 60) return false;
  if (action) {
    if (Since(r.last_act, now) < 0.3) return false;
    r.last_act = now;
  }
  return true;
}

// The value of a query parameter (only [0-9A-Za-z.-]: anything else is not taken).
std::string Param(const std::string& query, const char* name) {
  const std::string want = std::string(name) + "=";
  for (size_t at = 0; at < query.size();) {
    size_t amp = query.find('&', at);
    if (amp == std::string::npos) amp = query.size();
    if (query.compare(at, want.size(), want) == 0) {
      std::string v = query.substr(at + want.size(), amp - at - want.size());
      for (char ch : v)
        if (!isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-') return "";
      return v.substr(0, 64);
    }
    at = amp + 1;
  }
  return "";
}

void Answer(SOCKET c, uint32_t from) {
  DWORD to = 2000;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
  std::string req;
  char buf[512];
  while (req.size() < 2048 && req.find("\r\n\r\n") == std::string::npos) {
    int n = recv(c, buf, sizeof buf, 0);
    if (n <= 0) break;
    req.append(buf, size_t(n));
  }
  const std::string line = req.substr(0, req.find("\r\n"));
  int status = 200;
  const char* type = "text/html; charset=utf-8";
  std::string body;
  if (line.compare(0, 4, "GET ") != 0) {
    status = 405, type = "text/plain", body = "GET only\n";
  } else {
    size_t sp = line.find(' ', 4);
    std::string path = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4), query;
    if (size_t q = path.find('?'); q != std::string::npos) query = path.substr(q + 1), path.resize(q);
    const bool action = path == "/api/act";
    if (!Allowed(from, action)) {
      status = 429, type = "application/json", body = FailJson("too many requests: wait a moment");
    } else if (path == "/") {
      body = kPage;
    } else if (path == "/livemap.json") {
      type = "application/json";
      body = LiveJson();
    } else if (path == "/minimap.png") {
      replay::LiveSnapshot s;
      body = replay::LiveCopy(s) ? MinimapPng(s.map) : "";
      if (body.empty()) status = 404, type = "text/plain", body = "no picture of this level\n";
      else type = "image/png";
    } else if (path == "/api/state") {
      type = "application/json";
      body = StateJson(Param(query, "k"));
    } else if (action) {
      type = "application/json";
      body = Act(Param(query, "k"), Param(query, "a"), Param(query, "x"), Param(query, "z"));
    } else {
      status = 404, type = "text/plain", body = "not found\n";
    }
  }
  const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 429 ? "Too Many Requests" : "Method Not Allowed";
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
  head += body;
  for (size_t at = 0; at < head.size();) {
    int n = send(c, head.data() + at, int(head.size() - at), 0);
    if (n <= 0) break;
    at += size_t(n);
  }
}

void ServeLive() {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET listener = INVALID_SOCKET;
  uint16_t bound = 0;
  for (;;) {
    const bool hosting = (g_dedicated || g_lan_host) && !g_join_address && !LiveDisabled();
    if (!hosting) {  // (a joiner, or a game not hosting: nothing to serve)
      if (listener != INVALID_SOCKET) closesocket(listener), listener = INVALID_SOCKET;
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    const uint16_t want = uint16_t(kLivePortBase + g_port_offset);
    if (listener == INVALID_SOCKET || bound != want) {
      if (listener != INVALID_SOCKET) closesocket(listener), listener = INVALID_SOCKET;
      listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(want);
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
          listen(listener, 8) != 0) {
        RT_WARN("livemap: TCP port %u is not available (the live map is off)", unsigned(want));
        if (listener != INVALID_SOCKET) closesocket(listener), listener = INVALID_SOCKET;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        continue;
      }
      bound = want;
      RT_INFO("livemap: http://<this PC's address>:%u/ shows the live map", unsigned(want));
    }
    fd_set r;
    FD_ZERO(&r);
    FD_SET(listener, &r);
    timeval tv{0, 500000};
    if (select(0, &r, nullptr, nullptr, &tv) > 0) {
      sockaddr_in from{};
      int len = sizeof from;
      SOCKET c = accept(listener, reinterpret_cast<sockaddr*>(&from), &len);
      if (c != INVALID_SOCKET) {
        Answer(c, from.sin_addr.s_addr);  // (one at a time: the requests are bounded by that)
        closesocket(c);
      }
    }
  }
}
#endif

void Expire() {
  int64_t level_seen = -1;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int64_t level = g_level_open_ms.load();
    std::vector<std::string> notes;
    // The host's own player: its soldier under its camera.
    float eye[3];
    replay::LiveSnapshot snap;
    uint16_t id = 0;
    int team = 0;
    const bool local_found = !g_dedicated && !g_join_address && xgpu::CameraEye(eye) && replay::LiveCopy(snap) &&
                             SoldierUnder(snap, eye, &id, &team);
    {
      std::lock_guard l(g_mu);
      const auto now = Clock::now();
      if (level != level_seen) {  // a level opened or closed: the seats, teams and orders go
        level_seen = level;
        for (Seat& s : g_seat) s = Seat{};
        g_last_act.clear(), g_known.clear(), g_local = Known{};
        for (int t = kUS; t <= kJapan; ++t) g_order[t] = OrderState{}, g_notes[t].clear();
      }
      if (local_found) {
        if (g_local.team != team) RT_INFO("commander: this game's player is on the %s team", TeamName(team));
        g_local.id = id, g_local.team = team, g_local.seen = now;
      }
      for (int t = kUS; t <= kJapan; ++t) {
        Seat& s = g_seat[t];
        const std::string who = s.name;
        const bool web = s.web;
        if (s.held && !Fresh(s, now))
          notes.push_back(std::string(TeamName(t)) + " commander seat is free: " + who +
                          (web ? "'s map page closed" : " sent no input for 20 s"));
        OrderState& o = g_order[t];
        if (o.o.kind != kNoOrder && Since(o.at, now) > kOrderSeconds) {
          o = OrderState{};
          o.o.serial = ++g_order_serial;
          Note(t, "the order ran out");
        }
      }
    }
    for (const std::string& n : notes)
      if (g_announce) g_announce(n);
  }
}
}  // namespace

bool Available() { return std::strcmp(lan::GameTag(), "BF43") == 0; }

int ParseTeam(const std::string& word) {
  const std::string w = Lower(word);
  if (w == "us" || w == "1" || w == "america" || w == "american") return kUS;
  if (w == "japan" || w == "jp" || w == "japanese" || w == "2") return kJapan;
  return kNoTeam;
}

const char* TeamName(int team) { return team == kUS ? "US" : team == kJapan ? "Japan" : "no team"; }

Reply Request(uint32_t ip, uint16_t port, const std::string& name, const std::string& body, bool local_host) {
  std::lock_guard l(g_mu);
  const auto now = Clock::now();
  for (Seat& s : g_seat) Fresh(s, now);
  Reply r;
  const std::string cmd = body.substr(0, 1);
  std::string arg = body.size() > 2 ? body.substr(2) : "";
  auto mine = [&] {
    for (int t = kUS; t <= kJapan; ++t)
      if (SameHolder(g_seat[t], ip, port)) return t;
    return 0;
  };
  const int before = mine();
  if (cmd == "s") {
    std::string text = "commanders:";
    for (int t = kUS; t <= kJapan; ++t)
      text += std::string(" ") + TeamName(t) + " " + (g_seat[t].held ? g_seat[t].name : "free") + ";";
    if (before) text += " yours: the link is on your screen again";
    r.text = text;
  } else if (cmd == "k") {
    if (before) g_seat[before].seen = now;  // (a keep-alive: no answer but the seat)
  } else if (cmd == "l") {
    if (!before) {
      r.text = "you hold no commander seat";
    } else {
      r.text = name + " leaves the " + TeamName(before) + " commander seat";
      g_seat[before] = Seat{};
      r.changed = true;
    }
  } else if (cmd == "t") {
    const int team = ParseTeam(arg);
    // The player's own team only (the dedicated server's console: either).
    const bool console = local_host && g_dedicated && ip == 0 && port == 0;
    const int plays = KnownTeamLocked(ip, port);
    if (!team) {
      r.text = "COMMANDER: /commander us or japan";
    } else if (!console && !plays) {
      r.text = "the server doesn't know your team yet: deploy and walk a few steps on foot, then try again";
    } else if (!console && plays != team) {
      r.text = std::string("you play for ") + TeamName(plays) + ": /commander " + (plays == kUS ? "us" : "japan");
    } else if (before == team) {
      r.text = std::string("you already hold the ") + TeamName(team) + " commander seat";
    } else if (g_seat[team].held) {
      r.text = std::string("the ") + TeamName(team) + " commander seat is held by " + g_seat[team].name;
    } else {
      if (before) g_seat[before] = Seat{};  // (one seat a player)
      Seat& s = g_seat[team];
      s.held = true, s.ip = ip, s.port = port, s.name = name, s.seen = s.taken = now, s.token = NewToken();
      r.text = name + " takes the " + TeamName(team) + " commander seat";
      r.changed = true;
      RT_INFO("commander: %s takes the %s seat", name.c_str(), TeamName(team));
    }
  } else if (cmd == "c" && local_host) {
    const int s = atoi(arg.c_str());
    if (s < 5 || s > 600) {
      r.text = "COMMANDER: cooldown 5 to 600 seconds";
    } else {
      g_cooldown = s;
      r.text = "commander drops' cooldown " + std::to_string(s) + " s (vehicles twice that, orders " +
               std::to_string(int(kOrderCooldownS)) + " s)";
    }
  } else {
    r.text = "COMMANDER: /commander us|japan takes a seat, /commander leave gives it up, /commander alone shows them";
  }
  r.holds = mine();
  if (r.holds) r.token = g_seat[r.holds].token;
  return r;
}

bool Seated(int team) {
  if (team < kUS || team > kJapan) return false;
  std::lock_guard l(g_mu);
  return Fresh(g_seat[team], Clock::now());
}

bool KeepAliveDue() {
  const int team = g_joiner_team.load();
  if (!team) return false;
  const auto now = Clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_keep).count() < kKeepEveryMs) return false;
  if (IdleSeconds() >= kExpireS) return false;  // (no input: the seat is let go)
  g_last_keep = now;
  return true;
}

std::string OnHostReply(const std::string& body) {
  if (body.size() >= 2 && body[0] == 'h') {
    g_joiner_team = body[1] - '0';
    const size_t colon = body.find(':');
    const std::string token = colon == std::string::npos ? "" : body.substr(colon + 1);
    if (ValidToken(token) && token != g_joiner_token) {
      g_joiner_token = token;
      // The host's live map: its address, its port offset (as its game port's).
      const int game = ntohs(g_join_port);
      const int offset = game > 1003 ? game - 1003 : g_base_port_offset;
      char ip[32] = {};
      in_addr a{};
      a.s_addr = g_join_address;
      inet_ntop(AF_INET, &a, ip, sizeof ip);
      const std::string link = "http://" + std::string(ip) + ":" + std::to_string(kLivePortBase + offset) + "/?k=" + token;
      CopyToClipboard(link);
      {
        std::lock_guard l(g_mu);
        g_joiner_link = link;
      }
      RT_INFO("commander: your page: %s", link.c_str());
      return "[commander] your map page (copied to the clipboard, open it in a browser): " + link;
    }
  } else if (!body.empty() && body[0] == 'n') {
    std::lock_guard l(g_mu);
    g_joiner_team = 0, g_joiner_token.clear(), g_joiner_link.clear();
  }
  return "";
}

std::string EyeDue() {
  const auto now = Clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_eye).count() < kEyeEveryMs) return "";
  float eye[3];
  if (g_in_menus || !xgpu::CameraEye(eye) || !std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]))
    return "";
  g_last_eye = now;
  char b[64];
  snprintf(b, sizeof b, "%.2f,%.2f,%.2f", eye[0], eye[1], eye[2]);
  return b;
}

void OnPlayerEye(uint32_t ip, uint16_t port, const std::string& body) {
  float eye[3];
  if (sscanf(body.c_str(), "%f,%f,%f", &eye[0], &eye[1], &eye[2]) != 3) return;
  for (float v : eye)
    if (!std::isfinite(v) || std::fabs(v) > 1e5f) return;
  replay::LiveSnapshot s;
  uint16_t id = 0;
  int team = 0;
  if (!replay::LiveCopy(s) || !SoldierUnder(s, eye, &id, &team)) return;
  std::lock_guard l(g_mu);
  if (g_known.size() >= 64 && !g_known.count({ip, port})) return;  // (made-up senders)
  Known& k = g_known[{ip, port}];
  if (k.team != team) RT_INFO("commander: a joiner (chat port %u) is on the %s team", unsigned(port), TeamName(team));
  k.id = id, k.team = team, k.seen = Clock::now();
}

std::string PageLink(const std::string& token) {
  char ip[32] = "<this PC's address>";
  if (g_lan_ip) {
    in_addr a{};
    a.s_addr = g_lan_ip;
    inet_ntop(AF_INET, &a, ip, sizeof ip);
  }
  return "http://" + std::string(ip) + ":" + std::to_string(kLivePortBase + g_port_offset) + "/?k=" + token;
}

std::string MyLink() {
  std::lock_guard l(g_mu);
  if (g_joiner_team.load()) return g_joiner_link;
  for (int t = kUS; t <= kJapan; ++t)
    if (g_seat[t].held && g_seat[t].Local()) return PageLink(g_seat[t].token);
  return "";
}

void CopyToClipboard(const std::string& text) {
#ifdef _WIN32
  if (g_dedicated || !OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
    memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
#else
  (void)text;
#endif
}

bool TryAct(int team, uint32_t ip, uint16_t port, const std::string& action, double* wait_seconds) {
  std::lock_guard l(g_mu);
  if (team < kUS || team > kJapan || !SameHolder(g_seat[team], ip, port) || !Fresh(g_seat[team], Clock::now())) {
    if (wait_seconds) *wait_seconds = 0;
    return false;  // not this team's commander
  }
  return ActLocked(team, action, wait_seconds);
}

int Cooldown() { return g_cooldown; }

int CooldownFor(const std::string& action) {
  if (action == "order") return int(kOrderCooldownS);
  if (action == "vehicle") return 2 * g_cooldown;
  return g_cooldown;
}

Order CurrentOrder(int team) {
  if (team < kUS || team > kJapan) return {};
  std::lock_guard l(g_mu);
  return g_order[team].o;
}

void Start(std::function<void(const std::string&)> announce,
           std::function<void(uint32_t ip, uint16_t port, const std::string&)> tell) {
  std::call_once(g_once, [announce, tell] {
    if (!Available()) return;  // (Bad Company: no seats, no live map server)
    g_announce = announce, g_tell = tell;
    std::thread(Expire).detach();
#ifdef _WIN32
    std::thread(ServeLive).detach();
#endif
  });
}
}  // namespace rt::commander
