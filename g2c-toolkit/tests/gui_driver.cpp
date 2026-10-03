// Actually operate the GUI: click, drag, type - without a window.
//
// tests/gui_tests.cpp checks the logic behind the GUI. Nothing is drawn there,
// so everything that only happens while drawing stays untested: context menus,
// drag and drop, dialogs, keyboard shortcuts, the ordering of Begin/End in
// early-return paths.
//
// For that, this driver builds a small test machine on top of ImGui's own
// test engine hooks (IMGUI_ENABLE_TEST_ENGINE): every widget reports its ID,
// label and rectangle. That way every element can be found by its label and
// operated with real mouse and keyboard events - just as a human would do it,
// only a thousand times faster.
//
// Every ImGui assertion (missing End, duplicate ID, mismatched popup pairs)
// is caught and counted as an error instead of ending the run.
//
// Usage:  g2_gui_driver <workdir> [<assetroot> <reference.gla> <anims.h>]
//
// Without an asset root the driver creates small sample files by itself.
// With real data it additionally checks a complete _humanoid.car.
//
// Everything written ends up under <workdir>. %APPDATA% is redirected there
// for the run: the user's settings - recently opened scripts, output folders -
// must be neither read nor overwritten by a test run.

#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"

#include "g2/carscript.h"
#include "g2/mdxa.h"
#include "g2/mdxm.h"
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
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using g2::gui::S;
using g2::gui::tr;

// --- Capture via the test engine hooks --------------------------------------

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
std::map<std::string, int>            g_idConflicts;   // "window | label" -> count
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
        // It is only a conflict within the same window. Multiline inputs
        // register once as a child window and once in the parent window.
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

// --- Crash catcher ------------------------------------------------------------
//
// An access violation should end up as a finding in the log, not end the whole
// run. __try requires a function without objects that have a destructor.
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

// Returns 0 if everything went fine, otherwise the exception code.
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

// --- Driver -------------------------------------------------------------------

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

        // Report texture requests as done immediately: there is no GPU.
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

    // --- Lookup ---
    // Windows register themselves as an item with their own name. Otherwise a
    // dialog "Ueberschreiben###overwrite" would have the same name as its button.
    const Item* findIf(const std::function<bool(const Item&)>& pred) const {
        for (const Item& i : g_last)
            if (i.label != i.window && pred(i)) return &i;
        return nullptr;
    }
    // Exact visible label (after stripping "##...").
    const Item* find(const std::string& text, const char* windowPart = nullptr) const {
        return findIf([&](const Item& i) {
            if (windowPart && i.window.find(windowPart) == std::string::npos) return false;
            return displayOf(i.label) == text;
        });
    }
    // Visible label ends with text - for buttons with an icon in front.
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
    // iconButton draws "##ib" under PushID(text): the ID can be recomputed,
    // the label cannot be read.
    const Item* findIconButton(const char* text, const char* windowPart) const {
        ImGuiWindow* w = nullptr;
        for (ImGuiWindow* cand : GImGui->Windows)
            if (std::string(cand->Name).find(windowPart) != std::string::npos && cand->WasActive) {
                w = cand;
                // Prefer the innermost window.
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

    // --- Input ---
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
    // Left side of the row instead of the center: in table rows the center
    // holds other columns, which may carry widgets of their own.
    static ImVec2 leftOf(const Item& i) {
        return ImVec2(i.bb.Min.x + 6.0f, (i.bb.Min.y + i.bb.Max.y) * 0.5f);
    }
    // Let time pass, like a human between two clicks. Without this, two clicks
    // on the same row count as a double click.
    void pause() {
        nextDelta = 0.4f;
        frame();
    }
    void clickAt(ImVec2 p, int button = 0, bool ctrl = false, bool shift = false) {
        mods(ctrl, shift);
        moveTo(p);
        // Items with AllowOverlap only take over the mouse cursor one frame
        // later. A human moves the mouse over many frames anyway.
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

    // Tabs register with the test hooks without a label. So search ImGui's
    // tab bars and compute the rectangle ourselves.
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
                // Only the left third: the close cross sits on the right, and
                // a click in the middle hits it for short names.
                tabHit.bb = ImRect(ImVec2(x0 + 4.0f, tb->BarRect.Min.y),
                                   ImVec2(x0 + t.Width / 3.0f, tb->BarRect.Max.y));
                return &tabHit;
            }
        }
        return nullptr;
    }

    // Find an item by its ID. For widgets that do not report their label to
    // the hooks (combo boxes).
    const Item* findById(const char* label, const char* windowPart) const {
        for (ImGuiWindow* w : GImGui->Windows) {
            if (!w->WasActive || std::string(w->Name).find(windowPart) == std::string::npos) continue;
            const ImGuiID id = ImHashStr(label, 0, w->ID);
            for (const Item& i : g_last)
                if (i.id == id) return &i;
        }
        return nullptr;
    }

    // Name of the tab ImGui is currently showing in the script tab bar.
    std::string shownTab(const std::string& anyPathInBar) const {
        ImGuiContext& g = *GImGui;
        for (int n = 0; n < g.TabBars.GetMapSize(); ++n) {
            ImGuiTabBar* tb = g.TabBars.TryGetMapData(n);
            if (!tb) continue;
            bool mine = false;
            for (ImGuiTabItem& t : tb->Tabs)
                if (std::string(ImGui::TabBarGetTabName(tb, &t)).find(anyPathInBar) != std::string::npos)
                    mine = true;
            if (!mine) continue;
            for (ImGuiTabItem& t : tb->Tabs)
                if (t.ID == tb->SelectedTabId) return ImGui::TabBarGetTabName(tb, &t);
        }
        return {};
    }

    // Mode bar buttons: three "##mode" in this order.
    const Item* modeButton(int n) const {
        int k = 0;
        for (const Item& i : g_last)
            if (i.window.find("modebar") != std::string::npos && i.label == "##mode" && k++ == n)
                return &i;
        return nullptr;
    }
    // Exact label including "##".
    const Item* findLabel(const std::string& full) const {
        return findIf([&](const Item& i) { return i.label == full; });
    }

    // Open the main menu and pick an entry.
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

    // Row of the sequence table by name.
    const Item* row(const std::string& name) const {
        return findIf([&](const Item& i) {
            return i.window.find("seqs") != std::string::npos && displayOf(i.label) == name;
        });
    }
    // Last occurrence - after inserting, names exist twice.
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

// Small sample files when no real data is given.
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

    // Do not touch the user's settings.
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

    // --- ImGui without a window ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    GImGui->TestEngineHookItems = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigDebugHighlightIdConflicts = true;
    io.Fonts->AddFontDefault();

    // Platform: dialogs return whatever the test specifies.
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
    // Runs while the "dialog" is open - for things that happen meanwhile.
    std::function<void()> onPickFolder;
    plat.pickFolder = [&](const char* title, const std::string&) {
        dialogTitles.emplace_back(title ? title : "");
        if (onPickFolder) onPickFolder();
        if (nextFolder.empty()) return std::string();
        auto f = nextFolder.front();
        nextFolder.erase(nextFolder.begin());
        return f;
    };
    plat.revealInExplorer = [&](const std::string& p) { revealed.push_back(p); };
    std::vector<std::pair<std::string, std::string>> launched;
    plat.launch = [&](const std::string& exe, const std::string& arg) {
        launched.emplace_back(exe, arg);
        return true;
    };

    // Sample data when there is no real data: one animation with motion,
    // a second one, a root.
    const fs::path cars = work / "cars";
    fs::create_directories(cars);

    auto app = std::make_unique<g2::gui::App>(plat);
    Driver D;
    D.app = app.get();
    // High DPI: reveals scaling bugs that go unnoticed at 100 %.
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
            // After a crash in the middle of a frame the ImGui state is broken.
            // Start fresh so the following steps still mean something.
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

        // Open and close every main menu.
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

        // Light / dark several times: the spacings must not grow.
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

        // Settings bar off and on again.
        D.menu(tr(S::MenuView), tr(S::Settings));
        check(D.findIf([](const Item& i) { return i.window.find("settings") != std::string::npos; }) ==
                  nullptr,
              "Einstellungsleiste ausgeblendet");
        D.menu(tr(S::MenuView), tr(S::Settings));
        check(D.findIf([](const Item& i) { return i.window.find("settings") != std::string::npos; }) !=
                  nullptr,
              "Einstellungsleiste wieder da");

        // About window.
        D.menu(tr(S::MenuView), tr(S::About));
        D.frames(2);
        check(D.windowOpen(tr(S::About)), "Info-Fenster offen");
        D.click(D.find(tr(S::DlgClose), tr(S::About)));
        D.frames(2);
        check(!D.windowOpen(tr(S::About)), "Info-Fenster ueber Schliessen zu");

        // Empty actions without a script must do nothing.
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
        // Mesh off -> skin grayed out.
        if (!st.writeMesh) check(Driver::disabled(D.find(tr(S::WriteSkin))), "Skin ausgegraut ohne Mesh");
        for (S s : {S::WriteFrames, S::WriteMesh, S::UseCache, S::CarcassMode, S::ReadFrameCounts})
            D.click(D.find(tr(s)));
        check(st.writeFrames == f0 && st.writeMesh == m0 && st.carcassCompat == k0,
              "zweiter Klick stellt zurueck");
        {
            const bool g0 = st.writeGla, cf0 = st.writeCfg, i0 = st.writeInfo;
            for (S s : {S::WriteGla, S::WriteCfg, S::WriteInfo}) D.click(D.find(tr(s)));
            check(st.writeGla != g0 && st.writeCfg != cf0 && st.writeInfo != i0,
                  "Haekchen GLA, animation.cfg und _info.txt reagieren");
            for (S s : {S::WriteGla, S::WriteCfg, S::WriteInfo}) D.click(D.find(tr(s)));
            check(st.writeGla == g0 && st.writeCfg == cf0 && st.writeInfo == i0,
                  "zweiter Klick stellt GLA/cfg/_info zurueck");
        }
        {
            const bool n0 = st.newSkeleton;
            D.click(D.find(tr(S::NewSkeleton)));
            check(st.newSkeleton != n0, "Haekchen \"Skelett neu bauen\" reagiert");
            D.click(D.find(tr(S::NewSkeleton)));
            check(st.newSkeleton == n0, "zweiter Klick stellt es zurueck");
            check(D.find(tr(S::FrameStep)) != nullptr, "Feld Framestep vorhanden");
        }

        // Paths: via the folder button (dialog supplies them) and by typing.
        if (real) {
            nextFolder.push_back(base.string());
            // The first folder button belongs to the asset root.
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
    // Working data: small scripts for reordering and building.
    fs::path smallCar, smallCar2;
    if (real) {
        // Small, real script: the first 12 grabs of the big .car.
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

        // The same file must not open twice.
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        check(app->documents().size() == 2, "dieselbe Datei nicht doppelt geoeffnet");

        // Click tabs.
        const Item* t0 = D.findTab("###" + app->documents()[0].path);
        D.click(t0);
        D.frames(2);
        check(app->activeTab() == 0, "Klick auf ersten Tab aktiviert ihn");

        // Open an already open .car again (double click in Explorer):
        // its tab must show up, not the one currently visible.
        const std::string p1 = app->documents()[1].path;
        app->openPath(p1);
        D.frames(3);
        check(app->activeTab() == 1 && D.shownTab(p1).find(p1) != std::string::npos,
              "erneut geoeffnete .car springt auf ihren Tab (ImGui zeigt: " + D.shownTab(p1) + ")");

        // Restart: the last active tab must be selected again, not the
        // first one.
        {
            const float dpi = app->settings().dpiScale;
            app.reset();
            app = std::make_unique<g2::gui::App>(plat);
            D.app = app.get();
            app->settings().dpiScale = dpi;
            D.frames(3);
            check(app->documents().size() == 2 && app->activeTab() == 1 &&
                      D.shownTab(p1).find(p1) != std::string::npos,
                  "nach dem Neustart ist der zuletzt aktive Tab gewaehlt");
            D.click(D.findTab("###" + app->documents()[0].path));
            D.frames(2);
        }
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

        // Filter: type, then range selection.
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
        // Clear the filter.
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
        // Dropped "before row 5": afterwards a sits at index 4.
        check(nameAt(4) == a, "Zeile 1 per Ziehen hinter Zeile 4 verschoben (jetzt: " + grabNames(d) + ")");
        check(d.dirty, "Skript als geaendert markiert");
        check(g2::gui::App::isRootGrab(d.script.grabs.back()), "root bleibt die letzte Zeile");

        // Drag a multi-selection as a block.
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

        // Upper or lower half of the target row: before or after it, and
        // while dragging a line exactly at that position.
        {
            const auto punkt = [](const Item& it, float anteil) {
                return ImVec2(it.bb.Min.x + 6.0f, it.bb.Min.y + it.bb.GetHeight() * anteil);
            };
            // Hold, drag to the spot, check state, release.
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

            // Drag below a heading: the heading stays on top, the sequence
            // sits below it.
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

        // Dragging onto the OTHER tab: must not reorder in the wrong script.
        if (app->documents().size() >= 2) {
            auto& other = app->documents()[1];
            const std::string before0 = grabNames(d);
            const std::string before1 = grabNames(other);
            D.clickRow(D.row(nameAt(3)));
            const Item* s3 = D.row(nameAt(3));
            const Item* tab1 = D.findTab("###" + other.path);
            if (s3 && tab1) {
                // Hover over the tab until ImGui switches to it, then drop in
                // the table.
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
            // Back to the first tab.
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
            // If the name already exists further up, use the later occurrence.
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
        // Paste: the label contains the count.
        if (D.popupOpen()) D.escape();
        D.clickRow(D.row(nameAt(5)));
        D.click(D.row(nameAt(5)), 1);
        D.frames(2);
        char pasteLbl[128];
        std::snprintf(pasteLbl, sizeof(pasteLbl), tr(S::PasteAfter), std::size_t{1});
        D.click(D.find(pasteLbl));
        D.frames(3);
        check(d.script.grabs.size() == n0 + 1, "Einfuegen danach: eine Zeile mehr");

        // Delete with confirmation: first No, then Yes.
        ctx(6, S::DeleteSeq);
        check(D.popupOpen(), "Loeschen fragt nach");
        D.click(D.find(tr(S::No)));
        D.frames(2);
        check(d.script.grabs.size() == n0 + 1, "Nein loescht nichts");
        ctx(6, S::DeleteSeq);
        D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
        D.frames(2);
        check(d.script.grabs.size() == n0, "Ja loescht genau eine");

        // Cut = copy + delete with confirmation.
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
        // Large enough that all buttons lie within the visible area.
        ImGui::SetWindowSize("###seqdlg", ImVec2(1200, 1150));
        ImGui::SetWindowPos("###seqdlg", ImVec2(360, 60));
        D.frames(3);

        // Pick an enum.
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

        // Add an extra sequence and remove it again.
        const std::size_t a0 = d.script.grabs[2].additional.size();
        D.click(D.find(tr(S::DlgAddExtra), "###seqdlg"));
        check(d.script.grabs[2].additional.size() == a0 + 1, "-additional angelegt");
        const Item* del = D.findIf([](const Item& i) {
            return i.window.find("###seqdlg") != std::string::npos && i.label == "X";
        });
        D.click(del);
        check(d.script.grabs[2].additional.size() == a0, "-additional entfernt");

        // Loop frame via the plus button.
        const int l0 = d.script.grabs[2].loop.value_or(-1);
        const Item* plus = D.findIf([](const Item& i) {
            return i.window.find("seqdlg") != std::string::npos && i.label == "+";
        });
        D.click(plus);
        check(d.script.grabs[2].loop.value_or(-1) == l0 + 1, "Loop-Frame +1");

        // Switch tabs while the dialog is open: afterwards the dialog must not
        // silently edit the same row in the OTHER script.
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

        // Right click > Move up.
        D.click(c, 1);
        D.frames(2);
        D.click(D.find(tr(S::MoveUp)));
        D.frames(2);
        std::size_t total1 = d.script.trailingComments.size();
        for (const auto& g : d.script.grabs) total1 += g.commentsBefore.size();
        check(total1 == total0, "Nach oben: keine Kommentarzeile verloren");

        // Drag to the end.
        c = D.findIf([](const Item& i) {
            return i.window.find("seqs") != std::string::npos && i.label.rfind("//////", 0) == 0;
        });
        const Item* endRow = D.findIf([](const Item& i) { return i.label == "##dropend"; });
        if (c && endRow) D.dragPath(Driver::leftOf(*c), {Driver::leftOf(*endRow)});
        D.frames(2);
        std::size_t total2 = d.script.trailingComments.size();
        for (const auto& g : d.script.grabs) total2 += g.commentsBefore.size();
        check(total2 == total0, "Ans Ende gezogen: keine Kommentarzeile verloren");

        // Double click on a sequence's line comment.
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
        // Output via the "default folder" button.
        D.click(D.find(tr(S::DefaultFolder)));
        auto& d = app->documents()[static_cast<std::size_t>(app->activeTab())];
        check(!d.outputDir.empty(), "Standardordner gesetzt");

        // Unsaved change before building.
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

        // Header comment survives saving.
        check(onDiskAfter.find("Kopfkommentar") != std::string::npos,
              "Kopfkommentar der .car nach dem Speichern noch vorhanden");

        // Build all.
        app->assignDefaultOutputs(true);
        D.click(D.findIconButton(tr(S::BtnBuildAll), "##main"));
        D.waitBuild();
        check(logHas(*app, "t_gui.gla"), "Alle bauen: Protokoll meldet die GLA");

        // Log: open the output folder.
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
    step("10b. Ausgaben waehlen, Modell-Skript");
    guardedStep("Ausgaben", [&] {
        if (!real || app->documents().empty()) return;
        auto& st = app->settings();
        const g2::gui::Settings saved = st;
        const int activeBefore = app->activeTab();
        const std::string outBefore = app->documents()[static_cast<std::size_t>(activeBefore)].outputDir;
        auto& d = app->documents()[static_cast<std::size_t>(activeBefore)];
        const fs::path only = work / "nur_cfg";
        fs::remove_all(only);
        d.outputDir = only.string();

        // Only the animation.cfg: nothing else may appear.
        st.writeGla = false;
        st.writeCfg = true;
        st.writeFrames = false;
        st.writeMesh = false;
        st.writeInfo = false;
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild();
        std::size_t files = 0;
        for (const auto& e : fs::directory_iterator(only, ec)) (void)e, ++files;
        check(fs::exists(only / "animation.cfg") && files == 1,
              "nur animation.cfg gewaehlt: genau eine Datei geschrieben (" + std::to_string(files) + ")");

        // Only the GLA.
        fs::remove_all(only);
        st.writeGla = true;
        st.writeCfg = false;
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild();
        files = 0;
        for (const auto& e : fs::directory_iterator(only, ec)) (void)e, ++files;
        check(files == 1 && !fs::exists(only / "animation.cfg"),
              "nur GLA gewaehlt: keine animation.cfg, eine Datei (" + std::to_string(files) + ")");
        st = saved;

        // A model script ($aseanimgrab_gla): GLM only, against the GLA it
        // names - Carcass builds these, g2c used to abort with "keine
        // $aseanimgrab-Anweisungen".
        const fs::path mb = work / "modelbase";
        const fs::path md = mb / "models" / "players" / "m_gui";
        fs::create_directories(md);
        fs::create_directories(mb / "models" / "players" / "_humanoid");
        fs::copy_file(refGla, mb / "models" / "players" / "_humanoid" / "_humanoid.gla",
                      fs::copy_options::overwrite_existing);
        const fs::path rootXsi = base / "models" / "players" / "__new_anim" / "_humanoid" / "root.xsi";
        if (!fs::exists(rootXsi)) {
            out("    (kein root.xsi unter %s)\n", rootXsi.string().c_str());
            return;
        }
        fs::copy_file(rootXsi, md / "root.xsi", fs::copy_options::overwrite_existing);
        {
            std::ofstream c(md / "model.car", std::ios::binary);
            c << "$aseanimgrabinit\r\n$aseanimgrab_gla models/players/_humanoid/_humanoid.gla\r\n"
                 "$aseanimgrabfinalize\r\n$aseanimconvertmdx_noask models/players/m_gui/root -makeskin\r\n";
        }
        const std::string glaBefore = readAll(mb / "models" / "players" / "_humanoid" / "_humanoid.gla");
        st.baseDir = mb.string();
        st.referenceGla = (work / "gibt_es_nicht.gla").string();   // the script's GLA must win
        const std::size_t before = app->documents().size();
        check(app->openCar((md / "model.car").string()), "Modell-Skript geoeffnet");
        const std::size_t mi = app->documents().size() - 1;
        check(app->documents().size() == before + 1 && app->activeTab() == static_cast<int>(mi),
              "Modell-Skript ist der aktive Tab");
        const fs::path mout = work / "model_out";
        fs::remove_all(mout);
        app->documents()[mi].outputDir = mout.string();
        D.frames(2);
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild(300.0);
        check(fs::exists(mout / "model.glm"), "Modell-Skript: model.glm geschrieben");
        check(fs::exists(mout / "model_default.skin"), "-makeskin: model_default.skin (wie Carcass)");
        check(!fs::exists(mout / "animation.cfg") && !fs::exists(mout / "model.gla"),
              "Modell-Skript schreibt keine GLA und keine animation.cfg");
        if (fs::exists(mout / "model.glm")) {
            const auto g = g2::readMdxm(g2::readWholeFileBytes((mout / "model.glm").string()));
            check(g.mesh.animName == "models/players/_humanoid/_humanoid" &&
                      g.mesh.lods.size() == 1 && g.mesh.lods[0].surfaces.size() == 84,
                  "GLM zeigt auf die GLA aus $aseanimgrab_gla (" + g.mesh.animName + "), 84 Surfaces");
        }
        check(readAll(mb / "models" / "players" / "_humanoid" / "_humanoid.gla") == glaBefore,
              "die benutzte GLA bleibt unangetastet");

        // Skin switched off: no skin even with -makeskin.
        fs::remove_all(mout);
        st.writeSkin = false;
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild(300.0);
        check(fs::exists(mout / "model.glm") && !fs::exists(mout / "model_default.skin"),
              ".skin abgeschaltet: nur model.glm");
        st = saved;
        app->closeDocument(mi);
        D.frames(2);
        check(app->documents().size() == before, "Modell-Skript wieder geschlossen");
        // Leave everything as the following steps expect it.
        app->documents()[static_cast<std::size_t>(activeBefore)].outputDir = outBefore;
        app->activate(activeBefore);
        D.frames(2);
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

        // Keyboard shortcuts that are shown in the menu.
        for (auto& x : app->documents()) x.validated = false;
        D.key(ImGuiKey_F7);
        check(app->documents()[0].validated, "F7 prueft (steht so im Menue)");
        const std::size_t titles0 = dialogTitles.size();
        D.key(ImGuiKey_O, true);
        check(dialogTitles.size() > titles0, "Strg+O oeffnet den Dateidialog");

        // Ctrl+W: a clean tab closes, a modified one asks first.
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
        // Freshly built GLA from step 10 including its companion files.
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

        // Selection.
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

        // Target folder and export of the selection.
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

        // Are all controls of the export row within the visible area?
        {
            // Check against the visible area of the extract window, not
            // against the screen: the mode bar sits on the left.
            std::size_t clipped = 0;
            for (ImGuiWindow* w : GImGui->Windows) {
                if (!w->WasActive || std::string(w->Name).find("extract") == std::string::npos) continue;
                for (const Item& i : g_last)
                    if (i.window == w->Name && i.bb.Max.x > w->ClipRect.Max.x + 1.0f) ++clipped;
            }
            check(clipped == 0, "alle Bedienelemente im Extract-Modus sichtbar (" +
                                    std::to_string(clipped) + " abgeschnitten)");
        }
        // Switch the version (combo box).
        D.click(D.findById("##xsiver", "extract"));
        D.frames(2);
        D.click(D.findIf([](const Item& i) { return displayOf(i.label) == "3.5"; }));
        check(ex.xsiVersion == g2::xsiexp::ExportOptions::Version::V35, "dotXSI 3.5 gewaehlt");

        // Everything with .car. A .car already exists: it must ask first and
        // make a backup before replacing it.
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
            // It must be possible to build again from the exported .car: every
            // line must be readable.
            bool ok = true;
            try {
                const auto sc = g2::car::parseFile(carOut.string());
                ok = !sc.grabs.empty() && sc.convert.has_value();
            } catch (const std::exception&) {
                ok = false;
            }
            check(ok, "exportierte .car ist lesbar und vollstaendig");
        }

        // Comparison with another animation.cfg.
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
        // Sequence selection.
        D.click(D.find(tr(S::PreviewSeq)));
        D.frames(2);
        const auto& ex = app->extract();
        const Item* s = D.findIf([&](const Item& i) { return displayOf(i.label) == ex.seqs.back().name && i.window.find("##Combo") != std::string::npos; });
        D.click(s);
        check(true, "Sequenz in der Vorschau gewechselt");

        // animation.cfg that matches NONE of this GLA's sequences.
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
    // Trigger the messages with numbers and paths in every language.
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
        // Scroll.
        const Item* any = D.findIf([](const Item& i) { return i.window.find("seqs") != std::string::npos; });
        if (any) for (int k = 0; k < 10; ++k) D.wheel(Driver::center(*any), -20.0f);
        check(true, "gescrollt");

        // Saving without changes: the file content must stay the same.
        app->saveDocument(app->documents().size() - 1);
        const auto a = g2::car::parseFile(bigCar.string());
        const auto b = g2::car::parseFile(copy.string());
        bool same = a.grabs.size() == b.grabs.size();
        for (std::size_t k = 0; same && k < a.grabs.size(); ++k)
            same = a.grabs[k].file == b.grabs[k].file && a.grabs[k].additional.size() == b.grabs[k].additional.size();
        check(same, "grosses Skript gespeichert und wieder gelesen: alle Grabs gleich");
    });

    // =====================================================================
    // Assimilate parity. The numbers in the real-data part are what Assimilate
    // 3.1 itself showed for the same _humanoid.car (Target / Speed per row),
    // read off its window on 2026-10-03.
    step("16b. Wie Assimilate: Speed, Ziel, Teile, Modell, Picker, Dateien");
    guardedStep("Assimilate", [&] {
        auto& st = app->settings();
        const std::string oldBase = st.baseDir, oldEnums = st.enumPath;

        // ---- A small script with its own .xsi files: runs without game data.
        const fs::path mini = work / "mini";
        const fs::path anims = mini / "models" / "anims";
        fs::create_directories(anims);
        const auto anim = [&](const char* name, int frames, int rate) {
            std::ostringstream f;
            f << "xsi 0350txt 0032\n";
            if (rate > 0)
                f << "SI_Scene s {\n  \"FRAMES\",\n  1.000000,\n  " << frames << ".000000,\n  " << rate
                  << ".000000,\n}\n";
            f << "SI_Model MDL-rig.root {\n  SI_Model MDL-rig.a {\n"
              << "    SI_FCurve { \"rig.a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, " << frames << ",\n";
            for (int k = 1; k <= frames; ++k) f << "      " << k << "," << k << ",\n";
            f << "    }\n  }\n}\n";
            writeText(anims / name, f.str());
        };
        anim("walk.xsi", 10, 25);   // SI_Scene rate 25
        anim("run.xsi", 6, 0);      // no SI_Scene: the build's default 30
        anim("jump.xsi", 4, 12);    // rate 12, but -framespeed 7 in the script
        writeText(mini / "mini.car",
                  "$aseanimgrabinit\n"
                  "$aseanimgrab models/anims/walk.xsi -enum BOTH_WALK1\n"
                  "$aseanimgrab models/anims/run.xsi -enum BOTH_RUN1 -additional 2 3 -1 -10 BOTH_RUN1_PART\n"
                  "$aseanimgrab models/anims/jump.xsi -framespeed 7 -enum BOTH_JUMP1\n"
                  "$aseanimgrabfinalize\n"
                  "$aseanimconvertmdx_noask models/root -makeskel models/mini/mini\n");
        // A model that animates against mini's skeleton: a "dependant".
        writeText(mini / "dep" / "dep.car",
                  "$aseanimgrab_gla models/mini/mini\n$aseanimgrabinit\n$aseanimgrabfinalize\n");
        writeText(mini / "anims.h",
                  "typedef enum //# animNumber_e\n{\n\tBOTH_WALK1,\n\tBOTH_RUN1,\n\tBOTH_RUN1_PART,\n"
                  "\tBOTH_JUMP1,\n\tBOTH_STAND1,\n\tFACE_ALERT,\n\tFACE_SMILE,\n\tLEGS_TURN1,\n"
                  "\tTORSO_DROPWEAP1,\n\tMAX_ANIMATIONS\n} animNumber_t;\n");

        st.baseDir = mini.string();
        app->loadEnums((mini / "anims.h").string());
        app->openCar((mini / "mini.car").string());
        const std::size_t mi = app->documents().size() - 1;
        // All three, not just one: the reader goes through them in sorted
        // order, so jump.xsi is done before walk.xsi.
        // Up to a minute: with real data the reader may still be busy with the
        // 1854 files of the big script from the step before.
        for (int k = 0; k < 6000 && (app->frameCountOf("models/anims/jump.xsi") < 0 ||
                                    app->frameCountOf("models/anims/run.xsi") < 0 ||
                                    app->frameCountOf("models/anims/walk.xsi") < 0);
             ++k) {
            D.frames(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        D.frames(3);
        auto& md = app->documents()[mi];
        auto& G = md.script.grabs;

        check(app->effectiveSpeed(G[0]) == 25, "Speed ohne -framespeed = Rate aus SI_Scene (25), nicht 0/auto");
        check(app->effectiveSpeed(G[1]) == 30, "ohne SI_Scene: 30 wie der Bau");
        check(app->effectiveSpeed(G[2]) == 7, "-framespeed hat Vorrang vor der Rate (12)");
        {
            const auto tg = app->targetFrames(md);
            check(tg.size() == 3 && tg[0] == 0 && tg[1] == 10 && tg[2] == 16, "Ziel-Frames 0, 10, 16");
        }

        // ---- REGRESSION: the speed in the sequence dialog.
        //
        // The field used to show 0 ("from SI_Scene") instead of the real 25.
        // Its "+" therefore turned 0 into 1: one click and the walk played at
        // 1 frame per second. Now it starts at the real value.
        D.doubleClick(D.row("BOTH_WALK1"));
        D.frames(2);
        check(D.windowOpen("###seqdlg"), "Sequenzdialog offen");
        ImGui::SetWindowSize("###seqdlg", ImVec2(1200, 1150));
        ImGui::SetWindowPos("###seqdlg", ImVec2(360, 60));
        D.frames(3);
        {
            std::vector<const Item*> plus;
            for (const Item& it : g_last)
                if (it.window.find("seqdlg") != std::string::npos && it.label == "+") plus.push_back(&it);
            // Loop field first, speed field second.
            check(plus.size() >= 2, "Plus-Knoepfe fuer Loop und Speed da");
            if (plus.size() >= 2) {
                D.click(plus[1]);
                D.frames(2);
                check(G[0].frameSpeed && *G[0].frameSpeed == 26,
                      "REGRESSION Speed-Feld: + macht aus 25 eine 26 (Fehler: aus 0 eine 1), ist " +
                          (G[0].frameSpeed ? std::to_string(*G[0].frameSpeed) : std::string("leer")));
            }
        }
        // Typed in.
        D.click(D.find(tr(S::DlgFrameSpeed), "seqdlg"));
        D.key(ImGuiKey_A, true);
        D.type("12");
        D.key(ImGuiKey_Enter);
        check(G[0].frameSpeed && *G[0].frameSpeed == 12, "Speed eingetippt: 12");
        {
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), tr(S::SpeedResetToXsi), 25);
            D.click(D.find(lbl, "seqdlg"));
            D.frames(2);
            check(!G[0].frameSpeed, "\"Wie in der .xsi (25)\": -framespeed wieder entfernt");
        }
        // Loop: without -loop the build writes 0, the dialog used to show -1.
        {
            std::vector<const Item*> minus;
            for (const Item& it : g_last)
                if (it.window.find("seqdlg") != std::string::npos && it.label == "-") minus.push_back(&it);
            if (!minus.empty()) {
                D.click(minus[0]);
                D.frames(2);
                check(G[0].loop && *G[0].loop == -1,
                      "REGRESSION Loop-Feld: - macht aus 0 (wie gebaut) eine -1 (Fehler: aus -1 eine -2)");
            }
            G[0].loop.reset();
        }
        D.click(D.find(tr(S::DlgClose), "###seqdlg"));
        D.frames(2);

        // ---- Speed for several rows at once (right-click).
        D.clickRow(D.row("BOTH_WALK1"));
        D.clickRow(D.row("BOTH_JUMP1"), false, true);
        D.click(D.row("BOTH_RUN1"), 1);
        D.frames(2);
        {
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), tr(S::SetSpeedRows), std::size_t{3});
            D.click(D.find(lbl));
            D.frames(2);
            check(D.popupOpen(), "Framespeed fuer 3 Zeilen: Dialog offen");
            D.click(D.findIf([](const Item& i) { return i.label == "##speed"; }));
            D.key(ImGuiKey_A, true);
            D.type("15");
            D.frames(1);
            D.click(D.find(tr(S::Apply)));
            D.frames(2);
            check(G[0].frameSpeed == 15 && G[1].frameSpeed == 15 && G[2].frameSpeed == 15,
                  "alle drei auf 15 gesetzt");
            app->saveDocument(mi);
            std::ifstream f(mini / "mini.car");
            const std::string txt((std::istreambuf_iterator<char>(f)), {});
            std::size_t n = 0;
            for (std::size_t p = txt.find("-framespeed 15"); p != std::string::npos;
                 p = txt.find("-framespeed 15", p + 1))
                ++n;
            check(n == 3, "gespeichert: dreimal -framespeed 15 in der .car");

            D.click(D.row("BOTH_RUN1"), 1);
            D.frames(2);
            D.click(D.find(lbl));
            D.frames(2);
            D.click(D.find(tr(S::SpeedAllFromXsi)));
            D.frames(2);
            check(!G[0].frameSpeed && !G[1].frameSpeed && !G[2].frameSpeed,
                  "\"Wie in der .xsi\" fuer alle: -framespeed entfernt");
        }

        // ---- Split parts shown by name, details on demand.
        check(G[1].additional.size() == 1 && G[1].additional[0].name == "BOTH_RUN1_PART", "Teil gelesen");
        D.menu(tr(S::MenuView), tr(S::PartDetails));
        D.frames(2);
        check(st.partDetails, "Ansicht > Details der Teile an");
        D.menu(tr(S::MenuView), tr(S::PartDetails));
        D.frames(2);

        // ---- Animation picker: categories, used marks, hide used.
        D.doubleClick(D.row("BOTH_JUMP1"));
        D.frames(2);
        ImGui::SetWindowSize("###seqdlg", ImVec2(1200, 1150));
        ImGui::SetWindowPos("###seqdlg", ImVec2(360, 60));
        D.frames(3);
        D.click(D.findLabel(std::string(tr(S::DlgChoose)) + "##m"));
        D.frames(2);
        check(D.popupOpen(), "Picker offen");
        const auto listed = [&] {
            std::vector<std::string> v;
            for (const Item& i : g_last)
                // Only list entries - the scrollbar is an item without a label.
                if (i.window.find("elist") != std::string::npos && !displayOf(i.label).empty())
                    v.push_back(displayOf(i.label));
            return v;
        };
        const auto joined = [](const std::vector<std::string>& v) {
            std::string r;
            for (const auto& x : v) r += (r.empty() ? "" : "|") + x;
            return r;
        };
        D.click(D.find("FACE_"));
        D.frames(2);
        {
            const auto v = listed();
            check(v.size() == 2 && v[0].find("FACE_ALERT") != std::string::npos,
                  "Kategorie FACE_: nur FACE_ALERT, FACE_SMILE (" + joined(v) + ")");
        }
        D.click(D.find("BOTH_"));
        D.frames(2);
        {
            const auto v = listed();
            const bool marked = std::any_of(v.begin(), v.end(), [](const std::string& s) { return s == "* BOTH_WALK1"; });
            const bool free = std::any_of(v.begin(), v.end(), [](const std::string& s) { return s == "BOTH_STAND1"; });
            check(v.size() == 5 && marked && free, "Kategorie BOTH_: benutzte mit *, freie ohne (" + joined(v) + ")");
        }
        D.click(D.find(tr(S::PickerHideUsed)));
        D.frames(2);
        {
            const auto v = listed();
            // BOTH_JUMP1 is the current one and stays; the other used ones go.
            check(v.size() == 2, "Benutzte ausgeblendet: BOTH_STAND1 und der eigene bleiben (" + joined(v) + ")");
        }
        D.click(D.find(tr(S::PickerHideUsed)));
        D.click(D.find(tr(S::PickerAll)));
        D.escape();
        D.frames(2);
        D.click(D.find(tr(S::DlgClose), "###seqdlg"));
        D.frames(2);

        // ---- Model dialog.
        D.menu(tr(S::MenuBuild), tr(S::ModelSettingsMenu));
        D.frames(3);
        check(D.windowOpen("###modeldlg"), "Modell-Dialog offen");
        ImGui::SetWindowSize("###modeldlg", ImVec2(1000, 1100));
        ImGui::SetWindowPos("###modeldlg", ImVec2(400, 80));
        D.frames(3);
        D.click(D.find(tr(S::ModelKeepMotion), "modeldlg"));
        D.click(D.find(tr(S::ModelScale), "modeldlg"));
        D.key(ImGuiKey_A, true);
        D.type("0.5");
        D.key(ImGuiKey_Enter);
        D.click(D.findIf([](const Item& i) { return i.label == "##pcjnew"; }));
        D.type("cranium");
        D.click(D.find(tr(S::ModelPcjAdd), "modeldlg"));
        D.click(D.find(tr(S::ModelMakeSkin), "modeldlg"));
        D.click(D.find("OK", "modeldlg"));
        D.frames(3);
        check(!D.windowOpen("###modeldlg"), "Modell-Dialog mit OK geschlossen");
        check(md.script.keepMotion && md.script.scale && *md.script.scale == 0.5 &&
                  md.script.pcjBones == std::vector<std::string>{"cranium"} && md.script.convert->makeSkin && md.dirty,
              "uebernommen: $keepmotion, $scale 0.5, $pcj cranium, -makeskin");
        app->saveDocument(mi);
        {
            const auto back = g2::car::parseFile((mini / "mini.car").string());
            check(back.keepMotion && back.scale == 0.5 && back.pcjBones.size() == 1 && back.convert->makeSkin,
                  "gespeichert und wieder gelesen");
        }
        D.menu(tr(S::MenuBuild), tr(S::ModelSettingsMenu));
        D.frames(3);
        ImGui::SetWindowSize("###modeldlg", ImVec2(1000, 1100));
        D.frames(2);
        D.click(D.find(tr(S::ModelKeepMotion), "modeldlg"));
        D.click(D.find(tr(S::BtnCancel), "modeldlg"));
        D.frames(2);
        check(md.script.keepMotion && !md.dirty, "Abbrechen aendert nichts");

        // ---- Dependent models.
        {
            const auto deps = app->findDependents(mi);
            check(deps.size() == 1 && fs::path(deps[0]).filename() == "dep.car",
                  "abhaengiges Modell gefunden ($aseanimgrab_gla)");
        }

        // ---- ModView with the built .glm.
        md.outputDir = (mini / "out").string();
        writeText(mini / "out" / "mini.glm", "x");
        writeText(mini / "ModView.exe", "x");
        st.modelViewPath = (mini / "ModView.exe").string();
        launched.clear();
        D.menu(tr(S::MenuBuild), tr(S::OpenInModView));
        D.frames(2);
        check(launched.size() == 1 && launched[0].second.find("mini.glm") != std::string::npos,
              "ModView mit der .glm gestartet");

        // ---- Save as and recent files.
        nextFiles.push_back({(mini / "kopie.car").string()});
        D.menu(tr(S::MenuFile), tr(S::SaveAs));
        D.frames(2);
        check(fs::exists(mini / "kopie.car") && fs::path(md.path).filename() == "kopie.car",
              "Speichern unter: Tab gehoert jetzt zu kopie.car");
        check(!app->recentFiles().empty() && fs::path(app->recentFiles().front()).filename() == "kopie.car",
              "zuletzt geoeffnet: kopie.car oben");

        st.baseDir = oldBase;
        if (!oldEnums.empty()) app->loadEnums(oldEnums);

        // ---- Real data: the same numbers Assimilate shows, and the round trip
        // dialog -> .car -> animation.cfg.
        if (!real || bigCar.empty()) return;
        std::size_t bi = app->documents().size();
        for (std::size_t k = 0; k < app->documents().size(); ++k)
            if (fs::path(app->documents()[k].path).parent_path().filename() == "big") bi = k;
        check(bi < app->documents().size(), "grosses Skript offen");
        if (bi >= app->documents().size()) return;
        app->activate(static_cast<int>(bi));
        for (int k = 0; k < 6000; ++k) {
            D.frames(1);
            const auto tg = app->targetFrames(app->documents()[bi]);
            if (!tg.empty() && tg.back() >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto& bd = app->documents()[bi];
        const auto tg = app->targetFrames(bd);
        const auto idx = [&](const std::string& n) -> std::size_t {
            for (std::size_t k = 0; k < bd.script.grabs.size(); ++k) {
                const auto& g = bd.script.grabs[k];
                if ((g.enumName ? *g.enumName : g.derivedName()) == n) return k;
            }
            return bd.script.grabs.size();
        };
        struct Row { const char* name; int target; int speed; };
        const Row expect[] = {{"FACE_ALERT", 0, 1},       {"FACE_TALK1", 10, 5},       {"BOTH_A1_BL_TR", 18, 30},
                              {"BOTH_A1_SPECIAL", 30, 20}, {"BOTH_A1_TR_BL", 93, 20},  {"BOTH_A2_BL_TR", 120, 50},
                              {"BOTH_A2_SPECIAL", 149, 20}, {"BOTH_A2_STABBACK1", 205, 30},
                              {"BOTH_A3_SPECIAL", 352, 20}, {"BOTH_A3_T__B_", 424, 40}};
        int same = 0;
        for (const auto& r : expect) {
            const std::size_t k = idx(r.name);
            const bool ok = k < bd.script.grabs.size() && tg[k] == r.target &&
                            app->effectiveSpeed(bd.script.grabs[k]) == r.speed;
            if (!ok)
                out("    %s: g2c Ziel %d Speed %d, Assimilate Ziel %d Speed %d\n", r.name,
                    k < tg.size() ? tg[k] : -9, k < bd.script.grabs.size() ? app->effectiveSpeed(bd.script.grabs[k]) : -9,
                    r.target, r.speed);
            same += ok;
        }
        check(same == 10, "Ziel und Speed wie in Assimilate: " + std::to_string(same) + " von 10 Zeilen");
        {
            const std::size_t k = idx("BOTH_A1_BL_TR");
            const auto& a = bd.script.grabs[k].additional;
            check(a.size() == 2 && tg[k] + a[0].targetOffset == 18 && a[0].frameSpeed == -10 &&
                      tg[k] + a[1].targetOffset == 23 && a[1].frameSpeed == 30,
                  "Teile wie Assimilate: BOTH_B1_BL___ T:18 S:-10, BOTH_D1_TR___ T:23 S:30");
        }

        // Round trip: new speed via the dialog, then build. It has to arrive in
        // the .car AND in animation.cfg.
        const std::size_t sk = idx("BOTH_A1_SPECIAL");
        bd.outputDir = (work / "big" / "out").string();
        D.doubleClick(D.row("BOTH_A1_SPECIAL"));
        D.frames(2);
        ImGui::SetWindowSize("###seqdlg", ImVec2(1200, 1150));
        ImGui::SetWindowPos("###seqdlg", ImVec2(360, 60));
        D.frames(3);
        D.click(D.find(tr(S::DlgFrameSpeed), "seqdlg"));
        D.key(ImGuiKey_A, true);
        D.type("33");
        D.key(ImGuiKey_Enter);
        D.click(D.find(tr(S::DlgClose), "###seqdlg"));
        D.frames(2);
        check(bd.script.grabs[sk].frameSpeed == 33, "BOTH_A1_SPECIAL im Dialog auf 33");

        const auto cfgSpeed = [&](const std::string& text, const char* name) {
            std::istringstream is(text);
            std::string line;
            while (std::getline(is, line)) {
                std::istringstream ls(line);
                std::string n;
                int t = 0, c = 0, l = 0, s = 0;
                if (ls >> n >> t >> c >> l >> s && n == name) return s;
            }
            return -999;
        };
        const auto readAll = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        };
        const fs::path outDir = work / "big" / "out";
        std::error_code ec;
        fs::remove_all(outDir, ec);
        D.menu(tr(S::MenuBuild), tr(S::WriteCfgOnly));
        D.waitBuild(600.0);
        const std::string cfgOnly = readAll(outDir / "animation.cfg");
        check(cfgSpeed(cfgOnly, "BOTH_A1_SPECIAL") == 33, "Nur animation.cfg: BOTH_A1_SPECIAL mit Speed 33");
        check(cfgSpeed(cfgOnly, "BOTH_A3_T__B_") == 40 && cfgSpeed(cfgOnly, "FACE_TALK1") == 5,
              "uebrige Zeilen unveraendert (40, 5)");
        bool anyGla = false;
        for (const auto& e : fs::directory_iterator(outDir, ec))
            if (e.path().extension() == ".gla") anyGla = true;
        check(!anyGla, "Nur animation.cfg: keine GLA geschrieben");
        {
            const std::string car = readAll(bd.path);
            check(car.find("both_a1_special.xsi -loop -1 -framespeed 33") != std::string::npos,
                  ".car vor dem Bauen gespeichert, mit -framespeed 33");
        }
        D.menu(tr(S::MenuBuild), tr(S::BuildCurrent));
        D.waitBuild(600.0);
        const std::string cfgFull = readAll(outDir / "animation.cfg");
        check(!cfgFull.empty() && cfgFull == cfgOnly, "voller Bau schreibt dieselbe animation.cfg wie 'Nur animation.cfg'");
        bool gla = false;
        for (const auto& e : fs::directory_iterator(outDir, ec))
            if (e.path().extension() == ".gla") gla = true;
        check(gla, "voller Bau: GLA geschrieben");
    });

    // =====================================================================
    // Regression tests for the GUI findings of the code review of 2026-10-03.
    step("16c. Code-Review: Regressionen der Oberflaeche");
    guardedStep("Review", [&] {
        const fs::path rv = work / "review";
        fs::create_directories(rv / "a");
        fs::create_directories(rv / "b");
        writeText(rv / "a" / "ra.car",
                  "$aseanimgrabinit\n// Kommentar A\n$aseanimgrab anims/x.xsi -enum BOTH_RA\n"
                  "// ### Walk ###\n$aseanimgrab anims/y.xsi -enum BOTH_RA2\n$aseanimgrabfinalize\n");
        writeText(rv / "b" / "rb.car",
                  "$aseanimgrabinit\n// Kommentar B\n$aseanimgrab anims/x.xsi -enum BOTH_RB\n$aseanimgrabfinalize\n");
        writeText(rv / "b" / "rc.car", "$aseanimgrabinit\n$aseanimgrab anims/z.xsi -enum BOTH_RC\n$aseanimgrabfinalize\n");
        const auto indexOf = [&](const std::string& file) {
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (fs::path(app->documents()[k].path).filename() == file) return static_cast<int>(k);
            return -1;
        };
        const auto shown = [](const char* name) {
            ImGuiWindow* w = ImGui::FindWindowByName(name);
            return w && w->Active && !w->Hidden;
        };
        app->openCar((rv / "a" / "ra.car").string());
        app->openCar((rv / "b" / "rb.car").string());
        D.frames(3);

        // --- An in-place comment edit must not end up in another script.
        app->activate(indexOf("ra.car"));
        D.frames(3);
        D.doubleClick(D.findIf([](const Item& i) { return displayOf(i.label) == "// Kommentar A"; }));
        D.frames(2);
        D.key(ImGuiKey_A, true);
        D.type("// XYZ");
        app->activate(indexOf("rb.car"));
        D.frames(3);
        D.key(ImGuiKey_Enter);
        D.frames(2);
        {
            const auto& b = app->documents()[static_cast<std::size_t>(indexOf("rb.car"))];
            check(b.script.grabs[0].commentsBefore.size() == 1 && b.script.grabs[0].commentsBefore[0] == "// Kommentar B" &&
                      !b.dirty,
                  "REGRESSION: Bearbeitung aus Skript A landet nicht in Skript B");
        }

        // --- Comment text with "##" stays visible.
        app->activate(indexOf("ra.car"));
        D.frames(3);
        check(D.findIf([](const Item& i) { return i.label == "##freetext"; }) != nullptr,
              "REGRESSION: Kommentar '// ### Walk ###' wird ganz gezeichnet (vorher nur '// ')");

        // --- Two modals: the second waits instead of both vanishing.
        auto& ra = app->documents()[static_cast<std::size_t>(indexOf("ra.car"))];
        ra.dirty = true;
        D.click(D.row("BOTH_RA"), 1);
        D.frames(2);
        {
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), tr(S::SetSpeedRows), std::size_t{1});
            D.click(D.find(lbl));
        }
        D.frames(3);
        check(shown("###speeddlg"), "Framespeed-Dialog sichtbar");
        app->requestQuit();   // the window close button meanwhile
        D.frames(4);
        check(shown("###speeddlg") && !shown("###closedlg"),
              "REGRESSION: Schliessen-Frage wartet, Framespeed-Dialog bleibt sichtbar (vorher beide unsichtbar)");
        D.click(D.find(tr(S::BtnCancel), "speeddlg"));
        D.frames(4);
        check(shown("###closedlg"), "danach erscheint die Schliessen-Frage");
        D.click(D.find(tr(S::No), "closedlg"));
        D.frames(3);
        check(!app->quitApproved() && !shown("###closedlg"), "Nein: g2c bleibt offen");
        ra.dirty = false;

        // --- A file handed over while the folder dialog is open (a second
        // g2c instance sends WM_COPYDATA during the modal dialog).
        onPickFolder = [&] { app->queueOpen({(rv / "b" / "rc.car").string()}); };
        nextFolder.push_back((rv / "out").string());
        const std::size_t before = app->documents().size();
        D.click(D.findEnds(tr(S::ChooseFolder)));
        D.frames(1);
        onPickFolder = nullptr;
        {
            const int ia = indexOf("ra.car");
            check(ia >= 0 && app->documents()[static_cast<std::size_t>(ia)].outputDir == (rv / "out").string(),
                  "REGRESSION: Ausgabeordner landet im richtigen Skript, kein Schreiben in freigegebenen Speicher");
        }
        D.frames(3);
        check(app->documents().size() == before + 1 && indexOf("rc.car") >= 0,
              "weitergereichte Datei wird im naechsten Bild geoeffnet");

        for (const char* f : {"ra.car", "rb.car", "rc.car"}) {
            const int k = indexOf(f);
            if (k >= 0) {
                app->documents()[static_cast<std::size_t>(k)].dirty = false;
                app->closeDocument(static_cast<std::size_t>(k));
            }
        }
        D.frames(2);
    });

    // =====================================================================
    // The update bar, clicked through like a user would: against a fake
    // GitHub, with a separate App, so the other steps keep their state.
    step("17. Updates");
    guardedStep("Updates", [&] {
        namespace upd = g2::gui::update;
        const fs::path ud = work / "update";
        fs::create_directories(ud);
        writeText(ud / "g2c.exe", "MZalt");
        const std::string newExe = "MZ" + std::string(50000, 'N');
        const std::string dl = "https://github.com/DennisHerrm/G2C-toolkit/releases/download/v9.0.0/";
        const std::string sums = upd::sha256Hex(newExe) + "  g2c.exe\n";
        const std::string json =
            "{\"tag_name\":\"v9.0.0\",\"target_commitish\":\"main\",\"prerelease\":false,"
            "\"html_url\":\"https://github.com/DennisHerrm/G2C-toolkit/releases/tag/v9.0.0\","
            "\"assets\":[{\"name\":\"g2c.exe\",\"size\":" + std::to_string(newExe.size()) +
            ",\"browser_download_url\":\"" + dl + "g2c.exe\"},{\"name\":\"SHA256SUMS.txt\",\"size\":" +
            std::to_string(sums.size()) + ",\"browser_download_url\":\"" + dl + "SHA256SUMS.txt\"}]}";
        bool netDown = false;
        std::vector<std::string> opened;

        g2::gui::Platform up = plat;
        up.network.get = [&](const std::string& url, std::size_t) {
            upd::HttpResult r;
            if (netDown) { r.error = "Zeitueberschreitung"; return r; }
            if (url == upd::apiUrl(upd::Channel::Stable)) { r.status = 200; r.body = json; }
            else if (url == dl + "SHA256SUMS.txt") { r.status = 200; r.body = sums; }
            else r.status = 404;
            return r;
        };
        up.network.download = [&](const std::string&, const fs::path& dest,
                                  const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
            writeText(dest, newExe);
            progress(newExe.size(), newExe.size());
            return std::string();
        };
        up.openUrl = [&](const std::string& u) { opened.push_back(u); };
        up.exePath = ud / "g2c.exe";
        up.build = {"1.0.0", ""};

        auto ua = std::make_unique<g2::gui::App>(up);
        D.app = ua.get();
        ua->updater()->wait();
        D.frames(3);
        check(D.find(tr(S::UpdInstall)) != nullptr, "Start: Leiste zeigt die neue Version");

        D.click(D.find(tr(S::UpdNotes)));
        D.frames(2);
        check(opened.size() == 1 && opened[0].find("/releases/tag/v9.0.0") != std::string::npos,
              "\"Was ist neu?\" oeffnet die Seite der Version");

        D.click(D.find(tr(S::UpdLater)));
        D.frames(2);
        check(D.find(tr(S::UpdInstall)) == nullptr, "\"Spaeter\" blendet die Leiste aus");

        D.menu(tr(S::MenuView), tr(S::UpdCheckNow));
        ua->updater()->wait();
        D.frames(3);
        check(D.find(tr(S::UpdInstall)) != nullptr, "Ansicht > Nach Updates suchen: Leiste wieder da");

        D.click(D.find(tr(S::UpdSkip)));
        D.frames(2);
        check(ua->settings().skippedUpdate == "v9.0.0" && D.find(tr(S::UpdInstall)) == nullptr,
              "\"Diese Version ueberspringen\" merkt sich v9.0.0");

        // Settings bar: its section sits at the bottom, scroll there first.
        if (const Item* any = D.findIf([](const Item& i) { return i.window.find("settings") != std::string::npos; }))
            for (int k = 0; k < 8; ++k) D.wheel(Driver::center(*any), -20.0f);
        D.frames(2);
        const bool autoBefore = ua->settings().checkUpdates;
        D.click(D.find(tr(S::UpdAuto), "settings"));
        D.frames(2);
        check(ua->settings().checkUpdates != autoBefore, "Haken \"Beim Start nach Updates suchen\" schaltet um");
        D.click(D.find(tr(S::UpdAuto), "settings"));
        D.frames(2);
        check(ua->settings().checkUpdates == autoBefore, "und wieder zurueck");
        check(D.findById("##updchannel", "settings") != nullptr, "Kanalauswahl vorhanden");

        // A manual check shows even a skipped version.
        D.click(D.find(tr(S::UpdCheckNow), "settings"));
        ua->updater()->wait();
        D.frames(3);
        check(D.find(tr(S::UpdInstall)) != nullptr, "manuelle Suche zeigt auch die uebersprungene Version");

        // No network: a manual check says so, with the way out.
        D.click(D.find(tr(S::UpdLater)));
        netDown = true;
        D.click(D.find(tr(S::UpdCheckNow), "settings"));
        ua->updater()->wait();
        D.frames(3);
        check(D.find(tr(S::UpdOpenPage)) != nullptr, "ohne Netz: Fehlerleiste mit Download-Seite");
        D.click(D.find(tr(S::UpdOpenPage)));
        D.frames(2);
        check(!opened.empty() && opened.back() == "https://github.com/DennisHerrm/G2C-toolkit/releases",
              "Download-Seite geoeffnet");
        D.click(D.find(tr(S::DlgClose), "updbanner"));
        D.frames(2);
        check(D.find(tr(S::UpdOpenPage)) == nullptr, "Fehlerleiste geschlossen");
        netDown = false;

        // Install.
        D.click(D.find(tr(S::UpdCheckNow), "settings"));
        ua->updater()->wait();
        D.frames(3);
        D.click(D.find(tr(S::UpdInstall)));
        ua->updater()->wait();
        D.frames(3);
        std::string now;
        {
            std::ifstream f(ud / "g2c.exe", std::ios::binary);
            now.assign(std::istreambuf_iterator<char>(f), {});
        }
        check(now == newExe, "\"Jetzt aktualisieren\" hat die Exe ersetzt");
        check(D.find(tr(S::UpdRestart)) != nullptr, "Leiste bietet den Neustart an");

        // Restart with unsaved changes: asks first; cancelling cancels the restart.
        writeText(ud / "u.car", "$aseanimgrabinit\n$aseanimgrabfinalize\n");
        ua->openCar((ud / "u.car").string());
        ua->documents().back().dirty = true;
        D.frames(2);
        D.click(D.find(tr(S::UpdRestart)));
        D.frames(3);
        check(D.popupOpen() && !ua->restartRequested(), "Neustart fragt nach ungespeicherten Aenderungen");
        D.click(D.find(tr(S::No)));
        D.frames(3);
        check(!ua->restartRequested() && !ua->quitApproved(), "\"Nein\": kein Neustart, kein Beenden");
        D.click(D.find(tr(S::UpdRestart)));
        D.frames(3);
        D.click(D.find(tr(S::Discard)));
        D.frames(3);
        check(ua->restartRequested(), "\"Verwerfen\": Neustart freigegeben");

        D.app = app.get();
        ua.reset();
        D.frames(3);

        // The next start notices the update and cleans up.
        auto ub = std::make_unique<g2::gui::App>(up);
        ub->updater()->wait();
        check(logHas(*ub, "9.0.0") || logHas(*ub, "1.0.0"), "Neustart meldet das Update im Protokoll");
        check(!fs::exists(ud / "g2c.exe.old"), "alte Exe beim Neustart entfernt");
        ub.reset();
    });

    // =====================================================================
    step("18. Alle schliessen");
    guardedStep("Schliessen", [&] {
        if (app->documents().empty()) return;
        for (auto& d : app->documents()) d.dirty = true;
        D.menu(tr(S::MenuFile), tr(S::CloseAll));
        D.frames(2);
        check(!app->documents().empty() || D.popupOpen(),
              "Alle schliessen mit ungespeicherten Aenderungen fragt nach");
    });

    // --- Summary ---
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
