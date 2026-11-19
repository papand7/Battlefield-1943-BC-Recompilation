// COMMANDER SEATS, ORDERS, DROPS and the LIVE MAP (commander.cpp, commander_world.cpp;
// design in docs/COMMANDER.md).
//
// Each team (1 US, 2 Japan) may have one commander. Any player can take the
// seat with /commander us|japan (a joiner's request goes to the host, as its
// other packets do) and give it up with /commander leave. The host decides
// everything: the seats, the players' teams, the cooldowns, the orders and the
// drops.
//
// Teams: a joiner's game tells the host where its camera is ("BF43E!<chat
// port>|x,y,z" every 5 s, in a level); the host's live sample has every
// soldier, and the one under that eye (on foot: within 2.5 m) is the player's.
// Its team is kept for the level. The host's own player is found the same way
// with its own camera. A seat is only for a player of that team (the dedicated
// server's console may take either).
//
// The seat comes with a secret token: the holder is told a link to the live
// map with it (http://<host>:19440+offset/?k=<token>; a joiner's game builds it
// from the host's address and copies it to the clipboard). The page sends the
// orders and drops with it, and its polling keeps the seat: once the page has
// been opened, the seat lasts while the page polls (it frees 20 s after the page
// closes). Before that the game's own keep-alives keep it (input in the last 20 s).
//
// Packets: "BF43M!<chat port>|<body>" player to host ("s", "l", "k", "t <team>",
// and the host's own "c <seconds>"); "BF43M!h<team>:<token>" / "BF43M!n" host
// to player (holds the seat, its token / does not).
//
// The live map: while a match is recorded (replay.cpp takes the soldiers,
// vehicles and capture points ten times a second) a small HTTP server on the
// host's own address serves it: "/" the page (the commander's controls with a
// token), "/livemap.json" the data, "/minimap.png" the level's picture,
// "/api/state" and "/api/act" the commander's. Port: TCP 19440 + the port
// offset (BF_NO_LIVEMAP turns it off).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rt::commander {
enum Team { kNoTeam = 0, kUS = 1, kJapan = 2 };
// Battlefield 1943 only (the runtime is shared with Bad Company, which has none).
bool Available();
int ParseTeam(const std::string& word);  // "us", "1", "japan", "jp", "2": a team, else 0
const char* TeamName(int team);

struct Reply {
  std::string text;   // what the requester is told ("" none)
  int holds = 0;      // the team the requester holds now (0 none)
  std::string token;  // the seat's token when it holds one
  bool changed = false;  // a seat changed hands: everyone hears it
};
// The host's handling of one request (ip, port: the player's chat address; 0, 0
// the host itself). body: "s" status, "l" leave, "k" keep-alive, "t <team>"
// take, "c <seconds>" cooldown (the host only: local).
Reply Request(uint32_t ip, uint16_t port, const std::string& name, const std::string& body, bool local_host);
// Whether a team has a commander seated right now.
bool Seated(int team);

// Joiner: its game should send a keep-alive now (admin.cpp's chat thread).
bool KeepAliveDue();
// Joiner: the host's answer about this game's seat ("h<team>:<token>" or "n").
// A new token: the line to show (the page's link), else "".
std::string OnHostReply(const std::string& body);
// Joiner: its eye packet's body now ("x,y,z"), every 5 s in a level; "" none.
std::string EyeDue();
// Host: a joiner's eye ("x,y,z"): its soldier, so its team.
void OnPlayerEye(uint32_t ip, uint16_t port, const std::string& body);

// The link to the page for a token on this host ("http://<LAN address>:<port>/?k=...").
std::string PageLink(const std::string& token);
// Puts text on the clipboard (Windows; the link is long to type).
void CopyToClipboard(const std::string& text);
// This game's own seat's link ("" when it holds none): the host's (or the
// console's) own, or a joiner's as the host last told it.
std::string MyLink();

// Starts the seat expiry and the live map server (admin::Start). announce tells
// everyone; tell tells one player (ip 0, port 0: this game's own screen).
void Start(std::function<void(const std::string&)> announce,
           std::function<void(uint32_t ip, uint16_t port, const std::string&)> tell);

// Cooldowns: an action by a team's commander is allowed once its cooldown is
// over, and only from the seat's holder (its chat address, or the page's
// token). False (with the seconds to wait) otherwise; a true answer starts the
// cooldown. Actions: "order" (attack / defend), "ammo", "vehicle".
bool TryAct(int team, uint32_t ip, uint16_t port, const std::string& action, double* wait_seconds);
int Cooldown();                                // seconds: the drops' base (5 to 600; /commander cooldown)
int CooldownFor(const std::string& action);    // an action's, in seconds

// Orders, per team (expire after kOrderSeconds): attack or defend a flag.
enum OrderKind { kNoOrder = 0, kAttack = 1, kDefend = 2 };
struct Order {
  int kind = kNoOrder;
  float x = 0, z = 0;    // the flag's place
  uint32_t serial = 0;   // changes with each new order (the bots choose again)
};
Order CurrentOrder(int team);  // kind kNoOrder when none

// ---- the host's game world (commander_world.cpp) --------------------------------
// The server's update (bot_players.cpp's hook, the server's own thread): the
// drops waiting are done there, between the server's own steps.
void OnServerUpdate();
// A drop the page asked for, queued for the server's update. kind "ammo" or
// "vehicle"; the answer (what happened) comes back through done.
void QueueDrop(int team, const std::string& kind, float x, float z,
               std::function<void(bool ok, const std::string& text)> done);
// Whether a supply drop is still working, for the page: (team, x, z, seconds left).
// kind 1 heal, 2 ammo; y (has_y: the ground there, when the map's heights are read).
struct SupplyZone {
  int team;
  float x, z, left;
  int kind = 1;
  float y = 0;
  bool has_y = false;
};
std::vector<SupplyZone> SupplyZones();
// An airstrike called and not yet dropped: its point and the seconds to the blast.
struct StrikeWarning {
  int team;
  float x, z, left;
  float y = 0;
  bool has_y = false;
};
std::vector<StrikeWarning> PendingStrikes();

// This game's seat team (the host's own player's team, or a joiner's seat); 0 none.
int MyTeam();
// Sends a whole packet ("BF43Z!..." markers, "BF43X!..." blasts) to a player's
// chat address (the host's answer path; admin.cpp).
void SetRawSender(std::function<void(uint32_t ip, uint16_t port, const std::string& packet)> raw);
// Host (the server's thread): an airstrike's blast at x, y, z: its smoke and
// debris in this game and, by "BF43X!x,y,z", in every player's.
void OnBlast(float x, float y, float z);
// A game: the blast's effect from that packet's body (or the host's own).
void ShowBlast(const std::string& body);
// A chat line for the live map's log: team 0 all talk, else that team's line.
void LogChat(int team, const std::string& text);
// The host, once a second: each team's zones, order and airstrike warning, to
// its players ("BF43Z!"; commander_hud.cpp) and to this game's own view.
void HudSync();
}  // namespace rt::commander
