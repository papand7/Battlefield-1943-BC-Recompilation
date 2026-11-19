#!/bin/sh
# Battlefield 1943 dedicated server on Linux (bf1943_server, see docs/DEVELOPERS.md):
# hosts on your network (UDP 1003, browsers on UDP 19430).
# Edit the name, maps and bots below. Maps: Wake_island_s Iwo_Jima_s Guadal_Canal Coral_sea
GAME=original/v106
NAME="My Server"
# SERVERNAME: what server browsers show (default: the name above).
SERVERNAME="$NAME"
MAPS=Wake_island_s,Iwo_Jima_s,Guadal_Canal,Coral_sea
BOTS=0
# TICKRATE: 75 (the default), 60, 30 (the original game's rate) or 100.
TICKRATE=75
# WEATHER: clear or rain. TIME: dawn, day, sunset or night (or hours, 12 noon).
WEATHER=clear
TIME=day
# PASSWORD: a word players must type to join. Empty: anyone may join.
PASSWORD=
# HARDCORE=1: no HUD or crosshair for the players, soldiers take twice the damage.
HARDCORE=0
# ADMINPASSWORD: lets an admin who joined type /login <password> (the console takes commands without it).
ADMINPASSWORD=
# PUBLISH=1: also listed on the internet (the master server), not only on your network;
# your router must pass UDP ports 1003 and 19430 to this machine.
PUBLISH=0
# BOT_LLM=1: in difficult spots (stuck, losing a fight) the bots ask a small vision model
# (Qwen3.5 4B through llama.cpp, run on this machine) where to go. Installed into llm/ the
# first time (about 3.5 GB). Faster with a graphics card (Vulkan).
BOT_LLM=0
# TRAIN=1: a training run instead: nobody can join, 16 bots, the game twice as fast; the
# bots tune how they play (half of each team tries new settings each round, the better
# ones are kept: server_data/bot_knowledge/<map>.tune), ask about more (with BOT_LLM=1)
# and learn. It ends after TRAIN_MINUTES (real time) and writes a replay of the whole run
# (tracks, shots, kills, questions): open server_data/bot_advice/runs/<map>_<date>/index.html
TRAIN=0
TRAIN_MINUTES=30
# (the options are the positional parameters, so a name or password with spaces stays one word)
set -- --tickrate "$TICKRATE" --weather "$WEATHER" --time "$TIME"
[ -n "$PASSWORD" ] && set -- "$@" --password "$PASSWORD"
[ -n "$ADMINPASSWORD" ] && set -- "$@" --admin-password "$ADMINPASSWORD"
[ "$HARDCORE" = "1" ] && set -- "$@" --hardcore
[ "$PUBLISH" = "1" ] && set -- "$@" --publish
[ "$TRAIN" = "1" ] && set -- "$@" --train --speed 2 --train-minutes "$TRAIN_MINUTES"
cd "$(dirname "$0")" || exit 1
if [ "$BOT_LLM" = "1" ]; then
  if [ -f llm/bot_llm.ini ] || python3 tools/setup_bot_llm.py --dir llm; then set -- "$@" --bot-llm llm; fi
fi
# build_server.sh's build, or the one from the developer guide (build/linux)
BIN=build/linux-server/game/bf1943_server
[ -x "$BIN" ] || BIN=build/macos-server/game/bf1943_server
[ -x "$BIN" ] || BIN=build/linux/bf1943_server
[ -x "$BIN" ] || { echo "The server is not built yet: run build_server.sh (see README-SERVER.txt)"; exit 1; }
exec "$BIN"--game "$GAME" --user server_data --dedicated --name "$NAME" --servername "$SERVERNAME" --maps "$MAPS" --bots "$BOTS" "$@"
