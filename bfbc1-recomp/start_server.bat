@echo off
rem Battlefield: Bad Company dedicated server (Gold Rush): hosts on your network (UDP 1503, browsers on UDP 19431).
rem Edit the name, maps and bots below.
rem Maps: mp_harbour_day1 (Valley Run) mp_harbour_day2 (Deconstruction) mp_ignition_day1 (Oasis)
rem       mp_ignition_day2 (Final Ignition) mp_monastery_day1 (End of the Line) mp_monastery_day2 (Ascension)
rem       mp_beachhead_day1 (Harvest Day) mp_beachhead_day2 (Over and Out)
set GAME=..\bfbc1\extracted
set NAME=My Server
rem SERVERNAME: what server browsers show (default: the name above).
set SERVERNAME=%NAME%
set MAPS=mp_harbour_day1,mp_harbour_day2,mp_ignition_day1,mp_ignition_day2
set BOTS=0
rem TICKRATE: 75 (the default), 60, 30 (the original game's rate) or 100.
set TICKRATE=75
rem WEATHER: clear or rain. TIME: dawn, day, sunset or night (or hours, 12 noon).
set WEATHER=clear
set TIME=day
rem PASSWORD: a word players must type to join. Empty: anyone may join.
set PASSWORD=
rem HARDCORE=1: no HUD or crosshair for the players, soldiers take twice the damage.
set HARDCORE=0
rem ADMINPASSWORD: lets an admin who joined type /login <password> and then /kick, /ban, /map...
rem (the console of this window takes them without it: kick Bob, ban Bob, unban Bob, map oasis, bots 8, players).
set ADMINPASSWORD=
rem PUBLISH=1: also listed on the internet (the master server), not only on your network;
rem your router must pass UDP ports 1503 and 19431 to this PC.
set PUBLISH=0
set EXTRA=--tickrate %TICKRATE% --weather %WEATHER% --time %TIME%
if not "%PASSWORD%"=="" set EXTRA=%EXTRA% --password "%PASSWORD%"
if not "%ADMINPASSWORD%"=="" set EXTRA=%EXTRA% --admin-password "%ADMINPASSWORD%"
if "%HARDCORE%"=="1" set EXTRA=%EXTRA% --hardcore
if "%PUBLISH%"=="1" set EXTRA=%EXTRA% --publish
cd /d "%~dp0"
build\game\bfbc1.exe --game "%GAME%" --user server_data --dedicated --name "%NAME%" --servername "%SERVERNAME%" --maps %MAPS% --bots %BOTS% %EXTRA%
