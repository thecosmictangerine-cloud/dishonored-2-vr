#include "pch.h"
#include "WristHud.h"
#include "GpuTrace.h"
#include "GameInfo.h"
#include "XRCapture.h"
#include "XRSession.h"
#include "CVars.h"
#include "Settings.h"
#include "ScreenMode.h"
#include "VRRuntime.h"
#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex g_M;
    std::atomic<bool> g_Capture{ true };     // vr hudcap
    // Repeat the interface only while something uses it: the watch is on and VR frames are coming (set per XR frame,
    // cleared when they stop), or a hudshot is pending. Switched off in the ini or played flat, nothing is repeated.
    std::atomic<bool> g_Wanted{ false };
    std::atomic<int> g_ProbeWant{ 0 };       // frames still to probe (set by the command)
    std::atomic<bool> g_Probing{ false };    // between two Presents of a probed frame
    std::atomic<void*> g_BackBuffer{ nullptr };
    std::atomic<unsigned> g_BbW{ 0 }, g_BbH{ 0 };
    std::vector<std::string> g_Lines;        // under g_M
    unsigned long long g_ProbeOther = 0;     // draws into smaller targets, counted only
    unsigned long long g_ProbeSeq = 0;

    // The mod's own Present runs on the render thread with the immediate context: its draws (the ImGui reticle) go
    // into the backbuffer too and must not be taken for the game's interface.
    std::atomic<bool> g_InPresent{ false };
    std::atomic<DWORD> g_PresentThread{ 0 };

    // The HUD texture: the backbuffer's size and format, transparent black at the start of every frame.
    ID3D11Device* g_Dev = nullptr;           // not owned (the game's)
    ID3D11Texture2D* g_Tex = nullptr;
    std::atomic<ID3D11RenderTargetView*> g_Rtv{ nullptr };
    ID3D11ShaderResourceView* g_Srv = nullptr;
    // The game draws its interface through a UNORM view of the sRGB backbuffer (measured 2026-10-14, the development notes):
    // its shaders output gamma-encoded colour and blend in gamma space. Repeated through an sRGB view, every value came
    // out encoded once more (the backbuffer's 141 was the texture's 196, 36 was 105, 5 was 41): the watch and the
    // wheel's panel were washed out. So the texture is typeless with both views, and the repeat uses the view the game
    // used. (g_Rtv / g_Srv: the backbuffer's own format; these: the other one, when the format family has one.)
    std::atomic<ID3D11RenderTargetView*> g_RtvAlt{ nullptr };
    ID3D11ShaderResourceView* g_SrvAlt = nullptr;
    DXGI_FORMAT g_AltFmt = DXGI_FORMAT_UNKNOWN, g_ViewFmt = DXGI_FORMAT_UNKNOWN;
    std::atomic<int> g_GameView{ 0 };        // the format of the view the game draws its interface through (0 = not seen)
    bool IsSrgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
    unsigned g_TexW = 0, g_TexH = 0; DXGI_FORMAT g_TexFmt = DXGI_FORMAT_UNKNOWN;
    std::vector<IUnknown*> g_Retired;        // textures replaced on a resize: released a few frames later
    int g_RetireIn = 0;

    // Variants of the game's states for the repeated draw, by the game state's DESCRIPTION (a pointer can be reused
    // for another state after a level load; a description cannot lie).
    struct BlendKey { D3D11_BLEND_DESC d; bool operator<(const BlendKey& o) const { return memcmp(&d, &o.d, sizeof d) < 0; } };
    struct DsKey { D3D11_DEPTH_STENCIL_DESC d; bool operator<(const DsKey& o) const { return memcmp(&d, &o.d, sizeof d) < 0; } };
    std::map<BlendKey, ID3D11BlendState*> g_BlendVar;     // under g_M
    std::map<DsKey, ID3D11DepthStencilState*> g_DsVar;    // under g_M

    // Stats (heartbeat)
    std::atomic<unsigned long long> g_Frames{ 0 }, g_Mirrored{ 0 }, g_Lifted{ 0 }, g_SkipNoPs{ 0 }, g_SkipNoBlend{ 0 }, g_SkipOwn{ 0 }, g_Checked{ 0 };
    std::atomic<unsigned long long> g_CheckTicks{ 0 };
    unsigned g_LastFrameMirrored = 0; unsigned long long g_MirroredAtFrame = 0;
    double g_LastCheckUs = 0;

    std::string g_ShotTag; std::atomic<bool> g_ShotWant{ false };

    const char* DrawName(int t)
    {
        static const char* n[] = { "DrawIndexed", "Draw", "DrawIdxInst", "DrawInst", "DrawIdxInstIndirect", "DrawInstIndirect" };
        return t >= 0 && t < 6 ? n[t] : "?";
    }

    std::string BlendText(ID3D11BlendState* b)
    {
        if (!b) return "blend default";
        D3D11_BLEND_DESC d{}; b->GetDesc(&d);
        const auto& r = d.RenderTarget[0];
        if (!r.BlendEnable) return fmt::format("blend off mask {:X}", r.RenderTargetWriteMask);
        return fmt::format("blend c {}*{} op{} a {}*{} op{} mask {:X}", (int)r.SrcBlend, (int)r.DestBlend, (int)r.BlendOp,
                           (int)r.SrcBlendAlpha, (int)r.DestBlendAlpha, (int)r.BlendOpAlpha, r.RenderTargetWriteMask);
    }

    bool OwnDraw(int kind) { return kind == 0 && g_InPresent.load() && GetCurrentThreadId() == g_PresentThread.load(); }

    //! The game's blend, with alpha accumulated as coverage ("over"): onto transparent black the colour comes out
    //! premultiplied whichever of straight or premultiplied alpha the game's draw uses.
    ID3D11BlendState* BlendVariant(const D3D11_BLEND_DESC& in)
    {
        BlendKey k{}; memcpy(&k.d, &in, sizeof in);
        std::lock_guard lk(g_M);
        auto it = g_BlendVar.find(k);
        if (it != g_BlendVar.end()) return it->second;
        D3D11_BLEND_DESC d = in;
        for (auto& r : d.RenderTarget)
        {
            r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (r.RenderTargetWriteMask & 7) r.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        }
        ID3D11BlendState* s = nullptr;
        if (g_Dev) g_Dev->CreateBlendState(&d, &s);
        g_BlendVar[k] = s;
        return s;
    }
    //! The game's depth-stencil test with every write switched off: the repeated draw goes FIRST, so it must see the
    //! buffer exactly as the game's draw will, and leave it that way.
    ID3D11DepthStencilState* DsVariant(const D3D11_DEPTH_STENCIL_DESC& in)
    {
        DsKey k{}; memcpy(&k.d, &in, sizeof in);
        std::lock_guard lk(g_M);
        auto it = g_DsVar.find(k);
        if (it != g_DsVar.end()) return it->second;
        D3D11_DEPTH_STENCIL_DESC d = in;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; d.StencilWriteMask = 0;
        ID3D11DepthStencilState* s = nullptr;
        if (g_Dev) g_Dev->CreateDepthStencilState(&d, &s);
        g_DsVar[k] = s;
        return s;
    }

    void EnsureTexture(ID3D11Device* dev, ID3D11Texture2D* bb)
    {
        D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
        if (g_Tex && g_TexW == bd.Width && g_TexH == bd.Height && g_TexFmt == bd.Format) return;
        // the old one may still be referenced by command lists being recorded: retire it, release later
        if (g_Tex) { g_Retired.push_back(g_Tex); g_Retired.push_back(g_Srv); g_Retired.push_back(g_Rtv.load()); g_Retired.push_back(g_SrvAlt); g_Retired.push_back(g_RtvAlt.load()); g_RetireIn = 120; }
        g_Tex = nullptr; g_Srv = nullptr; g_Rtv = nullptr; g_SrvAlt = nullptr; g_RtvAlt = nullptr;
        // A typeless backbuffer gets a typed view; an sRGB one keeps sRGB. The texture itself is typeless where the
        // format family allows, with the other view too (UNORM <-> sRGB): the repeat goes through the one the game uses.
        DXGI_FORMAT view = bd.Format, family = bd.Format, alt = DXGI_FORMAT_UNKNOWN;
        if (view == DXGI_FORMAT_R8G8B8A8_TYPELESS) view = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (view == DXGI_FORMAT_B8G8R8A8_TYPELESS) view = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        { family = DXGI_FORMAT_R8G8B8A8_TYPELESS; alt = view == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; }
        else if (view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        { family = DXGI_FORMAT_B8G8R8A8_TYPELESS; alt = view == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM; }
        else family = view;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bd.Width; td.Height = bd.Height; td.MipLevels = 1; td.ArraySize = 1; td.Format = family;
        td.SampleDesc = { 1, 0 }; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* t = nullptr; ID3D11RenderTargetView* r = nullptr; ID3D11ShaderResourceView* s = nullptr;
        ID3D11RenderTargetView* ra = nullptr; ID3D11ShaderResourceView* sa = nullptr;
        auto rtvOf = [&](DXGI_FORMAT f, ID3D11RenderTargetView** o) { D3D11_RENDER_TARGET_VIEW_DESC d{}; d.Format = f; d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; return SUCCEEDED(dev->CreateRenderTargetView(t, &d, o)); };
        auto srvOf = [&](DXGI_FORMAT f, ID3D11ShaderResourceView** o) { D3D11_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = f; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1; return SUCCEEDED(dev->CreateShaderResourceView(t, &d, o)); };
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &t)) || !rtvOf(view, &r) || !srvOf(view, &s) ||
            (alt != DXGI_FORMAT_UNKNOWN && (!rtvOf(alt, &ra) || !srvOf(alt, &sa))))
        {
            LOGI("WristHud: could not create the HUD texture {}x{} format {} (views {} / {})", bd.Width, bd.Height, (int)family, (int)view, (int)alt);
            for (IUnknown* u : std::initializer_list<IUnknown*>{ t, r, s, ra, sa }) if (u) u->Release();
            g_TexW = bd.Width; g_TexH = bd.Height; g_TexFmt = bd.Format;   // do not retry every frame
            return;
        }
        g_Tex = t; g_Srv = s; g_SrvAlt = sa; g_TexW = bd.Width; g_TexH = bd.Height; g_TexFmt = bd.Format; g_ViewFmt = view; g_AltFmt = alt;
        ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
        const float clear[4] = { 0, 0, 0, 0 };
        if (imm) { imm->ClearRenderTargetView(r, clear); imm->Release(); }
        g_RtvAlt = ra;
        g_Rtv = r;
        LOGI("WristHud: HUD texture {}x{} format {} (views {} and {})", bd.Width, bd.Height, (int)family, (int)view, (int)alt);
    }
}

namespace { std::atomic<unsigned long long> g_LastXrFrame{ 0 }; void HudBackIfNoXr(); }

void WristHud::RequestProbe(int frames) { g_ProbeWant = frames < 1 ? 1 : frames; LOGI("WristHud: probing the next {} frame(s)", g_ProbeWant.load()); }
void WristHud::RequestShot(const std::string& tag) { { std::lock_guard lk(g_M); g_ShotTag = tag.empty() ? "hud" : tag; } g_ShotWant = true; }
void WristHud::SetCapture(bool on) { g_Capture = on; LOGI("WristHud: capture of the game's interface {}", on ? "on" : "off"); }
bool WristHud::Capture() { return g_Capture.load(); }

ID3D11ShaderResourceView* WristHud::Texture(unsigned& w, unsigned& h, bool* gamma)
{
    w = g_TexW; h = g_TexH;
    if (!g_Capture.load() || !g_Rtv.load()) return nullptr;
    // Whichever view a draw went through, the bytes stored are the colours the backbuffer shows (gamma-encoded), so
    // they are read through the sRGB view: linear light, premultiplied -- exact where the game blended in linear light,
    // and where it blended in gamma space exact over a dark world (the wheel's veil). Without an sRGB view the reader
    // decodes it itself (`gamma`).
    const bool alt = g_SrvAlt && IsSrgb(g_AltFmt);
    const DXGI_FORMAT f = alt ? g_AltFmt : g_ViewFmt;
    if (gamma) *gamma = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM;
    return alt ? g_SrvAlt : g_Srv;
}

void WristHud::OnDraw(ID3D11DeviceContext* ctx, int kind, int drawType, unsigned count)
{
    if (!g_Probing.load() || !ctx) return;
    ID3D11RenderTargetView* rtv[2] = {}; ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(2, rtv, &dsv);
    ID3D11Resource* res = nullptr; D3D11_TEXTURE2D_DESC td{};
    if (rtv[0]) { rtv[0]->GetResource(&res); if (res) ((ID3D11Texture2D*)res)->GetDesc(&td); }
    const bool full = res && td.Width == g_BbW.load() && td.Height == g_BbH.load();
    if (!full)
    {
        std::lock_guard lk(g_M); g_ProbeOther++;
    }
    else
    {
        D3D11_VIEWPORT vp{}; UINT nvp = 1; ctx->RSGetViewports(&nvp, &vp);
        ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
        ID3D11PixelShader* ps = nullptr; ctx->PSGetShader(&ps, nullptr, nullptr);
        ID3D11BlendState* bs = nullptr; float bf[4]; UINT sm = 0; ctx->OMGetBlendState(&bs, bf, &sm);
        ID3D11ShaderResourceView* srv[2] = {}; ctx->PSGetShaderResources(0, 2, srv);
        D3D11_PRIMITIVE_TOPOLOGY topo{}; ctx->IAGetPrimitiveTopology(&topo);
        std::string tex;
        for (int i = 0; i < 2; i++)
        {
            if (!srv[i]) continue;
            ID3D11Resource* r = nullptr; srv[i]->GetResource(&r);
            D3D11_RESOURCE_DIMENSION dim{}; if (r) r->GetType(&dim);
            if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC s{}; ((ID3D11Texture2D*)r)->GetDesc(&s); tex += fmt::format(" t{} {}x{} f{} {}", i, s.Width, s.Height, (int)s.Format, (void*)r); }
            else tex += fmt::format(" t{} dim{}", i, (int)dim);
            if (r) r->Release();
        }
        const uint64_t vh = GpuTrace::VsHashOf(vs);
        std::lock_guard lk(g_M);
        g_Lines.push_back(fmt::format("#{} {} {} n {} ctx {} rt {}{}{} f{} dsv {} rtv1 {} vp {:.0f},{:.0f} {:.0f}x{:.0f} vs {:X} ps {} topo {} {}{}",
            g_ProbeSeq++, kind ? "def" : "imm", DrawName(drawType), count, (void*)ctx, (void*)res, res == g_BackBuffer.load() ? " (BACKBUFFER)" : "",
            OwnDraw(kind) ? " (MOD)" : "", (int)td.Format, dsv ? 1 : 0, rtv[1] ? 1 : 0, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vh, (void*)ps,
            (int)topo, BlendText(bs), tex));
        if (vs) vs->Release(); if (ps) ps->Release(); if (bs) bs->Release();
        for (auto s : srv) if (s) s->Release();
    }
    if (res) res->Release();
    for (auto r : rtv) if (r) r->Release();
    if (dsv) dsv->Release();
}

bool WristHud::MirrorBegin(ID3D11DeviceContext* ctx, int kind, Mirror& m)
{
    ID3D11RenderTargetView* ours = g_Rtv.load();
    const bool lift = ScreenMode::LiftUi();
    if (!ours || !g_Capture.load() || !ctx || !(g_Wanted.load() || g_ShotWant.load() || lift)) return false;
    m.lift = lift;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    g_Checked++;
    // Is the game drawing into the backbuffer? One view, its resource, compared.
    ID3D11RenderTargetView* rtv0 = nullptr;
    ctx->OMGetRenderTargets(1, &rtv0, nullptr);
    bool bb = false;
    DXGI_FORMAT gameView = DXGI_FORMAT_UNKNOWN;
    if (rtv0)
    {
        ID3D11Resource* res = nullptr; rtv0->GetResource(&res);
        bb = res && res == g_BackBuffer.load();
        if (bb) { D3D11_RENDER_TARGET_VIEW_DESC vd{}; rtv0->GetDesc(&vd); gameView = vd.Format; }
        if (res) res->Release();
        rtv0->Release();
    }
    auto done = [&](bool r) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); g_CheckTicks += (unsigned long long)(t1.QuadPart - t0.QuadPart); return r; };
    if (!bb) return done(false);
    // (it uses BOTH in one frame: measured, the interface alternates between sRGB and UNORM views of the typeless
    // backbuffer, draw by draw -- so the view is matched per draw)
    if (g_GameView.exchange((int)gameView) != (int)gameView)
    {
        static int logged = 0;
        if (logged++ < 4) LOGI("WristHud: the game draws interface through a view of format {} ({}): repeated through the same", (int)gameView,
                               IsSrgb(gameView) ? "sRGB, linear blending" : "not sRGB: gamma-space blending");
    }
    if (ID3D11RenderTargetView* alt = g_RtvAlt.load(); alt && gameView == g_AltFmt) ours = alt;
    if (OwnDraw(kind)) { g_SkipOwn++; return done(false); }
    ID3D11PixelShader* ps = nullptr; ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) { g_SkipNoPs++; return done(false); }     // a stencil mask: its effect is in the shared depth-stencil buffer
    ps->Release();
    ctx->OMGetBlendState(&m.bs, m.factor, &m.sampleMask);
    D3D11_BLEND_DESC bd{};
    if (m.bs) m.bs->GetDesc(&bd);
    if (!m.bs || !bd.RenderTarget[0].BlendEnable)
    {   // an opaque full-screen pass (the scene's copy into the backbuffer), not interface
        if (m.bs) { m.bs->Release(); m.bs = nullptr; }
        g_SkipNoBlend++; return done(false);
    }
    ctx->OMGetDepthStencilState(&m.ds, &m.ref);
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (m.ds) m.ds->GetDesc(&dd);
    else
    {   // the default state
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.StencilEnable = FALSE; dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace = dd.BackFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
    }
    ID3D11BlendState* bv = BlendVariant(bd);
    ID3D11DepthStencilState* dv = DsVariant(dd);
    if (!bv || !dv)
    {
        if (m.bs) { m.bs->Release(); m.bs = nullptr; } if (m.ds) { m.ds->Release(); m.ds = nullptr; }
        return done(false);
    }
    ctx->OMGetRenderTargets(8, m.rtv, &m.dsv);
    ctx->OMSetRenderTargets(1, &ours, m.dsv);
    ctx->OMSetBlendState(bv, m.factor, m.sampleMask);
    ctx->OMSetDepthStencilState(dv, m.ref);
    g_Mirrored++; if (m.lift) g_Lifted++;
    return done(true);
}

namespace
{
    constexpr uint64_t kScaleformVs = 0x7ACC1A9A27C4D9E2ull;   // the interface's vertex shader for fills and masks (hudprobe)
    std::atomic<bool> g_DropBars{ false };
    std::atomic<int> g_BarsMode{ 1 };
    std::atomic<unsigned long long> g_BarsDropped{ 0 }, g_BarsFrames{ 0 };
    std::atomic<unsigned> g_BarsLastFrame{ 0 }, g_BarsThisFrame{ 0 };
}

void WristHud::SetDropBars(bool on)
{
    const bool want = on && g_BarsMode.load() != 0;
    if (g_DropBars.exchange(want) != want) LOGI("WristHud: cutscene letterbox {}", want ? "not drawn (the world above and below the eyes stays visible)" : "back to the game's");
}
void WristHud::SetBarsMode(int m) { g_BarsMode = m; if (!m) g_DropBars = false; LOGI("WristHud: cinebars {}", m ? "1: the cutscene letterbox is removed" : "0: the game's letterbox"); }
int WristHud::BarsMode() { return g_BarsMode.load(); }

bool WristHud::DropDraw(ID3D11DeviceContext* ctx, int kind)
{
    if (!g_DropBars.load() || !ctx || OwnDraw(kind)) return false;
    // An untextured fill with the interface's vertex shader, blended into the backbuffer: in a cutscene, the bars.
    ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
    const uint64_t vh = vs ? GpuTrace::VsHashOf(vs) : 0;
    if (vs) vs->Release();
    if (vh != kScaleformVs) return false;
    ID3D11PixelShader* ps = nullptr; ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return false;                               // a stencil mask, not a fill
    ps->Release();
    ID3D11ShaderResourceView* srv = nullptr; ctx->PSGetShaderResources(0, 1, &srv);
    if (srv) { srv->Release(); return false; }           // textured: text, icons, the skip prompt
    ID3D11RenderTargetView* rtv0 = nullptr; ctx->OMGetRenderTargets(1, &rtv0, nullptr);
    bool bb = false;
    if (rtv0) { ID3D11Resource* r = nullptr; rtv0->GetResource(&r); bb = r && r == g_BackBuffer.load(); if (r) r->Release(); rtv0->Release(); }
    if (!bb) return false;
    if (g_BarsDropped++ == 0) LOGI("WristHud: first cutscene letterbox bar dropped (an untextured interface fill into the backbuffer)");
    g_BarsThisFrame++;
    return true;
}

void WristHud::MirrorEnd(ID3D11DeviceContext* ctx, Mirror& m)
{
    UINT n = 0; for (UINT i = 0; i < 8; i++) if (m.rtv[i]) n = i + 1;
    ctx->OMSetRenderTargets(n, m.rtv, m.dsv);
    ctx->OMSetBlendState(m.bs, m.factor, m.sampleMask);
    ctx->OMSetDepthStencilState(m.ds, m.ref);
    for (auto& r : m.rtv) if (r) { r->Release(); r = nullptr; }
    if (m.dsv) { m.dsv->Release(); m.dsv = nullptr; }
    if (m.bs) { m.bs->Release(); m.bs = nullptr; }
    if (m.ds) { m.ds->Release(); m.ds = nullptr; }
}

void WristHud::BeginPresent(IDXGISwapChain* sc)
{
    g_PresentThread = GetCurrentThreadId();
    g_InPresent = true;
    g_Frames++;
    g_BarsLastFrame = g_BarsThisFrame.exchange(0);
    {
        const unsigned long long m = g_Mirrored.load();
        g_LastFrameMirrored = (unsigned)(m - g_MirroredAtFrame); g_MirroredAtFrame = m;
        static unsigned long long lastTicks = 0;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        const unsigned long long t = g_CheckTicks.load();
        g_LastCheckUs = (double)(t - lastTicks) * 1e6 / (double)f.QuadPart; lastTicks = t;
    }
    ID3D11Texture2D* bb = nullptr;
    if (sc && SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
    {
        D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
        g_BackBuffer = bb; g_BbW = d.Width; g_BbH = d.Height;
        if (!g_Dev) { bb->GetDevice(&g_Dev); if (g_Dev) g_Dev->Release(); }   // the game's device outlives the mod: not owned
        if (g_Dev && g_Capture.load()) EnsureTexture(g_Dev, bb);
        bb->Release();      // the swapchain keeps it alive; the pointer is only compared
    }
    if (g_RetireIn > 0 && --g_RetireIn == 0) { for (auto u : g_Retired) if (u) u->Release(); g_Retired.clear(); }
    HudBackIfNoXr();
    if (g_Probing.load())
    {
        std::vector<std::string> lines; unsigned long long other = 0;
        { std::lock_guard lk(g_M); lines.swap(g_Lines); other = g_ProbeOther; g_ProbeOther = 0; g_ProbeSeq = 0; }
        LOGI("hudprobe: frame of {} full-size draws (+{} into smaller targets), backbuffer {} {}x{}", lines.size(), other, g_BackBuffer.load(), g_BbW.load(), g_BbH.load());
        for (auto& l : lines) LOGI("hudprobe: {}", l);
        g_Probing = false;
    }
    if (g_ProbeWant.load() > 0) { g_ProbeWant--; g_Probing = true; }
    if (g_ShotWant.exchange(false) && g_Tex && g_Dev)
    {
        std::string tag; { std::lock_guard lk(g_M); tag = g_ShotTag; }
        ID3D11DeviceContext* imm = nullptr; g_Dev->GetImmediateContext(&imm);
        const std::wstring path = GameInfo::GameDir() + L"\\DVR_" + std::wstring(tag.begin(), tag.end()) + L"_hud.png";
        const bool ok = imm && XRCapture::SaveTextureToPng(g_Dev, imm, g_Tex, path.c_str());
        if (imm) imm->Release();
        LOGI("WristHud: HUD texture saved: {} ({} draws mirrored last frame)", ok ? "ok" : "FAILED", g_LastFrameMirrored);
    }
}

void WristHud::EndPresent()
{
    if (ID3D11RenderTargetView* r = g_Rtv.load(); r && g_Dev)
    {
        ID3D11DeviceContext* imm = nullptr; g_Dev->GetImmediateContext(&imm);
        const float clear[4] = { 0, 0, 0, 0 };
        if (imm) { imm->ClearRenderTargetView(r, clear); imm->Release(); }
    }
    g_InPresent = false;
}

std::string WristHud::Status()
{
    return fmt::format("hud capture {} tex {}x{} mirrored {} lifted {} (last frame {}) skip noPS {} noBlend {} mod {} checked {} checkus {:.0f} cinebars {} dropping {} dropped {} (last frame {})",
                       g_Capture.load() ? 1 : 0, g_TexW, g_TexH, g_Mirrored.load(), g_Lifted.load(), g_LastFrameMirrored, g_SkipNoPs.load(),
                       g_SkipNoBlend.load(), g_SkipOwn.load(), g_Checked.load(), g_LastCheckUs,
                       g_BarsMode.load(), g_DropBars.load() ? 1 : 0, g_BarsDropped.load(), g_BarsLastFrame.load());
}

// ---------------------------------------------------------------------------------------------------------------
// The watch: where the interface goes, when it shows, and what it shows.
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // Config (vr wristhud / vr wristcfg). Offsets in the frame the mod draws the left hand from: the controller's AIM
    // pose (with ControllerPitchOffset), -Z along the pointing ray (so +Z runs back along the forearm), +X to the right
    // (the left palm's side, so -X is the back of the hand), +Y up. Anchoring to the same pose the arm is drawn from
    // is what puts the watch on the DRAWN wrist on every runtime: a grip pose is defined differently by each one, the
    // mod's hand is not. In the mock the two poses are the same.
    struct WristCfg
    {
        int anchor = 1;             // 1 = the aim pose (as the arm), 0 = the runtime's grip pose
        float size = 0.085f;        // m, side of the square quad (the ring is ~3/4 of it: ~6 cm)
        float back = 0.05f;         // m out of the back of the hand from the controller's axis: ~2 cm over the drawn sleeve
        float along = 0.08f;        // m back from the controller along the forearm: the drawn wrist joint is 2-3 cm
                                    // back (measured, `drawnwrist`), a watch sits a little further up the arm
        float side = 0.0f;          // m towards the thumb
        float onDeg = 48.f, offDeg = 65.f;      // face turned to the eye within this to show / beyond this to hide
        float gazeOn = 40.f, gazeOff = 55.f;    // and the head looking at it within this / beyond this (eyes move too)
        float maxDist = 0.85f;      // m, eye to watch
        float fadeIn = 0.12f, fadeOut = 0.22f;  // s
        float backdrop = 0.45f;     // the dark disc behind
        float cropScale = 0.33f;    // crop side in units of the 16:9 frame's height (the ring is 0.244)
        int triggerHides = 1;       // the left trigger held (casting, firing the left-hand weapon) keeps it away
        float haptic = 0.12f;       // amplitude of a short tick when it appears (0 = none)
        int upright = 1;            // turn the face about its normal so it reads upright to the head
        int forceHud = 1;           // keep the game's HUD always on (ark_ui_show_master_hud 2) while the watch is on
    } g_W;
    std::atomic<int> g_WristOn{ 1 };
    std::mutex g_WM;                // g_W and the state below
    float g_Alpha = 0; bool g_Shown = false; float g_Roll = 0; bool g_RollValid = false;
    LARGE_INTEGER g_LastT{};
    int g_UserHud = -1;             // the player's own ark_ui_show_master_hud, to put back
    ULONGLONG g_NextHudCheck = 0;
    struct WStats { unsigned long long frames = 0, shown = 0, submitted = 0; float facing = 0, gaze = 0, dist = 0; int why = 0; } g_WS;
    const char* kWhy[] = { "ok", "off", "no session", "no texture", "no interface", "not tracked", "menu", "facing", "gaze", "far", "trigger" };

    void QRot(const float q[4], const float v[3], float o[3])
    {   // o = q v q*
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }
    void QRotInv(const float q[4], const float v[3], float o[3]) { const float c[4] = { -q[0], -q[1], -q[2], q[3] }; QRot(c, v, o); }
    //! Quaternion (x,y,z,w) of the rotation whose COLUMNS are c0, c1, c2.
    void QuatFromCols(const float c0[3], const float c1[3], const float c2[3], float q[4])
    {
        const float m00 = c0[0], m10 = c0[1], m20 = c0[2], m01 = c1[0], m11 = c1[1], m21 = c1[2], m02 = c2[0], m12 = c2[1], m22 = c2[2];
        const float tr = m00 + m11 + m22;
        if (tr > 0) { const float s = sqrtf(tr + 1.f) * 2; q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s; }
        else if (m00 > m11 && m00 > m22) { const float s = sqrtf(1.f + m00 - m11 - m22) * 2; q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s; }
        else if (m11 > m22) { const float s = sqrtf(1.f + m11 - m00 - m22) * 2; q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s; }
        else { const float s = sqrtf(1.f + m22 - m00 - m11) * 2; q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s; }
        const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (int i = 0; i < 4; i++) q[i] /= n;
    }
    float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    //! Where the game draws the HUD cluster (health, mana, equipped item) in a W x H backbuffer. Measured at 1902x973,
    //! 1280x720, 2560x1080, 1896x1968 and 1440x1440 (`vr hudshot`, scripts\testing\wrist\layout.ps1): the interface
    //! lives in a centred 16:9 frame of height Hs = min(H, W*9/16); the ring's centre is 0.1465*Hs right of that
    //! frame's left edge and 0.133*Hs + 0.0327*(H - Hs) down from the top of the screen (~1 px off in all five).
    void ClusterCentre(unsigned W, unsigned H, float& cx, float& cy, float& hs)
    {
        hs = std::min((float)H, (float)W * 9.f / 16.f);
        const float frameLeft = ((float)W - hs * 16.f / 9.f) * 0.5f;
        cx = frameLeft + 0.1465f * hs;
        cy = 0.133f * hs + 0.0327f * ((float)H - hs);
    }

    //! Keep the game's HUD on all the time while the watch is in use (contextual mode hides the cluster a few seconds
    //! after the last change: measured, the watch would be empty most of the time). The player's value is put back
    //! when the watch is switched off or VR stops.
    void ForceHud(bool want)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < g_NextHudCheck || !CVars::Available()) return;
        g_NextHudCheck = now + 1000;
        const int cur = CVars::GetInt("ark_ui_show_master_hud", -1);
        if (cur < 0) return;
        if (want && cur != 2)
        {
            g_UserHud = cur;
            CVars::SetInt("ark_ui_show_master_hud", 2);
            LOGI("WristHud: game HUD set to always on for the watch (was {})", cur);
        }
        else if (!want && g_UserHud >= 0)
        {
            CVars::SetInt("ark_ui_show_master_hud", g_UserHud);
            LOGI("WristHud: game HUD back to the player's setting ({})", g_UserHud);
            g_UserHud = -1;
        }
    }
}

namespace
{
    //! VR stopped (no XR frame for 2 s: headset off, connection lost, the runtime quit): the game's HUD goes back to
    //! the player's own setting, as it would with the watch switched off.
    void HudBackIfNoXr()
    {
        const unsigned long long last = g_LastXrFrame.load();
        if (!last || GetTickCount64() - last < 2000) return;
        std::lock_guard lk(g_WM);
        if (g_UserHud >= 0) ForceHud(false);
        g_Alpha = 0.f; g_Shown = false;
        g_Wanted = false;
    }
}

void WristHud::SetWatch(int on) { g_WristOn = on; LOGI("WristHud: watch {}", on ? "on" : "off"); }
int WristHud::Watch() { return g_WristOn.load(); }

bool WristHud::Set(const std::string& k, float v)
{
    std::lock_guard lk(g_WM);
    if (k == "size") g_W.size = v; else if (k == "back") g_W.back = v; else if (k == "along") g_W.along = v; else if (k == "side") g_W.side = v;
    else if (k == "on") g_W.onDeg = v; else if (k == "off") g_W.offDeg = v; else if (k == "gazeon") g_W.gazeOn = v; else if (k == "gazeoff") g_W.gazeOff = v;
    else if (k == "dist") g_W.maxDist = v; else if (k == "fadein") g_W.fadeIn = v; else if (k == "fadeout") g_W.fadeOut = v;
    else if (k == "backdrop") g_W.backdrop = v; else if (k == "crop") g_W.cropScale = v; else if (k == "upright") g_W.upright = (int)v;
    else if (k == "anchor") g_W.anchor = (int)v; else if (k == "forcehud") g_W.forceHud = (int)v; else if (k == "trigger") g_W.triggerHides = (int)v; else if (k == "haptic") g_W.haptic = v;
    else return false;
    return true;
}

std::string WristHud::Config()
{
    std::lock_guard lk(g_WM);
    return fmt::format("anchor {} size {} back {} along {} side {} on {} off {} gazeon {} gazeoff {} dist {} fadein {} fadeout {} backdrop {} crop {} upright {} forcehud {} trigger {} haptic {}",
                       g_W.anchor, g_W.size, g_W.back, g_W.along, g_W.side, g_W.onDeg, g_W.offDeg, g_W.gazeOn, g_W.gazeOff, g_W.maxDist, g_W.fadeIn,
                       g_W.fadeOut, g_W.backdrop, g_W.cropScale, g_W.upright, g_W.forceHud, g_W.triggerHides, g_W.haptic);
}

void WristHud::OnXrFrame(XRSession& xr, bool inMenu)
{
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    std::lock_guard lk(g_WM);
    const float dt = g_LastT.QuadPart ? std::min(0.1f, (float)(now.QuadPart - g_LastT.QuadPart) / (float)fq.QuadPart) : 0.f;
    g_LastT = now;
    g_WS.frames++;
    g_LastXrFrame = GetTickCount64();
    const bool on = g_WristOn.load() != 0 && g_Capture.load();
    const bool running = xr.IsSessionRunning();
    // In stereo the game HUD is lifted out of each eye's screen-space image and submitted by ScreenMode as one
    // finite-depth quad. Keep capturing it even when the wrist watch itself is disabled.
    const bool stereoOverlay = Settings::StereoHud() && VRRuntime::Stereo() && !inMenu && ScreenMode::Mode() != 0;
    g_Wanted = (on || stereoOverlay) && running;
    ForceHud(on && running && g_W.forceHud);

    int why = 0;
    float wq[4], wp[3];
    unsigned tw = 0, th = 0;
    bool gamma = false;
    ID3D11ShaderResourceView* srv = WristHud::Texture(tw, th, &gamma);
    if (!on) why = 1;
    else if (!running) why = 2;
    else if (!srv || !tw || !th) why = 3;
    else if (g_LastFrameMirrored == 0) why = 4;
    else if (!xr.WristPose(g_W.anchor, wq, wp) || xr.EyeCount() < 1 || !xr.Eye(0).valid) why = 5;
    // The mod draws the hand from the aim pose turned by ControllerPitchOffset about its X axis; the layer's space is
    // the runtime's own aim pose, so the offsets get the same turn: frame = pose * qx.
    float qx[4] = { 0, 0, 0, 1 };
    if (g_W.anchor == 1 && xr.PitchOffset() != 0.f)
    {
        const float h = xr.PitchOffset() * 0.5f / 57.29578f;
        qx[0] = sinf(h); qx[3] = cosf(h);
    }
    if (why != 5)
    {   // wq := wq * qx
        const float a[4] = { wq[0], wq[1], wq[2], wq[3] };
        wq[0] = a[3] * qx[0] + a[0] * qx[3] + a[1] * qx[2] - a[2] * qx[1];
        wq[1] = a[3] * qx[1] - a[0] * qx[2] + a[1] * qx[3] + a[2] * qx[0];
        wq[2] = a[3] * qx[2] + a[0] * qx[1] - a[1] * qx[0] + a[2] * qx[3];
        wq[3] = a[3] * qx[3] - a[0] * qx[0] - a[1] * qx[1] - a[2] * qx[2];
    }
    else if (inMenu) why = 6;

    XRWristLayer l;
    if (why == 0 || why == 6)
    {
        // the watch face: on the back of the wrist, facing out of it (-X of the grip)
        const float off[3] = { -g_W.back, g_W.side, g_W.along };
        float c[3]; QRot(wq, off, c); for (int i = 0; i < 3; i++) c[i] += wp[i];
        const float nLocal[3] = { -1.f, 0.f, 0.f };
        float n[3]; QRot(wq, nLocal, n);
        const XREyePose& hd = xr.Head();     // (the head's orientation, not the left eye's: canted displays)
        const float eye[3] = { hd.posX, hd.posY, hd.posZ };
        const float hq[4] = { hd.quatX, hd.quatY, hd.quatZ, hd.quatW };
        float v[3] = { eye[0] - c[0], eye[1] - c[1], eye[2] - c[2] };
        const float dist = sqrtf(Dot(v, v));
        if (dist > 1e-4f) for (float& x : v) x /= dist;
        const float fwdL[3] = { 0, 0, -1 }, upL[3] = { 0, 1, 0 };
        float fwd[3], up[3]; QRot(hq, fwdL, fwd); QRot(hq, upL, up);
        const float facing = Dot(n, v);
        const float gaze = -Dot(fwd, v);
        g_WS.facing = acosf(std::clamp(facing, -1.f, 1.f)) * 57.29578f;
        g_WS.gaze = acosf(std::clamp(gaze, -1.f, 1.f)) * 57.29578f;
        g_WS.dist = dist;
        if (why == 0)
        {
            const bool trigger = g_W.triggerHides && xr.Input().triggerL > 0.5f;
            const bool wantOn = g_WS.facing < g_W.onDeg && g_WS.gaze < g_W.gazeOn && dist < g_W.maxDist && !trigger;
            const bool keep = g_WS.facing < g_W.offDeg && g_WS.gaze < g_W.gazeOff && dist < g_W.maxDist + 0.1f && !trigger;
            if (!g_Shown && wantOn)
            {
                g_Shown = true;
                // a light tick, like a smartwatch waking up
                if (g_W.haptic > 0.f && Settings::Haptics()) xr.Vibrate(0, g_W.haptic, 0.018f, 160.f);
            }
            else if (g_Shown && !keep) g_Shown = false;
            if (!g_Shown) why = trigger ? 10 : g_WS.facing >= g_W.offDeg ? 7 : g_WS.gaze >= g_W.gazeOff ? 8 : dist >= g_W.maxDist ? 9 : 7;
        }
        else g_Shown = false;

        // Face orientation in the grip frame: normal -X; "up" = the head's up projected on the face, so the dial reads
        // upright however the forearm lies (across the chest, pointing ahead, raised). Without it, up is the thumb side.
        const float nG[3] = { -1.f, 0.f, 0.f };
        float upG[3]; QRotInv(wq, up, upG);
        float roll = atan2f(upG[2], upG[1]);            // angle of the head's up in the face's plane (y,z of the grip)
        const float inPlane = sqrtf(upG[1] * upG[1] + upG[2] * upG[2]);
        if (!g_W.upright) roll = 0.f;
        else if (inPlane < 0.2f) roll = g_RollValid ? g_Roll : roll;   // looking along the face's plane: keep the last
        if (!g_RollValid || !g_Shown && g_Alpha <= 0.f) { g_Roll = roll; g_RollValid = true; }
        else
        {   // smoothed, the short way round
            float d = roll - g_Roll; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
            g_Roll += d * (1.f - expf(-dt / 0.06f));
        }
        const float yq[3] = { 0.f, cosf(g_Roll), sinf(g_Roll) };
        const float xq[3] = { yq[1] * nG[2] - yq[2] * nG[1], yq[2] * nG[0] - yq[0] * nG[2], yq[0] * nG[1] - yq[1] * nG[0] };   // y x z
        float fq[4]; QuatFromCols(xq, yq, nG, fq);
        // into the layer's space: qx * (pose in the hand frame)
        QRot(qx, off, l.pos);
        l.quat[0] = qx[3] * fq[0] + qx[0] * fq[3] + qx[1] * fq[2] - qx[2] * fq[1];
        l.quat[1] = qx[3] * fq[1] - qx[0] * fq[2] + qx[1] * fq[3] + qx[2] * fq[0];
        l.quat[2] = qx[3] * fq[2] + qx[0] * fq[1] - qx[1] * fq[0] + qx[2] * fq[3];
        l.quat[3] = qx[3] * fq[3] - qx[0] * fq[0] - qx[1] * fq[1] - qx[2] * fq[2];
        l.space = g_W.anchor;
    }
    else g_Shown = false;

    // fade
    if (g_Shown) g_Alpha = g_W.fadeIn > 0 ? std::min(1.f, g_Alpha + dt / g_W.fadeIn) : 1.f;
    else g_Alpha = g_W.fadeOut > 0 ? std::max(0.f, g_Alpha - dt / g_W.fadeOut) : 0.f;
    if (why != 0 && why < 7) g_Alpha = 0.f;   // nothing to show / in a menu: gone at once (looking away fades)
    g_WS.why = why;
    if (g_Shown) g_WS.shown++;

    if (g_Alpha > 0.f && srv)
    {
        float cx, cy, hs; ClusterCentre(tw, th, cx, cy, hs);
        l.srv = srv; l.srcW = tw; l.srcH = th; l.gamma = gamma;
        l.center[0] = cx; l.center[1] = cy; l.half = 0.5f * g_W.cropScale * hs;
        l.edgeFade = 0.03f * hs;
        const float a = g_Alpha * g_Alpha * (3.f - 2.f * g_Alpha);   // smoothstep: eases in and out
        l.alpha = a; l.backdrop = g_W.backdrop; l.size = g_W.size;
        xr.SetWristLayer(l);
        g_WS.submitted++;
    }
}

std::string WristHud::WatchStatus()
{
    // Where the mod DREW the left hand's palm bone (its origin is the wrist joint) relative to the controller, in the
    // same axes as the watch's offsets (x right, y up, z back along the forearm): what the offsets should match.
    std::string drawn = "drawn -";
    float co[3], cr[9], po[3], pr[9], pf[3];
    if (GpuTrace::HandPoseWorld(0, co, cr) && GpuTrace::PalmFrame(0, po, pr, pf))
    {
        const float d[3] = { po[0] - co[0], po[1] - co[1], po[2] - co[2] };
        const float f = d[0] * cr[0] + d[1] * cr[3] + d[2] * cr[6], le = d[0] * cr[1] + d[1] * cr[4] + d[2] * cr[7], u = d[0] * cr[2] + d[1] * cr[5] + d[2] * cr[8];
        drawn = fmt::format("drawnwrist {:.3f} {:.3f} {:.3f}", -le, u, -f);
    }
    std::lock_guard lk(g_WM);
    return fmt::format("watch on {} shown {} alpha {:.2f} why {} facing {:.0f} gaze {:.0f} dist {:.2f} frames {} shownframes {} submitted {} userhud {} {} face {:.3f} {:.3f} {:.3f}",
                       g_WristOn.load(), g_Shown ? 1 : 0, g_Alpha, kWhy[g_WS.why], g_WS.facing, g_WS.gaze, g_WS.dist, g_WS.frames,
                       g_WS.shown, g_WS.submitted, g_UserHud, drawn, -g_W.back, g_W.side, g_W.along);
}

