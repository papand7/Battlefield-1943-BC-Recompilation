Battlefield 1943 and Battlefield: Bad Company - dedicated server
=================================================================

This package has no game files and no recompiled game code: it is the sources, built on
your own machine from your own copy of the game. Nothing of the games is distributed.

  bf1943-recomp/     Battlefield 1943's sources (and the code both games share), start_server.*
  bfbc1-recomp/      Battlefield: Bad Company's sources, start_server.*
  docs/DEVELOPERS.md the developer guide (see "Linux dedicated server")
  build_server.sh / build_server.bat    builds the server (once per game)

1. Your game files (put them anywhere, e.g. next to this folder)
   Battlefield 1943: version 1.06: the Xbox 360 package, or the folder with default.xex.
   Bad Company: the USA disc (.iso), or the folder with default.xex.

2. Build the server, once (20-60 minutes per game; no launcher, no window)
   Linux / macOS:  ./build_server.sh --game <your 1943 package or folder>
                   ./build_server.sh --game <your Bad Company .iso> --bfbc1
     (If it needs packages it prints the install command. It never runs sudo.
      macOS: Xcode command line tools, Homebrew's cmake ninja python glslang spirv-tools
      and the LunarG Vulkan SDK; see build_linux.sh and the README. If the script is not
      executable: sh build_server.sh ... or chmod +x *.sh bf1943-recomp/*.sh bfbc1-recomp/*.sh)
   Windows:        build_server.bat --game <your 1943 package or folder>
                   build_server.bat --game <your Bad Company .iso> --bfbc1
     (Needs the Visual Studio C++ build tools, LLVM, CMake, Ninja, Python 3 and the Vulkan
      SDK installed; the top of build_server.bat lists the winget ids. The Windows server is
      the game's own program run with --dedicated, so it is a full game build.)
   Your game files are unpacked (a folder: linked or copied) to bf1943-recomp/original/v106
   and bfbc1-recomp/original/bfbc1. They stay on your machine.

3. Start it
   bf1943-recomp/start_server.sh   or  bf1943-recomp/start_server.bat     (Battlefield 1943)
   bfbc1-recomp/start_server.sh    or  bfbc1-recomp/start_server.bat      (Bad Company)
   Open the script in a text editor first: the options are the lines at the top.

Options (in the start scripts)
   NAME, SERVERNAME   the name players and server browsers see
   MAPS               the maps it rotates through
                      1943: Wake_island_s Iwo_Jima_s Guadal_Canal Coral_sea
                      Bad Company (Gold Rush): mp_harbour_day1 mp_harbour_day2 mp_ignition_day1
                      mp_ignition_day2 mp_monastery_day1 mp_monastery_day2 mp_beachhead_day1
                      mp_beachhead_day2
   BOTS               number of bots (0: none)
   TICKRATE           75 (default), 60, 30 (the original game's rate) or 100
   WEATHER, TIME      clear or rain; dawn, day, sunset, night (or an hour, 12 = noon)
   PASSWORD           players must type it to join (empty: anyone)
   ADMINPASSWORD      an admin who joined types /login <password>, then /kick /ban /map /bots
                      ... (the console of the server window takes them without it)
   HARDCORE=1         no HUD or crosshair, soldiers take twice the damage
   PUBLISH=1          also listed on the internet (the master server), not only on your network
   BOT_LLM=1          (1943) the bots ask a small vision model in hard spots; downloads about
                      3.5 GB into llm/ the first time; needs Python
   TRAIN=1            (1943) a training run: nobody can join, bots tune how they play

Ports (let them through the host's firewall; with PUBLISH=1 also forward them on your router)
   Battlefield 1943:        UDP 1003 (the game), UDP 19430 (server browsers, password check),
                            TCP 19440 (the live map)
   Battlefield: Bad Company: UDP 1503 (the game), UDP 19431 (server browsers)
   They differ, so both servers can run on one machine.

Server data (saves, what the bots learn: server_data/) is written next to the start script.
More: docs/DEVELOPERS.md, section "Linux dedicated server". Project:
https://github.com/papand7/Battlefield-Recompilation-Project
Battlefield is a trademark of Electronic Arts; a fan project, not affiliated with EA or DICE.
