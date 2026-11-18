#include "admin_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "admin.h"
#include "bot_players.h"
#include "kernel.h"
#include "lan.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include "posix_net.h"
#endif

namespace rt {
std::string g_admin_password;
std::atomic<uint16_t> g_client_game_port{0};
bool g_admin_console = false;
std::atomic<bool> g_leaving_level{false};
extern uint32_t g_lan_ip;  // net.cpp
extern std::mutex g_host_map_mutex;  // (g_server_name, g_server_password: lan.cpp)
}  // namespace rt

namespace rt::admin {
namespace {
using Clock = std::chrono::steady_clock;

struct Player {
  std::string name, id;
  uint32_t ip = 0;          // network order
  uint16_t chat_port = 0;   // host order
  uint16_t game_port = 0;   // host order (its own: behind a router the host sees another)
  Clock::time_point seen;
};
struct Ban {
  std::string id, address, name;  // "-" for none
};
struct Block {
  uint32_t ip;
  uint16_t port;  // network order; 0: every port of the address
  Clock::time_point until;  // (max: a ban)
};

std::mutex g_mutex;
std::map<std::pair<uint32_t, uint16_t>, Player> g_players;  // (ip, chat port)
std::vector<Ban> g_bans;
std::vector<Block> g_blocks;
std::atomic<bool> g_any_block{false};
bool g_bans_loaded = false;

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
  return s;
}
std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string IpText(uint32_t ip) {
  char buf[32] = {};
  in_addr a{};
  a.s_addr = ip;
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}
// This PC (loopback, its LAN address): players there are told apart by port.
bool Local(uint32_t ip) { return (ntohl(ip) >> 24) == 127 || ip == 0 || ip == g_lan_ip; }
// Names and ids go into tab-separated lines and '|'-separated packets.
std::string Clean(std::string s, size_t max) {
  for (char& c : s)
    if (c == '|' || c == '\t' || c == '\n' || c == '\r') c = ' ';
  s = Trim(s);
  if (s.size() > max) s.resize(max);
  return s.empty() ? "-" : s;
}

std::filesystem::path BansPath() { return g_user_root / "bans.txt"; }
void LoadBansLocked() {
  if (g_bans_loaded) return;
  g_bans_loaded = true;
  std::ifstream f(BansPath());
  std::string line;
  bool v2 = false;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "#v2") v2 = true;  // (written with ids' leading '#' and '\' escaped; older files are not)
    if (line.empty() || line[0] == '#') continue;
    if (v2 && line[0] == '\\') line.erase(0, 1);  // (an id starting with '#' or '\': SaveBansLocked)
    Ban b;
    std::stringstream ss(line);
    std::getline(ss, b.id, '\t');
    std::getline(ss, b.address, '\t');
    std::getline(ss, b.name);
    if (b.id.empty()) continue;
    g_bans.push_back(b);
  }
  if (!g_bans.empty()) RT_INFO("admin: %zu bans (%s)", g_bans.size(), BansPath().u8string().c_str());
  // The banned addresses (not this PC's): dropped from the start.
  for (const Ban& b : g_bans) {
    in_addr a{};
    if (b.address != "-" && inet_pton(AF_INET, b.address.c_str(), &a) == 1 && !Local(a.s_addr))
      g_blocks.push_back({a.s_addr, 0, Clock::time_point::max()}), g_any_block = true;
  }
}
void SaveBansLocked() {
  std::error_code ec;
  std::filesystem::create_directories(g_user_root, ec);
  std::ofstream f(BansPath(), std::ios::trunc);
  auto flat = [](std::string s) {  // (a tab or a line end would split the line)
    for (char& c : s)
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
  };
  f << "#v2\n# Banned players: id <tab> address <tab> name (\"-\": not known). /unban removes them.\n";
  // The id is the player's own: one starting with '#' would read as a comment,
  // so such a line (and one starting with '\') gets a '\' in front.
  for (const Ban& b : g_bans)
    f << (!b.id.empty() && (b.id[0] == '#' || b.id[0] == '\\') ? "\\" : "") << flat(b.id) << '\t'
      << flat(b.address) << '\t' << flat(b.name) << '\n';
}
bool IsBanned(const Player& p) {
  for (const Ban& b : g_bans) {
    if (b.id != "-" && b.id == p.id) return true;
    if (b.id == "-" && b.name != "-" && Lower(b.name) == Lower(p.name)) return true;
    if (b.address != "-" && b.address == IpText(p.ip) && !Local(p.ip)) return true;
  }
  return false;
}

// Its game packets dropped for that long (a remote player's every port: a
// router changes them; one on this PC by its own port).
void BlockLocked(const Player& p, Clock::time_point until) {
  if (Local(p.ip) && !p.game_port) return;  // (not told: it can only be asked to leave)
  g_blocks.push_back({p.ip, Local(p.ip) ? htons(p.game_port) : uint16_t(0), until});
  g_any_block = true;
}

// Sends a joiner one of the host's notices ("BF43K!..." and so on).
void Tell(const Player& p, const std::string& packet) {
  if (p.chat_port) SendPacket(p.ip, p.chat_port, packet);
}

// Who a name means: "BOT n" (or botn), a player's name (or the start of one),
// an address, or its number in /players. Players: index into the list; bots:
// -(n) for BOT n.
struct Target {
  int bot = 0;  // 1..: BOT n
  std::pair<uint32_t, uint16_t> key{0, 0};
  bool found = false;
  std::string error;
};
Target FindLocked(const std::string& what_raw) {
  Target t;
  std::string what = Lower(Trim(what_raw));
  if (what.empty()) {
    t.error = "who? (a name from /players)";
    return t;
  }
  // BOT n
  std::string w = what;
  w.erase(std::remove(w.begin(), w.end(), ' '), w.end());
  if (w.size() > 3 && w.compare(0, 3, "bot") == 0 && std::all_of(w.begin() + 3, w.end(), ::isdigit)) {
    int n = atoi(w.c_str() + 3);
    if (n < 1 || n > BotPlayerCount()) {
      t.error = "no BOT " + std::to_string(n) + " (" + std::to_string(BotPlayerCount()) + " bots joined)";
      return t;
    }
    t.bot = n, t.found = true;
    return t;
  }
  // #n: its number in /players
  if (what[0] == '#' && what.size() > 1) {
    int n = atoi(what.c_str() + 1), i = 0;
    for (auto& [k, p] : g_players)
      if (++i == n) t.key = k, t.found = true;
    if (!t.found) t.error = "no player " + what;
    return t;
  }
  std::vector<std::pair<uint32_t, uint16_t>> exact, prefix;
  for (auto& [k, p] : g_players) {
    std::string n = Lower(p.name);
    if (n == what || IpText(p.ip) == what || p.id == what) exact.push_back(k);
    else if (n.compare(0, what.size(), what) == 0) prefix.push_back(k);
  }
  auto& list = exact.empty() ? prefix : exact;
  if (list.size() == 1) {
    t.key = list[0], t.found = true;
  } else if (list.empty()) {
    t.error = "no player called " + what_raw + " (/players lists them)";
  } else {
    t.error = std::to_string(list.size()) + " players match " + what_raw + ": use #n from /players";
  }
  return t;
}

// The maps (any case; short names too).
std::string MapName(const std::string& raw) {
  std::string m = Lower(Trim(raw));
  for (const char* const* a = MapAliases(); a[0] && a[1]; a += 2)
    if (m == a[0]) return a[1];
  return "";
}

std::string PlayersText() {
  std::string out;
  int i = 0;
  const auto now = Clock::now();
  for (auto& [k, p] : g_players) {
    char line[200];
    long ago = long(std::chrono::duration_cast<std::chrono::seconds>(now - p.seen).count());
    snprintf(line, sizeof(line), "#%d %s  %s:%u  id %s%s\n", ++i, p.name.c_str(), IpText(p.ip).c_str(), p.game_port,
             p.id.c_str(), ago > 15 ? "  (silent)" : "");
    out += line;
  }
  const int bots = BotPlayerCount();
  int active = 0;
  for (int b = 0; b < bots; ++b) active += !BotBenched(b);
  char line[160];
  snprintf(line, sizeof(line), "%s%d bots (%d playing); next map: %d", out.empty() ? "no players have said hello; " : "",
           bots, active, g_bot_count);
  return out + line;
}
}  // namespace

const char* ServerHelp() {
  static const std::string help = std::string("/players  /kick <name>  /ban <name>  /unban <name|id|address>  /map <") +
                                  MapChoices() + ">  /bots <n>";
  return help.c_str();
}

bool ServerCommand(const std::string& cmd, const std::string& arg, bool console, std::string& reply) {
  std::lock_guard lock(g_mutex);
  LoadBansLocked();
  if (cmd == "players" || cmd == "who") {
    reply = PlayersText();
    return true;
  }
  if (cmd == "kick" || cmd == "ban") {
    const bool ban = cmd == "ban";
    Target t = FindLocked(arg);
    if (!t.found) {
      reply = cmd + ": " + t.error;
      return true;
    }
    if (t.bot) {
      SetBotBenched(t.bot - 1, true);
      reply = "BOT " + std::to_string(t.bot) + " kicked (bots are not banned; /bots brings them back)";
      RT_INFO("admin: kicked BOT %d", t.bot);
      return true;
    }
    Player p = g_players[t.key];
    Tell(p, std::string("BF43K!") + (ban ? "You were banned from this server." : "You were kicked from this server."));
    BlockLocked(p, ban ? Clock::time_point::max() : Clock::now() + std::chrono::seconds(90));
    if (ban) {
      g_bans.push_back({p.id, Local(p.ip) ? "-" : IpText(p.ip), p.name});
      SaveBansLocked();
    }
    g_players.erase(t.key);
    reply = p.name + (ban ? " banned" : " kicked");
    RT_INFO("admin: %s %s (%s:%u, id %s)%s", ban ? "banned" : "kicked", p.name.c_str(), IpText(p.ip).c_str(),
            p.game_port, p.id.c_str(), console ? " from the console" : "");
    return true;
  }
  if (cmd == "unban") {
    std::string what = Lower(Trim(arg));
    size_t before = g_bans.size();
    std::vector<std::string> freed;
    g_bans.erase(std::remove_if(g_bans.begin(), g_bans.end(),
                                [&](const Ban& b) {
                                  bool m = !what.empty() && (Lower(b.name) == what || b.id == what || b.address == what);
                                  if (m) freed.push_back(b.address);
                                  return m;
                                }),
                 g_bans.end());
    if (g_bans.size() == before) {
      reply = "unban: nobody banned matches " + arg;
      return true;
    }
    for (const std::string& a : freed) {
      in_addr ia{};
      if (inet_pton(AF_INET, a.c_str(), &ia) == 1)
        g_blocks.erase(std::remove_if(g_blocks.begin(), g_blocks.end(), [&](const Block& b) { return b.ip == ia.s_addr; }),
                       g_blocks.end());
    }
    // (a ban on this PC's address blocked a port: those go too)
    g_blocks.erase(std::remove_if(g_blocks.begin(), g_blocks.end(),
                                  [](const Block& b) { return b.until == Clock::time_point::max() && Local(b.ip); }),
                   g_blocks.end());
    SaveBansLocked();
    reply = std::to_string(before - g_bans.size()) + " ban(s) lifted";
    RT_INFO("admin: unbanned %s", arg.c_str());
    return true;
  }
  if (cmd == "map") {
    std::string map = MapName(arg);
    if (map.empty()) {
      reply = std::string("map: ") + MapChoices();
      return true;
    }
    // The joiners leave too, and join again once it is up.
    for (auto& [k, p] : g_players) Tell(p, "BF43N!" + map);
    RequestMapChange(map);
    reply = "changing the map to " + map;
    RT_INFO("admin: map %s", map.c_str());
    return true;
  }
  if (cmd == "bots") {
    std::string a = Trim(arg);
    if (a.empty() || !std::all_of(a.begin(), a.end(), ::isdigit)) {
      reply = "bots: a number, 0 to 16 (" + std::to_string(BotPlayerCount()) + " joined this level)";
      return true;
    }
    int n = std::clamp(atoi(a.c_str()), 0, 16);
    g_bot_count = n;  // (the next level's)
    int joined = BotPlayerCount(), live = SetActiveBots(n);
    char buf[200];
    if (n <= joined)
      snprintf(buf, sizeof(buf), "%d bots playing", live);
    else
      snprintf(buf, sizeof(buf), "%d bots playing; %d more join with the next level (/map to start one now)", live,
               n - joined);
    reply = buf;
    RT_INFO("admin: bots %d (%d joined this level, %d playing)", n, joined, live);
    return true;
  }
  return false;
}

bool Blocked(uint32_t ip, uint16_t port) {
  if (!g_any_block.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  bool blocked = false;
  g_blocks.erase(std::remove_if(g_blocks.begin(), g_blocks.end(), [&](const Block& b) { return now >= b.until; }),
                 g_blocks.end());
  for (const Block& b : g_blocks) blocked |= b.ip == ip && (b.port == 0 || b.port == port);
  if (g_blocks.empty()) g_any_block = false;
  return blocked;
}

int HumanPlayerCount() {
  std::lock_guard lock(g_mutex);
  const auto fresh = Clock::now() - std::chrono::seconds(20);  // (a hello every 5 s)
  int n = 0;
  for (const auto& [k, p] : g_players) n += p.seen >= fresh;
  return n;
}

bool ChatBlocked(uint32_t ip, uint16_t chat_port) {
  if (!g_any_block.load(std::memory_order_relaxed)) return false;
  if (Blocked(ip, 0)) return true;  // (blocks of the whole address)
  uint16_t game_port = 0;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_players.find(std::make_pair(ip, chat_port));
    if (it != g_players.end()) game_port = it->second.game_port;
  }
  return game_port && Blocked(ip, htons(game_port));  // (a local player's block: by its game port)
}

std::string PlayerName(uint32_t ip, uint16_t chat_port) {
  std::lock_guard lock(g_mutex);
  auto it = g_players.find(std::make_pair(ip, chat_port));
  return it == g_players.end() || it->second.name.empty() ? "a player" : it->second.name;
}

void OnHello(uint32_t ip, uint16_t chat_port, const std::string& body) {
  // "<game port>|<id>|<name>"
  size_t a = body.find('|'), b = a == std::string::npos ? a : body.find('|', a + 1);
  if (b == std::string::npos) return;
  std::lock_guard lock(g_mutex);
  LoadBansLocked();
  auto key = std::make_pair(ip, chat_port);
  const auto stale = Clock::now() - std::chrono::minutes(5);  // (the hellos come every 5 s)
  for (auto it = g_players.begin(); it != g_players.end();)
    if (it->second.seen < stale) it = g_players.erase(it);
    else ++it;
  const bool fresh = !g_players.count(key);
  if (fresh && g_players.size() >= 64) return;  // (a flood of made-up senders)
  Player& p = g_players[key];
  p.ip = ip, p.chat_port = chat_port;
  p.game_port = uint16_t(atoi(body.c_str()));
  p.id = Clean(body.substr(a + 1, b - a - 1), 32);
  p.name = Clean(body.substr(b + 1), 32);
  p.seen = Clock::now();
  const bool banned = IsBanned(p);
  if (fresh && !banned)
    RT_INFO("admin: player %s at %s:%u (id %s)", p.name.c_str(), IpText(ip).c_str(), p.game_port, p.id.c_str());
  if (banned) {
    static std::map<std::pair<uint32_t, uint16_t>, Clock::time_point> told;  // (said once a minute)
    if (Clock::now() - told[key] > std::chrono::seconds(60)) {
      told[key] = Clock::now();
      RT_INFO("admin: %s (id %s, %s) is banned: sent away", p.name.c_str(), p.id.c_str(), IpText(ip).c_str());
    }
    Tell(p, "BF43K!You are banned from this server.");
    BlockLocked(p, Clock::time_point::max());
    g_players.erase(key);
  }
}

std::string HelloBody() {
  static const std::string id = [] {
    std::filesystem::path path = g_user_root / "player_id.txt";
    std::string s;
    if (std::ifstream f(path); f) std::getline(f, s);
    s = Trim(s);
    if (s.size() < 8) {
      std::random_device rd;
      char buf[24];
      snprintf(buf, sizeof(buf), "%08x%08x", unsigned(rd()), unsigned(rd()));
      s = buf;
      std::ofstream(path) << s << '\n';
    }
    return s.substr(0, 32);
  }();
  return std::to_string(g_client_game_port.load()) + "|" + id + "|" + Clean(g_gamertag, 32);
}

// A /map where leaving the level can't be done in this run: a dedicated
// server (no pause menu; the game's doExitLevel does nothing in a match) or a
// host with bot players (their made-up connections crash the server's
// teardown: "bad indirect call" right after the exit). The game starts again
// on the map, as the same command with --host-map <map> and --bots <the
// count now>, on the same console and pipes (a launcher keeps reading it),
// and this run ends. Joiners told of the change join it once it is up.
bool RestartOnMap(const std::string& map) {
#ifdef _WIN32
  // One argument as the C runtime reads it back (CommandLineToArgvW's rules):
  // backslashes doubled before a quote and before the closing quote.
  auto quote = [](const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t slashes = 0;
    for (char c : a) {
      if (c == '\\') {
        ++slashes;
        continue;
      }
      q.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
      q += c;
      slashes = 0;
    }
    q.append(slashes * 2, '\\');
    return q + "\"";
  };
  // The arguments as given (the C runtime's), without --host-map / --bots and
  // the server settings an admin can change while it runs (theirs now below).
  std::string line = quote(__argv[0]);
  for (int i = 1; i < __argc; ++i) {
    std::string a = __argv[i];
    if ((a == "--host-map" || a == "--bots" || a == "--password" || a == "--servername" || a == "--server-name" ||
         a == "--weather" || a == "--time" || a == "--hardcore-damage") &&
        i + 1 < __argc) {
      ++i;
      continue;
    }
    if (a == "--hardcore") continue;
    line += " " + quote(a);
  }
  line += " --host-map " + map + " --bots " + std::to_string(g_bot_count);
  auto given = [&](std::initializer_list<const char*> names) {
    for (int i = 1; i < __argc; ++i)
      for (const char* n : names)
        if (__argv[i] == std::string(n)) return true;
    return false;
  };
  // What a host chose in its menu or an admin changed goes on to the new run:
  // its server settings as they are now, so a /map keeps them.
  if (g_lan_host && !g_join_address && !g_dedicated && !given({"--host"})) line += " --host";
  if (g_hardcore && std::isfinite(g_hardcore_damage))
    line += " --hardcore-damage " + std::to_string(g_hardcore_damage);
  else if (g_hardcore)
    line += " --hardcore";
  std::string name, password;
  {
    std::lock_guard lock(g_host_map_mutex);  // (lan.cpp)
    name = g_server_name, password = g_server_password;
  }
  if (!name.empty()) line += " --servername " + quote(name);
  if (!password.empty()) line += " --password " + quote(password);
  if (g_weather) line += " --weather rain";
  if (std::isfinite(g_time_of_day)) line += " --time " + std::to_string(g_time_of_day);
  if (lan::Publish() && !given({"--publish"})) line += " --publish";
  // Only the console / pipe handles are passed on: the sockets (inheritable
  // by default) would keep this run's ports taken in the new one.
  STARTUPINFOEXA si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  HANDLE std_handles[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                           GetStdHandle(STD_ERROR_HANDLE)};
  si.StartupInfo.hStdInput = std_handles[0];
  si.StartupInfo.hStdOutput = std_handles[1];
  si.StartupInfo.hStdError = std_handles[2];
  std::vector<HANDLE> inherit;
  for (HANDLE h : std_handles) {
    if (!h || h == INVALID_HANDLE_VALUE || std::find(inherit.begin(), inherit.end(), h) != inherit.end()) continue;
    if (SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) inherit.push_back(h);
  }
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<uint8_t> attrs(attr_size);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
  bool listed = !inherit.empty() && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) &&
                UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                          inherit.size() * sizeof(HANDLE), nullptr, nullptr);
  if (!listed) si.lpAttributeList = nullptr;
  PROCESS_INFORMATION pi{};
  SetEnvironmentVariableA("BF_ADMIN_RESTARTED", map.c_str());
  std::vector<char> buf(line.begin(), line.end());
  buf.push_back(0);
  const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, listed ? TRUE : FALSE,
                                 listed ? EXTENDED_STARTUPINFO_PRESENT : 0, nullptr, nullptr, &si.StartupInfo, &pi);
  if (listed) DeleteProcThreadAttributeList(si.lpAttributeList);
  if (!ok) {
    RT_WARN("admin: could not start the game again on %s (%lu)", map.c_str(), GetLastError());
    return false;
  }
  RT_INFO("admin: the game starts again on %s (process %lu); this run ends", map.c_str(), pi.dwProcessId);
  ::CloseHandle(pi.hThread), ::CloseHandle(pi.hProcess);
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), 0);  // (its ports free for the new run)
  return true;
#else
  (void)map;
  return false;  // (the game leaves the level itself)
#endif
}

void OnKicked(const std::string& why) {
  RT_INFO("admin: %s", why.c_str());
  RequestMapChange("");  // (leave the level for the menus)
}
void OnMapChanging(const std::string& map) {
  RT_INFO("admin: the host changes the map to %s", map.c_str());
  RequestRejoin(map);
}
void OnMapReady(const std::string& map) {
  RT_INFO("admin: the host's %s is up", map.c_str());
  RejoinReady();
}
void MapReady(const std::string& map) {
  std::vector<Player> list;
  {
    std::lock_guard lock(g_mutex);
    for (auto& [k, p] : g_players) list.push_back(p);
  }
  RT_INFO("admin: %s is up; telling %zu players", map.c_str(), list.size());
  for (const Player& p : list) Tell(p, "BF43R!" + map);
}
}  // namespace rt::admin
