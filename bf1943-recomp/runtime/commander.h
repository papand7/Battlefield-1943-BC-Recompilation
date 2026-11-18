// COMMANDER SEATS and the LIVE MAP (commander.cpp; design in docs/COMMANDER.md).
//
// Each team (1 US, 2 Japan) may have one commander. Any player can take the
// seat with /commander us|japan (a joiner's request goes to the host, as its
// other packets do) and give it up with /commander leave. The seat is kept by
// keep-alives: a joiner's game sends one every 5 s while it had input in the
// last 20 s; the host's own seat is kept by the host's input. A seat with no
// keep-alive for 20 s is free. The host decides everything: the seat table and
// the cooldowns. Packets: "BF43M!<chat port>|<body>" (player to host: "s",
// "l", "k", "t <team>", and the host's own "c <seconds>") and "BF43M!h<team>" /
// "BF43M!n" (host to player: holds the seat / does not).
//
// The live map: while a match is recorded (replay.cpp takes the soldiers,
// vehicles and capture points ten times a second) a small read-only HTTP server
// on the host's own address serves it: "/" is a page, "/livemap.json" the data.
// Port: TCP 19440 + the port offset (BF_NO_LIVEMAP turns it off).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace rt::commander {
enum Team { kNoTeam = 0, kUS = 1, kJapan = 2 };
// Battlefield 1943 only (the runtime is shared with Bad Company, which has none).
bool Available();
int ParseTeam(const std::string& word);  // "us", "1", "japan", "jp", "2": a team, else 0
const char* TeamName(int team);

struct Reply {
  std::string text;  // what the requester is told ("" none)
  int holds = 0;     // the team the requester holds now (0 none)
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
// Joiner: the host's answer about this game's seat ("h<team>" or "n").
void OnHostReply(const std::string& body);

// Starts the seat expiry and the live map server (admin::Start). announce tells
// the players of a seat that was freed.
void Start(std::function<void(const std::string&)> announce);

// Cooldowns: an action by a team's commander (the phase 2 orders and drops) is
// allowed once its cooldown is over, and only from the seat's holder. False
// (with the seconds to wait) otherwise; a true answer starts the cooldown.
bool TryAct(int team, uint32_t ip, uint16_t port, const std::string& action, double* wait_seconds);
int Cooldown();  // seconds, for every action (5 to 600; /commander cooldown)
}  // namespace rt::commander
