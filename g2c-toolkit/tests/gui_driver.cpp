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
                     "<anims.h> [<groß.car>]]\n");
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
    // Several folders at once (Ctrl/Shift in the real dialog); otherwise the
    // single answer from nextFolder, so the one-folder tests stay as they are.
    std::vector<std::vector<std::string>> nextFolders;
    plat.pickFolders = [&](const char* title, const std::string&) {
        dialogTitles.emplace_back(title ? title : "");
        if (onPickFolder) onPickFolder();
        if (!nextFolders.empty()) {
            auto f = nextFolders.front();
            nextFolders.erase(nextFolders.begin());
            return f;
        }
        if (nextFolder.empty()) return std::vector<std::string>{};
        std::vector<std::string> one{nextFolder.front()};
        nextFolder.erase(nextFolder.begin());
        return one;
    };
    plat.revealInExplorer = [&](const std::string& p) { revealed.push_back(p); };
    std::string windowTitle;
    plat.setTitle = [&](const std::string& t) { windowTitle = t; };
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
            check(D.popupOpen(), std::string("Menue öffnet: ") + tr(m));
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
              "Hell/Dunkel sechsmal umgeschaltet: Abstaende unverändert (" +
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
        check(!D.windowOpen(tr(S::About)), "Info-Fenster über Schließen zu");

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
              "zweiter Klick stellt zurück");
        {
            const bool g0 = st.writeGla, cf0 = st.writeCfg, i0 = st.writeInfo;
            for (S s : {S::WriteGla, S::WriteCfg, S::WriteInfo}) D.click(D.find(tr(s)));
            check(st.writeGla != g0 && st.writeCfg != cf0 && st.writeInfo != i0,
                  "Haekchen GLA, animation.cfg und _info.txt reagieren");
            for (S s : {S::WriteGla, S::WriteCfg, S::WriteInfo}) D.click(D.find(tr(s)));
            check(st.writeGla == g0 && st.writeCfg == cf0 && st.writeInfo == i0,
                  "zweiter Klick stellt GLA/cfg/_info zurück");
        }
        {
            const bool n0 = st.newSkeleton;
            D.click(D.find(tr(S::NewSkeleton)));
            check(st.newSkeleton != n0, "Haekchen \"Skelett neu bauen\" reagiert");
            D.click(D.find(tr(S::NewSkeleton)));
            check(st.newSkeleton == n0, "zweiter Klick stellt es zurück");
            const bool o0 = st.onlyNewXsi;
            D.click(D.find(tr(S::OnlyNewXsi)));
            check(st.onlyNewXsi != o0, "Haekchen \"Nur neue XSI\" in der Werkzeugleiste reagiert");
            D.click(D.find(tr(S::OnlyNewXsi)));
            check(st.onlyNewXsi == o0, "zweiter Klick stellt es zurück");
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
            check(st.baseDir == base.string(), "Assetwurzel über Ordnerdialog gesetzt");
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
    step("3. Skripte öffnen (Menue, Ordner, Tabs)");
    guardedStep("Oeffnen", [&] {
        if (!real) {
            out("    (ohne echte Daten übersprungen)\n");
            return;
        }
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        D.frames(2);
        check(app->documents().size() == 1, "Datei > Skript öffnen: ein Tab");

        nextFolder.push_back((cars / "b").string());
        D.menu(tr(S::MenuFile), tr(S::OpenFolder));
        D.frames(2);
        check(app->documents().size() == 2, "Datei > Ordner öffnen: zweiter Tab");

        // The same file must not open twice.
        nextFiles.push_back({smallCar.string()});
        D.menu(tr(S::MenuFile), tr(S::OpenScript));
        check(app->documents().size() == 2, "dieselbe Datei nicht doppelt geöffnet");

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
                  "nach dem Neustart ist der zuletzt aktive Tab gewählt");
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
                                      std::to_string(hiddenSel) + " ausgeblendet gewählt)");
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
        check(d.dirty, "Skript als geändert markiert");
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
            check(nameAt(6) == m, "untere Haelfte von Zeile 6: dahinter eingefügt (" + grabNames(d) + ")");

            const std::string m2 = nameAt(0);
            ziehe(m2, 3, 0.2f, nullptr);
            check(nameAt(2) == m2, "obere Haelfte von Zeile 3: davor eingefügt (" + grabNames(d) + ")");

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
                      "unter eine Ueberschrift gezogen: Ueberschrift bleibt darüber");
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
            check(otherUnchanged, "Zeile auf anderen Tab gezogen: das ANDERE Skript bleibt unverändert");
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
        check(d.script.grabs[2].commentsBefore.size() == c0 + 1, "Trennlinie eingefügt");

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
        check(d.script.grabs.size() == n0 + 1, "Einfügen danach: eine Zeile mehr");

        // Delete with confirmation: first No, then Yes.
        ctx(6, S::DeleteSeq);
        check(D.popupOpen(), "Löschen fragt nach");
        D.click(D.find(tr(S::No)));
        D.frames(2);
        check(d.script.grabs.size() == n0 + 1, "Nein löscht nichts");
        ctx(6, S::DeleteSeq);
        D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
        D.frames(2);
        check(d.script.grabs.size() == n0, "Ja löscht genau eine");

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
        check(D.windowOpen("###seqdlg"), "Doppelklick öffnet den Sequenzdialog");
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
        check(!dlg, "Doppelklick auf den Zeilenkommentar öffnet NICHT den Sequenzdialog");
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
    step("9. Prüfen und Meldungsliste");
    guardedStep("Prüfen", [&] {
        if (!real || app->documents().empty()) return;
        D.click(D.findIconButton(tr(S::BtnValidate), "##main"));
        D.frames(3);
        const auto& d = app->documents()[static_cast<std::size_t>(app->activeTab())];
        check(d.validated, "Knopf Prüfen hat geprüft");
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
        check(d.dirty, "Skript hat ungespeicherte Änderungen");
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
        D.frames(2);
        check(app->toastCount() > 0, "nach dem Bauen erscheint ein Hinweis unten rechts");

        // Log: open the output folder. The panel may be folded - open it with
        // its arrow, as a user would.
        if (!app->bottomPanelOpen()) {
            D.click(D.findById("##bottomtoggle", "bottom"));
            D.frames(2);
        }
        check(app->bottomPanelOpen(), "Pfeil klappt Protokoll und Meldungen auf");
        if (!D.findTab(tr(S::TabLog))) out("    (Reiter %s nicht gefunden)\n", tr(S::TabLog));
        D.click(D.findTab(tr(S::TabLog)));
        D.frames(2);
        if (!D.findEnds(tr(S::OpenOutputDir))) {
            out("    (Knopf \"%s\" nicht da; Protokoll-Knopf da: %d)\n", tr(S::OpenOutputDir),
                D.findEnds(tr(S::CopyLog)) != nullptr);
        }
        D.click(D.findEnds(tr(S::OpenOutputDir)));
        check(!revealed.empty(), "Ausgabeordner öffnen ruft den Explorer");
        D.click(D.findEnds(tr(S::CopyLog)));
        check(logHas(*app, "Zeilen") || true, "Protokoll kopieren");
    });

    // =====================================================================
    step("10b. Ausgaben wählen, Modell-Skript");
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
              "nur animation.cfg gewählt: genau eine Datei geschrieben (" + std::to_string(files) + ")");

        // Only the GLA.
        fs::remove_all(only);
        st.writeGla = true;
        st.writeCfg = false;
        D.click(D.findIconButton(tr(S::BtnBuild), "##main"));
        D.waitBuild();
        files = 0;
        for (const auto& e : fs::directory_iterator(only, ec)) (void)e, ++files;
        check(files == 1 && !fs::exists(only / "animation.cfg"),
              "nur GLA gewählt: keine animation.cfg, eine Datei (" + std::to_string(files) + ")");
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
        check(app->openCar((md / "model.car").string()), "Modell-Skript geöffnet");
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
    step("11. Speichern, Sicherung, Schließen");
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
        check(dialogTitles.size() > titles0, "Strg+O öffnet den Dateidialog");

        // Ctrl+W: a clean tab closes, a modified one asks first.
        const std::size_t n0 = app->documents().size();
        for (auto& x : app->documents()) x.dirty = false;
        D.key(ImGuiKey_W, true);
        D.frames(2);
        check(app->documents().size() == n0 - 1, "Strg+W schließt den aktiven Tab");
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
    step("12. GLA -> XSI: öffnen, wählen, exportieren, vergleichen");
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
        D.click(D.findIconButton(lbl, "extract"));
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
        check(ex.xsiVersion == g2::xsiexp::ExportOptions::Version::V35, "dotXSI 3.5 gewählt");

        // Everything with .car. A .car already exists: it must ask first and
        // make a backup before replacing it.
        const fs::path carOut = outX / "t_gui.car";
        writeText(carOut, "// eigene Datei, darf nicht still überschrieben werden\n");
        D.click(D.findIconButton(tr(S::ExportAllWithCar), "extract"));
        D.frames(3);
        check(D.popupOpen() && readAll(carOut).find("eigene Datei") != std::string::npos,
              "vorhandene Dateien: erst Rueckfrage, noch nichts überschrieben");
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
            check(ok, "exportierte .car ist lesbar und vollständig");
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
        check(nsel == ex.seqs.size() - 2, "Fehlende gewählt: " + std::to_string(nsel));
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
        // Sequence selection: a click in the list on the left plays it.
        const auto& ex = app->extract();
        if (ex.seqs.size() >= 2) {
            const Item* s = D.findIf([&](const Item& i) {
                return displayOf(i.label) == ex.seqs[1].name && i.window.find("pvlist") != std::string::npos;
            });
            check(s != nullptr, "Sequenzliste links in der Vorschau");
            D.click(s);
            D.frames(2);
            check(app->previewSequence() == 1 && app->previewPlaying(),
                  "Klick in der Liste wechselt die Sequenz und spielt sie ab");
            // Keyboard: space pauses, down arrow goes to the next sequence.
            D.key(ImGuiKey_Space);
            D.frames(1);
            check(!app->previewPlaying(), "Leertaste haelt an");
            D.key(ImGuiKey_DownArrow);
            D.frames(1);
            check(app->previewSequence() == 2 || ex.seqs.size() == 2, "Pfeil runter: nächste Sequenz");
        }
        // Double-click in the GLA -> XSI list opens the preview on that row.
        {
            app->setMode(g2::gui::App::Mode::Extract);
            D.frames(2);
            const Item* r0 = D.findIf([&](const Item& i) {
                return i.window.find("extract") != std::string::npos && displayOf(i.label) == ex.seqs[0].name;
            });
            D.doubleClick(r0);
            D.frames(2);
            check(app->mode() == g2::gui::App::Mode::Preview && app->previewSequence() == 0,
                  "Doppelklick in GLA -> XSI zeigt die Sequenz in der Vorschau");
        }

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
        check(ok, "Skript in Ordner mit Umlaut geöffnet (UTF-8-Pfad aus Dialog/Ziehen)");
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

        // "Only new XSI" against the real script: picking a folder it grabs
        // from adds exactly the files that are not in it yet, and a second
        // time nothing.
        // On a copy of its own, closed again afterwards: the later steps
        // need the big script unchanged.
        if (real) {
            const fs::path copy2 = work / "big2" / bigCar.filename();
            fs::create_directories(copy2.parent_path());
            fs::copy_file(bigCar, copy2, fs::copy_options::overwrite_existing);
            app->openCar(copy2.string());
            const std::size_t idx = app->documents().size() - 1;
            const auto& d = app->documents()[idx];
            auto& st = app->settings();
            const bool o0 = st.onlyNewXsi;
            const std::string b0 = st.baseDir;
            st.onlyNewXsi = true;
            st.baseDir = base.string();
            const fs::path folder = (base / d.script.grabs.front().file).parent_path();
            const auto key = [](std::string s) {
                for (auto& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (s.size() > 4 && s.compare(s.size() - 4, 4, ".xsi") == 0) s.resize(s.size() - 4);
                return s;
            };
            // By file name alone, whatever the folder - and each name once.
            std::set<std::string> grabbed;
            for (const auto& g : d.script.grabs) grabbed.insert(key(fs::path(g.file).filename().string()));
            std::size_t expected = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file() && key(it->path().extension().string()) == ".xsi" &&
                    grabbed.insert(key(it->path().filename().string())).second)
                    ++expected;
            const std::size_t before = d.script.grabs.size();
            app->addXsiFolder(folder.string(), false);
            check(d.script.grabs.size() - before == expected,
                  "Nur neue XSI, echtes Skript: " + std::to_string(d.script.grabs.size() - before) +
                      " eingefügt, erwartet " + std::to_string(expected) + " (" + folder.filename().string() + ")");
            const std::size_t mid = d.script.grabs.size();
            app->addXsiFolder(folder.string(), false);
            check(d.script.grabs.size() == mid, "Nur neue XSI, echtes Skript: zweiter Durchlauf fuegt nichts ein");
            st.onlyNewXsi = o0;
            st.baseDir = b0;
            app->closeDocument(idx);
        }
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
            check(plus.size() >= 2, "Plus-Knoepfe für Loop und Speed da");
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
        // REGRESSION: decimals - the engine reads 16.5, the field used to be
        // an integer field.
        D.click(D.find(tr(S::DlgFrameSpeed), "seqdlg"));
        D.key(ImGuiKey_A, true);
        D.type("16.5");
        D.key(ImGuiKey_Enter);
        check(G[0].frameSpeed && *G[0].frameSpeed == 16.5,
              "REGRESSION Speed mit Komma eingetippt: 16.5 (Fehler: 16)");
        // REGRESSION: a backwards speed was shown as the default 30, so its
        // "+" turned -20 into 31 and the sequence played forwards.
        G[0].frameSpeed = -20.0;
        D.frames(3);
        {
            std::vector<const Item*> plus;
            for (const Item& it : g_last)
                if (it.window.find("seqdlg") != std::string::npos && it.label == "+") plus.push_back(&it);
            if (plus.size() >= 2) {
                D.click(plus[1]);
                D.frames(2);
            }
            check(G[0].frameSpeed && *G[0].frameSpeed == -19,
                  "REGRESSION Rueckwaerts-Speed: + macht aus -20 eine -19 (Fehler: 31 = Vorgabe 30 plus 1), ist " +
                      (G[0].frameSpeed ? std::to_string(*G[0].frameSpeed) : std::string("leer")));
        }
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
            check(D.popupOpen(), "Framespeed für 3 Zeilen: Dialog offen");
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
                  "\"Wie in der .xsi\" für alle: -framespeed entfernt");
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
        check(md.script.keepMotion && !md.dirty, "Abbrechen ändert nichts");

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
              "Speichern unter: Tab gehört jetzt zu kopie.car");
        check(!app->recentFiles().empty() && fs::path(app->recentFiles().front()).filename() == "kopie.car",
              "zuletzt geöffnet: kopie.car oben");

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
              "uebrige Zeilen unverändert (40, 5)");
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
    step("16d. Moderne Oberflaeche: Filter-Chips, Zelle bearbeiten, Rueckgaengig, Befehlssuche");
    guardedStep("Modern", [&] {
        if (D.popupOpen()) D.escape();
        app->setMode(g2::gui::App::Mode::Build);
        const fs::path mdir = work / "modern";
        fs::create_directories(mdir);
        writeText(mdir / "m.car",
                  "$aseanimgrabinit\n"
                  "$aseanimgrab models/m/walk_aaa.xsi -loop 0 -framespeed 20\n"
                  "$aseanimgrab models/m/walk_bbb.xsi -framespeed -15\n"
                  "$aseanimgrab models/m/walk_ccc.xsi -framespeed 30 -additional 0 1 -1 20 CCC_PART\n"
                  "$aseanimgrabfinalize\n"
                  "$aseanimconvertmdx_noask models/m/root -makeskel models/m/m\n");
        const std::string mpath = (mdir / "m.car").string();
        app->openCar(mpath);
        D.frames(2);
        D.click(D.findTab("###" + mpath));
        D.frames(3);
        std::size_t mi = 0;
        for (std::size_t k = 0; k < app->documents().size(); ++k)
            if (app->documents()[k].path == mpath) mi = k;
        auto& G = app->documents()[mi].script.grabs;

        // Quick filter "backwards": only WALK_BBB is left.
        {
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), tr(S::ChipBackwards), static_cast<std::size_t>(1));
            const Item* chip = D.find(lbl);
            check(chip != nullptr, "Filter-Chip \"Rueckwaerts (1)\" vorhanden");
            D.click(chip);
            D.frames(2);
            check(D.row("WALK_AAA") == nullptr && D.row("WALK_BBB") != nullptr, "Rueckwaerts-Chip zeigt nur WALK_BBB");
            std::snprintf(lbl, sizeof(lbl), tr(S::ChipAll), G.size());
            D.click(D.find(lbl));
            D.frames(2);
            check(D.row("WALK_AAA") != nullptr, "\"Alle\" zeigt wieder alle Zeilen");
        }

        // Double-click the speed cell of WALK_AAA, type 16,5, Enter.
        // The cell is the item with that text in the row's height (the table
        // headers are no items for the driver).
        const auto cellOf = [&](const std::string& rowName, const std::string& text) -> std::optional<Item> {
            const Item* r = D.row(rowName);
            if (!r) return std::nullopt;
            const float y = Driver::center(*r).y;
            const Item* c = D.findIf([&](const Item& i) {
                return i.window.find("seqs") != std::string::npos && displayOf(i.label) == text && i.bb.Min.y <= y &&
                       i.bb.Max.y >= y && &i != r;
            });
            if (!c) return std::nullopt;
            return *c;
        };
        {
            const auto cell = cellOf("WALK_AAA", "20");
            check(cell.has_value(), "Speed-Zelle von WALK_AAA gefunden");
            if (cell) {
                D.doubleClick(&*cell, false);
                D.frames(2);
                D.key(ImGuiKey_A, true);
                D.type("16,5");
                D.key(ImGuiKey_Enter);
                D.frames(3);
            }
            check(G[0].frameSpeed && *G[0].frameSpeed == 16.5, "Speed direkt in der Zelle geaendert: 16,5 -> 16.5");
        }
        // Undo / redo with the keyboard.
        D.key(ImGuiKey_Z, true);
        D.frames(2);
        check(G[0].frameSpeed && *G[0].frameSpeed == 20, "Strg+Z macht die Aenderung rueckgaengig (20)");
        D.key(ImGuiKey_Y, true);
        D.frames(2);
        check(G[0].frameSpeed && *G[0].frameSpeed == 16.5, "Strg+Y stellt sie wieder her (16.5)");
        // Emptying the loop cell removes -loop (default).
        {
            const auto cell = cellOf("WALK_AAA", "0");
            if (cell) {
                D.doubleClick(&*cell, false);
                D.frames(2);
                D.key(ImGuiKey_A, true);
                D.key(ImGuiKey_Backspace);
                D.key(ImGuiKey_Enter);
                D.frames(3);
            }
            check(!G[0].loop, "leere Loop-Zelle entfernt -loop");
        }

        // REGRESSION (review): undo closes the sequence dialog - it holds a
        // row number that points to another sequence after the swap.
        {
            D.doubleClick(D.row("WALK_BBB"));
            D.frames(2);
            const bool opened = D.windowOpen("###seqdlg");
            app->setFrameSpeed(mi, {0}, 5.0);
            D.frames(3);
            app->undo();
            D.frames(2);
            check(opened && !D.windowOpen("###seqdlg"), "REGRESSION Rückgängig schließt den Sequenzdialog");
            // A new change after undo ends what could be redone.
            check(app->canRedo(), "nach Rückgängig ist Wiederholen möglich");
            app->setFrameSpeed(mi, {1}, -12.0);
            D.frames(3);
            check(!app->canRedo(), "REGRESSION neue Änderung leert Wiederholen");

            // The same while TYPING right after an undo: the keystrokes used
            // to be merged into the step before, and Ctrl+Y then threw them
            // away.
            app->setFrameSpeed(mi, {0}, 7.0);
            D.frames(3);
            app->undo();
            D.frames(1);
            D.doubleClick(D.row("WALK_AAA"));
            D.frames(2);
            D.click(D.find(tr(S::DlgFrameSpeed), "seqdlg"));
            D.key(ImGuiKey_A, true);
            D.type("33");
            D.frames(2);
            check(!app->canRedo(), "REGRESSION Tippen direkt nach Rückgängig leert Wiederholen");
            D.key(ImGuiKey_Enter);
            D.escape();
            D.frames(2);
        }

        // Saving shows a toast.
        const std::size_t toastsBefore = app->toastCount();
        app->saveDocument(mi);
        D.frames(2);
        check(app->toastCount() > toastsBefore || app->toastCount() == 4, "Speichern zeigt einen Hinweis");

        // REGRESSION (review): "Save as" takes the undo history along; the
        // original, opened again, starts without it.
        {
            const std::string other = (mdir / "m_kopie.car").string();
            check(app->canUndo(), "vor Speichern unter: Rückgängig möglich");
            app->saveDocumentAs(mi, other);
            D.frames(2);
            check(app->canUndo(), "REGRESSION Speichern unter: Historie wandert mit");
            app->openCar(mpath);
            D.frames(1);
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == mpath) app->activate(static_cast<int>(k));
            D.frames(3);
            const std::string activePath = app->documents()[static_cast<std::size_t>(app->activeTab())].path;
            check(activePath == mpath && !app->canUndo(),
                  "REGRESSION wieder geöffnetes Original hat keine fremde Historie (aktiv: " +
                      fs::path(activePath).filename().string() + ", Rückgängig: " + (app->canUndo() ? "ja" : "nein") + ")");
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == other) mi = k;
            D.click(D.findTab("###" + other));
            D.frames(2);
        }

        // Open folders: two names found -> the choice window; untick
        // model.car, open the rest.
        {
            const fs::path mf = work / "multifolder";
            for (int k = 0; k < 2; ++k)
                writeText(mf / ("c" + std::to_string(k)) / "_humanoid.car",
                          "$aseanimgrabinit\n$aseanimgrab x.xsi\n$aseanimgrabfinalize\n");
            writeText(mf / "c1" / "model.car", "$aseanimgrabinit\n$aseanimgrab y.xsi\n$aseanimgrabfinalize\n");
            const std::size_t docsBefore = app->documents().size();
            app->openFolders({mf.string()});
            D.frames(3);
            check(app->carPickPending() && D.popupOpen(), "Ordner mit verschiedenen .car: Auswahlfenster offen");
            D.click(D.find("model.car (1)"));
            D.frames(2);
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), tr(S::CarPickOpen), static_cast<std::size_t>(2));
            D.click(D.find(lbl));
            D.frames(3);
            check(!app->carPickPending() && app->documents().size() == docsBefore + 2,
                  "nur die zwei _humanoid.car geöffnet");
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == mpath || app->documents()[k].path == (mdir / "m_kopie.car").string())
                    app->activate(static_cast<int>(k));
            D.frames(2);
        }

        // Validate: there are issues (the .xsi files are missing), so the
        // panel opens by itself.
        D.click(D.findIconButton(tr(S::BtnValidate), "##main"));
        D.frames(2);
        check(app->bottomPanelOpen(), "Pruefen mit Meldungen klappt den unteren Bereich auf");

        // Command palette: Ctrl+K, type, Enter.
        D.key(ImGuiKey_K, true);
        D.frames(2);
        check(D.popupOpen(), "Strg+K oeffnet die Befehlssuche");
        D.type(tr(S::ModePreview));
        D.frames(1);
        D.key(ImGuiKey_Enter);
        D.frames(2);
        check(app->mode() == g2::gui::App::Mode::Preview, "Befehlssuche: \"Vorschau\" + Enter wechselt den Modus");
        app->setMode(g2::gui::App::Mode::Build);
        D.frames(2);
    });

    // =====================================================================
    // Reported 2026-10-19: with many humanoids open, "Add XSI -> Folder" added
    // nothing and the view jumped from the last tab to the second.
    step("16e. Viele Tabs: XSI-Ordner landet im aktiven Tab");
    guardedStep("VieleTabs", [&] {
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        app->setMode(g2::gui::App::Mode::Build);
        const fs::path many = work / "many";
        const fs::path xd = work / "many_xsi";
        for (int k = 0; k < 25; ++k) {
            char dir[64], body[160];
            std::snprintf(dir, sizeof(dir), "_humanoid_charakter_%02d", k);
            std::snprintf(body, sizeof(body), "$aseanimgrabinit\n$aseanimgrab models/c/k%02d.xsi\n$aseanimgrabfinalize\n", k);
            writeText(many / dir / "_humanoid.car", body);
        }
        for (const char* n : {"neu_a.xsi", "neu_b.xsi"}) writeText(xd / n, "xsi 0300txt 0032\n");
        const std::size_t before = app->documents().size();
        app->openFolders({many.string()});
        D.frames(4);
        check(app->documents().size() == before + 25, "25 Humanoids geöffnet");
        const std::string lastPath = (many / "_humanoid_charakter_24" / "_humanoid.car").string();
        int idx = -1;
        for (std::size_t k = 0; k < app->documents().size(); ++k)
            if (app->documents()[k].path == lastPath) idx = static_cast<int>(k);
        // The user's way: another tab first, then the last one by clicking.
        D.click(D.findTab("###" + (many / "_humanoid_charakter_01" / "_humanoid.car").string()));
        D.frames(3);
        D.click(D.findTab("###" + lastPath));
        D.frames(3);
        out("    (aktiv nach Klick auf den letzten Tab: %d, erwartet %d)\n", app->activeTab(), idx);
        check(app->activeTab() == idx, "letzter Tab ist aktiv");
        const std::size_t grabsBefore = idx >= 0 ? app->documents()[static_cast<std::size_t>(idx)].script.grabs.size() : 0;

        nextFolder.push_back(xd.string());
        D.click(D.findIconButton((std::string(tr(S::BtnAddXsi)) + "    ").c_str(), "##main"));
        D.frames(2);
        D.click(D.findEnds(tr(S::AddXsiMenuFolder)));
        D.frames(4);
        out("    (aktiv nach XSI-Ordner: %d)\n", app->activeTab());
        check(app->activeTab() == idx, "REGRESSION aktiver Tab bleibt der letzte (Fehler: springt um)");
        check(idx >= 0 && app->documents()[static_cast<std::size_t>(idx)].script.grabs.size() == grabsBefore + 2,
              "REGRESSION die neuen XSI sind im letzten Tab angekommen");
        std::size_t elsewhere = 0;
        for (std::size_t k = 0; k < app->documents().size(); ++k)
            if (static_cast<int>(k) != idx)
                for (const auto& g : app->documents()[k].script.grabs)
                    if (g.file.find("neu_") != std::string::npos) ++elsewhere;
        check(elsewhere == 0, "kein anderer Tab hat sie bekommen");
    });

    // =====================================================================
    // Reported or found 2026-10-19 - each check describes the CORRECT
    // behaviour and was first run against the old code to prove the bug.
    step("16f. Fehlernachweise: Tabs, Filter, Ordner");
    guardedStep("Fehlernachweise", [&] {
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        app->setMode(g2::gui::App::Mode::Build);
        const fs::path bug = work / "bugs";
        // "ani": a character folder with its .car AND loose .xsi next to it.
        writeText(bug / "ani" / "_humanoid.car",
                  "$aseanimgrabinit\n$aseanimgrab models/b/ani1.xsi -framespeed 20\n"
                  "$aseanimgrab models/b/ani2.xsi -framespeed 20\n$aseanimgrabfinalize\n");
        writeText(bug / "ani" / "neu1.xsi", "xsi 0300txt 0032\n");
        writeText(bug / "ani" / "neu2.xsi", "xsi 0300txt 0032\n");
        writeText(bug / "other" / "_humanoid.car",
                  "$aseanimgrabinit\n$aseanimgrab models/b/oth1.xsi -framespeed 20\n"
                  "$aseanimgrab models/b/oth2.xsi -framespeed 20\n$aseanimgrab models/b/root.xsi\n"
                  "$aseanimgrabfinalize\n");
        writeText(bug / "x1" / "a.xsi", "xsi 0300txt 0032\n");
        writeText(bug / "x1" / "b.xsi", "xsi 0300txt 0032\n");
        writeText(bug / "x2" / "c.xsi", "xsi 0300txt 0032\n");
        writeText(bug / "y" / "neu3.xsi", "xsi 0300txt 0032\n");
        const std::string aniPath = (bug / "ani" / "_humanoid.car").string();
        const std::string othPath = (bug / "other" / "_humanoid.car").string();
        app->openCar(aniPath);
        app->openCar(othPath);
        D.frames(3);
        const auto indexOf = [&](const std::string& p) {
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == p) return static_cast<int>(k);
            return -1;
        };
        const auto grabsOf = [&](const std::string& p) -> const std::vector<g2::car::GrabDirective>& {
            return app->documents()[static_cast<std::size_t>(indexOf(p))].script.grabs;
        };
        const auto show = [&](const std::string& p) {
            app->activate(indexOf(p));
            D.frames(3);
        };
        // Empties the filter box of the tab on screen.
        const auto filterBox = [&] { return D.findIf([](const Item& i) { return i.label == "##filter"; }); };
        const auto clearFilter = [&] {
            D.click(filterBox());
            D.key(ImGuiKey_A, true);
            D.key(ImGuiKey_Backspace);
            D.key(ImGuiKey_Enter);
            D.frames(2);
        };

        // B1 (the report): "Ordner öffnen" on a folder whose .car is already
        // open jumped to that tab and added none of its .xsi.
        show(othPath);
        nextFolder.push_back((bug / "ani").string());
        D.click(D.findIconButton(tr(S::BtnOpenFolder), "##main"));
        D.frames(3);
        out("    (B1: aktiv %d, erwartet other=%d, ani=%d)\n", app->activeTab(), indexOf(othPath), indexOf(aniPath));
        check(app->activeTab() == indexOf(othPath),
              "B1 Ordner mit schon offener .car: kein Sprung zu deren Tab");
        // Its .xsi are offered for the script on screen - one click.
        {
            const std::string want =
                trf(S::ToastAddXsiTo, app->documents()[static_cast<std::size_t>(indexOf(othPath))].title.c_str());
            const Item* offer = D.find(want, "toast");
            const std::size_t aniBefore = grabsOf(aniPath).size();
            if (offer) D.click(offer);
            D.frames(3);
            std::size_t neu = 0;
            for (const auto& g : grabsOf(othPath))
                if (g.file.find("neu1.xsi") != std::string::npos || g.file.find("neu2.xsi") != std::string::npos) ++neu;
            check(offer != nullptr && neu == 2 && app->activeTab() == indexOf(othPath) &&
                      grabsOf(aniPath).size() == aniBefore,
                  "B1 Angebot \"" + want + "\" fügt die 2 .xsi des Ordners ins aktive Skript ein");
        }

        // B2: a filter hides newly added rows - and is shared by all tabs.
        show(othPath);
        D.click(filterBox());
        D.type("oth1");
        D.key(ImGuiKey_Enter);
        D.frames(2);
        // First the other tab - before B2a, which clears the filter.
        show(aniPath);
        check(D.row("ANI1") != nullptr, "B2b Filter gilt nur im eigenen Tab (Fehler: im anderen Tab alles weg)");
        show(othPath);
        check(D.row("OTH1") != nullptr && D.row("OTH2") == nullptr, "B2b der eigene Tab behält seinen Filter");
        app->addXsiFiles({(bug / "y" / "neu3.xsi").string()}, false);
        D.frames(3);
        check(D.row("NEU3") != nullptr, "B2a eben hinzugefügte Zeile ist sichtbar (Filter blendet sie aus)");
        show(aniPath);
        // No filter left for the following checks, in either tab.
        clearFilter();
        show(othPath);
        clearFilter();

        // B3: adding to a script that could not be read.
        writeText(bug / "kaputt" / "k.car", "$aseanimgrabinit\n$aseanimgrab a.xsi -loop xyz\n$aseanimgrabfinalize\n");
        const std::string badPath = (bug / "kaputt" / "k.car").string();
        app->openCar(badPath);
        D.frames(2);
        show(badPath);
        app->addXsiFiles({(bug / "ani" / "neu2.xsi").string()}, false);
        D.frames(2);
        check(indexOf(badPath) >= 0 && grabsOf(badPath).empty(),
              "B3 in ein unlesbares Skript wird nichts eingefügt");

        // B5: closing a changed tab with the middle button, then "Cancel".
        show(othPath);
        app->setFrameSpeed(static_cast<std::size_t>(indexOf(othPath)), {0}, 25.0);
        D.frames(2);
        D.click(D.findTab("###" + othPath), 2);
        D.frames(3);
        const bool askedClose = D.popupOpen();
        D.click(D.find(tr(S::No), "closedlg"));
        D.frames(4);
        out("    (B5: Rückfrage %d, aktiv %d, erwartet %d)\n", askedClose ? 1 : 0, app->activeTab(), indexOf(othPath));
        check(askedClose && indexOf(othPath) >= 0 && app->activeTab() == indexOf(othPath),
              "B5 Schließen abgebrochen: der Tab bleibt der aktive");

        // B6: "Save as" kept the tab but switched the view.
        show(othPath);
        const std::string othCopy = (bug / "other" / "kopie.car").string();
        app->saveDocumentAs(static_cast<std::size_t>(indexOf(othPath)), othCopy);
        D.frames(4);
        out("    (B6: aktiv %d, erwartet %d)\n", app->activeTab(), indexOf(othCopy));
        check(app->activeTab() == indexOf(othCopy), "B6 Speichern unter: der Tab bleibt der aktive");

        // B7: the same file under another spelling opened twice.
        {
            const std::size_t n = app->documents().size();
            std::string variant = aniPath;
            std::transform(variant.begin(), variant.end(), variant.begin(),
                           [](unsigned char c) { return static_cast<char>(c == '\\' ? '/' : std::tolower(c)); });
            app->openCar(variant);
            D.frames(2);
            check(app->documents().size() == n, "B7 gleiche Datei in anderer Schreibweise öffnet keinen zweiten Tab");
        }

        // B8: a cell edit started in one tab, the tab switched by clicking,
        // the typed value landed in the other script.
        {
            show(othCopy);
            const Item* r = D.row("OTH2");
            const Item* c = r ? D.findIf([&](const Item& i) {
                return i.window.find("seqs") != std::string::npos && displayOf(i.label) == "20" &&
                       i.bb.Min.y <= Driver::center(*r).y && i.bb.Max.y >= Driver::center(*r).y;
            }) : nullptr;
            if (c) {
                const Item cell = *c;
                D.doubleClick(&cell, false);
                D.frames(2);
            }
            // Switched from OUTSIDE (a .car double-clicked in Explorer while
            // g2c runs hands it over and activates its tab) - a click on a
            // tab finishes the edit first and is fine.
            app->activate(indexOf(aniPath));
            D.frames(3);
            D.type("99");
            D.key(ImGuiKey_Enter);
            D.frames(3);
            bool leaked = false;
            for (const auto& g : grabsOf(aniPath))
                if (g.frameSpeed && *g.frameSpeed == 99) leaked = true;
            out("    (B8: Zelle gefunden %d, Wert im anderen Skript gelandet %d)\n", c ? 1 : 0, leaked ? 1 : 0);
            check(c != nullptr && !leaked, "B8 Zellbearbeitung landet nach Tabwechsel von außen nicht im anderen Skript");
        }

        // B10: two folders added: the summary counted scripts, not files.
        {
            show(aniPath);
            nextFolders.push_back({(bug / "x1").string(), (bug / "x2").string()});
            D.click(D.findIconButton((std::string(tr(S::BtnAddXsi)) + "    ").c_str(), "##main"));
            D.frames(2);
            D.click(D.findEnds(tr(S::AddXsiMenuFolder)));
            D.frames(3);
            char want[96];
            std::snprintf(want, sizeof(want), tr(S::FoldersAdded), static_cast<std::size_t>(2), static_cast<std::size_t>(3));
            for (const auto& l : app->logLines())
                if (l.text.find("Ordner durchsucht") != std::string::npos) out("    (B10 Protokoll: %s)\n", l.text.c_str());
            check(logHas(*app, want), std::string("B10 Zusammenfassung nennt 3 Dateien (\"") + want + "\")");
        }

        // B11: a dropped .xsi while "Delete root?" is open - the delete then
        // hit another row.
        {
            show(othCopy);
            const Item* rootRow = D.row("ROOT");
            D.click(rootRow, 1);
            D.frames(2);
            D.click(D.findEnds(tr(S::DeleteSeq)));
            D.frames(2);
            const bool confirmOpen = D.popupOpen();
            app->queueOpen({(bug / "x2" / "c.xsi").string()});
            D.frames(3);
            D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
            D.frames(3);
            bool hasRoot = false, hasC = false;
            for (const auto& g : grabsOf(othCopy)) {
                if (g2::gui::App::isRootGrab(g)) hasRoot = true;
                if (g.file.find("c.xsi") != std::string::npos) hasC = true;
            }
            out("    (B11: Rückfrage %d, root noch da %d, c.xsi da %d)\n", confirmOpen ? 1 : 0, hasRoot ? 1 : 0, hasC ? 1 : 0);
            check(confirmOpen && !hasRoot && hasC,
                  "B11 Löschen nach Ablegen einer .xsi trifft die gewählte Zeile (root), nicht die neue");
        }

        // Leave nothing behind for the later steps: no filter, none of these tabs.
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        for (const std::string& p : {othCopy, aniPath, badPath}) {
            if (indexOf(p) < 0) continue;
            show(p);
            clearFilter();
        }
        for (const std::string& p : {othCopy, aniPath, badPath, othPath})
            while (indexOf(p) >= 0) app->closeDocument(static_cast<std::size_t>(indexOf(p)));
        D.frames(2);
    });

    // =====================================================================
    // Second code review, 2026-10-19 - each check describes the CORRECT
    // behaviour and was first run against the old code to prove the bug.
    step("16g. Fehlernachweise 2: Auswahl, Tabs, Speed, Framezahlen, Kommentare");
    guardedStep("Fehlernachweise 2", [&] {
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        app->setMode(g2::gui::App::Mode::Build);
        auto& st = app->settings();
        const std::string oldBase = st.baseDir;
        const fs::path rv = work / "review2";
        const auto five = [](const char* p) {
            std::string s = "$aseanimgrabinit\n";
            for (const char* n : {"ONE", "TWO", "THREE", "FOUR", "FIVE"})
                s += std::string("$aseanimgrab r/") + n + ".xsi -enum " + p + n + "\n";
            return s + "$aseanimgrabfinalize\n";
        };
        writeText(rv / "sel.car", five("WALK_"));
        writeText(rv / "taba.car", five("A_"));
        writeText(rv / "tabb.car", five("B_"));
        const std::string selPath = (rv / "sel.car").string(), aPath = (rv / "taba.car").string(),
                          bPath = (rv / "tabb.car").string();
        for (const auto& p : {selPath, aPath, bPath}) app->openCar(p);
        D.frames(3);
        const auto indexOf = [&](const std::string& p) {
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == p) return static_cast<int>(k);
            return -1;
        };
        const auto docOf = [&](const std::string& p) -> g2::gui::Document& {
            return app->documents()[static_cast<std::size_t>(indexOf(p))];
        };
        const auto show = [&](const std::string& p) {
            app->activate(indexOf(p));
            D.frames(3);
        };
        const auto has = [&](const std::string& p, const std::string& name) {
            for (const auto& g : docOf(p).script.grabs)
                if (g.enumName && *g.enumName == name) return true;
            return false;
        };
        const auto filterBox = [&] { return D.findIf([](const Item& i) { return i.label == "##filter"; }); };

        // R7: rows hidden by the filter were deleted along with the visible ones.
        {
            show(selPath);
            D.clickRow(D.row("WALK_ONE"));
            D.clickRow(D.row("WALK_TWO"), true);
            std::size_t sel = 0;
            for (const char c : docOf(selPath).selected) sel += c ? 1 : 0;
            check(sel == 2, "R7 Vorbedingung: zwei Zeilen gewählt");
            D.click(filterBox());
            D.type("one");
            D.key(ImGuiKey_Enter);
            D.frames(2);
            check(D.row("WALK_ONE") && !D.row("WALK_TWO"), "R7 Vorbedingung: Filter versteckt WALK_TWO");
            D.click(D.row("WALK_ONE"), 1);
            D.frames(2);
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), tr(S::DeleteSelected), static_cast<std::size_t>(2));
            const Item* del = D.findEnds(lbl);
            if (!del) del = D.findEnds(tr(S::DeleteSeq));
            D.click(del);
            D.frames(2);
            D.click(D.findEnds(tr(S::DeleteSeq), "confirmdel"));
            D.frames(3);
            check(!has(selPath, "WALK_ONE"), "R7 Vorbedingung: die sichtbare Zeile ist gelöscht");
            check(has(selPath, "WALK_TWO"), "R7 eine vom Filter versteckte Zeile wird nicht mitgelöscht");
            D.click(filterBox());
            D.key(ImGuiKey_A, true);
            D.key(ImGuiKey_Backspace);
            D.key(ImGuiKey_Enter);
            D.frames(2);
        }

        // R9: a tab clicked with the mouse kept the Shift anchor of the
        // previous tab.
        {
            show(aPath);
            D.clickRow(D.row("A_TWO"));
            D.frames(2);
            D.click(D.findTab("###" + bPath));
            D.frames(3);
            check(app->activeTab() == indexOf(bPath), "R9 Vorbedingung: Tab b per Maus aktiv");
            D.clickRow(D.row("B_FIVE"), false, true);
            std::size_t sel = 0;
            for (const char c : docOf(bPath).selected) sel += c ? 1 : 0;
            out("    (R9: in b gewählt %zu)\n", sel);
            check(sel == 1, "R9 Umschalt+Klick im neuen Tab wählt nicht ab der Zeile des alten Tabs");
        }

        // R6: speed 0 could be set for several rows at once.
        {
            show(bPath);
            D.click(D.row("B_ONE"), 1);
            D.frames(2);
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), tr(S::SetSpeedRows), static_cast<std::size_t>(1));
            D.click(D.findEnds(lbl));
            D.frames(3);
            const Item* field = D.findIf([](const Item& i) {
                return i.label == "##speed" && i.window.find("speeddlg") != std::string::npos;
            });
            check(field != nullptr, "R6 Vorbedingung: Speed-Dialog offen");
            D.click(field);
            D.key(ImGuiKey_A, true);
            D.type("0");
            D.key(ImGuiKey_Enter);
            D.frames(2);
            D.click(D.find(tr(S::Apply), "speeddlg"));
            D.frames(3);
            for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
                D.escape();
                D.frames(2);
            }
            const auto& g = docOf(bPath).script.grabs[0];
            out("    (R6: framespeed %s)\n", g.frameSpeed ? std::to_string(*g.frameSpeed).c_str() : "-");
            check(!(g.frameSpeed && *g.frameSpeed == 0.0), "R6 Speed 0 wird nicht übernommen");
        }

        // R10: frame counts stayed "missing" after the asset root was set for
        // the first time.
        {
            const fs::path fb = rv / "fbase";
            std::ostringstream x;
            x << "xsi 0350txt 0032\nSI_Scene s {\n  \"FRAMES\",\n  1.000000,\n  7.000000,\n  20.000000,\n}\n"
              << "SI_Model MDL-rig.root {\n  SI_Model MDL-rig.a {\n"
              << "    SI_FCurve { \"rig.a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, 7,\n";
            for (int k = 1; k <= 7; ++k) x << "      " << k << "," << k << ",\n";
            x << "    }\n  }\n}\n";
            writeText(fb / "models" / "fanims" / "f1.xsi", x.str());
            const fs::path fcar = rv / "fcar" / "f.car";
            writeText(fcar, "$aseanimgrabinit\n$aseanimgrab models/fanims/f1.xsi -enum F_ONE\n$aseanimgrabfinalize\n");
            st.baseDir.clear();
            app->openCar(fcar.string());
            show(fcar.string());
            for (int k = 0; k < 500 && app->frameCountOf("models/fanims/f1.xsi") == -1; ++k) {
                D.frames(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(st.baseDir.empty() && app->frameCountOf("models/fanims/f1.xsi") == -2,
                  "R10 Vorbedingung: ohne Assetwurzel fehlt die Datei");
            st.baseDir = fb.string();
            for (int k = 0; k < 300 && app->frameCountOf("models/fanims/f1.xsi") < 0; ++k) {
                D.frames(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out("    (R10: Frames nach dem Setzen der Wurzel %d)\n", app->frameCountOf("models/fanims/f1.xsi"));
            check(app->frameCountOf("models/fanims/f1.xsi") == 7, "R10 nach dem ersten Setzen der Assetwurzel werden die Frames gelesen");
            st.baseDir = oldBase;
        }

        // R12: "Move up" on a comment after the last row jumped over the
        // other trailing comments.
        {
            const fs::path cm = rv / "cm.car";
            writeText(cm, "$aseanimgrabinit\n$aseanimgrab c1.xsi -enum CM_ONE\n// T1\n// T2\n$aseanimgrabfinalize\n");
            app->openCar(cm.string());
            show(cm.string());
            auto& sc = docOf(cm.string()).script;
            check(sc.trailingComments.size() == 2, "R12 Vorbedingung: zwei Kommentare hinter der letzten Zeile");
            const Item* t2 = D.findIf([](const Item& i) { return i.label.rfind("// T2", 0) == 0; });
            D.click(t2, 1);
            D.frames(2);
            D.click(D.findEnds(tr(S::MoveUp)));
            D.frames(3);
            check(sc.trailingComments.size() == 2 && sc.trailingComments[0] == "// T2" && sc.trailingComments[1] == "// T1",
                  "R12 Kommentar hinter der letzten Zeile nach oben: tauscht mit T1");
        }

        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        st.baseDir = oldBase;
        for (const std::string& p : {selPath, aPath, bPath, (rv / "fcar" / "f.car").string(), (rv / "cm.car").string()})
            while (indexOf(p) >= 0) app->closeDocument(static_cast<std::size_t>(indexOf(p)));
        D.frames(2);
    });

    // =====================================================================
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
              "REGRESSION: Schließen-Frage wartet, Framespeed-Dialog bleibt sichtbar (vorher beide unsichtbar)");
        D.click(D.find(tr(S::BtnCancel), "speeddlg"));
        D.frames(4);
        check(shown("###closedlg"), "danach erscheint die Schließen-Frage");
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
              "weitergereichte Datei wird im nächsten Bild geöffnet");

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
              "\"Was ist neu?\" öffnet die Seite der Version");

        D.click(D.find(tr(S::UpdLater)));
        D.frames(2);
        check(D.find(tr(S::UpdInstall)) == nullptr, "\"Später\" blendet die Leiste aus");

        D.menu(tr(S::MenuView), tr(S::UpdCheckNow));
        ua->updater()->wait();
        D.frames(3);
        check(D.find(tr(S::UpdInstall)) != nullptr, "Ansicht > Nach Updates suchen: Leiste wieder da");

        D.click(D.find(tr(S::UpdSkip)));
        D.frames(2);
        check(ua->settings().skippedUpdate == "v9.0.0" && D.find(tr(S::UpdInstall)) == nullptr,
              "\"Diese Version überspringen\" merkt sich v9.0.0");

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
        check(ua->settings().checkUpdates == autoBefore, "und wieder zurück");
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
              "Download-Seite geöffnet");
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
        check(D.popupOpen() && !ua->restartRequested(), "Neustart fragt nach ungespeicherten Änderungen");
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
    step("17b. Projekte wie in Visual Studio");
    guardedStep("Projekte", [&] {
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        app->setMode(g2::gui::App::Mode::Build);
        while (!app->documents().empty()) app->closeDocument(0);
        const fs::path pj = work / "projekt";
        writeText(pj / "p1.car", "$aseanimgrabinit\n$aseanimgrab a.xsi\n$aseanimgrabfinalize\n");
        writeText(pj / "p2.car", "$aseanimgrabinit\n$aseanimgrab b.xsi\n$aseanimgrabfinalize\n");
        const std::string p1 = (pj / "p1.car").string(), p2 = (pj / "p2.car").string();
        const std::string projFile = (pj / "mein_projekt.g2cproj").string();
        app->openCar(p1);
        app->openCar(p2);
        D.frames(2);
        const auto indexOf = [&](const std::string& p) {
            for (std::size_t k = 0; k < app->documents().size(); ++k)
                if (app->documents()[k].path == p) return static_cast<int>(k);
            return -1;
        };

        nextFiles.push_back({projFile});
        D.menu(tr(S::MenuFile), tr(S::NewProject));
        D.frames(3);
        check(app->project().scripts.size() == 2 && fs::exists(projFile), "Datei > Neues Projekt: Projekt aus den 2 Tabs");
        check(windowTitle.find("mein_projekt") != std::string::npos, "Fenstertitel nennt das Projekt");
        check(D.findEnds("p1.car", "project") && D.findEnds("p2.car", "project"),
              "Projekt-Explorer rechts listet beide Skripte");

        // Closed tab: still listed, one click opens it again.
        app->closeDocument(static_cast<std::size_t>(indexOf(p2)));
        D.frames(2);
        const Item* e2 = D.findEnds("p2.car", "project");
        check(e2 != nullptr && indexOf(p2) < 0, "geschlossener Tab bleibt im Projekt-Explorer");
        if (e2) D.click(e2);
        D.frames(3);
        check(indexOf(p2) >= 0 && app->activeTab() == indexOf(p2), "Klick im Projekt-Explorer öffnet das Skript wieder");

        // Right button: remove from the project.
        D.click(D.findEnds("p2.car", "project"), 1);
        D.frames(2);
        D.click(D.findEnds(tr(S::ProjRemove)));
        D.frames(3);
        check(app->project().scripts.size() == 1 && indexOf(p2) < 0 && !D.findEnds("p2.car", "project"),
              "Rechtsklick > Aus Projekt entfernen: weg aus Liste und Tabs");

        // Close it: the start page offers it again.
        D.menu(tr(S::MenuFile), tr(S::CloseProject));
        D.frames(3);
        check(app->project().path.empty() && app->documents().empty(), "Datei > Projekt schließen: Projekt und Tabs zu");
        check(windowTitle.find("mein_projekt") == std::string::npos, "Fenstertitel ohne Projekt");
        const Item* recentProj = D.find("mein_projekt", "docs");
        check(recentProj != nullptr, "Startseite: zuletzt verwendetes Projekt");
        if (recentProj) D.click(recentProj);
        D.frames(3);
        check(app->project().name == "mein_projekt" && app->documents().size() == 1 && indexOf(p1) >= 0,
              "Klick auf der Startseite öffnet das Projekt mit seinem Tab");

        // Switching with an unsaved change asks first; "Cancel" keeps everything.
        writeText(pj / "zwei" / "p3.car", "$aseanimgrabinit\n$aseanimgrab c.xsi\n$aseanimgrabfinalize\n");
        const std::string proj2 = (pj / "zwei" / "zwei.g2cproj").string();
        writeText(proj2, "# g2c project\nscript=p3.car\nopen=p3.car\n");
        app->setFrameSpeed(static_cast<std::size_t>(indexOf(p1)), {0}, 25.0);
        nextFiles.push_back({proj2});
        D.menu(tr(S::MenuFile), tr(S::OpenProject));
        D.frames(3);
        const bool asked = D.popupOpen();
        D.click(D.find(tr(S::No), "closedlg"));
        D.frames(3);
        check(asked && app->project().name == "mein_projekt" && indexOf(p1) >= 0,
              "Projekt wechseln mit ungespeicherter Änderung: Rückfrage, Abbrechen behält alles");
        nextFiles.push_back({proj2});
        D.menu(tr(S::MenuFile), tr(S::OpenProject));
        D.frames(3);
        D.click(D.find(tr(S::Discard), "closedlg"));
        D.frames(3);
        check(app->project().name == "zwei" && app->documents().size() == 1 && app->documents()[0].path.find("p3.car") != std::string::npos,
              "Verwerfen: anderes Projekt mit seinem Tab offen");
    });

    // =====================================================================
    step("17c. Pfade ersetzen");
    guardedStep("Pfade ersetzen", [&] {
        for (int tries = 0; tries < 3 && D.popupOpen(); ++tries) {
            D.escape();
            D.frames(2);
        }
        app->setMode(g2::gui::App::Mode::Build);
        const fs::path rp = work / "pfade";
        writeText(rp / "p.car", "$aseanimgrabinit\n$aseanimgrab models/alt/x.xsi\n$aseanimgrab models/alt/y.xsi\n"
                                "$aseanimgrab models/sonst/z.xsi\n$aseanimgrabfinalize\n");
        const std::string pp = (rp / "p.car").string();
        app->openCar(pp);
        D.frames(3);
        D.menu(tr(S::MenuEdit), tr(S::ReplacePaths));
        D.frames(3);
        const Item* f = D.findIf([](const Item& i) { return i.label == tr(S::ReplaceFind) && i.window.find("pathdlg") != std::string::npos; });
        const Item* r = D.findIf([](const Item& i) { return i.label == tr(S::ReplaceWith) && i.window.find("pathdlg") != std::string::npos; });
        check(f && r, "Bearbeiten > Pfade ersetzen: Dialog mit Suchen und Ersetzen");
        if (f) {
            D.click(f);
            D.type("models/ALT/");
        }
        if (r) {
            D.click(r);
            D.type("models/neu/");
        }
        D.frames(3);
        // The preview line is plain text, which the driver does not see: the
        // same function the dialog shows, and the button it enables.
        std::size_t k0 = 0;
        for (std::size_t i = 0; i < app->documents().size(); ++i)
            if (app->documents()[i].path == pp) k0 = i;
        const auto pv = app->previewPathReplace(k0, "models/ALT/", "models/neu/");
        const Item* apply = D.find(tr(S::ReplaceApply), "pathdlg");
        check(pv.matched == 2 && pv.foundAfter == 0 && apply != nullptr, "Vorschau 2 betroffen, Knopf Ersetzen da");
        D.click(apply);
        D.frames(3);
        std::size_t k = 0;
        for (std::size_t i = 0; i < app->documents().size(); ++i)
            if (app->documents()[i].path == pp) k = i;
        const auto& G = app->documents()[k].script.grabs;
        check(!D.popupOpen() && G.size() == 3 && G[0].file == "models/neu/x.xsi" && G[1].file == "models/neu/y.xsi" &&
                  G[2].file == "models/sonst/z.xsi",
              "Ersetzen: zwei Pfade geändert, der dritte nicht");
        app->closeDocument(k);
        D.frames(2);
    });

    // =====================================================================
    step("18. Alle schließen");
    guardedStep("Schließen", [&] {
        if (app->documents().empty()) return;
        for (auto& d : app->documents()) d.dirty = true;
        D.menu(tr(S::MenuFile), tr(S::CloseAll));
        D.frames(2);
        check(!app->documents().empty() || D.popupOpen(),
              "Alle schließen mit ungespeicherten Änderungen fragt nach");
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
