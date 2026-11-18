# Commander (Battlefield 1943)

A commander for each team, as in Battlefield 2: one player per team who sees the whole
battlefield and, in phase 2, gives orders and calls in drops. Phase 1 (this change) has the
seat, the live map and the cooldown plumbing. All of it is decided on the host (a LAN host or a
dedicated server); players only send requests.

Code: `bf1943-recomp/runtime/commander.h`, `commander.cpp`; hooks in `admin.cpp` (the chat
command and its packets), `admin_server.cpp` (`PlayerName`), `replay.cpp` (the live sample).
Bad Company shares this runtime (its build compiles `bf1943-recomp/runtime/*.cpp`), so all of
it is gated by `commander::Available()`, which is true only for BF1943's LAN tag (`BF43`). Under
Bad Company there are no seats, no `/commander` command and no live map server.

## What exists and is reused

- **The host's view of the match.** `replay.cpp` (the match recorder) reads the server's
  soldiers through their spawners (8201A990, soldier at +120: position +180, team +48, health
  +3C), the vehicles (82013068, body transform) and the capture points (820190D0, owner +80)
  ten times a second while a level with players runs. `spectate.cpp` and the bots' director
  (`bots.cpp`) read the same objects. The live map takes the recorder's sample as it is; there
  is no extra memory scan.
- **Players and their addresses.** `admin.cpp`'s chat (UDP 19431 + offset) already carries a
  joiner's hello (name, stable id, ports) and the host's notices to it. `admin_server.cpp`
  keeps the joiners by (address, chat port).
- **No HTTP server existed.** `http.cpp` is only an outbound GET (the master server list), and
  the admin protocol is UDP. The live map adds a small read-only HTTP server (below).

## Phase 1

### The seat

- `/commander us` or `/commander japan` (chat on T, or the dedicated server's console without
  the `/`) takes that team's seat if it is free. `/commander leave` gives it up; `/commander`
  alone shows both seats. Anyone can do this, not only admins.
- A joiner's request goes to the host as `BF43M!<chat port>|t <team>` on the chat port (as its
  hello and `/login` commands do). The host answers `BF43M!h<team>` (you hold it) or `BF43M!n`,
  plus a chat line. A seat changing hands is told to everyone in chat.
- One seat per player: taking the other team's seat gives up the first one.
- **Heartbeat.** A joiner holding a seat sends `BF43M!<port>|k` every 5 s, but only while the
  PC had keyboard or mouse input in the last 20 s (`GetLastInputInfo`). The host's own seat is
  kept by the host's input. With no keep-alive for 20 s the host frees the seat and tells
  everyone. A player who quits stops sending, so its seat is freed the same way. Seats are
  also freed when a level opens or closes.
- A kicked or banned player's packets are already dropped (`ChatBlocked`), so it cannot sit.

### Cooldowns

`commander::TryAct(team, ip, port, action, &wait)`: allowed only from the team's seat holder,
and only once per cooldown for each (team, action); a refusal gives the seconds left. The
cooldown is the same for every action: 60 s by default, set with `/commander cooldown <5-600>`
(the host or the console only). Phase 1 has no actions yet, so nothing calls it.

### The live map

- The host serves it over HTTP on its own address: TCP **19440 + the port offset**
  (`http://<server address>:19440/`). `/` is a page that redraws once a second, and
  `/livemap.json` is the data:
  `{"ok":true,"map":...,"t":ms,"commander":{"us":bool,"japan":bool},"players":[{"id","team","x","z","yaw","hp","seated"}],"vehicles":[{"id","x","z","yaw"}],"points":[{"id","owner","x","z"}]}`.
- It runs only in a game that hosts (dedicated or `--host`), never in a joiner's game.
  `BF_NO_LIVEMAP=1` turns it off. If the port is taken, the log says so and it retries.
- **Cost.** It serves GET only, a request at a time (2 s timeout, 2 kB request cap), and
  rebuilds the JSON at most 5 times a second however often it is asked. It contains no player
  names and nothing from the game files, only ids, teams and positions.
- It shows data only while the match recorder runs (so not with `BF_NO_REPLAY`, and not in
  `--training`). Otherwise it answers `{"ok":false,...}`.
- For anyone outside the LAN to see it, TCP 19440 must be open in the host's firewall and
  router, as UDP 1003 and 19430 are for joining.

### Known gaps in phase 1

- The seat is not checked against the requester's team: a joiner's chat address can't yet be
  tied to its soldier. Phase 2 needs that link (see below) before orders matter.
- The map has no in-game view. The commander watches it in a browser (or on a second screen).
  The website seat ("closed browser" expiry) is not done: the browser is read-only and holds
  no seat.
- No key binding: the chat command is the only way to sit, to avoid taking a key the game uses.
- The live map server is Windows only. A Linux dedicated server has the seats but no map.

## Phase 2 (not built): orders and drops

All of these hook points were **found by reading the code, not verified in game**.

### Order markers

- A commander's order is a (team, kind, x, z) the host keeps for a while: "attack/defend this
  flag", "go here". It would come from the map page (a click sends a GET with the position,
  checked against the seat holder) or from chat (`/commander order <point id>`).
- For **bots**: the director in `bots.cpp` (`struct Director`, `Refresh`) already chooses
  flags from `points`. An order would weight the flag choice for that team's bots. The
  advisor's markers (`m.markers.push_back(... Marker::kObjective ...)`, about line 401, and the
  level markers about line 800) are where an order marker would also be shown to the bots'
  LLM advisor. Unverified.
- For **humans**: the minimap/HUD overlay (`game_patches.cpp`, the HUD drawing near line 293)
  could draw an order marker. A joiner would need the order sent to it (a `BF43M!o...` notice
  over the chat port, as the host's other notices are). Unverified.
- Every order goes through `TryAct(team, ..., "order", ...)`.

### Air drops (ammo, vehicles)

- **Ammo / supply crate.** No code here creates game entities yet. The engine's own spawning
  of a supply/ammo entity has not been found. Next step: `BF_TYPE_DUMP` (`type_dump.cpp`) to
  list engine type names and find the supply and ammo crate classes, then the server function
  that creates one (a call from the level's spawner entities). Unverified.
- **Vehicle drop.** Vehicles come from the level's vehicle spawners. The nearest working
  example of driving the engine's spawn logic from outside is the bots' soldier spawn in
  `bot_players.cpp` (a bot's spawn object `kSpawnObject` 8201A990 and its move receiver,
  `St32(b.mover + 0x30, b.spawner)`, about line 865). A drop could move an existing vehicle
  spawner's position, or respawn an idle vehicle (the vehicles `replay.cpp` already sees) at
  the commander's point. Both are unverified, and moving server objects must also replicate
  to the joiners.
- Each drop goes through `TryAct(team, ..., "ammo"/"vehicle", ...)`, with its own cooldown key.

### Also needed for phase 2

- **Seat-to-team check:** match a joiner's hello (its game port) to its server-side player.
  `net.cpp` knows each connection's address and port, and `bot_players.cpp` shows how a
  connection's spawner is found. That gives the requester's team, so only a team's own player
  can command it.
- The map page needs an authenticated action path (a per-seat token handed to the holder in
  chat, sent with each click) before it can issue orders.
