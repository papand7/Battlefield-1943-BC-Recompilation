# Local aim: where the right stick reaches the soldier's aim

Status: partial. The stick path is mapped down to the move record, and the
aim component's writer is known to copy the record. The guest function that
turns the stick into the record's yaw/pitch (the integration) is NOT pinned
yet. A live write-watch run was started but not finished (see "Next step").

## Pad read (GAME_PATCH 821C7FD8, runtime/game_patches.cpp ~l.729)

- Called through pad vtable +28 with r3 = pad, r4 = input; returns the axis in f1.
- Inputs 4/5 = right stick right/left (input & 1 flips sign), 6/7 = up/down.
- Pad +8 is the user index + 1; the mouse look (`MouseLookAxis(input >= 6)`) only
  applies to pad user 1.
- The patch takes `max(stick, mouse)` only when the mouse value is positive. The
  stick value itself is the game's own (`sub_821C7FD8`, not analysed here, so its
  deadzone and scale are unknown).

## Data path for the local player (from existing runtime code and logs)

1. `823B2C80` (runtime/bot_players.cpp ~l.1192): receives a real player's move
   message. r4 = message, vtable 8200E490; the input record is at +10 (vtable
   8200E48C). Record +8 = forward, +88 = minus the yaw, +8C = pitch.
2. `822ED8D8` (bot_players.cpp l.448): a player's received input copied into its
   soldier's input state (r3 vtable 82012320, +4 = soldier).
3. The server soldier (ServerSoldierEntity, bots.cpp l.13): aim component at
   soldier +130, yaw +134, pitch +138 (radians).

Evidence that the aim follows the record each frame. From the existing
`build/bottest/aim.log` (BF_BOT_AIMLOG, one line per second):

```
record 88:0.39 8C:0.00  ->  aim yaw -0.39 pitch 0.00   (next sample)
record 88:0.13 8C:0.37  ->  aim yaw -0.14 pitch 0.37
record 88:5.78 8C:0.17  ->  aim yaw -5.80 pitch 0.17
```

So aim yaw = -(record +88) and aim pitch = record +8C, copied with at most one
sample of lag. The aim writer itself only copies; the stick integration
(stick * rate * dt added to the record's heading and pitch) happens in the
client code that fills the record from the pad. That function is the one still
to be found.

## Can mouse look bypass it?

Yes, in two ways, both likely workable:

- In the host process (local and LAN games run the server in-process), write
  aim +134 / +138 directly each frame, as BF_BOT_YAW_SPIN already does with
  `St32(aim + 0x134, ...)`. This skips the stick entirely. Remember yaw is
  stored with the opposite sign to the record's +88.
- Or write the record's +88 / +8C from the mouse before the move message is
  sent. This keeps the network and server path unchanged.

Neither has been run against a local player yet.

## Next step: pin the writer

A temporary env-gated watch was added in the worktree (not committed; reverted):

```cpp
// in GAME_PATCH(823B2C80), after the BF_BOT_AIMLOG block
if (getenv("BF_AIM_WATCH") && Ld32(ctx.r4.u32) == 0x8200E490) {
  static bool armed = false;
  uint32_t so = Ld32(ctx.r3.u32 + 0x118);
  if (!armed && so && IsCommitted(so + 0x400) && Ld32(so) == kSoldier && Ld32(so + 0x130)) {
    armed = true;
    StartWriteWatch(Ld32(so + 0x130) + 0x134, 8, 0);  // logs call stacks of writes
  }
}
```

Run (worktree build, `build/aimtrace`): `bf1943.exe --host --host-map Wake_island_s
--port-offset 71 --game <main>\original\v106 --user <dir>`. Hosting starts only
after the title's startPressed callback (game_patches.cpp l.662), so the press
script needs Start early (e.g. `--press "10:start,20:a,30:a,60:rx+*3"`). The
write log then shows the function (SymbolStack, up to 6 frames) that writes
+134 / +138 while the stick is held.

Run status: one run (PID 11768) was still going at the time of writing. Its
graceful close was blocked by a Le Office hook, so it was not stopped. The
first run failed on the game root path (unquoted spaces) and did not host.
