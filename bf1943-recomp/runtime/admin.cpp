#include "admin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel.h"
#include "xenia_bridge.h"
#include "admin_server.h"
#include "bot_llm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <map>

namespace rt {
std::atomic<bool> g_admin_host{false};  // lan.cpp: this game hosts (its commands run here)
}

namespace rt::admin {
namespace {
std::mutex g_lock;
std::deque<std::pair<std::chrono::steady_clock::time_point, std::wstring>> g_lines;
std::string g_input;  // the line being typed
std::atomic<bool> g_typing{false};
constexpr size_t kMaxLines = 8;

std::wstring Wide(const std::string& s) {
  std::wstring w;
  size_t i = 0;
  while (i < s.size()) {
    unsigned c = uint8_t(s[i]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (i + n > s.size()) break;
    // (the lead byte's bits: 7 of a single byte, 5 / 4 / 3 of a sequence's)
    w.push_back(wchar_t((c & (n == 1 ? 0x7F : 0xFF >> (n + 1))) << (6 * (n - 1)) |
                        (n > 1 ? (uint8_t(s[i + 1]) & 0x3F) << (6 * (n - 2)) : 0) |
                        (n > 2 ? (uint8_t(s[i + 2]) & 0x3F) << 6 : 0) | (n > 3 ? uint8_t(s[i + 3]) & 0x3F : 0)));
    i += n;
  }
  return w;
}
std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
// A remote admin's command (run here for it): what it says goes back to it too.
thread_local std::string* t_reply = nullptr;
std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c < 0x80 ? char(c) : '?');
  return s;
}
void AddLine(std::wstring line) {
  if (t_reply) *t_reply += (t_reply->empty() ? "" : "\n") + Narrow(line);
  if (g_dedicated) RT_INFO("admin> %s", Narrow(line).c_str());  // (no screen: the console's answer)
  std::lock_guard lock(g_lock);
  g_lines.push_back({std::chrono::steady_clock::now(), std::move(line)});
  while (g_lines.size() > kMaxLines) g_lines.pop_front();
}
// (--host hosts without lan::Host: a game that joined nobody is its own host)
bool Host() { return g_dedicated || g_admin_host || !g_lan_host || !g_join_address; }
void Command(const std::string& line);
thread_local bool g_console_command = false;  // (the command being run came from the console)

// All-talk chat over our own UDP port (19431 + this game's offset): a client's
// line goes to the host with its listen port, the host shows it and relays it
// to every other player; the host's own lines go straight to them all.
constexpr int kChatPortBase = 19431;
#ifdef _WIN32
SOCKET g_chat_socket = INVALID_SOCKET;
std::mutex g_peers_lock;
std::map<uint32_t, std::pair<uint16_t, std::chrono::steady_clock::time_point>> g_peers;  // ip -> port, last seen
std::atomic<int> g_chat_port{0};  // ours, as told to the host

void SendRaw(uint32_t ip, uint16_t port, const std::string& packet) {
  if (g_chat_socket == INVALID_SOCKET || !ip) return;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip;
  sendto(g_chat_socket, packet.data(), int(packet.size()), 0, (sockaddr*)&to, sizeof(to));
}
void SendTo(uint32_t ip, uint16_t port, const std::string& text) { SendRaw(ip, port, "BF43C!" + text); }
// The host's chat port (a joiner's packets for it).
// (its port offset as its game port's: 1003 + offset)
uint16_t HostChatPort() {
  const int game = ntohs(g_join_port);
  return uint16_t(kChatPortBase + (game > 1003 ? game - 1003 : g_base_port_offset));
}
std::string g_login;  // a joiner's /login: the host's admin password, sent with its commands
std::atomic<bool> g_sent_away{false};  // kicked or banned: no hellos until in a level again
void BroadcastChat(const std::string& text) {
  if (!Host()) {
    if (g_join_address) {
      char head[16];
      snprintf(head, sizeof(head), "%u|", unsigned(g_chat_port.load()));
      SendTo(g_join_address, HostChatPort(), std::string(head) + text);
    } else {
      AddLine(L"> " + Wide(text));  // (offline: to this screen only)
    }
    return;
  }
  AddLine(L"> " + Wide(text));
  std::lock_guard lock(g_peers_lock);
  for (auto& [ip, peer] : g_peers) SendTo(ip, peer.first, text);
}
void ChatNetThread() {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  g_chat_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_chat_socket == INVALID_SOCKET) return;
  uint16_t port = uint16_t(kChatPortBase + g_port_offset);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  bool bound = bind(g_chat_socket, (sockaddr*)&a, sizeof(a)) == 0;
  // Taken (a joiner on the host's PC, before its port offset moved): one of
  // the next ports; a joiner tells the host which (a host keeps its own).
  for (int i = 1; !bound && i <= 20 && !g_dedicated; ++i) {
    a.sin_port = htons(uint16_t(port + 20 + i));
    if (bind(g_chat_socket, (sockaddr*)&a, sizeof(a)) == 0) bound = true, port = uint16_t(port + 20 + i);
  }
  if (!bound) {
    RT_WARN("chat: port %u busy (chat is local only)", port);
    return;
  }
  g_chat_port = port;
  DWORD timeout = 200;
  setsockopt(g_chat_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
  RT_INFO("chat: all-talk on UDP %u", port);
  char buf[512];
  auto next_hello = std::chrono::steady_clock::now();
  for (;;) {
    // A joiner in a level: its hello to the host every 5 s (admin_server.h).
    // (in the menus too: waiting to join again after a /map, the host's new
    // run learns of it)
    if (!g_in_menus) g_sent_away = false;  // (in a level again: joined anew)
    if (!Host() && g_join_address && !g_sent_away && std::chrono::steady_clock::now() >= next_hello) {
      next_hello = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      SendRaw(g_join_address, HostChatPort(), "BF43H!" + std::to_string(g_chat_port.load()) + "|" + HelloBody());
    }
    sockaddr_in from{};
    int len = sizeof(from);
    int n = recvfrom(g_chat_socket, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
    if (n < 6 || memcmp(buf, "BF43", 4) || buf[5] != '!') continue;
    const char kind = buf[4];
    std::string body(buf + 6, size_t(n) - 6);
    if (kind != 'C') {
      if (Host() && (kind == 'H' || kind == 'A')) {
        // "<chat port>|...": a joiner's hello, or a remote admin's command
        // ("<chat port>|<password>|<line>").
        size_t bar = body.find('|');
        const uint16_t peer_port = bar == std::string::npos ? 0 : uint16_t(atoi(body.c_str()));
        if (!peer_port) continue;
        {
          std::lock_guard lock(g_peers_lock);  // (it hears the chat from now on)
          g_peers[from.sin_addr.s_addr] = {peer_port, std::chrono::steady_clock::now()};
        }
        body = body.substr(bar + 1);
        if (kind == 'H') {
          OnHello(from.sin_addr.s_addr, peer_port, body);
          continue;
        }
        size_t bar2 = body.find('|');
        if (bar2 == std::string::npos) continue;
        const std::string password = body.substr(0, bar2), line = Trim(body.substr(bar2 + 1));
        std::string reply;
        if (g_admin_password.empty() || password != g_admin_password) {
          reply = g_admin_password.empty() ? "This server takes no remote admins." : "Wrong admin password (/login).";
          RT_INFO("admin: a command from %u.%u.%u.%u refused", from.sin_addr.S_un.S_un_b.s_b1,
                  from.sin_addr.S_un.S_un_b.s_b2, from.sin_addr.S_un.S_un_b.s_b3, from.sin_addr.S_un.S_un_b.s_b4);
        } else if (!line.empty() && line[0] == '/') {
          RT_INFO("admin: remote command %s", line.c_str());
          t_reply = &reply;
          Command(line);
          t_reply = nullptr;
        }
        for (size_t at = 0; at < reply.size();) {  // (a line a packet)
          size_t e = reply.find('\n', at);
          if (e == std::string::npos) e = reply.size();
          SendTo(from.sin_addr.s_addr, peer_port, "[admin] " + reply.substr(at, e - at));
          at = e + 1;
        }
      } else if (!Host() && from.sin_addr.s_addr == g_join_address) {
        // The host's notices: kicked, the map changes, the new map is up.
        if (kind == 'K') AddLine(Wide(body)), OnKicked(body), g_sent_away = true;
        else if (kind == 'N') AddLine(L"The host changes the map to " + Wide(body) + L"."), OnMapChanging(body);
        else if (kind == 'R') OnMapReady(body);
      }
      continue;
    }
    if (body.empty()) continue;
    std::string text = body;
    if (Host()) {
      // From a player: "port|text"; remember where to answer, relay to the rest.
      size_t bar = text.find('|');
      if (bar != std::string::npos && bar > 0 && bar < 7) {
        const uint16_t peer_port = uint16_t(atoi(text.substr(0, bar).c_str()));
        text = text.substr(bar + 1);
        if (peer_port) {
          std::lock_guard lock(g_peers_lock);
          g_peers[from.sin_addr.s_addr] = {peer_port, std::chrono::steady_clock::now()};
          for (auto& [ip, peer] : g_peers)
            if (ip != from.sin_addr.s_addr) SendTo(ip, peer.first, text);
        }
      } else {
        // A dedicated server's or host's own line relayed by a player? show it.
      }
      AddLine(L"> " + Wide(text));
      RT_INFO("chat: %s", text.c_str());
      if (g_dedicated) botllm::ChatReplyTo(text);  // (the bots' advisor may answer)
    } else {
      AddLine(L"> " + Wide(text));
    }
  }
}
#endif

void Command(const std::string& line) {
  std::string cmd = line.substr(1), arg;
  size_t sp = cmd.find(' ');
  if (sp != std::string::npos) arg = cmd.substr(sp + 1), cmd = cmd.substr(0, sp);
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return char(tolower(c)); });
  arg = Trim(arg);
  if (cmd == "help") {
    AddLine(L"/weather clear|rain  /time dawn|day|sunset|night  /name <text>  /password <text>  /bottalk on|off  /say <text>");
    AddLine(Wide(ServerHelp()) + (Host() ? L"" : L"  /login <admin password>"));
    return;
  }
  if (cmd == "login") {
    // A joiner: the host's admin password, sent along with its commands.
    g_login = arg;
    AddLine(arg.empty() ? L"logged out" : L"admin password kept: the host's commands go to it");
    return;
  }
  if (cmd == "say") {
#ifdef _WIN32
    if (!arg.empty()) BroadcastChat(arg);
#else
    if (!arg.empty()) AddLine(L"> " + Wide(arg));
#endif
    return;
  }
  if (!Host()) {
#ifdef _WIN32
    if (!g_login.empty() && g_join_address) {  // a remote admin: the host runs it (and answers)
      SendRaw(g_join_address, HostChatPort(),
              "BF43A!" + std::to_string(g_chat_port.load()) + "|" + g_login + "|" + line);
      return;
    }
#endif
    AddLine(L"Only the host can change the game (an admin: /login <password>).");
    return;
  }
  if (std::string reply; ServerCommand(cmd, arg, g_console_command, reply)) {
    for (size_t at = 0; at <= reply.size();) {
      size_t e = reply.find('\n', at);
      if (e == std::string::npos) e = reply.size();
      AddLine(Wide(reply.substr(at, e - at)));
      at = e + 1;
    }
    return;
  }
  if (cmd == "quit" && g_console_command) {
    RT_INFO("admin: the server stops (console)");
    std::fflush(nullptr);
    std::_Exit(0);
  }
  if (cmd == "bottalk") {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(tolower(c)); });
    if (a == "on") botllm::SetChatEnabled(true), AddLine(L"bots answer chat");
    else if (a == "off") botllm::SetChatEnabled(false), AddLine(L"bots no longer answer chat");
    else AddLine(L"BOTTALK: on or off");
    return;
  }
  if (cmd == "weather") {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(tolower(c)); });
    if (a != "clear" && a != "rain") {
      AddLine(L"WEATHER: clear or rain");
      return;
    }
    g_weather = a == "rain" ? 1 : 0;
    xgpu::SetWeather(float(g_weather), g_time_of_day);
    AddLine(L"weather " + Wide(a));
    RT_INFO("admin: weather %s", a.c_str());
  } else if (cmd == "time") {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(tolower(c)); });
    float h = -1;
    if (a == "dawn" || a == "sunrise") h = 6.5f;
    else if (a == "day" || a == "noon") h = 12.0f;
    else if (a == "dusk" || a == "sunset") h = 18.0f;
    else if (a == "night") h = 23.0f;
    else if (!a.empty()) h = float(atof(a.c_str()));
    if (h < 0 || h > 24) {
      AddLine(L"TIME: dawn, day, sunset, night or hours (12 noon)");
      return;
    }
    g_time_of_day = h;
    xgpu::SetWeather(float(g_weather), h);
    wchar_t buf[48];
    swprintf(buf, 48, L"time %.1f h", h);
    AddLine(buf);
    RT_INFO("admin: time %.1f h", h);
  } else if (cmd == "name") {
    g_server_name = arg.substr(0, 48);
    AddLine(L"server name: " + Wide(g_server_name));
    RT_INFO("admin: server name '%s'", g_server_name.c_str());
  } else if (cmd == "password") {
    g_server_password = arg.substr(0, 32);
    AddLine(arg.empty() ? L"password cleared" : L"password set");
    RT_INFO("admin: password %s", arg.empty() ? "cleared" : "set");
  } else if (cmd == "clear") {
    std::lock_guard lock(g_lock);
    g_lines.clear();
  } else {
    AddLine(L"Unknown command; /help for the list.");
  }
}

#ifdef _WIN32
struct Key { int vk; char c, shift; };
const Key kKeys[] = {
    {'A', 'a', 'A'}, {'B', 'b', 'B'}, {'C', 'c', 'C'}, {'D', 'd', 'D'}, {'E', 'e', 'E'}, {'F', 'f', 'F'},
    {'G', 'g', 'G'}, {'H', 'h', 'H'}, {'I', 'i', 'I'}, {'J', 'j', 'J'}, {'K', 'k', 'K'}, {'L', 'l', 'L'},
    {'M', 'm', 'M'}, {'N', 'n', 'N'}, {'O', 'o', 'O'}, {'P', 'p', 'P'}, {'Q', 'q', 'Q'}, {'R', 'r', 'R'},
    {'S', 's', 'S'}, {'T', 't', 'T'}, {'U', 'u', 'U'}, {'V', 'v', 'V'}, {'W', 'w', 'W'}, {'X', 'x', 'X'},
    {'Y', 'y', 'y'}, {'Z', 'z', 'Z'}, {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'},
    {'4', '4', '$'}, {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'}, {'8', '8', '*'}, {'9', '9', '('},
    {VK_SPACE, ' ', ' '}, {VK_OEM_PERIOD, '.', '>'}, {VK_OEM_COMMA, ',', '<'}, {VK_OEM_MINUS, '-', '_'},
    {VK_OEM_PLUS, '=', '+'}, {VK_OEM_1, ';', ':'}, {VK_OEM_2, '/', '?'}, {VK_OEM_3, '`', '~'},
    {VK_OEM_4, '[', '{'}, {VK_OEM_5, '\\', '|'}, {VK_OEM_6, ']', '}'}, {VK_OEM_7, '\'', '"'},
};
// Typing for the game's keys: while a line is open, the game gets none of them.
void ChatThread() {
  bool was[256] = {};
  bool open = false;
  std::string line;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    auto pressed = [&](int vk) {
      const bool d = vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
      const bool e = d && !was[vk];
      if (vk > 0 && vk < 256) was[vk] = d;
      return e;
    };
    if (!open) {
      if (pressed('T') && !g_in_menus) open = true, line.clear(), g_typing = true;
      continue;
    }
    g_typing = true;
    if (pressed(VK_ESCAPE)) {
      open = false, line.clear(), g_typing = false;
    } else if (pressed(VK_RETURN)) {
      open = false, g_typing = false;
      Execute(line, false);
      line.clear();
    } else if (pressed(VK_BACK)) {
      if (line.size()) line.pop_back();
    } else {
      for (const Key& k : kKeys) {
        if (!pressed(k.vk)) continue;
        if (line.size() < 120) line.push_back(shift ? k.shift : k.c);
        break;
      }
    }
    {
      std::lock_guard lock(g_lock);
      g_input = line;
    }
  }
}
#endif

void ConsoleThread() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // (a UTF-8 mark some pipes start with)
    line = Trim(line);
    if (!line.empty()) Execute(line, true);
  }
}
}  // namespace

void Start() {
  if (const char* m = getenv("BF_ADMIN_RESTARTED")) RT_INFO("admin: started again for /map %s", m);
#ifdef _WIN32
  std::thread(ChatThread).detach();
  std::thread(ChatNetThread).detach();
#endif
  if (g_dedicated || g_admin_console) std::thread(ConsoleThread).detach();
}

bool Typing() { return g_typing; }

bool Lines(std::vector<std::wstring>& out) {
  std::lock_guard lock(g_lock);
  const auto now = std::chrono::steady_clock::now();
  while (!g_lines.empty() && now - g_lines.front().first > std::chrono::seconds(10)) g_lines.pop_front();
  for (const auto& l : g_lines) out.push_back(l.second);
  if (g_typing) out.push_back(L"say: " + Wide(g_input) + L"_");
  return !out.empty();
}

void Say(const std::string& name, const std::string& text) {
#ifdef _WIN32
  BroadcastChat(name + ": " + text);
#else
  AddLine(Wide(name + ": " + text));
#endif
}

void Execute(const std::string& raw, bool console) {
  std::string line = Trim(raw);
  if (line.empty()) return;
  if (console) RT_INFO("admin: %s", line.c_str());
  // The console takes commands without their '/' too ("kick Bob").
  if (console && line[0] != '/') {
    static const char* const kWords[] = {"help", "players", "who", "kick", "ban", "unban", "map", "bots", "weather",
                                         "time", "name", "password", "bottalk", "say", "quit", "clear"};
    std::string first = line.substr(0, line.find(' '));
    std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return char(tolower(c)); });
    for (const char* w : kWords)
      if (first == w) line = "/" + line;
  }
  g_console_command = console;
  if (line[0] == '/') Command(line);
  g_console_command = false;
  if (line[0] == '/') return;
#ifdef _WIN32
  BroadcastChat(line);
#else
  AddLine(L"> " + Wide(line));
#endif
}

#ifdef _WIN32
void SendPacket(uint32_t ip, uint16_t port, const std::string& packet) { SendRaw(ip, port, packet); }
#else
void SendPacket(uint32_t, uint16_t, const std::string&) {}  // (no chat network here yet)
#endif
}  // namespace rt::admin
