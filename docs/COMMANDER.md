# Commander (Battlefield 1943)

A commander for each team, as in Battlefield 2: one player per team who sees the whole
battlefield, gives orders and calls in drops. Phase 1 made the seat, the live map and the
cooldown plumbing; phase 2 the commander's page (orders, drops) and the seat's team check. All
of it is decided on the host (a LAN host or a dedicated server); players only send requests.

Code: `bf1943-recomp/runtime/commander.h`, `commander.cpp` (seats, teams, orders, the HTTP
server and its page), `commander_world.cpp` (the drops, on the server's thread); hooks in
`admin.cpp` (the chat command and its packets), `admin_server.cpp` (`PlayerName`), `replay.cpp`
(the live sample), `bot_players.cpp` (the server's update calls `commander::OnServerUpdate`),
`bots.cpp` and `bots/brain.cpp` (the bots follow the orders).
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

### Known gaps in phase 1 (the first two are closed by phase 2)

- The seat is not checked against the requester's team: a joiner's chat address can't yet be
  tied to its soldier. Phase 2 needs that link (see below) before orders matter.
- The map has no in-game view. The commander watches it in a browser (or on a second screen).
  The website seat ("closed browser" expiry) is not done: the browser is read-only and holds
  no seat.
- No key binding: the chat command is the only way to sit, to avoid taking a key the game uses.
- The live map server is Windows only. A Linux dedicated server has the seats but no map.

## Phase 2: the commander's page, orders and drops

What was checked in game (Wake Island, port offset 281, 6 bots): on a dedicated server the
console takes a seat and gets its link; `/api/state`, wrong tokens, unknown actions, a click off
any flag, the order cooldown, the rate limit (429) all answer as described; an attack order
makes the team's bots choose that flag (logged "the commander's order: flag 0"); the seat frees
20 s after the page stops polling. On a windowed host the host's own player was found under its
camera (Japan), `/commander us` was refused and `/commander japan` taken, `me` pointed at its
soldier, a defend order sent the Japanese bots to the flag and the supply drop and orders were
told to the player (log: "told 1 players"; the chat overlay isn't in `--capture` frames). Not
checked: a second PC joining (the eye packet and the joiner's link are only exercised on the
host), a soldier actually healed by a supply drop (nobody was hurt in its zone during the test).

### Teams: which soldier is whose

- Every joiner's game sends the host where its camera is, `BF43E!<chat port>|x,y,z`, every 5 s
  while it is in a level (a new packet kind: older hosts ignore it). The host looks in its live
  sample (`replay::LiveCopy`, which now has each soldier's height) for a soldier on foot whose
  feet are 0 to 2.5 m under that eye and within 2 m across (none if soldiers of both teams are
  there). That soldier's player id (the recorder's, which stays the player's across lives) and
  team are kept for that chat address until the level ends.
- The host's own player is found the same way from its own camera, once a second.
- `/commander us|japan` is refused unless the requester's team is known and is that team ("you
  play for Japan: /commander japan"; "deploy and walk a few steps on foot first"). The
  dedicated server's console may take either seat.
- Limit: the eye is the joiner's own report. A modified client could claim another place; the
  host only checks that a soldier of that team stands there. A server-side tie (the join's
  address to its spawner in `bot_players.cpp`'s join hook) would close that; not done.

### The seat's token and the page

- Taking a seat makes a 16-hex-digit secret token. The host answers a joiner
  `BF43M!h<team>:<token>` (older joiners read only the team); the joiner's game builds the link
  from the host's address and port offset, `http://<host>:<19440 + offset>/?k=<token>`, shows
  it in chat, writes it to its log and copies it to the clipboard. The host (or the console)
  gets its own link with its LAN address. `/commander` alone shows the link again.
- The page is the live map (same address, one self-contained page, no external files): the
  level's own minimap picture under it (`/minimap.png`, made from the host's own game files as
  for the bots' advisor), flags in their owner's colour, soldiers, vehicles, orders (a dashed
  ring with ATTACK / DEFEND), supply zones, and the commander's own soldier marked "you". With a
  token it shows the commander's bar: the team, each action's cooldown, the last notes.
  Clicking a flag offers Attack / Defend; clicking anywhere offers Supply drop / Vehicle drop
  (and Cancel our order).
- **Keep-alive.** Once the page has polled `/api/state` (it does once a second) the page keeps
  the seat: 20 s without a poll frees it ("...'s map page closed", told to everyone), and anyone
  of that team can take it. Before the page is first opened the game's keep-alives keep it, and
  a new seat lasts at least 90 s (time to open the link).

### HTTP API (GET, on the live map's port)

- `/api/state?k=<token>`: `{"ok":true,"team":1,"name":...,"me":<player id or -1>,
  "wait":{"order":s,"ammo":s,"vehicle":s},"cooldown":{...},"notes":[...]}`, or `ok:false` when
  the token holds no seat (now). It is the page's heartbeat.
- `/api/act?k=<token>&a=<attack|defend|cancel|ammo|vehicle>&x=<m>&z=<m>`:
  `{"ok":true,"text"}` or `{"ok":false,"error","wait"?}`.
- `/livemap.json` gained `frame` (the picture's square and orientation), `orders`
  (`team, kind, point, x, z, left`), `supply` (`team, x, z, left`) and the commanders' names.
- **Checks on every request:** the token must be exactly 16 hex digits and a seat's (now);
  query values are taken only if made of `[0-9A-Za-z.-]` (64 characters at most); the action
  must be one of the five; x and z must parse whole as finite numbers inside the level's square
  (the minimap's, else +-1024 m); an order must be within 60 m of a flag. Rate limit per
  address: 60 requests in 10 s and one action every 0.3 s (else 429); at most 256 addresses are
  tracked. As before: one request at a time, 2 s timeout, 2 kB request cap.

### Cooldowns

`TryAct` and the page's actions share one check per team and action: orders 10 s, supply drops
`/commander cooldown <5-600>` (60 s by default), vehicle drops twice that. A drop that can't be
done (no free jeep, water, the ground not read yet) gives its cooldown back.

### Orders

- Per team: attack or defend a flag, its place, a serial that changes with every new order
  (and with a cancel or the expiry), 180 s life. In `livemap.json` and on the page.
- **Bots** (`bots.cpp` -> `botapi::World::order_point/order_kind/order_serial`, bot API
  version 10): the director maps the order to the flag's index; in `brain.cpp` each bot of that
  team whose serial differs chooses its flag again, and `ChoosePoint` adds 70 to the ordered
  flag's worth (the spreading by mates still applies: about half the team goes).
- **Players**: each player of the team whose soldier the host knows gets a chat line with the
  way from where it stands, e.g. `[commander] ATTACK the Japanese flag 230 m north-east of you`
  (north = up on the level's minimap picture). A player the host doesn't know yet gets none.
  The order is also on the HUD (phase 3, below).

### Drops (`commander_world.cpp`, on the server's thread)

The page's drop is queued; `commander::OnServerUpdate` (called from the server update hook in
`bot_players.cpp`, 82414430) does it between the server's own steps.

- **Heal zone (the page's "heal"; phase 2 called it the supply drop, "ammo").** BF1943 has no ammo crates in its levels: the engine has
  `ServerAmmoCrateEntity` / `AmmoCrateEntityData` (seen in the `BF_TYPE_DUMP` list, Bad
  Company's heritage), but BF1943's soldiers never run out of ammunition and heal by
  themselves, and no level places one. So the supply drop heals instead: for 30 s, every half
  second, the team's soldiers within 15 m of the point are set to full health (server soldier
  +3C).
- **Vehicle: NOT WORKING, off by default** (`BF_VEHICLE_DROP=1` turns it on for testing; the
  page shows the button greyed "(not yet)" and the server refuses it otherwise). Tested on a
  windowed host (Wake Island, the Japanese carrier): the server's jeep record moves (the live
  map and the vehicle entity read the new place), but the game keeps drawing the jeep where it
  was parked and nothing is there to enter at the drop point. What was tried:
  The team's free jeep (`Willys` for the US, `Kurogane` for Japan, else any jeep;
  free: no soldier within 6 m) nearest the point is moved there. A parked vehicle's Havok body
  is not simulated (its motion is another class than a driven one's: vtable 820B0AD4 against
  820B0A04) and there are two physics worlds in a host (the server's and its own client's), so
  the drop moves every copy of where the jeep stands (16-byte aligned vectors within 5 cm of
  it: its entity's, both bodies' transforms) and the bodies' centres of mass by the same step,
  and keeps the jeep's height over the ground (`BF_DROP_HEIGHT` adds metres; default 0.3). The
  ground there is the terrain or the map's collision above it (a deck, a pier); a point that is
  blue on the minimap picture is refused as water. The map's ground is read in the background
  once a seat is taken (the bots' `SightForMap`); a drop before it is ready is refused ("try
  again in a minute").
  First try: only the Havok body's transform (+E0, +F0, +100): the body kept the new values but
  never fell (not simulated) and the entity kept the old place. `BF_DROP_PROBE=1` logs the
  parked body beside a driven one's (the different motion vtables, the two worlds).
  **Next steps:** find how a parked vehicle is turned dynamic when someone gets in (the motion
  type change on entering) and do that before moving it; or respawn it the game's way: the
  level's `ServerVehicleSpawnEntity` (vtable 82018898) moved to the point and made to spawn
  (its transform and its respawn trigger are not found yet); or, simplest fallback, tell the
  team where the nearest free jeep is.
  Phase 3 found the first half of that: a vehicle's `+EC` is its spawner (vtable 82018898,
  `BF_VEHICLE_PROBE=1` logs each jeep's; 33 spawners on Guadalcanal) and the spawner's
  `+310` is where it makes it (the parked jeep's place, or its first place for a jeep that has
  been driven). Still missing: what makes a spawner make its vehicle again. The way to find
  it is the game's own respawn after a jeep is destroyed, which needs a destroyed jeep: the
  vehicles' health is not known (so the airstrike can't do it), and in the scripted test
  (`BF_CHANGE_PROBE`, the jeep nearest the host watched while the host turned and fired at
  it) the jeep 5 m away was driven off by a bot before the shots and the probe followed one
  37 m away: no hit, no health. The drop stays off.

## Phase 3: on screen, heal zones, airstrike, the page's chat

Checked in game (Guadalcanal, a windowed host on port offset 301, 8 bots, the host's player
on the Japanese team; frames of the window and a headless browser's shot of the page): the
heal zone's ring and the airstrike's ring on the ground in the first-person view, the panel
with their seconds, the order's marker and label with the flag's name, the airstrike's damage
(the host's soldier 7 m from the centre: 100 -> 31, then healed back to 100 by the zone), the
chat round trip. Not checked: a second PC (the "BF43Z!" / "BF43X!" packets are sent to the
known joiners but no joiner was there), the airstrike's smoke on a joiner's screen.

### On screen (`commander_hud.cpp`, drawn by the UI overlay in `xenia_shims/bridge.cc`)

- **The host decides, the team sees.** Once a second (the seats' expiry thread) the host sends
  each team's state to that team's known players only: `BF43Z!` + `zones|order|strike`
  (`z<kind>,x,y|n,z,left;...`, `o<kind>,x,y|n,z,left,flag,name`, `s x,y|n,z,left`). A joiner
  keeps the last state for its own team (its seat's team as the host told it); the host's own
  game keeps its own team's directly. The other team gets nothing: no packet, no ring, no line.
- **Projection.** `scene_fx` keeps the main view's world -> clip matrix of the last frame
  (`scenefx::ViewProj`, `xgpu::ViewProjection`; only with the scene effects on, as the
  enhanced preset has them). Clip y points up (checked: with it the other way a ring 5 m
  ahead was drawn above one 20 m ahead).
- **The discs.** A zone is a see-through filled disc on the ground (green heal, amber ammo,
  red airstrike) with a soft wide edge and a crisp one: 48 sides clipped in clip space at the
  near plane (standing inside one, the part behind the eye goes, so no line stretches across
  the screen) and at the window's edges, then filled as a fan. Inside a zone its fill is
  fainter; past 80 m from its edge it fades, to a third at 400 m. Checked from inside a heal
  zone, from 30 m off its edge and with the airstrike's 30 m ahead.
- **Ground height.** The host sends the ground at the point: a soldier's feet within 15 m
  when there is one (the surface walked on), else the terrain, or the map's collision up to
  2.5 m above it (a deck; higher is a roof). The players' game uses its own feet's height
  instead within 60 m when the two are less than 10 m apart. Guadalcanal's airfield shows
  why: its terrain is 2 m over the apron where the host stood and 8 m under it 45 m on, and
  the collision there found the sea floor (173) or a hangar roof (196) for a surface at 190;
  with the terrain alone a zone 45 m ahead was drawn near the player's feet.
- **What is drawn.** Heal zones: a green disc of 15 m on the ground. The airstrike's warning:
  a red disc of 12 m until the blast. The order: a square at the flag (red attack, blue
  defend) with its label (`ATTACK Japanese bunker 2:57 173 m behind`), kept on the window's
  edge when the flag is off screen and on the bottom edge when it is behind. A panel at the
  top right lists the zones, the airstrike and their seconds with the distance and the way
  (ahead, left, right, behind). Text is GDI (Windows only, as the live map).

### Heal and ammo zones

- The page's "Heal zone here" (action `heal`) is phase 2's supply drop (same healing), now
  shown on the HUD. Its own cooldown key `heal`.
- **Ammo zones: not working, refused** (`ammo zones are not working yet`). What is known:
  the HUD shows 30 rounds in the magazine, 3 spare magazines and 2 grenades; firing empties
  the magazine and a thrown grenade's icon becomes a refill timer (BF1943's grenades come
  back by themselves), the spare magazines did not change. Where the server keeps the counts
  was not found:
  - `BF_AMMO_PROBE=1` (bots fighting, small numbers dropping by 1 to 5): its best path,
    `[[soldier+18]+50]+94`, is one object shared by every soldier.
  - `BF_CHANGE_PROBE=1` (this game's own soldier, its words to +600 and two pointers on to
    +200 and +80, every 500 ms, against scripted presses: about 75 rounds fired in three
    bursts, a reload, three grenades; `--press 150:rt*2.5,...,168:x,175:lb,...`): no path
    counts down from 30 or from 2. What changes with the firing are animation states and
    buffers. The counts are further away (the soldier's weapon objects past those ranges) or
    only in the client's own soldier (the HUD's), not the server's.
  Next: the client soldier's weapon (ClientSoldierEntity, what the HUD reads), or the game's
  reload function found from the reload input.

### Airstrike (action `strike`)

- Called on the page ("Airstrike here"), its own cooldown of 180 s. The calling team's
  players get a chat line (`AIRSTRIKE in 8 s: 7 m west of you`) and the red ring and seconds
  on the HUD; the other team gets nothing until the blast.
- After 8 s the server (`commander_world.cpp`, its update) takes health off every soldier,
  of both teams, within 12 m: all of it within 4 m, falling to a fifth at the edge. Vehicles
  are not hurt (their health is not found).
- Its look: the scene effects' explosion (smoke, dust, debris: `xgpu::ExplosionAt`, what the
  game's own explosions add), five of them (the centre and 5 m around), in the host's game
  and in every joiner's (`BF43X!x,y,z`, to both teams: everyone sees a bomb fall). There is
  no game explosion object (the flash, the sound, the game's own damage): the server makes
  none. Like the game's own explosions' smoke, it is drawn only with the PC setting
  EFFECTS > EXPLOSION SMOKE on (off by default; `BF_PARTICLES=1` forces it): checked in game
  with it on (a smoke column and debris at the point 14 m ahead). With it off the blast is
  invisible (the warning ring just goes).

### The page

- Restyled as the game's field orders: paper and ink, olive and a stamp red, typewriter text
  (system fonts: Courier New, Arial Narrow / Black; no files, no web fonts), stamp buttons,
  the map beside the commander's sheet. The flags carry their names from the level
  (`MP_XX_The Hill` -> `THE HILL`, the bases `US base` / `Japanese base`: the capture point's
  data `*(+14)`, its name `+C`; `livemap.json` points have `name`).
- **The team log and its chat box.** `/api/chat?k=<token>` gives the last 100 lines the
  commander may see: all talk (`t:"all"`: every line of the game's chat on the host, the
  bots' too) and the team's own lines (`t:"team"`: the commander's notices to its team, its
  own lines). `/api/say?k=<token>&t=<text>` (at most 200 bytes, control characters taken
  out) sends a line to the team's players, as `[commander] Japan team: <name>: <text>`, by the
  game's chat path, and only to that team. Both check the token's seat on every request.

## Hardening (the bug-hunt pass)

- **The live map binds the host's own LAN address** (the address the game found on its default
  route, or `--lan-ip`), not every address. `BF_LIVEMAP_PUBLIC=1` binds all of them. If no
  route is known it binds loopback. Checked: a LAN-bound page answers (200), and a second client
  that connects and sends nothing does not delay a request (16 ms).
- **Per-connection handling:** each request has its own thread (8 at once, later ones are
  dropped); a receive deadline of 3 s for the whole request, send deadline 8 s; the answer is
  built one request at a time.
- **Eye packets (`E`) only from a registered player** (one that said hello on that chat port).
  The team is the soldier's under the eye, never the sender's claim.
- **Admin password guesses:** 5 wrong per source in 10 s, then refused until the window ends.
- **Fixed in the second pass:** the chat relay only for a peer that said hello, 10 lines a 2 s
  per source, the log line cut to 120 bytes; admin commands and seat requests only from a
  registered peer; the beacon answers 30 queries per 10 s per source; the known-player table
  forgets a player with no eye packet for 120 s (the cap of 64 stays); the heal zone rescans
  the soldiers every 10 s (not 2 s; a soldier made in between waits for the next scan); the
  vehicle drop writes only the body's words (its transform and centres of mass, each checked
  against the body's vtable), with no memory-wide scan; the release script checks XenonRecomp
  and the shaders before it copies anything (Windows).
- **Checked, no change needed:** the pad pointer (every caller holds the pad lock while it uses
  the pointer).
- **Vehicle drop (new spawn): not done.** The level's spawner vtable (82018898) was dumped in
  game (BF_SPAWN_PROBE=1): its methods are readable from the guest image (slot 2 is a short
  getter, slot 14 and others take an object pointer). Which slot creates a vehicle has not been
  identified by reading their code; that is the next step. The drop stays off.