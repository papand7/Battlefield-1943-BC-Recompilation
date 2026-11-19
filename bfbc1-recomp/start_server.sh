#!/bin/sh
# Battlefield: Bad Company dedicated server (Gold Rush) on Linux (bfbc1_server, see docs/DEVELOPERS.md):
# hosts on your network (UDP 1503, browsers on UDP 19431).
# Maps: mp_harbour_day1 mp_harbour_day2 mp_ignition_day1 mp_ignition_day2 mp_monastery_day1
#       mp_monastery_day2 mp_beachhead_day1 mp_beachhead_day2
GAME=../bfbc1/extracted
NAME="My Server"
SERVERNAME="$NAME"
MAPS=mp_harbour_day1,mp_harbour_day2,mp_ignition_day1,mp_ignition_day2
BOTS=0
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
# PUBLISH=1: also listed on the internet (the master server); UDP 1503 and 19431 must reach this machine.
PUBLISH=0
# (the options are the positional parameters, so a name or password with spaces stays one word)
set -- --tickrate "$TICKRATE" --weather "$WEATHER" --time "$TIME"
[ -n "$PASSWORD" ] && set -- "$@" --password "$PASSWORD"
[ -n "$ADMINPASSWORD" ] && set -- "$@" --admin-password "$ADMINPASSWORD"
[ "$HARDCORE" = "1" ] && set -- "$@" --hardcore
[ "$PUBLISH" = "1" ] && set -- "$@" --publish
cd "$(dirname "$0")" || exit 1
# the game files build_server.sh set up, and its build
[ -f original/bfbc1/default.xex ] && GAME=original/bfbc1
BIN=build/linux-bfbc1-server/game/bfbc1_server
[ -x "$BIN" ] || BIN=build/macos-bfbc1-server/game/bfbc1_server
[ -x "$BIN" ] || { echo "The server is not built yet: run build_server.sh --bfbc1 (see README-SERVER.txt)"; exit 1; }
exec "$BIN"--game "$GAME" --user server_data --dedicated --name "$NAME" --servername "$SERVERNAME" --maps "$MAPS" --bots "$BOTS" "$@"
