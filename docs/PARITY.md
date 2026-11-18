# Feature parity: Battlefield 1943 (BF43) vs Battlefield: Bad Company (BC)

Read-only audit of the tree at commit 4b0f69b (no builds, no game runs: "working" below means the
code and docs say so, not that this audit ran it). Goal: both games on the same level.

Legend: **OK** present and working (per code/docs/commits) | **UNV** present but unverified (shared or
ported code, no test or doc says it works, or a doc says it is known-broken) | **N/A** shared code that is
not wired for this game, or not applicable | **MISSING** not there.

## How the two builds relate (read this first)

- BC compiles **all of `bf1943-recomp/runtime/*.cpp` except** `game_patches.cpp`, `bots.cpp`, `bot_players.cpp`
  (`bfbc1-recomp/game/CMakeLists.txt` line 40), plus its own `bfbc1-recomp/runtime/{bfbc1_game,bc_bots,bc_mp_menu,bc_mp_menu_posix}.cpp`
  and Detour. So `pc_settings.cpp`, `pc_menu.cpp`, `pc_ui.cpp`, `xam.cpp` (mouse/keys/pad), `gpu.cpp`, `audio.cpp`, `lan.cpp`,
  `fps_overlay.cpp`, `bot_llm.cpp`, `bot_knowledge.cpp`, `bot_tuning.cpp`, `nav_autogen.cpp` and the whole Xenia renderer are literally the same code.
- Per-game halves: `game_patches.cpp` + `bots.cpp` + `bots/brain.cpp` + `bot_players.cpp` (BF43) vs `bfbc1_game.cpp` + `bc_bots.cpp` (BC).
  `bfbc1_game.cpp` re-implements the BF43 guest hooks at BC addresses (FOV, camera shake, motion blur, HUD 30 Hz step, volumes, vehicle keys, shadow/vegetation/particle settings).
- There is **one** `pc_settings.cpp` row list (`Build()`), not filtered per game: BC shows every row BF43 shows (so a row can exist in BC's page and do nothing, see "UNV" notes).
- The BF43 PC main menu / pause menu (`CustomMenus()`) is on only for BF43 (`bfbc1_game.cpp:109` returns false). BC keeps the game's own menus with a patched
  PC SETTINGS row (`bfbc1-recomp/tools/bc_menu_patch.py`, `launcher/setup.js patchMenu`) and its own MP menu (`bc_mp_menu.cpp`).
- Stale docs: `bfbc1_game.cpp:1-3` ("No guest patches yet... bots are not supported") and the root README's BC "Not Done Yet" line (bots, HD pack) are out of date: commits 224ff21, 5960706, bcfa42b added them. BF43 README says Host/Join Game "don't work at the moment" while `docs/DEVELOPERS.md` and README's own LAN text say they do: conflicting.
- Not in the repo at all (neither game): frame generation, ultrawide/aspect option, plant wind, dust motes/leaves, adaptive quality, F2 burst
  (README mentions `--capture-burst`, a test option, not F2), `pc_look.cpp`/direct-aim file, mouse over RDP, binaural audio, free camera F3,
  spectator/replay viewer (only the bot *training* replay HTML), hardcore mode, weather/time of day, /kick /ban /map /bots admin commands, all-talk chat option,
  server password, macOS builds (only the texture-pack tool runs on macOS: commit cb014b7). Rows for these say MISSING / MISSING.

## 1. Graphics

| Feature | BF43 | BC | Evidence | Port effort (if one-sided) / key files |
|---|---|---|---|---|
| Scene effects (`--scene-fx`: fog, light shafts, GTAO, clouds, water, surface detail, muzzle-flash light) | OK | UNV | BF43 README "Scene effects", `scene_fx.cc`; BC uses the same renderer, presets in `main.cpp:182`; root README says BC ray-traced effects "stall the opening cutscene" | medium: verify on BC levels; the effects read the camera/sun/shadow-slice constants of the lit draws (`scene_fx.cc`) - BC shader constants unchecked. `bf1943-recomp/thirdparty/xenia/src/xenia/gpu/vulkan/scene_fx*.cc` |
| Ray-traced sun shadows (BLAS from guest meshes) | OK | UNV | BF43 README RT section; BC README "ray-traced shadows (they stall the opening cutscene)" under Not Done | medium-large: the mesh decode recognises BF43's vertex-shader patterns (c12-c15 clip, c16-c18 world, foliage c20-c22, skinned/multipart); BC needs its own shader pattern list. `scene_fx_rt.cc` |
| Screen-space shadows | OK | UNV | same renderer, setting `shadows` | small: verify |
| Bounce light (GI), cloud shadows, water foam/refraction/shallows | OK | UNV | BF43 README; BC water differs (rivers, lakes); sea-level logic is Wake Island specific ("the sea at sea level measured from hulls") | medium: sea-level/plane assumptions in `scene_fx.cc` |
| Shadow map size/distance (`--shadowmap`, `--shadow-distance`, SHADOW DETAIL) | OK | OK | `bfbc1_game.cpp:706-770` (WorldRender vtable 820632A0, same fields), launcher `shadowDetail` | n/a |
| Vegetation draw distance (`--lod`, `--overgrowth`) | OK | OK | `bfbc1_game.cpp:729,819` | n/a |
| Particle quality / soft particles / explosion smoke (`--emitter`) | OK | OK | `bfbc1_game.cpp:771-783`; explosion position hook `:639` | n/a |
| PBR materials (`--pbr`, shader table) | OK (183-line table) | OK (180-line table) | `bf1943-recomp/data/pbr_shaders.txt`, `bfbc1-recomp/data/pbr_shaders.txt` (copied next to the exe by `game/CMakeLists.txt:59`) | n/a (table is per game; `tools/pbr_taps.py`) |
| Parallax occlusion | OK | UNV | needs pack normal maps with height; BC pack made 2026 (bcfa42b), not described as tested | small |
| HD texture pack + tool | OK | UNV | `tools/texpack`; launcher MAKE/UPDATE for BC too (bcfa42b: `texpack_bfbc1`/`texpack-bfbc1`); root README still lists BC pack as not done | small: run and check; `launcher/main.js` `games.bfbc1.textures` |
| 16x anisotropic filtering (`--aniso`) | OK | OK | shared `main.cpp:310` | n/a |
| Upscalers: FSR, CAS, bilinear | OK | OK | shared; `--sharpen fsr|cas|off`; BC launcher passes the same args (`main.js:441`) | n/a |
| Upscalers: FSR 3, DLSS, AUTO, upscale quality | OK | UNV | rows `upscaling`/`upscale_quality` in the shared page; BC CMake copies `amd_fidelityfx_*_vk.dll`, `nvngx_dlss.dll` (`game/CMakeLists.txt:66-71`); temporal needs motion vectors/jitter - BC unchecked | small-medium: verify jitter/motion-vector capture on BC draws. `bf_fsr3.cc`, `bf_dlss.cc` |
| Frame generation | MISSING | MISSING | not in the tree | large for both (needs motion vectors + presenter work) |
| Sharpening | OK | OK | CAS/FSR sharpen pass in swap | n/a |
| HDR10 (+ white/peak rows) | OK | UNV | shared `bf_hdr.h`, `--hdr`; BC menus use a different look, "front end no_stretch" is set by BF43 menu code | small: check BC front end brightness. `pc_menu.cpp`, `bfhdr::no_stretch` |
| Looks: stock/enhanced/vivid/toon/noir/ww2 | OK | UNV | `--preset`, `--look` in `main.cpp`; renderer-only | small |
| Motion blur toggle | OK | OK | `bfbc1_game.cpp:576-598` | n/a |
| Camera shake scale | OK | OK | `bfbc1_game.cpp:555` | n/a |
| Field of view row | OK | OK | `bfbc1_game.cpp:495-550` | n/a |
| Resolution scale / window size / display mode / vsync / FPS limit | OK | OK | shared rows; BC launcher defaults to 1x scale (`renderer.js:87`) | n/a |
| Client sim at frame rate above 30 Hz | OK | OK | `game_patches.cpp:43-90` / `bfbc1_game.cpp:229-257` | n/a |
| HUD/menu Flash animation fix above 30 fps | OK | OK | `bfbc1_game.cpp:601-620` | n/a |
| Physics-effects timing fix | OK | OK | `bfbc1_game.cpp:622` | n/a |
| Brightness / gamma rows | OK | OK | shared final pass | n/a |
| Blood and gore | OK | UNV | `--blood`; BC soldier layouts `g_blood_server/client` in `bfbc1_game.cpp:117` but "the blows' impulses are not known" so explosion dismemberment only on health-based rule; README says BC blood not done | medium: find BC impulse offsets for `SoldierLayout` (killing-blow impulse) |
| Ultrawide / aspect ratio option | MISSING | MISSING | none | medium for both (viewport/HUD scaling, FOV by aspect) |
| Plant wind, dust motes, falling leaves | MISSING | MISSING | none | medium (new effects in `scene_fx`) |
| Adaptive quality | MISSING | MISSING | none | medium (frame-time governed `scenefx::Configure` presets) |
| QUALITY PRESET (LOW..ULTRA, CUSTOM) for effects | OK | OK | shared `pc_settings.cpp:196` | n/a |
| Foliage / terrain ray tracing specifics (heightmap BLAS, billboards) | OK | UNV | BF43 vertex shaders `1B4EBA8B2370E9A7`, `ACDC14B71948C0B0` | large for BC (own terrain shaders, own heightmap format) |

## 2. Input

| Feature | BF43 | BC | Evidence | Port effort / files |
|---|---|---|---|---|
| Raw mouse look (direct aim, unclamped, no stick deadzone) | OK | OK | shared `xam.cpp` mouse path; BC `bfbc1_game.cpp:167` adds mouse into the right-stick axis read | n/a (no `pc_look.cpp` exists; the raw-input path lives in `xam.cpp`) |
| Mouse sensitivity, aim sensitivity, invert | OK | OK | shared rows `mouse_sens`, `aim_sens`, `invert_mouse`; aim scaling reads the FOV controller (`+0x28/+0x30`), ported | n/a |
| Mouse over RDP / remote desktop | MISSING | MISSING | none | small-medium (relative-mode fallback in `xam.cpp`) |
| Key rebinding (16 actions, 2 keys each) | OK | UNV | `kActions` in `pc_settings.cpp:93`; the action to pad-button map is BF43's layout (xam.cpp "Battlefield PC (Bad Company 2) layout" comment) and BC's weapon/gadget/grenade buttons differ (BC has a gadget/specialisation button, no knife-only) | small: add BC action list (BC has 4-5 more buttons: gadget, change seat) |
| Key/button prompts (KEYBOARD, XBOX, PS3, PS5, DECK) | OK | OK | `pc_ui.cpp` atlases: BF43's plus `bc_buttons`, `bc_hud`, `bc_igm_mp` (commit 224ff21) | n/a |
| Vehicle keys (W/S throttle-brake, A/D steer, arrows fly) | OK | OK | `bfbc1_game.cpp:902-1020` (LocalVehicle; AH60 flies) | n/a |
| Plane mouse sensitivity / invert | OK | N/A | BC has helicopters (AH60) only; the rows are in BC's page and act on `g_local_flying` | small: verify helicopter response; `xam.cpp:430` |
| Vehicle guns / turrets aimed by the mouse | OK | MISSING | `game_patches.cpp:458-608` sets `g_vehicle_mouse_stick`; no BC code sets it (`kernel.h:131`, BC null) | medium: find BC turret helper (the `sub_822ADE38` equivalent), same 0x34/0x44 fields. `bfbc1_game.cpp` |
| Aim toggle / sprint toggle rows | OK | UNV | shared xam.cpp | small: verify |
| Gamepad | OK | OK | XInput; BC menus pad-only by design | n/a |
| Mouse in menus (hover, click, back prompt) | OK | UNV | BF43: `bf_menu.h` row capture; BC: `bfbc1_game.cpp:697` pushes the "Menu" input context; `bc_mp_menu.cpp` has own handling | small |
| F10 settings / Alt+Enter fullscreen / F2 FPS | OK | OK | shared; BC via PC SETTINGS row and F10 | n/a |
| F2 burst capture, F3 free camera | MISSING | MISSING | none | medium (free camera needs a camera hook; `bots.cpp`'s `CameraEye` is read-only) |

## 3. Multiplayer

| Feature | BF43 | BC | Evidence | Port effort / files |
|---|---|---|---|---|
| LAN host / join (menu) | UNV (README: Host/Join "being rebuilt, don't work"; DEVELOPERS: works; tested 2 copies on 1 PC) | UNV (works on 1 PC, "across two PCs not tested") | READMEs; `pc_settings.cpp:938` (BF43 host/join pages), `bc_mp_menu.cpp` (BC) | small: verify on two PCs, firewall UDP 1003/19430 resp. 1500-1503/19431 |
| Join by address | OK | OK | BF43 `--join <addr> --host-map`; BC menu "JOIN BY ADDRESS" (`:port`) | n/a |
| Server browser | OK (LAN + master list) | OK (LAN; internet list via shared `lan::Refresh`, only if servers publish) | `lan.cpp:201`, `bc_mp_menu.cpp:541-600` | n/a |
| Server name | OK (`--name`, launcher, start_server) | OK via launcher SERVER page / `--name`; BC in-game HOST menu has no name field | `main.cpp:213`; `bc_mp_menu.cpp:579` (MAP only) | small: add NAME/BOTS rows to BC host page |
| Password / private server | MISSING | MISSING | none | medium: handshake token in `net.cpp`/`lan.cpp` |
| Master server beacon (`--publish`, "List on the internet") | OK | UNV | `lan.cpp:52` `MasterGame()` returns "bfbc1" for BC, `--publish` shared; BC menu has no checkbox; own master server source in `tools/masterserver` | small: add LIST ON INTERNET row to BC host page; test the `bfbc1` game key against the master |
| Dedicated server (`--dedicated`) | OK (Windows + Linux) | OK (launcher SERVER page; Linux build `bfbc1_server`) | commits 2e259c1, 0784bbe, abd13b0; `launcher/main.js:216-224` | n/a |
| Server start scripts (`start_server.bat/.sh`) | OK | MISSING | the scripts hardcode BF43 maps/ports/`original\v106` | small: clone with BC maps `mp_harbour_day1...`, ports 1500-1503 |
| Server GUI (`server_gui.bat/.sh`, launcher SERVER page) | OK | OK | "SERVER page (both games)" (bcfa42b) | n/a |
| In-game host options: map | OK (4 maps) | OK (8 Gold Rush maps) | `kMaps` / `bc_mp_menu.cpp:76-100` | n/a |
| In-game host options: bots count | OK (0-15) | MISSING in menu (launcher/dedicated `--bots` works) | `pc_settings.cpp:1006` vs `bc_mp_menu.cpp:579` | small: add BOTS row to `bc_mp_menu.cpp`, call the same bot-start path |
| All-talk / chat settings | MISSING | MISSING | none | medium-large (game chat is the console's party/voice path) |
| Admin commands /kick /ban /map /bots | MISSING | MISSING | none (server console is log only) | medium for both (chat or stdin command parser in `main.cpp` + kick via connection close hook: `bfbc1_game.cpp:685` / `game_patches.cpp:277`) |
| Idle-player kick handling | OK (timeouts raised, vanished players dropped 180 s/30 s) | OK (long timeouts, game's 2 min spawn-screen rule kept) | `game_patches.cpp:209`, `bfbc1_game.cpp:410` | n/a |
| Online (EA / Arcadia emulator) | UNV (`server/arcadia360`, optional) | MISSING ("playing online" not done) | READMEs | large for BC |
| Two games on one PC | OK | OK | BC ports = BF43 + 500 (`bfbc1_game.cpp:39`) | n/a |

## 4. Game modes

| Feature | BF43 | BC | Evidence | Port effort |
|---|---|---|---|---|
| Native modes | OK: Conquest (Wake, Iwo Jima, Guadalcanal), Air Superiority (Coral Sea) | OK: Gold Rush only (disc has no Conquest) | READMEs | N/A (BC Conquest arrived in a later title update; not on the disc) |
| Hardcore | MISSING | MISSING | none | medium: server-side settings (HUD, friendly fire) via engine `Table.Member` settings (`--args`, `XenonGameSettings.cfg`) |
| Weather / time of day | MISSING | MISSING | none | medium-large (sun/sky constants are read by `scene_fx`; level lighting data) |
| Campaign / single player | N/A: tutorial level only (no campaign in XBLA game) | OK: campaign plays ~100 fps at 1x; saving unchecked | README BC section | N/A |

## 5. Bots (server-side players)

| Feature | BF43 | BC | Evidence | Port effort / files |
|---|---|---|---|---|
| Bots as players inside the server (no game copy per bot) | OK (default; `BF_BOT_COPIES=1` old way) | OK | 1b94108, 0f1cfe3; `bot_players.cpp` / `bc_bots.cpp` | n/a |
| Infantry: nav-mesh routes, fight, sprint, jump, crouch, unstick, line of sight | OK | OK | `bots/brain.cpp`, `bots/nav.cpp` / `bc_bots.cpp`; LOS via collision (`0d7ea49`) | n/a |
| Objectives | OK (control points, temperature map) | OK (Gold Rush crates: arm / defuse / guard) | README / 224ff21 | n/a |
| Mixed kits, grenades | OK (kits with own first weapon) | OK | 97f1d18, f276a2b | n/a |
| Vehicles on land (jeeps, tanks) | OK | OK (Humvee, M1A2, M3A3, T90, BMD; tank gun, gunner seat) | d3ea790; `bc_bots.cpp:2845` | n/a |
| Boats / landing craft | OK (LCVP, Daihatsu) | N/A (BC has a Pbl patrol boat, bots do not use it: `Drivable` omits it) | `bc_bots.cpp:2845` | small-medium |
| Planes | OK (fly, hold height, circle flag; no bombing/dogfight) | N/A (BC has none) | `brain.cpp:399+` | N/A |
| Helicopters (AH60) | N/A (BF43 has none) | MISSING (player can fly it; bots do not: `AH60` not in `Drivable`) | `bfbc1_game.cpp:907` | large (flight controller like `brain.cpp` plane code) |
| Vehicle nav meshes (`.vnav`, `.hf`) | MISSING (straight lines + sea/terrain heights) | OK (BC tooling; "steering not solved" in f9b8cff) | `make_navs.py` makes `.vnav/.hf` for BC only | medium: BF43 reuse of `bots/nav.cpp` vehicle mesh |
| Medkit / gadgets / repair / revive behaviour | N/A (no gadgets in BF43) | MISSING (BC has medic/engineer gadgets; bots only fight and use grenades) | `bc_bots.cpp` kit_class | medium-large |
| Nav mesh generation (`tools/navgen`, `make_navs.py`, auto-generate on first host) | OK | OK | `nav_autogen.cpp`; 4b0f69b; `BF_NO_NAV_AUTOGEN` | n/a |
| Learned routes / blocked polygons | OK | OK | `<user>/nav/<map>.blocked`; BC also `nav/<map>.txt` trails | n/a |
| Learning from matches (heatmaps, `bot_knowledge`) | OK (merge with `tools/bot_knowledge.py`) | OK (5960706; no shipped data, `data/bot_knowledge/README.md` only) | `bot_knowledge.cpp` shared | small: ship merged BC knowledge |
| Self-play tuning (`bot_tuning`, `.tune`) | OK | OK | ad640a6 / 5960706 | n/a |
| LLM advisor (`--bot-llm`) | OK | OK | `bot_llm.h:7`, README | n/a |
| Training runs (`--train`) + replay HTML | OK | OK (crash fixed a2598d0; rivers/lakes on replay map) | launcher SERVER page | n/a |
| Planes bombing / dogfight | MISSING | N/A | `docs/DEVELOPERS.md` | large |

## 6. Spectating, replay

| Feature | BF43 | BC | Notes |
|---|---|---|---|
| Free camera (F3) | MISSING | MISSING | not in tree |
| In-game replay recording/viewer | MISSING | MISSING | not in tree |
| Pad input record/replay for testing (`BF_RECORD`/`BF_REPLAY`) | OK | OK | `xam.cpp:711`, `bfbc1_game.cpp:345` (developer tool) |
| Bot training replay (HTML in browser) | OK | OK | launcher OPEN REPLAY; `bot_players.cpp`, `bc_bots.cpp` |

## 7. Audio

| Feature | BF43 | BC | Evidence | Port effort |
|---|---|---|---|---|
| XMA decode (FFmpeg) + XAudio2 output | OK | OK | shared `audio.cpp` | n/a |
| Linux sound (SDL3) | OK | UNV | 912a429 (bf1943), BC Linux commit abd13b0 | small: verify |
| Master volume | OK | OK | shared row | n/a |
| Music / voice volume rows | OK | OK | `game_patches.cpp:225` / `bfbc1_game.cpp:652` (sound system `*82C7C034 +740/+744/+749`) | n/a |
| Mute / lower frame rate in background | OK | OK | shared rows `background_mute`, `background_fps` | n/a |
| Binaural / HRTF | MISSING | MISSING | none | medium (post-mix in `audio.cpp`) |

## 8. Quality of life, install, platforms

| Feature | BF43 | BC | Evidence | Port effort |
|---|---|---|---|---|
| FPS overlay (OFF/FPS/FULL, F2) | OK | OK | shared `fps_overlay.cpp` | n/a |
| PC Settings page (F10; GRAPHICS, EFFECTS, CONTROLS, KEYS, SOUND, PROFILE) | OK | OK (via patched OPTIONS row `CHANGE STORAGE DEVICE -> PC SETTINGS`, F10) | `bfbc1_game.cpp:119-153,448`; BC look: `SetBadCompanyLook()` | n/a |
| Rows that do nothing on BC | - | Plane mouse rows (no planes), `explosion_smoke` partially (hook exists, `bfbc1_game.cpp:639`), BF43-sea effects (`reflect`, `water_*`) on BC's rivers/lakes | shared `Build()` has no per-game filter | small: hide rows per game in `pc_settings.cpp Build()` using a game tag (`g_tag`/`CustomMenus`) |
| New WW2 main menu, pause menu (RESUME/SETTINGS/SUICIDE/QUIT), PRESS ANY KEY title | OK | N/A (BC keeps the game's menus + PC SETTINGS row + own Multiplayer menu) | `CustomMenus()` false in BC | N/A / large to replace BC's front end |
| Skip intro | OK | OK | `bfbc1_game.cpp:73`, row `skip_intro` | n/a |
| Launcher (Electron, per-game tabs, resolution, upscaling, preset, FPS, window, display, HDR, blood, HD textures, shadow detail, EFFECTS card, name, SERVER page, auto-update) | OK | OK (one launcher, BC tab; resolution defaults to 1x) | `launcher/main.js`, `renderer.js:87` | n/a |
| First-run setup/install from the player's files | OK (STFS package or `default.xex`) | OK (`.iso` via `xiso_extract.py` or `default.xex`) | `launcher/setup.js`, `tools/xiso_extract.py` | n/a |
| Build on the player's PC (clang, VS, Vulkan SDK via winget) | OK | OK | launcher setup | n/a |
| Linux: dedicated server | OK | OK | `game/linux/CMakeLists.txt` in both; 619d491, abd13b0 | n/a |
| Linux: client (Vulkan) | OK | UNV ("Bad Company on Linux", `bc_mp_menu_posix.cpp`) | `build_linux.sh --bfbc1`, AppImage launcher 9f0c36f | small: verify |
| macOS | MISSING (texpack tool only) | MISSING | cb014b7 | large (MoltenVK, memory model) |
| Windows only DirectX-free | OK | OK | abd13b0 | n/a |
| Texture pack tool on Windows/Linux/macOS | OK | OK | `tools/texpack`, `texpack.py make` | n/a |
| Menu button icons as PC key caps | OK | OK | `pc_ui.cpp` | n/a |
| Test automation (`BF_AUTO_MP`) | N/A | OK | `bfbc1_game.cpp:259` | N/A |

## 9. What BC has that BF43 does NOT (reverse direction)

1. **Campaign / single player** (about 100 fps at 1x): BF43 has only the tutorial level. Content difference, not a port gap.
2. **Vehicle navigation meshes** (`.vnav`, `.hf`; `tools/make_navs.py` BC branch; `bc_bots.cpp` vehicle mesh): BF43's jeeps, tanks, boats steer without them.
3. **Learned player trails** (`nav/<map>.txt`, `bc_bots.cpp:821`) on top of the shared blocked-polygon file.
4. **Test automation**: `BF_AUTO_MP` (menu stepping from a file), `BF_BOT_HOST_PILOT`, `BF_BOT_PILOT_SHOOT`; BF43's new menus have no equivalent.
5. **Disc-image install** (`.iso` via `tools/xiso_extract.py`); BF43 takes an STFS package.
6. **Native-look menus**: BC's settings page and Multiplayer menu match the game's own menus (`SetBadCompanyLook`, `bc_mp_menu.cpp`); BF43 replaces the originals with new WW2 menus.
7. **Eight maps** with real names and the game's descriptions in the host menu (BF43 has four).
8. **Game-specific objectives for bots**: Gold Rush crates (arm, defuse, guard).

BC has nothing renderer-side that BF43 lacks; all effects are shared code.

## 10. Highest-value gaps

### In BC (relative to BF43), ranked
1. Verify scene effects + ray-traced shadows on BC levels (README: RT stalls the opening cutscene); needs BC shader-pattern table in `scene_fx_rt.cc`. Largest visual gap.
2. Vehicle guns/turrets aimed by mouse (`g_vehicle_mouse_stick` null in BC).
3. BC key-binding action list (gadget, seat change, BC-specific buttons) in `pc_settings.cpp` `kActions`.
4. Host menu: bots count, server name, LIST ON INTERNET in `bc_mp_menu.cpp` (BF43's host page has bots and publish).
5. `start_server.bat/.sh` for BC (only the launcher SERVER page starts a BC server).
6. Per-game filtering of rows in `pc_settings.cpp` (plane sens, BF43 sea effects).
7. Blood: killing-blow impulse offsets so explosion dismemberment works (`g_blood_*` layouts have 0).
8. HD texture pack: build and verify, update the stale README claim; verify FSR 3 / DLSS temporal inputs on BC.
9. Bots: helicopters (AH60), patrol boats, gadget use (medic/engineer).
10. Ship merged bot knowledge for BC (`data/bot_knowledge`), as BF43 does.

### In BF43 (relative to BC), ranked
1. Make Host/Join Game in the new menus work and make README and DEVELOPERS agree (README says they are broken).
2. Vehicle nav meshes for jeeps, tanks and boats (`.vnav`/`.hf` tooling exists, BC's steering is also unsolved).
3. Unattended test automation for the new menus (BC has `BF_AUTO_MP`).
4. Learned player trails (`nav/<map>.txt`) as BC records them.
5. Shipped merged bot knowledge per map is already there; nothing else significant: BF43 has no further gaps against BC (the rest is content or menu design).

### Missing in both (most valuable to add to both)
Frame generation, ultrawide/aspect option, free camera/spectator, hardcore mode, weather/time of day, server password, admin commands (/kick /ban /map /bots), all-talk chat option, adaptive quality, mouse over RDP, plant wind/dust/leaves, binaural audio, macOS build.
