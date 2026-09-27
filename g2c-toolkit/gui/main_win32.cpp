// gui/main_win32.cpp — Fenster, Renderer und Dateidialoge unter Windows.
//
// Alles, was das Betriebssystem braucht, steckt hier. Die eigentliche
// Oberflaeche in app.cpp kennt nur imgui.h und laesst sich deshalb auch ohne
// Windows uebersetzen und pruefen.
//
// ACHTUNG: Diese Datei konnte nicht getestet werden. Sie entstand auf einem
// Linux-Rechner ohne Windows-SDK; uebersetzt und ausgefuehrt wurde sie nie.
// Die Oberflaeche selbst (app.cpp) ist geprueft, diese Anbindung nicht.

#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <shlobj.h>
#include <tchar.h>
#include <windows.h>

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

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device*           g_device = nullptr;
bool                    g_usingWarp = false;

// Startprotokoll.
//
// Stuerzt das Programm beim Start ab, sieht der Nutzer nichts — kein
// Fenster, keine Meldung, und auf einem fremden Rechner ist nicht
// feststellbar, woran es lag. Jeder Schritt wird deshalb VOR seiner
// Ausfuehrung vermerkt und die Datei sofort geschlossen. Was zuletzt
// darin steht, ist der Schritt, der nicht mehr fertig wurde.
// Wo das Startprotokoll hinkommt.
//
// Neben die Exe — dort sucht man es, und dorthin kann man den Nutzer
// verweisen, ohne ihm %APPDATA% erklaeren zu muessen.
//
// Aber nicht immer: liegt das Programm unter "Programme", in einem
// Netzwerkpfad oder auf einem schreibgeschuetzten Medium, schlaegt das
// Schreiben fehl. Dann muss es einen Rueckfall geben, sonst ist gerade auf
// den Rechnern kein Protokoll da, auf denen etwas schiefgeht.
//
// Der Test ist ein Schreibversuch, keine Rechtepruefung: unter Windows sagt
// die Rechtelage allein nicht zuverlaessig, ob eine Datei entsteht
// (Virtualisierung, Richtlinien, Virenschutz).
std::wstring startupLogPath() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring p = exe;
        const std::size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring cand = p.substr(0, slash + 1) + L"g2c-startup.log";
            // Schreibversuch. Gelingt er, bleibt es dabei.
            FILE* probe = nullptr;
            if (_wfopen_s(&probe, cand.c_str(), L"a") == 0 && probe) {
                std::fclose(probe);
                cached = cand;
                return cached;
            }
        }
    }

    // Rueckfall: %APPDATA%\g2c\startup.log
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

// --- Zeichenkettenumwandlung ----------------------------------------------

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

// --- Dateidialoge ----------------------------------------------------------

// Der Filter enthaelt eingebettete Nullbytes; toWide bricht daran ab.
// Deshalb Stueck fuer Stueck umwandeln.
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

// Speichern-Dialog: der Name darf noch nicht existieren.
//
// Fuer "Neue .car" war bisher der Oeffnen-Dialog mit OFN_FILEMUSTEXIST im
// Einsatz — ein neuer Name liess sich damit gar nicht eingeben.
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
    // Kein OFN_OVERWRITEPROMPT: newCar lehnt vorhandene Dateien ohnehin ab
    // und sagt es im Protokoll — ueberschrieben wird nie.
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return toUtf8(buf.data());
}

std::vector<std::string> openFilesDialog(HWND owner, const char* title, const char* filter,
                                         bool multi, const std::string& startDir) {
    // GetOpenFileNameW mit Mehrfachauswahl. Der Puffer muss grosszuegig sein:
    // bei vielen ausgewaehlten Dateien stehen dort Verzeichnis und alle Namen
    // nacheinander, durch Nullbytes getrennt.
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
    // Startordner. OFN_NOCHANGEDIR ist wichtig: ohne das aendert der Dialog
    // das Arbeitsverzeichnis des Programms.
    const std::wstring startW = toWide(startDir);
    if (!startW.empty()) ofn.lpstrInitialDir = startW.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (multi) ofn.Flags |= OFN_ALLOWMULTISELECT;

    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;

    // Bei Einfachauswahl steht ein vollstaendiger Pfad im Puffer. Bei
    // Mehrfachauswahl das Verzeichnis, dann die Dateinamen — jeweils
    // nullterminiert, am Ende ein doppeltes Nullbyte.
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

// Setzt den Startordner eines IFileDialog, sofern er existiert.
void setStartFolder(IFileDialog* dlg, const std::string& startDir) {
    if (startDir.empty()) return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(toWide(startDir).c_str(), nullptr,
                                              IID_PPV_ARGS(&item)))) {
        // SetFolder statt SetDefaultFolder: der zuletzt benutzte Ordner soll
        // wirklich aufgehen, nicht nur beim allerersten Mal vorgeschlagen
        // werden.
        dlg->SetFolder(item);
        item->Release();
    }
}

// Mehrere Ordner auf einmal auswaehlen.
//
// Derselbe Dialog wie fuer einen Ordner, nur mit FOS_ALLOWMULTISELECT.
// IFileOpenDialog liefert die Auswahl dann ueber GetResults als
// IShellItemArray — GetResult (Einzahl) schlaegt in dem Fall fehl, deshalb
// braucht es eine eigene Funktion und nicht nur ein zusaetzliches Flag.
//
// Im Dialog werden Ordner mit Strg und Umschalt ausgewaehlt wie Dateien.
// Kleiner Halter fuer COM-Zeiger.
//
// Die Dialoge geben ihre Objekte bisher von Hand frei. Das ist richtig,
// solange nichts dazwischen wirft — aber toUtf8 und std::string koennen
// bei Speichermangel werfen, und dann bleibt das Objekt liegen.
//
// Ein Leck in einem Dateidialog ist kein Drama. Es von Hand richtig zu
// machen und dabei auf Ausnahmefreiheit zu hoffen, ist trotzdem die
// schlechtere Loesung, wenn die gute drei Zeilen kostet.
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

    // .get(): ComPtr wandelt sich bewusst NICHT stillschweigend in einen
    // Zeiger um. Bequem waere das, aber dann liesse sich der Halter auch
    // versehentlich an Release() oder delete uebergeben — und genau davor
    // soll er schuetzen.
    setStartFolder(dlg.get(), startDir);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    // FOS_FORCEFILESYSTEM bewusst NICHT setzen.
    //
    // Zusammen mit FOS_ALLOWMULTISELECT verhindert es die Mehrfachauswahl:
    // der Dialog laesst dann nur einen Ordner markieren, ohne eine Meldung.
    // Dass echte Pfade herauskommen, wird stattdessen beim Auslesen
    // geprueft — GetDisplayName(SIGDN_FILESYSPATH) scheitert bei allem, was
    // kein Ordner im Dateisystem ist, und der Eintrag wird uebersprungen.
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
    // IFileDialog statt SHBrowseForFolder: der alte Dialog ist winzig, kann
    // keinen Pfad eintippen und sieht seit Vista aus wie aus Windows 2000.
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

// Bildpunkte je Zoll des Systems, vertraeglich mit aelteren Windows-Fassungen.
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

// --- Schriften -------------------------------------------------------------
//
// Segoe UI deckt Latein samt Umlauten ab, aber kein Chinesisch und kein
// Japanisch. Deshalb wird bei diesen Sprachen eine zweite Schrift
// DAZUGELEGT — per MergeMode landen beide im selben Zeichensatz, und die
// lateinischen Zeichen kommen weiterhin scharf aus Segoe UI.
//
// Glyphenbereiche braucht es seit ImGui 1.92 nicht mehr: der Zeichensatz
// laedt nach, was tatsaechlich angezeigt wird. Vorher mussten fuer
// Chinesisch 2500 und fuer Japanisch 1946 Ideogramme im Voraus gerastert
// werden, bei jedem Sprachwechsel neu.
// Schrift nur laden, wenn die Datei WIRKLICH da ist.
//
// ImGui loest bei einer fehlenden Schriftdatei ein IM_ASSERT_USER_ERROR
// aus. Ist die Pruefung aktiv, bricht das Programm dort ab — und zwar nur
// auf den Rechnern, denen die Datei fehlt. Genau so sah es aus: auf
// Windows 11 lief es (dort gibt es SegoeIcons.ttf), auf Windows 10 nicht.
//
// Vorher nachsehen kostet einen Systemaufruf und nimmt der Sache jede
// Abhaengigkeit davon, wie ImGui uebersetzt wurde.
ImFont* addFontIfPresent(ImGuiIO& io, const std::wstring& path, float size,
                         const ImFontConfig* cfg, const ImWchar* ranges) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return io.Fonts->AddFontFromFileTTF(toUtf8(path).c_str(), size, cfg, ranges);
}

void buildFonts(ImGuiIO& io, float dpi, g2::gui::Lang lang) {
    io.Fonts->Clear();

    // Unsinnige Skalierung abfangen.
    //
    // ImGui erzeugt bei Schriftgroesse 0 keine Glyphen und stuerzt beim
    // Zeichnen ab. GetDpiScaleForHwnd liefert normalerweise 1.0 bis 3.0,
    // aber ein defekter Treiber oder eine ungewoehnliche
    // Mehrschirmeinrichtung kann 0 melden — und dann ist der Absturz
    // weit weg von seiner Ursache.
    if (!(dpi > 0.1f) || dpi > 8.0f) dpi = 1.0f;

    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) {
        io.Fonts->AddFontDefault();
        return;
    }
    const std::wstring fontDir = std::wstring(winDir) + L"\\Fonts\\";
    const float size = 17.0f * dpi;

    startupLog("  Grundschrift segoeui.ttf");
    // Keine Bereichsangaben mehr.
    //
    // Seit ImGui 1.92 laedt der Zeichensatz Glyphen bei Bedarf nach, sofern
    // das Backend ImGuiBackendFlags_RendererHasTextures unterstuetzt — das
    // DX11-Backend tut das. Alle GetGlyphRangesXXX() sind damit veraltet.
    //
    // Der Gewinn ist keine Kosmetik: fuer Chinesisch wurden bisher 2500 und
    // fuer Japanisch 1946 Ideogramme im Voraus gerastert, bei jedem
    // Sprachwechsel neu. Jetzt entsteht nur, was auch angezeigt wird.
    ImFont* base = addFontIfPresent(io, fontDir + L"segoeui.ttf", size, nullptr, nullptr);
    if (!base) {
        io.Fonts->AddFontDefault();
        return;
    }

    // Zweite Schrift dazulegen. Welche Datei es ist, haengt von der Sprache
    // ab; alle drei gehoeren zum Lieferumfang von Windows.
    const wchar_t* cjkFile = nullptr;
    switch (lang) {
        case g2::gui::Lang::Zh: cjkFile = L"msyh.ttc"; break;      // Microsoft YaHei
        case g2::gui::Lang::Ja: cjkFile = L"YuGothM.ttc"; break;   // Yu Gothic Medium
        default: break;
    }

    if (cjkFile) {
        startupLog("  CJK-Schrift");
        ImFontConfig cfg;
        cfg.MergeMode = true;          // in denselben Zeichensatz einfuegen
        cfg.OversampleH = 1;           // CJK-Glyphen sind gross, das genuegt
        cfg.OversampleV = 1;
        if (!addFontIfPresent(io, fontDir + cjkFile, size, &cfg, nullptr)) {
            // Ausweichschriften, falls die bevorzugte fehlt. Lieber eine
            // andere Schrift als leere Kaesten.
            for (const wchar_t* alt : {L"meiryo.ttc", L"msgothic.ttc", L"simsun.ttc",
                                       L"malgun.ttf"}) {
                if (addFontIfPresent(io, fontDir + alt, size, &cfg, nullptr)) break;
            }
        }
    }

    // Symbolschrift dazulegen.
    //
    // Windows 11 bringt "Segoe Fluent Icons" mit, Windows 10 nur
    // "Segoe MDL2 Assets". Beide decken denselben Bereich ab, die Fluent-
    // Variante hat rundere Ecken. Erst die neuere versuchen.
    {
        // Nur den Bereich anfordern, der tatsaechlich benutzt wird.
        //
        // Vorher standen hier E700 bis F8FF, also 4608 Zeichen — von denen
        // zwanzig gebraucht werden. Der Zeichensatz legt fuer den ganzen
        // Bereich eine Nachschlagetabelle an, und das ist Speicher und Zeit
        // fuer nichts.
        static const ImWchar iconRange[] = {static_cast<ImWchar>(g2::gui::kIconUsedMin),
                                            static_cast<ImWchar>(g2::gui::kIconUsedMax), 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        // Etwas kleiner als die Schrift: die Symbole sind auf voller
        // Zeilenhoehe gezeichnet und wirken sonst wuchtiger als der Text.
        cfg.GlyphOffset.y = 1.0f * dpi;

        startupLog("  Symbolschrift");
        bool ok = false;
        for (const wchar_t* f : {L"SegoeIcons.ttf", L"segmdl2.ttf"}) {
            if (addFontIfPresent(io, fontDir + f, size * 0.86f, &cfg, iconRange)) {
                ok = true;
                break;
            }
        }
        // Fehlt die Schrift, werden die Symbole weggelassen statt als leere
        // Kaesten angezeigt.
        g2::gui::setIconsAvailable(ok);
    }

    startupLog("  Zeichensatz aufbauen");
    if (!io.Fonts->Build()) {
        // Schlaegt der Aufbau fehl, ist der Zeichensatz unbrauchbar. Mit
        // der Standardschrift weitermachen statt mit einem leeren Atlas
        // ins Zeichnen zu gehen — dort waere der Absturz weit weg von
        // seiner Ursache.
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

    // Erst die Grafikkarte, dann der Softwarerasterisierer.
    //
    // Ohne den Rueckfallweg startet das Programm auf Rechnern ohne
    // D3D11-faehigen Treiber gar nicht — und das betrifft mehr Faelle als
    // man denkt: virtuelle Maschinen, Remotedesktop, alte oder fehlende
    // Grafiktreiber. WARP gehoert zu Windows und ist immer vorhanden; fuer
    // eine Oberflaeche aus Linien und Text reicht er vollkommen.
    // NICHT unter 10_0 gehen.
    //
    // ImGuis DX11-Anbindung uebersetzt ihre Shader mit "vs_4_0", und das ist
    // ein Ziel fuer Feature-Level 10.0 aufwaerts. Auf 9_1 oder 9_3 laesst
    // sich das Geraet zwar erzeugen, aber die Shader scheitern — das Fenster
    // erscheint weiss und das Programm stuerzt ab. Level 9.x braeuchte die
    // Profile "vs_4_0_level_9_x", die ImGui nicht benutzt.
    //
    // Ein Geraet zu erzeugen, das anschliessend nichts zeichnen kann, ist
    // schlechter als gar keins: bei letzterem erscheint wenigstens die
    // Fehlermeldung.
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

// Kennung der WM_COPYDATA-Nachricht "diese Dateien oeffnen" ("G2C1").
constexpr ULONG_PTR kOpenFilesMessage = 0x47324331;

// Dateien in der laufenden Oberflaeche oeffnen — derselbe Weg fuer Ziehen
// aufs Fenster und fuer Dateien, die ein zweiter Programmstart weiterreicht.
//
// .xsi gesammelt anhaengen, alles andere ueber openPath, also genau so wie
// beim Ziehen auf die Exe. Frueher wurden .gla und anims.h beim Ziehen aufs
// Fenster stillschweigend ignoriert, und ein nicht (mehr) vorhandener Pfad
// galt als Ordner, weil INVALID_FILE_ATTRIBUTES das Verzeichnisbit hat.
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
            // Dateien auf das Fenster ziehen: .car werden geoeffnet, .xsi an
            // das aktuelle Skript angehaengt.
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
            // Ein zweiter Start von g2c — etwa per Doppelklick auf eine .car —
            // reicht seine Dateien hierher weiter, statt ein zweites Fenster
            // mit allen Tabs aufzumachen. Siehe forwardToRunningInstance.
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
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // Alt-Menue unterdruecken
            break;

        case WM_CLOSE:
            // Nicht gleich zerstoeren. Frueher ging das Fenster sofort zu:
            // ungespeicherte Aenderungen waren ohne Rueckfrage weg, und
            // saveWindowPlacement fand danach kein Fenster mehr vor, sodass
            // die Fensterlage nie gespeichert wurde.
            //
            // Jetzt entscheidet die Oberflaeche. Ohne Aenderungen endet die
            // Hauptschleife sofort; sonst fragt ein Dialog, und die Schleife
            // endet, sobald er beantwortet ist. Zerstoert wird das Fenster
            // erst danach, am Ende von wWinMain.
            if (!g_app || g_app->requestQuit()) PostQuitMessage(0);
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

// --- Fensterlage merken ----------------------------------------------------
//
// GetWindowPlacement statt GetWindowRect: die Struktur kennt neben der
// Position auch den Zustand (normal, maximiert, minimiert) UND die Groesse,
// die das Fenster im nicht-maximierten Zustand haette. Mit GetWindowRect
// allein waere ein maximiertes Fenster beim naechsten Start bildschirmgross
// aber nicht maximiert — und beim Wiederherstellen bliebe es riesig.
//
// Ein minimiertes Fenster wird als normal gespeichert: niemand will ein
// Programm starten, das sofort in der Taskleiste verschwindet.
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

    // Liegt das Fenster ausserhalb aller Bildschirme, nicht wiederherstellen.
    //
    // Das passiert nach dem Abziehen eines zweiten Monitors: das Fenster
    // waere da, aber unsichtbar und nicht erreichbar.
    const HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;

    wp.rcNormalPosition = r;
    return SetWindowPlacement(hwnd, &wp) != 0;
}

// Einstiegspunkt der Kommandozeile, liegt in tools/g2c.cpp.
int g2cMain(int argc, char** argv);

namespace {

// Ist das erste Argument ein Befehl der Kommandozeile?
//
// Sonst waere gemeint: "diese Dateien in der Oberflaeche oeffnen". Das ist
// der Fall, wenn man eine .car auf die Exe zieht — dann will man das
// Fenster sehen, kein Konsolenfenster.
bool looksLikeCommand(const char* a) {
    // Dieselbe Liste wie in g2cMain. export, makecar und about fehlten: sie
    // oeffneten die Oberflaeche, obwohl die Fehlermeldung beim Grafikstart
    // genau "g2c about" empfiehlt.
    static const char* kCmds[] = {"build", "anim",   "mesh",    "info",  "check", "xsi",
                                  "car",   "validate", "diff",  "scan",  "export", "makecar",
                                  "about", "version", "-h",     "--help", "/?",   "help",
                                  "-v",    "--version"};
    for (const char* c : kCmds)
        if (std::strcmp(a, c) == 0) return true;
    return false;
}

// Ausgabe an die aufrufende Eingabeaufforderung haengen.
//
// Das Programm ist als Fensteranwendung gebaut — sonst blitzte bei jedem
// Doppelklick ein schwarzes Konsolenfenster auf. Fensteranwendungen haben
// aber keine Konsole; ohne AttachConsole liefe die Kommandozeilenfassung
// stumm. ATTACH_PARENT_PROCESS haengt sie an die Konsole, aus der sie
// gestartet wurde. Klappt das nicht (etwa per Doppelklick gestartet), wird
// eine eigene geoeffnet, damit die Ausgabe nicht verlorengeht.
void attachOrOpenConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
    }
    // Umgeleitete Ausgabe ("g2c info a.gla > out.txt") nicht auf die
    // Konsole umbiegen. Frueher wurde stdout immer neu auf CONOUT$ geoeffnet,
    // und die Datei blieb leer.
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

    // Die Eingabeaufforderung hat ihre Eingabezeile schon zurueckgegeben;
    // ohne diesen Umbruch begaenne unsere Ausgabe mitten in der Zeile.
    std::printf("\n");
}

// Argumente als UTF-8, unabhaengig vom Einstiegspunkt.
//
// __argv ist bei wWinMain LEER — die Laufzeitbibliothek fuellt bei einem
// Unicode-Einstiegspunkt nur __wargv. Ein Zugriff auf __argv[1] laeuft
// deshalb in einen Nullzeiger, und das Programm stuerzt ab, BEVOR das
// Fenster erscheint. Von aussen sieht es aus, als passiere nichts.
//
// CommandLineToArgvW ist ausserdem der einzige zuverlaessige Weg fuer
// Pfade mit Umlauten oder anderen Zeichen ausserhalb der Codepage — und
// genau solche Pfade zieht man auf ein Programm.
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
        pointers_.push_back(nullptr);   // argv ist nullterminiert
    }

    // Startet mit "-reset" der gespeicherte Zustand verworfen werden?
    //
    // Ohne diesen Ausweg gibt es keinen, wenn eine Einstellungsdatei den
    // Start verhindert: das Programm stuerzt bei jedem Versuch an
    // derselben Stelle ab, und der Ordner ist fuer den Nutzer nicht
    // auffindbar.
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

    // Dateien unter den Argumenten, ohne Schalter wie -reset oder -nofont.
    // Die wurden frueher ebenfalls als Pfad geoeffnet und erzeugten beim
    // Start eine "nicht gefunden"-Meldung.
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

// Laeuft g2c schon? Dann die Dateien dorthin schicken, statt ein zweites
// Fenster zu oeffnen.
//
// Ein Doppelklick auf eine .car startete frueher bei offenem g2c ein zweites
// Fenster, das wieder alle zuletzt offenen Tabs lud. Beide schrieben beim
// Beenden ihre Einstellungen, und das zuletzt geschlossene gewann.
//
// Das laufende Fenster oeffnet die Datei — oder springt auf ihren Tab, wenn
// sie schon offen ist — und kommt nach vorn. Antwortet es nicht (aeltere
// Fassung, haengt), startet dieses Programm ganz normal.
bool forwardToRunningInstance(const Args& args) {
    if (args.hasReset()) return false;
    const auto files = args.files();
    if (files.empty()) return false;
    const HWND other = FindWindowW(L"g2cWindow", nullptr);
    if (!other) return false;

    // Absolut machen: das andere Programm hat ein anderes Arbeitsverzeichnis.
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

    // Das andere Fenster darf sich nach vorn holen — ohne diese Erlaubnis
    // blinkte es nur in der Taskleiste.
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);

    DWORD_PTR result = 0;
    const LRESULT ok = SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                           SMTO_ABORTIFHUNG, 5000, &result);
    return ok != 0 && result == TRUE;
}

}  // namespace

int wWinMainGuarded(HINSTANCE inst);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Jede Ausnahme abfangen und ZEIGEN.
    //
    // Ohne das beendet sich das Programm bei einem Fehler beim Laden der
    // Einstellungen oder beim Wiederherstellen der Tabs lautlos — und ein
    // Fenster, das sich sofort wieder schliesst, ist vom Nutzer nicht von
    // einem kaputten Download zu unterscheiden.
    try {
        return wWinMainGuarded(inst);
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
    // Bei jedem Start neu beginnen: eine Datei, die endlos waechst, liest
    // niemand, und nur der letzte Versuch ist interessant.
    {
        const std::wstring p = startupLogPath();
        if (!p.empty()) DeleteFileW(p.c_str());
    }
    startupLog("Start");
    Args args;
    // Mit einem Befehl als erstem Argument arbeitet dieselbe Exe als
    // Kommandozeilenwerkzeug. Eine Datei als Argument oeffnet dagegen die
    // Oberflaeche mit dieser Datei.
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

    // MUSS vor dem Erzeugen des Fensters stehen.
    //
    // Ohne diese Anmeldung haelt Windows die Anwendung fuer eine alte mit
    // fester Aufloesung von 96 dpi: sie zeichnet klein und Windows skaliert
    // das fertige Bild hoch. Das Ergebnis ist durchgehend unscharf — Schrift
    // wie Linien. Richtig ist, gleich in der Aufloesung des Bildschirms zu
    // zeichnen.
    startupLog("DPI-Bewusstsein setzen");
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Symbol aus den eigenen Ressourcen. hIcon ist das grosse (Alt+Tab),
    // hIconSm das kleine in der Titelleiste — beide angeben, sonst
    // skaliert Windows das grosse herunter und es wird unscharf.
    //
    // LoadImageW mit SM_CXSMICON holt gezielt die 16-Pixel-Fassung aus der
    // .ico. LoadIconW liefert immer die grosse; fuer die Titelleiste waere
    // das eine heruntergerechnete und damit matschige Darstellung.
    //
    // Kennung 1, passend zu icon/g2c.rc: Windows nimmt fuer das Symbol im
    // Explorer das Icon mit der NIEDRIGSTEN Kennung in der Exe.
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"g2cWindow";
    startupLog("Fensterklasse anmelden");
    RegisterClassExW(&wc);

    // Fenstergroesse in Bildpunkten, damit es auf hochaufloesenden
    // Bildschirmen nicht winzig erscheint.
    //
    // GetDpiForSystem gibt es erst ab Windows 10 1607. Fest eingebunden
    // startet das Programm auf aelteren Systemen gar nicht — es faellt schon
    // beim Laden mit "Einsprungpunkt nicht gefunden" aus, bevor eine einzige
    // Zeile lief. Deshalb zur Laufzeit nachschlagen und sonst den alten Weg
    // ueber den Geraetekontext nehmen, den es seit jeher gibt.
    const UINT dpiRaw = systemDpi();
    const int  w = MulDiv(1500, static_cast<int>(dpiRaw), 96);
    const int  h = MulDiv(950, static_cast<int>(dpiRaw), 96);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"g2c \x2014 Ghoul2 Toolkit", WS_OVERLAPPEDWINDOW,
                              100, 100, w, h, nullptr, nullptr, inst, nullptr);
    startupLog("DirectX-Geraet erzeugen");
    if (!createDevice(hwnd)) {
        destroyDevice();
        UnregisterClassW(wc.lpszClassName, inst);

        // NICHT stillschweigend beenden.
        //
        // Ein Fenster, das sich beim Doppelklick sofort wieder schliesst,
        // ist fuer den Nutzer nicht von einem kaputten Download zu
        // unterscheiden. Sagen, was fehlt.
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
    // Fensterzustand und Spaltenbreiten neben die Einstellungen legen,
    // nicht ins Arbeitsverzeichnis.
    static std::string iniPath;
    iniPath = (std::filesystem::path(g2::gui::App::configDir()) / "g2c_gui.ini").string();
    io.IniFilename = iniPath.c_str();

    // Skalierung des Bildschirms ermitteln, auf dem das Fenster liegt.
    bool firstFrame = true;
    startupLog("DPI ermitteln");
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    // Mit "g2c -nofont" nur die eingebaute Schrift benutzen.
    //
    // Die Schriften von Windows sind der einzige Teil des Starts, der von
    // Dateien auf dem fremden Rechner abhaengt — eine beschaedigte oder
    // ersetzte segoeui.ttf ist nicht auszuschliessen. Dieser Schalter
    // ueberspringt sie vollstaendig.
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

    // Der wahrscheinlichste Absturzpunkt nach der Grafik: hier werden die
    // gespeicherten Einstellungen gelesen und die zuletzt geoeffneten
    // Skripte wiederhergestellt. Eine beschaedigte Datei in %APPDATA%\g2c
    // schlaegt genau hier zu.
    // Mit "g2c -reset" laesst sich der gespeicherte Zustand ueberspringen.
    //
    // Ohne das gibt es keinen Ausweg, wenn eine Einstellungsdatei den Start
    // verhindert: das Programm stuerzt bei jedem Versuch an derselben
    // Stelle ab, und der Ordner ist fuer den Nutzer nicht auffindbar.
    if (args.hasReset()) {
        startupLog("gespeicherten Zustand verwerfen (-reset)");
        // Derselbe Ordner, aus dem auch gelesen wird — im mitnehmbaren
        // Betrieb liegt er neben der Exe, nicht unter %APPDATA%. Und die
        // Fensterdatei heisst g2c_gui.ini; geloescht wurde frueher eine
        // imgui.ini, die es nie gab.
        const std::filesystem::path dir(g2::gui::App::configDir());
        for (const char* f : {"g2c_settings.txt", "g2c_window.txt", "g2c_gui.ini"}) {
            std::error_code ec;
            std::filesystem::remove(dir / f, ec);
        }
    }

    startupLog("Einstellungen laden und Skripte wiederherstellen");
    g2::gui::App app(std::move(plat));
    app.settings().dpiScale = dpi;

    // Auf die Exe gezogene oder als Argument uebergebene Dateien oeffnen.
    for (const auto& f : args.files()) app.openPath(f);
    // Der Oberflaeche sagen, wo das Protokoll liegt — sie ermittelt es
    // nicht selbst, damit die Auskunft und die tatsaechlich beschriebene
    // Datei nicht auseinanderlaufen koennen.
    app.setLogPath(toUtf8(startupLogPath()));

    g_app = &app;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resize) {
            releaseRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resize = false;
            createRenderTarget();
        }

        // Sprachwechsel: der Zeichensatz muss mit anderen Glyphenbereichen
        // neu aufgebaut werden. Die Texturen des Renderers haengen daran,
        // also erst freigeben, dann neu bauen.
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

        // Fensterkreuz bei ungespeicherten Aenderungen: die Oberflaeche hat
        // gefragt, und die Antwort war "speichern" oder "verwerfen".
        if (app.quitApproved()) running = false;

        ImGui::Render();
        const float clear[4] = {0.09f, 0.09f, 0.10f, 1.0f};
        // Ohne Renderziel nichts zeichnen.
        //
        // Schlaegt die Erzeugung fehl — etwa nach einem Treiberneustart —,
        // waere g_rtv null. OMSetRenderTargets mit null zeichnet ins Leere:
        // das Fenster bleibt weiss, und der naechste Zugriff stuerzt ab.
        if (!g_rtv) {
            ::Sleep(16);
            continue;
        }
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Treiberneustart abfangen.
        //
        // Faellt der Grafiktreiber aus und wird von Windows neu gestartet
        // (bei NVIDIA, AMD und Intel gleichermaessen ueblich, etwa nach
        // einem Treiberupdate im laufenden Betrieb), meldet Present
        // DEVICE_REMOVED oder DEVICE_RESET. Danach schlaegt JEDER weitere
        // Aufruf fehl — ohne Pruefung sieht der Nutzer ein weisses Fenster
        // und dann einen Absturz.
        if (firstFrame) {
            startupLog("erstes Bild gezeichnet - ab hier laeuft alles");
            firstFrame = false;
        }
        const HRESULT pr = g_swapChain->Present(1, 0);   // vsync
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
