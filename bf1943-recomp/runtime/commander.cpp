// COMMANDER SEATS and the LIVE MAP (commander.h).
#include "commander.h"

#include "guest.h"
#include "kernel.h"
#include "lan.h"
#include "replay.h"

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
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace rt::commander {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kExpireS = 20.0;    // a seat with no keep-alive this long is free
constexpr int kKeepEveryMs = 5000;   // a joiner's keep-alive period
constexpr int kLivePortBase = 19440;  // TCP; + the port offset

struct Seat {
  bool held = false;
  uint32_t ip = 0;     // network order; 0 and port 0: this game's own player (the host)
  uint16_t port = 0;   // the chat port
  std::string name;
  Clock::time_point seen;
  bool Local() const { return ip == 0 && port == 0; }
};

std::mutex g_mu;
Seat g_seat[3];  // by team
std::map<std::pair<int, std::string>, Clock::time_point> g_last_act;  // (team, action): last allowed
int g_cooldown = 60;
std::atomic<int> g_joiner_team{0};  // a joiner: the seat this game holds (0 none)
Clock::time_point g_last_keep;      // (a joiner's last keep-alive)
std::once_flag g_once;

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

// A seat still held (under g_mu). A local seat is kept by the host's input.
bool Fresh(Seat& s, Clock::time_point now) {
  if (!s.held) return false;
  if (s.Local() && IdleSeconds() < kExpireS) s.seen = now;
  if (std::chrono::duration<double>(now - s.seen).count() <= kExpireS) return true;
  s = Seat{};
  return false;
}

bool SameHolder(const Seat& s, uint32_t ip, uint16_t port) { return s.held && s.ip == ip && s.port == port; }

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
  return s;
}

// ---- the live map (Windows: the game's own HTTP server) -------------------------

std::string Num(double v) {
  char b[32];
  snprintf(b, sizeof b, "%.2f", v);
  return b;
}

std::string LiveJson() {
  static std::mutex m;
  static std::string cached;
  static Clock::time_point cached_at;
  std::lock_guard l(m);
  const auto now = Clock::now();
  if (!cached.empty() && std::chrono::duration<double>(now - cached_at).count() < 0.2) return cached;  // (rate-limited)
  replay::LiveSnapshot s;
  if (!replay::LiveCopy(s)) {
    cached = R"({"ok":false,"error":"no match is running on this server"})", cached_at = now;
    return cached;
  }
  std::ostringstream o;
  o << "{\"ok\":true,\"map\":\"";
  for (char c : s.map) if (c != '"' && c != '\\' && c >= ' ') o << c;
  o << "\",\"t\":" << s.t << ",\"commander\":{\"us\":" << (Seated(kUS) ? "true" : "false")
    << ",\"japan\":" << (Seated(kJapan) ? "true" : "false") << "},\"players\":[";
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

// A small page that draws LiveJson (ASCII only: no page encoding to set).
const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Live map</title>
<style>body{margin:0;background:#0b0e14;color:#e8edf7;font:14px system-ui,sans-serif}
#top{padding:10px 16px;color:#8a96ad}canvas{display:block;margin:0 auto;background:#131824;border:1px solid #232d42}</style>
</head><body><div id="top">Live map: connecting...</div><canvas id="c" width="900" height="900"></canvas>
<script>
var c=document.getElementById('c'),g=c.getContext('2d'),hdr=document.getElementById('top');
var box=null;
function draw(d){
  g.clearRect(0,0,c.width,c.height);
  if(!d.ok){hdr.textContent=d.error;return;}
  var pts=[];
  d.players.forEach(function(p){pts.push([p.x,p.z]);});
  d.vehicles.forEach(function(v){pts.push([v.x,v.z]);});
  d.points.forEach(function(p){pts.push([p.x,p.z]);});
  if(!pts.length){hdr.textContent=d.map+': no units yet';return;}
  var x0=Infinity,x1=-Infinity,z0=Infinity,z1=-Infinity;
  pts.forEach(function(q){x0=Math.min(x0,q[0]);x1=Math.max(x1,q[0]);z0=Math.min(z0,q[1]);z1=Math.max(z1,q[1]);});
  var pad=40,s=Math.max(x1-x0,z1-z0,50);
  if(!box){box={x0:x0,z0:z0,s:s};}
  box.x0=Math.min(box.x0,x0);box.z0=Math.min(box.z0,z0);box.s=Math.max(box.s,s);
  var k=(c.width-2*pad)/box.s;
  function X(x){return pad+(x-box.x0)*k;}
  function Y(z){return c.height-pad-(z-box.z0)*k;}
  var col=['#8a96ad','#6eafff','#ff645a'];
  d.points.forEach(function(p){
    g.beginPath();g.arc(X(p.x),Y(p.z),12,0,6.3);g.fillStyle=col[p.owner]||col[0];g.globalAlpha=0.5;g.fill();
    g.globalAlpha=1;g.strokeStyle=col[p.owner]||col[0];g.lineWidth=2;g.stroke();
    g.fillStyle='#e8edf7';g.fillText('P'+p.id,X(p.x)+14,Y(p.z)+4);
  });
  d.vehicles.forEach(function(v){g.fillStyle='#e8edf7';g.fillRect(X(v.x)-4,Y(v.z)-4,8,8);});
  d.players.forEach(function(p){
    g.beginPath();g.arc(X(p.x),Y(p.z),4,0,6.3);g.fillStyle=col[p.team]||col[0];g.fill();
  });
  hdr.textContent=d.map+' - US commander: '+(d.commander.us?'seated':'free')+' - Japan commander: '+(d.commander.japan?'seated':'free');
}
function tick(){fetch('/livemap.json',{cache:'no-store'}).then(function(r){return r.json();}).then(draw).catch(function(){hdr.textContent='no answer from the server';});}
tick();setInterval(tick,1000);
</script></body></html>
)HTML";

#ifdef _WIN32
bool LiveDisabled() { return std::getenv("BF_NO_LIVEMAP") != nullptr; }

void Answer(SOCKET c) {
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
    std::string path = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
    if (size_t q = path.find('?'); q != std::string::npos) path.resize(q);
    if (path == "/") {
      body = kPage;
    } else if (path == "/livemap.json") {
      type = "application/json";
      body = LiveJson();
    } else {
      status = 404, type = "text/plain", body = "not found\n";
    }
  }
  const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Method Not Allowed";
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
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
      SOCKET c = accept(listener, nullptr, nullptr);
      if (c != INVALID_SOCKET) {
        Answer(c);  // (one at a time: the requests are bounded by that)
        closesocket(c);
      }
    }
  }
}
#endif

void Expire(std::function<void(const std::string&)> announce) {
  int64_t level_seen = -1;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int64_t level = g_level_open_ms.load();
    std::vector<std::string> notes;
    {
      std::lock_guard l(g_mu);
      if (level != level_seen) {  // a level opened or closed: the seats are free
        level_seen = level;
        for (Seat& s : g_seat) s = Seat{};
        g_last_act.clear();
      }
      const auto now = Clock::now();
      for (int team = kUS; team <= kJapan; ++team) {
        Seat& s = g_seat[team];
        const std::string who = s.name;
        if (s.held && !Fresh(s, now))
          notes.push_back(std::string(TeamName(team)) + " commander seat is free: " + who + " sent no input for 20 s");
      }
    }
    for (const std::string& n : notes) announce(n);
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
    if (!team) {
      r.text = "COMMANDER: /commander us or japan";
    } else if (before == team) {
      r.text = std::string("you already hold the ") + TeamName(team) + " commander seat";
    } else if (g_seat[team].held) {
      r.text = std::string("the ") + TeamName(team) + " commander seat is held by " + g_seat[team].name;
    } else {
      if (before) g_seat[before] = Seat{};  // (one seat a player)
      Seat& s = g_seat[team];
      s.held = true, s.ip = ip, s.port = port, s.name = name, s.seen = now;
      r.text = name + " takes the " + TeamName(team) + " commander seat";
      r.changed = true;
    }
  } else if (cmd == "c" && local_host) {
    const int s = atoi(arg.c_str());
    if (s < 5 || s > 600) {
      r.text = "COMMANDER: cooldown 5 to 600 seconds";
    } else {
      g_cooldown = s;
      r.text = "commander cooldown " + std::to_string(s) + " s";
    }
  } else {
    r.text = "COMMANDER: /commander us|japan takes a seat, /commander leave gives it up, /commander alone shows them";
  }
  r.holds = mine();
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

void OnHostReply(const std::string& body) {
  if (body.size() >= 2 && body[0] == 'h') g_joiner_team = body[1] - '0';
  else if (!body.empty() && body[0] == 'n') g_joiner_team = 0;
}

bool TryAct(int team, uint32_t ip, uint16_t port, const std::string& action, double* wait_seconds) {
  std::lock_guard l(g_mu);
  const auto now = Clock::now();
  if (team < kUS || team > kJapan || !SameHolder(g_seat[team], ip, port) || !Fresh(g_seat[team], now)) {
    if (wait_seconds) *wait_seconds = 0;
    return false;  // not this team's commander
  }
  auto key = std::make_pair(team, action);
  auto it = g_last_act.find(key);
  if (it != g_last_act.end()) {
    const double since = std::chrono::duration<double>(now - it->second).count();
    if (since < g_cooldown) {
      if (wait_seconds) *wait_seconds = g_cooldown - since;
      return false;
    }
  }
  g_last_act[key] = now;
  if (wait_seconds) *wait_seconds = 0;
  return true;
}

int Cooldown() {
  std::lock_guard l(g_mu);
  return g_cooldown;
}

void Start(std::function<void(const std::string&)> announce) {
  std::call_once(g_once, [announce] {
    if (!Available()) return;  // (Bad Company: no seats, no live map server)
    std::thread(Expire, announce).detach();
#ifdef _WIN32
    std::thread(ServeLive).detach();
#endif
  });
}
}  // namespace rt::commander
