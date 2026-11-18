// The host's admin commands (admin_server.cpp), typed in chat by the host or
// a remote admin (--admin-password, /login), or on a dedicated server's
// console: /kick, /ban, /unban, /players, /map, /bots.
//
// Players are known by the chat's "hello" (admin.cpp): every 5 s a joiner
// tells the host its name, a stable id (made once, kept in its user folder,
// player_id.txt) and the UDP port its game sends from. A kicked or banned
// player's game packets are dropped (net.cpp), so the server lets it go as one
// who left, and the player's game is told to leave the level itself first.
// Bans are kept in the host's user folder, bans.txt (id, address, name).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace rt {
extern std::string g_admin_password;              // --admin-password: remote admins' (empty: host and console only)
extern std::atomic<uint16_t> g_client_game_port;  // a joiner's game port (net.cpp: from its first packet to the host)
extern bool g_admin_console;                      // --console: a windowed game reads commands on stdin too
// The level is being left (a /map, a kick, the pause menu's Exit Game): the
// bots' connections may go with it (bot_players.cpp keeps them otherwise).
extern std::atomic<bool> g_leaving_level;
}  // namespace rt

namespace rt::admin {
// One of the host's commands: true if it is one (reply: what it did, lines
// separated by '\n'). console: typed on the dedicated server's console.
bool ServerCommand(const std::string& cmd, const std::string& arg, bool console, std::string& reply);
// The help line for them.
const char* ServerHelp();
// net.cpp, hosting: whether a game packet from this address (network order)
// is dropped (a kicked or banned player's).
bool Blocked(uint32_t ip, uint16_t port);
// admin.cpp: whether a chat packet from this address (network order) and chat
// port is a kicked or banned player's (dropped, not registered as a peer).
bool ChatBlocked(uint32_t ip, uint16_t chat_port);
// Players in the match for the beacon: joiners heard from lately (their hellos).
int HumanPlayerCount();
// The chat's packets (admin.cpp). Host: a joiner's hello ("<game port>|<id>|<name>").
void OnHello(uint32_t ip, uint16_t chat_port, const std::string& body);
// Host: a joiner's name from its hellos ("a player" if none yet): the commander seats' (commander.h).
std::string PlayerName(uint32_t ip, uint16_t chat_port);
// Joiner: its hello.
std::string HelloBody();
// Joiner: the host's notices (kicked; the map changes; the new map is up).
void OnKicked(const std::string& why);
void OnMapChanging(const std::string& map);
void OnMapReady(const std::string& map);
// game_patches.cpp (host): the level of a /map is up; tells the joiners.
void MapReady(const std::string& map);
// The game's maps for /map (each game defines them: game_patches.cpp,
// bfbc1_game.cpp): alias, level, alias, level... ending in nullptr (aliases in
// lower case), and the list the help line shows.
const char* const* MapAliases();
const char* MapChoices();
// game_patches.cpp: a /map by starting the game again on the map (Windows;
// false if it couldn't: the level is left in this run then).
bool RestartOnMap(const std::string& map);
}  // namespace rt::admin

namespace rt {
// game_patches.cpp, from the main loop: leave the level (the game's
// doExitLevel), then host the map (empty: stay in the menus).
void RequestMapChange(const std::string& map);
// A joiner: leave the level, and join the host again on the map once it says
// it is up (OnMapReady).
void RequestRejoin(const std::string& map);
void RejoinReady();
// Counts the servers made (a new level's): the bots join each one.
extern std::atomic<int> g_server_generation;
}  // namespace rt
