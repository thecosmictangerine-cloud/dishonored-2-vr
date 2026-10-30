#include "pch.h"
#include "Commands.h"
#include "Watchdog.h"
#include "GameInfo.h"
#include "CVars.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "Game.h"
#include "CameraHook.h"
#include "PlayerViewHook.h"
#include "ViewMatrixHook.h"
#include "XInputHook.h"
#include "KbmInject.h"
#include "Reticle.h"
#include "WristHud.h"
#include "PointerRay.h"
#include "GunRay.h"
#include "HandAimHook.h"
#include "BodyHide.h"
#include "WeaponPose.h"
#include "Watchpoint.h"
#include "GpuTrace.h"
#include "PlayerState.h"
#include "MeleeAim.h"
#include "SwordContact.h"
#include "NpcSkel.h"
#include "SwordGesture.h"
#include "ThrowGesture.h"
#include "BodyView.h"
#include "GrabHand.h"
#include "TestAids.h"
#include "HandFx.h"
#include "BinkVideo.h"
#include "Settings.h"
#include "RenderRes.h"
#include "StereoTaa.h"
#include "StereoSync.h"
#include "MetaAsw.h"
#include "ScreenMode.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <vector>
#include <set>
#include <map>

namespace
{
    LARGE_INTEGER g_LastPoll{}, g_LastBeat{};
    unsigned long long g_Frames = 0;
    uint8_t* g_AimObj = nullptr;   //!< research: camera/aim object found by 'findvt'
    uint8_t* g_AimCpnt = nullptr;  //!< research: aim component found by 'findaim'
    // Auto-activation of the VR package (vr auto 0|1, default 1). The first time a player is being simulated for
    // ~1.5 s, `vr weapon6dof 1` runs by itself, then every line of <game>\DVR_autoexec.txt. Once is enough: the
    // package survives loads and level changes. Not at startup: the flags applied before the logos coincided with a
    // black screen twice (never explained), and at the main menu there is nothing for them to act on.
    std::atomic<int> g_Auto{ 1 };
    bool g_AutoDone = false; int g_AutoStable = 0;

    //! Grenades thrown by hand (ThrowGesture): hold the left trigger (cooks), let go and it leaves with the hand's
    //! velocity. throwcfg <name> <value> tunes it (no args: list); throwtrack <s> [ms] logs the next flight. Out of the
    //! `vr` chain, which is at MSVC's nesting limit (C1061).
    //! The headset's frames when the game or the runtime stalls. Out of the `vr` chain (C1061).
    bool FrameCommand(const std::string& what, std::istringstream& ss)
    {
        // a game hitch on demand: the render thread sleeps <ms> at the next Present (Watchdog)
        if (what == "stall") { int ms = 3000; ss >> ms; Watchdog::Stall(ms); }
        // the OpenXR frame thread: 0 = the headset waits while the game sends no frame (the old hourglass); the render
        // thread's bound on the wait for a frame; a new session after <s> for one the runtime stopped and did not offer
        // back (0 = wait for it, the old way)
        else if (what == "keepalive") { int m = 1; ss >> m; VRRuntime::SetKeepAlive(m); }
        else if (what == "xrwaitbound") { int ms = 50; ss >> ms; VRRuntime::SetWaitBound(ms); }
        else if (what == "xrkick") { float sec = 5.f; std::string all; ss >> sec >> all; VRRuntime::SetKick(sec, all == "all"); }
        else if (what == "d3dmt") { int m = 1; ss >> m; VRRuntime::SetD3DProtect(m != 0); }
        // Meta's ASW for this game (MetaAsw): 1 = off for the app in VR focus (default), 0 = left to Meta
        else if (what == "metaasw") { int m = 1; ss >> m; MetaAsw::SetMode(m); }
        // pose prediction: 0 = the poses of the last frame (before); 1 = located k display periods ahead,
        // for when the image is shown (default); 2 = the learned gap as measured. predictservo: the head-aim servo's
        // target at the next view's display time too. predictmax: the farthest ahead (ms).
        else if (what == "predict") { int m = 1; ss >> m; VRRuntime::SetPredict(m); }
        else if (what == "predictservo") { int m = 1; ss >> m; VRRuntime::SetPredictServo(m != 0); }
        else if (what == "predictmax") { float ms = 120.f; ss >> ms; VRRuntime::SetPredictMax(ms); }
        // the hands drawn from the poses their VIEW was built from (1, default) or from the latest located (0, before)
        else if (what == "handsfromview") { int m = 1; ss >> m; GpuTrace::SetHandsFromView(m != 0); }
        // eye phasing: 1 = the TAA's measured view wins over the frame-order rule (default), 0 = the rule only (before)
        else if (what == "phaseloop") { int m = 1; ss >> m; VRRuntime::SetPhaseClosed(m != 0); }
        // synchronised stereo falls back to mono when no pair reaches the headset (1, default)
        else if (what == "stereohealth") { int m = 1; ss >> m; VRRuntime::SetStereoHealth(m != 0); }
        // the desktop window in synchronised stereo: 0 = the left eye (default), -1 = as presented (alternating eyes)
        else if (what == "mirroreye") { int m = 0; ss >> m; VRRuntime::SetMirrorEye(m); }
        // an unpaired stereo image to its own eye (1, default) or to both as mono (0, before); the frozen backdrop of
        // the menu screen keeps both eyes' pictures of a pair (1, default) or the right one's (0, before)
        else if (what == "unpairedeye") { int m = 1; ss >> m; VRRuntime::SetUnpairedOwnEye(m != 0); }
        else if (what == "backdropeyes") { int m = 1; ss >> m; VRRuntime::SetBackdropEyes(m != 0); }
        // test of phaseloop: the frame-order rule put n views off (measured: +-1 undone within 2 frames)
        else if (what == "phaseoffset") { int d = 0; ss >> d; VRRuntime::SetPhaseOffset(d); }
        // per-eye frustum: 1 = each stereo eye's view turned to its frustum's centre with its own
        // symmetric fov (default), `margin` degrees more on each side (default 0); 2 = the asymmetric matrix (research:
        // breaks the engine's tiled lighting); 0 = every view the symmetric cover of both eyes (before). layerfov: 0 = the
        // eye's native pose and fov, a turned image warped into it (default); 1 = the layer declares each stereo image's
        // own pose and fov (the only way a margin reaches the compositor; Meta's runtime does not honour it: double image)
        else if (what == "eyeproj") { int m = 1; float mg = CameraHook::EyeProjMargin(); ss >> m >> mg; CameraHook::SetEyeProj(m, mg); }
        else if (what == "layerfov") { int m = 0; ss >> m; VRRuntime::SetLayerFov(m); }
        else return false;
        return true;
    }

    //! The HUD on the left wrist (WristHud). Out of the `vr` chain (C1061).
    bool WristCommand(const std::string& what, std::istringstream& ss)
    {
        if (what == "hudprobe") { int n = 1; ss >> n; WristHud::RequestProbe(n); }
        else if (what == "hudshot") { std::string t = "hud"; ss >> t; WristHud::RequestShot(t); }
        else if (what == "hudcap") { int on = 1; ss >> on; WristHud::SetCapture(on != 0); }
        else if (what == "wristhud") { int on = 1; ss >> on; WristHud::SetWatch(on); }
        else if (what == "wristcfg")
        {
            std::string k; float v = 0;
            if (ss >> k >> v) LOGI("cmd vr wristcfg {} {} -> {}", k, v, WristHud::Set(k, v) ? "ok" : "UNKNOWN NAME");
            LOGI("wristcfg: {}", WristHud::Config());
        }
        else return false;
        return true;
    }

    bool ThrowCommand(const std::string& what, std::istringstream& ss)
    {
        if (what == "throw") { int m = 1; ss >> m; ThrowGesture::SetEnabled(m != 0); LOGI("cmd vr throw {}", m); }
        else if (what == "throwcfg")
        {
            std::string k; float v = 0;
            if (ss >> k >> v) LOGI("cmd vr throwcfg {} {} -> {}", k, v, ThrowGesture::Set(k, v) ? "ok" : "UNKNOWN NAME");
            LOGI("throwcfg: {}", ThrowGesture::Config());
        }
        else if (what == "throwtrack") { float sec = 4; int ms = 100; ss >> sec >> ms; ThrowGesture::Track(sec, ms); LOGI("cmd vr throwtrack {} s every {} ms", sec, ms); }
        else if (what == "grab") { int m = 1; ss >> m; GrabHand::SetEnabled(m != 0); }
        else if (what == "grabthrow") { int m = 1; ss >> m; GrabHand::SetThrowMode(m); }
        else if (what == "grabhand") { int h = 1; ss >> h; GrabHand::SetHand(h); }
        else if (what == "grabcal") GrabHand::Calibrate();
        else if (what == "grabarm") { int m = 2; ss >> m; GrabHand::SetShowArms(m); }
        else if (what == "carryarm") { int m = 1; ss >> m; GrabHand::SetCarryHide(m); }
        else if (what == "carrygaze") { int m = 1; ss >> m; GrabHand::SetCarryGaze(m != 0); }
        else if (what == "grabrelease") { int m = 2; ss >> m; GrabHand::SetReleaseMode(m); }
        else if (what == "armforeign") { int m = 1; ss >> m; GpuTrace::SetArmForeign(m); }
        else if (what == "weapongrip")
        {   // vr weapongrip <0|1|2> | forget | forgetall
            std::string a; ss >> a;
            if (a == "forget") GpuTrace::WeaponGripForget(false);
            else if (a == "forgetall") GpuTrace::WeaponGripForget(true);
            else if (!a.empty()) { int m = 1; try { m = std::stoi(a); } catch (...) {} GpuTrace::SetWeaponGrip(m); }
            LOGI("weapongrip: {}", GpuTrace::WeaponGripStats());
        }
        else if (what == "handtrace")
        {   // vr handtrace <frames> [every] | vr handtrace ref
            std::string a; ss >> a;
            if (a == "ref") GpuTrace::HandTraceRef();
            else { int n = 600, e = 6; try { n = std::stoi(a); } catch (...) {} ss >> e; GpuTrace::HandTrace(n, e); }
        }
        else if (what == "grabfist") { int m = 1; ss >> m; GpuTrace::SetGrabFist(m); }
        else if (what == "handrest")
        {   // vr handrest <mode> [curl] [grip] | vr handrest forget
            std::string a; ss >> a;
            if (a == "forget") GpuTrace::ForgetHandRest();
            else { int m = 1, g = -1; float c = -1.f; try { m = std::stoi(a); } catch (...) {} ss >> c >> g; GpuTrace::SetHandRest(m, c, g); }
            LOGI("handrest: {}", GpuTrace::HandRestStats());
        }
        else if (what == "grabreach") { int m = 1; float b = -1; ss >> m >> b; GrabHand::SetReach(m, b); LOGI("grab sensor {}", GrabHand::Sensor()); }
        else if (what == "grabdiag") { int n = 3; ss >> n; GrabHand::Diag(n); }
        else if (what == "swordhit") { int m = 1; ss >> m; SwordContact::SetEnabled(m); }
        else if (what == "swordfilter") { int m = 1; ss >> m; MeleeAim::SetFilterMode(m); }
        else if (what == "bladeaim") { int m = 1; ss >> m; MeleeAim::SetBladeAim(m); }
        else if (what == "swordscan") { float r = 0; ss >> r; SwordContact::Scan(r); }
        else if (what == "swordskel") { int m = 1; ss >> m; NpcSkel::SetMode(m); }
        else if (what == "skelprobe") { std::string h; ss >> h; try { NpcSkel::Probe((uint32_t)std::stoul(h, nullptr, 16)); } catch (...) { LOGI("skelprobe <id hex>"); } }
        else if (what == "swordcfg")
        {
            std::string k; float v = 0;
            if (ss >> k >> v) { const bool ok = k == "boxmargin" ? (MeleeAim::SetBoxMargin(v), true) : SwordContact::Set(k, v); LOGI("cmd vr swordcfg {} {} -> {}", k, v, ok ? "ok" : "UNKNOWN NAME"); }
            LOGI("swordcfg: {}", SwordContact::Config());
        }
        else if (what == "meleeshape") { int n = 40; ss >> n; MeleeAim::ShapeLog(n); }
        else if (what == "sweepmode") { int m = 1; ss >> m; MeleeAim::SetSweepMode(m); }
        else if (what == "contactvel") { int m = 1; ss >> m; MeleeAim::SetContactVel(m); }
        else if (what == "meleeopen") { int ms = 30; ss >> ms; MeleeAim::OpenAt(ms); }
        else if (what == "meleetrace") { int n = 400; ss >> n; MeleeAim::Trace(n); }
        else if (what == "raylog") { int n = 300; ss >> n; PointerRay::Log(n); }
        else if (what == "pointer") { int m = 2; ss >> m; PointerRay::SetMode(m); }
        else if (GunRay::Command(what, ss)) {}
        else if (what == "pickscan") { float y0 = -30, y1 = 30, p0 = -70, p1 = 10, st = 2.5f; int n = 8; ss >> y0 >> y1 >> p0 >> p1 >> st >> n; PointerRay::Scan(y0, y1, p0, p1, st, n); }
        else if (what == "pointercfg")
        {
            std::string k; float v = 0;
            if (ss >> k >> v) LOGI("cmd vr pointercfg {} {} -> {}", k, v, PointerRay::Set(k, v) ? "ok" : "UNKNOWN NAME");
            LOGI("pointercfg: {}", PointerRay::Config());
        }
        else if (what == "grabcfg")
        {
            std::string k; float v = 0;
            if (ss >> k >> v) LOGI("cmd vr grabcfg {} {} -> {}", k, v, GrabHand::Set(k, v) ? "ok" : "UNKNOWN NAME");
            LOGI("grabcfg: {}", GrabHand::Config());
        }
        else return false;
        return true;
    }
    //! The controller layout (KbmInject). Kept out of the `vr` else-if chain, which is at the compiler's nesting limit.
    bool ControlsCommand(const std::string& what, std::istringstream& ss)
    {
        if (what == "potions") { int m = 1; ss >> m; KbmInject::SetPotions(m); LOGI("cmd vr potions {}", m); }
        // Y / X quick slots, potions in the wheel, menu button short = journal (1, default); 0 = the old layout
        else if (what == "quickslots") { int m = 1; ss >> m; KbmInject::SetQuickSlots(m); LOGI("cmd vr quickslots {}", m); }
        else if (what == "autofocus") { int m = 1; ss >> m; KbmInject::SetAutoFocus(m); }
        else if (what == "quickkeys") { std::string k; ss >> k; KbmInject::SetQuickKeys(k); LOGI("cmd vr quickkeys {} -> {}", k, KbmInject::ControlsInfo()); }
        else if (what == "quickhold") { float s = 0.5f; ss >> s; KbmInject::SetQuickHold(s); LOGI("cmd vr quickhold {}", s); }
        else return false;
        return true;
    }
    //! Testing aids (TestAids): the Outsider's mark, powers, infinite mana. Kept out of the `vr` else-if chain,
    //! which is at the compiler's nesting limit.
    bool AidCommand(const std::string& what, std::istringstream& ss)
    {
        if (what != "givemark" && what != "givepower" && what != "giveupgrade" && what != "items" && what != "decls" && what != "giveequip" && what != "giveitem" && what != "ammo" && what != "infitems" && what != "giveall" && what != "autoall" && what != "mana" && what != "hp" && what != "autopowers" && what != "possessview" && what != "shadowarms" && what != "handfx" && what != "handfxlog") return false;
        std::string n; ss >> n;
        if (what == "givemark") { TestAids::GiveMark(); LOGI("cmd vr givemark"); }
        else if (what == "giveupgrade") { TestAids::GiveUpgrade(n); LOGI("cmd vr giveupgrade {}", n); }
        else if (what == "givepower") { TestAids::GivePower(n); LOGI("cmd vr givepower {}", n); }
        else if (what == "items") { TestAids::ListItems(n); LOGI("cmd vr items {}", n); }
        else if (what == "decls") { TestAids::ListDecls(n); LOGI("cmd vr decls {}", n); }
        else if (what == "giveequip") { TestAids::GiveEquip(n); LOGI("cmd vr giveequip {}", n); }
        else if (what == "giveitem") { TestAids::GiveItem(n); LOGI("cmd vr giveitem {}", n); }
        else if (what == "ammo") TestAids::SetInfiniteAmmo(n == "inf" || n == "1" ? 1 : 0);
        else if (what == "infitems") TestAids::SetInfiniteItems(n == "0" || n == "off" ? 0 : 1);
        else if (what == "giveall") { TestAids::GiveAll(); LOGI("cmd vr giveall"); }
        else if (what == "autoall") TestAids::SetAutoAll(n == "0" || n == "off" ? 0 : 1);
        else if (what == "mana" && !n.empty() && n != "inf" && n != "1" && n != "0" && (isdigit((unsigned char)n[0]) || n[0] == '.')) TestAids::SetMana((float)std::atof(n.c_str()));
        else if (what == "mana") TestAids::SetInfiniteMana(n == "inf" || n == "1" ? 1 : 0);
        else if (what == "hp" && !n.empty() && n != "inf" && n != "1" && n != "0" && (isdigit((unsigned char)n[0]) || n[0] == '.')) TestAids::SetHealth((float)std::atof(n.c_str()));
        else if (what == "hp") TestAids::SetInfiniteHealth(n == "inf" || n == "1" ? 1 : 0);
        else if (what == "autopowers") TestAids::SetAutoPowers(n);
        else if (what == "possessview") ViewMatrixHook::SetPossessView(n != "0");
        else if (what == "shadowarms") { PlayerState::SetShadowArms(n != "0"); LOGI("cmd vr shadowarms {}", n); }
        else if (what == "handfx") HandFx::SetMode(n.empty() ? 2 : std::atoi(n.c_str()));   // 0 off, 1 log only, 2 correct
        else if (what == "handfxlog") HandFx::LogReport();
        else return false;
        return true;
    }

    //! Research commands for held objects (HoldObject): out of the main chain, which is at MSVC's nesting limit.
    bool ResearchCommand(const std::string& cmd, std::istringstream& ss)
    {
        if (cmd == "stateobj")
        {
            // Research: stateobj <StateName>: the player's instance of that arkPlayerState (last `this` its methods saw).
            std::string name; ss >> name; void* o = PlayerState::StateObject(name.c_str());
            LOGI("stateobj {} -> {} vtable exe+0x{:X}", name, o, o ? *(uintptr_t*)o - GameInfo::Base() : 0);
        }
        else if (cmd == "holdinfo")
        {
            // Research: the object HoldObject holds (state+0x68 = its spawn id), its world transform, and where that
            // sits in the camera's frame (right, up, forward).
            uint8_t* st = (uint8_t*)PlayerState::StateObject("HoldObject");
            if (!st) { LOGI("holdinfo: HoldObject never ran"); return true; }
            const int id = *(int*)(st + 0x68);
            uint8_t* ent = (uint8_t*)BodyHide::EntityById(id);
            if (!ent) { LOGI("holdinfo: state {} id {} -> no entity", (void*)st, id); return true; }
            const float* o = (const float*)(ent + 0x300); const float* a = (const float*)(ent + 0x30C);
            float co[3], ca[9]; std::string rel;
            if (HandAimHook::RealCamera(co, ca))
            {
                const float d[3] = { o[0] - co[0], o[1] - co[1], o[2] - co[2] };
                // camera axis rows: forward, left, up
                rel = fmt::format(" cam-local fwd {:.3f} left {:.3f} up {:.3f}", d[0] * ca[0] + d[1] * ca[1] + d[2] * ca[2],
                                  d[0] * ca[3] + d[1] * ca[4] + d[2] * ca[5], d[0] * ca[6] + d[1] * ca[7] + d[2] * ca[8]);
            }
            LOGI("holdinfo: state {} id {} entity {} vtable exe+0x{:X} origin ({:.3f},{:.3f},{:.3f}) axis ({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f}) vel ({:.2f},{:.2f},{:.2f}){}",
                 (void*)st, id, (void*)ent, *(uintptr_t*)ent - GameInfo::Base(), o[0], o[1], o[2], a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                 ((float*)(ent + 0x33C))[0], ((float*)(ent + 0x33C))[1], ((float*)(ent + 0x33C))[2], rel);
        }
        else if (cmd == "holdprobe")
        {
            // Research: holdprobe [n]: HandFx logs the next n binds of the held object and the joint queries in them.
            int n = 6; ss >> n;
            uint8_t* st = (uint8_t*)PlayerState::StateObject("HoldObject");
            const int id = st ? *(int*)(st + 0x68) : 0;
            void* ent = BodyHide::EntityById(id);
            HandFx::ProbeEntity(ent ? ent : (void*)1, n);
        }
        else if (cmd == "scanvec")
        {
            // Research: scanvec <x> <y> <z> [tol=0.01] [maxhits=20]: private RW memory holding those three floats.
            float v[3] = {}, tol = 0.01f; int maxHits = 20; ss >> v[0] >> v[1] >> v[2] >> tol >> maxHits;
            int hits = 0; uint8_t* p = nullptr; MEMORY_BASIC_INFORMATION mbi{};
            while (VirtualQuery(p, &mbi, sizeof mbi) && hits < maxHits)
            {
                uint8_t* rb = (uint8_t*)mbi.BaseAddress; p = rb + mbi.RegionSize;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
                if ((uintptr_t)rb > 0x00007FFFFFFFFFFFull) break;
                const float* f = (const float*)rb; const size_t n = mbi.RegionSize / 4;
                for (size_t i = 0; i + 2 < n && hits < maxHits; i++)
                    if (fabsf(f[i] - v[0]) < tol && fabsf(f[i + 1] - v[1]) < tol && fabsf(f[i + 2] - v[2]) < tol)
                    { hits++; LOGI("scanvec: {} = ({:.3f},{:.3f},{:.3f}) [-0x300 vtable? {:#x}]", (void*)&f[i], f[i], f[i + 1], f[i + 2],
                                   i * 4 >= 0x300 ? *(uintptr_t*)((uint8_t*)&f[i] - 0x300) : 0); }
            }
            LOGI("scanvec: {} hits", hits);
        }
        else return false;
        return true;
    }

    void Execute(const std::string& line)
    {
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        if (cmd == "vr") { std::istringstream s2(line); std::string a, b; s2 >> a >> b; if (b == "weapon6dof") g_AutoDone = true; }
        if (cmd.empty()) return;
        if (ResearchCommand(cmd, ss)) return;
        if (cmd == "capture") { std::string tag; ss >> tag; if (tag.empty()) tag = "shot"; VRRuntime::RequestCapture(tag); }
        else if (cmd == "set")
        {
            std::string name; float v = 0; ss >> name >> v;
            bool ok = (v == (int)v) ? CVars::SetInt(name.c_str(), (int)v) : CVars::SetFloat(name.c_str(), v);
            LOGI("cmd set {} {} -> {}", name, v, ok ? "ok" : "unknown cvar");
        }
        else if (cmd == "get")
        {
            std::string name; ss >> name;
            if (!CVars::Address(name.c_str())) LOGI("cmd get {} -> unknown cvar", name);
            else LOGI("cmd get {} -> int {} float {} str \"{}\"", name, CVars::GetInt(name.c_str()), CVars::GetFloat(name.c_str()), CVars::GetString(name.c_str()));
        }
        else if (cmd == "log") { std::string rest; std::getline(ss, rest); LOGI("cmd log:{}", rest); }
        else if (cmd == "exec")
        {
            // Engine console command via idCmdSystem::BufferCommandText (vtable slot 9): queued, run by the main thread.
            std::string rest; std::getline(ss, rest);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            LOGI("cmd exec -> engine: '{}' ok={}", rest, Game::BufferCommandText(rest.c_str()));
        }
        else if (cmd == "watch" || cmd == "watchr")
        {
            // watch aim | watch armr | watch <hex address> [len] [hits]
            std::string what; ss >> what; int len = 4, hits = 6; ss >> len >> hits; std::string scope; ss >> scope;
            uintptr_t addr = 0; uint8_t* cam = (uint8_t*)HandAimHook::CameraObject();
            if (what == "aim" && cam) addr = (uintptr_t)cam + HandAimHook::kAimAxis;
            else if (what == "camorg" && cam) addr = (uintptr_t)cam + HandAimHook::kCamOrigin;
            else if (what == "fporg" && cam) addr = (uintptr_t)cam + HandAimHook::kCamFpOrigin;
            else if (what == "camaxis" && cam) addr = (uintptr_t)cam + HandAimHook::kCamAxis;
            else if (what == "aimorg" && cam) addr = (uintptr_t)cam + HandAimHook::kAimOrigin;
            else if (what == "pitch" && cam) addr = (uintptr_t)cam + HandAimHook::kPitch;
            else if (what == "off") { Watchpoint::Disarm(); return; }
            else if (what == "dump") { Watchpoint::Dump(); return; }
            else addr = strtoull(what.c_str(), nullptr, 16);
            if (!addr) { LOGI("watch: no address (camera object known: {})", (void*)cam); return; }
            Watchpoint::Arm(addr, len, hits, cmd == "watchr", scope == "all");
        }
        else if (cmd == "findaim")
        {
            // Research: find the aim component: an object pointed to from the player whose floats at
            // +0x2C..+0x34 (m_currentAimWorldDir) form a unit vector within 5 deg of the view forward.
            uint8_t* p = (uint8_t*)Game::LocalPlayer(); float o[3], a[9];
            if (!p || !Game::PlayerView(o, a)) { LOGI("findaim: no player"); return; }
            int hits = 0;
            for (uint32_t off = 0; off < 0x6000; off += 8)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(p + off, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                uint8_t* q = *(uint8_t**)(p + off);
                if ((uintptr_t)q < 0x10000 || !VirtualQuery(q, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                if ((uintptr_t)q + 0x60 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) continue;
                const float* d = (const float*)(q + 0x2C);
                float n = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (fabsf(n - 1.f) > 0.01f) continue;
                float dot = d[0] * a[0] + d[1] * a[1] + d[2] * a[2];
                if (dot > 0.996f)
                {
                    hits++; g_AimCpnt = q;
                    const float* pos = (const float*)(q + 0x38); const float* ld = (const float*)(q + 0x20);
                    LOGI("findaim: player+0x{:X} -> {} worldDir=({:.3f},{:.3f},{:.3f}) localDir=({:.3f},{:.3f},{:.3f}) pos=({:.2f},{:.2f},{:.2f})",
                         off, (void*)q, d[0], d[1], d[2], ld[0], ld[1], ld[2], pos[0], pos[1], pos[2]);
                }
            }
            LOGI("findaim: {} hits", hits);
        }
        else if (cmd == "peek" || cmd == "poke")
        {
            // Research: peek <hex addr|gun|aimcp|cam> [+hexoff] [ndwords]   poke <same> [+hexoff] i|f <value>
            std::string what; ss >> what; uintptr_t addr = 0;
            if (what == "gun") addr = (uintptr_t)HandAimHook::GunCpnt();
            else if (what == "aimcp") addr = (uintptr_t)HandAimHook::AimCpnt();
            else if (what == "cam") addr = (uintptr_t)HandAimHook::CameraObject();
            else if (what == "player") addr = (uintptr_t)Game::LocalPlayer();
            else addr = strtoull(what.c_str(), nullptr, 16);
            std::string tok; ss >> tok;
            if (!tok.empty() && tok[0] == '+') { addr += strtoull(tok.c_str() + 1, nullptr, 16); tok.clear(); ss >> tok; }
            MEMORY_BASIC_INFORMATION mbi{};
            if (!addr || !VirtualQuery((void*)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) { LOGI("{}: bad address {:#x}", cmd, addr); return; }
            if (cmd == "peek")
            {
                int n = tok.empty() ? 8 : atoi(tok.c_str()); std::string s;
                for (int i = 0; i < n; i++) { char b[64]; uint32_t u = *(uint32_t*)(addr + 4 * i); snprintf(b, sizeof b, " [%X]=%u/%g", 4 * i, u, *(float*)&u); s += b; }
                LOGI("peek {:#x}:{}", addr, s);
            }
            else
            {
                std::string val; ss >> val;
                if (tok == "f") *(float*)addr = (float)atof(val.c_str()); else *(int32_t*)addr = atoi(val.c_str());
                LOGI("poke {:#x} {} {}", addr, tok, val);
            }
        }
        else if (cmd == "scanptr" || cmd == "findaimcp")
        {
            // Research: scan all committed private RW memory for 8-byte-aligned qwords equal to a value.
            //   scanptr <hex value> [maxhits]     -> logs every holder address
            //   findaimcp                          -> value = camera component; holder must be aimCpnt+0x58
            //                                         (m_cameraCpnt) with a unit vector at aimCpnt+0x2C
            uintptr_t want = 0; int maxHits = 20; bool aim = cmd == "findaimcp";
            if (aim) want = (uintptr_t)HandAimHook::CameraObject(); else { std::string hex; ss >> hex >> maxHits; want = strtoull(hex.c_str(), nullptr, 16); }
            if (!want) { LOGI("{}: no value", cmd); return; }
            int hits = 0; uint8_t* p = nullptr; MEMORY_BASIC_INFORMATION mbi{}; size_t scanned = 0;
            while (VirtualQuery(p, &mbi, sizeof mbi) && hits < maxHits)
            {
                uint8_t* base = (uint8_t*)mbi.BaseAddress; p = base + mbi.RegionSize;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
                if ((uintptr_t)base > 0x00007FFFFFFFFFFFull) break;
                scanned += mbi.RegionSize;
                const uintptr_t* q = (const uintptr_t*)base; size_t n = mbi.RegionSize / 8;
                for (size_t i = 0; i < n && hits < maxHits; i++)
                {
                    if (q[i] != want) continue;
                    uint8_t* holder = (uint8_t*)&q[i];
                    if (aim)
                    {
                        if (i * 8 < 0x58 || (i * 8) + 0x40 > mbi.RegionSize) continue;
                        uint8_t* obj = holder - 0x58; const float* d = (const float*)(obj + 0x2C);
                        float nn = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                        if (!(nn > 0.98f && nn < 1.02f)) continue;
                        hits++; g_AimCpnt = obj;
                        LOGI("findaimcp: aimCpnt {} vtable exe+0x{:X} worldDir=({:.3f},{:.3f},{:.3f}) pos=({:.2f},{:.2f},{:.2f})", (void*)obj,
                             *(uintptr_t*)obj - GameInfo::Base(), d[0], d[1], d[2], ((float*)(obj + 0x38))[0], ((float*)(obj + 0x38))[1], ((float*)(obj + 0x38))[2]);
                        HandAimHook::SetAimCpnt(obj);
                    }
                    else { hits++; LOGI("scanptr: {} holds {:#x}", (void*)holder, want); }
                }
            }
            LOGI("{}: {} hits, scanned {} MB", cmd, hits, scanned >> 20);
        }
        else if (cmd == "calli")
        {
            // Research: calli <rva> [a1 [a2 [a3]]] -> logs rax of exe+rva(a1, a2, a3). Args: decimal int, 0x/long hex
            // pointer, exe+HEX (image-relative), player / cam / aimcp. Runs on the command thread: only for pure lookups.
            std::string rva; ss >> rva; uintptr_t a[3] = {};
            for (int i = 0; i < 3; i++)
            {
                std::string t; if (!(ss >> t)) break;
                if (t == "player") a[i] = (uintptr_t)Game::LocalPlayer();
                else if (t == "cam") a[i] = (uintptr_t)HandAimHook::CameraObject();
                else if (t == "aimcp") a[i] = (uintptr_t)HandAimHook::AimCpnt();
                else if (t.rfind("exe+", 0) == 0) a[i] = GameInfo::Base() + strtoull(t.c_str() + 4, nullptr, 16);
                else if (t.find_first_of("xXabcdefABCDEF") != std::string::npos || t.size() > 9) a[i] = strtoull(t.c_str(), nullptr, 16);
                else a[i] = (uintptr_t)strtoll(t.c_str(), nullptr, 10);
            }
            using F = void* (*)(uintptr_t, uintptr_t, uintptr_t); F f = (F)(GameInfo::Base() + strtoull(rva.c_str(), nullptr, 16));
            void* r = f(a[0], a[1], a[2]);
            LOGI("calli exe+0x{} ({:#x}, {:#x}, {:#x}) -> {}", rva, a[0], a[1], a[2], r);
        }
        else if (cmd == "gpudump")
        {
            std::string tag; ss >> tag; if (tag.empty()) tag = "dump";
            GpuTrace::RequestDump(tag.c_str());
            LOGI("cmd gpudump {}", tag);
        }
        else if (cmd == "gputarget") { std::vector<unsigned> v; unsigned x; while (ss >> x) v.push_back(x); GpuTrace::SetTargets(v); }
        // Hide draws by their vertex shader's DXBC hash (hex, as gpudump prints it); no argument or 0 = none.
        else if (cmd == "gpuskiphash")
        {
            std::vector<uint64_t> v; std::string w;
            while (ss >> w) { uint64_t h = std::strtoull(w.c_str(), nullptr, 16); if (h) v.push_back(h); }
            GpuTrace::SetSkipVsHashes(v);
        }
        // Research: freeze the game's time (g_stopTime 1) this many ms after the next Slash begins, to look at the
        // sword's trail on a still frame. g_runFrames N then steps it.
        else if (cmd == "slashfreeze") { int ms = 250; ss >> ms; MeleeAim::FreezeAfterSlash(ms); LOGI("cmd slashfreeze {} ms", ms); }
        else if (cmd == "gputargetweapon") { GpuTrace::TargetsFromWeapon(); }
        else if (cmd == "gpucb") { GpuTrace::RequestConstantDump(); }
        else if (cmd == "gpushader") { GpuTrace::RequestShaderDump(); }
        else if (cmd == "gpuslot") { int n=1; ss >> n; GpuTrace::SetSlot(n); }
        else if (cmd == "gpuzero") { int f=-1,c=0; ss >> f >> c; GpuTrace::SetZeroRange(f,c); }
        else if (cmd == "gpubind") { int on=1; ss >> on; GpuTrace::SetBind(on != 0); }
        // stage 5: the weapon in 6DOF. 1 = follow the controller, 2 = rebuild the MVP unchanged (the plumbing test:
        // anything visible in mode 2 means the factorisation is wrong, before any pixel has been asked to move).
        else if (cmd == "gpu6dof") { int m=1; ss >> m; GpuTrace::SetSixDof(m); }
        else if (cmd == "gpucal") { GpuTrace::Calibrate(); }
        else if (cmd == "gpuhand") { int h=-1; ss >> h; GpuTrace::SetSixDofHand(h); }
        // Both take an optional leading hand (l/r); with three numbers only, they apply to both hands.
        // The HAND's own grip/trim, for when a power is held: gpuhandgrip/gpuhandtrim, same argument shape.
        else if (cmd == "gputrim" || cmd == "gpugrip" || cmd == "gpuhandtrim" || cmd == "gpuhandgrip")
        {
            std::string first; ss >> first;
            int hand = -1;
            if (first == "l" || first == "L" || first == "0") hand = 0;
            else if (first == "r" || first == "R" || first == "1") hand = 1;
            float a = 0, b = 0, c = 0;
            if (hand < 0) { a = (float)atof(first.c_str()); ss >> b >> c; }
            else ss >> a >> b >> c;
            if (cmd == "gputrim") GpuTrace::SetSixDofOffset(hand, a, b, c);
            else if (cmd == "gpugrip") GpuTrace::SetGrip(hand, a, b, c);
            else if (cmd == "gpuhandtrim") GpuTrace::SetHandOffset(hand, a, b, c);
            else GpuTrace::SetHandGrip(hand, a, b, c);
        }
        else if (cmd == "gpuhandrel") { int n = 20; ss >> n; GpuTrace::RequestHandRelLog(n); }
        else if (cmd == "gpucrouchhold") { int m = 1; ss >> m; GpuTrace::SetCrouchHold(m); }
        else if (cmd == "gpuhandhold") { int m = 1; ss >> m; GpuTrace::SetUseHandHold(m); }
        else if (cmd == "gpugadget") { int on = 2; float m = 0.f; ss >> on >> m; GpuTrace::SetGadget(on, m); }
        else if (cmd == "gpuarmcb") { int n = 16; ss >> n; GpuTrace::SetArmCbDump(n); }
        else if (cmd == "gpudiag") { int n=8; ss >> n; GpuTrace::SetDiag(n); }
        else if (cmd == "gpubasis") { int m=0; ss >> m; GpuTrace::SetBasis(m); }
        // Identify the weapon by where it is drawn rather than by index count (which is specific to one weapon in
        // one save): gpufind lists the near-eye draws, gpunear makes them the target.
        else if (cmd == "gpufind") { float d=2.f; int n=1; ss >> d >> n; GpuTrace::RequestFind(d, n); }
        else if (cmd == "gpunear") { float d=0.f; ss >> d; GpuTrace::SetNear(d); }
        // Separate the weapon from the arms: they are submeshes of one skinned model sharing one matrix, but they
        // are separate draws, and the constants are rewritten per draw.
        else if (cmd == "gpuweapon") { GpuTrace::IdentifyWeapon(); }
        else if (cmd == "gpuonly") { int on=1; ss >> on; GpuTrace::SetOnlyWeapon(on != 0); }
        else if (cmd == "gpuid") { int m=1; ss >> m; GpuTrace::SetIdMode(m); }
        else if (cmd == "gpuanchorr") { float m=0.35f; ss >> m; GpuTrace::SetAnchorRadius(m); }
        else if (cmd == "gpuhold") { int m=1; ss >> m; GpuTrace::SetHoldMode(m); }
        else if (cmd == "gpuview") { float x=0,y=0,z=0; ss >> x >> y >> z; GpuTrace::SetViewOffset(x,y,z); }
        else if (cmd == "gpuskip")
        {
            int on = 1; ss >> on;
            GpuTrace::SetSkip(on != 0);
        }
        else if (cmd == "hideweapon")
        {
            // Identify the weapon's draw calls: dump with it visible, dump with it hidden, diff.
            int hand = 0, on = 1; ss >> hand >> on;
            BodyHide::SetHideWeapon(hand, on != 0);
            LOGI("cmd hideweapon {} {}", hand, on);
        }
        else if (cmd == "scanpose")
        {
            // Research: find the joint palette wherever it lives. Scan every committed private RW page for a
            // 12-float block = orthonormal 3x3 followed by a translation within <radius> of a world point
            // (default: the camera eye). That is what a world-space bone matrix looks like, and it does not
            // assume the palette hangs off the model object (modeljoints found nothing there).
            //   scanpose <x> <y> <z> [radius] [maxhits]
            float wx = 0, wy = 0, wz = 0, radius = 0.5f; int maxHits = 24;
            ss >> wx >> wy >> wz >> radius >> maxHits;
            if (wx == 0 && wy == 0 && wz == 0)
            {
                uint8_t* cam = (uint8_t*)HandAimHook::CameraObject(); if (!cam) { LOGI("scanpose: no camera"); return; }
                const float* eye = (const float*)(cam + HandAimHook::kCamOrigin); wx = eye[0]; wy = eye[1]; wz = eye[2];
            }
            LOGI("scanpose: target=({:.2f},{:.2f},{:.2f}) radius={}", wx, wy, wz, radius);
            auto ortho = [](const float* m) {
                for (int i = 0; i < 9; i++) if (!std::isfinite(m[i])) return false;
                for (int r = 0; r < 3; r++)
                {
                    const float* v = m + r * 3;
                    float n = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                    if (n < 0.98f || n > 1.02f) return false;
                }
                for (int a = 0; a < 3; a++) for (int b = a + 1; b < 3; b++)
                {
                    const float* u = m + a * 3; const float* v = m + b * 3;
                    if (fabsf(u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) > 0.02f) return false;
                }
                return true;
            };
            int hits = 0; uint8_t* p = nullptr; MEMORY_BASIC_INFORMATION mbi{};
            while (VirtualQuery(p, &mbi, sizeof mbi) && hits < maxHits)
            {
                uint8_t* base = (uint8_t*)mbi.BaseAddress; p = base + mbi.RegionSize;
                if ((uintptr_t)base > 0x00007FFFFFFFFFFFull) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                    || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
                const float* f = (const float*)base; size_t n = mbi.RegionSize / 4;
                for (size_t i = 9; i + 3 <= n && hits < maxHits; i++)
                {
                    const float* t = f + i;
                    if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) continue;
                    float dx = t[0] - wx, dy = t[1] - wy, dz = t[2] - wz;
                    if (!(dx * dx + dy * dy + dz * dz <= radius * radius)) continue;
                    if (!ortho(f + i - 9)) continue;
                    hits++;
                    LOGI("scanpose: {} t=({:.3f},{:.3f},{:.3f}) fwd=({:.3f},{:.3f},{:.3f})",
                         (void*)(f + i - 9), t[0], t[1], t[2], f[i - 9], f[i - 8], f[i - 7]);
                }
            }
            LOGI("scanpose: {} hits", hits);
        }
        else if (cmd == "modeljoints")
        {
            // Research: the FP weapon/arm models are skinned, so what places them on screen is the joint matrix
            // buffer, not the transform set (writing/freezing that was measured to change nothing). Find the buffer:
            // walk every pointer inside arkSkinnedRenderModel and report those holding float triplets that sit near
            // the model's own origin — a joint palette in world space looks exactly like that.
            //   modeljoints <hex model addr> [radius m]
            std::string what; ss >> what; float radius = 1.5f; ss >> radius;
            uint8_t* m = (uint8_t*)strtoull(what.c_str(), nullptr, 16);
            MEMORY_BASIC_INFORMATION mbi{};
            if (!m || !VirtualQuery(m, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) { LOGI("modeljoints: bad model {}", (void*)m); return; }
            uint8_t* ts = *(uint8_t**)(m + BodyHide::kModelTransformSet);
            if (!ts) { LOGI("modeljoints: model {} has no transform set", (void*)m); return; }
            const float* org = (const float*)(ts + 0xC4);
            LOGI("modeljoints: model {} origin=({:.2f},{:.2f},{:.2f}) radius={}", (void*)m, org[0], org[1], org[2], radius);
            (void)org; (void)radius;
            // Joints are computed relative to the camera (see ENGINE.md), so their translations are small and
            // world-position matching finds nothing. Detect the rotation part instead: three orthonormal rows.
            auto isMatrix = [](const float* m) {
                for (int i = 0; i < 9; i++) if (!std::isfinite(m[i])) return false;   // NaN passes every comparison
                for (int r = 0; r < 3; r++)
                {
                    const float* v = m + r * 3;
                    float n = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                    if (n < 0.98f || n > 1.02f) return false;
                }
                for (int a = 0; a < 3; a++) for (int b = a + 1; b < 3; b++)
                {
                    const float* u = m + a * 3; const float* v = m + b * 3;
                    if (fabsf(u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) > 0.02f) return false;
                }
                return true;
            };
            int found = 0;
            for (uint32_t off = 0; off + 8 <= 0x2130 && found < 40; off += 8)
            {
                uint8_t* p = *(uint8_t**)(m + off);
                MEMORY_BASIC_INFORMATION b{};
                if (!p || (uintptr_t)p < 0x10000 || !VirtualQuery(p, &b, sizeof b) || b.State != MEM_COMMIT
                    || (b.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                size_t avail = (uintptr_t)b.BaseAddress + b.RegionSize - (uintptr_t)p;
                if (avail < 0x30) continue;
                // try both common joint layouts: 3x4 (12 floats) and 4x4 (16 floats), translation in the last row
                for (int stride : { 12, 16 })
                {
                    size_t maxJ = avail / (stride * sizeof(float)); if (maxJ > 128) maxJ = 128;
                    int hits = 0; int firstHit = -1;
                    for (size_t j = 0; j < maxJ; j++)
                    {
                        const float* mat = (const float*)p + j * stride;
                        if (isMatrix(mat) && !(mat[0] == 1.f && mat[4] == 1.f && mat[8] == 1.f)) { hits++; if (firstHit < 0) firstHit = (int)j; }
                    }
                    if (hits >= 3)
                    {
                        const float* t0 = (const float*)p + (size_t)firstHit * stride + (stride == 12 ? 9 : 12);
                        LOGI("modeljoints: +0x{:X} -> {} stride={} hits={}/{} first={} t=({:.2f},{:.2f},{:.2f})",
                             off, (void*)p, stride, hits, (int)maxJ, firstHit, t0[0], t0[1], t0[2]);
                        found++;
                        break;
                    }
                }
            }
            LOGI("modeljoints: {} candidate buffers", found);
        }
        else if (cmd == "nearmodels")
        {
            // Research: arkSkinnedRenderModel instances whose transform set origin is within <radius> m of the camera eye.
            float radius = 2.f; ss >> radius;
            uint8_t* cam = (uint8_t*)HandAimHook::CameraObject(); if (!cam) { LOGI("nearmodels: no camera"); return; }
            const float* eye = (const float*)(cam + HandAimHook::kCamOrigin);
            uintptr_t want = GameInfo::Base() + BodyHide::kSkinnedModelVtable;
            int hits = 0, total = 0; uint8_t* p = nullptr; MEMORY_BASIC_INFORMATION mbi{};
            while (VirtualQuery(p, &mbi, sizeof mbi))
            {
                uint8_t* base = (uint8_t*)mbi.BaseAddress; p = base + mbi.RegionSize;
                if ((uintptr_t)base > 0x00007FFFFFFFFFFFull) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
                const uintptr_t* q = (const uintptr_t*)base; size_t n = mbi.RegionSize / 8;
                for (size_t i = 0; i < n; i++)
                {
                    if (q[i] != want) continue;
                    uint8_t* m = (uint8_t*)&q[i]; total++;
                    if ((uintptr_t)m + 0x2130 > (uintptr_t)base + mbi.RegionSize) continue;
                    uint8_t* ts = *(uint8_t**)(m + BodyHide::kModelTransformSet); MEMORY_BASIC_INFORMATION m2{};
                    if (!ts || !VirtualQuery(ts, &m2, sizeof m2) || m2.State != MEM_COMMIT || (uintptr_t)ts + 0xD0 > (uintptr_t)m2.BaseAddress + m2.RegionSize) continue;
                    const float* o = (const float*)(ts + 0xC4);
                    float dx = o[0] - eye[0], dy = o[1] - eye[1], dz = o[2] - eye[2];
                    if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                    hits++;
                    const uint64_t* masks = (const uint64_t*)(m + BodyHide::kMeshVisibilityMasks);
                    LOGI("nearmodels: model {} tset {} pos=({:.2f},{:.2f},{:.2f}) numVisible={} masks={:#x} {:#x}", (void*)m, (void*)ts, o[0], o[1], o[2], *(int*)(m + BodyHide::kNumVisibleMeshes), masks[0], masks[1]);
                }
            }
            LOGI("nearmodels: {} near of {} skinned models", hits, total);
        }
        else if (cmd == "armmasks") { std::string tag = "now"; ss >> tag; BodyHide::LogMasks(tag.c_str()); }
        else if (cmd == "scanvt")
        {
            // Research: scanvt <vtRva> [maxhits] -> heap objects whose first qword is that vtable
            std::string hex; int maxHits = 30; ss >> hex >> maxHits; uintptr_t want = GameInfo::Base() + strtoull(hex.c_str(), nullptr, 16);
            int hits = 0; uint8_t* p = nullptr; MEMORY_BASIC_INFORMATION mbi{};
            while (VirtualQuery(p, &mbi, sizeof mbi) && hits < maxHits)
            {
                uint8_t* base = (uint8_t*)mbi.BaseAddress; p = base + mbi.RegionSize;
                if ((uintptr_t)base > 0x00007FFFFFFFFFFFull) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
                const uintptr_t* q = (const uintptr_t*)base; size_t n = mbi.RegionSize / 8;
                for (size_t i = 0; i < n && hits < maxHits; i++) if (q[i] == want) { hits++; LOGI("scanvt: {} has vtable exe+0x{:X}", (void*)&q[i], want - GameInfo::Base()); }
            }
            LOGI("scanvt: {} hits", hits);
        }
        else if (cmd == "stateprobe") { int n = 60; ss >> n; PlayerState::Probe(n); LOGI("cmd stateprobe {}", n); }
        else if (cmd == "meleesites") { MeleeAim::DumpSites(); }
        else if (cmd == "tdlog") { int n = 300; ss >> n; PlayerState::Timeline(n); LOGI("cmd tdlog {}", n); }
        else if (cmd == "findvt")
        {
            // Research: findvt <vtRva> [objHex|player|cam|aimcp] [lenHex]: pointers inside an object (default: the local
            // player, 0x6000 bytes) to objects whose vtable is exe+<rva>.
            std::string hex, objs, lens; ss >> hex >> objs >> lens; uintptr_t want = GameInfo::Base() + strtoull(hex.c_str(), nullptr, 16);
            uint8_t* p = (uint8_t*)Game::LocalPlayer(); uint32_t len = 0x6000;
            if (objs == "cam") p = (uint8_t*)HandAimHook::CameraObject(); else if (objs == "aimcp") p = (uint8_t*)HandAimHook::AimCpnt();
            else if (!objs.empty() && objs != "player") p = (uint8_t*)strtoull(objs.c_str(), nullptr, 16);
            if (!lens.empty()) len = (uint32_t)strtoul(lens.c_str(), nullptr, 16);
            if (!p) { LOGI("findvt: no object"); return; }
            int hits = 0;
            for (uint32_t off = 0; off < len; off += 8)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(p + off, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                uint8_t* q = *(uint8_t**)(p + off);
                if ((uintptr_t)q < 0x10000 || !VirtualQuery(q, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                if ((uintptr_t)q + 8 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) continue;
                if (*(uintptr_t*)q == want) { hits++; LOGI("findvt: player+0x{:X} -> {} has vtable exe+0x{:X}", off, (void*)q, want - GameInfo::Base()); g_AimObj = q; }
            }
            // also: is the player itself that class?
            if (*(uintptr_t*)p == want) { LOGI("findvt: the player object itself has that vtable"); g_AimObj = p; }
            LOGI("findvt: {} hits", hits);
        }
        else if (cmd == "findrv")
        {
            // Research: scan the local player object for pointers to blocks that look like a
            // renderView_t of the player (vieworg near the player's view origin, orthonormal axis).
            uint8_t* p = (uint8_t*)Game::LocalPlayer();
            float o[3], a[9];
            if (!p || !Game::PlayerView(o, a)) { LOGI("findrv: no player"); return; }
            int hits = 0;
            for (uint32_t off = 0; off < 0x4000; off += 8)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(p + off, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                uint8_t* q = *(uint8_t**)(p + off);
                if ((uintptr_t)q < 0x10000 || !VirtualQuery(q, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                if ((uintptr_t)q + 0x160 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) continue;
                const float* vo = (const float*)(q + 0xA8);
                const float* va = (const float*)(q + 0xCC);
                float d = fabsf(vo[0] - o[0]) + fabsf(vo[1] - o[1]) + fabsf(vo[2] - o[2]);
                float n0 = va[0] * va[0] + va[1] * va[1] + va[2] * va[2];
                if (d < 2.0f && fabsf(n0 - 1.0f) < 0.01f)
                {
                    hits++;
                    LOGI("findrv: player+0x{:X} -> {} looks like renderView_t: fov={:.3f}/{:.3f} org=({:.2f},{:.2f},{:.2f}) fwd=({:.2f},{:.2f},{:.2f}) useExplicit={}",
                         off, (void*)q, *(float*)(q + 0x9C), *(float*)(q + 0xA0), vo[0], vo[1], vo[2], va[0], va[1], va[2], (int)*(uint8_t*)(q + 0x140));
                }
            }
            // Also: floats inside the player that equal the view origin (other copies of the camera)
            for (uint32_t off = 0; off < 0x4000; off += 4)
            {
                const float* f = (const float*)(p + off);
                if (fabsf(f[0] - o[0]) < 1e-4f && fabsf(f[1] - o[1]) < 1e-4f && fabsf(f[2] - o[2]) < 1e-4f && off != Game::kPlayerViewOrigin)
                    LOGI("findrv: player+0x{:X} holds a copy of the view origin", off);
            }
            LOGI("findrv: {} renderView candidates", hits);
        }
        else if (cmd == "gpuhandbones")
        {   // gpuhandbones <l|r> [clear] <bone indices or a-b ranges...>: bones of that arm to collapse (not the hand)
            std::string sd; ss >> sd; int side = sd == "r" ? 1 : 0; bool clear = false; std::vector<int> v; std::string tok;
            while (ss >> tok)
            {
                if (tok == "clear") { clear = true; continue; }
                if (tok == "stats") { GpuTrace::LogBoneStats(side); continue; }
                if (tok == "reset") { GpuTrace::ResetBoneStats(side); continue; }
                if (tok == "auto") { float th = 1.0f; ss >> th; GpuTrace::AutoCollapse(side, th); continue; }
                size_t dash = tok.find('-');
                if (dash != std::string::npos) { int a = atoi(tok.substr(0, dash).c_str()), b = atoi(tok.substr(dash + 1).c_str()); for (int i = a; i <= b; i++) v.push_back(i); }
                else v.push_back(atoi(tok.c_str()));
            }
            GpuTrace::SetCollapseBones(side, v, clear);
            LOGI("cmd gpuhandbones {} clear={} n={}", sd, clear, v.size());
        }
        else if (cmd == "gpuhandcut") { float c = 0.22f; ss >> c; GpuTrace::SetHandCut(c); LOGI("cmd gpuhandcut {}", c); }
        else if (cmd == "gpuhandbonesinfo") { std::string sd; ss >> sd; GpuTrace::LogArmBones(sd == "r" ? 1 : 0); }
        else if (cmd == "vbdump") { unsigned n = 14013; std::string sd; ss >> n >> sd; GpuTrace::RequestVertexDump(n, sd == "r" ? 1 : 0); LOGI("cmd vbdump {} {}", n, sd == "r" ? "r" : "l"); }
        // ---- the full arm / body (vr body 1) ----
        else if (cmd == "gpuarminfo") { std::string sd; ss >> sd; GpuTrace::LogArmRig(sd == "r" ? 1 : 0); }
        else if (cmd == "gpuarmseg")
        {   // gpuarmseg <l|r> <auto|hand|fore|upper|root|none> <bone indices or a-b ranges...>: segment of bones of the main palette
            std::string sd, what; ss >> sd >> what; std::vector<int> v; std::string tok;
            while (ss >> tok)
            {
                size_t dash = tok.find('-');
                if (dash != std::string::npos) { int a = atoi(tok.substr(0, dash).c_str()), b = atoi(tok.substr(dash + 1).c_str()); for (int i = a; i <= b; i++) v.push_back(i); }
                else v.push_back(atoi(tok.c_str()));
            }
            const int seg = what == "hand" ? 1 : what == "fore" ? 2 : what == "upper" ? 3 : what == "root" ? 4 : what == "none" ? 5 : 0;
            GpuTrace::SetArmSegment(sd == "r" ? 1 : 0, v, seg);
            LOGI("cmd gpuarmseg {} {} n={}", sd, what, v.size());
        }
        else if (cmd == "gpuarmjoints") { std::string sd; ss >> sd; GpuTrace::ResetArmJoints(sd == "l" ? 0 : sd == "r" ? 1 : -1); LOGI("cmd gpuarmjoints reset {}", sd); }
        else if (cmd == "gpuarmprior") { float f = 0.26f, u = 0.30f; ss >> f >> u; GpuTrace::SetArmPriors(f, u); LOGI("cmd gpuarmprior {} {}", f, u); }
        else if (cmd == "gpushoulder") { float f = -0.06f, o = 0.17f, u = -0.20f; ss >> f >> o >> u; GpuTrace::SetShoulder(f, o, u); LOGI("cmd gpushoulder {} {} {}", f, o, u); }
        else if (cmd == "gpupole") { float f = -0.2f, o = 0.4f, u = -0.8f; ss >> f >> o >> u; GpuTrace::SetPole(f, o, u); LOGI("cmd gpupole {} {} {}", f, o, u); }
        else if (cmd == "dumpmodel")
        {   // dumpmodel <l|r> [idx]: every pointer-looking field of that arm's arkSkinnedRenderModel, with what it
            // points at rendered as text -- the joint NAMES live somewhere off this object.
            std::string sd; int idx = 0; ss >> sd >> idx;
            uint8_t* mdl = (uint8_t*)BodyHide::ArmModel(sd == "r" ? 1 : 0, idx);
            auto readable = [](const void* p, size_t n) { MEMORY_BASIC_INFORMATION mbi{}; return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize; };
            if (!readable(mdl, 0x2200)) { LOGI("dumpmodel: no model ({})", (void*)mdl); return; }
            std::wstring path = GameInfo::GameDir() + L"\\DVR_model_" + (sd == "r" ? L"r" : L"l") + L".txt";
            FILE* fo = _wfopen(path.c_str(), L"w"); if (!fo) return;
            auto text = [&](const uint8_t* p, size_t n, std::string& o) { o.clear(); for (size_t i = 0; i < n; i++) { uint8_t ch = p[i]; o += (ch >= 32 && ch < 127) ? (char)ch : '.'; } };
            std::string t;
            fprintf(fo, "model %p vtable exe+0x%llX\n", (void*)mdl, (unsigned long long)(*(uintptr_t*)mdl - GameInfo::Base()));
            for (size_t off = 0; off < 0x2200; off += 8)
            {
                uintptr_t v = *(uintptr_t*)(mdl + off);
                if (v < 0x10000 || v > 0x7FFFFFFFFFFF || !readable((void*)v, 64)) continue;
                text((const uint8_t*)v, 64, t);
                fprintf(fo, "+%05zX -> %012llX  %s\n", off, (unsigned long long)v, t.c_str());
                // one level down: an array of pointers to strings (a name table)
                std::string names; int cnt = 0;
                for (int k = 0; k < 16; k++)
                {
                    uintptr_t q = *(uintptr_t*)(v + k * 8);
                    if (q < 0x10000 || q > 0x7FFFFFFFFFFF || !readable((void*)q, 32)) break;
                    const char* cs = (const char*)q; int len = 0; while (len < 31 && cs[len] >= 32 && cs[len] < 127) len++;
                    if (len < 2 || cs[len] != 0) break;
                    names += " '" + std::string(cs, len) + "'"; cnt++;
                }
                if (cnt >= 3) fprintf(fo, "         name table? %s\n", names.c_str());
            }
            // deeper: breadth-first over pointers (3 levels, bounded) collecting every printable string. Joint names
            // live somewhere below the mesh/skeleton resources this object points at.
            {
                std::set<uintptr_t> seen; std::vector<uintptr_t> frontier{ (uintptr_t)mdl }; std::set<std::string> strs;
                for (int depth = 0; depth < 4 && !frontier.empty() && seen.size() < 6000; depth++)
                {
                    std::vector<uintptr_t> next;
                    for (uintptr_t node : frontier)
                    {
                        if (!seen.insert(node).second) continue;
                        const size_t span = depth == 0 ? 0x2200 : 0x400;
                        if (!readable((void*)node, span)) continue;
                        for (size_t off = 0; off < span; off += 8)
                        {
                            uintptr_t v = *(uintptr_t*)(node + off);
                            if (v < 0x10000 || v > 0x7FFFFFFFFFFF || !readable((void*)v, 64)) continue;
                            const char* cs = (const char*)v; int len = 0; while (len < 63 && cs[len] >= 32 && cs[len] < 127) len++;
                            if (len >= 4 && cs[len] == 0) strs.insert(std::string(cs, len));
                            else if (next.size() < 4000) next.push_back(v);
                        }
                    }
                    frontier.swap(next);
                }
                fprintf(fo, "\n--- %zu strings within 3 pointer hops (%zu nodes) ---\n", strs.size(), seen.size());
                for (auto& st : strs) fprintf(fo, "%s\n", st.c_str());
                // Ordered name tables: a node holding >= 8 pointers to "*_jnt" strings at a fixed stride is the joint
                // array of a skeleton. Print it IN ORDER with the stride and the pointer path from the model, so the
                // bone index -> name mapping and the offsets to reach it are both on record.
                std::map<uintptr_t, std::pair<uintptr_t, size_t>> parent;   // node -> (parent node, offset in parent)
                std::set<uintptr_t> seen2; std::vector<uintptr_t> fr2{ (uintptr_t)mdl };
                auto isStr = [&](uintptr_t q, std::string& out) {
                    if (q < 0x10000 || q > 0x7FFFFFFFFFFF || !readable((void*)q, 64)) return false;
                    const char* cs = (const char*)q; int len = 0; while (len < 63 && cs[len] >= 32 && cs[len] < 127) len++;
                    if (len < 4 || cs[len] != 0) return false; out.assign(cs, len); return true; };
                for (int depth = 0; depth < 4 && !fr2.empty() && seen2.size() < 6000; depth++)
                {
                    std::vector<uintptr_t> next;
                    for (uintptr_t node : fr2)
                    {
                        if (!seen2.insert(node).second) continue;
                        const size_t span = depth == 0 ? 0x2200 : 0x2000;
                        if (!readable((void*)node, span)) continue;
                        for (size_t stride = 8; stride <= 128; stride += 8)
                        {
                            for (size_t start = 0; start < span && start < stride; start += 8)
                            {
                                std::vector<std::string> names; std::string sname; size_t k = 0;
                                for (; start + k * stride + 8 <= span; k++)
                                {
                                    uintptr_t q = *(uintptr_t*)(node + start + k * stride);
                                    if (!isStr(q, sname)) break;
                                    names.push_back(sname);
                                }
                                if (names.size() >= 8)
                                {
                                    int jnt = 0; for (auto& nm : names) if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, "_jnt") == 0) jnt++;
                                    if (jnt >= 4)
                                    {
                                        fprintf(fo, "\nNAME TABLE at %012llX stride %zu start +%zX count %zu (path from model:", (unsigned long long)node, stride, start, names.size());
                                        uintptr_t cur = node; std::string pathStr;
                                        while (cur != (uintptr_t)mdl && parent.count(cur)) { char b2[64]; snprintf(b2, sizeof b2, " [+%zX]", parent[cur].second); pathStr = b2 + pathStr; cur = parent[cur].first; }
                                        fprintf(fo, "%s)\n", pathStr.c_str());
                                        for (size_t i = 0; i < names.size(); i++) fprintf(fo, "  %3zu %s\n", i, names[i].c_str());
                                    }
                                }
                            }
                        }
                        for (size_t off = 0; off < span; off += 8)
                        {
                            uintptr_t v = *(uintptr_t*)(node + off);
                            if (v < 0x10000 || v > 0x7FFFFFFFFFFF || !readable((void*)v, 64)) continue;
                            std::string tmp; if (isStr(v, tmp)) continue;
                            if (!parent.count(v)) parent[v] = { node, off };
                            if (next.size() < 4000) next.push_back(v);
                        }
                    }
                    fr2.swap(next);
                }
            }
            fclose(fo);
            LOGI("dumpmodel: wrote {}", sd);
        }
        else if (cmd == "cpnt")
        {
            // Research: cpnt <mana|powers|mark|attr|inv|typeinfoRvaHex|@addrHex> [lenHex] [tag] [depth] [childLenHex]:
            // dump the player's component (or the object at an address) to <game>\DVR_cpnt_<tag>.txt.
            std::string what, lens, tag, cl; int depth = 0; ss >> what >> lens >> tag >> depth >> cl;
            const uint32_t len = lens.empty() ? 0x400 : (uint32_t)strtoul(lens.c_str(), nullptr, 16);
            const uint32_t childLen = cl.empty() ? 0x100 : (uint32_t)strtoul(cl.c_str(), nullptr, 16);
            if (!what.empty() && what[0] == '@')
                TestAids::RequestDumpAt((uintptr_t)strtoull(what.c_str() + 1, nullptr, 16), len, tag.empty() ? what.substr(1) : tag, depth, childLen);
            else if (what == "arml" || what == "armr" || what == "armmodelr" || what == "armmodell")
            {   // the first-person arm entity, or its first render model
                const int side = what.back() == 'r' ? 1 : 0;
                void* e = what.size() > 4 ? BodyHide::ArmModel(side, 0) : side ? BodyHide::RightArmEntity() : BodyHide::LeftArmEntity();
                if (e) TestAids::RequestDumpAt((uintptr_t)e, len, tag.empty() ? what : tag, depth, childLen); else LOGI("cpnt {}: none", what);
            }
            else
            {
                uint32_t ti = what == "mana" ? TestAids::kManaCpntTypeInfo : what == "powers" ? TestAids::kPowerSystemCpntTypeInfo
                            : what == "mark" ? TestAids::kOutsiderMarkCpntTypeInfo : what == "attr" ? TestAids::kAttributesCpntTypeInfo
                            : what == "inv" ? BodyHide::kEquipCpntTypeInfo : (uint32_t)strtoul(what.c_str(), nullptr, 16);
                TestAids::RequestDump(ti, len, tag.empty() ? what : tag, depth, childLen);
            }
        }
        else if (cmd == "dumpcmdsys")
        {
            // Research: log the idCmdSystem vtable so we can identify BufferCommandText statically.
            uintptr_t base = GameInfo::Base();
            void** objp = (void**)(base + Game::kCmdSystemPtr);
            uint8_t* obj = (uint8_t*)*objp;
            uintptr_t* vt = obj ? *(uintptr_t**)obj : nullptr;
            LOGI("cmdSystem obj={} vtable=exe+0x{:X}", (void*)obj, vt ? (uintptr_t)vt - base : 0);
            for (int i = 0; vt && i < 24; i++) LOGI("  slot {:2} -> exe+0x{:X}", i, vt[i] - base);
        }
        else if (cmd == "vr")
        {
            std::string what; ss >> what;
            if (AidCommand(what, ss)) return;
            if (ControlsCommand(what, ss)) return;
            if (ThrowCommand(what, ss)) return;
            if (WristCommand(what, ss)) return;
            if (FrameCommand(what, ss)) return;
            if (BodyView::Command(what, ss)) return;
            if (RenderRes::Command(what, ss)) return;
            if (StereoTaa::Command(what, ss)) return;
            if (StereoSync::Command(what, ss)) return;
            if (ScreenMode::Command(what, ss)) return;
            if (KbmInject::Command(what, ss)) return;
            {   // cutscenes
                if (what == "cinearms") { int m = 0; ss >> m; PlayerState::SetCineArms(m != 0); LOGI("cmd vr cinearms {}", m); return; }
                if (what == "cinecontrols") { int m = 1; ss >> m; KbmInject::SetCineControls(m != 0); LOGI("cmd vr cinecontrols {}", m); return; }
                if (what == "cinebars") { int m = 1; ss >> m; WristHud::SetBarsMode(m); LOGI("cmd vr cinebars {}", m); return; }
                if (what == "cinecut") { float d = 1.2f, y = 60.f; ss >> d >> y; ViewMatrixHook::SetCineCut(d, y); LOGI("cmd vr cinecut {} {}", d, y); return; }
                // the development notes: cinetrack 1 = the eye ON the moving cutscene camera (0 = the old 4 m/s follow, A/B);
                // cinevehicle 1 = the view turns with a vehicle the player rides (0 = never)
                if (what == "cinetrack") { int m = 1; ss >> m; ViewMatrixHook::SetCineTrack(m != 0); LOGI("cmd vr cinetrack {}", m); return; }
                if (what == "cinevehicle") { int m = 1; ss >> m; ViewMatrixHook::SetCineVehicle(m != 0); LOGI("cmd vr cinevehicle {}", m); return; }
            }
            if (what == "cam") { int m = 1; ss >> m; CameraHook::SetMode(m); LOGI("cmd vr cam {}", m); }
            else if (what == "pv") { int m = 1; ss >> m; PlayerViewHook::SetEnabled(m != 0); LOGI("cmd vr pv {}", m); }
            else if (what == "vm") { int m = 1; ss >> m; ViewMatrixHook::SetEnabled(m != 0); LOGI("cmd vr vm {}", m); }
            else if (what == "pad") { int m = 1; ss >> m; XInputHook::SetEnabled(m != 0); LOGI("cmd vr pad {}", m); }
            else if (what == "snap") { float d = 45; ss >> d; XInputHook::SetSnapAngle(d); KbmInject::SetSnapAngle(d); LOGI("cmd vr snap {}", d); }
            else if (what == "kbm") { int m = 1; ss >> m; KbmInject::SetEnabled(m != 0); LOGI("cmd vr kbm {}", m); }
            else if (what == "hand") { int m = 1; ss >> m; HandAimHook::SetMode(m); LOGI("cmd vr hand {}", m); }
            else if (what == "armsfollow") { int m = 1; ss >> m; HandAimHook::SetArmsFollow(m != 0); LOGI("cmd vr armsfollow {}", m); }
            else if (what == "motion")
            {   // one switch for the motion-controls package: controller fire ray, hidden arms, weapons anchored + oriented to the controller
                int m = 1; ss >> m; bool on = m != 0;
                HandAimHook::SetMode(on ? 2 : 0); BodyHide::SetHideArms(on); HandAimHook::SetFpAnchor(on ? 2 : 0); HandAimHook::SetArmsFollow(on);
                LOGI("cmd vr motion {}", m);
            }
            else if (what == "weapon6dof")
            {   // the whole 6DOF-weapon package: pick the first-person draws by where they are drawn, rewrite their
                // per-instance MVP from the controller pose, and anchor on the controller's pose right now so that
                // switching it on changes nothing until the hand moves. Arms stay visible: they follow too.
                int m = 1; ss >> m; bool on = m != 0;
                GpuTrace::SetNear(on ? 1.5f : 0.f);
                GpuTrace::SetOnlyWeapon(true);
                GpuTrace::SetIdMode(1);                 // the weapon is whatever belongs to the equipment entity
                GpuTrace::SetHoldMode(1);               // barrel points where the controller points, every session
                GpuTrace::SetSixDof(on ? 1 : 0);
                // The fire ray has to come along, or you aim with the model and the shot leaves from the middle of
                // the view. Same hand for both, so what you see and where it goes agree.
                HandAimHook::SetMode(on ? 2 : 0);
                // Head aim is not optional here, it is what keeps the weapon on screen. The engine culls before we
                // ever see a draw, using the weapon's REAL position in front of the character's body; with the body
                // yaw decoupled from the head, turning away from where the character faces takes the weapon out of
                // the frustum and the draws simply stop arriving. Measured with the head turned and the body still:
                // 26 draws at 0 deg and 45 deg, 6 at 90 deg (pieces missing), 0 from 135 deg on (weapon gone) --
                // exactly the "some parts disappear, then all of it" the user reported. With head aim on it is 26
                // at every angle, 180 deg included. It also makes the movement stick follow the view instead of the
                // character's facing, since W/A/S/D are body-relative.
                KbmInject::SetHeadAim(on);
                ViewMatrixHook::SetKeepGameAxis(on);
                // Takedowns: with the view on the game's axis, a takedown's animated camera glued the world to the
                // face for its whole length. The takedown view takes the rotation from the head instead.
                ViewMatrixHook::SetAnimCam(on ? 2 : 0);
                // The engine anchors the muzzle flash to the weapon's own render transform, and the GPU rewrite
                // never touches that -- so the flash stayed in the middle of the view while the pistol was down in
                // the hand. Feeding the drawn pose back into that transform puts it on the barrel. Mode 1 is the
                // render models only: moving the equipment ENTITY as well (bit 16) hides the weapon from the
                // identification, which matches draws against that entity - measured, 26 draws down to 5.
                WeaponPose::SetEnabled(on ? 1 : 0);
                // The hands: the arm models' draws follow their hand, collapsed to the hand alone. They need the
                // arm models visible, and the torso is hidden because it filled the middle of the view on hardware.
                GpuTrace::SetHandFollow(on ? 1 : 0);
                if (on) { BodyHide::SetHideArms(false); BodyHide::SetHideBody(true); }
                // The body (vr body): full arms reaching the controllers by IK, and the torso, both hung off the HMD.
                // Its own switch so it can be turned off on hardware without losing the rest of the package.
                GpuTrace::SetArmMode(on ? 1 : 0); GpuTrace::SetBodyFollow(on ? 1 : 0);
                if (on) BodyHide::SetHideBody(false);
                // No IdentifyWeapon() here: it is a several-frame scan, and Calibrate() lands on the first draw
                // after it -- in the user's session the weapon anchored 0.5 s BEFORE the scan finished, so the
                // anchor was taken against the wrong set. The entity test needs no scan at all, so anchoring
                // immediately is now the correct order.
                if (on) GpuTrace::Calibrate();
                // Every finished feature is part of the package (the user's rule, 2026-09-30): the per-hand reticle too.
                Reticle::SetMode(on ? 1 : 0);
                // What you see of yourself where it really is (BodyView, 2026-10-04): the game rendered 95 x 59 deg and a
                // relaxed hand sat outside the picture; now the render covers the headset's whole fov, and the picture is
                // drawn at its true angles (VRRuntime::SetOrtho, on by default).
                if (on) { if (!BodyView::FovAuto()) LOGI("vr weapon6dof: headset fov not known yet, render fov applied when it arrives"); }
                else { BodyView::FovAutoOff(); CameraHook::SetFovOverride(0, 0); }
                LOGI("cmd vr weapon6dof {}", m);
            }
            // Takedowns and other scripted animations (see PlayerState::AnimActive): animcam 0|1|2 is the view
            // (ViewMatrixHook::SetAnimCam), animcamspeed <m/s> how fast the eye may follow the animation's position,
            // animarms 0|1 whether the arms go to the animation, animstate <Name> <mask> which states count
            // (1 the view follows the head, 2 the animation takes the arms/weapon/mouse, 4 a cutscene, 8 the servo
            // holds, 16 the player moving: only the rotation is the head's, 32 the servo on the camera's yaw; 0 = as
            // play). Defaults: Mantle/Vault/Climb 25, Sliding 49, CatFall 9.
            else if (what == "animcam") { int m = 2; ss >> m; ViewMatrixHook::SetAnimCam(m); LOGI("cmd vr animcam {}", m); }
            else if (what == "animcamspeed") { float s = 4.0f; ss >> s; ViewMatrixHook::SetAnimCamSpeed(s); LOGI("cmd vr animcamspeed {}", s); }
            else if (what == "animarms") { int m = 1; ss >> m; PlayerState::SetAnimArms(m != 0); LOGI("cmd vr animarms {}", m); }
            else if (what == "animstate")
            {
                std::string n; int m = 3; ss >> n >> m;
                const bool ok = PlayerState::SetAnimState(n.c_str(), m);
                LOGI("cmd vr animstate {} {} -> {} (now: {})", n, m, ok ? "ok" : "unknown state", PlayerState::AnimStates());
            }
            else if (what == "armtest") { int m = 0; ss >> m; HandAimHook::SetArmTest(m); LOGI("cmd vr armtest {}", m); }
            else if (what == "anchorhand") { int m = -1; ss >> m; HandAimHook::SetAnchorHand(m); LOGI("cmd vr anchorhand {}", m); }
            else if (what == "fpanchor") { int m = 1; ss >> m; HandAimHook::SetFpAnchor(m); LOGI("cmd vr fpanchor {}", m); }
            else if (what == "camtest2") { float dz = 0; ss >> dz; HandAimHook::SetCamTest2(dz); LOGI("cmd vr camtest2 {}", dz); }
            else if (what == "camtest") { float dz = 0; ss >> dz; HandAimHook::SetCamTest(dz); LOGI("cmd vr camtest {}", dz); }
            else if (what == "gunhand") { int m = 0; ss >> m; HandAimHook::SetGunHand(m); LOGI("cmd vr gunhand {}", m); }
            else if (what == "powerhand") { int m = 0; ss >> m; HandAimHook::SetPowerHand(m); LOGI("cmd vr powerhand {}", m); }
            else if (what == "poweraim") { int m = 2; ss >> m; HandAimHook::SetPowerAimMask(m); LOGI("cmd vr poweraim {}", m); }
            else if (what == "aimoverride") { int m = 1; ss >> m; HandAimHook::SetAimOverride(m); LOGI("cmd vr aimoverride {}", m); }
            // The sword strikes along the right controller (MeleeAim); meleeorigin: 0 from the head, 1 from the hand.
            else if (what == "meleeaim") { int m = 1; ss >> m; MeleeAim::SetEnabled(m != 0); LOGI("cmd vr meleeaim {}", m); }
            else if (what == "meleeorigin") { int m = 0; ss >> m; MeleeAim::SetOrigin(m); LOGI("cmd vr meleeorigin {}", m); }
            // The sword by gesture (SwordGesture): mask 1 slash, 2 power attack (charge pose + thrust), 4 block
            // (fist at the left shoulder); 0 = trigger and grip only. swingcfg <name> <value> tunes it (no args: list).
            // The sword's trail (the swoosh drawn along the ANIMATED sword): 1 (default) hidden around the swing.
            else if (what == "notrail") { int m = 1; ss >> m; GpuTrace::SetNoTrail(m != 0); LOGI("cmd vr notrail {}", m); }
            else if (what == "swing") { int m = 7; ss >> m; SwordGesture::SetMask(m); LOGI("cmd vr swing {}", m); }
            else if (what == "swingcfg")
            {
                std::string k; float v = 0;
                if (ss >> k >> v) LOGI("cmd vr swingcfg {} {} -> {}", k, v, SwordGesture::Set(k, v) ? "ok" : "UNKNOWN NAME");
                LOGI("swingcfg: {}", SwordGesture::Config());
            }
            else if (what == "meleekey") { int vk = 0; ss >> vk; KbmInject::SetMeleeKey(vk); LOGI("cmd vr meleekey {} (0 = MOUSE1)", vk); }
            else if (what == "swinglog") { int n = 200; ss >> n; SwordGesture::LogSamples(n); LOGI("cmd vr swinglog {}", n); }
            else if (what == "buzz") { int hnd = 1; float a = 0.5f, sec = 0.1f; ss >> hnd >> a >> sec; LOGI("cmd vr buzz {} {} {} -> {}", hnd, a, sec, VRRuntime::Vibrate(hnd, a, sec)); }
            else if (what == "hidearms") { int m = 1; ss >> m; BodyHide::SetHideArms(m != 0); LOGI("cmd vr hidearms {}", m); }
            else if (what == "hands") { int m = 1; ss >> m; GpuTrace::SetHandFollow(m); if (m) BodyHide::SetHideArms(false); LOGI("cmd vr hands {} (hidearms {})", m, BodyHide::HideArms()); }
            else if (what == "handcollapse") { int m = 1; ss >> m; GpuTrace::SetHandCollapse(m); LOGI("cmd vr handcollapse {}", m); }
            else if (what == "hidearmside") { int sd = 0, m = 1; ss >> sd >> m; BodyHide::SetHideArmSide(sd, m != 0); LOGI("cmd vr hidearmside {} {}", sd, m); }
            else if (what == "hidebody") { int m = 1; ss >> m; BodyHide::SetHideBody(m != 0); LOGI("cmd vr hidebody {}", m); }
            else if (what == "body")
            {   // the whole body: full arms with IK to the controllers, and the game's torso, both hung off the HMD.
                // 0 = back to the hand-only mode with the torso hidden.
                int m = 1; ss >> m; const bool on = m != 0;
                GpuTrace::SetArmMode(on ? 1 : 0);
                GpuTrace::SetBodyFollow(on ? 1 : 0);
                BodyHide::SetHideBody(!on);
                if (on) { GpuTrace::SetHandFollow(1); BodyHide::SetHideArms(false); }
                LOGI("cmd vr body {}", m);
            }
            else if (what == "armmode") { int m = 1; ss >> m; GpuTrace::SetArmMode(m); LOGI("cmd vr armmode {}", m); }
            else if (what == "bodyfollow") { int m = 1; ss >> m; GpuTrace::SetBodyFollow(m); LOGI("cmd vr bodyfollow {}", m); }
            else if (what == "weaponpose") { int m = 1; ss >> m; WeaponPose::SetEnabled(m); LOGI("cmd vr weaponpose {}", m); }
            else if (what == "wplog") { int m = 1; ss >> m; WeaponPose::SetResearchLog(m != 0); LOGI("cmd vr wplog {}", m); }
            else if (what == "grip") { int h = 0; float p = 0, y = 0, r = 0, fx = 0, fy = 0, fz = 0; ss >> h >> p >> y >> r >> fx >> fy >> fz; WeaponPose::SetGrip(h, p, y, r, fx, fy, fz); LOGI("cmd vr grip {} rot {} {} {} t {} {} {}", h, p, y, r, fx, fy, fz); }
            else if (what == "stereo")
            {
                // Alternating eyes hand the game's TAA the OTHER eye's frame as its history: measured in the mock, the
                // near hand came out semi-transparent with the table showing through it (the FXAA preset did not
                // help, r_skipTemporalAA 1 did). So stereo used to switch TAA off -- and with it all the game's AA.
                // Since 2026-10-14 each eye has its own TAA (StereoTaa, which owns r_skipTemporalAA): `vr stereotaa 0`
                // = the old way.
                // 1 = alternating eyes (each frame one eye, the other keeps its older image: the eyes see two instants of
                // the world, ghosting in motion); 2 = synchronised (StereoSync: two frames per pair, the second one held,
                // both eyes from one instant and one located head pose, submitted together).
                int m = 1; ss >> m;
                StereoTaa::OnStereoChange(m != 0);
                StereoSync::SetActive(m == 2);
                VRRuntime::SetStereo(m == 1);
                VRRuntime::NoteStereoCommand();
                LOGI("cmd vr stereo {} ({}{})", m, !m ? "mono" : m == 2 ? "synchronised, " : "alternating, ",
                     !m ? "" : StereoTaa::KeepTaaInStereo() ? "each eye its own TAA (StereoTaa)" : "TAA off in stereo (vr stereotaa 0)");
            }
            else if (what == "headaim") { int m = 1; ss >> m; KbmInject::SetHeadAim(m != 0); ViewMatrixHook::SetKeepGameAxis(m != 0); LOGI("cmd vr headaim {}", m); }
            // 0 (default) = the controllers do not touch the game's menus; 1 = they do, the pre-2026-09-15 behaviour.
            else if (what == "auto") { int m = 1; ss >> m; g_Auto = m; LOGI("cmd vr auto {} (done {})", m, g_AutoDone); }
            else if (what == "turnspeed") { float d = 120; ss >> d; KbmInject::SetTurnSpeed(d); LOGI("cmd vr turnspeed {}", d); }
            else if (what == "movepwm") { int m = 0; ss >> m; KbmInject::SetMovePwm(m); LOGI("cmd vr movepwm {}", m); }

            else if (what == "menuinput") { int m = 0; ss >> m; KbmInject::SetMenuInput(m); LOGI("cmd vr menuinput {}", m); }
            // 0 off, 1 both hands, 2 the power hand only, 3 the gun hand only; reticlerange = where the ring sits
            // along the ray when the engine has not computed a landing point (metres).
            else if (what == "reticle") { int m = 1; ss >> m; Reticle::SetMode(m); LOGI("cmd vr reticle {}", m); }
            else if (what == "aimdot") { int m = 1; ss >> m; Reticle::SetAimDot(m != 0); LOGI("cmd vr aimdot {}", m); }
            else if (what == "handaxes") { int m = 1; ss >> m; Reticle::SetAxes(m); LOGI("cmd vr handaxes {}", m); }
            else if (what == "reticlerange") { float r = 8.f; ss >> r; Reticle::SetRange(r); LOGI("cmd vr reticlerange {}", Reticle::Range()); }
            else if (what == "projany") { int m = 1; ss >> m; GpuTrace::SetProjAny(m); }
            else if (what == "reticlemark") { float w[3]; if (ss >> w[0] >> w[1] >> w[2]) Reticle::AddMark(w); else Reticle::ClearMarks(); }
            else if (what == "mcal") { std::string v; ss >> v; if (v == "auto" || v.empty()) KbmInject::Calibrate(); else KbmInject::SetMouseCountsPerDegree((float)atof(v.c_str())); LOGI("cmd vr mcal {}", v); }
            else if (what == "recenter") { CameraHook::Recenter(); ViewMatrixHook::Recenter(); LOGI("cmd vr recenter"); }
            else if (what == "scale") { float s = 1; ss >> s; CameraHook::SetWorldScale(s); ViewMatrixHook::SetWorldScale(s); LOGI("cmd vr scale {}", s); }
            else LOGI("cmd vr unknown: {}", what);
        }
        else LOGI("cmd unknown: {}", line);
    }

    void Heartbeat()
    {
        auto st = VRRuntime::GetStatus();
        std::wstring path = GameInfo::GameDir() + L"\\DVR_heartbeat.txt";
        std::ofstream f(path, std::ios::trunc);
        f << "frames " << g_Frames << "\n"
          << "xr_init " << st.initialized << "\nxr_running " << st.running << "\nxr_state " << st.state
          << "\nxr_frames " << st.framesSubmitted << "\nxr_eye " << st.eyeW << "x" << st.eyeH
          << "\nxr_runtime " << st.runtime << "\nxr_system " << st.system << "\nxr_error " << st.error
          << "\nxr_stereo " << VRRuntime::Stereo() << " mode " << (StereoSync::Active() ? "2 synchronised" : VRRuntime::Stereo() ? "1 alternating" : "0 mono") << " eye " << VRRuntime::CurrentEye() << " presented " << VRRuntime::PresentedEye() << "\n";
        f << RenderRes::Status() << "\n";
        f << StereoTaa::Status() << "\n";
        f << StereoSync::Status() << "\n";
        f << MetaAsw::Status() << "\n";
        f << VRRuntime::SyncStatus() << "\n";
        f << VRRuntime::PredictStatus() << "\n";
        f << CameraHook::EyeProjStatus() << " | layerfov " << VRRuntime::LayerFov() << "\n";
        f << ScreenMode::Status() << "\n";
        f << Watchdog::Status() << "\n";
        f << VRRuntime::PumpStatus() << "\n";
        float o[3], a[9];
        if (Game::PlayerView(o, a))
        {
            f << "cam_pos " << o[0] << " " << o[1] << " " << o[2] << "\n";
            f << "cam_fwd " << a[0] << " " << a[1] << " " << a[2] << "\n";
            f << "cam_left " << a[3] << " " << a[4] << " " << a[5] << "\n";
            f << "cam_up " << a[6] << " " << a[7] << " " << a[8] << "\n";
        }
        else f << "cam_pos none\n";
        {
            auto hs = HandAimHook::GetStats();
            f << "hand_calls " << hs.calls << " mode " << HandAimHook::Mode() << " gunhand " << HandAimHook::GunHand() << " getaim " << HandAimHook::GetAimCalls()
              << " aimpoint " << hs.aimPointCalls << "/" << hs.aimPointApplied << " target " << hs.targetCalls << "/" << hs.targetApplied
              << " ctrl_org " << hs.ctrlOrg[0] << " " << hs.ctrlOrg[1] << " " << hs.ctrlOrg[2]
              << " ctrl_fwd " << hs.ctrlFwd[0] << " " << hs.ctrlFwd[1] << " " << hs.ctrlFwd[2]
              << " last_aimpoint " << hs.lastAimPoint[0] << " " << hs.lastAimPoint[1] << " " << hs.lastAimPoint[2] << "\n";
            f << "poweraim override " << HandAimHook::AimOverride() << "/" << hs.aimOverrides << " mask " << HandAimHook::PowerAimMask() << " hand " << HandAimHook::PowerHand() << " power " << hs.powerCalls << "/" << hs.powerApplied << " throw " << hs.throwCalls << "/" << hs.throwApplied << " place " << hs.placeCalls << "/" << hs.placeApplied << " search " << hs.searchCalls << "/" << hs.searchApplied << " grapple " << hs.grappleCalls << "/" << hs.grappleApplied
              << " org " << hs.powerOrg[0] << " " << hs.powerOrg[1] << " " << hs.powerOrg[2] << " fwd " << hs.powerFwd[0] << " " << hs.powerFwd[1] << " " << hs.powerFwd[2] << "\n";
            f << "poweraim_target active " << hs.powerActive << " hit " << hs.powerHit[0] << " " << hs.powerHit[1] << " " << hs.powerHit[2] << " dest " << hs.powerTarget[0] << " " << hs.powerTarget[1] << " " << hs.powerTarget[2]
              << " end " << hs.powerEnd[0] << " " << hs.powerEnd[1] << " " << hs.powerEnd[2] << "\n";
            f << "shakecalls " << hs.shakeCalls << " gpu_draws " << GpuTrace::Draws()
              << " gpu_frames " << GpuTrace::Frames() << " gpu_on " << GpuTrace::Installed()
              << " gpu_swap " << GpuTrace::SwapStats() << "\n";
        f << "gpu_shadercache " << GpuTrace::ShaderCache() << "\n";
            f << "gpu6dof " << GpuTrace::SixDofStats() << "\n";
            f << "gpu6dof_weapondraws " << GpuTrace::WeaponDraws() << "\n";
            f << "gpuhands " << GpuTrace::HandStats() << "\n";
            f << "gpubody " << GpuTrace::BodyStats() << "\n";
            f << "handjit " << GpuTrace::HandJitStats() << "\n";

            f << "pstate " << PlayerState::Stats() << "\n";
            f << "meleeaim " << MeleeAim::Stats() << "\n";
            f << "sword " << SwordGesture::Stats() << "\n";
            f << "swordhit " << SwordContact::Stats() << "\n";
            f << "throw " << ThrowGesture::Stats() << "\n";
            f << "grab " << GrabHand::Stats() << "\n";
            f << "pointer " << PointerRay::Stats() << "\n";
            f << "gunray " << GunRay::Stats() << "\n";
            f << "grabfist " << GpuTrace::FistStats() << "\n";
            f << "handrest " << GpuTrace::HandRestStats() << "\n";
            f << "weapongrip " << GpuTrace::WeaponGripStats() << "\n";
            f << "aids " << TestAids::Stats() << "\n";
            f << "handfx " << HandFx::Stats() << "\n";
            f << "trail " << GpuTrace::TrailStats() << "\n";
            f << "animcam " << ViewMatrixHook::AnimStats() << " armsuspend " << GpuTrace::AnimSuspended() << " suspendeddraws " << GpuTrace::AnimSuspendedDraws()
              << " animarms " << PlayerState::AnimArms() << " states " << PlayerState::AnimStates() << "\n";
            f << "fpanchor " << HandAimHook::FpAnchor() << " anchorhand " << HandAimHook::AnchorHand()
              << " rvcalls " << hs.rvCalls << " matched " << hs.rvMatched << " applied " << hs.rvApplied << " posefail " << hs.rvPoseFail
              << " org " << hs.rvOrg[0] << " " << hs.rvOrg[1] << " " << hs.rvOrg[2]
              << " fwd " << hs.rvFwd[0] << " " << hs.rvFwd[1] << " " << hs.rvFwd[2] << "\n";
            auto bh = BodyHide::GetStats();
            f << "hide arms " << BodyHide::HideArms() << " body " << BodyHide::HideBody() << " models_zeroed " << bh.modelsHidden << " numvis body/L/R " << bh.numVisibleBody << " " << bh.numVisibleLeft << " " << bh.numVisibleRight << " larm " << bh.leftArm << " rarm " << bh.rightArm
              << " stale " << bh.staleModels << " playerchanges " << bh.playerChanges << "\n";
            auto wp = WeaponPose::GetStats();
            f << "weaponpose " << WeaponPose::Enabled() << " calls " << wp.calls << " applied L/R " << wp.appliedL << " " << wp.appliedR << " ents " << wp.lastL << " " << wp.lastR << " tset " << wp.tsetCalls << "/" << wp.tsetApplied << "\n";
            f << "aimcp " << HandAimHook::AimCpnt() << " gun "<< HandAimHook::GunCpnt() << " calls " << hs.gunCalls << " spawn " << hs.gunSpawn[0] << " " << hs.gunSpawn[1] << " " << hs.gunSpawn[2] << "\n";
        }
        if (g_AimCpnt)
        {
            const float* d = (const float*)(g_AimCpnt + 0x2C); const float* pos = (const float*)(g_AimCpnt + 0x38);
            f << "aimcpnt_dir " << d[0] << " " << d[1] << " " << d[2] << " pos " << pos[0] << " " << pos[1] << " " << pos[2] << "\n";
        }
        if (uint8_t* pl = g_AimObj ? g_AimObj : (uint8_t*)HandAimHook::CameraObject())
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(pl + 0x25400, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
            {
                const float* ao = (const float*)(pl + 0x2527C); const float* aa = (const float*)(pl + 0x25288);
                const float* ra = (const float*)(pl + 0x25328); const float* la = (const float*)(pl + 0x25304);
                f << "aim_org " << ao[0] << " " << ao[1] << " " << ao[2] << " fwd " << aa[0] << " " << aa[1] << " " << aa[2] << "\n";
                f << "arm_r_fwd " << ra[0] << " " << ra[1] << " " << ra[2] << " arm_l_fwd " << la[0] << " " << la[1] << " " << la[2] << "\n";
                f << "cam_pitch " << *(const float*)(pl + 0x25264) << " yaw " << *(const float*)(pl + 0x25274) << " armsPitch " << *(const float*)(pl + 0x25268) << "\n";
            }
        }
        {
            auto cs = CameraHook::GetStats();
            f << "rv_calls " << cs.calls << " main " << cs.mainViewCalls << " applied " << cs.applied << " refreshes " << cs.refreshes << " mode " << CameraHook::Mode() << "\n"
              << "rv_org " << cs.lastOrg[0] << " " << cs.lastOrg[1] << " " << cs.lastOrg[2] << "\n"
              << "rv_fwd " << cs.lastFwd[0] << " " << cs.lastFwd[1] << " " << cs.lastFwd[2] << "\n"
              << "rv_fov " << cs.lastFovX << " " << cs.lastFovY << " size " << cs.lastW << "x" << cs.lastH << "\n";
            auto ps = PlayerViewHook::GetStats();
            f << "pv_calls " << ps.calls << " applied " << ps.applied << " on " << PlayerViewHook::Enabled()
              << " org " << ps.lastOrg[0] << " " << ps.lastOrg[1] << " " << ps.lastOrg[2]
              << " fwd " << ps.lastFwd[0] << " " << ps.lastFwd[1] << " " << ps.lastFwd[2] << "\n";
            auto vs = ViewMatrixHook::GetStats();
            f << "vm_calls " << vs.calls << " main " << vs.mainCalls << " applied " << vs.applied << " on " << ViewMatrixHook::Enabled()
              << " org " << vs.lastOrg[0] << " " << vs.lastOrg[1] << " " << vs.lastOrg[2]
              << " fwd " << vs.lastFwd[0] << " " << vs.lastFwd[1] << " " << vs.lastFwd[2] << "\n";
            auto xs = XInputHook::GetStats();
            f << "pad_calls " << xs.calls << " synth " << xs.synthesized << " on " << XInputHook::Enabled()
              << " L " << xs.lx << " " << xs.ly << " R " << xs.rx << " " << xs.ry << " T " << xs.lt << " " << xs.rt << " btn " << xs.buttons << "\n";
            auto ks = KbmInject::GetStats();
            f << "kbm_ticks " << ks.ticks << " input " << ks.inputTicks << " fg " << ks.fgTicks << " keys " << ks.keyEvents << " mouse " << ks.mouseEvents
              << " held 0x" << std::hex << ks.heldMask << std::dec << " on " << KbmInject::Enabled() << " lastdx " << ks.lastDx << " maxturn " << ks.maxTurnX << " headaim " << KbmInject::HeadAim() << " yawerr " << ks.yawError << " pitcherr " << ks.pitchError << " bodyyaw " << ks.bodyYaw << " headyaw " << ks.headYaw << " sim " << ks.simulating << " menuinput " << KbmInject::MenuInput() << " menusafe " << ks.menuSafe << " fpdraws " << ks.fpDraws
              << " mode " << (ks.mode == 0 ? "play" : ks.mode == 1 ? "menu" : "wheel") << " sprint " << ks.sprint << " states " << PlayerState::RunningCount() << " possess " << ks.possessing << " scene " << ks.scene << " playrule " << ks.playRule << " latched " << ks.playLatched << "\n";
            f << KbmInject::ControlsInfo() << "\n";
            f << BinkVideo::Status() << "\n";
            {
                auto rs = Reticle::GetStats();
                f << "reticle mode " << Reticle::Mode() << " axes " << Reticle::Axes() << " range " << Reticle::Range() << " frames " << rs.frames
                  << " drawn " << rs.drawn << " handvalid " << rs.handValid << " projvalid " << rs.projValid
                  << " enginetarget " << rs.engineTarget << "\n";
            }
            f << WristHud::Status() << "\n";
            f << WristHud::WatchStatus() << "\n";
            auto hp = VRRuntime::GetHeadPose();
            f << "hmd_pos " << hp.pos[0] << " " << hp.pos[1] << " " << hp.pos[2] << " quat " << hp.quat[0] << " " << hp.quat[1] << " " << hp.quat[2] << " " << hp.quat[3] << " valid " << hp.valid << "\n";
            {   // hand poses straight from the runtime: an invalid hand is why the FP anchor stops applying (posefail)
                XRInputState in; bool got = VRRuntime::GetInput(in);
                f << "input got " << got;
                for (int i = 0; i < 2; i++)
                    f << (i == 0 ? " L " : " R ") << in.hand[i].posX << " " << in.hand[i].posY << " " << in.hand[i].posZ
                      << " q " << in.hand[i].quatX << " " << in.hand[i].quatY << " " << in.hand[i].quatZ << " " << in.hand[i].quatW
                      << " valid " << in.hand[i].valid << " held " << in.hand[i].held;
                f << " lostframes L " << VRRuntime::LostFrames(0) << " R " << VRRuntime::LostFrames(1) << " head " << VRRuntime::LostFrames(2) << " recenters " << VRRuntime::RecenterEvents();
                f << "\n";
            }
        }
        for (auto& e : CVars::Known()) f << "cvar " << e.name << " " << CVars::GetString(e.name) << "\n";
    }
}

void Commands::OnPresent()
{
    g_Frames++;
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    if ((now.QuadPart - g_LastPoll.QuadPart) * 10 >= fq.QuadPart)
    {
        g_LastPoll = now;
        // Legacy single file + sequence-numbered files (DVR_cmd_<n>.txt, processed in name order,
        // so rapid successive commands are never overwritten/reordered).
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((GameInfo::GameDir() + L"\\DVR_cmd*.txt").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { files.push_back(GameInfo::GameDir() + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(files.begin(), files.end());
        for (auto& path : files)
        {
            std::ifstream f(path);
            if (!f) continue;   // being written right now; retry next poll
            std::string line;
            while (std::getline(f, line)) Execute(line);
            f.close();
            DeleteFileW(path.c_str());
        }
    }
    {   // Recentre the view's position reference: when the runtime recentres (the Meta button held), and when a new
        // player appears (a load or a level change). The reference used to be taken once, from the first pose of the
        // session -- usually the user sitting at the desk starting the game -- so standing up and walking to the play
        // space left the eye that far from the character's for the whole session.
        static unsigned lastRecenter = 0; static unsigned long long lastPlayer = 0; static bool init = false;
        const unsigned rc = VRRuntime::RecenterEvents();
        const unsigned long long pc = BodyHide::GetStats().playerChanges;
        if (!init) { lastRecenter = rc; lastPlayer = pc; init = true; }
        if (rc != lastRecenter || pc != lastPlayer)
        {
            LOGI("recenter: {} (runtime recenters {}, player changes {})", rc != lastRecenter ? "the runtime recentred" : "a new player", rc, pc);
            CameraHook::Recenter(); ViewMatrixHook::Recenter(); KbmInject::ResetHeadRef();
            lastRecenter = rc; lastPlayer = pc;
        }
    }
    {   // The player's settings that do not need a game (turning, the button layout): once, on the first frame.
        static bool startup = false;
        if (!startup) { startup = true; for (const std::string& c : Settings::StartupCommands()) { LOGI("settings: {}", c); Execute(c); } }
    }
    if (g_Auto.load() && !g_AutoDone)
    {
        float o[3], a[9];
        const bool inGame = PlayerState::RunningCount() > 0 && KbmInject::GetStats().simulating && Game::PlayerView(o, a);
        // Only with the headset's session RUNNING. Without a headset (none connected, no OpenXR runtime, the runtime
        // refused us) the package would hide the body, pin the hands to controllers that do not exist and hand the
        // view to a head pose that never comes: the game must stay the normal flat game. If the headset comes up
        // later (Link connected after the game started), the package switches on then.
        const VRRuntime::Status xr = VRRuntime::GetStatus();
        static unsigned long long waitedNoXr = 0;
        if (inGame && !xr.running && waitedNoXr++ == 600)
            LOGI("auto: in a game, but the headset's OpenXR session is not running (initialized {}, state {}{}): the game stays flat "
                 "and the VR package switches on when the headset is there", xr.initialized, xr.state, xr.error.empty() ? "" : ", " + xr.error);
        g_AutoStable = inGame && xr.running ? g_AutoStable + 1 : 0;
        if (g_AutoStable >= 90)
        {
            LOGI("auto: a player is being simulated ({} states running) - switching the VR package on", PlayerState::RunningCount());
            if (const size_t vs = GpuTrace::ShaderCount(); vs < 50)
                LOGI("auto: WARNING only {} vertex shaders were seen being created: the mod hooked D3D11 after the game had "
                     "made them, so the arms and weapons cannot be recognised and the controllers will act as in a menu. "
                     "Restart the game.", vs);
            Execute("vr weapon6dof 1");
            CameraHook::Recenter(); ViewMatrixHook::Recenter(); KbmInject::ResetHeadRef();
            for (const std::string& c : Settings::PackageCommands()) { LOGI("auto (settings): {}", c); Execute(c); }
            std::ifstream f(GameInfo::GameDir() + L"\\DVR_autoexec.txt");
            std::string line;
            while (f && std::getline(f, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty() && line[0] != '#') { LOGI("auto: {}", line); Execute(line); } }
        }
    }
    if (now.QuadPart - g_LastBeat.QuadPart >= fq.QuadPart) { g_LastBeat = now; if (Settings::StatusFile()) Heartbeat(); }
}

