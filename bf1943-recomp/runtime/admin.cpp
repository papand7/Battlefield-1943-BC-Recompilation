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
    w.push_back(wchar_t((c & (0x7F >> n)) << (6 * (n - 1)) |
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
void AddLine(std::wstring line) {
  std::lock_guard lock(g_lock);
  g_lines.push_back({std::chrono::steady_clock::now(), std::move(line)});
  while (g_lines.size() > kMaxLines) g_lines.pop_front();
}
bool Host() { return g_dedicated || g_admin_host || !g_lan_host; }

// All-talk chat over our own UDP port (19431 + this game's offset): a client's
// line goes to the host with its listen port, the host shows it and relays it
// to every other player; the host's own lines go straight to them all.
constexpr int kChatPortBase = 19431;
#ifdef _WIN32
SOCKET g_chat_socket = INVALID_SOCKET;
std::mutex g_peers_lock;
std::map<uint32_t, std::pair<uint16_t, std::chrono::steady_clock::time_point>> g_peers;  // ip -> port, last seen
std::atomic<int> g_chat_port{0};  // ours, as told to the host

void SendTo(uint32_t ip, uint16_t port, const std::string& text) {
  if (g_chat_socket == INVALID_SOCKET || !ip) return;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip;
  const std::string packet = "BF43C!" + text;
  sendto(g_chat_socket, packet.data(), int(packet.size()), 0, (sockaddr*)&to, sizeof(to));
}
void BroadcastChat(const std::string& text) {
  if (!Host()) {
    if (g_join_address) {
      char head[16];
      snprintf(head, sizeof(head), "%u|", unsigned(g_chat_port.load()));
      SendTo(g_join_address, uint16_t(kChatPortBase + g_base_port_offset), std::string(head) + text);
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
  const uint16_t port = uint16_t(kChatPortBase + g_port_offset);
  g_chat_port = port;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (bind(g_chat_socket, (sockaddr*)&a, sizeof(a))) {
    RT_WARN("chat: port %u busy (chat is local only)", port);
    return;
  }
  DWORD timeout = 200;
  setsockopt(g_chat_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
  RT_INFO("chat: all-talk on UDP %u", port);
  char buf[512];
  for (;;) {
    sockaddr_in from{};
    int len = sizeof(from);
    int n = recvfrom(g_chat_socket, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
    if (n < 7 || memcmp(buf, "BF43C!", 6)) continue;
    std::string text(buf + 6, size_t(n) - 6);
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
    AddLine(L"Only the host can change the game.");
    return;
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
    line = Trim(line);
    if (!line.empty()) Execute(line, true);
  }
}
}  // namespace

void Start() {
#ifdef _WIN32
  std::thread(ChatThread).detach();
  std::thread(ChatNetThread).detach();
#endif
  if (g_dedicated) std::thread(ConsoleThread).detach();
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
  if (line[0] == '/') Command(line);
#ifdef _WIN32
  else BroadcastChat(line);
#else
  else AddLine(L"> " + Wide(line));
#endif
}
}  // namespace rt::admin
