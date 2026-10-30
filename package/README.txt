================================================================================================
  DISHONORED 2 VR  -  version {VERSION}  -  by The Cosmic Tangerine
================================================================================================

A mod that plays Dishonored 2 in a PC VR headset: the view follows your head, your hands are
your controllers (the sword strikes where you swing it, the pistol and crossbow fire where they
point, powers come out of your hand), and you can grab and throw things with your hands.

It is a free, open-source, unofficial fan mod, not affiliated with or endorsed by Arkane Studios
or Bethesda. Source code, updates and bug reports: {REPO_URL}
Support the development: patreon.com/CosmicTangerine  -  ko-fi.com/cosmictangerine


------------------------------------------------------------------------------------------------
 1. WHAT YOU NEED
------------------------------------------------------------------------------------------------

 * Dishonored 2 on STEAM, updated (Steam always installs the latest version).
   NOT supported: the GOG or Microsoft Store / Game Pass versions, and "Death of the Outsider".
   (On those, the mod shows a message at startup and stays off; the game runs normally.)
 * Windows 10 or 11, 64-bit, and a PC that can run VR.
 * A PC VR headset:
     - Meta Quest 2 / 3 / Pro with Link or Air Link ......... tested (Quest 3)
     - Meta Quest with Virtual Desktop (VDXR) ............... should work
     - Meta Quest through SteamVR ........................... should work
     - PlayStation VR2 through SteamVR (PC adapter) ......... experimental (see "PS VR2" below)
     - Valve Index and other SteamVR headsets ............... experimental
 * Nothing else to install. (No Visual C++ redistributable, no launcher, no script loader.)


------------------------------------------------------------------------------------------------
 2. INSTALL
------------------------------------------------------------------------------------------------

 1. In Steam: right-click Dishonored 2 > Manage > Browse local files.
    A folder opens; it contains Dishonored2.exe.
 2. Copy these files from the zip INTO THAT FOLDER, next to Dishonored2.exe:
        dinput8.dll
        DishonoredVR.dll
        DishonoredVR.ini
 3. That's it. If Windows or your antivirus deletes dinput8.dll or DishonoredVR.dll, restore them
    and add an exception: mods like this one load into the game, which antivirus programs
    sometimes dislike.

 Already using another mod that comes with its own dinput8.dll? Only one can be there.

 UNINSTALL: delete dinput8.dll, DishonoredVR.dll, DishonoredVR.ini, DishonoredVR*.log and the
 DVR_*.txt files from the game folder. The game itself is never modified.


------------------------------------------------------------------------------------------------
 3. ONE-TIME SETUP: THE "OPENXR RUNTIME"
------------------------------------------------------------------------------------------------

 The mod talks to your headset through OpenXR. Windows has ONE "active OpenXR runtime" at a time,
 and it must be the app your headset uses right now. This is the most common reason for "nothing
 shows up in the headset".

   Quest with Link / Air Link:  Meta Horizon Link app > Settings > General >
                                 OpenXR Runtime: "Set Meta Horizon Link as active"
   Quest with Virtual Desktop:  Virtual Desktop Streamer > OpenXR Runtime: VDXR
   SteamVR (PS VR2, Index...):  SteamVR > Settings > OpenXR > "Set SteamVR as OpenXR Runtime"

 GAME OPTIONS (Options > Visuals in the game):
   - Display mode: WINDOWED. Not fullscreen and not borderless: in those two the game renders at
     your monitor's size and the headset gets a blurry picture.
   - The game's own resolution setting does NOT change what you see in VR: the mod renders the
     game at the resolution your HEADSET asks for. For a sharper or faster picture change the
     resolution in the headset's PC app (Meta Horizon Link app: the headset's graphics
     preferences, render resolution; Virtual Desktop: Streaming tab; SteamVR: Settings > Video),
     or RenderScale in DishonoredVR.ini (section 6). Graphics quality (shadows, textures...)
     works as usual.
   - Leave the key bindings as you like them (the mod reads them); mouse sensitivity and
     "invert mouse" are handled by the mod.


------------------------------------------------------------------------------------------------
 4. PLAYING
------------------------------------------------------------------------------------------------

 1. Connect the headset (Link / Air Link / Virtual Desktop / SteamVR running).
 2. Start Dishonored 2 from Steam as usual. When it is running, CLICK THE GAME'S WINDOW ON THE PC
    ONCE so it is the window in front (see the box below), then put the headset on.
 3. Menus show on a big screen in front of you, fixed in the room: look around it freely. Use
    the controllers: sticks move, A selects, B goes back, grips change tab, triggers change
    sub-tab, a stick click brings the screen back in front of you. Loading screens show on the
    same screen.
 4. Load or start a game. A moment after you are in control of your character the VR mode
    switches on by itself: your hands appear and the view follows your head.
    Cutscenes happen around you: look wherever you like with your head (the right stick turns
    too). To skip one, hold A or X for about 3 seconds (the game's "Skip" ring fills up).

 +--------------------------------------------------------------------------------------------+
 | IMPORTANT: THE GAME MUST BE THE WINDOW IN FRONT ON THE PC (focused) WHILE YOU PLAY.        |
 | The mod turns the controllers into the game's keyboard and mouse, and Windows only gives   |
 | those to the window in front. With another window in front (browser, Discord, Steam...)    |
 | the controllers do nothing and the game pauses. The mod brings the game back to the front  |
 | by itself when you press a controller button or put the headset on, and closes that pause  |
 | (FocusGameWindow in DishonoredVR.ini). If Windows does not let it, click the game's window |
 | on the PC once (or Alt+Tab to it).                                                         |
 +--------------------------------------------------------------------------------------------+

 No headset connected? The game runs as the normal flat game. If you connect the headset
 later, VR switches on by itself. If the connection drops while playing, VR should come back by
 itself when the headset reconnects.

 Recenter the view with your headset's own recenter (Quest: hold the Meta button).


------------------------------------------------------------------------------------------------
 5. CONTROLS
------------------------------------------------------------------------------------------------

 IN GAME
   Left stick ........................ move.  Click: sprint (on until you stop)
   Right stick ....................... turn (smooth; snap in DishonoredVR.ini).  Click: zoom
   Right trigger ..................... sword / right-hand weapon
   Swing your RIGHT ARM .............. strike with the sword (what the blade touches is hit)
   Fist by your right ear, blade
     forward, then thrust ............ power attack (lower the blade to cancel it)
   Fist at your left shoulder ........ block (the right grip blocks too)
   Left trigger ...................... power or left-hand weapon (pistol, crossbow) - they fire
                                       where your hand points
   Grenades / springrazors ........... hold the left trigger, throw with your hand, let go
   Right grip, pointing at something . take / use / open it (hold it to pick up a body).
                                       Holding an object: grip again = drop, right trigger = throw
                                       Carrying a body: grip again = put it down
   Right grip, pointing at nothing ... block
   Left grip (hold) .................. power wheel - pick with the stick, let go to equip
                                       (it opens on a panel in front of you; the world stays put)
       ...with the wheel open: B ..... drink a health elixir
                               A ..... drink a mana elixir
                               Y / X . assign the highlighted item to a quick slot
   A ................................. jump
   B ................................. crouch (toggle)
   Y ................................. quick slot 1 (hold 0.5 s: slot 3 - the controller buzzes)
   X ................................. quick slot 2 (hold 0.5 s: slot 4)
   Menu button (left controller) ..... short press: journal.  Hold: pause menu (save / load).
                                       The world stops and dims around the menu screen
   BOTH STICKS PRESSED TOGETHER ...... same as the menu button (for controllers without one)
   Look at the back of your LEFT
     wrist, like checking a watch .... health, mana and the equipped item with its ammo
                                       (hidden while the left trigger is held)
   Lean, peek, crouch for real ....... just do it with your body: the view follows your head.
                                       There is no lean button.

 IN CUTSCENES
   Your head ......................... look around (the scene happens around you; a train or a
                                       boat you ride turns you with it)
   Right stick ....................... turn
   A or X, held about 3 s ............ skip the cutscene (also a mission's opening video)
   Menu button ....................... journal / pause, as in game

 With Reticle = 1 in DishonoredVR.ini (off in this build), two lines show where each hand points:
 the RIGHT one is what the right grip takes or uses, the LEFT one is where a power or shot goes,
 with a ring where it will land.
 The red left-hand aim dot is controlled separately: [Display] AimDot = 1 (0 to turn it off).
 It follows the gun ray, a picked object, an active power landing point, or the left hand's aim direction.
 StereoHudWidth (35 - 90 degrees, default 60) controls how wide the stereo HUD appears; lower values bring edge notifications inward.

 IN MENUS
   Sticks: move   A: select   B or menu: back   Grips: next tab   Triggers: previous / next sub-tab
   Click either stick: the screen comes in front of you (it also does by itself when you look
   away from it for a moment)
   The new game's character choice and a black market's counter are menus too, on the same
   screen: left / right picks Corvo or Emily, A confirms ("Play as ...? No / Yes"); in a shop
   A buys, the grips switch Ammunition / Items, B goes back out.

 VALVE INDEX (experimental)
   The left controller's A / B act as X / Y. Menu button: press the left trackpad down hard.

 PS VR2 (Sense controllers through SteamVR)
   Square / Triangle are X / Y, Cross / Circle are A / B. SteamVR does not pass the Create button
   to the game: use BOTH STICKS PRESSED TOGETHER for the journal and the pause menu, or assign
   Create in SteamVR > Settings > Controllers > Manage Controller Bindings > Dishonored 2 >
   "Menu: journal (short) / pause (hold)".


------------------------------------------------------------------------------------------------
 6. SETTINGS
------------------------------------------------------------------------------------------------

 DishonoredVR.ini (in the game folder) - open it with Notepad. Each setting is explained there.
 Changes apply the next time you start the game. 1 = on, 0 = off. Deleting a line (or the whole
 file) gives you the default back. The values in this build's file:

   [Controls]  TurnMode = smooth (or snap)       SmoothTurnSpeed = 120 (deg/s)  SnapTurnAngle = 45
               QuickSlotHoldTime = 0.5 (s)       Haptics = 1 (vibration)
               Layout = default (the controls above; "classic" = Y leans, X interacts,
                                 menu button pauses)
               ControllerPitchOffset = 0 (tilts the hands up/down, degrees)
               FocusGameWindow = 1 (the game is brought back to the front when you use the controllers)
   [Gameplay]  SwordGestures = 1   ThrowGestures = 1   GrabObjects = 1
   [Display]   Reticle = 0 (1 = the two pointing lines) WristHud = 1  WristHudSize = 8.5 (cm)
               StereoHud = 1 (stereo: one fused HUD panel 2.4 m ahead; 0 = game screen-space HUD)
               DisableMetaASW = 1 (Link / Air Link: Meta's ASW off for this game, see section 7)
               DisableMotionBlur = 1   Stereo3D = 1 (real depth, each eye drawn at the same instant,
                                 about half the frame rate; 0 = flat, both eyes the same
                                 picture; 2 = the old alternating 3D of version 1.0)
               RenderScale = 1 (sharpness: 1 = what the headset asks for, lower = faster)
               MenuScreen = 1   MenuScreenSize = 62 (deg wide)   MenuScreenDistance = 2.4 (m)
               MenuBackground = 0.22 (how bright the paused world is around the menu screen)
               CutsceneLetterbox = 0 (1 = keep the black bars)   CutsceneArms = 0 (1 = the
                                 cutscene moves your hands, not the controllers)
   [Cheats]    UnlockEverything = 1   InfiniteMana = 1   InfiniteAmmo = 0   InfiniteHealth = 0
   [Advanced]  LogControllerInput = 1   WriteStatusFile = 0
               KeepAliveFrames = 1 (while the game stops for a moment, the headset keeps the last
                                 picture instead of an hourglass)
               NewSessionAfter = 5 (s: VR restarts by itself when the headset's app stops it;
                                 Link / Air Link only)

 TWO CHEATS ARE ON by default, so every power and weapon can be tried in VR right away:
 UnlockEverything (every power, upgrade, bone charm crafting, black market upgrade and weapon, on
 every save you load, with full ammo once) and InfiniteMana. Ammo, gadgets, elixirs and health
 work as in the game (InfiniteAmmo / InfiniteHealth = 1 to change that). The cheats change your
 game for real: once the game saves or autosaves, the save keeps them. Use saves you do not mind
 changing, or set them to 0 before playing your own.

 Not in the file, always on: the view follows your head (and the left stick walks where you
 look), the hands and arms follow your controllers, and takedowns keep the view on your head.
 DisableMotionBlur also unticks motion blur in the game's own options (it stays off afterwards;
 tick it again there if you go back to the flat game).


------------------------------------------------------------------------------------------------
 7. IF SOMETHING GOES WRONG
------------------------------------------------------------------------------------------------

 Nothing in the headset
   - Is your headset's app the active OpenXR runtime? (section 3)
   - Was the headset connected and its PC app running? (Connecting later also works.)
   - Open DishonoredVR.log in the game folder and look for "OpenXR session not started (...)":
     the text in brackets says why.

 The game does not start at all on a PC with an AMD graphics card
   - Dishonored 2 itself (also without the mod) fails to start on some AMD setups and needs a
     replacement of its AMD AGS library (amd_ags_x64.dll). This is the game, not the mod.

 "Dishonored 2 VR is NOT active: this game build is not supported" at startup
   - Only the current Steam version of Dishonored 2 is supported (section 1).

 The controllers do nothing / the triggers, grips or the power wheel do nothing / the game paused
 by itself
   - The game window lost focus (section 4): another window is in front of it on the PC. Click
     the game's window on the PC once (or Alt+Tab to it), then put the headset back on.
   - A small window from the mod appeared on the PC screen and the game ignores the mouse and
     keyboard: the Insert key opens it. Press Insert again to close it.

 Hands and weapons point too high or too low compared to your real controllers
   - DishonoredVR.ini > ControllerPitchOffset (degrees, + = up). Try steps of 10.

 A laptop with two graphics cards: VR does not start, log mentions "different GPU"
   - Windows Settings > System > Display > Graphics > Dishonored2.exe > High performance.

 An hourglass in the headset / the picture freezes
   - The headset's app stopped taking frames (an Air Link / Link hiccup). The game keeps running
     on the PC and VR comes back by itself: on Link / Air Link at most NewSessionAfter seconds
     after the headset's app lets go; on SteamVR / Virtual Desktop when their app resumes.
     If it happens often on Air Link: 5 or 6 GHz Wi-Fi, the router close and in
     sight, nothing else downloading -- or a Link cable.
   - Please send DishonoredVR.log (section 8): it says what was stuck ("Watchdog: STALL").

 Low frame rate
   - Lower the resolution in SteamVR / the Meta Horizon app / Virtual Desktop, or set
     DishonoredVR.ini > RenderScale = 0.8. Lower the game's graphics settings. Stereo3D = 1
     draws the world twice per frame (about half the frame rate): use 0 if it stutters.

 Warped, "glitchy" edges around your hands or the world when you move fast
   - Your headset's app is inventing frames because the game runs below the headset's refresh.
     Meta (Link / Air Link): the mod turns ASW off for this game (DisableMetaASW = 1); if it is
     still on, Ctrl+Numpad1 on the PC keyboard turns it off. SteamVR: Settings > Video > Per-
     application video settings > Dishonored 2 > Motion Smoothing off. Virtual Desktop: its
     streaming settings > SSW off. Pimax: Pimax Play > Smart Smoothing off.

 Menus too small / too big, or too close
   - DishonoredVR.ini > MenuScreenSize (degrees: 70 = bigger) and MenuScreenDistance.

 Blurry picture
   - The game must be in a window (Options > Visuals > Display mode: windowed), not fullscreen
     and not borderless (both render at your monitor's size).
   - Raise the resolution in SteamVR / the Meta Horizon app / Virtual Desktop, or RenderScale.


------------------------------------------------------------------------------------------------
 8. REPORTING A PROBLEM  (thank you!)
------------------------------------------------------------------------------------------------

 Send, right after it happened (before starting the game again if you can):
   - DishonoredVR.log and DishonoredVR.prev.log from the game folder
     (if they are not there: %LOCALAPPDATA%\DishonoredVR)
   - your headset, and how it is connected (Link, Air Link, Virtual Desktop, SteamVR)
   - what you did, what you expected, what happened (a photo or video helps a lot)
 The log contains your PC's hardware and the mod's state; it does not contain personal data
 beyond your Windows user name in file paths.


------------------------------------------------------------------------------------------------
 Third-party software in this mod: see THIRD-PARTY-NOTICES.txt.
------------------------------------------------------------------------------------------------

