# Controllers

Both games (Battlefield 1943 and Bad Company) share one pad layer, `bf1943-recomp/runtime/pad.cpp`. The games read the pad as an Xbox 360 one (XInput's `XINPUT_STATE`); every other pad is translated into that.

## What is supported

| Platform | Xbox 360 / One / Series | DualShock 3/4, DualSense, Switch Pro, Steam Deck, generic pads |
| --- | --- | --- |
| Windows | XInput (as before) | SDL3 gamepad API, into the player slots XInput leaves free |
| Linux, macOS | SDL3 | SDL3 |

- Pads of the Xbox family on Windows are served by XInput only; SDL ignores them, so a pad is never counted twice. (A virtual XInput pad such as Steam Input's or DS4Windows' counts as an Xbox pad; the real device behind it, if still visible to SDL, becomes a second user, which the game does not use.)
- Hot-plug works while the game runs. Pads are also read when the window is in the background (the game decides what is in front).
- Rumble: XInput's on Xbox pads, SDL's on the rest (DualShock 4, DualSense, Switch Pro rumble through their own drivers).
- SDL's controller database maps most pads. A pad it has no mapping for is not seen; add one with the `SDL_GAMECONTROLLERCONFIG` environment variable (a line from the SDL_GameControllerDB project).

## Mapping (SDL gamepad to the 360 pad the game expects)

| Game sees | Xbox | PlayStation | Switch Pro | Steam Deck |
| --- | --- | --- | --- | --- |
| A, B, X, Y | A, B, X, Y | Cross, Circle, Square, Triangle | position-based: bottom, right, left, top button | A, B, X, Y |
| LB, RB | LB, RB | L1, R1 | L, R | L1, R1 |
| LT, RT (analog) | LT, RT | L2, R2 | ZL, ZR (digital: full pull) | L2, R2 |
| Back, Start | View, Menu | Create / Share, Options | -, + | View, Menu |
| Stick clicks | LS, RS | L3, R3 | stick clicks | L3, R3 |
| D-pad | D-pad | D-pad | D-pad | D-pad |

The mapping is by position, as the games were made for an Xbox layout: on a Switch pad the bottom button (Nintendo B) is the game's A. The PS button, touchpad and paddles are not used. SDL pads' sticks read centred inside a small inner dead zone (about 9 percent) to hide noise; the game applies its own dead zone on top, as for an Xbox pad. Triggers are 0 to 255.

## Button prompts

PC SETTINGS, BUTTON PROMPTS: AUTO (default) shows the keyboard's keys with no pad, and with one the buttons of the pad that was used last: Xbox 360 or Series, PlayStation 3 (coloured symbols, Start/Select) or DualShock 4 / DualSense (grey symbols, Options/Create), Steam Deck (L1/R1, Menu/View). A Switch pad shows the Series set, an unknown pad the Xbox 360 set. You can force a set (KEYBOARD, XBOX 360, XBOX SERIES, PLAYSTATION 3, PLAYSTATION 5, STEAM DECK).

The glyphs are drawn by the runtime (`pc_ui.cpp`, `DrawPadIcon`) into the game's icon atlases when the game loads its textures, so a pad plugged in later shows its buttons from the next load (a new level, the front end); the new menus (`pc_settings.cpp`, `bc_mp_menu.cpp`) follow at once (PlayStation: X, O, L1, R1, in the PlayStation colours in Bad Company's menu).

## The new menus with a pad alone

| Input | Does |
| --- | --- |
| D-pad, left stick | Move; held repeats |
| A (Cross) | Select; open a page; change a value; start typing in a text row |
| B (Circle), Back | Back out; Start also resumes the pause menu |
| LB / RB | Previous / next page (Settings tabs) |
| Left / Right on a value | Change it |

Text rows (SERVER NAME, PASSWORD, the join ADDRESS): press A on the row, then Up and Down step the last letter through a-z, A-Z, 0-9, space - _ . (an address: 0-9 . :), Right adds a letter (a copy of the last, then Up/Down moves it), Left removes the last, Y switches its case, A or B is done. This is a deliberate fallback rather than an on-screen keyboard (no new rendering code, same on every platform); with a keyboard attached typing works as before. Key bindings (Settings, Keys) can only be bound from a keyboard: on a pad, A starts the capture and B cancels it. Joining a game found in the list needs no typing, except a password.

## How to test

No game run is needed for the first two; the rest needs real hardware.

1. Build: `bf1943-recomp\build\bottest\build.bat` style build of `game`, and `bfbc1-recomp\game\build.bat`. SDL3 3.2.20 is downloaded at the first configure (CMake `FetchContent`, `game/sdl_pads.cmake`).
2. Run with a pad connected and look in the log for `input: controller <name> (<prompts>)` and, on AUTO, `ui: button prompts AUTO -> ...`.
3. Menus: move with D-pad and stick, A, B, bumpers; HOST GAME, SERVER NAME: A, Up/Down, Right, A.
4. `BF_VEH_DEBUG=1` logs every pad change (`pad: buttons ...`) to check the mapping.
5. Unplug and replug during the main menu; the log shows `controller ... removed` and the new one.

Untested without hardware: DualShock 3 over USB (needs a driver SDL can use on Windows: Steam or a libusb-style driver), Switch Pro over Bluetooth, Steam Deck in desktop mode, and every platform's rumble.
