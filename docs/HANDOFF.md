# Halo Trial Android — handoff

**Read this file. You should not need anything else to start.**
`docs/JOURNAL.md` is the session-by-session history — go there only when you
want the *why* behind something, and search it by symptom.

**Date of this revision:** 2026-09-23, morning (bots drive vehicles; fast
rounds no longer pass through people)
**Repo:** `/home/commander/projects/halo-trial-android`
**Branch / HEAD:** `fp-animated-guns`, tracking `origin/main` on GitHub.
Last published build: bots-drive build (see `git log -1`), verify.sh
75/75, on the sideload page, archived in `apks/INDEX` on the backup drive,
pushed to GitHub `main`. No device report on it, nor on the playtest-fixes
build before it. Network protocol is still **v4**.

**Start of next session, in order:**
1. `ls -lt scratch/uploads/ | head` -- screenshots from this build? The
   owner's reports decide what comes first.
2. Otherwise NEXT ENGINEERING OBJECTIVES, top down.
3. After any published build: it is already backed up; push to GitHub
   (the owner wants it kept current; check for Trial data first).

---

## What this is

A clean-room native ARM64 Android engine that reads the owner's own legally
obtained **Halo: Combat Evolved Trial** data and plays Blood Gulch. C, Vulkan,
AAudio, no engine dependencies. Every number it can get from a tag, it gets
from a tag.

**Hard constraints — do not break these:**

- **No Halo assets, executables or DLLs are ever committed.** The owner
  supplies their own Trial copy. No DRM is involved or circumvented.
- **The shareable APK carries no Trial data** (`verify.sh` checks it). Since
  2026-09-22 the owner publishes a PERSONAL build with
  `scripts/publish_apk.sh --with-assets`, which puts their own maps
  (`bloodgulch`, `bitmaps`, `sounds`, `ui`) into the APK uncompressed; native
  code maps them straight out of it. That APK is for the owner's own device
  only and must never be given to anyone else -- a LAN friend installs the
  plain build and picks their own maps.
- **Never commit Trial `.map` files.** They live outside the repo at
  `/home/commander/halo-trial-data/extract/maps/`.
- Project code is **GPLv3**.
- Git author on this repo is **`Phase2 <schultz0@proton.me>`**.
- **Push after every commit** (the owner's standing instruction since
  2026-09-23), and release each published build's guest APK. Backups go to `origin`
  (https://github.com/madpai/open-halo-project, public), local
  `fp-animated-guns` → remote `main`. On 2026-09-23 the history was
  rewritten to remove the Trial's decoded title theme (`in_p0-6.wav`,
  committed by mistake in the main menu commit); `*.wav`/`*.ogg` are now
  ignored. Any hash from before that date in the journal is stale.

---

## The loop

This working rhythm is the owner's, it is good, and it should be kept.

### 1. Change something, then verify

```
cd /home/commander/projects/halo-trial-android
HTA_MAP=/home/commander/halo-trial-data/extract/maps/bloodgulch.map scripts/verify.sh
```

The latest gate has 75 checks: host build and tests, real Trial map tests,
synthetic-fixture CLI, offscreen rendering, two desktop Blood Gulch clients,
Android build, and the APK contents and asset boundary.

The host NVIDIA Vulkan driver currently fails `vkCreateInstance`. The saved
Mesa lavapipe under `scratch/lvp/` runs the render checks and the desktop
network smoke test. Use:

```
VK_ICD_FILENAMES=$PWD/scratch/lvp/usr/share/vulkan/icd.d/lvp_icd.json \
  HTA_MAP=/home/commander/halo-trial-data/extract/maps/bloodgulch.map scripts/verify.sh
```

The 2026-09-23 vehicle-paths build passed **75/75** with that command.
The normal shareable build contains no Trial maps; `verify.sh` checks that.

**`verify.sh` must be green before you publish.** If you add a module, add its
test to `CMakeLists.txt` *and* to both halves of `verify.sh`.

Note: `src/platform/platform_android.c` is **only** compiled by the Android
target. `cmake --build build-host` will not catch a mistake in it — only
`verify.sh` (via `gradle assembleDebug`) will.

### 2. Publish to the sideload server

```
scripts/publish_apk.sh --with-assets --title "what changed" --notes-text "a sentence or two"
```

Builds the owner's map-bundled APK and then cleans the Android app build to
produce a small asset-free guest APK. It copies both to the serve root,
refreshes `SHA256SUMS`, stamps the page with the source revision and build
time, and starts the server if it is not up.
The owner then installs from **http://100.89.1.14:8731** (Tailscale-bound).
Use `--with-assets` for the owner's personal test APK. The launcher icon is
already in `android/app/src/main/res/drawable/` and referenced by the manifest.
The publish script marks builds from uncommitted source as `-dirty`.

- `--notes` takes an HTML fragment; `--notes-text` takes one paragraph.
- `--no-build` publishes the existing build; use a normal build when either
  APK needs to change.
- The page is a committed template at `scripts/sideload/index.html.tmpl` —
  edit **that**, not the generated `scratch/serve/index.html`.
- The serve root is `scratch/serve/` (gitignored, so maps and APKs never enter
  git). It must **not** live in a session scratchpad under `/tmp`: that
  directory dies with the session and the page then advertises a build from
  hours earlier while claiming to be current. If `publish_apk.sh` warns that a
  server is running with a different root, kill it and rerun.

**Write real release notes.** The owner reads them, and they are how a change
gets tested deliberately rather than stumbled into.

**Local backup.** Every publish ends by running `scripts/backup_local.sh`,
which mirrors the whole project folder, a git bundle of every branch, the
owner's Trial data, and that build's personal and guest APKs to
`/mnt/media/backups/halo-trial-android/` (the 8 TB drive; `apks/INDEX`
lists every build, `apks/latest` is the newest). It skips with a warning if
the drive is not mounted. The owner wants this copy current at all times --
run it by hand after work that is not published. It holds Trial data: it
is private and never goes anywhere else.

**GitHub.** `origin` is https://github.com/madpai/open-halo-project
(public); local `fp-animated-guns` tracks its `main`. `gh` is installed in
`~/.local/bin` and logged in as madpai (Git uses it for HTTPS). Releases
carry the **guest APK only** -- never the personal one:
`gh release create vX.Y.Z scratch/serve/halo-trial-guest.apk --target main`,
then check that `unzip -Z1` of the APK lists no `assets/maps/`. v0.2.0 was
the first (Team Slayer, CTF, every vehicle). Before any push, check that
`git rev-list --all --objects | grep -iE '\.(map|wav|ogg)$'` prints nothing.

### 3. The owner tests on device and sends screenshots

Screenshots uploaded from the phone land in `scratch/serve/uploads/` and are
mirrored to `scratch/uploads/`. List them newest-first:

```
ls -lt scratch/uploads/ | head
```

Then read them directly — they carry the HUD readout (position, ground/air,
**fps**) which is often the whole diagnosis. The 120 fps idle reading is what
proved the particle-lag was CPU raycasts and not fill.

### 4. Say what to look at next

End a session by naming the **current testing objective** — what specifically
to try on device and what a failure would look like. Keep it in this file, in
the section below, and update it every time.

---

## CURRENT TESTING OBJECTIVE

> **Open Asset Lab host integration (2026-09-23):** no new Android APK was
> published for this work. Open Halo now has an external `.oalmap` host loader
> and `open-halo-map-test`; its existing Trial game path is unchanged. The
> companion Asset Lab service is at `http://100.89.1.14:8762` on Tailscale.
> On the S24+, first open that page, sign in with the private token from
> `python -m assetlab access` in the Asset Lab repo, submit a registered map,
> disconnect and reconnect, then inspect its staged preview/report. A failure
> is inability to load the page, submit, or see the completed job. This tests
> the remote workflow only; walking in an imported map needs a later Android
> exploration mode. Continue the Trial vehicle-path test below on the current
> APK.


> **Vehicle-paths + Ghost-strafe build (5d51b76)** at `http://100.89.1.14:8731/`. Host-verified
> (verify.sh 75/75, test_nav 30, test_ride 92); not yet on a phone. The
> bots-drive and playtest-fixes lists below are still unreported too --
> one SINGLEPLAYER TEAM SLAYER game with 7 bots covers most of it.
>
> **New in this build:**
>
> A. **Out of the base:** in the first 20 s, the Scorpions drive clear of
>    the posts they park among (they used to sit turning into one all
>    match), and Warthogs and Ghosts leaving a base steer round the cars
>    still parked there instead of ramming them. Failure: a vehicle
>    grinding against a post or another car for more than ~3 s.
> B. **Open ground:** driven vehicles keep a car's width off walls and
>    rocks and go round a base, not along its wall. They should rarely
>    rock back and forth now. Report any that do, with the HUD position.
> C. **Round, not through:** an enemy behind a rock or a base corner is
>    driven round to; one in the open is still driven straight at.
> D. **A stuck car is left alone** until it goes home (60 s empty):
>    nobody climbs into a Warthog someone just abandoned on a rock.
> E. **Ghost fights** (build 5d51b76, v0.2.2): a bot Ghost circling you
>    near rocks or a base wall strafes away from them, not into them.
>
> **From the bots-drive build (still unreported):**
>
> 1. **Bots take the wheel:** SINGLEPLAYER, TEAM SLAYER, 7 bots. Within a
>    minute bots walk to the Warthogs, Ghosts and Scorpions near them and
>    drive off. Failure: nobody drives, or a bot stands at a door forever
>    (it should give up after 8 s).
> 2. **Crews:** a red bot at a Warthog's wheel waits up to 3 s for a red bot
>    to climb on the gun, then drives with it. Stand near your own team's
>    Warthog: a bot may take the wheel -- climb on the gun and ride.
> 3. **Fighting from them:** a Warthog without a gunner tries to run you
>    over; with a gunner it circles you. A Ghost closes and strafes firing;
>    a Scorpion stops about 25 wu off and shells your FEET. Is the tank too
>    deadly? (tell me the skill setting)
> 4. **Jams:** bots back out when they hit something; after three in a row
>    they get out and walk. Report any bot rocking in place for more than
>    ~5 s, with the HUD position.
> 5. **CTF:** attackers drive toward the enemy base and get out ~10 wu from
>    the flag.
> 0. **Fixed from your report:** die, respawn, get back in a Ghost -- your
>    body under it no longer plays a death animation (it did until you got
>    out). Check the same with the Warthog seats.
> 6. **Your own shots:** tank shells and fuel rods used to fly through a
>    man and burst far behind him. A direct hit now stops in him. Check it
>    feels right from the Scorpion.
>
> **Still unreported, from the playtest-fixes build:**
>
> 1. **Tracers:** fire the rifle and the Warthog chaingun. Expect short
>    yellow streaks on about one round in four -- not solid lines.
> 2. **Explosions:** Scorpion cannon, Banshee fuel rod, rocket, grenade.
>    Each should now visibly blow up (fire, smoke, dirt), and a blast
>    within ~8 wu shakes the camera; firing the cannon rattles your view
>    and kicks the tank back nose-up. Report: too much shake? too little?
> 3. **Aim:** the crosshair turns RED on an enemy inside the gun's autoaim
>    cone (on foot and on vehicle guns), and rounds bend onto him; tank
>    shells and plasma lead a moving target. Is aiming from a vehicle
>    usable now?
> 4. **Hulls:** shoot a Warthog: the driving readout shows HULL %; below a
>    third it sparks; three rockets (or two tank shells and change) blow it
>    up, killing whoever is in it -- the kill is yours. It is back home 20 s
>    later. Blasts shove and can flip vehicles into the air.
> 5. **Momentum:** let go of the stick at speed -- the Warthog rolls on
>    (the brake still stops it); the Ghost and Banshee slide wide in hard
>    turns; bodies squat and roll on their suspension. Report: too floaty?
> 6. **LAN team games:** CREATE GAME now has GAME (Team Slayer / CTF).
>    Needs two phones on the new build (protocol v4 -- an old build cannot
>    join). Check team colours, scores, flags taken/returned/scored with
>    the announcer on the joining phone, and SWAP dropping a carried flag.
> 7. **Bots on guns:** SINGLEPLAYER, TEAM SLAYER, with bots; drive a
>    Warthog near a red bot and stop: it climbs onto the chaingun and
>    shoots at blue. Get out and it gets off.
> 8. **CTF waypoints:** a red and a blue chevron with metres over each flag,
>    pinned to the screen edge when off screen, blinking while away.

## GAME TYPES (done 2026-09-23) — how they work

- **Rules** `src/game/game.c`: `hta_game_set_mode(g, HTA_MODE_*)` before
  adding units (sets `teams` and the mode's default limit). `HTA_TEAM_AUTO`
  puts a unit on the smaller side, red when even. Team spawns use the
  scenario's `team index` (35 red, 37 blue). Team Slayer keeps
  `team_score[]` from kills less suicides and betrayals. `finish_team`
  speaks the Trial's "Your team won/lost", "Game ends in a draw".
- **Flags** come from the scenario's netgame flags (**Scenario +888**, 148
  each; type 0 is a CTF stand, `usage id` the team). The flag is
  `weapons\flag\flag`, added to the roster (last index) though it has no
  trigger; units carry it as `unit.flag` (team of the flag held), which
  `hta_game_held` answers before the carry slots. `flags_update` does take /
  return / capture / reset; `drop_flag` on death, swap, disconnect. Events
  are `HTA_EV_FLAG` with the Trial's own words (text 144-150, 167-169) and
  lines (`red_team_has_the_flag` ...). The cyborg holds it as a rifle
  ("stand rifle f melee" is in the graph).
- **Bots**: `hta_game_ctf_goal` -- carry home, take ours back, chase the
  thief, every third player on a side defends, else attack. A cross-map A*
  from the back of a base ran out of budget, so each stand gets a
  **flow field** (`hta_nav_field`, Dijkstra over reversed links) built at
  match start: 0.04 s for both on the host. Attackers keep walking while
  they shoot (`PUSH_STOP`), carriers weave and jump.
- **Colour**: ShaderModel `change color source` (**+76**) marks surfaces
  that take the owner's colour where the multipurpose map's **blue** is.
  `hta_gfx_dynamic.change/change_color`; it rides to the shader packed as
  `0xRRGGBB+1` in `ambient.w` (the chicago path returns before that code).
- **Cloth**: the flag object's widget (**Object +332**) is a `flag` tag
  (Flag, 96): 16x13 vertices of 0.025 wu between the pole's `flag top` and
  `flag bottom` markers, `flag_red` / `flags_blue` chicago shaders. Built
  once in `view.c` as two submeshes after the pole's; `hta_game_view_flag_parts`
  names the ranges; held-weapon instances carry submesh ranges now.
- **Android**: the menu's GAME row (config[9]); `carried_flag` swaps the
  first-person flag in and the gun back out with its ammo.
- Tests: `test_game` sections `[capture the flag]`, `[team slayer]`,
  `[a bot runs the flag]`, `[bots play capture the flag]`. Tools:
  `htamatch --mode ctf|team`, `htaview --weapon "flag\flag" --fp idle`.

## VEHICLES (done 2026-09-22) — how they work

- **Engine** `src/engine/vehicle.c/.h`: a TYPE per palette entry (seats,
  markers, turret yaw/pitch nodes, trigger markers, model split into rigid
  PARTS by node, collision built once in model space) and a CAR per
  placement. Kinds from `vehi+756`: jeep (the old Warthog physics,
  unchanged), tank (treads pivot at the driver seat's yaw rate), scout
  (Ghost: yaws toward the look, strafes), fighter (Banshee: flies along the
  look; empty it falls and lands), turret (aims only). Rosters from the
  scenario's slayer spawn flags (`HTA_VROSTER_*`, menu option VEHICLES).
- **Collision instances**: `hta_collision.instances` — each car's type grid
  queried through its pose (ground/ray/depenetrate). Replaced the old
  every-frame rebuild of a shared grid (1.5 ms → 0.03 ms per update).
  `extra` still exists. Vehicle physics strips instances (walks the static
  world only; car-car contact is mass-point spheres).
- **Drawing**: `hta_vehicles_parts` → instanced parts of one static mesh per
  type (`HTA_GFX_MAX_INSTANCES` is 512).
- **Game** `src/game/game.c`: units have `vehicle`/`seat`; `in.action` gets
  in/out (host-validated); driver input → `car.ctl`; gunner aims the turret
  and fires the vehicle gun. Vehicle guns are roster entries (`vehicle`,
  `trigger`), one per trigger, loaded by `hta_weapon_load_trigger`, with
  spin-up, magazines, chamber times; rounds converge on the crosshair.
  Rounds slower than 150 wu/s fly as projectiles even without a model
  (`hta_projectiles_equip_any`). Splatter uses `globals\vehicle_collision`.
  Scorpion driver and Banshee pilot are enclosed (no ray hits, blast × the
  vehicle's `rider damage fraction`).
- **Android**: `vehicle_controls` / `vehicle_camera` / `vehicle_transition` /
  `vehicle_status` / `vehicle_sounds` in `platform_android.c`. A Warthog
  driver's look is relative to the hull; everyone else looks in world space.
- **Network v3**: `VEHICLES` snapshot (32 cars, 36 bytes each), `DROPS`,
  CONTROL gains `HTA_NET_ALT` and `action_count`. Clients interpolate the
  host's cars; no client-side prediction yet.
- **Network v4** (2026-09-23): `GAME` packet (62 bytes: mode, limit, team
  scores, winner team, both flags, 32 hulls); entity flag 64 = blue;
  `HTA_NET_FX_WRECK`. Joiners run `hta_game_mirror_rules`.
- **Hulls** (2026-09-23): `hta_game_vgun.hull`; `hta_game_car_at` matches
  a hit point to a car's collision box; `hta_game_hurt_car_jpt` uses the
  round's `jpt!` vs thick metal; `wreck()` kills riders, blasts with the
  Scorpion shell explosion, deactivates the car for
  `HTA_VEHICLE_WRECK_TIME`. `hta_vehicles_push` throws a car (blasts,
  cannon recoil). Tests: `test_ride` [hulls], [autoaim], [a bot on the
  gun]; `test_vehicle` momentum checks; `test_shake`.
- Tests: `test_vehicle` (94), `test_ride` (59: seats, guns, splatter,
  drops, tracker), `test_contrail`, `test_net`. Tools: `htaview --drive N
  --car C`, `htamatch --ride <placement>`, `HTA_LOOK_DROP=1 htamatch`.

## NEXT ENGINEERING OBJECTIVES

Done 2026-09-23 morning: bots drive the Warthog, Ghost and Scorpion
(`brain.c` `drive`, `board_offer`, `wheel_free`), crew each other's guns,
and projectiles hit people along their path (`fly` in `game.c`).
Done 2026-09-23 midday: vehicle-aware paths -- every nav node carries its
clearance for cars, drivers plan over ground wide enough for their car,
steer round other vehicles, and a jam is the physics refusing a move
(team matches: 21% of wheel time blocked -> 10%). Remaining, in order:

1. **Device feedback** on this build and the ones before it (see CURRENT
   TESTING OBJECTIVE).
2. **Driving in a fight** (in progress, see `docs/RUNNING_LOG.md`). The
   Ghost's blind strafing was most of it: now it strafes only toward open
   ground, FFA 15.1% -> 8.7% blocked. What is left per match: chasing on
   a path ~34 s, straight at a man ~26 s, tank/Ghost holding at range
   ~24 s. `scripts/drivebench.sh ffa` measures it.
3. **Banshee pilots.** `drivable()` leaves the Banshee to people. Flying
   needs no path, only height and a target.
4. **CTF polish:** the cloth moving; carriers as passengers (check Halo PC
   first); bots that capture against a defence.
5. **Oddball, King of the Hill, Race.** Ball `weapons\ball\ball` (held as
   a pistol); oddball spawns are netgame flags type 2; hills type 8 by
   usage id; race checkpoints type 3. Lines and texts (95, 155-166,
   170-177) are in the Trial. The GAME packet has room for their state.
6. **Client-side prediction** for the local driver on a joining phone.
7. **Smaller gaps:** a damaged hull could darken (instances carry no tint
   yet); powerups spinning; active camouflage rendering; free-for-all
   player colours; the tracker's sweep; the remaining menu screens.

## Where things stand

### Working, confirmed on device

Blood Gulch renders with lightmaps, detail maps, sky and glass; scenery and
vehicles are placed and collidable. First-person movement on the Trial's own
biped physics — run, crouch, jump, slopes, headroom, fall damage.

**Weapons.** All eleven playable, with real first-person models, hands from
`globals`, and their own animation graphs: ready, idle, fire, reload (shotgun
shell-at-a-time chaining included), melee, and `throw-grenade`. Muzzle flashes
from the firing effect's own particle. Spent brass from the ejection marker.
Per-weapon zoom with the sniper's scope furniture. On-gun round counters drawn
with the HUD font.

**HUD.** Crosshair, shield and health meters, ammo pips, the rounds counter,
scope overlays, and a full-screen fade.

**Sound.** Positional and panned, the weapon's own firing sounds, footsteps by
material, impacts by material, the flamethrower's looping roar, the shield's
five HUD sounds (recharge hum, hit, low, depleted, heartbeat), and the Chief's
death lines.

**Combat.** Hitscan and object projectiles, per-material impact effects and
decals, particles with the tags' own physics and colours, grenades, rockets
that explode properly, health and shields with recharge.

**Dying.** Full sequence: death line, the camera leaves your head and watches
your body go down with one of Halo's kill animations, five seconds, respawn at
a spawn point away from where you died.

**Items.** Blood Gulch's own 37 placements with real respawn timers and
weighted choices. Two-weapon carry, the map's own AR+pistol loadout, SWAP to
switch or to pick up.

**World-object filtering.** Bodies, corpses, pickups, grenades and projectiles
now upload mipmaps once alongside their textures. HUD, viewmodel and particle
uploads retain their existing filtering. Device confirmation pending.

**Bots and Slayer** (host-verified; see "New on 2026-09-22"). The old
stationary target is replaced by bots whenever bots are on.

**A drivable Warthog.** Human jeeps (`vehi+756 == 1`) come out of the static
world into their own render mesh and their own collision grid, and the twelve
Blood Gulch placements are drivable: enter, throttle, reverse, steer, brake,
exit. Speeds, acceleration, steering lock and turn rate all come from the
vehicle tag; the wheelbase and mass points come from its `phys` tag. Chase
camera, DRIVE/EXIT/BRAKE touch labels and a km/h speedometer. The moving hull
is solid to bullets, grenades, footsteps and the player. The owner has tested
the driving revisions on device and accepted this vehicle slice for now. The
tire meshes spin and steer by model node and travel down to meet terrain. The
chassis can leave the ground over a crest. Wall and vehicle contacts now
exchange a 2-D impulse using the `phys` mass, centre of mass and yaw inertia;
throttle keeps spinning the tires under load and can pivot a blocked jeep.
The exact behavior of the latest collision release is host-verified; a
separate phone result was not reported.

**Placed model textures.** Model UV scales are restored from `mod2+48/+52`;
opaque placed `shader_model` surfaces use scene lighting. This fixes the
Warthog sampling the wrong texture regions and being drawn unlit. Reflections
remain a rendering gap; phone confirmation of this fix is pending.

### New on 2026-09-22 (host-verified, not yet seen on a device)

- **`src/game/`**: portable game layer. `game.c` units, attributed damage,
  Slayer scoring, kill feed from `ui\multiplayer_game_text`, bot names from
  `ui\random_player_names`, announcer events; `brain.c` bot AI; `nav.c`
  layered walkable grid from collision with A* (cached to disk);
  `view.c` skinned bot bodies + held weapons (instanced draw);
  `menu.c` the main menu from ui.map.
- **Renderer**: `hta_gfx_set_instances` (static mesh + rigid transform);
  sky pass with its own depth range, per-layer blending and the chicago
  multi-map fold (`chicago` fields on `hta_submesh`, mode `light_color.w=2`).
- **Audio**: Ogg Vorbis via vendored `src/third_party/stb_vorbis.c`;
  `hta_sound_decode_chain` for long sounds cut into permutations.
- **Tools**: `htamatch` (bot match rendered offscreen), `htamenu`.
- **Personal APK**: `publish_apk.sh --with-assets`; native maps
  `apk:maps/*.map` straight out of the APK.

### Not started

- **Bots flying.** Bots drive the Warthog, Ghost and Scorpion and crew
  guns; nobody but a person flies the Banshee.
- **Multiplayer beyond on-foot Slayer.** Android hosts now simulate remote
  movement, bots, damage, death, pickups, scoring and respawn; clients receive
  the match state. Vehicles and other game modes remain absent online. See
  `NETWORK_PROGRESS.md`.
- **Deeper menus.** The main menu and multiplayer submenu art/words come from
  ui.map. The Java overlay now offers solo match settings, multiplayer
  create/join, LAN discovery and Internet direct IPv4 join. These are custom
  screens, not a `DeLa` interpreter. Profiles remains inactive; SETTINGS is
  the Java setup screen. The II button pauses the game.
- **Other game types.** Slayer, Team Slayer and CTF run (solo and LAN);
  Oddball, King of the Hill and Race do not.
- **Music.** There is none in Blood Gulch, and that is correct — Halo CE
  multiplayer maps carry no score. The campaign map has it.

### Multiplayer continuation

- The gameplay state and per-frame update currently live in
  `src/platform/platform_android.c` (`hta_android`, `android_main`). The
  player, vehicles, projectiles, vitals and other mechanics already have
  portable `src/engine/` modules. Read the live loop before deciding where
  shared session state, simulation timing and network messages belong.
- Two automated desktop clients still exercise the visual transport path.
  Their headless server does not run a match. Android hosts now own on-foot
  Slayer movement, damage, bots and scoring and send all match units to the
  clients. The host phone joins its own nonblocking UDP server through loopback.
  Two-human/device runtime verification of on-foot play remains pending.
  Online vehicles are the next engineering objective; network vehicle entry
  remains disabled in the current build, and solo driving is unaffected.
- Preserve the asset boundary: each client imports its own Trial data. Do not
  send or bundle map assets. Use the existing `scripts/verify.sh` gate before
  publishing an APK for device testing.

### Known gaps worth fixing

| | |
|---|---|
| Items do not rotate | Halo spins powerups. Doing it means paying the full item upload every frame or splitting powerups into their own dynamic mesh. The latter. |
| Camouflage does nothing | It runs its timer. Nothing to hide from yet. |
| No hit sound on the body | `weapons\*\effects\impact cyborg shield` is in the cache and is the right thing to reach for. |
| The bot's rifle is hardcoded | Should be whatever it is carrying, once it carries anything. |
| Vehicle collision remains planar | Wall and jeep contacts rebound, deflect and apply yaw torque, but there is no full 3-D rigid body or flip. A truly too-narrow passage can still trap the Warthog; reverse or exit if safe. |
| Vehicles do not roll over | A blast throws and tumbles a vehicle in the air, but on landing it levels out; lying on its roof (and being flipped back) is not simulated. Hulls ARE destructible now (ours; Halo CE's were not). |
| Motion tracker sweep | The sweep ring art has an opaque edge; it is not drawn, so the tracker does not animate its sweep. |
| Joiner driving lag | A joining phone sees its own vehicle respond one snapshot late; no prediction. |

---

## How to read Halo's tags

This is the part that makes or breaks a session.

**1. Struct sizes must reconcile.** Walk a struct's fields in
`upstream/invader/src/tag/hek/definition/*.json`, add up the sizes, and check
the total against the declared `size`. If it matches, every offset before it is
trustworthy. If it does not, the walk drifted and you must probe instead.

**2. `"bounds": true` counts as TWO values.** Missing that is what put every
`Weapon` offset past `zoom magnification range` in the wrong place.
`"count": N` multiplies.

**3. Probe when the walk drifts.** Write a throwaway C program against
`libhta_engine.a`, scan the tag data for a dependency whose class is what you
expect, and print the offset. `Projectile`'s `impact damage` at **+548** and
`UnitHUDInterface`'s sounds at **+960** were both found this way, and both
disagree with a naive walk.

**4. Sanity-check the values against the world.** A number that reconciles can
still be the wrong field. The spent-casing radii were checked against real
cartridge sizes (9 mm, 7.62×51, 12-gauge, .50 BMG) and that is what revealed
every particle was being drawn at twice its tagged size.

**5. Velocities in Halo tags are per TICK (30/s).** Projectile initial and
final velocity, falling-damage velocities. Ranges and timers are not.

**6. One world unit is 3.048 m** (ten feet). The standing eye is 0.62 wu.
Comments in this codebase sometimes say "12.5 cm" where they mean 0.125 wu —
do not trust a unit in a comment, check the constant.

### Structs known to reconcile

Projectile 588 · Effect 64 · EffectEvent 68 · EffectPart 104 · EffectParticle
232 · Particle 356 · PointPhysics 64 · ShaderEnvironment 836 · ShaderModel 440
· ShaderTransparentGlass 480 · ShaderTransparentChicago 108 · Sound 164 ·
SoundLooping 84 · Font 156 · WeaponHUDInterface 380 · Globals 428 ·
Vehicle 1008 · GBXModel 232 · ModelCollisionGeometry 664 · DamageEffect 672 · Object 380 · Unit 752 ·
Dialogue 4112 · Scenario 1456 · ScenarioNetgameEquipment 144 · ItemCollection
92 · ItemCollectionPermutation 84 · ScenarioPlayerStartingProfile 104 ·
ScenarioStartingEquipment 204 · UnitHUDInterfaceHUDSound 56 · Equipment 944 ·
ScenarioNetgameFlags 148 (Scenario +888) · Flag 96 · FlagAttachmentPoint 52

### Offsets found by probing (the walk drifts to reach them)

| what | where |
|---|---|
| Projectile `impact damage` | **proj+548** (needler has none; scan for any damaging `jpt!`) |
| Projectile detonation responses | proj+576, 160 each, kind at +2 (reflect == 2) |
| Projectile `detonation timer starts` | proj+384 |
| UnitHUDInterface sounds | **unhi+960**, 56 each, `latched to` at +16 |
| `unhi` motion sensor background / foreground | 620 / 724 |
| ParticleSystem particle types | pctl+92, 128 each |
| — type radius / states / particle states | +44 / +104 (192 each) / +116 (376 each) |
| — state rate / duration | state+88 / state+32 |
| — pstate bitmap / radius mult / blend / colours | +48 / +128 / +226 / +96 and +112 |
| Equipment powerup type / grenade type / time / pickup sound | 776 / 778 / 780 / 784 |
| DamageEffect per-material multipliers | **jpt+512**, one float per MaterialType |
| Unit `melee damage` | 380 + 268 |
| Vehicle type / driving floats | **vehi+756** (u16; 1 == human jeep) / **vehi+760**, 8 floats: forward, reverse, accel, decel, left turn, right turn, wheel circumference, turn rate |
| Object model / animation graph / attachments | +40 / +56 / +320 (72 each) |

**MaterialType 21 is cyborg armour, 22 is cyborg energy shield.** Those two
are how much of a hit a player actually takes.

---

## Traps that have already cost a session

Each of these bit once and is now defended by a test. Search `JOURNAL.md` for
the full story.

- **Overlay animations (`type 1`) must not be played as ordinary clips.** They
  keyframe a handful of nodes; every other node takes the *animation's own
  defaults*, not the pose underneath. The body turns inside out. Use
  `hta_anim_animates` and compose. This has bitten **twice** — the needler's
  ammunition and the cyborg's flinch. **Check `anims[i].type` before playing
  anything new.**
- **`hta_anim_find` matches a SUBSTRING.** The cyborg has 254 animations and a
  dozen seated ones; plain `"idle"` finds `B-driver unarmed idle`, a body
  sitting in a Banshee, hanging in the air.
- **Fix shading before trusting animation.** The needler was "broken" through
  three animation rewrites; it was a `sgla` shader nothing handled.
- **A texture table must exist before anything interns into it.**
  `hta_model_append_skinned` fills `mesh.textures`, it does not create it.
- **`hta_submesh_init` exists because zero is a valid texture index.** A
  memset left `detail_tex = 0`, which multiplied every model by its own
  texture 0.
- **`add_elem` sets its fields one by one**, so a new field on `hta_hud_elem`
  is garbage on every other element until you initialise it there.
- **A tint alpha of zero means "no tint"** to the HUD draw loop, which then
  falls back to opaque white. A fade of nothing must collapse the quad.
- **Relink probe binaries after `cmake --build`.** A stale probe against the
  old static library gives confident wrong answers. This has recurred.
- **Read one-shot vitals flags BEFORE `hta_vitals_update`**, which clears them.
- **Blast deaths land a frame late** because projectiles update after vitals.
  That is invisible and fine.
- **A corpse must stop no bullets**, or a dead body soaks the magazine meant
  for the next one.
- **`hta_collision_build` memsets the grid**, which clears the optional
  `extra` link to a moving-object grid. Re-point it after any rebuild, or
  vehicles silently stop being solid.
- **Vehicle physics must walk the STATIC grid** (`extra = NULL`). Given the
  whole world, every jeep collides with its own hull and cannot move.
- **Type-1 overlays are DELTAS from their own first frame**, on the body as
  on the viewmodel: `local = ov[f] . ov[0]^-1 . local`. Substituting them
  flips a running body upside down (bit twice). Type 2 substitutes.
  `test_actor` sweeps 63 stance/overlay pairs.
- **The sky lives 6,400-98,000 units out.** It has its own depth range in
  the sky pass; never draw it with the world's far plane.
- **The local player's vitals are the game's** (`s->vit`). Damage the local
  player through `hta_game_*` so it is attributed; the game notices deaths.
- **Menu → game → menu relaunches GameActivity** only after the old native
  thread has exited (`onDestroy`); two `android_main`s would share the
  static state.
- **The nav cache key** is map CRC + biped radius/height/slope. Change the
  nav algorithm or `hta_nav_node` → bump `NAV_VERSION` in `nav.c` (3 since
  car clearance).
- **Build the nav grid over the RENDER BSP's bounds**, never the collision
  mesh's: the collision reaches up to a lid at z 50, and a grid started
  above it takes the lid's top for a floor -- twice the nodes, the map in
  two regions, every car path "unreachable". `hta_nav_build` also drops
  `col.instances` itself now (parked vehicles are not walls).
- **A jam is the physics refusing a move, not slowness.** Bots compare
  `hta_vehicle.blocked` across a second. Judged on distance alone, a car
  still rolling back out of the last jam looked stuck again -- 90 "jams" a
  match, most of them that loop -- while a tank turning on the spot into
  a post (no gas, so never a jam) sat there all match. test_ride "from
  home".
- **A* needs its closed set.** Without it every re-pushed node was expanded
  again: base to base took 64k expansions on foot and 100k+ for a car,
  over the 60k budget, so cross-map drives had no path at all. Car
  searches also overweight the heuristic (`NAV_CAR_GREED`). test_nav "for
  cars" plans base to base inside the budget.
- **A seated unit's `eye.pos` is the vehicle's chase camera** -- behind
  and above a Warthog, sometimes inside a hillside. Line of sight for a
  seated bot comes from its body (`eye_of` in `brain.c`); AIM angles stay
  from `eye.pos`, because `vfire` converges on the camera's crosshair ray.
- **A death must clear the body's remembered clip** (`base_clip` in
  `view.c`). The local body is skipped on foot, so back in the same seat
  after a respawn the name matched and the death kept playing under the
  Ghost. test_ride "back in the seat you died in".
- **A fast round hits along its path, not at its point.** A tank shell
  moves 1.3+ wu an update; the body is 0.35 wu wide. `fly` sweeps
  `hta_game_ray` from last position to this one (test_ride "a shell does
  not pass through a man").
- **Place test targets on the ground, not in a vehicle's frame.** A parked
  car sits tilted on its springs; 14 wu out along its frame is under the
  grass, and a buried target is invisible. `open_ground` in test_ride
  finds level, open field.
- **Dynamic meshes write one vertex slot per in-flight frame.** Uploading a
  change once leaves stale geometry in the other slots. The count belongs to
  the swapchain, so re-upload for several frames.

---

## Numbers that are OURS, not the tags'

Every one of these is invented because no tag carries it. If something feels
wrong, this list is the first place to look — they are all one constant.

| constant | value | what it is |
|---|---|---|
| `HTA_HUD_PHONE_SCALE` | 1.75 | HUD size on a phone |
| `HTA_HUD_WEAPON_SCALE` | 0.5 | the weapon block |
| `HTA_SOUND_NEAR` / `_FAR` | 3 / 60 wu | distance attenuation |
| `HTA_GRENADE_THROW` | 9.0 wu/s | throw speed |
| `PROJ_BOUNCE` | 0.35 | projectile restitution |
| `PART_REST_SPEED` | 0.15 wu/s | below this a bounced particle has landed |
| `HTA_PART_AREA` | 120 sq wu | live particle quad budget — **the GPU knob** |
| `HTA_PART_RECUR` | 0.25 s | how often an effect is assumed to recur |
| `HTA_RESPAWN_DELAY` | 5 s | player respawn (gametype value, not in the map) |
| `HTA_DEATH_*` | — | death camera pull-back, fade timings |
| `HTA_VITALS_LOW` | 0.25 | when "low shield"/"low health" sounds start |
| `HTA_DAMAGE_FLASH_TIME` | 0.55 s | Android red hit cue fade; overlay uses a 3.5% screen edge, up to 190/255 edge and 35/255 centre alpha |
| `HTA_OVERSHIELD_MULT` | 3.0 | how much overshield gives (tag says how long only) |
| `HTA_PICKUP_REACH` | 0.5 wu | pickup radius |
| `HTA_PICKUP_LIFT` | 0.06 wu | how far an item floats off its placement |
| `HTA_ITEM_RESPAWN_DEFAULT` | 15 s | when neither placement nor collection says |
| `HTA_MELEE_REACH` | 0.5 wu | how far a swing reaches |
| `HTA_VM_KEY_FRACTION` | 0.35 | when a clip "does its thing" if `key frame` is 0 |
| `HTA_BOT_RESPAWN` | 5 s | how long a body lies there |
| `HTA_ITEMS_UPLOAD_FRAMES` | 8 | ≥ any swapchain image count |
| `HTA_VEHICLE_ENTER_REACH` | 0.9 wu | how close to a driver's seat DRIVE appears |
| `HTA_VEHICLE_EXIT_SPEED` | 0.5 wu/s | below this a jeep counts as stopped, for entering and exiting |
| `HTA_VEHICLE_ADHESION_SPEED_FRACTION` | 0.25 of tagged forward speed | below this, the `phys` ground depth can keep the chassis in contact over rough terrain; above it, the jeep can fly off a crest |
| `HTA_VEHICLE_SETTLE_TIME` | 0.5 s | time an undriven jeep keeps posing its chassis on terrain before sleeping |
| `HTA_VEHICLE_RESTITUTION` | 0.20 | normal-velocity rebound on wall and jeep contacts; no crash restitution is tagged |
| `HTA_VEHICLE_YAW_DAMP` | 2.0 /s | decay of collision-induced yaw velocity; no tagged yaw damping law |
| `HTA_VEHICLE_STEP` | 1/120 s | fixed physics substep, so frame rate cannot change handling |
| `HTA_VEHICLE_CLEARANCE` | 0.04 wu | how far a mass point may be pushed before it counts as blocked |
| `HTA_VEHICLE_MAX_SLOPE` | 0.75 rad | cap on the pitch/roll the wheels may pose the body to (43°) |
| `HTA_VEHICLE_CAMERA_BACK` | 2.5 wu | chase camera distance, pulled in by terrain |
| `HTA_VEHICLE_CAMERA_UP` | 0.7 wu | chase camera height above the hull |
| `HTA_SLAYER_SCORE_LIMIT` | 25 | Slayer's kill limit (stock Halo CE gametype, not in any map) |
| `HTA_SLAYER_RESPAWN` | 5 s | bots' respawn (the player's `HTA_RESPAWN_DELAY`) |
| `HTA_MULTIKILL_WINDOW` | 4 s | double/triple kill / killtacular window |
| `HTA_SPREE_KILLS` / `HTA_RIOT_KILLS` | 5 / 10 | killing spree, running riot |
| `HTA_CREDIT_WINDOW` | 5 s | a death this soon after a hit is that attacker's kill |
| `HTA_BACKSMACK_MULT` | 10 | melee from behind kills (weapon melee `jpt!` is 56) |
| `UNIT_SWING_TIME` / `UNIT_THROW_TIME` | 0.6 / 0.45 s | bot/remote melee and grenade timing |
| brain tables (`src/game/brain.c`) | per skill | sight range, FOV, turn rate, reaction, aim error; weapon preference |
| `HTA_NAV_CELL` | 0.35 wu | nav grid spacing; max drop 1.0 wu |
| `HTA_FEED_TIME` / `HTA_BANNER_TIME` / `HTA_POSTGAME` | 6 / 3 / 10 s | kill feed, announcer banner, scoreboard |
| menu layout (`src/game/menu.c`) | fractions | measured from a PC Trial screenshot; menu light and camera sway ours |
| sky depth range | 10 .. 200000 | the sky pass's own near/far planes |
| `HTA_SCOUT_REVERSE_FRACTION` / `_STRAFE_` | 0.5 / 0.75 | Ghost reverse and strafe speed, of forward (tag has none) |
| `HTA_FIGHTER_STRAFE_FRACTION` | 0.5 | Banshee strafe |
| `HTA_FIGHTER_MAX_PITCH` / `_BANK` | 1.0 rad / 0.35 | Banshee nose follows the look this far; visual bank per rad/s of turn |
| `HTA_VEHICLE_RESPAWN` | 60 s | an empty vehicle away from home goes back (gametype value) |
| `HTA_VEHICLE_BARREL_SPIN` | 30 rad/s | chaingun barrels while firing |
| `HTA_SPLATTER_MIN_SPEED` / `_FULL_` | 1 / 3 wu/s | closing speed at which a vehicle starts to hurt / deals all of `vehicle_collision` |
| `HTA_VEHICLE_AIM_RANGE` | 200 wu | crosshair ray for vehicle-gun convergence |
| `VEHICLE_TRAVEL_SPEED` (game.c) | 150 wu/s | slower vehicle rounds fly, faster are hitscan |
| tank/scout/fighter hull turn | the driver seat's `yaw rate` | a tag value used for something the tag does not say it is for |
| `HTA_CONT_MIN_LIFE` | 0.06 s | shortest contrail point life (the AR tracer's is 0.01) |
| tracer speed | 300 wu/s | hitscan tracers' head speed |
| `HTA_DROP_LIFE` / `_REACH` | 30 s / 0.5 wu | dropped weapons |
| `HTA_MOTION_RANGE` | 25 m (8.2 wu) | tracker radius; the tag's 20 has no unit and the art says 15m |
| `HTA_MOTION_SPEED` / `_FIRE` | 0.5 wu/s / 1 s | shown when moving faster / after firing |
| `HTA_HUD_SENSOR_X/Y` | 4, 4 canvas px | tracker in the bottom-left corner |
| engine pitch | 0.85 .. 1.35 | engine loop rate from idle to full speed |
| `HTA_CTF_SCORE_LIMIT` | 3 | CTF captures (stock Halo CE gametype) |
| `HTA_FLAG_RESET` | 30 s | a dropped flag left alone goes home |
| `HTA_FLAG_REACH` / `HTA_CAPTURE_REACH` | 0.5 / 0.75 wu | take a flag / score on your stand (and within -0.4..0.9 wu vertically) |
| `HTA_FLAG_REGRAB` | 1 s | put it down and you cannot take it straight back |
| carriers on foot | — | a flag carrier cannot board a vehicle |
| friendly fire | off | team games: a teammate's hit does nothing |
| `hta_game_team_color` | red 0.78,0.10,0.08 / blue 0.12,0.24,0.85 | team armour colour (Halo's colours live in the exe) |
| CTF roles | every third on a side defends | `hta_game_ctf_goal` |
| `PUSH_STOP` (brain.c) | 5 wu | an attacker stops to duel only this close |
| `CLOTH_RIPPLE` (view.c) | 0.035 wu | the still cloth's baked ripple; Halo simulates it |
| `HTA_TRACER_TAIL` | 1.2 wu | a hitscan tracer's streak is cut this far behind its head (the tag's 0.01 s point life, stretched to 0.06 s to be drawable, made 18 wu beams) |
| `HTA_PART_TYPES` / `_RECIPES` / `_MAX` | 64 / 40 / 1536 | particle table sizes; at 24/16/768 every explosion was silently dropped once vehicles were on |
| `HTA_HULL_*` | jeep 240, tank 480, Ghost 160, Banshee 220, turret 200 | vehicle hull strength in `jpt!` points (the tags say 0: Halo CE MP vehicles are indestructible). Damage per hit is the tag's `jpt!` vs thick metal (material 7) |
| `HTA_VEHICLE_WRECK_TIME` | 20 s | a destroyed vehicle is gone this long, then back home |
| wreck blast | the Scorpion shell explosion's own effect and damage | what a hull going up looks like and does |
| `HTA_BLAST_PUSH` | 0.03 wu/s per damage point x 5000/mass | how hard a blast throws a vehicle (plus tumble) |
| `HTA_RECOIL_PUSH` / `_MAX` | 0.01 per blast point x 20000/mass, max 1 wu/s | a cannon's kick back on its vehicle |
| `HTA_VEHICLE_COAST_*` | jeep 0.15, tank 0.45, Ghost 0.30, Banshee 0.20 of the tag's decel | slowdown with the stick released; the tag decel is now braking only |
| `HTA_VEHICLE_DRIFT_GRIP` | 1.0 of accel | Ghost/Banshee: velocity across the hull corrected separately, so turns slide |
| `HTA_VEHICLE_SWAY_*` | jeep 0.018, tank 0.008, Ghost 0.014 rad per wu/s^2; max 0.14; spring 45, damping 7 | visual suspension pitch/roll |
| `HTA_HUD_TARGET_R/G/B` | 1.0, 0.12, 0.08 | the red reticle (no tag carries it) |
| spark threshold | hull below 35% | a failing hull throws the chaingun's metal-impact sparks |
| bot driving (`src/game/brain.c`) | `BOARD_REACH` 18 wu, `BOARD_GIVE_UP` 8 s, `BOARD_FIGHT` 15 wu, `BOARD_OBJECTIVE` 30 wu | a bot walks to an empty Warthog/Ghost/Scorpion wheel this close, gives up after this long, not with an enemy this near or a flag this near |
| | `DISMOUNT_NEAR` 10 wu, `DISMOUNT_HULL` 0.25 | gets out this near a flag, or with the hull below this |
| | `WAIT_GUNNER` 3 s | a bot Warthog driver holds for a teammate bot to climb on the gun |
| | `TANK_RANGE` 25 wu, `GHOST_RANGE` 12 wu, `ORBIT` 14 wu | the tank shells from range; the Ghost closes then strafes; a crewed Warthog circles |
| | `STUCK_LIMIT` 3 jams, `JAM_SKIP` `HTA_VEHICLE_RESPAWN` + 5 s, `ROAM_MIN/MAX` 30/70 wu, `BEHIND` 1.9 rad | a jam backs out 1.2 s; three and it walks, and no bot takes that car until it has gone home; roam goals the path search can reach; a goal behind a Warthog is backed round to |
| | `JAM_HELD` 0.3 s | a second in which the physics refused the car this long, with the stick or wheel pushed and under 0.5 wu moved, is a jam |
| | `AVOID_AHEAD` 8 wu, `AVOID_GAP` 0.8 wu | another car on the line ahead this near is passed beside, bodies this far apart (the grid has no vehicles in it); an enemy's car is rammed |
| | `STRAFE_LOOK` 3 wu | a Ghost strafes only toward a side with open car ground this far off |
| | `DRIVE_FROM` 6 wu, `DRIVE_TO` 15 wu | how far round itself / its goal a car looks for open ground |
| car clearance (`src/game/nav.c`) | `hta_nav_car_clear`: 0.8 x the collision radius, less 0.2 wu, in 0.35 wu cells | room a car keeps from walls: Warthog and Ghost 2 cells, Scorpion 5. A node is open ground when it is not near a wall and all 8 neighbours are there within `HTA_VEHICLE_MAX_SLOPE` |
| | `NAV_ESCAPE` 10 wu | a car path may cross narrower ground only this near its ends, at 2 + the shortfall times the cost |
| | room cost 1 + 0.5 per cell short of `clear + 2`; `NAV_CAR_GREED` 2 | car paths keep to the middle of what room there is; the search's distance-to-go counts double |
| blast aim | the feet | bots aim anything that explodes at an on-foot target's feet, as people do |
| shell tolerance | 0.03 rad + 0.3 wu | a single-shot cannon is laid this tight before a bot fires |

---

## Tools

**README screenshots** live on the GitHub pre-release `media`, never in
git. To refresh one, render it (e.g. `htamenu ui.map --width 1600 --height
900 --time 6`, `htaview ... --fp idle --eye X Y Z --yaw D`, `htamatch
--mode ctf|team ... --width 1600 --height 900`), convert with
`magick X.ppm -quality 86 -strip name.jpg`, and
`gh release upload media name.jpg --clobber`. The README links
`releases/download/media/<name>.jpg`; headless Firefox
(`firefox --headless --screenshot out.png <url>`) shows whether it renders.

**If host Vulkan fails** (`VK_ERROR_INCOMPATIBLE_DRIVER`: NVIDIA userspace
newer than the loaded kernel module until a reboot), use the unpacked Mesa
lavapipe: `export VK_ICD_FILENAMES=$PWD/scratch/lvp/usr/share/vulkan/icd.d/lvp_icd.json`
before `scripts/verify.sh`. `scripts/tagwalk.py <Struct> [filter]`
prints Invader struct offsets and checks the total against the declared size.
`scripts/ndkcheck.sh` syntax-checks `platform_android.c` with the NDK in a
second (verify.sh is still the real gate).

```
# look at the first-person view without a device
./build-host/htaview $HTA_MAP --fp idle   --shots 2 --out /tmp/fp
./build-host/htaview $HTA_MAP --weapon "rocket launcher" --fly 0.05
./build-host/htaview $HTA_MAP --drive 4 --steer 0.3   # drive a jeep, then render it

# chase a "stuck here" report: the HUD readout gives x y z
./build-host/htaprobe $HTA_MAP --at 96.57 -155.72 --z 0.81   # why it pushes
./build-host/htaprobe $HTA_MAP --find 0.81 --near 96 -155    # if a digit is unreadable
./build-host/htaview  $HTA_MAP --eye 96.57 -155.72 1.43 --yaw 180

# a bot match / the main menu, rendered offscreen
./build-host/htamatch $HTA_MAP --bots 6 --seconds 20 --shots 4 --back -0.9
./build-host/htamenu  $(dirname $HTA_MAP)/ui.map --select 1

# how bots drive: 5 minutes headless, live vehicles, every entry/exit/wreck
# logged, and at the end "driving: N entries, S s at a wheel, B s blocked" --
# blocked is time the vehicle physics refused a driven move. --seed N plays
# another match of the same setup.
./build-host/htamatch $HTA_MAP --bots 8 --mode team --seconds 300 --shots 0 --vehicles --seed 3
# ...but one match is chaos. Judge driving changes on eight, side by side:
scripts/drivebench.sh team        # or ctf / ffa
# Baseline (Ghost strafes toward open ground): team 7.4% blocked, ctf 9.5%,
# ffa 8.7% (before vehicle-aware paths: 21.2 / 13.0 / 15.9).

# tags and sounds
./build-host/htainfo  $HTA_MAP
./build-host/htasound $HTA_MAP --dump <tag>
```

`htaprobe --at` prints the whole vertical column, headroom, standing and
crouching push, and every nearby triangle with a verdict. **Probe from the
player's own z** — a ground query from the sky finds the roof. `--eye` takes
the eye position, so add the 0.62 standing eye height to their feet z.

**Writing a throwaway probe** is the normal way to answer a tag question:

```c
cc -O1 -I src probe.c build-host/libhta_engine.a -lm -o probe
```

Put them in the session scratchpad, not the repo. Relink after every rebuild.

---

## Key files

| Area | Path |
|---|---|
| **Tag layouts** | `upstream/invader/src/tag/hek/definition/*.json` |
| Cache, tag lookup, reflexives | `src/asset/cache.c`, `src/asset/bsp.c` |
| Bitmaps, shaders, detail maps, tints | `src/asset/bitmap.c` |
| Models, skinning, markers, LOD | `src/asset/model.c` |
| Animation graphs, transform algebra | `src/asset/anim.c` |
| Weapons, magazines, triggers | `src/asset/weapon.c` |
| Effects, particles, damage, attachments | `src/asset/effect.c` |
| Sounds (`snd!`, Xbox ADPCM) | `src/asset/sound.c` |
| Dialogue (`udlg`) | `src/asset/dialogue.c` |
| Items, collections, starting loadout | `src/asset/items.c` |
| Biped physics | `src/asset/biped.c` |
| HUD font digits | `src/asset/font.c` |
| Player movement, collision grid, **ray query** | `src/engine/player.c` |
| Vehicles: types, seats, physics per kind, parts, cameras | `src/engine/vehicle.c` |
| Contrails (tracers, trails) | `src/engine/contrail.c` |
| First-person weapon | `src/engine/viewmodel.c` |
| World-space skinned character | `src/engine/actor.c` |
| A body that can be shot | `src/engine/bot.c` |
| Hitscan, scorch marks | `src/engine/gun.c` |
| Projectiles in flight | `src/engine/projectile.c` |
| Particles | `src/engine/particle.c` |
| Health and shields | `src/engine/vitals.c` |
| Pickups | `src/engine/pickup.c` |
| Magazine and reload | `src/engine/ammo.c` |
| Screen HUD | `src/engine/hud.c` |
| Voice mixer | `src/engine/audio.c` |
| Vulkan renderer | `src/gfx/gfx_vulkan.c`, `shaders/*` |
| Android glue, game loop, JNI | `src/platform/platform_android.c` |
| Touch HUD, DBG pad | `android/.../GameActivity.java` |
| Map picker | `android/.../SetupActivity.java` |
| **Game rules, units, Slayer, events** | `src/game/game.c` |
| Bot AI | `src/game/brain.c` |
| Nav grid, A*, disk cache | `src/game/nav.c` |
| Bodies + held weapons for units | `src/game/view.c` |
| Main menu from ui.map | `src/game/menu.c` |
| `ustr` string lists | `src/asset/strings.c` |
| Ogg Vorbis (stb_vorbis) | `src/asset/ogg.c`, `src/third_party/stb_vorbis.c` |
| Tests | `tests/test_*.c` — most take `$HTA_MAP` |

---

## Other docs

| | |
|---|---|
| `docs/JOURNAL.md` | every session, newest first. The *why*. Search by symptom. |
| `docs/BUILD_ENVIRONMENT.md` | SDK/NDK, toolchain, how the build is wired |
| `docs/BLOOD_GULCH_ASSETS.md` | what is in the map |
| `docs/INVADER_ASSET_PIPELINE.md` | how Invader's definitions are used |
| `docs/ANDROID_PORT_INVESTIGATION.md` | the original feasibility work |
| `docs/PROGRESS.md` | early milestone log |

---

## Working notes for the next agent

- **The owner tests every build.** Small, shippable slices beat big ones.
  Publish, say what to look at, and let the next screenshot decide.
- **Say which numbers are invented.** The owner has repeatedly, correctly,
  pushed back on things that felt wrong, and every time the answer was either
  a tag we were not reading or a constant we had made up. Flag them in the
  release notes.
- **When something looks wrong, suspect the data path before the art.** The
  body that "looked ugly" was unlit and unarmed; the model was fine.
- **Measure before optimising.** The particle lag was blamed on fill twice and
  was CPU raycasts both times. The 120 fps idle reading in a screenshot was
  the proof.
- **Write the test that would have caught it.** Every trap above has one.
