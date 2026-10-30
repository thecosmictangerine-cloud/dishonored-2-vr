#include "pch.h"
#include "Overlay.h"
#include "GameInfo.h"
#include "CVars.h"
#include "XRProbe.h"
#include "VRRuntime.h"
#include "Reticle.h"
#include "RenderRes.h"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    bool g_Init = false;
    std::atomic<bool> g_Visible{ false };
    HWND g_Hwnd = nullptr;
    WNDPROC g_OrigWndProc = nullptr;
    ID3D11Device* g_Device = nullptr;
    ID3D11DeviceContext* g_Ctx = nullptr;
    ID3D11RenderTargetView* g_RTV = nullptr;
    UINT g_W = 0, g_H = 0;
    double g_Fps = 0; LARGE_INTEGER g_LastT{}; int g_FrameCount = 0;

    // vr window: the window may be larger than the desktop (see Overlay::SetWindowClient)
    std::atomic<bool> g_BigWindow{ false };
    std::atomic<int> g_WantW{ 0 }, g_WantH{ 0 };
    RECT g_OrigRect{}; bool g_OrigSaved = false;

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_GETMINMAXINFO && g_BigWindow.load())
        {
            const LRESULT r = CallWindowProcW(g_OrigWndProc, h, m, w, l);
            MINMAXINFO* mm = (MINMAXINFO*)l;
            mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 16384;
            return r;
        }
        // A lone Alt (or Alt+Space) puts a window into Windows' menu mode: a modal loop inside DefWindowProc on the
        // game's own thread, which stops the game until ESC or a click. Nothing in VR needs the window's system menu,
        // and the mod's own input can produce a lone Alt (a lean key, the focus fallback in KbmInject).
        if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;
        if (m == WM_KEYDOWN && w == VK_INSERT) { g_Visible = !g_Visible; return 0; }
        if (g_Visible)
        {
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            switch (m)
            {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_KEYUP:
            case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_INPUT:
                return 0; // swallow while the overlay owns input
            }
        }
        return CallWindowProcW(g_OrigWndProc, h, m, w, l);
    }

    bool EnsureRTV(IDXGISwapChain* sc)
    {
        if (g_RTV) return true;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
        D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); g_W = d.Width; g_H = d.Height;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        // typeless backbuffers need an explicit view format.
        rd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM :
                    d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : d.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        HRESULT hr = g_Device->CreateRenderTargetView(bb, &rd, &g_RTV);
        bb->Release();
        if (FAILED(hr)) LOGI("Overlay: CreateRTV failed 0x{:08X} fmt={}", (unsigned)hr, (int)d.Format);
        else LOGI("Overlay: backbuffer {}x{} fmt={}", g_W, g_H, (int)d.Format);
        return SUCCEEDED(hr);
    }

    bool Init(IDXGISwapChain* sc)
    {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_Device))) return false;
        g_Device->GetImmediateContext(&g_Ctx);
        DXGI_SWAP_CHAIN_DESC sd; sc->GetDesc(&sd);
        g_Hwnd = sd.OutputWindow;
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui_ImplWin32_Init(g_Hwnd);
        ImGui_ImplDX11_Init(g_Device, g_Ctx);
        g_OrigWndProc = (WNDPROC)SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
        QueryPerformanceCounter(&g_LastT);
        if (GetEnvironmentVariableW(L"DVR_OVERLAY_AUTOSHOW", nullptr, 0)) g_Visible = true; // headless testing
        LOGI("Overlay: initialized on hwnd={} (INSERT toggles)", (void*)g_Hwnd);
        return true;
    }

    void DrawUI()
    {
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("DishonoredVR - stage 0");
        ImGui::Text("Exe %s (%s)  |  %ux%u  |  %.1f fps", GameInfo::Version().c_str(),
                    GameInfo::IsSupportedBuild() ? "supported" : "UNSUPPORTED build", g_W, g_H, g_Fps);
        ImGui::Text("OpenXR probe: %s", XRProbe::Summary().c_str());
        {
            auto st = VRRuntime::GetStatus();
            if (st.initialized)
                ImGui::Text("XR session: %s / %s  state=%d running=%d  eye=%ux%u  frames=%llu",
                            st.runtime.c_str(), st.system.c_str(), st.state, (int)st.running, st.eyeW, st.eyeH, st.framesSubmitted);
            else
                ImGui::Text("XR session: not running (%s)", st.error.c_str());
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("CVars", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (!CVars::Available()) ImGui::TextDisabled("static cvar offsets disabled");
            for (auto& e : CVars::Known())
            {
                int v = CVars::GetInt(e.name);
                ImGui::PushID(e.name);
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputInt(e.name, &v, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) CVars::SetInt(e.name, v);
                ImGui::SameLine(); ImGui::TextDisabled("f=%.3f str=\"%s\"", CVars::GetFloat(e.name), CVars::GetString(e.name).c_str());
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("log", ImVec2(0, 200), true);
            for (auto& l : Log::Tail(60)) ImGui::TextUnformatted(l.c_str());
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        ImGui::End();
    }
}

void Overlay::SetWindowClient(int w, int h) { g_WantW = w; g_WantH = h < 0 ? 0 : h; if (w == 0) g_WantW = -1; }

std::string Overlay::WindowInfo()
{
    RECT c{}, r{}; if (g_Hwnd) { GetClientRect(g_Hwnd, &c); GetWindowRect(g_Hwnd, &r); }
    return fmt::format("backbuffer {}x{} client {}x{} window {},{} {}x{} big {}", g_W, g_H, c.right, c.bottom, r.left, r.top,
                       r.right - r.left, r.bottom - r.top, g_BigWindow.load());
}

namespace
{
    //! On the window's own thread (Present runs on it): resize so the client area is w x h PHYSICAL pixels. The game is
    //! not DPI-aware, so the window API works in scaled units; the scale is the backbuffer over the client rect.
    void ApplyWindowSize()
    {
        const int w = g_WantW.exchange(0), h = g_WantH.exchange(0);
        if (!w || !g_Hwnd) return;
        if (!g_OrigSaved) { GetWindowRect(g_Hwnd, &g_OrigRect); g_OrigSaved = true; }
        if (w < 0)
        {
            g_BigWindow = false;
            SetWindowPos(g_Hwnd, nullptr, g_OrigRect.left, g_OrigRect.top, g_OrigRect.right - g_OrigRect.left, g_OrigRect.bottom - g_OrigRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            LOGI("vr window: back to {}x{}", g_OrigRect.right - g_OrigRect.left, g_OrigRect.bottom - g_OrigRect.top);
            return;
        }
        RECT c{}; GetClientRect(g_Hwnd, &c);
        const float scale = (c.right > 0 && g_W > 0) ? (float)g_W / (float)c.right : 1.f;
        RECT want = { 0, 0, (LONG)lroundf(w / scale), (LONG)lroundf(h / scale) };
        AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtrW(g_Hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(g_Hwnd, GWL_EXSTYLE));
        RECT r{}; GetWindowRect(g_Hwnd, &r);
        g_BigWindow = true;
        // keep the top-left corner on the desktop: the window's centre is where KbmInject puts the cursor for a click
        SetWindowPos(g_Hwnd, nullptr, r.left < 0 ? 0 : r.left, 0, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
        LOGI("vr window: client {}x{} px requested (dpi scale {:.2f}) -> {}", w, h, scale, Overlay::WindowInfo());
    }
}

void Overlay::OnPresent(IDXGISwapChain* sc)
{
    if (!g_Init) { g_Init = Init(sc); if (!g_Init) return; }
    ApplyWindowSize();
    g_FrameCount++;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    double dt = double(now.QuadPart - g_LastT.QuadPart) / f.QuadPart;
    if (dt >= 0.5) { g_Fps = g_FrameCount / dt; g_FrameCount = 0; g_LastT = now; }

    // The reticle rides on the same ImGui pass as the debug overlay, but it has to work with the overlay HIDDEN:
    // this is a gameplay aid, not a debug panel. Drawing here is what puts it in the headset -- OnPresent runs
    // before VRRuntime::OnPresent, so the backbuffer the mod submits already has it.
    const bool reticle = Reticle::Mode() != 0 || Reticle::Axes() != 0 || Reticle::AimDot();
    if (!g_Visible && !reticle) return;
    if (!EnsureRTV(sc)) return;
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    {   // the backbuffer can be larger than the window (RenderRes): draw in its pixels, the mouse mapped into them
        ImGuiIO& io = ImGui::GetIO();
        int cw = 0, ch = 0;
        if (g_W && g_H && RenderRes::RealClient(cw, ch) && ((UINT)cw != g_W || (UINT)ch != g_H))
        {
            const float sx = (float)g_W / cw, sy = (float)g_H / ch;
            io.DisplaySize = ImVec2((float)g_W, (float)g_H);
            if (io.MousePos.x > -FLT_MAX) io.MousePos = ImVec2(io.MousePos.x * sx, io.MousePos.y * sy);
        }
    }
    ImGui::NewFrame();
    if (g_Visible) DrawUI();
    if (reticle) Reticle::Draw();
    ImGui::Render();
    g_Ctx->OMSetRenderTargets(1, &g_RTV, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::OnResize(IDXGISwapChain*)
{
    if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
}

void Overlay::Shutdown()
{
    if (!g_Init) return;
    if (g_Hwnd && g_OrigWndProc) SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)g_OrigWndProc);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_RTV) g_RTV->Release();
    if (g_Ctx) g_Ctx->Release();
    if (g_Device) g_Device->Release();
    g_Init = false;
}

bool Overlay::Visible() { return g_Visible; }
HWND Overlay::Window() { return g_Hwnd; }

