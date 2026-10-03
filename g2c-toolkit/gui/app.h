// gui/app.h - user interface for g2c.
//
// Deliberately separated from the window binding: this file only knows imgui.h
// and the g2 library, no Win32 and no DirectX. Everything that needs the
// operating system - file dialogs, folder selection - comes in through the
// Platform struct.
//
// The reason is not portability for its own sake but testability: this way the
// much larger part of the UI can be compiled and tested without opening a
// window.

#pragma once

#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/preview.h"
#include "gui/update.h"

#include "g2/animenums.h"
#include "g2/xsi_export.h"
#include "g2/carbuild.h"
#include "g2/carscript.h"
#include "g2/carvalidate.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace g2::gui {

// What the UI needs from the operating system.
struct Platform {
    // Multiple selection possible; empty list = cancelled.
    //
    // startDir says where the dialog should open. Empty = Windows decides.
    std::function<std::vector<std::string>(const char* title, const char* filter, bool multi,
                                           const std::string& startDir)>
        openFiles;
    std::function<std::string(const char* title, const std::string& startDir)> pickFolder;

    // Several folders at once. Empty if the platform can't do it - then the
    // UI falls back to pickFolder.
    std::function<std::vector<std::string>(const char* title, const std::string& startDir)>
        pickFolders;
    std::function<void(const std::string& path)> revealInExplorer;

    // Save dialog: a name that does not have to exist yet. Empty = cancelled.
    // If the function is missing, the UI falls back to openFiles (only
    // sensible in tests).
    //
    // "New .car" used to use the open dialog. That only accepts existing
    // files, but newCar() rejects existing ones - so creating a new file
    // didn't work at all.
    std::function<std::string(const char* title, const char* filter, const std::string& startDir,
                              const char* defaultExt)>
        saveFile;

    // --- Updates ------------------------------------------------------------
    //
    // Without network.get there is no update check at all - the tests run
    // with a fake server here, the real one is WinHTTP in main_win32.cpp.
    update::Network network;

    // The running executable, the file an update replaces.
    std::filesystem::path exePath;

    // Version of this build. Tests pretend to be an older release.
    update::BuildInfo build = update::thisBuild();

    // Opens a web page in the browser: release notes, download page.
    std::function<void(const std::string& url)> openUrl;

    // Starts another program with one argument (ModView with a .glm).
    // false if it could not be started.
    std::function<bool(const std::string& exe, const std::string& arg)> launch;
};

// One open script - one tab.
struct Document {
    std::string  path;
    std::string  title;      // file name, shown in the tab bar
    car::Script  script;
    bool         dirty = false;
    bool         open = true;
    std::string  loadError;

    // Result of the last validation, so the display doesn't recompute it on
    // every frame.
    car::ValidateResult validation;
    bool                validated = false;

    // Row selection for multi-row actions.
    std::vector<char> selected;

    // Output location of this script. MUST be set, otherwise nothing is built.
    //
    // There is deliberately no shared folder for all of them: in a model tree
    // every output is called "_humanoid.gla", and a shared target would let
    // twenty scripts overwrite the same file - without an error message, with
    // the result that only the last one survives. A setting that destroys data
    // in most cases does not belong in the program.
    std::string outputDir;

    void syncSelection() { selected.assign(script.grabs.size(), 0); }
};

// Settings that apply to all tabs.
struct Settings {
    std::string enumPath;      // anims.h
    std::string baseDir;       // root under which models/ lives
    std::string referenceGla;  // skeleton source

    bool writeFrames = true;
    bool writeMesh = true;
    bool writeSkin = true;
    bool useCache = true;
    bool carcassCompat = false;

    // Create a .car.bak on the first save.
    //
    // Only ONCE per file - the original state before the first edit is what
    // you want back, not the one from a moment ago. Anyone who doesn't need it
    // switches it off.
    bool keepBackup = true;
    bool readFrameCounts = false;

    // Always all cores. Deliberately not a setting: the output is provably
    // bit-identical regardless of thread count, and a setting that can only be
    // set wrong isn't a setting.
    static constexpr int threads = 0;

    bool darkMode = true;

    // ModView, for "Open in ModView". Asked for on first use if empty.
    std::string modelViewPath;

    // Show start, count, loop and speed of each split part next to its name,
    // like Assimilate's "Frame Details On Additional Sequences".
    bool partDetails = false;

    // Ask GitHub for a newer version at startup. Only a check - installing
    // always needs a click. A self-built "dev" version never checks by itself.
    bool checkUpdates = true;

    // -1 = the channel this build came from (release -> stable, snapshot ->
    // snapshot), otherwise update::Channel.
    int updateChannel = -1;

    // "Skip this version": release id (tag or commit) not to offer again.
    std::string skippedUpdate;

    // Language as a number, so the settings file stays simple.
    int  language = 0;

    // Pixels per logical point, set by the window binding.
    //
    // Without this ImGui draws at 96 dpi and Windows scales the finished image
    // up - the result is blurry. The right way is to draw larger in the first
    // place: larger font, larger spacing, sharp edges.
    float dpiScale = 1.0f;
};

// One line in the log.
struct LogLine {
    enum class Kind { Info, Good, Warn, Bad };
    Kind        kind = Kind::Info;
    std::string text;
};

// State of a running batch build.
struct BuildJob {
    std::atomic<bool>        running{false};
    std::atomic<bool>        cancel{false};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> total{0};
    std::string              current;
    std::mutex               currentMutex;
    std::thread              worker;

    ~BuildJob();
    void join();

    // Explicitly non-movable and non-copyable.
    //
    // mutex and thread are anyway, so the compiler doesn't generate the
    // operations. Writing it out costs nothing and makes the intent visible
    // instead of leaving it to be inferred from the member types.
    BuildJob() = default;
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;
    BuildJob(BuildJob&&) = delete;
    BuildJob& operator=(BuildJob&&) = delete;
};

// Why an update failed, in the current language. Shared by the update bar
// and "g2c update" on the command line.
std::string describeUpdateError(const update::Status& st);

class App {
public:
    explicit App(Platform platform);
    ~App();

    // App holds a BuildJob with a running thread - copying or moving would be
    // wrong in every case.
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // One frame. Calls ImGui and returns.
    void draw();

    // --- Usable without a window too, so it can be tested -----------------

    bool openCar(const std::string& path);
    std::size_t openFolder(const std::string& root);   // returns the number of hits

    // File or folder, whichever it is. For dropping onto the window and for
    // command-line arguments at startup.
    bool openPath(const std::string& path);

    // --- Second mode: GLA back to dotXSI ----------------------------------
    //
    // Deliberately separate from building. The two directions share almost
    // nothing: different inputs, different outputs, different terms. Squeezed
    // into one shared UI you would constantly have to grey out fields that
    // make no sense in the current mode.
    enum class Mode { Build, Extract, Preview };

    struct ExtractSeq {
        std::string name;
        int         start = 0;
        int         count = 0;
        int         loopFrame = -1;
        int         fps = 20;
    };

    struct ExtractState {
        std::string glaPath;
        std::string cfgPath;

        // Path to the .frames of the source GLA. Without it the root motion
        // is missing, and a run animation runs in place after rebuilding.
        std::string framesPath;

        // Offset the source GLA was built with. Estimated and displayed on
        // opening; it must be re-applied on export, otherwise rebuilding
        // subtracts it a second time.
        std::optional<std::array<float, 3>> origin;

        // Which bind pose goes into the BASEPOSE block.
        //
        // Only matters to anyone who sends the files through Raven's Carcass;
        // g2c itself builds identically with every setting, because it
        // evaluates the FCurves and doesn't look at the block at all.
        xsiexp::ExportOptions::BasePose basePose = xsiexp::ExportOptions::BasePose::World;

        // dotXSI version of the generated files.
        //
        // v3.0 names the templates like Raven's root.xsi, v3.5 leaves them
        // unnamed like Raven's animation files. Some older tools expect 3.0.
        xsiexp::ExportOptions::Version xsiVersion = xsiexp::ExportOptions::Version::V30;

        // --- Comparison with another humanoid -----------------------------
        //
        // Sequence names of the other side, lower-cased. This answers: which
        // animations does THIS GLA have that the other one doesn't? Exactly
        // those are the ones you want to take over.
        std::string           comparePath;
        std::set<std::string> compareNames;
        bool                  onlyMissing = false;

        std::size_t             withMotion = 0;
        std::string             outDir;
        MdxaFile                gla;
        bool                    loaded = false;
        std::vector<ExtractSeq> seqs;
        std::vector<char>       selected;
        std::string             error;
    };

    Mode                mode() const { return mode_; }
    void                setMode(Mode m) { mode_ = m; }
    ExtractState&       extract() { return extract_; }
    const ExtractState& extract() const { return extract_; }

    bool        loadGlaForExtract(const std::string& path);
    bool        loadAnimationCfg(const std::string& path);
    bool        loadFramesFile(const std::string& path);

    // Tests only: set the active tab without drawing the UI.
    void setActiveForTest(int i) { active_ = i; }

    // Select a tab from outside: sets active_ AND tells ImGui on the next draw
    // which tab to show.
    void activate(int i) {
        active_ = i;
        selectTab_ = i;
    }

    // Looks for animation.cfg and .frames next to the GLA.
    //
    // They are practically always in the same folder; making the user pick
    // them one by one is work the program can do itself. The buttons remain
    // for the exception.
    void        findCompanionFiles(const std::string& glaPath);

    // Load a second animation.cfg to compare sequence names.
    bool        loadCompareCfg(const std::string& path);

    // Does the sequence exist on the other side?
    bool        existsInCompare(const std::string& name) const;

    // Select all that are missing over there. Returns the count.
    std::size_t selectMissing();
    std::size_t exportSequences(const std::vector<std::size_t>& rows, const std::string& dir);

    // Export everything AND write a .car alongside, with which the GLA can be
    // rebuilt right away. Returns the number of .xsi files written.
    std::size_t exportAllWithScript(const std::string& dir, const std::string& carPath,
                                    const std::string& xsiPrefix);
    void closeDocument(std::size_t index);

    // Appends files as $aseanimgrab. `toAll` distributes them to all tabs.
    std::size_t addXsiFiles(const std::vector<std::string>& files, bool toAll);

    void validateDocument(std::size_t index);
    void validateAll();

    bool loadEnums(const std::string& path);

    // Assign each script the folder of its own .car. Convenience without the
    // danger of a shared target: the paths are all different.
    std::size_t assignDefaultOutputs(bool onlyEmpty = true);

    // --- Reordering and deleting sequences --------------------------------
    //
    // Pure data operations, so they can be tested without a window.

    // Moves ONE sequence from `from` to `to`. The selection marks move along,
    // otherwise the selection points at the wrong row after the move.
    bool moveGrab(std::size_t docIndex, std::size_t from, std::size_t to);

    // Moves SEVERAL rows as a block in front of the row "before".
    // The selection stays on the same sequences.
    std::size_t moveGrabs(std::size_t docIndex, std::vector<std::size_t> rows,
                          std::size_t before);

    // ROOT to the end.
    //
    // root.xsi provides the base pose; in Raven's scripts that grab always
    // comes last. Inserting new animations before it is therefore not
    // cosmetic, it keeps the file the way Carcass and Assimilate expect it.
    bool keepRootLast(std::size_t docIndex);

    // Is this the ROOT grab?
    static bool isRootGrab(const car::GrabDirective& g);

    // Deletes the given rows. Processed in descending order so the indices
    // don't shift under the loop.
    //
    // keepComments: separators and comment lines above the deleted sequences
    // stay in place and then belong to the next one. False when cutting -
    // there they move into the clipboard too.
    std::size_t deleteGrabs(std::size_t docIndex, std::vector<std::size_t> rows,
                            bool keepComments = true);

    // Creates an empty, valid script and opens it as a tab.
    bool newCar(const std::string& path);

    // --- Copy and paste ---------------------------------------------------
    //
    // The clipboard holds complete grabs, including -additional, loop and
    // rate. It works across tabs: copying from one script and pasting into
    // another is the actual purpose.
    //
    // What is copied are copies, not references. If the source is deleted
    // afterwards, the clipboard stays valid.
    std::size_t copyGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows);
    std::size_t cutGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows);
    std::size_t pasteGrabs(std::size_t docIndex, std::size_t before);
    std::size_t clipboardSize() const { return clipboard_.size(); }

    void setLogPath(std::string p) { logPath_ = std::move(p); }

    // The whole log as text.
    //
    // For the clipboard. With a timestamp and program info in the header: a
    // bug report without version and system info always costs a follow-up
    // question.
    std::string logAsText() const;

    // Append all .xsi files of a folder - recursively, sorted.
    std::size_t addXsiFolder(const std::string& folder, bool toAll);

    // Frame count of a source file, if already known. -1 = not read yet,
    // -2 = not found.
    int frameCountOf(const std::string& relPath) const;

    // Frame rate from the file's SI_Scene: > 0 the rate, 0 the file has
    // none, -1 not read yet.
    int xsiRateOf(const std::string& relPath) const;

    // The framespeed the build writes into animation.cfg: -framespeed if
    // given, otherwise the SI_Scene rate, otherwise the build's default. 0 =
    // not known yet (file still being read).
    //
    // The table used to show "auto" and the dialog 0 for most sequences -
    // Assimilate showed the real number, and nobody could see how fast an
    // animation actually runs.
    int effectiveSpeed(const car::GrabDirective& g) const;

    // Start frame of every grab in the GLA, as the build lays them out. -1
    // from the first grab whose frame count is not known yet.
    std::vector<int> targetFrames(const Document& d) const;

    // --- Assimilate's "Model" dialog ---------------------------------------
    //
    // The header of the script: conversion line, $scale, $keepmotion, $pcj.
    // Assimilate edited all of it in one dialog; g2c could only read it.
    struct ModelEdit {
        car::ModelSettings  head;
        bool                haveConvert = false;
        bool                convertFromInclude = false;   // lives in another file
        std::string         root;
        bool                ownSkeleton = false;
        std::string         skeleton;
        bool                haveOrigin = false;
        std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
        double              scale = 1.0;
        bool                makeSkin = false;
        bool                loseDupVerts = false;
        bool                smooth = false;
    };
    ModelEdit modelEditOf(std::size_t docIndex) const;
    // Writes the edit into the script. true if anything changed.
    bool      applyModelEdit(std::size_t docIndex, const ModelEdit& e);
    void      openModelDialog(std::size_t docIndex);

    // Save under a new name; the tab then belongs to the new file. false if
    // another open tab already has that file, or writing fails.
    bool saveDocumentAs(std::size_t index, const std::string& path);

    // Most recently opened scripts, newest first.
    const std::deque<std::string>& recentFiles() const { return recent_; }

    // Assimilate's "Write Config Data": the full build, but only
    // animation.cfg is written - GLA, .frames and mesh stay untouched.
    void writeConfigOnly(bool allTabs) { startBuild(allTabs, true); }

    // .car files under the asset root that animate against the skeleton this
    // script makes ($aseanimgrab_gla / $aseanimref_gla). Assimilate: "Build
    // dependant models".
    std::vector<std::string> findDependents(std::size_t docIndex) const;
    // Opens them as tabs and builds them. Returns how many.
    std::size_t buildDependents(std::size_t docIndex);

    // ModView with the .glm this script builds. false (with a log line) if
    // ModView or the .glm is missing.
    bool openInModView(std::size_t docIndex);

    // Sets -framespeed on several rows at once; nullopt removes it again
    // (back to the rate from the .xsi). Returns the number of rows changed.
    std::size_t setFrameSpeed(std::size_t docIndex, const std::vector<std::size_t>& rows,
                              std::optional<int> speed);

    // Remember settings across sessions.
    // Folder for settings and window state.
    //
    // Static, because the window binding needs it before the App is even
    // constructed: the path of the imgui file must be set before ImGui draws
    // the first frame.
    static std::string configDir();

    // File/folder dialogs with their own memory.
    //
    // Each purpose remembers ITS folder: anims.h lives somewhere else than the
    // .xsi files, and those somewhere else than the output folder. A single
    // shared "last used" folder means clicking through the tree again on
    // every second dialog.
    std::vector<std::string> askFiles(const char* purpose, const char* title,
                                      const char* filter, bool multi);
    std::string              askFolder(const char* purpose, const char* title);

    // Several folders. Falls back to askFolder if the platform can't do
    // multiple selection - then at most one comes back, but never none.
    std::vector<std::string> askFolders(const char* purpose, const char* title);
    std::string settingsPath() const;
    void        saveSettings() const;
    void        loadSettings();

    // Write the script back. Creates a backup first.
    bool saveDocument(std::size_t index);
    std::size_t saveAllDocuments();

    void log(LogLine::Kind kind, std::string text);

    Settings&                      settings() { return settings_; }
    const std::vector<Document>&   documents() const { return docs_; }
    std::vector<Document>&         documents() { return docs_; }
    const anim::EnumTable&         enums() const { return enums_; }
    const std::deque<LogLine>&     logLines() const { return log_; }
    int                            activeTab() const { return active_; }

    bool buildRunning() const { return job_.running.load(); }

    // --- Closing with unsaved changes -------------------------------------
    //
    // The tab close button, Ctrl+W, "Close all" and the window close button
    // used to discard changes without asking. Now a dialog asks: save,
    // discard or cancel.

    // Closes the tabs immediately if none is modified, otherwise the dialog
    // asks on the next frame.
    void requestClose(std::vector<std::size_t> indices);

    // For the window close button. true = may quit immediately. Otherwise the
    // dialog appears, and quitApproved() reports the answer later.
    bool requestQuit();
    bool quitApproved() const { return quitApproved_; }

    // "Restart now" after an update: quit like the close button (unsaved
    // changes are asked about), then the window binding starts the new exe.
    bool restartRequested() const { return restartRequested_ && quitApproved_; }

    // Null if the platform has no network access.
    update::Updater* updater() { return updater_.get(); }
    update::Channel  updateChannel() const;
    void             checkForUpdates(bool manual);

    // The window binding polls this: for Chinese or Japanese the font atlas
    // must be rebuilt with different glyph ranges.
    bool fontsDirty() const { return fontsDirty_; }
    void clearFontsDirty() { fontsDirty_ = false; }

private:
    void drawMenuBar();
    void drawToolbar();
    void drawSettingsPanel();
    void drawTabs();
    void drawSequenceTable(Document& d);
    void drawModeBar();
    void drawExtractPanel();
    void drawPreviewPanel();
    void drawIssues();
    void drawSequenceDialog(Document& d);
    // Selection list for enums. Returns true if something was chosen.
    bool drawEnumChooser(const char* popupId, std::string& target, const Document* d = nullptr);
    void drawLog();

    // Colored icon, then text - both on one line.
    //
    // ImGui draws text in a single color, including icons inside a label.
    // Drawn separately, the icon can be colored, and that helps at a glance:
    // green means done, red means error, without having to read the text.
    //
    // Without an icon font only the text is drawn.
    static void iconText(const char* icon, const IconColor& col, const char* text);

    // Button with a COLORED icon.
    //
    // ImGui draws a button's text in a single color, including any icon in
    // it. Here the button is first drawn empty and then icon and text are
    // placed on top via the draw list - that way the icon gets its own color
    // while the frame and hover behavior stay ImGui's.
    //
    // Without an icon font only the text is drawn, and the button is
    // correspondingly narrower.
    static bool iconButton(const char* icon, const IconColor& col, const char* text,
                           bool small = false);
    void drawStatusBar();
    void applyStyle();
    void refreshTabTitles();

    void startBuild(bool allTabs, bool cfgOnly = false);
    void startBuildIndices(std::vector<std::size_t> which, bool cfgOnly);
    void drawModelDialog();
    void addRecent(const std::string& path);

    // Runs in the worker thread. Gets copies of the document AND the settings:
    // the UI stays usable during the build, and a field being typed into must
    // not be read by the thread at the same time.
    void buildOne(const Document& d, const Settings& st, bool cfgOnly = false);

    void drawCloseDialog();
    void drawSpeedDialog();
    void drawUpdateBanner();
    void drawUpdateSettings();
    void drawOverwriteDialog();
    void closeEditorOf(std::size_t docIndex);
    void newCarDialog();
    void openScriptDialog();
    void openFolderDialog();
    void handleShortcuts();

    // Write the file, true on success. If it fails, the reason is in the log -
    // the output then does not count as built.
    bool writeOutput(const std::string& title, const std::string& path, const std::string& data);

    // Export with confirmation if files would be overwritten.
    void exportWithConfirm(std::vector<std::size_t> rows, bool withCar);
    void runExport(const std::vector<std::size_t>& rows, bool withCar);

    // Table rows the filter currently shows.
    bool rowVisible(const Document& d, std::size_t i) const;
    std::string askSaveFile(const char* purpose, const char* title, const char* filter,
                            const char* defaultExt);

    Platform             platform_;
    Settings             settings_;
    std::vector<Document> docs_;
    anim::EnumTable      enums_;
    std::deque<LogLine>  log_;
    // mutable so that logAsText() can be const: locking doesn't change the
    // visible state.
    mutable std::mutex   logMutex_;
    int                  active_ = 0;
    // Tab that ImGui should select on the next draw; -1 = none.
    int                  selectTab_ = -1;
    bool                 showSettings_ = true;
    Mode                 mode_ = Mode::Build;

    // --- Preview -----------------------------------------------------------
    //
    // Uses the GLA loaded in GLA -> XSI mode. Loading it a second time would
    // be wasteful: the file is 15 MB and is already in memory.
    PreviewCamera        previewCam_;
    Playback             playback_;
    int                  previewSeq_ = -1;
    double               previewLastTime_ = 0.0;
    ExtractState         extract_;
    char                 extractFilter_[128] = {0};
    int                  extractAnchor_ = -1;

    bool                 showAbout_ = false;

    // Path of the startup log, set by the platform layer.
    //
    // The UI doesn't determine it itself: only main_win32.cpp knows where the
    // exe lives, and the information must match the file that is actually
    // written there.
    std::string          logPath_;

    // Output folder written last, for the button in the log.
    // The build thread writes it, the UI reads it: only touch it under
    // outputDirMutex_.
    std::string          lastOutputDir_;
    mutable std::mutex   outputDirMutex_;

    // Confirmation on close: paths of the affected tabs.
    std::vector<std::string> pendingClose_;
    bool                     quitRequested_ = false;
    bool                     quitApproved_ = false;
    bool                     restartRequested_ = false;

    // Model dialog: the script it belongs to and the edit in progress.
    bool                     modelOpen_ = false;
    std::string              modelDocPath_;
    ModelEdit                modelEdit_;
    char                     pcjInput_[128] = {0};
    int                      pcjSel_ = -1;

    std::deque<std::string>  recent_;

    // Animation picker: category (0 = all) and "hide used".
    int                      enumCategory_ = 0;
    bool                     enumHideUsed_ = false;

    // "Set framespeed" for several rows: which rows of which script.
    std::vector<std::size_t> speedRows_;
    std::string              speedDocPath_;
    int                      speedValue_ = 20;

    // Created in the constructor when the platform offers network access.
    std::unique_ptr<update::Updater> updater_;
    // "Later": no banner for the rest of this session.
    bool                             updateBannerHidden_ = false;

    // Confirmation before overwriting on export.
    struct PendingExport {
        std::vector<std::size_t> rows;
        bool                     withCar = false;
        std::vector<std::string> existing;
        bool                     active = false;
    };
    PendingExport pendingExport_;

    // Which script do the open sequence dialog and the delete confirmation
    // belong to? Both used to refer to the active tab - anyone who switched
    // tabs in between edited or deleted in the wrong script.
    std::string editDocPath_;
    std::string pendingDeleteDocPath_;
    bool        pendingDeleteIsCut_ = false;
    bool                 styleApplied_ = false;
    bool                 fontsDirty_ = false;
    char                 filter_[128] = {0};
    BuildJob             job_;

    // Selected message -> jump into the table.
    int  jumpToRow_ = -1;
    int  issueSelected_ = -1;

    // Open edit dialog: index of the grab, -1 = none.
    // Deferred paste: only executed after the table.
    long pendingPaste_ = -1;

    // Clipboard for sequences, across tabs.
    std::vector<car::GrabDirective> clipboard_;

    // Anchor row for range selection.
    int  selAnchor_ = -1;

    // Drag-selecting with the mouse button held, like in Explorer.
    //
    // Whether it's "drag a selection" or "move rows" is decided on press: on
    // an ALREADY selected row a move starts, on any other row a new
    // selection. That's exactly how Explorer behaves, which is why nobody has
    // to have it explained.
    bool rangeSelecting_ = false;

    // Deferred reordering: only executed after the table.
    //
    // splitAt: this many comment lines of the grab being inserted before
    // (counted from the top) move over to the inserted sequence. Anyone who
    // drags a sequence under a heading - onto the line between heading and
    // row - wants it below the heading, not above it.
    struct PendingBlock {
        std::vector<std::size_t> rows;
        std::size_t              before = 0;
        std::size_t              splitAt = 0;
    };
    PendingBlock pendingBlock_;
    // Rows whose deletion still has to be confirmed.
    std::vector<std::size_t> pendingDelete_;

    int  editRow_ = -1;

    // Which comment line is currently being edited.
    //
    // Two numbers, because a sequence can have several comment lines: which
    // grab, and which line within it. -1 means: none.
    int  editCommentGrab_ = -1;
    int  editCommentLine_ = -1;
    char editCommentBuf_[512] = {};

    // Which trailing line comment is currently being edited. -1 means: none.
    // A separator that was just dropped: from where, to where.
    //
    // As with sequences, the move is executed AFTER the table. Modifying the
    // list that is being iterated over in the middle of drawing is the
    // classic road to a crash.
    //
    // The target is a position in a comment list: zuGrab == number of grabs
    // means the lines after the last animation, zuZeile == kAnhaengen means
    // the end of the list.
    static constexpr std::size_t kAnhaengen = static_cast<std::size_t>(-1);
    struct PendingCommentMove {
        std::size_t vonGrab = 0;
        std::size_t vonZeile = 0;
        std::size_t zuGrab = 0;
        std::size_t zuZeile = kAnhaengen;
        bool        aktiv = false;
    };
    PendingCommentMove pendingCommentMove_;

    // Which message is currently being clicked through, and which occurrence.
    // For a duplicate name each click jumps to the next one.
    // Duplicate names from the last build attempt.
    //
    // buildOne gets the document read-only - so the messages are entered by
    // the caller, which is allowed to modify it.
    // The build runs in a thread with copies of the documents; so the main
    // thread enters the messages while drawing. The mutex protects the
    // hand-over.
    std::mutex               dupMutex_;
    std::vector<std::string> pendingDuplicates_;
    std::string              pendingDupDoc_;

    int  issueCycleFor_ = -1;
    int  issueCycleIdx_ = 0;

    int  editTrailGrab_ = -1;
    // The input field of an edit that was just started should get focus -
    // once, on the next draw.
    bool focusEditField_ = false;
    char editTrailBuf_[256] = {};

public:
    // Move a comment line to a different position.
    //
    // Technically, comments are stored as "commentsBefore" on the following
    // grab - that's what the file format requires, and that way the text ends
    // up in the right place in animation.cfg when building.
    //
    // For the user, though, a separator should behave like an element of its
    // own that can be moved independently of the animations. This function
    // detaches it from one grab and attaches it to the other.
    //
    // "nachOben" (upwards) moves it up one row in the display: within the same
    // grab, or to the end of the comment block before it.
    bool moveComment(std::size_t doc, std::size_t grab, std::size_t line, bool nachOben);

private:
    bool editOpen_ = false;
    char enumFilter_[128] = {0};

    // Remembered output locations per .car path, loaded from the settings.
    std::map<std::string, std::string> savedOutputs_;

    // Tabs open last time, restored at startup.
    // Last used folder per purpose.
    std::map<std::string, std::string> lastDirs_;

    std::vector<std::string> savedTabs_;
    int                      savedActive_ = 0;

    // --- Frame counts in the background -----------------------------------
    //
    // Reading 1289 files in the foreground would freeze the window for
    // seconds. So it runs alongside and the column fills in bit by bit. A
    // button for it would be needless work for the user - you always want to
    // see the number.
    mutable std::mutex          frameMutex_;
    // What the background reader found in each .xsi.
    struct XsiInfo {
        int frames = -2;   // -2 = not found / unreadable
        int rate = 0;      // SI_Scene frame rate, 0 = none in the file
    };
    std::map<std::string, XsiInfo> xsiInfo_;   // relative path -> info
    std::atomic<bool>           frameWorkerRunning_{false};
    std::atomic<bool>           frameWorkerStop_{false};
    std::thread                 frameWorker_;
    std::string                 frameWorkerBaseDir_;

    void startFrameWorker(const Document& d);
    void stopFrameWorker();
};

}  // namespace g2::gui
