// Chat and admin commands (both games): T opens a chat line drawn over the
// game (fps_overlay.cpp); a line starting with '/' is a command. The host of
// a LAN game, or a dedicated server (also on its console, stdin), runs them:
// weather, time of day, server name and password. Joiners may chat, but only
// the host's commands change the game.
#pragma once
#include <string>
#include <vector>

namespace rt::admin {
// Starts the keyboard chat (Windows) and, on a dedicated server, its console.
void Start();
// While a chat line is being typed: the game's keys and mouse are left alone.
bool Typing();
// The lines to draw: recent chat, the line being typed; true if any.
bool Lines(std::vector<std::wstring>& out);
// One line (a command with '/', else chat). console: from the server's stdin.
void Execute(const std::string& line, bool console);
// A bot's chat line (all-talk, as "BOT n: ...").
void Say(const std::string& name, const std::string& text);
}  // namespace rt::admin
