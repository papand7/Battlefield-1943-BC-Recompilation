# Bots flying planes (Coral Sea, Air Superiority)

Findings from running `--dedicated --host-map Coral_sea --bots N` (host side, no window) and
reading the server's memory. Test notes, not a spec. (Use `--port-offset 20` when another copy
of the game is running: two servers on UDP 1003 made runs crash now and then.)

## Getting into a plane

* Coral Sea has two carriers (US near -650,880; Japan near 650,-900). Each carrier deck has
  4 Corsair (US) / 4 Zero (Japan) and Bofors guns, deck at y about 77 (sea about 60). A bot
  spawns on its carrier's deck within a few metres of the planes.
* Entering works as for a landing craft (the brain walks to the plane, sweeps the aim over its
  entry and pulses B = ChangeVehicle + Interact, `Ride`/`kBoard`). The seat flag (soldier
  +0x34C, high byte 0 = seated) turns on, the soldier then sits at the plane's place. The
  seat is the pilot's (throttle and sticks work).
* A flying plane's `ServerVehicleEntity` position (`Vehicle::pos`) is stale (where it was
  made); the seated soldier's position is the plane's. Enemy planes show as enemy soldiers
  with `vehicle != 0` at a height.
* On Coral Sea every bot is a pilot (`World::nav_path` contains "Coral"; elsewhere one bot in
  four takes a plane within 50 m). Plane lost -> pilot dies -> normal spawn screen -> new plane
  on the deck (seen repeatedly, 18 boardings in 5 minutes with 6 bots).

## Controls (analog actions at +8 + 4 i of the input record, bits at +78)

`bot_players.cpp` maps the brain's pad when seated. Measured with `BF_PLANE_ACT_SCAN` (every
action 1..9 at +1.0 for 3 s, plane flying free) and `BF_PLANE_PROBE_RX`:

| action | what +1.0 does | pad |
|---|---|---|
| 0 Throttle | engine; 1.0 takes off after a deck run of about 8 s (the engine holds about 40-60 m/s; below about 30 m/s it mushes) | LT |
| 4 Yaw (rudder) | turns **left** about 9-10 deg/s at full, a small bank (about -10 deg) follows by itself, no runaway | -rx |
| 5 Pitch | a pitch **rate**, +1.0 = nose **down** at about 0.3 rad/s; so `ry < 0` pulls up | ry |
| 6 Roll | a roll rate, about 1 rad/s at 0.6; +1.0 rolls **left**. The bank stays when let go. A bank alone hardly turns the plane (60 deg of bank: 2-4 deg/s); a bank with a pull does (about 10 deg/s) but climbs and is easy to run away into inverted flight | -rx (off by default now) |
| 7 Fire | guns | RT |
| 1 Strafe, 2 Brake, 3 HandBrake | nothing visible in the air | lx, LB |

Range -1..1 (clamped in `bot_players.cpp`). `BF_PLANE_YAW_MODE` (bot_players.cpp): 0 yaw and
roll both from rx (the old mapping), 1 roll only, 2 yaw only (**default**: the rudder turns
the plane), 3 roll from rx and yaw from the left stick X.

Note: with yaw+roll both driven (mode 0), some flights (mostly Corsairs) showed no response to
pitch/roll/yaw at all for the whole flight (attitude frozen, flying straight until killed at the
edge); not understood. With the rudder-only mapping all pilots answered in the last runs.

## The plane's orientation (new in the brain's World)

The vehicle entity's `+0xF0` is its physics body; there a 3x3 matrix (three float rows, 16
bytes apart) sits at `+0x1F0`: row 0 = right, row 1 = up, row 2 = forward (left-handed:
forward = (sin yaw, cos yaw) in x/z, right = up x forward) and the position at `+0x220`
(for jeeps/boats the position is at `+0xF0`; planes keep nothing there). `bots.cpp` finds the
plane as the one whose body position equals the soldier's (the vehicle found at boarding can
be a neighbour on the deck) and passes the rows as `BotIO::att_ok / att[9]` (API version 9).
Bank = `atan2(-right.y, up.y)` (+ = right wing down), pitch = `asin(forward.y)`. In a few
flights the body was not readable (`att_ok` false: wrong object or different layout); the brain
then flies from the track of positions.

## The brain (runtime/bots/brain.cpp, `Fly`)

* Takeoff: throttle, a gentle climb once above 24 m/s (the climb angle is limited by speed).
* Height: the climb angle of the velocity is steered to what the height wanted needs (cruise
  deck + 220..420 m, hunting: the target's height, never below deck + 110 m or 100 m over the
  terrain `World::ground` finds along the track 6 s ahead); the stick is a pitch rate, so the
  pitch angle (flight path + angle of attack) is what is steered.
* Route: patrol waypoints (random, within 600 m of the middle of the flags), the heading error
  through the rudder (P + D on the heading rate); beyond 1150 m from the middle it turns back
  (planes were not killed at 1300 m; the limit was not measured).
* Combat: the nearest enemy plane in the air within 1800 m (a pilot already hunted counts 150 m
  nearer), aiming ahead by distance/300 m/s of its tracked velocity; fire (RT) when the nose is
  within `0.04 + 1.5/d` rad of that point and d < 700 m.
* Known weakness: both planes turn at the same rate, so a pair chasing each other circles at
  about 280 m without ever getting the nose on (Lufbery); firing happened only in the first
  approach and in head-on passes. A harder turn needs the roll + pull mix (`BF_PLANE_TURN=bank`
  is the old banking controller, unfinished).

Env for tests: `BF_PLANE_TRACE=1` (a line per second and bot), `BF_PLANE_LOGDT`,
`BF_PLANE_RAW=<file>`, `BF_PLANE_NOCTRL` (+ `BF_PLANE_PROBE_RX`/`RY`, `BF_PLANE_ACT_SCAN`),
`BF_PLANE_ATTLOG`, `BF_PLANE_DUMP=<file>` (the plane's memory), `BF_PLANE_KRUD`, `KRUDD`,
`KPITCH`, `ROLL_SIGN`, `PITCH_SIGN`, `BANK_MAX`, `KBANK`, `PULL`, `BF_PLANE_YAW_MODE`.
