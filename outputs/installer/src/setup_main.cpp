// INSTALL19: SkyBreak-Setup.exe entry point: argument parsing, single instance, Win32 window + Direct3D 11 + Dear ImGui
// main loop, --headless, --ui-snapshot (offscreen, WARP = D3D11 software renderer, no GPU needed) and the
// uninstall self-copy (so the install folder can be removed).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib> // REVINSTALL19: atof
#include <string>
#include <vector>

#include "app.h"
#include "common.h"
#include "engine.h"
#include "headless.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "setup_theme.h"
#include "ui_wic.h" // nwii::ui::DecodeImageWic / SavePngWic (read-only reuse of the launcher's WIC helpers)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
sb::App* g_app = nullptr;

void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
void CleanupRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDevice(HWND hwnd, bool warp, bool with_swapchain) {
    D3D_FEATURE_LEVEL got;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_DRIVER_TYPE types[] = {warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    for (D3D_DRIVER_TYPE t : types) {
        HRESULT hr;
        if (with_swapchain) {
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            hr = D3D11CreateDeviceAndSwapChain(nullptr, t, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        } else {
            hr = D3D11CreateDevice(nullptr, t, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        }
        if (SUCCEEDED(hr)) {
            sb::Log("[d3d] device %s", t == D3D_DRIVER_TYPE_WARP ? "WARP (software)" : "hardware");
            if (with_swapchain) CreateRtv();
            return true;
        }
    }
    return false;
}

void CleanupDevice() {
    CleanupRtv();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resize_w = LOWORD(lParam);
        g_resize_h = HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
        mm->ptMinTrackSize.x = 900;
        mm->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = HDROP(wParam);
        wchar_t buf[MAX_PATH * 4];
        if (DragQueryFileW(d, 0, buf, UINT(std::size(buf))) && g_app) g_app->OnDrop(std::filesystem::path(buf));
        DragFinish(d);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool LoadLogo(int& w, int& h, ID3D11ShaderResourceView** srv) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    const uint8_t* p = static_cast<const uint8_t*>(LockResource(g));
    DWORD n = SizeofResource(nullptr, r);
    if (!p || !n) return false;
    std::vector<uint8_t> bytes(p, p + n);
    nwii::ui::Image img;
    std::string err;
    if (!nwii::ui::DecodeImageWic(bytes, img, &err)) { sb::Log("[logo] %s", err.c_str()); return false; }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(img.w);
    d.Height = UINT(img.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{img.rgba.data(), UINT(img.w * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &tex))) return false;
    HRESULT hr = g_dev->CreateShaderResourceView(tex, nullptr, srv);
    tex->Release();
    w = img.w;
    h = img.h;
    return SUCCEEDED(hr);
}

sb::UiFonts LoadFonts(float s) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    sb::UiFonts f;
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string fonts = sb::U(std::filesystem::path(win) / L"Fonts");
    auto add = [&](const char* file, float px) -> ImFont* {
        std::string path = fonts + "\\" + file;
        if (GetFileAttributesW(sb::W(path).c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        // INSTALL19 10 Oct: Latin-1 + general punctuation (the em dash of the brand line, quotes, ellipsis, bullet) + euro
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x20AC, 0x20AC, 0};
        return io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
    };
    // Segoe UI ships with Windows (not distributed); ImGui's built-in font is the fallback.
    f.body = add("segoeui.ttf", std::round(17 * s));
    f.bold = add("seguisb.ttf", std::round(18 * s));
    f.title = add("seguisb.ttf", std::round(30 * s));
    f.tiny = add("segoeui.ttf", std::round(14 * s));
    if (!f.body) {
        ImFontConfig c;
        c.SizePixels = std::round(15 * s);
        f.body = io.Fonts->AddFontDefault(&c);
    }
    if (!f.bold) f.bold = f.body;
    if (!f.title) f.title = f.bold;
    if (!f.tiny) f.tiny = f.body;
    io.FontDefault = f.body;
    io.Fonts->Build();
    return f;
}

void InitImGui(HWND hwnd, float s, sb::UiFonts& fonts) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    sb::theme::ApplyStyle(s);
    fonts = LoadFonts(s);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
}

std::string Arg(const std::vector<std::wstring>& a, size_t& i) { return i + 1 < a.size() ? sb::U(a[++i]) : std::string(); }

sb::Options ParseArgs(const std::vector<std::wstring>& a, sb::HeadlessIo& io) {
    sb::Options o;
    for (size_t i = 1; i < a.size(); ++i) {
        std::wstring k = a[i];
        if (k == L"--headless") o.headless = true;
        else if (k == L"--answers") o.answers = sb::P(Arg(a, i));
        else if (k == L"--sandbox") o.sandbox = sb::P(Arg(a, i));
        else if (k == L"--mock-prereqs") o.mock_prereqs = sb::P(Arg(a, i));
        else if (k == L"--package") o.package_dir = sb::P(Arg(a, i));
        else if (k == L"--log") o.log = sb::P(Arg(a, i));
        else if (k == L"--result") io.result = sb::P(Arg(a, i));
        else if (k == L"--lang") o.lang = Arg(a, i);
        else if (k == L"--uninstall") o.uninstall = true;
        else if (k == L"--modify") o.modify = true;
        else if (k == L"--verify-package") o.verify_package = true;
        else if (k == L"--delete-saves") o.delete_saves = true;
        else if (k == L"--from-temp") o.from_temp = true;
        else if (k == L"--install-dir") o.install_dir = sb::P(Arg(a, i));
        else if (k == L"--warp") o.warp = true;
        else if (k == L"--ui-snapshot") o.ui_snapshot = sb::P(Arg(a, i));
        else if (k == L"--mutate") o.mutate = Arg(a, i);
        else if (k == L"--build-script") o.build_script = sb::P(Arg(a, i));
        else if (k == L"--no-dialog") o.no_dialog = true;
        else if (k == L"--mock-free-gb") o.mock_free_gb = atof(Arg(a, i).c_str()); // REVINSTALL19 (tests, sandbox only)
        else if (k == L"--wait-closed") o.wait_closed_s = atoi(Arg(a, i).c_str()); // REL22: headless waits for SkyBreak to close
        else if (k == L"--cancel-at") {
            std::string v = Arg(a, i);
            sscanf(v.c_str(), "%d:%d", &o.cancel_at_step, &o.cancel_at_a);
        }
    }
    return o;
}

// --ui-snapshot DIR: every page in NL and EN to PNG, offscreen (no window shown).
int RunSnapshots(sb::Engine& e, HWND hwnd, const sb::fs::path& dir) {
    const UINT W = 1280, H = 800;
    sb::UiFonts fonts;
    InitImGui(hwnd, 1.0f, fonts);
    int lw = 0, lh = 0;
    ID3D11ShaderResourceView* logo = nullptr;
    if (e.opt().mutate != "no-logo") LoadLogo(lw, lh, &logo); // test mutant: snapshots without the embedded logo
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &rt);
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(rt, nullptr, &rtv);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    g_dev->CreateTexture2D(&sd, nullptr, &staging);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    int written = 0;
    for (const char* lang : {"nl", "en"}) {
        for (int p = 0; p <= sb::App::kPageCount + 1; ++p) { // extra images: the consent page over Benodigdheden, REL22: the
                                                                // "SkyBreak is still running" popup over Overzicht
            e.SetLang(lang);
            sb::App app(e, fonts, logo, lw, lh);
            if (p < sb::App::kPageCount) app.SnapshotState(sb::App::Page(p));
            else if (p == sb::App::kPageCount) app.SnapshotConsent("python");
            else app.SnapshotRunning();
            for (int frame = 0; frame < 4; ++frame) {
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(float(W), float(H));
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                app.Frame(float(W), float(H));
                ImGui::Render();
                const float clear[4] = {0.07f, 0.056f, 0.038f, 1.0f};
                g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                g_ctx->ClearRenderTargetView(rtv, clear);
                D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
                g_ctx->RSSetViewports(1, &vp);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            g_ctx->CopyResource(staging, rt);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                nwii::ui::Image img;
                img.w = int(W);
                img.h = int(H);
                img.rgba.resize(size_t(W) * H * 4);
                for (UINT y = 0; y < H; ++y) memcpy(&img.rgba[size_t(y) * W * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, W * 4);
                g_ctx->Unmap(staging, 0);
                for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
                std::string err;
                std::string id = p < sb::App::kPageCount ? sb::App::PageId(sb::App::Page(p)) : p == sb::App::kPageCount ? "4b-toestemming" : "5b-draait-nog";
                sb::fs::path out = dir / sb::P(std::string(lang) + "-" + id + ".png");
                if (nwii::ui::SavePngWic(sb::U(out), img, &err)) ++written;
                else sb::Log("[snapshot] %s: %s", sb::U(out).c_str(), err.c_str());
            }
        }
    }
    sb::Log("[snapshot] %d PNG written to %s", written, sb::U(dir).c_str());
    staging->Release();
    rtv->Release();
    rt->Release();
    if (logo) logo->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return written == 2 * (sb::App::kPageCount + 1) ? 0 : 1;
}

// Uninstall from <install>\setup: continue from a temp copy so the install folder can be deleted.
bool RelaunchFromTemp(const sb::Options& o, const std::vector<std::wstring>& args) {
    if (!o.uninstall || o.from_temp || o.headless || !o.sandbox.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sb::ExeDir() / L"install.json", ec)) return false;
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH, tmp);
    sb::fs::path copy = sb::fs::path(tmp) / (L"SkyBreak-Setup-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(sb::ExePath().c_str(), copy.c_str(), FALSE)) return false;
    std::wstring cl = sb::ProcessRunner::QuoteArg(copy.wstring());
    for (size_t i = 1; i < args.size(); ++i) cl += L" " + sb::ProcessRunner::QuoteArg(args[i]);
    cl += L" --from-temp --install-dir " + sb::ProcessRunner::QuoteArg(sb::ExeDir().parent_path().wstring());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void ScheduleSelfDelete() {
    // the temp copy removes itself a few seconds after exit (no reboot-time delete, which would need admin)
    std::wstring cl = L"cmd.exe /d /c ping -n 4 127.0.0.1 >nul & del /f /q " + sb::ProcessRunner::QuoteArg(sb::ExePath().wstring());
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    sb::HeadlessIo hio;
    sb::Options o = ParseArgs(args, hio);

    sb::fs::path log = o.log;
    if (log.empty() && !o.sandbox.empty()) {
        log = sb::Normal(o.sandbox) / L"setup-logs" / (L"setup-" + sb::W(sb::NowStamp()) + L".log"); // nothing outside the sandbox
        o.log = log;
    }
    if (log.empty()) {
        wchar_t tmp[MAX_PATH + 1];
        GetTempPathW(MAX_PATH, tmp);
        log = sb::fs::path(tmp) / L"SkyBreak-Setup" / (L"setup-" + sb::W(sb::NowStamp()) + L".log");
        o.log = log;
    }
    sb::SetLogFile(log);
    sb::Log("SkyBreak-Setup start: %s", sb::U(std::wstring(GetCommandLineW())).c_str());

    if (RelaunchFromTemp(o, args)) return 0;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, o.ui_snapshot.empty() && !o.headless ? L"Local\\SkyBreakSetup" : nullptr);
    if (o.ui_snapshot.empty() && !o.headless && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"SkyBreak-Setup draait al.", L"SkyBreak", MB_ICONINFORMATION);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    sb::Engine engine(o);
    int rc = 0;
    if (o.headless) {
        rc = sb::RunHeadless(engine, hio);
        if (!o.sandbox.empty()) { // every platform side effect of this run, for the sandbox guard test
            std::string a;
            for (auto& l : engine.plat().AuditLines()) a += l + "\n";
            sb::AppendText(sb::Normal(o.sandbox) / L"audit-platform.txt", a);
        }
        CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return rc;
    }

    // window
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"SkyBreakSetup", LoadIconW(inst, MAKEINTRESOURCEW(1))};
    RegisterClassExW(&wc);
    bool snapshot = !o.ui_snapshot.empty();
    std::wstring title = engine.Lang() == "en" ? L"Install SkyBreak" : L"SkyBreak installeren";
    if (o.uninstall) title = engine.Lang() == "en" ? L"Uninstall SkyBreak" : L"SkyBreak verwijderen";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr,
                                nullptr, inst, nullptr);
    float s = snapshot ? 1.0f : float(GetDpiForWindow(hwnd)) / 96.0f;
    if (!snapshot) SetWindowPos(hwnd, nullptr, 0, 0, int(1100 * s), int(720 * s), SWP_NOMOVE | SWP_NOZORDER);
    if (!CreateDevice(hwnd, o.warp || snapshot, !snapshot)) {
        MessageBoxW(nullptr, L"Direct3D 11 kon niet starten.", L"SkyBreak", MB_ICONERROR);
        return 1;
    }
    if (snapshot) {
        rc = RunSnapshots(engine, hwnd, o.ui_snapshot);
        CleanupDevice();
        DestroyWindow(hwnd);
        CoUninitialize();
        return rc;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    sb::UiFonts fonts;
    InitImGui(hwnd, s, fonts);
    int lw = 0, lh = 0;
    ID3D11ShaderResourceView* logo = nullptr;
    LoadLogo(lw, lh, &logo);
    sb::App app(engine, fonts, logo, lw, lh);
    app.SetHwnd(hwnd);
    g_app = &app;
    if (o.uninstall) app.StartUninstallMode();
    else if (o.modify) app.StartModifyMode();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || app.WantsQuit()) break;
        if (g_resize_w && g_resize_h) {
            CleanupRtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            CreateRtv();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        app.Frame(io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        const float clear[4] = {0.07f, 0.056f, 0.038f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    g_app = nullptr;
    if (logo) logo->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    if (o.from_temp && o.sandbox.empty()) ScheduleSelfDelete();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
