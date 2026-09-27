// gui/main_win32.cpp - window, renderer and file dialogs on Windows.
//
// Everything that needs the operating system lives here. The actual UI in
// app.cpp only knows imgui.h and can therefore be compiled and tested without
// Windows too.
//
// WARNING: This file could not be tested. It was written on a Linux machine
// without the Windows SDK; it was never compiled or run.
// The UI itself (app.cpp) is tested, this binding is not.

#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/update.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <shlobj.h>
#include <tchar.h>
#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <iterator>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device*           g_device = nullptr;
bool                    g_usingWarp = false;

// Startup log.
//
// If the program crashes at startup, the user sees nothing - no window, no
// message, and on someone else's machine there is no way to find out what
// caused it. So every step is recorded BEFORE it runs and the file is closed
// immediately. Whatever is last in it is the step that never finished.
// Where the startup log goes.
//
// Next to the exe - that's where people look for it, and you can point the
// user there without having to explain %APPDATA%.
//
// But not always: if the program is under "Program Files", on a network path
// or on read-only media, writing fails. Then there must be a fallback,
// otherwise there is no log precisely on the machines where something goes
// wrong.
//
// The test is a write attempt, not a permission check: on Windows the
// permissions alone don't reliably tell whether a file will be created
// (virtualization, policies, antivirus).
std::wstring startupLogPath() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring p = exe;
        const std::size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring cand = p.substr(0, slash + 1) + L"g2c-startup.log";
            // Write attempt. If it succeeds, that's the one.
            FILE* probe = nullptr;
            if (_wfopen_s(&probe, cand.c_str(), L"a") == 0 && probe) {
                std::fclose(probe);
                cached = cand;
                return cached;
            }
        }
    }

    // Fallback: %APPDATA%\g2c\startup.log
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return {};
    const std::wstring dir = std::wstring(appdata) + L"\\g2c";
    CreateDirectoryW(dir.c_str(), nullptr);
    cached = dir + L"\\startup.log";
    return cached;
}

void startupLog(const char* step) {
    const std::wstring path = startupLogPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, step);
    std::fclose(f);
}
HRESULT                 g_deviceError = S_OK;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_resize = false;
UINT                    g_resizeW = 0, g_resizeH = 0;
g2::gui::App*           g_app = nullptr;

// --- String conversion ----------------------------------------------------

std::string toUtf8(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

// --- File dialogs ----------------------------------------------------------

// The filter contains embedded null bytes; toWide stops at them.
// So convert it piece by piece.
std::wstring dialogFilter(const char* filter) {
    std::wstring fw;
    if (filter) {
        const char* p = filter;
        while (*p) {
            const std::string part(p);
            fw.append(toWide(part));
            fw.push_back(L'\0');
            p += part.size() + 1;
        }
    }
    fw.push_back(L'\0');
    return fw;
}

// Save dialog: the name doesn't have to exist yet.
//
// "New .car" used to use the open dialog with OFN_FILEMUSTEXIST - a new name
// couldn't be entered with it at all.
std::string saveFileDialog(HWND owner, const char* title, const char* filter,
                           const std::string& startDir, const char* defaultExt) {
    std::vector<wchar_t> buf(4096, L'\0');
    const std::wstring wtitle = toWide(title ? title : "");
    const std::wstring fw = dialogFilter(filter);
    const std::wstring startW = toWide(startDir);
    const std::wstring extW = toWide(defaultExt ? defaultExt : "");

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = fw.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    if (!startW.empty()) ofn.lpstrInitialDir = startW.c_str();
    if (!extW.empty()) ofn.lpstrDefExt = extW.c_str();
    // No OFN_OVERWRITEPROMPT: newCar rejects existing files anyway and says
    // so in the log - nothing is ever overwritten.
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return toUtf8(buf.data());
}

std::vector<std::string> openFilesDialog(HWND owner, const char* title, const char* filter,
                                         bool multi, const std::string& startDir) {
    // GetOpenFileNameW with multiple selection. The buffer must be generous:
    // with many selected files it holds the directory and all names one after
    // another, separated by null bytes.
    std::vector<wchar_t> buf(64 * 1024, L'\0');
    const std::wstring wtitle = toWide(title ? title : "");
    const std::wstring fw = dialogFilter(filter);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = fw.empty() ? nullptr : fw.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    // Start folder. OFN_NOCHANGEDIR is important: without it the dialog
    // changes the program's working directory.
    const std::wstring startW = toWide(startDir);
    if (!startW.empty()) ofn.lpstrInitialDir = startW.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (multi) ofn.Flags |= OFN_ALLOWMULTISELECT;

    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;

    // With single selection the buffer holds one full path. With multiple
    // selection the directory, then the file names - each null-terminated,
    // with a double null byte at the end.
    const wchar_t* p = buf.data();
    const std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        out.push_back(toUtf8(first));
        return out;
    }
    while (*p) {
        std::wstring name(p);
        p += name.size() + 1;
        std::wstring full = first;
        if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
        full += name;
        out.push_back(toUtf8(full));
    }
    return out;
}

// Sets the start folder of an IFileDialog, if it exists.
void setStartFolder(IFileDialog* dlg, const std::string& startDir) {
    if (startDir.empty()) return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(toWide(startDir).c_str(), nullptr,
                                              IID_PPV_ARGS(&item)))) {
        // SetFolder instead of SetDefaultFolder: the last used folder should
        // really open, not just be suggested the very first time.
        dlg->SetFolder(item);
        item->Release();
    }
}

// Select several folders at once.
//
// The same dialog as for one folder, just with FOS_ALLOWMULTISELECT.
// IFileOpenDialog then returns the selection via GetResults as an
// IShellItemArray - GetResult (singular) fails in that case, so it needs a
// function of its own and not just an extra flag.
//
// In the dialog, folders are selected with Ctrl and Shift just like files.
// Small holder for COM pointers.
//
// The dialogs have so far released their objects by hand. That's correct as
// long as nothing throws in between - but toUtf8 and std::string can throw
// when out of memory, and then the object leaks.
//
// A leak in a file dialog is no big deal. Still, doing it right by hand and
// hoping nothing throws is the worse solution when the good one costs three
// lines.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() { return &p_; }
    T*  get() const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

std::vector<std::string> pickFoldersDialog(HWND owner, const char* title,
                                           const std::string& startDir) {
    std::vector<std::string> result;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dlg.put()))))
        return result;

    // .get(): ComPtr deliberately does NOT silently convert to a pointer.
    // That would be convenient, but then the holder could also accidentally
    // be passed to Release() or delete - exactly what it is meant to guard
    // against.
    setStartFolder(dlg.get(), startDir);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    // Deliberately do NOT set FOS_FORCEFILESYSTEM.
    //
    // Combined with FOS_ALLOWMULTISELECT it prevents multiple selection: the
    // dialog then only lets you mark one folder, without any message.
    // That real paths come out is checked when reading the results instead -
    // GetDisplayName(SIGDN_FILESYSPATH) fails for anything that isn't a
    // folder in the file system, and that entry is skipped.
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    if (title) {
        const std::wstring w = toWide(title);
        dlg->SetTitle(w.c_str());
    }

    if (SUCCEEDED(dlg->Show(owner))) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(dlg->GetResults(items.put())) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IShellItem> item;
                if (FAILED(items->GetItemAt(i, item.put())) || !item) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result.push_back(toUtf8(path));
                    CoTaskMemFree(path);
                }
            }
        }
    }
    return result;
}

std::string pickFolderDialog(HWND owner, const char* title, const std::string& startDir) {
    // IFileDialog instead of SHBrowseForFolder: the old dialog is tiny, can't
    // take a typed-in path and has looked like Windows 2000 ever since Vista.
    std::string result;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return result;

    setStartFolder(dlg, startDir);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (title) {
        const std::wstring w = toWide(title);
        dlg->SetTitle(w.c_str());
    }

    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = toUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// System dots per inch, compatible with older Windows versions.
UINT systemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static GetDpiForSystemFn fn = []() -> GetDpiForSystemFn {
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            return reinterpret_cast<GetDpiForSystemFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForSystem")));
        return nullptr;
    }();
    if (fn) return fn();

    const HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

// --- Fonts -----------------------------------------------------------------
//
// Segoe UI covers Latin including umlauts, but no Chinese and no Japanese. So
// for those languages a second font is ADDED ON TOP - via MergeMode both end
// up in the same font atlas, and the Latin characters still come crisply from
// Segoe UI.
//
// Glyph ranges are no longer needed since ImGui 1.92: the atlas loads on
// demand whatever is actually displayed. Before that, 2500 ideographs for
// Chinese and 1946 for Japanese had to be rasterized up front, again on every
// language switch.
// Only load a font if the file is REALLY there.
//
// ImGui raises an IM_ASSERT_USER_ERROR for a missing font file. If the
// assertion is active, the program aborts there - and only on the machines
// that lack the file. That's exactly what it looked like: on Windows 11 it
// ran (SegoeIcons.ttf exists there), on Windows 10 it didn't.
//
// Checking beforehand costs one system call and removes any dependence on
// how ImGui was compiled.
ImFont* addFontIfPresent(ImGuiIO& io, const std::wstring& path, float size,
                         const ImFontConfig* cfg, const ImWchar* ranges) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return io.Fonts->AddFontFromFileTTF(toUtf8(path).c_str(), size, cfg, ranges);
}

void buildFonts(ImGuiIO& io, float dpi, g2::gui::Lang lang) {
    io.Fonts->Clear();

    // Catch nonsensical scaling.
    //
    // At font size 0 ImGui creates no glyphs and crashes while drawing.
    // GetDpiScaleForHwnd normally returns 1.0 to 3.0, but a broken driver or
    // an unusual multi-monitor setup can report 0 - and then the crash is far
    // away from its cause.
    if (!(dpi > 0.1f) || dpi > 8.0f) dpi = 1.0f;

    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) {
        io.Fonts->AddFontDefault();
        return;
    }
    const std::wstring fontDir = std::wstring(winDir) + L"\\Fonts\\";
    const float size = 17.0f * dpi;

    startupLog("  Grundschrift segoeui.ttf");
    // No more glyph ranges.
    //
    // Since ImGui 1.92 the font atlas loads glyphs on demand, as long as the
    // backend supports ImGuiBackendFlags_RendererHasTextures - the DX11
    // backend does. All GetGlyphRangesXXX() are therefore obsolete.
    //
    // The gain is not cosmetic: for Chinese 2500 and for Japanese 1946
    // ideographs used to be rasterized up front, again on every language
    // switch. Now only what is actually displayed gets created.
    ImFont* base = addFontIfPresent(io, fontDir + L"segoeui.ttf", size, nullptr, nullptr);
    if (!base) {
        io.Fonts->AddFontDefault();
        return;
    }

    // Add a second font on top. Which file it is depends on the language; all
    // three ship with Windows.
    const wchar_t* cjkFile = nullptr;
    switch (lang) {
        case g2::gui::Lang::Zh: cjkFile = L"msyh.ttc"; break;      // Microsoft YaHei
        case g2::gui::Lang::Ja: cjkFile = L"YuGothM.ttc"; break;   // Yu Gothic Medium
        default: break;
    }

    if (cjkFile) {
        startupLog("  CJK-Schrift");
        ImFontConfig cfg;
        cfg.MergeMode = true;          // merge into the same font atlas
        cfg.OversampleH = 1;           // CJK glyphs are large, this is enough
        cfg.OversampleV = 1;
        if (!addFontIfPresent(io, fontDir + cjkFile, size, &cfg, nullptr)) {
            // Fallback fonts in case the preferred one is missing. Better a
            // different font than empty boxes.
            for (const wchar_t* alt : {L"meiryo.ttc", L"msgothic.ttc", L"simsun.ttc",
                                       L"malgun.ttf"}) {
                if (addFontIfPresent(io, fontDir + alt, size, &cfg, nullptr)) break;
            }
        }
    }

    // Add the icon font on top.
    //
    // Windows 11 ships "Segoe Fluent Icons", Windows 10 only "Segoe MDL2
    // Assets". Both cover the same range, the Fluent variant has rounder
    // corners. Try the newer one first.
    {
        // Only request the range that is actually used.
        //
        // This used to be E700 to F8FF, i.e. 4608 characters - of which twenty
        // are needed. The font atlas builds a lookup table for the whole
        // range, and that's memory and time for nothing.
        static const ImWchar iconRange[] = {static_cast<ImWchar>(g2::gui::kIconUsedMin),
                                            static_cast<ImWchar>(g2::gui::kIconUsedMax), 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        // Slightly smaller than the font: the icons are drawn at full line
        // height and otherwise look heavier than the text.
        cfg.GlyphOffset.y = 1.0f * dpi;

        startupLog("  Symbolschrift");
        bool ok = false;
        for (const wchar_t* f : {L"SegoeIcons.ttf", L"segmdl2.ttf"}) {
            if (addFontIfPresent(io, fontDir + f, size * 0.86f, &cfg, iconRange)) {
                ok = true;
                break;
            }
        }
        // If the font is missing, the icons are left out instead of being
        // shown as empty boxes.
        g2::gui::setIconsAvailable(ok);
    }

    startupLog("  Zeichensatz aufbauen");
    if (!io.Fonts->Build()) {
        // If the build fails, the font atlas is unusable. Carry on with the
        // default font instead of going into drawing with an empty atlas -
        // there the crash would be far away from its cause.
        startupLog("  Zeichensatz fehlgeschlagen - Standardschrift");
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        g2::gui::setIconsAvailable(false);
    }
}

// --- DirectX ---------------------------------------------------------------

bool createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr) && g_rtv != nullptr;
}

void releaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    // First the graphics card, then the software rasterizer.
    //
    // Without the fallback the program doesn't start at all on machines
    // without a D3D11-capable driver - and that affects more cases than you'd
    // think: virtual machines, Remote Desktop, old or missing graphics
    // drivers. WARP is part of Windows and always present; for a UI made of
    // lines and text it is entirely sufficient.
    // Do NOT go below 10_0.
    //
    // ImGui's DX11 binding compiles its shaders with "vs_4_0", which is a
    // target for feature level 10.0 and up. On 9_1 or 9_3 the device can be
    // created, but the shaders fail - the window shows up white and the
    // program crashes. Level 9.x would need the "vs_4_0_level_9_x" profiles,
    // which ImGui doesn't use.
    //
    // Creating a device that then can't draw anything is worse than none at
    // all: with the latter at least the error message appears.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    const D3D_DRIVER_TYPE tries[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};

    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE t : tries) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, t, nullptr, 0, levels,
                                           static_cast<UINT>(std::size(levels)),
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got,
                                           &g_context);
        if (hr == S_OK) {
            g_usingWarp = (t == D3D_DRIVER_TYPE_WARP);
            break;
        }
    }
    if (hr != S_OK) {
        g_deviceError = hr;
        return false;
    }
    if (!createRenderTarget()) {
        g_deviceError = E_FAIL;
        return false;
    }
    return true;
}

void destroyDevice() {
    releaseRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ID of the WM_COPYDATA message "open these files" ("G2C1").
constexpr ULONG_PTR kOpenFilesMessage = 0x47324331;

// Open files in the running UI - the same path for dropping onto the window
// and for files handed over by a second program launch.
//
// Append .xsi files in one batch, everything else via openPath, i.e. exactly
// like dropping onto the exe. Previously .gla and anims.h dropped onto the
// window were silently ignored, and a path that did not (or no longer) exist
// counted as a folder, because INVALID_FILE_ATTRIBUTES has the directory bit
// set.
void openPaths(const std::vector<std::string>& paths) {
    if (!g_app) return;
    std::vector<std::string> xsi;
    for (const std::string& p : paths) {
        std::string lower = p;
        for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".xsi") == 0)
            xsi.push_back(p);
        else
            g_app->openPath(p);
    }
    if (!xsi.empty()) g_app->addXsiFiles(xsi, false);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
            g_resize = true;
            return 0;

        case WM_DROPFILES: {
            // Files dropped onto the window: .car files are opened, .xsi files
            // appended to the current script.
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path))))
                    paths.push_back(toUtf8(path));
            }
            DragFinish(drop);
            openPaths(paths);
            return 0;
        }

        case WM_COPYDATA: {
            // A second launch of g2c - e.g. by double-clicking a .car - hands
            // its files over to here instead of opening a second window with
            // all the tabs. See forwardToRunningInstance.
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!cds || cds->dwData != kOpenFilesMessage || !g_app) break;
            std::vector<std::string> paths;
            const char* p = static_cast<const char*>(cds->lpData);
            const char* end = p + cds->cbData;
            while (p < end) {
                const std::size_t n = strnlen(p, static_cast<std::size_t>(end - p));
                if (n) paths.emplace_back(p, n);
                p += n + 1;
            }
            openPaths(paths);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }

        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // suppress the Alt menu
            break;

        case WM_CLOSE:
            // Don't destroy right away. The window used to close immediately:
            // unsaved changes were gone without asking, and afterwards
            // saveWindowPlacement found no window any more, so the window
            // placement was never saved.
            //
            // Now the UI decides. Without changes the main loop ends
            // immediately; otherwise a dialog asks, and the loop ends as soon
            // as it is answered. The window is only destroyed after that, at
            // the end of wWinMain.
            if (!g_app || g_app->requestQuit()) PostQuitMessage(0);
            // Closed from the taskbar while minimized: nothing is drawn then,
            // so bring the window back or the question would stay invisible.
            else if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// --- Remember window placement ---------------------------------------------
//
// GetWindowPlacement instead of GetWindowRect: besides the position the struct
// also knows the state (normal, maximized, minimized) AND the size the window
// would have when not maximized. With GetWindowRect alone a maximized window
// would be screen-sized but not maximized on the next start - and on restore
// it would stay huge.
//
// A minimized window is saved as normal: nobody wants to start a program that
// immediately vanishes into the taskbar.
std::string placementFile() {
    return (std::filesystem::path(g2::gui::App::configDir()) / "g2c_window.txt").string();
}

void saveWindowPlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;

    UINT show = wp.showCmd;
    if (show == SW_SHOWMINIMIZED) show = SW_SHOWNORMAL;

    std::ofstream f(placementFile());
    if (!f) return;
    const RECT& r = wp.rcNormalPosition;
    f << "show=" << show << "\n"
      << "left=" << r.left << "\n"
      << "top=" << r.top << "\n"
      << "right=" << r.right << "\n"
      << "bottom=" << r.bottom << "\n";
}

bool restoreWindowPlacement(HWND hwnd) {
    std::ifstream f(placementFile());
    if (!f) return false;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    wp.showCmd = SW_SHOWNORMAL;
    RECT r{0, 0, 0, 0};

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        long v = 0;
        try {
            v = std::stol(line.substr(eq + 1));
        } catch (...) {
            continue;
        }
        if (k == "show") wp.showCmd = static_cast<UINT>(v);
        else if (k == "left") r.left = v;
        else if (k == "top") r.top = v;
        else if (k == "right") r.right = v;
        else if (k == "bottom") r.bottom = v;
    }

    if (r.right - r.left < 400 || r.bottom - r.top < 300) return false;

    // If the window lies outside all screens, don't restore it.
    //
    // That happens after unplugging a second monitor: the window would be
    // there, but invisible and unreachable.
    const HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;

    wp.rcNormalPosition = r;
    return SetWindowPlacement(hwnd, &wp) != 0;
}

// Command-line entry point, lives in tools/g2c.cpp.
int g2cMain(int argc, char** argv);

namespace {

// Is the first argument a command-line command?
//
// Otherwise the meaning would be: "open these files in the UI". That's the
// case when a .car is dropped onto the exe - then you want to see the window,
// not a console window.
bool looksLikeCommand(const char* a) {
    // The same list as in g2cMain. export, makecar and about were missing:
    // they opened the UI, even though the error message on graphics startup
    // recommends exactly "g2c about".
    static const char* kCmds[] = {"build", "anim",   "mesh",    "info",  "check", "xsi",
                                  "car",   "validate", "diff",  "scan",  "export", "makecar",
                                  "about", "version", "-h",     "--help", "/?",   "help",
                                  "-v",    "--version"};
    for (const char* c : kCmds)
        if (std::strcmp(a, c) == 0) return true;
    return false;
}

// Attach output to the calling command prompt.
//
// The program is built as a GUI application - otherwise a black console
// window would flash up on every double-click. GUI applications have no
// console, though; without AttachConsole the command-line version would run
// silently. ATTACH_PARENT_PROCESS attaches it to the console it was started
// from. If that doesn't work (e.g. launched by double-click), a console of its
// own is opened so the output doesn't get lost.
void attachOrOpenConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
    }
    // Don't bend redirected output ("g2c info a.gla > out.txt") back to the
    // console. stdout used to always be reopened on CONOUT$, and the file
    // stayed empty.
    const auto redirected = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        if (!h || h == INVALID_HANDLE_VALUE) return false;
        const DWORD t = GetFileType(h);
        return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
    };
    FILE* f = nullptr;
    if (!redirected(STD_OUTPUT_HANDLE)) freopen_s(&f, "CONOUT$", "w", stdout);
    if (!redirected(STD_ERROR_HANDLE)) freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The command prompt has already printed its prompt line again; without
    // this line break our output would start in the middle of the line.
    std::printf("\n");
}

// Arguments as UTF-8, regardless of the entry point.
//
// With wWinMain __argv is EMPTY - with a Unicode entry point the runtime
// library only fills __wargv. Accessing __argv[1] therefore hits a null
// pointer, and the program crashes BEFORE the window appears. From the
// outside it looks as if nothing happens.
//
// CommandLineToArgvW is also the only reliable way for paths with umlauts or
// other characters outside the code page - and those are exactly the kind of
// paths people drop onto a program.
class Args {
public:
    Args() {
        int n = 0;
        wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
        if (!w) return;
        storage_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) storage_.push_back(toUtf8(w[i]));
        LocalFree(w);
        for (auto& s : storage_) pointers_.push_back(s.data());
        pointers_.push_back(nullptr);   // argv is null-terminated
    }

    // Started with "-reset" - should the saved state be discarded?
    //
    // Without this escape hatch there is none when a settings file prevents
    // startup: the program crashes at the same spot on every attempt, and the
    // user can't find the folder.
    bool hasNoFont() const {
        for (const auto& a : storage_)
            if (a == "-nofont" || a == "--nofont" || a == "/nofont") return true;
        return false;
    }

    bool hasReset() const {
        for (const auto& a : storage_)
            if (a == "-reset" || a == "--reset" || a == "/reset") return true;
        return false;
    }

    int    count() const { return static_cast<int>(storage_.size()); }
    char** argv() { return pointers_.data(); }
    const char* at(int i) const {
        return i >= 0 && i < count() ? storage_[static_cast<std::size_t>(i)].c_str() : "";
    }

    // Files among the arguments, without switches like -reset or -nofont.
    // Those used to be opened as paths too and produced a "not found" message
    // at startup.
    std::vector<std::string> files() const {
        std::vector<std::string> out;
        for (std::size_t i = 1; i < storage_.size(); ++i)
            if (!storage_[i].empty() && storage_[i][0] != '-' && storage_[i][0] != '/')
                out.push_back(storage_[i]);
        return out;
    }

private:
    std::vector<std::string> storage_;
    std::vector<char*>       pointers_;
};

// Is g2c already running? Then send the files there instead of opening a
// second window.
//
// Double-clicking a .car while g2c was open used to start a second window that
// again loaded all the tabs open last time. Both wrote their settings on exit,
// and the one closed last won.
//
// The running window opens the file - or jumps to its tab if it is already
// open - and comes to the front. If it doesn't answer (older version, hung),
// this program starts completely normally.
bool forwardToRunningInstance(const Args& args) {
    if (args.hasReset()) return false;
    const auto files = args.files();
    if (files.empty()) return false;
    const HWND other = FindWindowW(L"g2cWindow", nullptr);
    if (!other) return false;

    // Make absolute: the other program has a different working directory.
    std::string data;
    for (const auto& f : files) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(std::filesystem::path(toWide(f)), ec);
        data += ec ? f : toUtf8(abs.wstring());
        data.push_back('\0');
    }

    COPYDATASTRUCT cds{};
    cds.dwData = kOpenFilesMessage;
    cds.cbData = static_cast<DWORD>(data.size());
    cds.lpData = data.data();

    // The other window is allowed to bring itself to the front - without this
    // permission it would only flash in the taskbar.
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);

    DWORD_PTR result = 0;
    const LRESULT ok = SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                           SMTO_ABORTIFHUNG, 5000, &result);
    return ok != 0 && result == TRUE;
}

// --- HTTP for the updater -------------------------------------------------
//
// WinHTTP ships with every Windows, uses the system's proxy settings and
// certificate store, and follows the redirect from github.com to the storage
// host on its own. No library needed.

// Test hook: G2C_UPDATE_TEST_SERVER=http://127.0.0.1:8765 sends the requests
// for api.github.com and github.com there instead. The updater still checks
// the ORIGINAL URLs, so the trust rules are exercised unchanged.
std::string redirectForTest(const std::string& url) {
    wchar_t buf[512];
    const DWORD n = GetEnvironmentVariableW(L"G2C_UPDATE_TEST_SERVER", buf, 512);
    if (n == 0 || n >= 512) return url;
    const std::string server = toUtf8(buf);
    for (const char* host : {"https://api.github.com", "https://github.com"})
        if (url.rfind(host, 0) == 0) return server + url.substr(std::strlen(host));
    return url;
}

std::string winHttpError(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                       FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   GetModuleHandleW(L"winhttp.dll"), code, 0,
                                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = n && text ? toUtf8(text) : std::string();
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? "error " + std::to_string(code) : out;
}

class HttpGet {
public:
    // Sends the request and waits for the headers. status() == 0 means it failed.
    explicit HttpGet(const std::string& originalUrl) { open(originalUrl); }

    ~HttpGet() {
        for (HINTERNET h : {request_, connect_, session_})
            if (h) WinHttpCloseHandle(h);
    }

    HttpGet(const HttpGet&) = delete;
    HttpGet& operator=(const HttpGet&) = delete;

    int                status() const { return status_; }
    const std::string& error() const { return error_; }
    std::uint64_t      length() const { return length_; }

    // Next piece of the body. 0 = end, -1 = error.
    long long read(char* buf, DWORD size) {
        DWORD got = 0;
        if (!WinHttpReadData(request_, buf, size, &got)) {
            fail();
            return -1;
        }
        return got;
    }

private:
    void open(const std::string& originalUrl) {
        const std::string url = redirectForTest(originalUrl);
        const std::wstring wurl = toWide(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail();
        const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                                  std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

        const std::wstring agent = L"g2c/" + toWide(g2::gui::update::thisBuild().version);
        session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return fail();
        // Resolve, connect, send, receive. A dead network must not keep the
        // program from closing for minutes.
        WinHttpSetTimeouts(session_, 10000, 10000, 15000, 30000);

        connect_ = WinHttpConnect(session_, host.c_str(), uc.nPort, 0);
        if (!connect_) return fail();
        const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request_ = WinHttpOpenRequest(connect_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request_) return fail();
        if (originalUrl.rfind("https://api.github.com/", 0) == 0)
            WinHttpAddRequestHeaders(request_, L"Accept: application/vnd.github+json\r\n",
                                     static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request_, nullptr))
            return fail();

        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX))
            return fail();
        status_ = static_cast<int>(status);

        DWORD length = 0;
        size = sizeof(length);
        if (WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &size,
                                WINHTTP_NO_HEADER_INDEX))
            length_ = length;
    }

    void fail() {
        error_ = winHttpError(GetLastError());
        status_ = 0;
    }

    HINTERNET     session_ = nullptr;
    HINTERNET     connect_ = nullptr;
    HINTERNET     request_ = nullptr;
    int           status_ = 0;
    std::uint64_t length_ = 0;
    std::string   error_;
};

g2::gui::update::HttpResult httpGet(const std::string& url, std::size_t maxBytes) {
    g2::gui::update::HttpResult out;
    HttpGet req(url);
    out.status = req.status();
    out.error = req.error();
    if (out.status == 0) return out;
    char buf[16384];
    for (;;) {
        const long long n = req.read(buf, sizeof(buf));
        if (n < 0) return {0, {}, req.error()};
        if (n == 0) break;
        out.body.append(buf, static_cast<std::size_t>(n));
        if (out.body.size() > maxBytes) return {0, {}, "answer too large"};
    }
    return out;
}

std::string httpDownload(const std::string& url, const std::filesystem::path& dest,
                         const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    HttpGet req(url);
    if (req.status() == 0) return req.error();
    if (req.status() != 200) return "HTTP " + std::to_string(req.status());
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot write " + toUtf8(dest.filename().wstring());
    std::vector<char> buf(1 << 16);
    std::uint64_t done = 0;
    for (;;) {
        const long long n = req.read(buf.data(), static_cast<DWORD>(buf.size()));
        if (n < 0) return req.error();
        if (n == 0) break;
        out.write(buf.data(), static_cast<std::streamsize>(n));
        if (!out) return "cannot write " + toUtf8(dest.filename().wstring());
        done += static_cast<std::uint64_t>(n);
        if (progress && !progress(done, req.length())) return "cancelled";
    }
    out.close();
    return out ? std::string() : "cannot write " + toUtf8(dest.filename().wstring());
}

std::filesystem::path exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

// Set when the user chose "Restart now" after an update. The new process is
// started at the very end of wWinMain: after the App has saved the settings
// and after the window is gone - otherwise the new instance would find the
// old window and hand its files over to it.
bool g_relaunch = false;

void relaunchSelf() {
    const std::wstring exe = exePath().wstring();
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // The user just clicked "Restart now", so this process may still bring
    // windows to the front - pass that on, or the new window opens behind
    // whatever else is on the screen.
    AllowSetForegroundWindow(ASFW_ANY);
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// "g2c update [-check] [-stable|-snapshot]": the same updater as the update
// bar, for the command line. Lives here and not in tools/g2c.cpp because only
// this exe has WinHTTP - g2c-cli.exe is built for other systems too.
int cmdUpdate(const Args& args) {
    namespace upd = g2::gui::update;
    bool checkOnly = false;
    const upd::BuildInfo build = upd::thisBuild();
    upd::Channel channel = upd::defaultChannel(build);
    for (int i = 2; i < args.count(); ++i) {
        const std::string a = args.at(i);
        if (a == "-check") checkOnly = true;
        else if (a == "-stable") channel = upd::Channel::Stable;
        else if (a == "-snapshot") channel = upd::Channel::Snapshot;
        else {
            std::printf("Unbekannte Option: %s\n  g2c update [-check] [-stable|-snapshot]\n",
                        a.c_str());
            return 2;
        }
    }

    // Leftovers of the previous "g2c update" - that one could not delete the
    // exe it was running from.
    upd::cleanupAfterUpdate(exePath());

    upd::Updater u({httpGet, httpDownload}, build, exePath());
    std::printf("Installiert : %s\n", upd::displayName(build).c_str());
    u.check(channel, true);
    u.wait();
    upd::Status st = u.status();
    if (st.phase == upd::Phase::Failed) {
        std::printf("%s\n", g2::gui::describeUpdateError(st).c_str());
        return 1;
    }
    std::printf("Verfuegbar  : %s\n", upd::displayName(st.release, channel).c_str());
    if (st.phase == upd::Phase::UpToDate) {
        std::printf("g2c ist aktuell.\n");
        return 0;
    }
    if (checkOnly) {
        std::printf("Installieren mit: g2c update\n");
        return 0;
    }

    u.install();
    const auto progress = [&] {
        st = u.status();
        std::printf("\rLade herunter: %.1f / %.1f MB   ", static_cast<double>(st.done) / 1048576.0,
                    static_cast<double>(st.total) / 1048576.0);
        std::fflush(stdout);
    };
    while (u.busy()) {
        progress();
        ::Sleep(200);
    }
    u.wait();
    progress();   // the final state, even if the download was too fast to see
    std::printf("\n");
    if (st.phase != upd::Phase::Installed) {
        std::printf("%s\n", g2::gui::describeUpdateError(st).c_str());
        return 1;
    }
    if (!st.detail.empty()) std::printf("Hinweis: %s\n", st.detail.c_str());
    std::printf("Installiert. Der naechste Start verwendet %s.\n",
                upd::displayName(st.release, channel).c_str());
    return 0;
}

}  // namespace

int wWinMainGuarded(HINSTANCE inst);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Catch every exception and SHOW it.
    //
    // Without this the program exits silently on an error while loading the
    // settings or restoring the tabs - and to the user, a window that closes
    // again immediately is indistinguishable from a broken download.
    try {
        const int rc = wWinMainGuarded(inst);
        // Here the App is destroyed (settings saved) and the window is gone.
        if (g_relaunch) relaunchSelf();
        return rc;
    } catch (const std::exception& e) {
        const std::wstring what = toWide(std::string("g2c wurde beendet:\n\n") + e.what());
        MessageBoxW(nullptr, what.c_str(), L"g2c", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"g2c wurde durch einen unbekannten Fehler beendet.", L"g2c",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}

int wWinMainGuarded(HINSTANCE inst) {
    // Start fresh on every launch: nobody reads a file that grows endlessly,
    // and only the last attempt is of interest.
    {
        const std::wstring p = startupLogPath();
        if (!p.empty()) DeleteFileW(p.c_str());
    }
    startupLog("Start");
    Args args;
    // With a command as the first argument, the same exe works as a
    // command-line tool. A file as the argument, on the other hand, opens the
    // UI with that file.
    if (args.count() > 1 && std::string(args.at(1)) == "update") {
        attachOrOpenConsole();
        const int rc = cmdUpdate(args);
        std::fflush(stdout);
        return rc;
    }
    if (args.count() > 1 && looksLikeCommand(args.at(1))) {
        attachOrOpenConsole();
        const int rc = g2cMain(args.count(), args.argv());
        std::fflush(stdout);
        return rc;
    }
    if (forwardToRunningInstance(args)) {
        startupLog("an das laufende Fenster weitergereicht");
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // MUST come before the window is created.
    //
    // Without this declaration Windows treats the application as an old one
    // with a fixed resolution of 96 dpi: it draws small and Windows scales the
    // finished image up. The result is blurry throughout - text and lines
    // alike. The right way is to draw at the screen's resolution in the first
    // place.
    startupLog("DPI-Bewusstsein setzen");
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Icon from our own resources. hIcon is the large one (Alt+Tab), hIconSm
    // the small one in the title bar - specify both, otherwise Windows scales
    // the large one down and it turns blurry.
    //
    // LoadImageW with SM_CXSMICON specifically fetches the 16-pixel version
    // from the .ico. LoadIconW always returns the large one; for the title bar
    // that would be a downscaled and therefore mushy rendering.
    //
    // ID 1, matching icon/g2c.rc: for the icon in Explorer, Windows takes the
    // icon with the LOWEST ID in the exe.
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"g2cWindow";
    startupLog("Fensterklasse anmelden");
    RegisterClassExW(&wc);

    // Window size in pixels, so it doesn't look tiny on high-resolution
    // screens.
    //
    // GetDpiForSystem only exists from Windows 10 1607 on. Linked statically,
    // the program doesn't start at all on older systems - it already fails at
    // load time with "entry point not found", before a single line has run.
    // So look it up at runtime and otherwise take the old route via the device
    // context, which has always existed.
    const UINT dpiRaw = systemDpi();
    const int  w = MulDiv(1500, static_cast<int>(dpiRaw), 96);
    const int  h = MulDiv(950, static_cast<int>(dpiRaw), 96);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"g2c \x2014 Ghoul2 Toolkit", WS_OVERLAPPEDWINDOW,
                              100, 100, w, h, nullptr, nullptr, inst, nullptr);
    startupLog("DirectX-Geraet erzeugen");
    if (!createDevice(hwnd)) {
        destroyDevice();
        UnregisterClassW(wc.lpszClassName, inst);

        // Do NOT exit silently.
        //
        // To the user, a window that closes again immediately after a
        // double-click is indistinguishable from a broken download. Say what
        // is missing.
        wchar_t msg[512];
        _snwprintf_s(msg, _TRUNCATE,
                     L"g2c konnte die Grafikausgabe nicht starten.\n\n"
                     L"Fehlercode: 0x%08X\n\n"
                     L"Weder die Grafikkarte noch der Software-Rasterisierer (WARP) "
                     L"liessen sich verwenden.\n\n"
                     L"Moegliche Ursachen:\n"
                     L"  - Grafiktreiber fehlt oder ist veraltet\n"
                     L"  - Windows-Fassung aelter als 10\n"
                     L"  - Remotedesktop ohne Grafikweiterleitung\n\n"
                     L"Die Kommandozeile funktioniert unabhaengig davon:\n"
                     L"  g2c build ... / g2c export ... / g2c about\n\n"
                     L"Protokoll: %s",
                     static_cast<unsigned>(g_deviceError), startupLogPath().c_str());
        MessageBoxW(nullptr, msg, L"g2c", MB_OK | MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(hwnd, TRUE);
    if (!restoreWindowPlacement(hwnd)) ShowWindow(hwnd, SW_SHOWDEFAULT);
    startupLog("Fenster anzeigen");
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    startupLog("ImGui-Kontext anlegen");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Put window state and column widths next to the settings, not into the
    // working directory.
    static std::string iniPath;
    iniPath = (std::filesystem::path(g2::gui::App::configDir()) / "g2c_gui.ini").string();
    io.IniFilename = iniPath.c_str();

    // Determine the scaling of the screen the window is on.
    bool firstFrame = true;
    startupLog("DPI ermitteln");
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    // With "g2c -nofont" only use the built-in font.
    //
    // The Windows fonts are the only part of startup that depends on files on
    // someone else's machine - a damaged or replaced segoeui.ttf can't be
    // ruled out. This switch skips them entirely.
    if (args.hasNoFont()) {
        startupLog("Schriften ueberspringen (-nofont)");
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        g2::gui::setIconsAvailable(false);
    } else {
        startupLog("Schriften laden");
        buildFonts(io, dpi, g2::gui::language());
    }

    startupLog("Win32-Anbindung starten");
    ImGui_ImplWin32_Init(hwnd);
    startupLog("DirectX-Anbindung starten");
    ImGui_ImplDX11_Init(g_device, g_context);
    startupLog("Anbindungen bereit");

    g2::gui::Platform plat;
    plat.openFiles = [hwnd](const char* title, const char* filter, bool multi,
                            const std::string& startDir) {
        return openFilesDialog(hwnd, title, filter, multi, startDir);
    };
    plat.pickFolders = [hwnd](const char* title, const std::string& startDir) {
        return pickFoldersDialog(hwnd, title, startDir);
    };
    plat.pickFolder = [hwnd](const char* title, const std::string& startDir) {
        return pickFolderDialog(hwnd, title, startDir);
    };
    plat.saveFile = [hwnd](const char* title, const char* filter, const std::string& startDir,
                           const char* defaultExt) {
        return saveFileDialog(hwnd, title, filter, startDir, defaultExt);
    };
    plat.revealInExplorer = [](const std::string& path) {
        const std::wstring w = toWide(path);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w + L"\"").c_str(),
                      nullptr, SW_SHOWNORMAL);
    };
    plat.network.get = httpGet;
    plat.network.download = httpDownload;
    plat.exePath = exePath();
    plat.openUrl = [](const std::string& url) {
        // Only web pages - never hand ShellExecute a path or a program.
        if (url.rfind("https://", 0) != 0) return;
        ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };

    // The most likely crash point after the graphics: this is where the saved
    // settings are read and the scripts opened last time are restored. A
    // damaged file in %APPDATA%\g2c strikes exactly here.
    // With "g2c -reset" the saved state can be skipped.
    //
    // Without it there is no way out when a settings file prevents startup:
    // the program crashes at the same spot on every attempt, and the user
    // can't find the folder.
    if (args.hasReset()) {
        startupLog("gespeicherten Zustand verwerfen (-reset)");
        // The same folder that is read from - in portable mode it lives next
        // to the exe, not under %APPDATA%. And the window file is called
        // g2c_gui.ini; previously an imgui.ini was deleted, which never
        // existed.
        const std::filesystem::path dir(g2::gui::App::configDir());
        for (const char* f : {"g2c_settings.txt", "g2c_window.txt", "g2c_gui.ini"}) {
            std::error_code ec;
            std::filesystem::remove(dir / f, ec);
        }
    }

    startupLog("Einstellungen laden und Skripte wiederherstellen");
    g2::gui::App app(std::move(plat));
    app.settings().dpiScale = dpi;

    // Open files dropped onto the exe or passed as arguments.
    for (const auto& f : args.files()) app.openPath(f);
    // Tell the UI where the log is - it doesn't determine it itself, so that
    // the information and the file actually being written can't drift apart.
    app.setLogPath(toUtf8(startupLogPath()));

    g_app = &app;

    bool running = true;
    bool occluded = false;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // Minimized or screen locked: nothing to show. Present does not wait
        // for the display then, and the loop used to spin at about 60 % of a
        // core the whole time the window sat in the taskbar. Builds run on
        // their own threads and are not slowed down by this.
        if (IsIconic(hwnd) ||
            (occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            ::Sleep(15);
            continue;
        }
        occluded = false;

        if (g_resize) {
            releaseRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resize = false;
            createRenderTarget();
        }

        // Language switch: the font atlas must be rebuilt with different
        // glyph ranges. The renderer's textures depend on it, so release them
        // first, then rebuild.
        if (app.fontsDirty()) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            buildFonts(io, dpi, g2::gui::language());
            ImGui_ImplDX11_CreateDeviceObjects();
            app.clearFontsDirty();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.draw();

        // Window close button with unsaved changes: the UI has asked, and
        // the answer was "save" or "discard".
        if (app.quitApproved()) running = false;

        ImGui::Render();
        const float clear[4] = {0.09f, 0.09f, 0.10f, 1.0f};
        // Don't draw anything without a render target.
        //
        // If creating it fails - e.g. after a driver restart - g_rtv would be
        // null. OMSetRenderTargets with null draws into nothing: the window
        // stays white, and the next access crashes.
        if (!g_rtv) {
            ::Sleep(16);
            continue;
        }
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Catch a driver restart.
        //
        // If the graphics driver fails and is restarted by Windows (equally
        // common with NVIDIA, AMD and Intel, e.g. after a driver update while
        // running), Present reports DEVICE_REMOVED or DEVICE_RESET. After
        // that EVERY further call fails - without a check the user sees a
        // white window and then a crash.
        if (firstFrame) {
            startupLog("erstes Bild gezeichnet - ab hier laeuft alles");
            firstFrame = false;
        }
        const HRESULT pr = g_swapChain->Present(1, 0);   // vsync
        occluded = pr == DXGI_STATUS_OCCLUDED;
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            const HRESULT reason =
                (pr == DXGI_ERROR_DEVICE_REMOVED && g_device) ? g_device->GetDeviceRemovedReason()
                                                              : pr;
            wchar_t text[512];
            _snwprintf_s(text, _TRUNCATE,
                         L"Der Grafiktreiber wurde zurueckgesetzt und g2c muss beendet "
                         L"werden.\n\nCode: 0x%08X\n\n"
                         L"Ungespeicherte Aenderungen an den Skripten gehen verloren.\n"
                         L"Haeufigste Ursache ist ein Treiberupdate im laufenden Betrieb "
                         L"oder ein haengender Grafiktreiber.",
                         static_cast<unsigned>(reason));
            MessageBoxW(hwnd, text, L"g2c", MB_OK | MB_ICONERROR);
            break;
        }
    }

    g_app = nullptr;
    g_relaunch = app.restartRequested();
    saveWindowPlacement(hwnd);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    return 0;
}
