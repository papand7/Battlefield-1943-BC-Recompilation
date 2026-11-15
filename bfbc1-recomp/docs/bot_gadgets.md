# Bad Company: what the bots can do with gadgets

Findings from running Valley Run (mp_harbour_day1) as a dedicated server with
bots (`--dedicated --maps mp_harbour_day1 --bots N`), the test options
`BF_BOT_STRING_SCAN`, `BF_BOT_WEAPON_DUMP`, `BF_BOT_KIT_DUMP`,
`BF_BOT_GRENADE_TEST`, and `BF_BOT_CLASS`. (The trail recorder thread's start-up
crash, before guest memory existed, is fixed: it waits for `GuestMemoryReady()`, and
`IsCommitted` is false until then.)

## Which kit carries what

From the loadout texts and asset names in guest memory (`BF_BOT_STRING_SCAN=medic,ammo,...`):

| Class | Loadout text |
|---|---|
| Assault | HAND GRENADE, AUTO INJECTOR (UNLOCK), MAIN WEAPON, GRENADE LAUNCHER |
| Support | POWER TOOL, MORTAR STRIKE (UNLOCK), MAIN WEAPON, **MEDKIT** |
| Demolition | HAND GRENADE, ANTI TANK MINE (UNLOCK), MAIN WEAPON, ROCKET LAUNCHER |
| Recon | HAND GUN, MOTION SENSOR, MAIN WEAPON, LASER DESIGNATOR (UNLOCK) |
| Specialist | HAND GRENADE, TRACER GUN, MAIN WEAPON, REMOTE EXPLOSIVES (UNLOCK) |

* **Medkit** (Support): `Objects/Weapons/Handheld/MedKit`, thrown as
  `Objects/Weapons/Projectiles/MedKit` that becomes a `ServerMedicBagEntity`
  (vtable 82018758; client twin `ClientMedicBagEntity`); data `MedicBagEntityData`
  (820747FC), firing effects `ServerMedicBagFiringEffects` / `ServerMedkitFiringEffects`.
  Control text "THROW MEDKIT". The medkit heals the soldiers near the bag.
* **Auto injector** (Assault, unlocked): a self-heal ("Next time, use the Auto Injector
  to regain lost health", "50 heals with auto injector"), also a melee weapon.
  Not used by the bots yet.
* **No ammo gadget.** There is no ammo box item in any kit: ammo comes from
  `ServerAmmoCrateEntity` crates that stand on the map ("STAND CLOSE TO RESUPPLY",
  `HUD_INTERACTION_RESUPPLY`). A bot can't "drop an ammo box", so nothing was built for it.
  (The ServerAmmoCrateEntity objects seen in the first 0x6000 bytes after a soldier are
  just heap neighbours: the map's own crates.)
* **No revive/defibrillator.** No string for defibrillator or revive exists in the
  game's data (the medic "need a medic" voice lines only exist). A soldier is simply
  dead until he respawns. Stage 3 does not apply to Bad Company 1.

## The input

Input words (see bc_bots.cpp): digital actions are bits of +84 (and +80 for 32 and up).
Known: 13 jump, 16 crouch (toggle), 21 reload, 23 sprint; fire is the analog +28 with the
+3C marker. With `BF_BOT_CLASS=Support BF_BOT_GRENADE_TEST=1` (bot 0 stands, one candidate
bit is held 0.3 s per 5 s phase, then the fire button for 0.5 s at 1.3 s, and the
live objects of the Server* classes are counted), a `ServerMedicBagEntity` first appeared
in the phase of **bit 18 of +84**, and in no phase before it (bits 29-36, 10). So bit 18
takes the gadget out ("TOGGLE GADGET"), and the usual fire button throws it. Pressing
bit 18 again is assumed to take the gun back (not verified separately: the bots' hits are
applied by the server hook, so they don't depend on the weapon held).

## What "a teammate needs health" is read from

A soldier's health is the float at +3C (0-100), position +200/+204/+208, team +48. Nothing
else is needed: a hurt teammate is one with +3C below the limit.

## What the bots do (bc_bots.cpp, "The Support kit's gadget is a medkit")

A Support bot on foot with no enemy about, not within 25 s of its last throw, picks the
teammate with the lowest health under 70 within 9 m (or itself under 50) and stands still,
turns to him (yaw toward him, pitch 0.2 to 0.6, 0.9 for its own feet), presses bit 18 (0.3 s),
fires at 1.3 s for 0.5 s, presses bit 18 again at 2.5 s. Log lines: `bot N: medkit for ...`,
`bot N: medkit: K bag(s) in the world`, `bot N: medkit thrown at X: its health A -> B`.
`BF_BOT_NO_GADGETS=1` turns it off, `BF_BOT_GADGET_HEALTH=101` makes any teammate count
(testing), `BF_BOT_CLASS=Support` makes every bot a Support.

Confirmed in a running game: bags appear in the world (one per throw: 2 bags after 2 bots
threw), and bot 2 at health 78 threw one at itself and was at 88 four seconds later.
Bags don't expire quickly (8 alive after 8 throws), and a second throw at the same place
25 s later sometimes made no new bag: the medkit seems to be limited (a cooldown or a
few uses), not studied further.

## The Assault's auto injector: not available to the bots (blocked)

Tried with `BF_BOT_INJECT_SCAN=1` (bot 0 alone, so no enemy; every 7 s it is hurt down to
40 through the server's damage call 822D1AA0 with no attacker, then one candidate button of
+84/+80 is tapped at 0.5 s, fire held 1.8-2.6 s, the button again at 4 s, and its health
compared at 6.8 s; bit -1 presses nothing):

* Health does not come back by itself (the control phases stayed at 40 for minutes).
* **Support** (control): the bit 18 phase healed 40 -> 55 (its medkit), no other phase did.
  So the method sees a heal.
* **Assault**: no phase healed (bits 18, 19, 20, 17, 29-36, 10, 22, 24-28, 11, 12, 14), also
  not with the injector's SoldierWeaponData forced as the spawn's weapon choice (+1D8,
  `BF_BOT_WEAPON_PICK=AutoInjector`).

The loadout text says why: "AUTO INJECTOR **(UNLOCK)**", the kit's unlock slot, still locked
(like the Support's MORTAR STRIKE, the Recon's LASER DESIGNATOR...). Unlocks came from the
player's online stats; nobody here has them, and the bots get the kit's defaults. The weapon
data of the unlockables has a `PersistentWeapon` at +100 (a stats record: a short name such as
'm16', 'pwtl'); no unlock flag was found in it. Giving the bots the injector needs the game's
unlock check found first (where a soldier's weapons are built from its kit). Until then no
Assault behaviour was added (bit 18 on an Assault would take out its grenade launcher).

Test options kept: `BF_BOT_INJECT_SCAN`, `BF_BOT_WEAPON_PICK=<anim tree name part>` (Assault
bots' +1D8), `BF_BOT_WEAPON_REFS` (the weapon datas' PersistentWeapon words and who points at
the injector's and medkit's data), and with `BF_BC_KITS` every SoldierWeaponData's assets.

## BF1943

Its kits (Infantry, Rifleman, Scout) carry no medkit, ammo box or revive item; only a
Wrench (repair) and repair platforms exist (as found earlier from asset names). Nothing to
do for the bots there beyond a repair with the wrench.
