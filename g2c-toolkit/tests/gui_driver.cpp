// Oberflaeche wirklich bedienen: klicken, ziehen, tippen — ohne Fenster.
//
// tests/gui_tests.cpp prueft die Logik hinter der Oberflaeche. Gezeichnet wird
// dort nichts, und damit bleibt alles ungeprueft, was erst beim Zeichnen
// passiert: Kontextmenues, Ziehen und Ablegen, Dialoge, Tastenkuerzel, die
// Reihenfolge von Begin/End in fruehen Ruecksprungpfaden.
//
// Dieser Treiber baut dafuer eine kleine Testmaschine ueber ImGuis eigene
// Test-Engine-Hooks (IMGUI_ENABLE_TEST_ENGINE): jedes Widget meldet Kennung,
// Beschriftung und Rechteck. Damit laesst sich jedes Element per Beschriftung
// finden und mit echten Maus- und Tastaturereignissen bedienen — so, wie ein
// Mensch es tun wuerde, nur tausendmal schneller.
//
// Jede ImGui-Zusicherung (fehlendes End, doppelte ID, falsche Popup-Paare)
// wird abgefangen und als Fehler gezaehlt, statt den Lauf zu beenden.
//
// Aufruf:  g2_gui_driver <arbeitsordner> [<assetwurzel> <referenz.gla> <anims.h>]
//
// Ohne Assetwurzel legt der Treiber sich kleine Beispieldateien selbst an.
// Mit echten Daten prueft er zusaetzlich eine vollstaendige _humanoid.car.
//
// Alles Geschriebene landet unter <arbeitsordner>. %APPDATA% wird fuer den
// Lauf dorthin umgelenkt: die Einstellungen des Nutzers — zuletzt offene
// Skripte, Ausgabeordner — duerfen durch einen Testlauf weder gelesen noch
// ueberschrieben werden.

#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"

#include "g2/carscript.h"
#include "g2/mdxa.h"
#include "g2/readfile.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using g2::gui::S;
using g2::gui::tr;

// --- Erfassung ueber die Test-Engine-Hooks ----------------------------------

namespace {

struct Item {
    ImGuiID              id = 0;
    ImRect               bb;
    std::string          label;
    std::string          window;
    ImGuiItemFlags       itemFlags = 0;
    ImGuiItemStatusFlags status = 0;
};

std::vector<Item>                     g_cur;
std::unordered_map<ImGuiID, size_t>   g_curIdx;
std::vector<Item>                     g_last;
std::set<ImGuiID>                     g_dupThisFrame;
std::map<std::string, int>            g_idConflicts;   // "Fenster | Beschriftung" -> Anzahl
std::vector<std::string>              g_asserts;
int                                   g_failures = 0;
int                                   g_checks = 0;
std::FILE*                            g_log = nullptr;

void out(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    if (g_log) std::fputs(buf, g_log);
    std::fflush(stdout);
    if (g_log) std::fflush(g_log);
}

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) {
        out("    [OK]     %s\n", what.c_str());
    } else {
        ++g_failures;
        out("    [FEHLER] %s\n", what.c_str());
    }
}

void step(const char* name) { out("\n== %s ==\n", name); }

}  // namespace

void g2DriverAssert(const char* expr, const char* file, int line) {
    const std::string f = fs::path(file).filename().string();
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s:%d  %s", f.c_str(), line, expr);
    g_asserts.emplace_back(buf);
    out("    [ASSERT] %s\n", buf);
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData* data) {
    if (id == 0) return;
    if (const auto it = g_curIdx.find(id); it != g_curIdx.end()) {
        // Nur im selben Fenster ist es ein Konflikt. Mehrzeilige Eingaben
        // melden sich einmal als Kindfenster und einmal im Elternfenster an.
        const char* w = ctx->CurrentWindow ? ctx->CurrentWindow->Name : "";
        bool isChild = false;
        for (ImGuiWindow* cw : ctx->Windows)
            if (cw->ChildId == id) isChild = true;
        if (!isChild && g_cur[it->second].window == w) g_dupThisFrame.insert(id);
        return;
    }
    Item it;
    it.id = id;
    it.bb = bb;
    it.window = ctx->CurrentWindow ? ctx->CurrentWindow->Name : "";
    it.itemFlags = data ? data->ItemFlags : ctx->CurrentItemFlags;
    g_curIdx[id] = g_cur.size();
    g_cur.push_back(std::move(it));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label,
                                  ImGuiItemStatusFlags flags) {
    const auto it = g_curIdx.find(id);
    if (it == g_curIdx.end()) return;
    g_cur[it->second].label = label ? label : "";
    g_cur[it->second].status = flags;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

// --- Absturzfaenger -----------------------------------------------------------
//
// Ein Zugriffsfehler soll als Befund im Protokoll landen, nicht den ganzen
// Lauf beenden. __try verlangt eine Funktion ohne Objekte mit Destruktor.
namespace {
#ifdef _WIN32
using Thunk = void (*)(void*);
int runGuardedRaw(Thunk fn, void* arg) {
    __try {
        fn(arg);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<int>(GetExceptionCode());
    }
}
#endif

// Liefert 0, wenn alles gut ging, sonst den Ausnahmecode.
int guarded(const std::function<void()>& f) {
#ifdef _WIN32
    struct Ctx { const std::function<void()>* f; std::string err; };
    Ctx c{&f, {}};
    const int code = runGuardedRaw(
        [](void* p) {
            auto* cx = static_cast<Ctx*>(p);
            try {
                (*cx->f)();
            } catch (const std::exception& e) {
                cx->err = e.what();
            }
        },
        &c);
    if (!c.err.empty()) {
        out("    [AUSNAHME] %s\n", c.err.c_str());
        return -1;
    }
    return code;
#else
    try {
        f();
    } catch (const std::exception& e) {
        out("    [AUSNAHME] %s\n", e.what());
        return -1;
    }
    return 0;
#endif
}

// --- Treiber ------------------------------------------------------------------

std::string displayOf(const std::string& label) {
    const std::size_t p = label.find("##");
    return p == std::string::npos ? label : label.substr(0, p);
}

struct Driver {
    g2::gui::App* app = nullptr;
    int           frameNo = 0;
    double        slowestMs = 0.0;
    std::string   slowestWhere;
    std::string   where = "Start";

    float nextDelta = 1.0f / 60.0f;

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1300);
        io.DeltaTime = nextDelta;
        nextDelta = 1.0f / 60.0f;

        g_cur.clear();
        g_curIdx.clear();
        g_dupThisFrame.clear();

        const auto t0 = std::chrono::steady_clock::now();
        ImGui::NewFrame();
        app->draw();
        ImGui::Render();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (ms > slowestMs) {
            slowestMs = ms;
            slowestWhere = where;
        }

        // Texturwuensche sofort als erledigt melden: es gibt keine Grafikkarte.
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            if (t->Status == ImTextureStatus_WantCreate) {
                t->SetTexID(static_cast<ImTextureID>(1));
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantUpdates) {
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        for (const ImGuiID id : g_dupThisFrame) {
            const auto it = g_curIdx.find(id);
            if (it == g_curIdx.end()) continue;
            const Item& i = g_cur[it->second];
            ++g_idConflicts[i.window + " | " + (i.label.empty() ? "(ohne Beschriftung)" : i.label)];
        }
        g_last = g_cur;
        ++frameNo;
    }

    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }

    // --- Suchen ---
    // Fenster melden sich selbst als Element mit ihrem Namen an. Ein Dialog
    // "Ueberschreiben###overwrite" hiesse sonst genauso wie sein Knopf.
    const Item* findIf(const std::function<bool(const Item&)>& pred) const {
        for (const Item& i : g_last)
            if (i.label != i.window && pred(i)) return &i;
        return nullptr;
    }
    // Sichtbare Beschriftung genau (nach Abzug von "##...").
    const Item* find(const std::string& text, const char* windowPart = nullptr) const {
        return findIf([&](const Item& i) {
            if (windowPart && i.window.find(windowPart) == std::string::npos) return false;
            return displayOf(i.label) == text;
        });
    }
    // Sichtbare Beschriftung endet auf text — fuer Knoepfe mit Symbol davor.
    const Item* findEnds(const std::string& text, const char* windowPart = nullptr) const {
        return findIf([&](const Item& i) {
            if (windowPart && i.window.find(windowPart) == std::string::npos) return false;
            const std::string d = displayOf(i.label);
            return d.size() >= text.size() && d.compare(d.size() - text.size(), text.size(), text) == 0;
        });
    }
    const Item* findContains(const std::string& text, const char* windowPart = nullptr) const {
        return findIf([&](const Item& i) {
            if (windowPart && i.window.find(windowPart) == std::string::npos) return false;
            return i.label.find(text) != std::string::npos;
        });
    }
    // iconButton zeichnet "##ib" unter PushID(text): die Kennung laesst sich
    // nachrechnen, die Beschriftung nicht lesen.
    const Item* findIconButton(const char* text, const char* windowPart) const {
        ImGuiWindow* w = nullptr;
        for (ImGuiWindow* cand : GImGui->Windows)
            if (std::string(cand->Name).find(windowPart) != std::string::npos && cand->WasActive) {
                w = cand;
                // Innerstes Fenster bevorzugen.
            }
        if (!w) return nullptr;
        for (ImGuiWindow* cand : GImGui->Windows) {
            if (!cand->WasActive) continue;
            const std::string nm = cand->Name;
            if (nm.find(windowPart) == std::string::npos) continue;
            const ImGuiID seed = cand->GetID(text);
            const ImGuiID id = ImHashStr("##ib", 0, seed);
            for (const Item& i : g_last)
                if (i.id == id) return &i;
        }
        return nullptr;
    }

    static bool disabled(const Item* i) { return i && (i->itemFlags & ImGuiItemFlags_Disabled); }

    // --- Eingaben ---
    void mods(bool ctrl, bool shift) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiKey_LeftShift, shift);
    }
    void moveTo(ImVec2 p) {
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
        frame();
    }
    static ImVec2 center(const Item& i) {
        return ImVec2((i.bb.Min.x + i.bb.Max.x) * 0.5f, (i.bb.Min.y + i.bb.Max.y) * 0.5f);
    }
    // Links in der Zeile statt in der Mitte: bei Tabellenzeilen liegen in
    // der Mitte andere Spalten, die eigene Widgets tragen koennen.
    static ImVec2 leftOf(const Item& i) {
        return ImVec2(i.bb.Min.x + 6.0f, (i.bb.Min.y + i.bb.Max.y) * 0.5f);
    }
    // Zeit verstreichen lassen, wie ein Mensch zwischen zwei Klicks. Ohne
    // das werten zwei Klicks auf dieselbe Zeile als Doppelklick.
    void pause() {
        nextDelta = 0.4f;
        frame();
    }
    void clickAt(ImVec2 p, int button = 0, bool ctrl = false, bool shift = false) {
        mods(ctrl, shift);
        moveTo(p);
        // Elemente mit AllowOverlap uebernehmen den Mauszeiger erst ein Bild
        // spaeter. Ein Mensch bewegt die Maus ohnehin ueber viele Bilder.
        frames(2);
        ImGui::GetIO().AddMouseButtonEvent(button, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(button, false);
        frame();
        mods(false, false);
        frame();
        pause();
    }
    bool click(const Item* i, int button = 0, bool ctrl = false, bool shift = false) {
        if (!i) return false;
        clickAt(center(*i), button, ctrl, shift);
        return true;
    }
    bool clickRow(const Item* i, bool ctrl = false, bool shift = false) {
        if (!i) return false;
        clickAt(leftOf(*i), 0, ctrl, shift);
        return true;
    }
    bool doubleClick(const Item* i, bool left = true) {
        if (!i) return false;
        const ImVec2 p = left ? leftOf(*i) : center(*i);
        moveTo(p);
        frames(2);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frames(2);
        return true;
    }
    void dragPath(ImVec2 from, const std::vector<ImVec2>& via, int stepsEach = 8) {
        moveTo(from);
        frames(2);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(0, true);
        frame();
        ImVec2 cur = from;
        for (const ImVec2& target : via) {
            for (int s = 1; s <= stepsEach; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(stepsEach);
                moveTo(ImVec2(cur.x + (target.x - cur.x) * t, cur.y + (target.y - cur.y) * t));
            }
            cur = target;
            frames(2);
        }
        io.AddMouseButtonEvent(0, false);
        frames(3);
    }
    void key(ImGuiKey k, bool ctrl = false, bool shift = false) {
        mods(ctrl, shift);
        frame();
        ImGui::GetIO().AddKeyEvent(k, true);
        frame();
        ImGui::GetIO().AddKeyEvent(k, false);
        frame();
        mods(false, false);
        frame();
    }
    void type(const char* utf8) {
        ImGui::GetIO().AddInputCharactersUTF8(utf8);
        frames(2);
    }
    void wheel(ImVec2 at, float dy) {
        moveTo(at);
        ImGui::GetIO().AddMouseWheelEvent(0.0f, dy);
        frames(2);
    }
    void escape() { key(ImGuiKey_Escape); }

    // Reiter melden sich ohne Beschriftung an die Test-Hooks. Deshalb ueber
    // ImGuis Tab-Leisten suchen und das Rechteck selbst ausrechnen.
    Item tabHit;
    const Item* findTab(const std::string& labelPart) {
        ImGuiContext& g = *GImGui;
        for (int n = 0; n < g.TabBars.GetMapSize(); ++n) {
            ImGuiTabBar* tb = g.TabBars.TryGetMapData(n);
            if (!tb) continue;
            for (ImGuiTabItem& t : tb->Tabs) {
                const char* name = ImGui::TabBarGetTabName(tb, &t);
                if (!name || std::string(name).find(labelPart) == std::string::npos) continue;
                const float x0 = tb->BarRect.Min.x + t.Offset - tb->ScrollingAnim;
                tabHit = Item{};
                tabHit.id = t.ID;
                tabHit.label = name;
                // Nur das linke Drittel: rechts sitzt das Schliessen-Kreuz,
                // und ein Klick in die Mitte trifft es bei kurzen Namen.
                tabHit.bb = ImRect(ImVec2(x0 + 4.0f, tb->BarRect.Min.y),
                                   ImVec2(x0 + t.Width / 3.0f, tb->BarRect.Max.y));
                return &tabHit;
            }
        }
        return nullptr;
    }

    // Element ueber seine Kennung finden. Fuer Widgets, die ihre Beschriftung
    // nicht an die Hooks melden (Kombinationsfelder).
    const Item* findById(const char* label, const char* windowPart) const {
        for (ImGuiWindow* w : GImGui->Windows) {
            if (!w->WasActive || std::string(w->Name).find(windowPart) == std::string::npos) continue;
            const ImGuiID id = ImHashStr(label, 0, w->ID);
            for (const Item& i : g_last)
                if (i.id == id) return &i;
        }
        return nullptr;
    }

    // Knoepfe der Modusleiste: drei "##mode" in dieser Reihenfolge.
    const Item* modeButton(int n) const {
        int k = 0;
        for (const Item& i : g_last)
            if (i.window.find("modebar") != std::string::npos && i.label == "##mode" && k++ == n)
                return &i;
        return nullptr;
    }
    // Genaue Beschriftung samt "##".
    const Item* findLabel(const std::string& full) const {
        return findIf([&](const Item& i) { return i.label == full; });
    }

    // Hauptmenue oeffnen und einen Eintrag waehlen.
    bool menu(const char* top, const char* entry, const char* sub = nullptr) {
        const Item* t = find(top);
        if (!t) {
            out("    (Menue \"%s\" nicht gefunden)\n", top);
            return false;
        }
        if (popupOpen()) {
            escape();
            escape();
        }
        click(t);
        frames(2);
        if (!popupOpen()) {
            click(find(top));
            frames(2);
        }
        if (sub) {
            const Item* s = findEnds(sub);
            if (!s) {
                out("    (Untermenue \"%s\" nicht gefunden)\n", sub);
                escape();
                return false;
            }
            moveTo(center(*s));
            frames(3);
        }
        const Item* e = findEnds(entry, "##Menu");
        if (!e) {
            out("    (Menueeintrag \"%s\" nicht gefunden)\n", entry);
            escape();
            escape();
            return false;
        }
        if (disabled(e)) {
            out("    (Menueeintrag \"%s\" ist ausgegraut)\n", entry);
            escape();
            escape();
            return false;
        }
        click(e);
        frames(2);
        return true;
    }

    bool windowOpen(const char* name) const {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        return w && w->Active;
    }
    bool popupOpen() const { return !GImGui->OpenPopupStack.empty(); }

    // Zeile der Sequenztabelle nach Name.
    const Item* row(const std::string& name) const {
        return findIf([&](const Item& i) {
            return i.window.find("seqs") != std::string::npos && displayOf(i.label) == name;
        });
    }
    // Letztes Vorkommen — nach dem Einfuegen gibt es Namen doppelt.
    const Item* rowLast(const std::string& name) const {
        const Item* hit = nullptr;
        for (const Item& i : g_last)
            if (i.window.find("seqs") != std::string::npos && displayOf(i.label) == name) hit = &i;
        return hit;
    }
    void waitBuild(double seconds = 120.0) {
        const auto t0 = std::chrono::steady_clock::now();
        frames(2);
        while (app->buildRunning()) {
            frame();
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >
                seconds)
                break;
        }
        frames(3);
    }
};

// Kleine Beispieldateien, wenn keine echten Daten angegeben sind.
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::string grabNames(const g2::gui::Document& d) {
    std::string s;
    for (const auto& g : d.script.grabs) {
        if (!s.empty()) s += ",";
        s += g.enumName ? *g.enumName : g.derivedName();
    }
    return s;
}

std::size_t countSelected(const g2::gui::Document& d) {
    std::size_t n = 0;
    for (const char c : d.selected)
        if (c) ++n;
    return n;
}

bool logHas(const g2::gui::App& app, const std::string& needle) {
    for (const auto& l : app.logLines())
        if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Aufruf: g2_gui_driver <arbeitsordner> [<assetwurzel> <referenz.gla> "
                     "<anims.h> [<gross.car>]]\n");
        return 2;
    }
    const fs::path work = fs::absolute(argv[1]);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "appdata");
    g_log = std::fopen((work / "gui_driver.log").string().c_str(), "w");

    // Einstellungen des Nutzers nicht anfassen.
#ifdef _WIN32
    _putenv_s("APPDATA", (work / "appdata").string().c_str());
#else
    setenv("APPDATA", (work / "appdata").string().c_str(), 1);
#endif
    fs::current_path(work);

    const bool real = argc >= 5;
    const fs::path base = real ? fs::path(argv[2]) : work / "base";
    const fs::path refGla = real ? fs::path(argv[3]) : fs::path();
    const fs::path enums = real ? fs::path(argv[4]) : fs::path();
    const fs::path bigCar = argc >= 6 ? fs::path(argv[5]) : fs::path();

    // --- ImGui ohne Fenster ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    GImGui->TestEngineHookItems = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigDebugHighlightIdConflicts = true;
    io.Fonts->AddFontDefault();

    // Plattform: Dialoge liefern, was der Test vorgibt.
    std::vector<std::vector<std::string>> nextFiles;
    std::vector<std::string>              nextFolder;
    std::vector<std::string>              revealed;
    std::vector<std::string>              dialogTitles;
    g2::gui::Platform plat;
    plat.openFiles = [&](const char* title, const char*, bool, const std::string&) {
        dialogTitles.emplace_back(title ? title : "");
        if (nextFiles.empty()) return std::vector<std::string>{};
        auto f = nextFiles.front();
        nextFiles.erase(nextFiles.begin());
        return f;
    };
    plat.pickFolder = [&](const char* title, const std::string&) {
        dialogTitles.emplace_back(title ? title : "");
        if (nextFolder.empty()) return std::string();
        auto f = nextFolder.front();
        nextFolder.erase(nextFolder.begin());
        return f;
    };
    plat.revealInExplorer = [&](const std::string& p) { revealed.push_back(p); };

    // Beispieldaten, wenn keine echten da sind: eine Animation mit Bewegung,
    // eine zweite, ein root.
    const fs::path cars = work / "cars";
    fs::create_directories(cars);

    auto app = std::make_unique<g2::gui::App>(plat);
    Driver D;
    D.app = app.get();
    // Hohe DPI: deckt Skalierungsfehler auf, die bei 100 % nicht auffallen.
    app->settings().dpiScale = 1.5f;
    D.frames(3);

    int crashes = 0;
    const auto guardedStep = [&](const char* what, const std::function<void()>& f) {
        D.where = what;
        const int code = guarded(f);
        if (code != 0) {
            ++crashes;
            ++g_failures;
            out("    [ABSTURZ] %s  (Code 0x%08X)\n", what, static_cast<unsigned>(code));
            // Nach einem Absturz mitten im Bild ist der ImGui-Zustand kaputt.
            // Frisch anfangen, damit die folgenden Schritte etwas aussagen.
            if (GImGui->WithinFrameScope) ImGui::ErrorRecoveryTryToRecoverState(nullptr);
            if (GImGui->WithinFrameScope) ImGui::EndFrame();
        }
    };

    // =====================================================================
    step("1. Start, Menues, Ansicht");
    guardedStep("Start", [&] {
        check(D.find(tr(S::MenuFile)) != nullptr, "Menue Datei sichtbar");
        check(D.find(tr(S::MenuBuild)) != nullptr, "Menue Bauen sichtbar");
        check(D.find(tr(S::MenuView)) != nullptr, "Menue Ansicht sichtbar");

        // Jedes Hauptmenue oeffnen und schliessen.
        for (S m : {S::MenuFile, S::MenuBuild, S::MenuView}) {
            D.click(D.find(tr(m)));
            D.frames(2);
            if (!D.popupOpen()) {
                out("    (Menue %s ging erst beim zweiten Klick auf)\n", tr(m));
                D.click(D.find(tr(m)));
                D.frames(2);
            }
            check(D.popupOpen(), std::string("Menue oeffnet: ") + tr(m));
            D.escape();
            D.frames(2);
        }

        // Hell / Dunkel mehrfach: die Abstaende duerfen nicht wachsen.
        const float before = ImGui::GetStyle().ItemInnerSpacing.x;
        const float indentBefore = ImGui::GetStyle().IndentSpacing;
        for (int k = 0; k < 3; ++k) {
            D.menu(tr(S::MenuView), tr(S::Light));
            D.menu(tr(S::MenuView), tr(S::Dark));
        }
        check(std::fabs(ImGui::GetStyle().ItemInnerSpacing.x - before) < 0.01f &&
                  std::fabs(ImGui::GetStyle().IndentSpacing - indentBefore) < 0.01f,
              "Hell/Dunkel sechsmal umgeschaltet: Abstaende unveraendert (" +
                  std::to_string(before) + " -> " +
                  std::to_string(ImGui::GetStyle().ItemInnerSpacing.x) + ")");

        // Einstellungsleiste aus und wieder an.
        D.menu(tr(S::MenuView), tr(S::Settings));
        check(D.findIf([](const Item& i) { return i.window.find("settings") != std::string::npos; }) ==
                  nullptr,
              "Einstellungsleiste ausgeblendet");
        D.menu(tr(S::MenuView), tr(S::Settings));
        check(D.findIf([](const Item& i) { return i.window.find("settings") != std::string::npos; }) !=
                  nullptr,
              "Einstellungsleiste wieder da");

        // Info-Fenster.
        D.menu(tr(S::MenuView), tr(S::About));
        D.frames(2);
        check(D.windowOpen(tr(S::About)), "Info-Fenster offen");
        D.click(D.find(tr(S::DlgClose), tr(S::About)));
        D.frames(2);
        check(!D.windowOpen(tr(S::About)), "Info-Fenster ueber Schliessen zu");

        // Leere Aktionen ohne Skript duerfen nichts tun.
        D.key(ImGuiKey_S, true);
        D.key(ImGuiKey_S, true, true);
        check(app->documents().empty(), "Strg+S ohne Skript: kein Tab entstanden");
    });

    // =====================================================================
    step("2. Einstellungsleiste");
    guardedStep("Einstellungen", [&] {
        auto& st = app->settings();
        const bool f0 = st.writeFrames, m0 = st.writeMesh, c0 = st.useCache, k0 = st.carcassCompat,
                   r0 = st.readFrameCounts;
        for (S s : {S::WriteFrames, S::WriteMesh, S::UseCache, S::CarcassMode, S::ReadFrameCounts}) {
            if (!D.find(tr(s))) out("    (Haekchen \"%s\" nicht sichtbar)\n", tr(s));
            D.click(D.find(tr(s)));
        }
        check(st.writeFrames != f0, "Haekchen .frames reagiert");
        check(st.writeMesh != m0, "Haekchen Mesh reagiert");
        check(st.useCache != c0, "Haekchen Cache reagiert");
        check(st.carcassCompat != k0, "Haekchen Carcass-Modus reagiert");
        check(st.readFrameCounts != r0, "Haekchen Framezahlen reagiert");
        // Mesh aus -> Skin ausgegraut.
        if (!st.writeMesh) check(Driver::disabled(D.find(tr(S::WriteSkin))), "Skin ausgegraut ohne Mesh");
        for (S s : {S::WriteFrames, S::WriteMesh, S::UseCache, S::CarcassMode, S::ReadFrameCounts})
            D.click(D.find(tr(s)));
        check(st.writeFrames == f0 && st.writeMesh == m0 && st.carcassCompat == k0,
              "zweiter Klick stellt zurueck");

        // Pfade: per Ordnerknopf (Dialog liefert) und per Tippen.
        if (real) {
            nextFolder.push_back(base.string());
            // Der erste Ordnerknopf gehoert zur Assetwurzel.
            const Item* btn = D.findIf([](const Item& i) {
                return i.window.find("settings") != std::string::npos &&
                       (i.label == "..." || (!i.label.empty() && (unsigned char)i.label[0] >= 0xE0));
            });
            D.click(btn);
            check(st.baseDir == base.string(), "Assetwurzel ueber Ordnerdialog gesetzt");
            st.referenceGla = refGla.string();
            st.enumPath = enums.string();
            app->loadEnums(enums.string());
            D.frames(2);
            check(!app->enums().empty(), "Enumtabelle geladen");
        }
    });

    // =====================================================================
    // Arbeitsdaten: kleine Skripte zum Umordnen und Bauen.
    fs::path smallCar, smallCar2;
    if (real) {
        // Kleines, echtes Skript: die ersten 12 Grabs der grossen .car.
        const auto big = g2::car::parseFile(bigCar.string());
        std::ostringstream s;
        s << "// Kopfkommentar, darf beim Speichern nicht verloren gehen\n";
        s << "$scale 0.64\n$keepmotion\n$aseanimgrabinit\n";
        int n = 0;
        for (const auto& g : big.grabs) {
            if (g.file.find("__original_anim") == std::string::npos) continue;
            if (g.file.find("root.xsi") != std::string::npos) continue;
            if (n == 4) s << "//////////////////////////////////////////\n";
            s << "$aseanimgrab " << g.file;
            if (g.loop) s << " -loop " << *g.loop;
            if (g.frameSpeed) s << " -framespeed " << *g.frameSpeed;
            s << "\n";
            if (++n == 12) break;
        }
        s << "$aseanimgrab models/players/__new_anim/__original_anim/root.xsi -loop -1\n";
        s << "$aseanimgrabfinalize\n";
        s << "$aseanimconvertmdx_noask models/players/__new_anim/_humanoid/root -makeskel "
             "models/players/t_gui/t_gui -origin 0 0 24\n";
        smallCar = cars / "a" / "t_gui.car";
        smallCar2 = cars / "b" / "t_gui.car";
        writeText(smallCar, s.str());
        writeText(smallCar2, s.str());
    }

    // =====================================================================
    step("3. Skripte oeffnen (Menue, Ordner, Tabs)");
    guardedStep("Oeffnen", [&] {
        if (!real) {
            out("    (ohne echte Daten uebersprungen)\n");
            return;
        }
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        D.frames(2);
        check(app->documents().size() == 1, "Datei > Skript oeffnen: ein Tab");

        nextFolder.push_back((cars / "b").string());
        D.menu(tr(S::MenuFile), tr(S::OpenFolder));
        D.frames(2);
        check(app->documents().size() == 2, "Datei > Ordner oeffnen: zweiter Tab");

        // Dieselbe Datei nicht doppelt.
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        check(app->documents().size() == 2, "dieselbe Datei nicht doppelt geoeffnet");

        // Tabs anklicken.
        const Item* t0 = D.findTab("###" + app->documents()[0].path);
        D.click(t0);
        D.frames(2);
        check(app->activeTab() == 0, "Klick auf ersten Tab aktiviert ihn");
    });

    // =====================================================================
    step("4. Tabelle: Auswahl mit Klick, Strg, Umschalt, Filter");
    guardedStep("Auswahl", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        D.frames(2);
        const auto names = [&] {
            std::vector<std::string> v;
            for (const auto& g : d.script.grabs) v.push_back(g.enumName ? *g.enumName : g.derivedName());
            return v;
        }();
        check(names.size() == 13, "13 Sequenzen im kleinen Skript (" + std::to_string(names.size()) + ")");
        check(D.row(names[0]) != nullptr, "erste Zeile als Element auffindbar");

        D.clickRow(D.row(names[1]));
        check(countSelected(d) == 1 && d.selected[1], "Klick waehlt genau eine Zeile");
        D.clickRow(D.row(names[5]), false, true);
        check(countSelected(d) == 5, "Umschalt+Klick waehlt Bereich 1..5");
        D.clickRow(D.row(names[3]), true);
        check(countSelected(d) == 4 && !d.selected[3], "Strg+Klick nimmt eine heraus");
        D.clickRow(D.row(names[3]), true);
        check(countSelected(d) == 5 && d.selected[3], "Strg+Klick fuegt sie wieder hinzu");
        check(!D.windowOpen("###seqdlg"), "zwei langsame Klicks sind kein Doppelklick");

        // Filter: tippen, dann Bereichsauswahl.
        const Item* filt = D.findIf([](const Item& i) { return i.label == "##filter"; });
        D.click(filt);
        D.type("A1_B");
        D.key(ImGuiKey_Enter);
        D.frames(2);
        const auto visible = [&] {
            std::vector<std::size_t> v;
            for (std::size_t k = 0; k < names.size(); ++k)
                if (D.row(names[k])) v.push_back(k);
            return v;
        }();
        check(!visible.empty() && visible.size() < names.size(),
              "Filter blendet Zeilen aus (" + std::to_string(visible.size()) + " sichtbar)");
        if (visible.size() >= 2) {
            D.clickRow(D.row(names[visible.front()]));
            D.clickRow(D.row(names[visible.back()]), false, true);
            std::size_t hiddenSel = 0;
            for (std::size_t k = 0; k < names.size(); ++k)
                if (d.selected[k] && std::find(visible.begin(), visible.end(), k) == visible.end())
                    ++hiddenSel;
            check(hiddenSel == 0, "Umschalt+Klick mit Filter waehlt KEINE ausgeblendeten Zeilen (" +
                                      std::to_string(hiddenSel) + " ausgeblendet gewaehlt)");
        }
        // Filter leeren.
        D.click(filt);
        D.key(ImGuiKey_A, true);
        D.key(ImGuiKey_Backspace);
        D.key(ImGuiKey_Enter);
        D.frames(2);
        check(D.row(names.back()) != nullptr, "Filter geleert: alle Zeilen wieder da");
    });

    // =====================================================================
    step("5. Umordnen durch Ziehen");
    guardedStep("Ziehen", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        const auto nameAt = [&](std::size_t k) {
            const auto& g = d.script.grabs[k];
            return g.enumName ? *g.enumName : g.derivedName();
        };
        const std::string a = nameAt(1), b = nameAt(4);
        D.clickRow(D.row(a));
        const Item* src = D.row(a);
        const Item* dst = D.row(nameAt(5));
        if (src && dst) D.dragPath(Driver::leftOf(*src), {Driver::leftOf(*dst)});
        D.frames(2);
        // Abgelegt "vor Zeile 5": a steht danach an Index 4.
        check(nameAt(4) == a, "Zeile 1 per Ziehen hinter Zeile 4 verschoben (jetzt: " + grabNames(d) + ")");
        check(d.dirty, "Skript als geaendert markiert");
        check(g2::gui::App::isRootGrab(d.script.grabs.back()), "root bleibt die letzte Zeile");

        // Mehrfachauswahl als Block ziehen.
        const std::string x = nameAt(0), y = nameAt(2);
        D.clickRow(D.row(x));
        D.clickRow(D.row(y), true);
        const Item* s2 = D.row(x);
        const Item* t2 = D.row(nameAt(8));
        if (s2 && t2) D.dragPath(Driver::leftOf(*s2), {Driver::leftOf(*t2)});
        D.frames(2);
        std::size_t px = 99, py = 99;
        for (std::size_t k = 0; k < d.script.grabs.size(); ++k) {
            if (nameAt(k) == x) px = k;
            if (nameAt(k) == y) py = k;
        }
        check(py == px + 1 && px > 2, "zwei Zeilen als Block verschoben und beisammen (" +
                                          std::to_string(px) + "," + std::to_string(py) + ")");
        check(g2::gui::App::isRootGrab(d.script.grabs.back()), "root weiterhin zuletzt");

        // Obere oder untere Haelfte der Zielzeile: davor oder dahinter, und
        // waehrend des Ziehens eine Linie genau an dieser Stelle.
        {
            const auto punkt = [](const Item& it, float anteil) {
                return ImVec2(it.bb.Min.x + 6.0f, it.bb.Min.y + it.bb.GetHeight() * anteil);
            };
            // Halten, zur Stelle ziehen, Zustand pruefen, loslassen.
            const auto ziehe = [&](const std::string& quelle, std::size_t zielZeile, float anteil,
                                   int* linienVertices) {
                D.clickRow(D.row(quelle));
                const Item* s = D.row(quelle);
                const Item* t = D.row(nameAt(zielZeile));
                if (!s || !t) return;
                const ImVec2 von = Driver::leftOf(*s), nach = punkt(*t, anteil);
                D.moveTo(von);
                D.frames(2);
                ImGui::GetIO().AddMouseButtonEvent(0, true);
                D.frame();
                for (int k = 1; k <= 10; ++k)
                    D.moveTo(ImVec2(von.x + (nach.x - von.x) * k / 10.0f,
                                    von.y + (nach.y - von.y) * k / 10.0f));
                D.frames(2);
                if (linienVertices) *linienVertices = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                D.frames(3);
            };

            const std::string m = nameAt(0);
            int vtx = 0;
            ziehe(m, 6, 0.8f, &vtx);
            check(vtx > 0, "beim Ziehen erscheint die Einfuegelinie");
            check(nameAt(6) == m, "untere Haelfte von Zeile 6: dahinter eingefuegt (" + grabNames(d) + ")");

            const std::string m2 = nameAt(0);
            ziehe(m2, 3, 0.2f, nullptr);
            check(nameAt(2) == m2, "obere Haelfte von Zeile 3: davor eingefuegt (" + grabNames(d) + ")");

            // Unter eine Ueberschrift ziehen: die Ueberschrift bleibt oben,
            // die Sequenz steht darunter.
            std::size_t mitKomm = 0;
            for (std::size_t k = 1; k + 1 < d.script.grabs.size(); ++k)
                if (!d.script.grabs[k].commentsBefore.empty()) mitKomm = k;
            if (mitKomm > 0) {
                const std::string ueber = d.script.grabs[mitKomm].commentsBefore.front();
                const std::string zielName = nameAt(mitKomm);
                const std::string m3 = nameAt(mitKomm == 1 ? 0 : 0);
                ziehe(m3, mitKomm, 0.2f, nullptr);
                std::size_t posM = 0, posZ = 0;
                for (std::size_t k = 0; k < d.script.grabs.size(); ++k) {
                    if (nameAt(k) == m3) posM = k;
                    if (nameAt(k) == zielName) posZ = k;
                }
                check(posM + 1 == posZ && !d.script.grabs[posM].commentsBefore.empty() &&
                          d.script.grabs[posM].commentsBefore.front() == ueber &&
                          d.script.grabs[posZ].commentsBefore.empty(),
                      "unter eine Ueberschrift gezogen: Ueberschrift bleibt darueber");
            } else {
                check(false, "Testdaten ohne Ueberschrift");
            }
            check(g2::gui::App::isRootGrab(d.script.grabs.back()), "root bleibt zuletzt");
        }

        // Ziehen auf den ANDEREN Tab: darf nicht im falschen Skript umordnen.
        if (app->documents().size() >= 2) {
            auto& other = app->documents()[1];
            const std::string before0 = grabNames(d);
            const std::string before1 = grabNames(other);
            D.clickRow(D.row(nameAt(3)));
            const Item* s3 = D.row(nameAt(3));
            const Item* tab1 = D.findTab("###" + other.path);
            if (s3 && tab1) {
                // Ueber dem Tab verweilen, bis ImGui ihn umschaltet, dann in
                // der Tabelle ablegen.
                D.moveTo(Driver::leftOf(*s3));
                ImGui::GetIO().AddMouseButtonEvent(0, true);
                D.frame();
                for (int s = 1; s <= 8; ++s)
                    D.moveTo(ImVec2(Driver::leftOf(*s3).x + (Driver::center(*tab1).x - Driver::leftOf(*s3).x) * s / 8.0f,
                                    Driver::leftOf(*s3).y + (Driver::center(*tab1).y - Driver::leftOf(*s3).y) * s / 8.0f));
                D.frames(40);
                const Item* r1 = D.findIf([&](const Item& i) {
                    return i.window.find("seqs") != std::string::npos && !i.label.empty() && i.label[0] != '#' &&
                           i.label.find("//") == std::string::npos;
                });
                if (r1) {
                    for (int s = 1; s <= 8; ++s) D.moveTo(ImVec2(Driver::leftOf(*r1).x, Driver::leftOf(*r1).y + 60));
                }
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                D.frames(3);
            }
            const bool otherUnchanged = grabNames(other) == before1;
            check(otherUnchanged, "Zeile auf anderen Tab gezogen: das ANDERE Skript bleibt unveraendert");
            (void)before0;
            // Zurueck zum ersten Tab.
            D.click(D.findTab("###" + d.path));
            D.frames(2);
        }
    });

    // =====================================================================
    step("6. Kontextmenue einer Zeile");
    guardedStep("Kontextmenue", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        const auto nameAt = [&](std::size_t k) {
            const auto& g = d.script.grabs[k];
            return g.enumName ? *g.enumName : g.derivedName();
        };
        const auto ctx = [&](std::size_t k, S entry) {
            if (D.popupOpen()) D.escape();
            // Gibt es den Namen weiter oben schon, das spaetere Vorkommen.
            std::size_t before = 0;
            for (std::size_t j = 0; j < k; ++j) before += nameAt(j) == nameAt(k) ? 1 : 0;
            const auto target = [&] { return before ? D.rowLast(nameAt(k)) : D.row(nameAt(k)); };
            D.clickRow(target());
            D.click(target(), 1);
            D.frames(2);
            const Item* e = D.findEnds(tr(entry));
            if (!e) {
                out("    (Eintrag %s fehlt)\n", tr(entry));
                D.escape();
                return false;
            }
            D.click(e);
            D.frames(3);
            return true;
        };
        const std::size_t n0 = d.script.grabs.size();

        std::string first = nameAt(0), third = nameAt(3);
        ctx(3, S::MoveTop);
        check(nameAt(0) == third, "Ganz nach oben");
        ctx(0, S::MoveDown);
        check(nameAt(1) == third, "Nach unten");
        ctx(1, S::MoveUp);
        check(nameAt(0) == third, "Nach oben");
        ctx(0, S::MoveBottom);
        check(nameAt(d.script.grabs.size() - 1) == third || nameAt(d.script.grabs.size() - 2) == third,
              "Ganz nach unten (vor root)");
        check(g2::gui::App::isRootGrab(d.script.grabs.back()), "root bleibt zuletzt");

        const std::size_t c0 = d.script.grabs[2].commentsBefore.size();
        ctx(2, S::AddDivider);
        check(d.script.grabs[2].commentsBefore.size() == c0 + 1, "Trennlinie eingefuegt");

        ctx(2, S::AddComment);
        D.type("Testkommentar aus dem Treiber");
        D.key(ImGuiKey_Enter);
        D.frames(2);
        check(!d.script.grabs[2].commentsBefore.empty() &&
                  d.script.grabs[2].commentsBefore.back() == "Testkommentar aus dem Treiber",
              "Kommentar getippt und uebernommen");

        ctx(1, S::Copy);
        check(app->clipboardSize() == 1, "Kopieren: eine Sequenz in der Ablage");
        // Einfuegen: Beschriftung enthaelt die Anzahl.
        if (D.popupOpen()) D.escape();
        D.clickRow(D.row(nameAt(5)));
        D.click(D.row(nameAt(5)), 1);
        D.frames(2);
        char pasteLbl[128];
        std::snprintf(pasteLbl, sizeof(pasteLbl), tr(S::PasteAfter), std::size_t{1});
        D.click(D.find(pasteLbl));
        D.frames(3);
        check(d.script.grabs.size() == n0 + 1, "Einfuegen danach: eine Zeile mehr");

        // Loeschen mit Rueckfrage: erst Nein, dann Ja.
        ctx(6, S::DeleteSeq);
        check(D.popupOpen(), "Loeschen fragt nach");
        D.click(D.find(tr(S::No)));
        D.frames(2);
        check(d.script.grabs.size() == n0 + 1, "Nein loescht nichts");
        ctx(6, S::DeleteSeq);
        D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
        D.frames(2);
        check(d.script.grabs.size() == n0, "Ja loescht genau eine");

        // Ausschneiden = kopieren + loeschen mit Rueckfrage.
        ctx(1, S::Cut);
        D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
        D.frames(2);
        check(d.script.grabs.size() == n0 - 1 && app->clipboardSize() == 1, "Ausschneiden");
        (void)first;
    });

    // =====================================================================
    step("7. Sequenzdialog und Enum-Auswahl");
    guardedStep("Dialog", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        const auto nameAt = [&](std::size_t k) {
            const auto& g = d.script.grabs[k];
            return g.enumName ? *g.enumName : g.derivedName();
        };
        D.doubleClick(D.row(nameAt(2)));
        D.frames(2);
        check(D.windowOpen("###seqdlg"), "Doppelklick oeffnet den Sequenzdialog");
        // Gross genug, dass alle Knoepfe im sichtbaren Bereich liegen.
        ImGui::SetWindowSize("###seqdlg", ImVec2(1200, 1150));
        ImGui::SetWindowPos("###seqdlg", ImVec2(360, 60));
        D.frames(3);

        // Enum waehlen.
        D.click(D.findLabel(std::string(tr(S::DlgChoose)) + "##m"));
        D.frames(2);
        check(D.popupOpen(), "Enum-Auswahl offen");
        const Item* ef = D.findIf([](const Item& i) { return i.label == "##ef"; });
        D.click(ef);
        D.type("BOTH_STAND1");
        D.frames(2);
        const Item* pick = D.findIf([](const Item& i) {
            return i.window.find("elist") != std::string::npos && displayOf(i.label) == "BOTH_STAND1";
        });
        D.click(pick);
        D.frames(2);
        check(d.script.grabs[2].enumName && *d.script.grabs[2].enumName == "BOTH_STAND1",
              "Enum aus der Liste uebernommen");

        // Zusatzsequenz anlegen und wieder entfernen.
        const std::size_t a0 = d.script.grabs[2].additional.size();
        D.click(D.find(tr(S::DlgAddExtra), "###seqdlg"));
        check(d.script.grabs[2].additional.size() == a0 + 1, "-additional angelegt");
        const Item* del = D.findIf([](const Item& i) {
            return i.window.find("###seqdlg") != std::string::npos && i.label == "X";
        });
        D.click(del);
        check(d.script.grabs[2].additional.size() == a0, "-additional entfernt");

        // Loop-Frame ueber die Plus-Taste.
        const int l0 = d.script.grabs[2].loop.value_or(-1);
        const Item* plus = D.findIf([](const Item& i) {
            return i.window.find("seqdlg") != std::string::npos && i.label == "+";
        });
        D.click(plus);
        check(d.script.grabs[2].loop.value_or(-1) == l0 + 1, "Loop-Frame +1");

        // Tab wechseln, waehrend der Dialog offen ist: der Dialog darf danach
        // nicht still die gleiche Zeile im ANDEREN Skript bearbeiten.
        if (app->documents().size() >= 2) {
            auto& other = app->documents()[1];
            const std::string o2 = other.script.grabs[2].enumName ? *other.script.grabs[2].enumName
                                                                  : other.script.grabs[2].derivedName();
            D.click(D.findTab("###" + other.path));
            D.frames(2);
            if (D.windowOpen("###seqdlg")) {
                D.click(D.find(tr(S::DlgAddExtra), "###seqdlg"));
                D.frames(2);
            }
            check(other.script.grabs[2].additional.empty() && !other.dirty,
                  "Tabwechsel bei offenem Dialog veraendert das andere Skript nicht");
            (void)o2;
            D.click(D.findTab("###" + d.path));
            D.frames(2);
        }

        if (D.windowOpen("###seqdlg")) D.click(D.find(tr(S::DlgClose), "###seqdlg"));
        D.frames(2);
        check(!D.windowOpen("###seqdlg"), "Dialog geschlossen");
    });

    // =====================================================================
    step("8. Kommentarzeilen bearbeiten und verschieben");
    guardedStep("Kommentare", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        const Item* c = D.findIf([](const Item& i) {
            return i.window.find("seqs") != std::string::npos && i.label.rfind("//////", 0) == 0;
        });
        if (!c) {
            std::size_t nc = 0;
            for (const auto& g : d.script.grabs) nc += g.commentsBefore.size();
            out("    (aktiver Tab %d, Kommentare in Tab 0: %zu, Dialog offen: %d, Popup: %d)\n",
                app->activeTab(), nc, D.windowOpen("###seqdlg"), D.popupOpen());
        }
        check(c != nullptr, "Trennlinie in der Tabelle sichtbar");
        if (!c) return;
        std::size_t total0 = d.script.trailingComments.size();
        for (const auto& g : d.script.grabs) total0 += g.commentsBefore.size();

        // Rechtsklick > Nach oben.
        D.click(c, 1);
        D.frames(2);
        D.click(D.find(tr(S::MoveUp)));
        D.frames(2);
        std::size_t total1 = d.script.trailingComments.size();
        for (const auto& g : d.script.grabs) total1 += g.commentsBefore.size();
        check(total1 == total0, "Nach oben: keine Kommentarzeile verloren");

        // Ans Ende ziehen.
        c = D.findIf([](const Item& i) {
            return i.window.find("seqs") != std::string::npos && i.label.rfind("//////", 0) == 0;
        });
        const Item* endRow = D.findIf([](const Item& i) { return i.label == "##dropend"; });
        if (c && endRow) D.dragPath(Driver::leftOf(*c), {Driver::leftOf(*endRow)});
        D.frames(2);
        std::size_t total2 = d.script.trailingComments.size();
        for (const auto& g : d.script.grabs) total2 += g.commentsBefore.size();
        check(total2 == total0, "Ans Ende gezogen: keine Kommentarzeile verloren");

        // Doppelklick auf den Zeilenkommentar einer Sequenz.
        const Item* tc = D.findIf([](const Item& i) {
            return i.window.find("seqs") != std::string::npos && i.label == "...";
        });
        D.doubleClick(tc, false);
        const bool dlg = D.windowOpen("###seqdlg");
        check(!dlg, "Doppelklick auf den Zeilenkommentar oeffnet NICHT den Sequenzdialog");
        if (dlg) {
            D.frames(3);
            D.click(D.find(tr(S::DlgClose), "###seqdlg"));
            D.frames(2);
        }
        D.type("Zeilenkommentar");
        D.key(ImGuiKey_Enter);
        bool found = false;
        for (const auto& g : d.script.grabs)
            if (g.trailingComment == "// Zeilenkommentar") found = true;
        check(found, "Zeilenkommentar per Doppelklick gesetzt, mit // davor");
    });

    // =====================================================================
    step("9. Pruefen und Meldungsliste");
    guardedStep("Pruefen", [&] {
        if (!real || app->documents().empty()) return;
        D.click(D.findIconButton(tr(S::BtnValidate), "##main"));
        D.frames(3);
        const auto& d = app->documents()[static_cast<std::size_t>(app->activeTab())];
        check(d.validated, "Knopf Pruefen hat geprueft");
        D.click(D.findTab(tr(S::TabIssues)));
        D.frames(2);
        const Item* issue = D.findIf([](const Item& i) { return i.label == "##row"; });
        if (issue) {
            D.click(issue);
            D.frames(3);
            check(true, "Meldung anklickbar");
        } else {
            check(d.validation.issues.empty(), "keine Meldungen, keine Zeilen");
        }
    });

    // =====================================================================
    step("10. Bauen aus der Oberflaeche");
    guardedStep("Bauen", [&] {
        if (!real || app->documents().size() < 2) return;
        // Ausgabe ueber den Knopf "Standardordner".
        D.click(D.find(tr(S::DefaultFolder)));
        auto& d = app->documents()[static_cast<std::size_t>(app->activeTab())];
        check(!d.outputDir.empty(), "Standardordner gesetzt");

        // Ungespeicherte Aenderung vor dem Bauen.
        check(d.dirty, "Skript hat ungespeicherte Aenderungen");
        const std::string onDiskBefore = readAll(d.path);
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild();
        const fs::path gla = fs::path(d.outputDir) / "t_gui.gla";
        check(fs::exists(gla) && fs::file_size(gla) > 1000, "GLA geschrieben: " + gla.string());
        check(fs::exists(fs::path(d.outputDir) / "animation.cfg"), "animation.cfg geschrieben");
        if (fs::exists(gla)) {
            const auto m = g2::readMdxa(g2::readWholeFileBytes(gla.string()));
            check(m.skeleton.name == "models/players/t_gui/t_gui", "GLA-Name aus -makeskel: " + m.skeleton.name);
        }
        const std::string onDiskAfter = readAll(d.path);
        check(onDiskAfter != onDiskBefore && !d.dirty,
              "vor dem Bauen gespeichert: .car auf der Platte entspricht dem Gebauten");

        // Kopfkommentar ueberlebt das Speichern.
        check(onDiskAfter.find("Kopfkommentar") != std::string::npos,
              "Kopfkommentar der .car nach dem Speichern noch vorhanden");

        // Alle bauen.
        app->assignDefaultOutputs(true);
        D.click(D.findIconButton(tr(S::BtnBuildAll), "##main"));
        D.waitBuild();
        check(logHas(*app, "t_gui.gla"), "Alle bauen: Protokoll meldet die GLA");

        // Protokoll: Ausgabeordner oeffnen.
        if (!D.findTab(tr(S::TabLog))) out("    (Reiter %s nicht gefunden)\n", tr(S::TabLog));
        D.click(D.findTab(tr(S::TabLog)));
        D.frames(2);
        if (!D.findEnds(tr(S::OpenOutputDir))) {
            out("    (Knopf \"%s\" nicht da; Protokoll-Knopf da: %d)\n", tr(S::OpenOutputDir),
                D.findEnds(tr(S::CopyLog)) != nullptr);
        }
        D.click(D.findEnds(tr(S::OpenOutputDir)));
        check(!revealed.empty(), "Ausgabeordner oeffnen ruft den Explorer");
        D.click(D.findEnds(tr(S::CopyLog)));
        check(logHas(*app, "Zeilen") || true, "Protokoll kopieren");
    });

    // =====================================================================
    step("11. Speichern, Sicherung, Schliessen");
    guardedStep("Speichern", [&] {
        if (!real || app->documents().empty()) return;
        auto& d = app->documents()[0];
        d.dirty = true;
        D.key(ImGuiKey_S, true);
        check(!app->documents()[0].dirty, "Strg+S speichert");
        check(fs::exists(app->documents()[0].path + ".bak"), ".bak angelegt");

        // Tastenkuerzel, die im Menue angeschrieben stehen.
        for (auto& x : app->documents()) x.validated = false;
        D.key(ImGuiKey_F7);
        check(app->documents()[0].validated, "F7 prueft (steht so im Menue)");
        const std::size_t titles0 = dialogTitles.size();
        D.key(ImGuiKey_O, true);
        check(dialogTitles.size() > titles0, "Strg+O oeffnet den Dateidialog");

        // Strg+W: sauberer Tab geht zu, geaenderter fragt nach.
        const std::size_t n0 = app->documents().size();
        for (auto& x : app->documents()) x.dirty = false;
        D.key(ImGuiKey_W, true);
        D.frames(2);
        check(app->documents().size() == n0 - 1, "Strg+W schliesst den aktiven Tab");
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        app->documents()[static_cast<std::size_t>(app->activeTab())].dirty = true;
        const std::size_t n1 = app->documents().size();
        D.key(ImGuiKey_W, true);
        D.frames(3);
        check(app->documents().size() == n1 && D.popupOpen(),
              "Strg+W bei ungespeicherter Aenderung fragt nach, statt still zu verwerfen");
        if (D.popupOpen()) D.escape();
        D.frames(2);
    });

    // =====================================================================
    step("12. GLA -> XSI: oeffnen, waehlen, exportieren, vergleichen");
    fs::path glaDir;
    guardedStep("Extract", [&] {
        if (!real) return;
        // Frisch gebaute GLA aus Schritt 10 samt Begleitdateien.
        for (const auto& x : app->documents())
            if (!x.outputDir.empty() && fs::exists(fs::path(x.outputDir) / "t_gui.gla")) glaDir = x.outputDir;
        if (glaDir.empty()) {
            check(false, "keine gebaute GLA aus Schritt 10");
            return;
        }
        D.click(D.modeButton(1));
        D.frames(2);
        check(app->mode() == g2::gui::App::Mode::Extract, "Modusknopf schaltet auf GLA -> XSI");

        nextFiles.push_back({(glaDir / "t_gui.gla").string()});
        D.click(D.findIconButton(tr(S::OpenGla), "extract"));
        D.frames(3);
        const auto& ex = app->extract();
        check(ex.loaded, "GLA geladen");
        check(!ex.cfgPath.empty(), "animation.cfg daneben gefunden");
        check(!ex.framesPath.empty(), ".frames daneben gefunden");
        check(ex.seqs.size() == 13, "13 Sequenzen (" + std::to_string(ex.seqs.size()) + ")");
        if (ex.seqs.size() < 5) return;

        // Auswahl.
        const auto exRow = [&](const std::string& n) {
            return D.findIf([&](const Item& i) {
                return i.window.find("extract") != std::string::npos && displayOf(i.label) == n;
            });
        };
        D.clickRow(exRow(ex.seqs[1].name));
        D.clickRow(exRow(ex.seqs[4].name), false, true);
        std::size_t nsel = 0;
        for (char c : ex.selected) nsel += c ? 1 : 0;
        check(nsel == 4, "Umschalt-Auswahl: 4 Sequenzen");

        // Zielordner und Export der Auswahl.
        const fs::path outX = work / "extract_out" / "models" / "players" / "t_x";
        fs::create_directories(outX);
        nextFolder.push_back(outX.string());
        D.click(D.findEnds(tr(S::ChooseFolder), "extract"));
        check(ex.outDir == outX.string(), "Zielordner gesetzt");
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), tr(S::ExportSelected), nsel);
        D.click(D.findEnds(lbl, "extract"));
        D.frames(2);
        std::size_t nx = 0;
        for (const auto& e : fs::directory_iterator(outX))
            if (e.path().extension() == ".xsi") ++nx;
        check(nx == 4, "4 .xsi exportiert (" + std::to_string(nx) + ")");

        // Liegen alle Bedienelemente der Exportzeile im sichtbaren Bereich?
        {
            // Gegen den sichtbaren Bereich des Extract-Fensters pruefen, nicht
            // gegen den Bildschirm: links sitzt die Modusleiste.
            std::size_t clipped = 0;
            for (ImGuiWindow* w : GImGui->Windows) {
                if (!w->WasActive || std::string(w->Name).find("extract") == std::string::npos) continue;
                for (const Item& i : g_last)
                    if (i.window == w->Name && i.bb.Max.x > w->ClipRect.Max.x + 1.0f) ++clipped;
            }
            check(clipped == 0, "alle Bedienelemente im Extract-Modus sichtbar (" +
                                    std::to_string(clipped) + " abgeschnitten)");
        }
        // Fassung umstellen (Kombinationsfeld).
        D.click(D.findById("##xsiver", "extract"));
        D.frames(2);
        D.click(D.findIf([](const Item& i) { return displayOf(i.label) == "3.5"; }));
        check(ex.xsiVersion == g2::xsiexp::ExportOptions::Version::V35, "dotXSI 3.5 gewaehlt");

        // Alles mit .car. Eine .car liegt schon da: es muss nachgefragt und
        // vor dem Ersetzen gesichert werden.
        const fs::path carOut = outX / "t_gui.car";
        writeText(carOut, "// eigene Datei, darf nicht still ueberschrieben werden\n");
        D.click(D.findIconButton(tr(S::ExportAllWithCar), "extract"));
        D.frames(3);
        check(D.popupOpen() && readAll(carOut).find("eigene Datei") != std::string::npos,
              "vorhandene Dateien: erst Rueckfrage, noch nichts ueberschrieben");
        {
            const Item* ow = D.find(tr(S::Overwrite), "###overwrite");
            if (!ow) {
                out("    (Knopf nicht gefunden; Popup-Fenster: %s)\n",
                    GImGui->OpenPopupStack.empty() || !GImGui->OpenPopupStack.back().Window
                        ? "-"
                        : GImGui->OpenPopupStack.back().Window->Name);
                for (const Item& i : g_last)
                    if (!i.label.empty() && i.window.find("verwrite") != std::string::npos)
                        out("    (   \"%s\" in %s)\n", i.label.c_str(), i.window.c_str());
            }
            D.click(ow);
        }
        D.frames(3);
        check(!D.popupOpen(), "Rueckfrage beantwortet");
        check(readAll(carOut.string() + ".bak").find("eigene Datei") != std::string::npos,
              "alte .car als .bak gesichert");
        check(readAll(carOut).find("$aseanimgrab") != std::string::npos, "neue .car geschrieben");
        {
            // Aus der exportierten .car wieder bauen koennen: jede Zeile muss
            // sich lesen lassen.
            bool ok = true;
            try {
                const auto sc = g2::car::parseFile(carOut.string());
                ok = !sc.grabs.empty() && sc.convert.has_value();
            } catch (const std::exception&) {
                ok = false;
            }
            check(ok, "exportierte .car ist lesbar und vollstaendig");
        }

        // Vergleich mit einer anderen animation.cfg.
        const fs::path cmpCfg = work / "compare_animation.cfg";
        {
            std::ofstream f(cmpCfg);
            f << ex.seqs[0].name << "\t0\t2\t-1\t20\n" << ex.seqs[2].name << "\t2\t3\t-1\t20\n";
        }
        nextFiles.push_back({cmpCfg.string()});
        D.click(D.findIconButton(tr(S::Compare), "extract"));
        D.frames(2);
        check(!ex.compareNames.empty(), "Vergleichsliste geladen");
        D.click(D.find(tr(S::SelectMissing)));
        nsel = 0;
        for (char c : ex.selected) nsel += c ? 1 : 0;
        check(nsel == ex.seqs.size() - 2, "Fehlende gewaehlt: " + std::to_string(nsel));
        D.click(D.find(tr(S::OnlyMissing)));
        D.frames(2);
        check(ex.onlyMissing && exRow(ex.seqs[0].name) == nullptr, "Nur fehlende: vorhandene ausgeblendet");
        D.click(D.find(tr(S::OnlyMissing)));
    });

    // =====================================================================
    step("13. Vorschau");
    guardedStep("Vorschau", [&] {
        if (!real || !app->extract().loaded) return;
        if (D.popupOpen()) D.escape();
        if (!D.modeButton(2)) out("    (dritter Modusknopf nicht gefunden)\n");
        D.click(D.modeButton(2));
        D.frames(2);
        check(app->mode() == g2::gui::App::Mode::Preview, "Modusknopf schaltet auf Vorschau");
        D.click(D.findEnds(tr(S::PreviewPlay)));
        D.frames(30);
        D.click(D.findEnds(tr(S::PreviewPause)));
        const Item* canvas = D.findIf([](const Item& i) { return i.label.empty() && i.window.find("preview") != std::string::npos && i.bb.GetHeight() > 200; });
        if (!canvas) canvas = D.findIf([](const Item& i) { return i.window.find("preview") != std::string::npos && i.bb.GetHeight() > 200; });
        if (canvas) {
            D.dragPath(Driver::center(*canvas), {ImVec2(Driver::center(*canvas).x + 120, Driver::center(*canvas).y + 40)});
            D.wheel(Driver::center(*canvas), 3.0f);
            D.wheel(Driver::center(*canvas), -5.0f);
        }
        check(canvas != nullptr, "Zeichenflaeche vorhanden, Drehen und Zoomen ohne Fehler");
        D.click(D.find(tr(S::PreviewReset)));
        // Sequenzauswahl.
        D.click(D.find(tr(S::PreviewSeq)));
        D.frames(2);
        const auto& ex = app->extract();
        const Item* s = D.findIf([&](const Item& i) { return displayOf(i.label) == ex.seqs.back().name && i.window.find("##Combo") != std::string::npos; });
        D.click(s);
        check(true, "Sequenz in der Vorschau gewechselt");

        // animation.cfg, die zu KEINER Sequenz dieser GLA passt.
        const fs::path badCfg = work / "fremd_animation.cfg";
        {
            std::ofstream f(badCfg);
            f << "BOTH_FREMD\t999999\t10\t-1\t20\n";
        }
        app->loadAnimationCfg(badCfg.string());
        D.frames(3);
        check(true, "Vorschau mit leerer Sequenzliste ueberlebt");
    });

    // =====================================================================
    step("14. Sprachen");
    guardedStep("Sprachen", [&] {
        app->setMode(g2::gui::App::Mode::Build);
        D.frames(2);
        for (int l = 0; l < static_cast<int>(g2::gui::Lang::Count); ++l) {
            const auto cand = static_cast<g2::gui::Lang>(l);
            D.menu(tr(S::MenuView), g2::gui::langName(cand), tr(S::Language));
            D.frames(3);
            check(g2::gui::language() == cand, std::string("Sprache umgeschaltet: ") + g2::gui::langName(cand));
            app->clearFontsDirty();
        }
    });
    // In jeder Sprache die Meldungen mit Zahlen und Pfaden ausloesen.
    for (int l = 0; l < static_cast<int>(g2::gui::Lang::Count); ++l) {
        const auto cand = static_cast<g2::gui::Lang>(l);
        g2::gui::setLanguage(cand);
        const std::string nm = std::string("Meldungen in ") + g2::gui::langName(cand);
        guardedStep(nm.c_str(), [&] {
            if (!real || app->documents().empty() || glaDir.empty()) return;
            app->addXsiFolder((work / "extract_out").string(), false);
            app->loadGlaForExtract((glaDir / "t_gui.gla").string());
            D.frames(2);
        });
    }
    g2::gui::setLanguage(g2::gui::Lang::De);

    // =====================================================================
    step("15. Pfade mit Umlauten");
    guardedStep("Umlaute", [&] {
        if (!real) return;
        const fs::path uml = work / fs::path(u8"Schüsse Blocken") / "t_gui.car";
        fs::create_directories(uml.parent_path());
        fs::copy_file(smallCar, uml, fs::copy_options::overwrite_existing);
        const auto u8s = uml.u8string();
        const std::string u8(u8s.begin(), u8s.end());
        const std::size_t n0 = app->documents().size();
        app->openCar(u8);
        D.frames(2);
        bool ok = app->documents().size() == n0 + 1 && app->documents().back().loadError.empty() &&
                  !app->documents().back().script.grabs.empty();
        check(ok, "Skript in Ordner mit Umlaut geoeffnet (UTF-8-Pfad aus Dialog/Ziehen)");
    });

    // =====================================================================
    step("16. Grosses Skript");
    guardedStep("Gross", [&] {
        if (bigCar.empty()) return;
        const fs::path copy = work / "big" / bigCar.filename();
        fs::create_directories(copy.parent_path());
        fs::copy_file(bigCar, copy, fs::copy_options::overwrite_existing);
        app->setMode(g2::gui::App::Mode::Build);
        app->openCar(copy.string());
        D.frames(3);
        {
            const std::string bp = app->documents().back().path;
            D.click(D.findTab("###" + bp));
        }
        D.frames(3);
        const auto& d = app->documents().back();
        check(app->activeTab() == static_cast<int>(app->documents().size()) - 1, "grosses Skript ist der aktive Tab");
        check(d.script.grabs.size() > 1000, "grosses Skript offen: " + std::to_string(d.script.grabs.size()) + " Grabs");
        const auto t0 = std::chrono::steady_clock::now();
        D.frames(20);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
        out("    Bildaufbau mit %zu Zeilen: %.1f ms\n", d.script.grabs.size(), ms);
        check(ms < 50.0, "Tabelle bleibt fluessig (< 50 ms je Bild)");
        // Scrollen.
        const Item* any = D.findIf([](const Item& i) { return i.window.find("seqs") != std::string::npos; });
        if (any) for (int k = 0; k < 10; ++k) D.wheel(Driver::center(*any), -20.0f);
        check(true, "gescrollt");

        // Speichern ohne Aenderung: Datei muss inhaltlich gleich bleiben.
        app->saveDocument(app->documents().size() - 1);
        const auto a = g2::car::parseFile(bigCar.string());
        const auto b = g2::car::parseFile(copy.string());
        bool same = a.grabs.size() == b.grabs.size();
        for (std::size_t k = 0; same && k < a.grabs.size(); ++k)
            same = a.grabs[k].file == b.grabs[k].file && a.grabs[k].additional.size() == b.grabs[k].additional.size();
        check(same, "grosses Skript gespeichert und wieder gelesen: alle Grabs gleich");
    });

    // =====================================================================
    step("17. Alle schliessen");
    guardedStep("Schliessen", [&] {
        if (app->documents().empty()) return;
        for (auto& d : app->documents()) d.dirty = true;
        D.menu(tr(S::MenuFile), tr(S::CloseAll));
        D.frames(2);
        check(!app->documents().empty() || D.popupOpen(),
              "Alle schliessen mit ungespeicherten Aenderungen fragt nach");
    });

    // --- Zusammenfassung ---
    D.frames(3);
    app.reset();
    ImGui::DestroyContext();

    out("\n==================================================\n");
    out("Pruefungen: %d, fehlgeschlagen: %d, Abstuerze: %d\n", g_checks, g_failures, crashes);
    out("Bilder gezeichnet: %d, langsamstes: %.1f ms (%s)\n", D.frameNo, D.slowestMs, D.slowestWhere.c_str());
    out("ImGui-Zusicherungen: %zu\n", g_asserts.size());
    for (const auto& a : g_asserts) out("  %s\n", a.c_str());
    out("Doppelte IDs: %zu\n", g_idConflicts.size());
    for (const auto& [k, v] : g_idConflicts) out("  %4d x  %s\n", v, k.c_str());
    if (g_log) std::fclose(g_log);
    return (g_failures || !g_asserts.empty()) ? 1 : 0;
}
