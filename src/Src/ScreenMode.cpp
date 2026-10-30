#include "pch.h"
#include "ScreenMode.h"
#include "XRSession.h"
#include "KbmInject.h"
#include "WristHud.h"
#include "VRRuntime.h"
#include "Settings.h"
#include "Game.h"
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{
    constexpr float kD2R = 0.01745329f, kR2D = 57.29578f;
    std::atomic<int> g_Mode{ 1 };
    std::atomic<bool> g_Flat{ false }, g_Wheel{ false }, g_Lift{ false };
    std::atomic<bool> g_RecenterReq{ false };
    std::mutex g_M;                            // the config and the state below (render thread + the command thread)

    struct Cfg
    {
        float size = 62.f;          // deg: the screen's width seen from where it is placed
        float dist = 2.4f;          // m
        float pitchMin = -30.f, pitchMax = 15.f;   // deg: it is placed at the head's pitch within these
        float follow = 40.f;        // deg: the gaze this far from the screen's centre (its edges are at ~31)...
        float followDelay = 0.8f;   // s: ...this long, and it comes back in front (0 = never)
        float glide = 0.45f;        // s: how long a move takes
        float fadeIn = 0.22f;       // s: the screen and its surroundings in
        float fadeOut = 0.28f;      // s: the game back in from black after a menu
        float backdrop = 0.22f;     // brightness of the frozen game picture around it (0 = none)
        float env = 1.f;            // brightness of the environment
        float glow = 1.f;           // the screen's light spill on the surroundings
        float corner = 0.018f;      // corner radius, fraction of the screen's height
        float density = 28.f;       // px per degree of the screen's swapchain (sampled by the compositor at display resolution)
        int wheel = 1;              // the power wheel on a panel; 0 = left in the picture
        float wheelSize = 72.f;     // deg: width of the game's whole interface frame on the wheel's panel (the ring is ~40% of it)
        float wheelDist = 1.5f;     // m
        float wheelPitchMin = -40.f, wheelPitchMax = 30.f;
        float wheelFade = 0.12f;    // s
        int shopCrop = 1;           // a black market's screen: 0 = the band, 1 = from the band's top to the frame's bottom, 2 = all
        int enterFrames = 2;        // frames of menu mode before the screen comes up (a one-frame flicker is not a menu)
        int exitFrames = 2;         // frames of play before it goes
    } g_C;

    enum Kind { kPlay = 0, kScreen = 1, kWheel = 2 };
    const char* KindName(int k) { return k == kScreen ? "screen" : k == kWheel ? "wheel" : "play"; }
    Kind g_Kind = kPlay, g_PendKind = kPlay; int g_Pending = 0;
    int g_Scene = 0;                           // KbmInject's menu over the world (2 = a black market)
    float g_LastCrop[4] = {};                  // what of the picture the screen showed last (px)

    //! Where a panel is: the eye it was placed for, the direction and distance from that eye.
    struct Place { float eye[3] = {}; float yaw = 0, pitch = 0, d = 2.4f; bool valid = false; };
    Place g_Cur, g_From, g_To; float g_GlideT = -1.f;   // the screen, and its move (-1: not moving)
    Place g_WheelAt; float g_WheelAlpha = 0.f;
    float g_ScreenAlpha = 0.f, g_SnapFade = 1.f;
    bool g_FromPlay = false;                   // this screen came straight out of play (its frozen picture is the game a moment ago)
    float g_LastBackdrop = 0.f, g_ExitFrom = 0.f;   // the frozen picture's brightness on the last screen frame / where the game fades back in from
    double g_AwaySince = -1.0;                 // the gaze has been off the screen since (s)
    bool g_ShowedScreen = false; double g_GameSince = -100.0;
    double g_LastGame = -100.0;                // the last frame that showed the game (s)
    int g_FlatOnly = 0;                        // frames shown as the screen only because the picture was rendered flat
    void* g_Player = nullptr;
    unsigned g_RecenterGen = 0;
    double g_Last = 0.0;
    std::atomic<double> g_ExpectUntil{ 0.0 };   // a menu asked for by the controllers is expected until (s)
    std::string g_ExpectWhat;
    struct St { unsigned long long frames = 0, screenFrames = 0, flatOnly = 0, entries = 0, exits = 0, wheels = 0, wheelPanels = 0,
                recenters = 0, follows = 0, freezes = 0, noFreeze = 0, snapDropped = 0, expects = 0, expectMissed = 0; std::string lastWhy; } g_S;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Ease(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
    float Wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
    void QRot(const float q[4], const float v[3], float o[3])
    {
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }
    //! XR direction (x right, y up, -z ahead) of a yaw (about +Y, + = left) and a pitch (+ = up).
    void Dir(float yaw, float pitch, float o[3]) { o[0] = -sinf(yaw) * cosf(pitch); o[1] = sinf(pitch); o[2] = -cosf(yaw) * cosf(pitch); }
    //! The quad's orientation: yaw, then pitch about its own X; its +Z faces back along the direction it is in.
    void Quat(float yaw, float pitch, float q[4])
    {
        const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
        q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;
    }
    //! The head now: the eyes' midpoint, the head's own orientation (XRSession::Head: not the left eye's, which a
    //! headset with canted displays turns ~10 deg out).
    bool Head(XRSession& xr, float p[3], float q[4])
    {
        if (xr.EyeCount() < 1 || !xr.Eye(0).valid) return false;
        const XREyePose& h = xr.Head();
        p[0] = h.posX; p[1] = h.posY; p[2] = h.posZ;
        q[0] = h.quatX; q[1] = h.quatY; q[2] = h.quatZ; q[3] = h.quatW;
        return true;
    }
    //! In front of the head: its yaw, its pitch within [pmin, pmax], `dist` away.
    Place Ahead(const float p[3], const float q[4], float dist, float pmin, float pmax)
    {
        const float fl[3] = { 0, 0, -1 }; float f[3]; QRot(q, fl, f);
        Place o; memcpy(o.eye, p, 12);
        o.yaw = atan2f(-f[0], -f[2]);
        o.pitch = std::clamp(asinf(std::clamp(f[1], -1.f, 1.f)), pmin * kD2R, pmax * kD2R);
        o.d = dist; o.valid = true;
        return o;
    }
    void Centre(const Place& pl, float c[3]) { float d[3]; Dir(pl.yaw, pl.pitch, d); for (int i = 0; i < 3; i++) c[i] = pl.eye[i] + d[i] * pl.d; }
    Place Lerp(const Place& a, const Place& b, float t)
    {
        Place o; o.valid = true;
        for (int i = 0; i < 3; i++) o.eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * t;
        o.yaw = a.yaw + Wrap(b.yaw - a.yaw) * t; o.pitch = a.pitch + (b.pitch - a.pitch) * t; o.d = a.d + (b.d - a.d) * t;
        return o;
    }
    void GlideTo(const Place& to, const char* why)
    {
        g_From = g_Cur; g_To = to; g_GlideT = 0.f; g_AwaySince = -1.0;
        LOGI("ScreenMode: screen moves in front ({}): yaw {:.0f} -> {:.0f}, pitch {:.0f} -> {:.0f} deg", why, g_Cur.yaw * kR2D, to.yaw * kR2D, g_Cur.pitch * kR2D, to.pitch * kR2D);
    }

    //! Where the game lays its menus out: a 16:9 band of the picture (measured on the title, pause, load, options and
    //! journal screens at 2266x2348: every item inside the centred band of height W*9/16).
    void Band(unsigned W, unsigned H, float r[4])
    {
        if ((float)H * 16.f >= (float)W * 9.f) { r[2] = (float)W; r[3] = (float)W * 9.f / 16.f; r[0] = 0.f; r[1] = ((float)H - r[3]) * 0.5f; }
        else { r[3] = (float)H; r[2] = (float)H * 16.f / 9.f; r[1] = 0.f; r[0] = ((float)W - r[2]) * 0.5f; }
    }

    //! What of the picture goes on the screen. A menu: the band. A black market lays its interface out against the
    //! frame's EDGES instead: its item carousel sits on the bottom edge of the 2266x2348 frame (rows
    //! ~2000-2280, measured), below the band, and the band alone showed the shopkeeper and no shop. So its screen runs
    //! from the band's top down to the frame's bottom (shopCrop 1): the same width, and so the same text size, as a menu.
    void Crop(unsigned W, unsigned H, float r[4])
    {
        Band(W, H, r);
        if (g_Scene != 2 || g_C.shopCrop == 0) return;
        if (g_C.shopCrop == 2) { r[0] = 0.f; r[1] = 0.f; r[2] = (float)W; r[3] = (float)H; }
        else r[3] = (float)H - r[1];
    }

    //! The kind the controls are in, once it has held for its frames.
    Kind Decide(XRSession& xr)
    {
        Kind want = kPlay;
        if (g_Mode.load() != 0 && xr.IsSessionRunning() && KbmInject::Enabled())
        {
            const KbmInject::Stats ks = KbmInject::GetStats();
            const int m = ks.mode;
            g_Scene = ks.scene;
            want = m == 1 ? kScreen : m == 2 ? kWheel : kPlay;
            // the controllers just sent the pause / journal key: the menu is coming, take it now
            const double until = g_ExpectUntil.load();
            if (until > 0.0)
            {
                if (m == 1) g_ExpectUntil = 0.0;                         // recognised: from here on as any menu
                else if (Now() < until) { want = kScreen; g_PendKind = kScreen; g_Pending = 1000; }
                else { g_ExpectUntil = 0.0; g_S.expectMissed++; LOGI("ScreenMode: no menu came after the {}: back to play", g_ExpectWhat); }
            }
        }
        if (want == g_Kind) { g_Pending = 0; return g_Kind; }
        if (want != g_PendKind) { g_PendKind = want; g_Pending = 0; }
        const int need = want == kScreen ? g_C.enterFrames : g_Kind == kScreen ? g_C.exitFrames : 0;
        return ++g_Pending > need ? want : g_Kind;
    }
}

bool ScreenMode::FlatRender() { return g_Flat.load(); }
bool ScreenMode::WheelView() { return g_Wheel.load(); }
bool ScreenMode::LiftUi() { return g_Lift.load(); }
void ScreenMode::Expect(const char* what)
{
    if (g_Mode.load() == 0 || g_Flat.load()) return;
    { std::lock_guard lk(g_M); g_ExpectWhat = what; g_S.expects++; }
    g_ExpectUntil = Now() + 0.7;
    LOGI("ScreenMode: the controllers asked for the {}: the screen comes up now", what);
}
void ScreenMode::RequestRecenter(const char* why) { if (g_Flat.load()) { g_RecenterReq = true; std::lock_guard lk(g_M); g_S.lastWhy = why; } }

bool ScreenMode::OnXrFrame(XRSession& xr, ID3D11Texture2D* bb, bool imageFlat, bool imageKnown)
{
    std::lock_guard lk(g_M);
    const double now = Now();
    const float dt = g_Last > 0.0 ? (float)std::clamp(now - g_Last, 0.0, 0.1) : 0.f;
    g_Last = now;
    g_S.frames++;
    float hp[3] = {}, hq[4] = { 0, 0, 0, 1 };
    const bool head = Head(xr, hp, hq);

    // ---- the mode ----
    const Kind kind = Decide(xr);
    if (kind != g_Kind)
    {
        const Kind was = g_Kind;
        g_Kind = kind;
        LOGI("ScreenMode: {} -> {}", KindName(was), KindName(kind));
        if (kind == kScreen)
        {
            g_S.entries++;
            g_ScreenAlpha = 0.f; g_SnapFade = 1.f; g_GlideT = -1.f; g_AwaySince = -1.0;
            g_Cur = head ? Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax) : Place();
            g_RecenterGen = xr.RecenterGeneration();
            g_Player = Game::LocalPlayer();
            // the game, as it was a moment ago (the menu was already drawn over the last few frames before it was known)
            const bool fresh = now - g_LastGame < 1.0;
            const bool expected = g_ExpectUntil.load() > 0.0;
            // A menu over the world (the character choice, a black market) shows its interface a few frames before its
            // state's first tick says so (measured: the shop's item carousel was in the 0.3 s picture, a ghost of it
            // under the screen): the oldest picture in the ring there, the usual one if none is that old.
            const bool frozen = fresh && g_C.backdrop > 0.f &&
                ((!expected && g_Scene != 0 && xr.FreezeBackdrop(0.45f, 2.5f)) || xr.FreezeBackdrop(expected ? 0.f : 0.3f, 2.5f));
            if (frozen) g_S.freezes++; else g_S.noFreeze++;
            g_FromPlay = frozen;
            LOGI("ScreenMode: screen at yaw {:.0f} pitch {:.0f} deg, {:.1f} m, {:.0f} deg wide; backdrop {}", g_Cur.yaw * kR2D, g_Cur.pitch * kR2D,
                 g_C.dist, g_C.size, frozen ? fmt::format("the game {:.2f} s before", xr.BackdropAge()) : fresh ? "none (no picture to freeze)" : "none (not coming from play)");
        }
        if (was == kScreen) { g_S.exits++; xr.UnfreezeBackdrop(); }
        if (kind == kWheel)
        {
            g_S.wheels++; g_WheelAlpha = 0.f;
            g_WheelAt = head ? Ahead(hp, hq, g_C.wheelDist, g_C.wheelPitchMin, g_C.wheelPitchMax) : Place();
        }
    }
    unsigned wtw = 0, wth = 0;
    bool wgamma = false;
    ID3D11ShaderResourceView* wsrv = WristHud::Texture(wtw, wth, &wgamma);
    g_Flat = g_Kind == kScreen;
    g_Wheel = g_Kind == kWheel;
    const bool stereoHud = Settings::StereoHud() && VRRuntime::Stereo() && g_Mode.load() != 0 &&
                           g_Kind == kPlay && head && wsrv && wtw && wth;
    g_Lift = (g_Kind == kWheel && g_C.wheel != 0 && wsrv && wtw && wth) || stereoHud;

    // ---- what this frame shows ----
    // A picture rendered flat is shown on the screen even once play has resumed (the pipeline is deeper than one
    // Present): across the eyes it would be the flat game's view at the headset's angles for a frame or two.
    bool screen = g_Kind == kScreen;
    if (!screen && imageKnown && imageFlat && g_ShowedScreen && g_FlatOnly < 8) { screen = true; g_FlatOnly++; g_S.flatOnly++; }
    if (!screen) g_FlatOnly = 0;
    if (screen && bb && head)
    {
        // the screen: placed, moved in front on request / after looking away for a while, gliding when it moves
        if (!g_Cur.valid || xr.RecenterGeneration() != g_RecenterGen)
        {
            g_Cur = Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax); g_GlideT = -1.f; g_RecenterGen = xr.RecenterGeneration();
        }
        if (g_RecenterReq.exchange(false))
        {
            g_S.recenters++;
            GlideTo(Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax), g_S.lastWhy.empty() ? "request" : g_S.lastWhy.c_str());
            if (Settings::Haptics()) { xr.Vibrate(0, 0.25f, 0.02f); xr.Vibrate(1, 0.25f, 0.02f); }
        }
        float c[3]; Centre(g_Cur, c);
        {
            const float fl[3] = { 0, 0, -1 }; float f[3]; QRot(hq, fl, f);
            float v[3] = { c[0] - hp[0], c[1] - hp[1], c[2] - hp[2] };
            const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            const float off = n > 1e-4f ? acosf(std::clamp((v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / n, -1.f, 1.f)) * kR2D : 0.f;
            if (g_GlideT < 0.f && g_C.followDelay > 0.f && off > g_C.follow)
            {
                if (g_AwaySince < 0.0) g_AwaySince = now;
                else if (now - g_AwaySince > g_C.followDelay) { g_S.follows++; GlideTo(Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax), "out of sight"); }
            }
            else g_AwaySince = -1.0;
        }
        if (g_GlideT >= 0.f)
        {
            g_GlideT += g_C.glide > 0.f ? dt / g_C.glide : 1.f;
            g_Cur = g_GlideT >= 1.f ? g_To : Lerp(g_From, g_To, Ease(g_GlideT));
            if (g_GlideT >= 1.f) g_GlideT = -1.f;
            Centre(g_Cur, c);
        }
        g_ScreenAlpha = g_C.fadeIn > 0.f ? std::min(1.f, g_ScreenAlpha + dt / g_C.fadeIn) : 1.f;
        const float a = Ease(g_ScreenAlpha);
        // A load builds a new player: the frozen picture is of the place left behind. Let it go.
        if (g_Player != Game::LocalPlayer() && g_SnapFade > 0.f && xr.BackdropFrozen())
        {
            if (g_SnapFade >= 1.f) { g_S.snapDropped++; LOGI("ScreenMode: a new player (a load): the frozen picture fades out"); }
            g_SnapFade = std::max(0.f, g_SnapFade - dt / 0.5f);
        }

        D3D11_TEXTURE2D_DESC bd = {}; bb->GetDesc(&bd);
        XRPanelLayer l;
        l.tex = bb; l.srcW = bd.Width; l.srcH = bd.Height;
        Crop(bd.Width, bd.Height, l.crop);
        memcpy(g_LastCrop, l.crop, sizeof g_LastCrop);
        l.premultiplied = false;
        l.alpha = a;
        l.corner = g_C.corner; l.soft = 0.0025f;
        const float aspect = l.crop[3] > 0.f ? l.crop[2] / l.crop[3] : 16.f / 9.f;
        const float grow = 0.965f + 0.035f * a;          // it opens out a little as it fades in
        l.width = 2.f * g_C.dist * tanf(g_C.size * 0.5f * kD2R) * grow; l.height = l.width / aspect;
        Quat(g_Cur.yaw, g_Cur.pitch, l.quat);
        memcpy(l.pos, c, 12);
        l.outW = (unsigned)std::clamp(std::min(l.crop[2], g_C.size * g_C.density), 256.f, 4096.f);
        l.outH = (unsigned)std::clamp(l.outW / aspect, 144.f, 4096.f);
        xr.SetPanelLayer(0, l);
        XRBackdrop b;
        if (g_FromPlay && xr.BackdropFrozen())
        {   // Out of play: the frozen picture IS the game a moment ago, so it starts at full brightness and colour and
            // dims down while the screen comes up -- the world stops and darkens, no cut to black.
            b.snapshot = (1.f + (g_C.backdrop - 1.f) * a) * g_SnapFade; b.desat = 0.35f * a; b.env = g_C.env * a; b.fade = 1.f;
        }
        else { b.snapshot = g_C.backdrop * g_SnapFade; b.env = g_C.env; b.fade = a; }
        g_LastBackdrop = xr.BackdropFrozen() ? b.snapshot : 0.f;
        for (int i = 0; i < 3; i++) b.glowDir[i] = c[i] - hp[i];
        { const float n = sqrtf(b.glowDir[0] * b.glowDir[0] + b.glowDir[1] * b.glowDir[1] + b.glowDir[2] * b.glowDir[2]); if (n > 1e-4f) for (float& x : b.glowDir) x /= n; }
        b.glow = g_C.glow * a;
        xr.SubmitScreen(b);
        g_S.screenFrames++;
        g_ShowedScreen = true;
        return true;
    }

    // ---- the game ----
    if (g_ShowedScreen)
    {   // back from the screen: both eyes start over from this picture, faded in from black
        g_ShowedScreen = false; g_GameSince = now;
        g_ExitFrom = std::clamp(g_LastBackdrop, 0.f, 1.f);   // from the darkened frozen world, or from black
        xr.ResyncEyes();
    }
    g_LastGame = now;
    if (g_C.fadeOut > 0.f && now - g_GameSince < g_C.fadeOut) xr.SetFrameFade(g_ExitFrom + (1.f - g_ExitFrom) * Ease((float)((now - g_GameSince) / g_C.fadeOut)));
    if (stereoHud)
    {   // The game's 2D HUD has no useful disparity when drawn into each eye's screen space. Lift it from the backbuffer
        // and submit the same captured image on a finite-depth quad: both eyes see one fused HUD plane in front of the
        // world. Markers remain screen-projected by the game, but no longer sit at infinity.
        constexpr float kHudDist = 2.4f;
        const float kHudWidthDeg = Settings::StereoHudWidth();
        const Place at = Ahead(hp, hq, kHudDist, -30.f, 15.f);
        float c[3]; Centre(at, c);
        XRPanelLayer l;
        l.srv = wsrv; l.srcW = wtw; l.srcH = wth;
        l.crop[0] = 0.f; l.crop[1] = 0.f; l.crop[2] = (float)wtw; l.crop[3] = (float)wth;
        l.premultiplied = true; l.gamma = wgamma; l.alpha = 1.f;
        const float aspect = (float)wtw / (float)wth;
        l.width = 2.f * kHudDist * tanf(kHudWidthDeg * 0.5f * kD2R); l.height = l.width / aspect;
        Quat(at.yaw, at.pitch, l.quat); memcpy(l.pos, c, 12);
        l.outW = (unsigned)std::clamp(std::min((float)wtw, kHudWidthDeg * g_C.density), 256.f, 4096.f);
        l.outH = (unsigned)std::clamp(l.outW / aspect, 144.f, 4096.f);
        xr.SetPanelLayer(1, l);
    }
    else if (g_Lift.load() && head)
    {   // the wheel: the game's interface on a panel where it was opened
        if (!g_WheelAt.valid) g_WheelAt = Ahead(hp, hq, g_C.wheelDist, g_C.wheelPitchMin, g_C.wheelPitchMax);
        g_WheelAlpha = g_C.wheelFade > 0.f ? std::min(1.f, g_WheelAlpha + dt / g_C.wheelFade) : 1.f;
        XRPanelLayer l;
        l.srv = wsrv; l.srcW = wtw; l.srcH = wth;
        l.crop[0] = 0.f; l.crop[1] = 0.f; l.crop[2] = (float)wtw; l.crop[3] = (float)wth;
        l.premultiplied = true; l.gamma = wgamma;
        l.alpha = Ease(g_WheelAlpha);
        l.corner = 0.f; l.soft = 0.12f;                  // the interface's own dark veil fades out at the panel's edge
        const float aspect = (float)wtw / (float)wth;
        l.width = 2.f * g_C.wheelDist * tanf(g_C.wheelSize * 0.5f * kD2R); l.height = l.width / aspect;
        Quat(g_WheelAt.yaw, g_WheelAt.pitch, l.quat);
        Centre(g_WheelAt, l.pos);
        l.outW = (unsigned)std::clamp(std::min((float)wtw, g_C.wheelSize * g_C.density), 256.f, 4096.f);
        l.outH = (unsigned)std::clamp(l.outW / aspect, 144.f, 4096.f);
        xr.SetPanelLayer(1, l);
        g_S.wheelPanels++;
    }
    else if (g_Kind != kWheel) g_WheelAt.valid = false;
    return false;
}

void ScreenMode::SetMode(int m)
{
    g_Mode = m ? 1 : 0;
    if (!m) { g_Flat = false; g_Wheel = false; g_Lift = false; }
    LOGI("ScreenMode: {}", m ? "on: menus on a screen in the room, the wheel on a panel" : "off: menus across the eyes, as before");
}
int ScreenMode::Mode() { return g_Mode.load(); }

bool ScreenMode::Set(const std::string& k, float v)
{
    std::lock_guard lk(g_M);
    Cfg& c = g_C;
    if (k == "size") c.size = std::clamp(v, 20.f, 120.f); else if (k == "dist") c.dist = std::clamp(v, 0.5f, 20.f);
    else if (k == "pitchmin") c.pitchMin = v; else if (k == "pitchmax") c.pitchMax = v;
    else if (k == "follow") c.follow = v; else if (k == "followdelay") c.followDelay = v; else if (k == "glide") c.glide = v;
    else if (k == "fadein") c.fadeIn = v; else if (k == "fadeout") c.fadeOut = v;
    else if (k == "backdrop") c.backdrop = std::clamp(v, 0.f, 1.f); else if (k == "env") c.env = std::clamp(v, 0.f, 4.f); else if (k == "glow") c.glow = std::clamp(v, 0.f, 4.f);
    else if (k == "corner") c.corner = std::clamp(v, 0.f, 0.5f); else if (k == "density") c.density = std::clamp(v, 8.f, 64.f);
    else if (k == "wheel") c.wheel = (int)v; else if (k == "wheelsize") c.wheelSize = std::clamp(v, 20.f, 120.f); else if (k == "wheeldist") c.wheelDist = std::clamp(v, 0.4f, 10.f);
    else if (k == "wheelpitchmin") c.wheelPitchMin = v; else if (k == "wheelpitchmax") c.wheelPitchMax = v; else if (k == "wheelfade") c.wheelFade = v;
    else if (k == "shopcrop") c.shopCrop = std::clamp((int)v, 0, 2);
    else if (k == "enter") c.enterFrames = std::max(0, (int)v); else if (k == "exit") c.exitFrames = std::max(0, (int)v);
    else return false;
    return true;
}

std::string ScreenMode::Config()
{
    std::lock_guard lk(g_M);
    const Cfg& c = g_C;
    return fmt::format("size {} dist {} pitchmin {} pitchmax {} follow {} followdelay {} glide {} fadein {} fadeout {} backdrop {} env {} glow {} corner {} density {} "
                       "wheel {} wheelsize {} wheeldist {} wheelpitchmin {} wheelpitchmax {} wheelfade {} shopcrop {} enter {} exit {}",
                       c.size, c.dist, c.pitchMin, c.pitchMax, c.follow, c.followDelay, c.glide, c.fadeIn, c.fadeOut, c.backdrop, c.env, c.glow, c.corner, c.density,
                       c.wheel, c.wheelSize, c.wheelDist, c.wheelPitchMin, c.wheelPitchMax, c.wheelFade, c.shopCrop, c.enterFrames, c.exitFrames);
}

bool ScreenMode::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "screen") { int m = 1; ss >> m; SetMode(m); return true; }
    if (what == "screencfg")
    {
        std::string k; float v = 0;
        if (ss >> k >> v) LOGI("cmd vr screencfg {} {} -> {}", k, v, Set(k, v) ? "ok" : "UNKNOWN NAME");
        LOGI("screencfg: {}", Config());
        return true;
    }
    if (what == "screenrecenter") { g_RecenterReq = true; { std::lock_guard lk(g_M); g_S.lastWhy = "command"; } LOGI("cmd vr screenrecenter"); return true; }
    return false;
}

std::string ScreenMode::Status()
{
    std::lock_guard lk(g_M);
    float c[3] = {}; if (g_Cur.valid) Centre(g_Cur, c);
    return fmt::format("screen on {} kind {} scene {} crop {:.0f} {:.0f} {:.0f} {:.0f} flat {} wheelview {} lift {} alpha {:.2f} entries {} exits {} frames {} screenframes {} flatonly {} "
                       "place yaw {:.1f} pitch {:.1f} dist {:.2f} centre {:.2f} {:.2f} {:.2f} gliding {} recenters {} follows {} "
                       "backdrop freezes {} none {} dropped {} snapfade {:.2f} expects {} missed {} wheels {} wheelpanels {} wheelalpha {:.2f} wheelyaw {:.1f} wheelpitch {:.1f}",
                       g_Mode.load(), KindName(g_Kind), g_Scene, g_LastCrop[0], g_LastCrop[1], g_LastCrop[2], g_LastCrop[3], g_Flat.load() ? 1 : 0, g_Wheel.load() ? 1 : 0, g_Lift.load() ? 1 : 0, g_ScreenAlpha,
                       g_S.entries, g_S.exits, g_S.frames, g_S.screenFrames, g_S.flatOnly,
                       g_Cur.yaw * kR2D, g_Cur.pitch * kR2D, g_Cur.d, c[0], c[1], c[2], g_GlideT >= 0.f ? 1 : 0, g_S.recenters, g_S.follows,
                       g_S.freezes, g_S.noFreeze, g_S.snapDropped, g_SnapFade, g_S.expects, g_S.expectMissed, g_S.wheels, g_S.wheelPanels, g_WheelAlpha, g_WheelAt.yaw * kR2D, g_WheelAt.pitch * kR2D);
}

