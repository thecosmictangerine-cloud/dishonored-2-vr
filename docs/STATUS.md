# Dishonored 2 VR: feature status

What works, how well it has been checked, and what is known to be broken or still open, by feature area. It covers
the 0.5.0 release (beta).

Where this comes from: the development log of every feature, the engine notes in [ENGINE.md](ENGINE.md), the
player [README](../package/README.txt) and the shipped [DishonoredVR.ini](../package/DishonoredVR.ini). When this file
and the code disagree, the code wins. Please report it.

---

## 1. Summary

### Game

| | |
|---|---|
| Supported game | **Dishonored 2 on Steam, build 1.77.9.0** (the current Steam build). The build is checked by version, file size and link timestamp: Steam rebuilt the exe once without changing "1.77.9.0". |
| Not supported | The GOG version (a different exe that also says 1.77.9), Microsoft Store / Game Pass, and *Death of the Outsider*. On these the mod shows a message box at startup, installs no hooks, and the game runs as normal. |
| Display mode | **Windowed only.** In fullscreen and borderless the engine renders at the monitor's size, so the headset gets a blurry picture (see section 3). |
| OS | Windows 10 / 11, 64-bit. The DLLs link the C runtime statically, so no redistributable is needed. |
| Graphics API | D3D11 (the game's only renderer). |
| Development PC | One laptop with an NVIDIA RTX 3060 Laptop GPU. All performance numbers here come from it. Testers' hardware is not recorded. No AMD or Intel GPU has been tested, as far as the records show. |

### Runtimes and headsets

| Runtime / headset | State |
|---|---|
| **Meta Quest 3, Meta's OpenXR runtime (Link / Air Link)** | **The reference setup.** Every "Validated" and "Played" entry below comes from it. The recorded sessions ran over Air Link. |
| Quest 2 / Quest Pro on Meta's runtime | Expected to work (same runtime and controllers). Not recorded as tested. |
| Quest through Virtual Desktop (VDXR) | A beta tester used it with 0.1.0 and reported a blurry, zoomed-in picture, which 0.2.0 fixed. Not re-verified since. |
| SteamVR (any headset) | Startup was validated with SteamVR's "null" virtual headset; this is also where a startup crash every SteamVR user would have hit was found and fixed. Gameplay through SteamVR on a real headset has not been tested. The automatic new session after a stopped one runs only on Meta's runtime. |
| PlayStation VR2 through SteamVR | Experimental. SteamVR does not pass the Create button through, so pressing both sticks acts as the menu button. The native PS VR2 interaction profile is rejected (`XR_ERROR_PATH_UNSUPPORTED`), so the Sense controllers appear as Touch. Not tested on hardware. |
| Valve Index | Experimental. It has its own interaction profile (menu = left trackpad pressed hard). Not tested on hardware. |
| WMR, Vive, Pico, Pimax and others | No dedicated interaction profiles. Headsets with canted displays (for example Pimax without parallel projections) are expected to come out rotated or with black wedges (section 3). |
| No headset / no runtime | The game runs as the normal flat game. VR switches on by itself when a headset appears. Tested in the mock. |

### Verification levels used in the tables

| Level | Meaning |
|---|---|
| **Validated (Q3)** | The developer checked this specific behaviour on a Quest 3 and confirmed it works. |
| **Played (Q3)** | Used during real play sessions on a Quest 3, or seen working in their logs, with no dedicated check recorded. |
| **Mock** | Measured only in the project's fake OpenXR runtime (`mockxr/`): scripted head and controller motion, an emulated compositor (projection and quad layers, Meta's stalls and session stops), screenshots and logs. Precise, but not hardware. |
| **Fixed, unverified** | A fix for a problem seen on the headset. Built and shipped, not re-checked in game yet. |
| **Code only** | Implemented. No recorded measurement. |
| **Untested** / **Unclear** | Nobody has tried it / the records do not say. |

"Mock, then Played" means the latest refinement was measured in the mock on top of a base that had been used on the
headset.

---

## 2. Features by area

### 2.1 View and rendering

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Game frame shown in the headset (mono: the same image to both eyes) | Default | Played (Q3) | Default output. Real depth needs `Stereo3D` (below). |
| 6DOF head view (head position and rotation drive the engine's view matrix) | Done | Validated (Q3) | |
| Picture drawn at its true angles in each eye (orthoscopic, not stretched), with the real render pose declared to the compositor | Done | Played (Q3) | In the mock the stretch is 1.000 / 1.000 (before: 0.763 / 1.052). World swim fell from 0.87 / 2.92 deg to 0.12 / 0.25 deg. |
| Field of view covering the whole eye (`vr fov auto`; 108x110 deg on a Quest 3) | Done | Played (Q3); the "mode" fix is Mock | Applied as soon as the headset reports its fov, and again if the fov changes. Before 0.2.0 it was lost when VR started before the fov arrived. Capped at 150 deg per axis. |
| Render resolution follows the headset, not the PC window (`RenderScale`) | Done | Played (Q3) | Quest 3 default 2266x2348. About 4x the pixels of 0.1.0: 37-80 fps on the RTX 3060 laptop. Lower the resolution in the headset's PC app or set `RenderScale` < 1. Capped at 4096 per side and 16 MP (Mock). The game's dynamic resolution is held off while in VR. The game's own config file is never changed. |
| Stereo 3D, synchronised (`Stereo3D = 1`, `vr stereo 2`) | Done, on by default | Played (Q3) | Both eyes rendered from the same world instant (every second game frame held, Draw only) and submitted together; per-eye frustum and TAA. About half the frame rate. |
| Stereo 3D, alternating eyes (`Stereo3D = 2`, `vr stereo 1`; was `Stereo3D = 1` in 1.0) | Off | Played (Q3) | Real depth, but each eye refreshes at half the frame rate and sees another instant. At about 40 fps on the Quest it felt clearly worse (judder, depth shimmer). |
| Per-eye temporal anti-aliasing in stereo (`vr stereotaa`) | Done | Mock; tried briefly on the Q3 with no verdict | Each eye gets its own TAA history and previous matrices. Mock: motion vectors 17 px -> 0.03 px (same as mono). Open issues in section 3. |
| Camera motion blur off (`DisableMotionBlur`) | Default | Code only | It also unticks the option in the game's own settings, and it stays off afterwards. |
| Player body (torso and legs) hidden; only the arms are drawn | Default | Validated (Q3) | Always on in VR. |
| Sword swing trail hidden | Default | Mock | Recognised by three vertex shaders, only around the player's own Slash. |
| Game's own 2D HUD | Stereo patch: one finite-depth panel (`StereoHud`, `StereoHudWidth`) | Code only | In stereo, captured once and submitted on a head-locked quad 2.4 m ahead; the same image goes to both eyes. Width defaults to 60 degrees (35-90). The captured texture is transparent where the game draws no HUD; its notifications keep their own fade timing. Crosshair and markers stay 2D projections on that panel, not world-anchored. `StereoHud = 0` restores the original per-eye screen-space draw. Hardware verification remains pending. |
| Left-hand aim point | Red point (`AimDot`) | Code only | Projects the left controller's gun/pointer ray into the current eye; uses the weapon hit, pick target, or active power landing point when available, otherwise a configurable-range point. Hardware verification remains pending. |

### 2.2 Head tracking and the turning servo

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| The game's yaw and pitch follow the head (the mod injects mouse motion as a closed-loop servo) | Done | Validated (Q3) | Tracking error 0.0-0.1 deg in the mock. The left stick walks where you look. |
| Servo idle while the game is not simulating (menus, loads, pause) | Done | Played (Q3) | Nothing is injected into menus. |
| Mouse sensitivity and "invert Y" respected | Done | Mock | Counts are scaled by the game's live sensitivity. |
| Stale-reference guard (the servo can no longer lock up after a big camera jump) | Done | Mock | Fixed for loads, slides and the end of cutscenes. |
| Runtime recenter; position reference reset on every load | Done | Mock | |
| Tracking loss: last good head and hand pose held | Done | Mock | |
| Smooth turn (120 deg/s) or snap turn (45 deg) on the right stick | Done | Played (Q3) | `TurnMode`, `SmoothTurnSpeed`, `SnapTurnAngle`. |

### 2.3 Controls and button layout

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Controllers become the game's keyboard and mouse (SendInput) | Done | Validated (Q3) | Keys are sent by physical position (DirectInput codes), so they work with any keyboard layout. Ctrl, Shift and Alt are sent correctly. |
| The player's own key bindings are read from the game's config | Done | Mock | Measured with Jump on K and Crouch on V. No real AZERTY keyboard tested. Menus use fixed keys (an ESC rebound by the player is untested). |
| Console-style movement: 8-way stick, latched sprint, crouch toggle | Done | Mock, then Played | |
| Default layout: A jump, B crouch, Y / X quick slots 1-2 (held 0.5 s: 3-4), menu button short = journal / held = pause, potions B / A with the wheel open, in menus grips = next tab and triggers = sub-tabs | Done | Mock; pause Validated (Q3) | The game has no "previous tab": grips cycle through the tabs. With the wheel open, Y / X assign the highlighted item to a slot. |
| `Layout = classic` (Y leans, X interacts, menu button pauses) | Done | Mock | |
| Both sticks pressed together = menu button (for controllers without one) | Done | Mock | Needed for PS VR2. |
| Interact only with the right grip and the pointer (X no longer interacts) | Done | Played (Q3) | Lean by moving your body: there is no lean button. |
| `ControllerPitchOffset` (tilts the hands up or down) | Done | Mock | For controllers whose aim pose differs from Touch. Not measured on Index or Sense. |
| Haptics (`Haptics`) | Done | Mock (the mock logs vibrations) | |
| Controller input lost for 0.3 s: everything held is released, buttons latch until let go | Done | Mock | Stops a runtime stall from leaving the player walking. Not triggered by the game's own hitches. |
| **Game brought to the front automatically** when a controller is used or the headset is put on, and the pause the game opened by itself is closed (`FocusGameWindow`) | Done | Validated (Q3) | Uses the input-queue-attach method. Does not fake an Alt press. Windows can still refuse (for example when an elevated window is in front): then click the game window once. |
| A lone Alt / Alt+Space no longer opens the window's system menu (a modal loop that froze the game) | Done | Code only | |
| Clicks only reach the game's client area (not its frame) | Done | Fixed, unverified | Probable cause of a 6.6 s freeze that ESC ended. Not reproduced. |
| Left-handed mode / any item in either hand | Not done | - | The game ties each item to a fixed hand. |

### 2.4 Hands and arms

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Weapon held in the controller with 6DOF (each draw's matrix is factored and rebuilt; the weapon is identified by its equipment entity) | Done | Validated (Q3) | Pistol, crossbow, sword and mine. |
| Full arms with a two-bone IK from the shoulder to the controller | Done | Validated (Q3) | Anatomical shoulder, arm lengthened to 0.56 m (Played). The elbow depends on learned bone proportions (2-7 cm). |
| Hand with a power placed by anatomy (palm facing the body's midline) | Done | Validated (Q3) | Seen on the left hand. A power in the right hand has never been seen (the right hand normally holds the sword). |
| The hand stays on a held weapon in every state (one learned grip per weapon class, side and skeleton; learned grips ship with the DLL) | Done | Mock (Corvo and Emily, pistol, crossbow, sword) | Measured 0.2-0.3 deg / 0.0 cm (the old code was 80 deg off, with fingers 19 cm off). A weapon never seen before follows the animation until it has been held standing still for about 1.5 s. |
| Empty hand: one fixed open pose, closed into a fist by the grip | Done | Mock | Learned once per session after standing still for 1 s; until then the game's fingers show. Rest curl default 0 (the game's open hand). |
| No hand tremor while running (each arm mesh posed from its own frame) | Done | Played (Q3) | 0.01 mm on every mesh in the mock. |
| Arms survive level loads (palette re-adoption) | Done | Mock, then Played | |
| Shadow Walk claws no longer corrupt the arm analysis | Done | Mock | Before: the hand was 177 deg / 35 cm off for the rest of the session. |
| Gadgets (grenade, mine) rest in the palm | Done | Mock (Emily) | Sometimes drawn very dark in some poses (not investigated). |
| The arm that carries a corpse is hidden | Done | Played (Q3) | |

### 2.5 Weapons

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Sword aims where the right hand points (melee direction from the controller) | Done | Validated (Q3) | The strike starts at the head. |
| Sword gestures: swing the arm to strike, fist by the right ear then thrust = power attack, fist at the left shoulder = block (`SwordGestures`) | Done | Played (Q3); thresholds retuned in Mock | A strike needs a swing of the ARM (hand > 1.3 m/s over 12 cm). Wrist flicks do nothing. The right trigger always works too. The power-attack gesture has only been measured in the mock. |
| Sword hits what the drawn blade touches (contact sweep; characters hit by their skeleton, corpses lying down included) | Done | Played (Q3) on the first version; refinements Mock | Air swings make the swing sound and hit nothing. The game's hit still lands about 0.25 s after the contact (accepted). Walls and static world are not detected. Non-humanoid creatures (rats, dogs) are untested. |
| Pistol and crossbow fire along the drawn barrel, and the reticle ring ends on what the ray hits | Done | Mock (headset verdict not recorded) | The right bottle of three breaks (0.2-0.6 cm). Spread is off by default (exact along the barrel). `vr gunspread 1` restores the game's 1.5-2 deg. |
| Pistol "explosion" cone, muzzle flash, light, sparks and smoke come from the drawn muzzle | Done | Mock | A paper-wad particle still flies out in front of the face (section 3). |
| Bullet and chamber parts sit on the drawn gun; the gun is no longer culled while reloading | Done | Mock | The crossbow's firelock variant is untested. |
| Mines and springrazors placed where the hand aims | Done | Mock | |

### 2.6 Powers, gadgets and throwing

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Powers aim where the hand points | Done | Mock, then Played | |
| Power effects come out of the drawn hand | Done for Far Reach and Blink | Mock | Other powers' effects are unverified. |
| Every power tried, including possession of humans (needs the upgrade) and rats | Done | Mock | Possession: yaw servo on the camera, pitch from the head, level roll, head translation scaled by the host's eye height. The host's arms are not drawn. Shadow Walk: camera only, the game's own arms. |
| Grenades thrown with the hand's velocity (`ThrowGestures`) | Done | Mock | About 0.3 s between letting go and the throw (the game's animation). |
| Power wheel on a panel; the world stays in VR | Done | Validated (Q3) | Picking with the stick sometimes lands on the neighbouring slot (correct it by eye before releasing). |

### 2.7 Grabbing and carrying

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Right-hand pointer ray picks what to take or use (both phases of the game's interaction sensor, filtered ray, 3 m like the flat game) | Done | Mock, then Played | The left hand does not use the stable pointer. |
| Grab with the right grip, carry it with the hand, grip again = drop, right trigger = throw with the hand's velocity (`GrabObjects`) | Done | Played (Q3) (a throw by hand is in a real session log) | Held objects push nothing (this is the game's own design). Grabbing with the left hand reuses the right hand's measurements (mirroring untested). |
| Held objects sit in the drawn fist, fitted by their bounding box | Done | Mock | Unusual shapes are kept "as they stood". |
| Carry a body: hold the grip to pick up, grip again to put it down | Done | Played (Q3) | |
| While carrying a body the interaction sensor aims with the eyes (doors open instead of the body dropping) | Done | Fixed, unverified | `vr carrygaze 0` restores the old behaviour. |

### 2.8 Menus and screens

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Menus and loading screens on a screen fixed in the room (62 deg wide, 2.4 m away). It comes back in front if you look away for 0.8 s or click a stick | Done | Validated (Q3) | `MenuScreen*` in the ini. |
| Pause: the world stops and dims around the screen, no black cut; journal and pause open in the same frame | Done | Validated (Q3) | The frozen backdrop can be up to 0.2 s old. |
| In menus: sticks / A / B / grips / triggers as keys, never the mouse | Done | Played (Q3) | |
| New game's character choice as a menu (stick picks Corvo / Emily, A confirms) | Done | Mock | B does nothing in its confirmation dialog: the game has no "back" there. |
| Black market shop as a menu (cursor hidden, B exits a section and then the shop) | Done | Mock | The shop screen is taller than a menu (62 x about 51 deg). `vr screencfg shopcrop`. |
| Dialogue choices over the world | Done | Mock (skiff dialogue) | |
| Notes and books, the death screen, the spyglass | Not adapted | Untested | Left as before. |
| Mission videos (Bink) on the screen; hold A or X to skip | Done | Mock | Only one mission video and the startup videos were measured. |

### 2.9 Wrist HUD

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Health, mana and the equipped item with its ammo, shown as a watch on the back of the left wrist when you turn it towards you and look at it (`WristHud`, `WristHudSize`) | Done, on by default | Played (Q3) (no formal verdict recorded) | Hidden while the left trigger is held and in menus. While on, it forces the game's HUD option to "always" in memory (the player's own setting comes back when it is switched off). Anchored to the Touch aim pose: placement on other controllers is unverified. |
| The game's interface colours reproduced exactly (the backbuffer is typeless; the UI alternates UNORM and sRGB views) | Done | Mock | |

### 2.10 Cutscenes

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Look around with your head (rotation from the head, position from the cutscene camera; the right stick turns too) | Done | Played (Q3); refinements Mock | |
| Black letterbox bars removed (`CutsceneLetterbox`) | Done | Mock | |
| Hold A or X for about 3 s to skip | Done | Mock | |
| Hands stay on the controllers during cutscenes (`CutsceneArms = 0`) | Default | Played (Q3) | |
| Cutscenes that carry the player (train, skiff): the eye rides the camera and the view turns with the vehicle (`vr cinevehicle 0` turns that off) | Done | Mock | Train turned 67 deg, skiff 154 deg. The skiff's slow final docking turn (about 19 deg) is not followed. |
| Hard landing after a big drop (`CatFall`): no tilt, the head stays in control | Done | Mock | The eye drops 1-1.4 m in about 0.3 s, following the animation. |
| Cutscene modes without arms, `KnockedDown`, the `Dead` state | Treated like cutscenes / untouched | Untested | |

### 2.11 Climbing, sliding, swimming

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Mantle onto tables and ledges: head rotation, the play view's eye | Done | Validated (Q3) | `Vault` and `Climb` are handled by name only and have not been measured. The game sheathes both weapons for about 1 s during a mantle (the game's behaviour). |
| Slide: no 21-degree roll in the headset; the servo follows the camera; B while sprinting always slides | Done | Validated (Q3) | |
| Swimming at the surface counts as play (it used to freeze the view as if it were a menu) | Fixed | Mock (the bug was seen on the Q3) | `SwimMantle` too. |

### 2.12 Takedowns

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Choke, kills from behind, fatalities: the view rotates with the head, the animation keeps the arms and the weapon, a smooth return afterwards | Done | Validated (Q3) | Validated before the cutscene work changed the shared view and servo code. A re-check is pending. |

### 2.13 Robustness

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| Crash log: a fault writes code, address, module+RVA, registers and backtrace to `DishonoredVR.log`; the previous log is kept as `DishonoredVR.prev.log` | Done | In use | Release symbols are kept per build. `tools/symbolize.py` resolves a tester's log. |
| Watchdog: a render-thread stall > 2 s is logged with the OpenXR call it is in, its stack and the focused window | Done | Mock | |
| OpenXR frame thread: the game never blocks inside `xrWaitFrame` (at most a 50 ms wait) | Done | Mock | A stalled compositor no longer freezes the game on the PC. The hourglass itself comes from the runtime and cannot be avoided. |
| Keep-alive frames: during the game's own hitches the headset keeps showing the last picture (`KeepAliveFrames`) | Done | Mock (also ran on SteamVR null) | D3D11 multithread protection is on for the whole session (no measurable cost in the mock). |
| New session when Meta's runtime stops VR and does not resume it within 5 s (`NewSessionAfter`) | Done | Played (Q3): seen working in a real session log | Meta's runtime only. Other runtimes wait for their app. |
| Connection lost (`LOSS_PENDING`) -> everything is shut down and restarted | Done | Played (Q3): worked once in a real session log | |
| Headset connected after the game started; no headset at all | Done | Mock | Retried every 3 s. Instance creation is retried for about 1 minute (so SteamVR is not relaunched for flat players). |
| Runtime asks to exit (`EXITING`: "Quit" in Meta's menu, SteamVR closed) -> flat until the game restarts | By design | Mock | |
| Bounded swapchain waits; `xrEndFrame` failures logged | Done | Mock / Code only | |
| No writes into freed engine objects across level loads | Done | Mock | |
| Crash loading a save with a weapon out | Fixed | Mock | |
| Crash entering the Clockwork Mansion (entity lookup while the world is being replaced) | Fixed | Fixed, unverified | All entity-by-id lookups now go through a guarded wrapper. |
| Freeze of several seconds after every load caused by "unlock everything" | Fixed | Mock | 12.9 s -> 7 ms. |
| Unsupported game build -> message box, no hooks | Done | Tested with a patched build | |
| OpenXR validation layer over a whole session | 0 errors | Mock | |
| Machine diagnostics in the log (GPUs, displays, keyboard layout, runtimes, overlays) | Done | Code only | Laptops with two GPUs get a clear message when the headset wants the other GPU. |

### 2.14 Cheats and settings

| Feature | State | Verification | Notes / known issues |
|---|---|---|---|
| `DishonoredVR.ini` (defaults in code; a missing file, key or value falls back to the default and the log says so) | Done | Mock | Read at startup only. |
| `UnlockEverything` (default 1): every power, upgrade, bone charm crafting, black market upgrade and weapon on every loaded save, with full ammo once | Done | Validated (Q3) (the Heart fix) | It no longer gives the Heart: holding one blocked taking the story Heart in the Void at the start of the game. It no longer switches on infinite ammo or health. Changes saves for real. |
| `InfiniteMana` (default 1) | Done | Fixed, unverified | Fills to at least 100 (a save could bring a lowered cap, which left half a bar). |
| `InfiniteAmmo`, `InfiniteHealth` (default 0) | Done | Mock | |
| `Reticle` (default 0): two pointing lines, a ring where shots land | Done, off | Mock | |
| Developer console (`vr ...` commands), `DVR_autoexec.txt`, status file `DVR_heartbeat.txt` (`WriteStatusFile`) | Tools | - | Press Insert to open the overlay. |

**Release 0.5.0 configuration:** synchronised 3D (`Stereo3D = 1`), `DisableMetaASW = 1`, otherwise as 1.0.0.
**Release 1.0.0 configuration:** mono (`Stereo3D = 0`), `UnlockEverything = 1`, `InfiniteMana = 1`,
`InfiniteAmmo = 0`, `InfiniteHealth = 0`, `Reticle = 0`, `WristHud = 1`, `FocusGameWindow = 1`, `RenderScale = 1`,
`MenuScreen = 1`, `KeepAliveFrames = 1`, `NewSessionAfter = 5`.

---

## 3. Known issues and limitations

Only what is current. Items that later work fixed have been dropped.

### Setup and compatibility
- **Windowed mode only.** In fullscreen or borderless the engine renders at the monitor's size whatever the mod says.
  In borderless the mod keeps its resolution override off on purpose, because enabling it measurably tears the
  picture. The result is a blurry picture. There is no on-screen warning yet.
- **Steam 1.77.9.0 only** (no GOG, Microsoft Store, or *Death of the Outsider*).
- **Only the Quest 3 on Meta's runtime has been played.** SteamVR on a real headset, Virtual Desktop (since 0.1.0),
  PS VR2 and Index are unverified. WMR, Vive and Pico have no interaction profiles. The aim-pose angle of the Index
  and Sense controllers is unmeasured (`ControllerPitchOffset`). The wrist watch is tuned to Touch.
- **Canted-display headsets** (Pimax without parallel projections): the "head" is the left eye's orientation and the
  fov ignores eye rotation, so the picture is expected to come out rotated or with black wedges.
- Swapchain format has no B8G8R8A8 fallback, and swapchain regrow ignores the runtime's maximum (the 4096 cap covers
  every runtime measured).
- **Only one GPU measured** (RTX 3060 laptop). VR at the headset's resolution is about 4x the pixels of the flat
  window. Lower the resolution in the headset's PC app, or set `RenderScale`, if the frame rate is low.
- SteamVR "null" driver: without a room setup, SteamVR's room wizard closes the game after about 5 s. This only
  matters for headless testing.
- **The game must be the window in front on the PC.** `FocusGameWindow` brings it back, but Windows can refuse (for
  example when an elevated window is in front). Then click the game window once.
- `EXITING` from the runtime ("Quit" in Meta's menu, SteamVR closed) leaves the game flat until it is restarted.
- Only one `dinput8.dll` mod can be installed at a time.

### Freezes and sessions
- **Air Link / Link compositor stalls still show the headset's hourglass.** The game no longer freezes on the PC, and
  VR comes back by itself (5 s after Meta's runtime lets go). How often it happens depends on the Wi-Fi link.
- **Two freezes from beta logs are not investigated**: one hang without an hourglass, and one session where the
  whole Quest 3 hung. Both were on a build from before 0.2.0.
- Many "controller lost tracking" events for both controllers at once, without the session stopping, were seen in a
  real session. Not investigated.
- **The last play session's fixes are not verified in game**: the Clockwork Mansion crash, the frame-click freeze,
  doors while carrying a body, and infinite mana at 100.
- A runtime that blocks entirely during the roughly 1 ms between `xrBeginFrame` and `xrEndFrame` would still block the
  render thread (the watchdog would say where).
- Keep-alive frames on SteamVR use the immediate context. Corruption during loads has not been seen but cannot be ruled
  out. If loads look wrong there: `KeepAliveFrames = 0`.
- The render targets are reallocated twice when the session stops and starts (headset off and on).
- Shutting down at process exit still calls OpenXR under the loader lock. It has always worked on Meta and SteamVR.

### Picture
- **The stereo HUD panel is not yet verified on a headset.** It uses the same captured game-UI texture as the wheel and wrist layer, at 2.4 m and 60 degrees wide by default (configurable from 35 to 90). Markers are not attached to their 3D targets. `StereoHud = 0` returns the UI to the per-eye image. The wrist HUD continues to show health, mana and the item.
  While the watch is on, the game's HUD option is forced to "always" in memory.
- **Stereo 3D is experimental**: alternating eyes judder at low frame rates. Moving entities keep a residual of about
  0.3 px (peaks 2-4 px) that mono does not have. Two cvar bugs: switching `vr stereotaa` from 0 back to 1 restores a
  stale `r_skipTemporalAA`, and in mode 0 changing the AA option in the game's menu undoes "TAA off".
- UI blending is in gamma space in the game and in linear space in the compositor. This is exact over dark
  backgrounds and approximate over bright ones (for example the wheel's veil over a window).
- The wheel panel does not fade out on close.
- The "Skip" prompt and the rest of the game's HUD stay fixed to the view.

### Hands, weapons and powers
- **Pistol paper-wad particle**: a mesh particle of the muzzle effect flies out in front of the face for about 0.15 s.
  Its matrix is rebuilt from the camera. Cause not found.
- **Pistol spread** is off by default (shots go exactly along the barrel). Whether to default to the game's spread is
  undecided.
- The sword does not detect walls or static world. The hit lands about 0.25 s after the contact (accepted). Hit
  volumes for non-humanoid creatures are untested.
- Power-wheel stick selection sometimes highlights the neighbouring slot.
- Power effects from the drawn hand are verified only for Far Reach and Blink. A power in the right hand has never been
  seen.
- Possession: the host's arms are not drawn.
- A mantle sheathes both weapons for about 1 s (the game's behaviour).
- Grenades leave about 0.3 s after letting go (the game's animation). Gadgets are sometimes drawn very dark.
- A weapon never seen before needs about 1.5 s held standing still before the hand locks onto it. The empty-hand pose
  is learned again each session.
- Held objects do not push other objects (the game's own design). Left-hand grabbing and grabbing while keeping the
  sword are not done.

### Game flow
- Notes and books, the death screen and the spyglass are not adapted to the menu screen.
- `Vault`, `Climb` and `KnockedDown` are handled by name and have not been measured. Cutscene modes without arms are
  untested.
- Takedowns need a re-check after the cutscene view changes.
- Cheats change saves for real once the game saves or autosaves.

---

## 4. Open work and ideas

From the latest open items and the roadmap. None of it is scheduled.

- **Re-test the last session's fixes** in game: the mansion entry and exit, the door while carrying a body, the ESC
  freeze.
- Borderless: an on-screen warning, or find where the engine reads the monitor size.
- A separate undistorted 16:9 crop for the PC window only (for recording), keeping the full fov in the headset.
- The remaining stereo issues (the residual, the two cvar bugs). Full stereo (two views per frame) is unexplored.
- Pick the power-wheel slot directly from the stick angle.
- Hands: the left hand on the stable pointer, interacting with either hand, the host's arms in possession, the
  pistol's paper wad.
- Sword: walls and static world, an instant hit, its own trail, the trigger aiming head-to-tip.
- More on-world screens (notes, death, spyglass) on the menu screen.
- Controller profiles for WMR / Vive / Pico, support for canted displays, a swapchain format fallback.
- Comfort and settings: an in-VR settings menu, a comfort vignette, changing `RenderScale` without restarting.
- Left-handed mode / any item in any hand (it conflicts with the game's design).

---

## 5. Changelog

No git tags exist. Versions come from `src/CMakeLists.txt`, the package script and the session log.

### 0.5.0 (beta, replaces 1.0.0)
Version numbering reset: 1.0.0 was labelled too early. 0.5.0 is NEWER than 1.0.0 and replaces it; the mod stays in
beta until it has been played on more headsets and runtimes.

- Real 3D, now ON by default (`Stereo3D = 1`; 0 = flat): both eyes are drawn from the same instant of the game (the game's simulation is held for
  the second eye), so depth stays steady while moving; 1.0's alternating 3D (`Stereo3D = 2`) made depth shimmer.
- Each eye renders only its own field of view (about 20% fewer pixels in 3D) and its own anti-aliasing history with
  the right motion of every object; the headset gets each picture with the head pose it was predicted for.
- Fixed from 1.0: in 3D the two eyes could be swapped and the per-eye anti-aliasing silently switched itself off;
  the picture's head pose was one frame late. Opening the pause menu at the wrong instant no longer crashes 3D.
- `DisableMetaASW = 1`: Meta's ASW is turned off for this game on Link / Air Link (it warped the edges of moving hands
  and of the world on fast head turns). Other runtimes are not touched; the README says where their smoothing is.
- Canted displays (Pimax): the head's orientation comes from the headset's view space and each eye keeps its own.
- A play state that draws no arms (such as climbing the tutorial's chain) is no longer read as a menu once the player
  has been seen playing (not yet checked on the chain itself).

### 1.0.0 (public release)
- Crash entering the Clockwork Mansion fixed (entity lookup during a level change).
- Probable cause of a freeze that ESC ended fixed: clicks now only land in the game window's client area. A lone Alt
  no longer opens the window's system menu.
- While carrying a body the interaction sensor aims with the eyes, so doors open instead of the body being dropped.
- Infinite mana fills to at least 100.
- `UnlockEverything` no longer gives the Heart (it blocked the story Heart in the Void) and no longer forces infinite
  ammo or health. New defaults: ammo and health work as in the game.
- `FocusGameWindow`: the game comes to the front when the controllers are used or the headset is put on, and closes
  the pause it opened by itself.

### 0.2.0 (beta, commit `b9f3003`)
- Sharp picture: the engine renders at the headset's resolution (`RenderScale`) instead of stretching the 16:9 window.
  The game's dynamic resolution is held off. Render budget 4096 per side / 16 MP.
- `vr fov auto` became a mode: applied whenever the headset reports its fov (it was lost at startup in 0.1.0).
- Menus, loading screens and the pause on a screen fixed in the room. The power wheel on a panel.
- Wrist HUD (watch on the back of the left wrist).
- Cutscenes you look around in, without letterbox bars, skippable with A / X. Rides on the train and skiff followed.
  Mission videos skippable. The hard landing.
- Mantle and slide take the head's rotation. Swimming at the surface no longer freezes the view.
- Character choice and black market as menus driven by the controllers.
- Per-eye TAA for the experimental stereo mode. A crash loading a save with a weapon out fixed.
- Freeze handling: OpenXR frame thread, keep-alive frames, a new session on Meta's runtime, bounded waits, input
  released on loss, watchdog.
- The multi-second freeze after every load caused by "unlock everything" fixed.
- Tester configuration: `Reticle = 0`, all four cheats on, mono.

### 0.1.0 (private beta)
- First packaged build (zip: `dinput8.dll`, `DishonoredVR.dll`, `DishonoredVR.ini`, README, third-party notices).
  Static C runtime. Supported-build check. Machine diagnostics in the log.
- The core: 6DOF head view, head-following servo, controllers as keyboard and mouse (console-style layout, Y / X
  quick slots, potions in the wheel), full arms with IK, weapons in the hands, sword by gesture and blade contact,
  guns firing along the drawn barrel, powers from the hand, grenades thrown by hand, grabbing / carrying / throwing,
  takedowns from the head, unlock-everything cheats.
- Robustness for other machines: SteamVR startup crash fixed, mouse sensitivity / invert Y, physical key positions and
  the player's bindings, no headset = flat game, headset connected later or reconnected, Index profile, both sticks =
  menu button, OpenXR 1.0 requested.
- Known in this version and fixed in 0.2.0: blurry, "zoomed-in" picture; menus filling the whole view and following
  the head; cutscenes not following the head.

