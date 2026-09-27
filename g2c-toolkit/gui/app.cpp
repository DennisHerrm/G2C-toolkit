#include "gui/app.h"
#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/preview.h"

#include "g2/mdxa.h"
#include "g2/xsi_export.h"
#include "g2/parallel.h"
#include "g2/mdxm.h"
#include "g2/readfile.h"
#include "g2/sidefiles.h"
#include "g2/xsi_mesh.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace g2::gui {
namespace fs = std::filesystem;

BuildJob::~BuildJob() { join(); }

void BuildJob::join() {
    cancel.store(true);
    if (worker.joinable()) worker.join();
    running.store(false);
}

App::App(Platform platform) : platform_(std::move(platform)) {
    loadSettings();

    // Restore the scripts that were open last time. Missing ones are silently
    // skipped - an avalanche of errors at startup would be more annoying than
    // the missing tab.
    std::size_t restored = 0;
    for (const auto& t : savedTabs_) {
        std::error_code ec;
        if (!fs::exists(t, ec)) continue;
        if (openCar(t)) ++restored;
    }
    if (restored) {
        if (savedActive_ >= 0 && savedActive_ < static_cast<int>(docs_.size()))
            activate(savedActive_);
        log(LogLine::Kind::Good, trf(S::LogRestored, restored));
    } else {
        log(LogLine::Kind::Info, tr(S::LogReady));
    }

    if (platform_.network.get) {
        updater_ = std::make_unique<update::Updater>(platform_.network, platform_.build,
                                                     platform_.exePath);
        if (updater_->justUpdated())
            log(LogLine::Kind::Good, trf(S::UpdDone, update::displayName(platform_.build).c_str()));
        // A self-built version never asks on its own: it has no version
        // number to compare, and a developer's build must not be offered a
        // replacement on every start.
        const bool autoCheck = settings_.checkUpdates &&
                               update::kindOf(platform_.build) != update::BuildKind::Dev;
        updater_->startup(autoCheck, updateChannel());
    }
}

update::Channel App::updateChannel() const {
    if (settings_.updateChannel == static_cast<int>(update::Channel::Stable))
        return update::Channel::Stable;
    if (settings_.updateChannel == static_cast<int>(update::Channel::Snapshot))
        return update::Channel::Snapshot;
    return update::defaultChannel(platform_.build);
}

void App::checkForUpdates(bool manual) {
    if (!updater_) return;
    updateBannerHidden_ = false;
    updater_->check(updateChannel(), manual);
}

App::~App() {
    stopFrameWorker();
    job_.join();
    saveSettings();
}

// --- Settings across sessions ----------------------------------------------
//
// Simple key=value format next to the executable. No INI parser, no JSON:
// it's ten values, and a dedicated format would cost more than it's worth here.

// Where the settings live.
//
// Not in the working directory: that depends on HOW the program was started.
// Via double-click it's the exe's folder, from a shortcut it's the shortcut's
// working directory, from the command prompt some other one. The settings were
// therefore sometimes there and sometimes gone.
//
// Not under "Documents" either: that folder belongs to the user for their own
// files. Programs that store their configuration there clutter it up. Games
// store savegames there because people want to back them up and share them -
// nobody wants that for window positions and checkboxes.
//
// The right place is %APPDATA%\g2c\. Anyone who prefers it portable - USB stick,
// network drive, several branches side by side - puts an empty file
// "g2c_portable.txt" next to the exe; then everything lives next to it.
// File dialog that remembers the last folder per purpose.
std::vector<std::string> App::askFiles(const char* purpose, const char* title,
                                       const char* filter, bool multi) {
    if (!platform_.openFiles) return {};
    const auto it = lastDirs_.find(purpose);
    const auto out = platform_.openFiles(title, filter, multi,
                                         it == lastDirs_.end() ? std::string() : it->second);
    if (!out.empty()) {
        std::error_code ec;
        const fs::path p = fs::path(out.front()).parent_path();
        if (!p.empty()) lastDirs_[purpose] = p.string();
    }
    return out;
}

std::string App::askSaveFile(const char* purpose, const char* title, const char* filter,
                             const char* defaultExt) {
    const auto it = lastDirs_.find(purpose);
    const std::string start = it == lastDirs_.end() ? std::string() : it->second;
    std::string out;
    if (platform_.saveFile) {
        out = platform_.saveFile(title, filter, start, defaultExt);
    } else if (platform_.openFiles) {
        const auto f = platform_.openFiles(title, filter, false, start);
        if (!f.empty()) out = f.front();
    }
    if (!out.empty()) {
        const fs::path p = fs::path(out).parent_path();
        if (!p.empty()) lastDirs_[purpose] = p.string();
    }
    return out;
}

// These three actions exist in the menu, in the toolbar and as keyboard
// shortcuts. All paths call the same function - previously there were three
// copies, and the keyboard shortcuts were missing entirely.
void App::newCarDialog() {
    std::string p = askSaveFile("car", tr(S::DlgTitleNewCar), "Carcass-Skript (*.car)\0*.car\0",
                                "car");
    if (p.empty()) return;
    if (fs::path(p).extension().empty()) p += ".car";
    newCar(p);
}

void App::openScriptDialog() {
    for (const auto& f :
         askFiles("car", tr(S::DlgTitleCar), "Carcass-Skript (*.car)\0*.car\0Alle\0*.*\0", true))
        openCar(f);
}

void App::openFolderDialog() {
    const std::string dir = askFolder("carfolder", tr(S::DlgTitleCarFolder));
    if (!dir.empty()) openFolder(dir);
}

std::string App::askFolder(const char* purpose, const char* title) {
    if (!platform_.pickFolder) return {};
    const auto it = lastDirs_.find(purpose);
    std::string out =
        platform_.pickFolder(title, it == lastDirs_.end() ? std::string() : it->second);
    if (!out.empty()) lastDirs_[purpose] = out;
    return out;
}

bool App::moveComment(std::size_t doc, std::size_t grab, std::size_t line, bool nachOben) {
    if (doc >= docs_.size()) return false;
    Document& d = docs_[doc];
    if (grab >= d.script.grabs.size()) return false;
    auto& von = d.script.grabs[grab].commentsBefore;
    if (line >= von.size()) return false;

    const std::string text = von[line];

    if (nachOben) {
        if (line > 0) {
            // Swap within the same block.
            std::swap(von[line], von[line - 1]);
        } else {
            // Append to the previous grab - at its end, so that the separator
            // ends up exactly one line higher in the display.
            if (grab == 0) return false;
            von.erase(von.begin() + static_cast<long>(line));
            d.script.grabs[grab - 1].commentsBefore.push_back(text);
        }
    } else {
        if (line + 1 < von.size()) {
            std::swap(von[line], von[line + 1]);
        } else {
            if (grab + 1 >= d.script.grabs.size()) {
                // After the LAST animation. There is no grab left there that
                // the separator could be attached to - that's what
                // trailingComments is for.
                von.erase(von.begin() + static_cast<long>(line));
                d.script.trailingComments.push_back(text);
            } else {
                // At the end: move to the next grab, at its beginning.
                von.erase(von.begin() + static_cast<long>(line));
                auto& zu = d.script.grabs[grab + 1].commentsBefore;
                zu.insert(zu.begin(), text);
            }
        }
    }
    d.dirty = true;
    d.validated = false;
    return true;
}

std::vector<std::string> App::askFolders(const char* purpose, const char* title) {
    const auto it = lastDirs_.find(purpose);
    const std::string start = it == lastDirs_.end() ? std::string() : it->second;

    std::vector<std::string> out;
    if (platform_.pickFolders) {
        out = platform_.pickFolders(title, start);
    } else if (platform_.pickFolder) {
        // Fallback: one is better than none.
        std::string one = platform_.pickFolder(title, start);
        if (!one.empty()) out.push_back(std::move(one));
    }
    if (!out.empty()) lastDirs_[purpose] = out.front();
    return out;
}

std::string App::configDir() {
    std::error_code ec;

    // Portable mode: marker file next to the exe.
    const fs::path here = fs::current_path(ec);
    if (!ec && fs::exists(here / "g2c_portable.txt", ec)) return here.string();

    std::string appdata = envValue("APPDATA");
    if (appdata.empty()) appdata = envValue("HOME");   // for tests under Linux
    if (appdata.empty()) return here.string();

    const fs::path dir = fs::path(appdata) / "g2c";
    fs::create_directories(dir, ec);
    if (ec) return here.string();
    return dir.string();
}

std::string App::settingsPath() const {
    const fs::path target = fs::path(configDir()) / "g2c_settings.txt";

    // One-time migration from the old location, so nobody loses their
    // settings.
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        const fs::path old = fs::current_path(ec) / "g2c_settings.txt";
        if (!ec && fs::exists(old, ec) && old != target) fs::copy_file(old, target, ec);
    }
    return target.string();
}

void App::saveSettings() const {
    // Build it completely in memory first, then replace in one go. If the
    // program crashes on exit, the old file stays whole instead of half-written.
    std::ostringstream f;
    f << "basedir=" << settings_.baseDir << "\n";
    f << "refgla=" << settings_.referenceGla << "\n";
    f << "enums=" << settings_.enumPath << "\n";
    f << "frames=" << (settings_.writeFrames ? 1 : 0) << "\n";
    f << "mesh=" << (settings_.writeMesh ? 1 : 0) << "\n";
    f << "skin=" << (settings_.writeSkin ? 1 : 0) << "\n";
    f << "cache=" << (settings_.useCache ? 1 : 0) << "\n";
    f << "carcass=" << (settings_.carcassCompat ? 1 : 0) << "\n";
    f << "backup=" << (settings_.keepBackup ? 1 : 0) << "\n";
    f << "readframes=" << (settings_.readFrameCounts ? 1 : 0) << "\n";
    f << "dark=" << (settings_.darkMode ? 1 : 0) << "\n";
    f << "lang=" << settings_.language << "\n";
    f << "updates=" << (settings_.checkUpdates ? 1 : 0) << "\n";
    f << "updchannel=" << settings_.updateChannel << "\n";
    if (!settings_.skippedUpdate.empty()) f << "updskip=" << settings_.skippedUpdate << "\n";

    // The bind pose choice belongs to the export, not to the script - so it is
    // kept across sessions. Anyone who had to change it once doesn't want to
    // repeat that on every start.
    f << "xsiver=" << (extract_.xsiVersion == xsiexp::ExportOptions::Version::V30 ? 30 : 35)
      << "\n";
    f << "basepose=" << (extract_.basePose == xsiexp::ExportOptions::BasePose::World   ? 0
                         : extract_.basePose == xsiexp::ExportOptions::BasePose::Local ? 1
                                                                                       : 2)
      << "\n";

    // Output locations per script. That way the mapping is kept even if the
    // tabs are opened differently on the next start.
    for (const auto& d : docs_)
        if (!d.outputDir.empty()) f << "out:" << d.path << "=" << d.outputDir << "\n";

    // Open tabs and the active one. On the next start the same state is back -
    // with twenty scripts that's the difference between "keep working" and
    // "first reopen everything".
    f << "active=" << active_ << "\n";
    for (const auto& d : docs_) f << "tab=" << d.path << "\n";

    // Last used folder per file dialog.
    for (const auto& [k, v] : lastDirs_) f << "dir:" << k << "=" << v << "\n";

    try {
        writeFileChecked(settingsPath(), f.str());
    } catch (const std::exception&) {
        // Settings are a convenience. On exit there is nobody left to show
        // the error to.
    }
}

void App::loadSettings() {
    std::ifstream f(settingsPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        const auto asBool = [&] { return val == "1"; };

        if (key == "basedir") settings_.baseDir = val;
        else if (key == "refgla") settings_.referenceGla = val;
        else if (key == "enums") settings_.enumPath = val;
        else if (key == "frames") settings_.writeFrames = asBool();
        else if (key == "mesh") settings_.writeMesh = asBool();
        else if (key == "skin") settings_.writeSkin = asBool();
        else if (key == "cache") settings_.useCache = asBool();
        else if (key == "carcass") settings_.carcassCompat = asBool();
        else if (key == "backup") settings_.keepBackup = asBool();
        else if (key == "xsiver")
            extract_.xsiVersion = std::atoi(val.c_str()) == 35
                                      ? xsiexp::ExportOptions::Version::V35
                                      : xsiexp::ExportOptions::Version::V30;
        else if (key == "basepose") {
            const int v = std::atoi(val.c_str());
            extract_.basePose = v == 1   ? xsiexp::ExportOptions::BasePose::Local
                                : v == 2 ? xsiexp::ExportOptions::BasePose::None
                                         : xsiexp::ExportOptions::BasePose::World;
        }
        else if (key == "readframes") settings_.readFrameCounts = asBool();
        else if (key == "dark") settings_.darkMode = asBool();
        else if (key == "updates") settings_.checkUpdates = asBool();
        else if (key == "updchannel") {
            const int v = std::atoi(val.c_str());
            settings_.updateChannel = v == 0 || v == 1 ? v : -1;
        }
        else if (key == "updskip") settings_.skippedUpdate = val;
        else if (key == "lang") {
            try {
                settings_.language = std::stoi(val);
                setLanguage(static_cast<Lang>(settings_.language));
            } catch (...) {
            }
        }
        // "threads" is deliberately ignored: older settings files might
        // contain a limit that is no longer supposed to exist.
        else if (key.rfind("out:", 0) == 0) savedOutputs_[key.substr(4)] = val;
        else if (key == "tab") savedTabs_.push_back(val);
        else if (key.rfind("dir:", 0) == 0) lastDirs_[key.substr(4)] = val;
        else if (key == "active") { try { savedActive_ = std::stoi(val); } catch (...) {} }
    }
    if (!settings_.enumPath.empty()) {
        try {
            enums_ = anim::parseEnumHeaderFile(settings_.enumPath);
        } catch (const std::exception&) {
        }
    }
}

void App::log(LogLine::Kind kind, std::string text) {
    std::lock_guard<std::mutex> lock(logMutex_);
    log_.push_back(LogLine{kind, std::move(text)});
    // Don't let it grow without bound - a batch build over hundreds of scripts
    // would otherwise quickly pile up more than anyone reads.
    while (log_.size() > 2000) log_.pop_front();
}

// --- Documents -------------------------------------------------------------

bool App::openCar(const std::string& path) {
    for (std::size_t i = 0; i < docs_.size(); ++i) {
        if (docs_[i].path == path) {
            activate(static_cast<int>(i));
            log(LogLine::Kind::Info, trf(S::LogAlreadyOpen, path.c_str()));
            return true;
        }
    }

    Document d;
    d.path = path;
    d.title = fs::path(path).filename().string();
    // The final title is produced below in refreshTabTitles: for identical
    // file names the folder decides.
    try {
        car::ParseOptions po;
        po.followIncludes = false;
        d.script = car::parseFile(path, po);
    } catch (const std::exception& e) {
        d.loadError = e.what();
        log(LogLine::Kind::Bad, d.title + ": " + d.loadError);
        docs_.push_back(std::move(d));
        activate(static_cast<int>(docs_.size()) - 1);
        return false;
    }
    d.syncSelection();
    if (const auto it = savedOutputs_.find(path); it != savedOutputs_.end())
        d.outputDir = it->second;

    // Derive the asset root and reference GLA from the location of the .car,
    // as long as nothing is configured.
    //
    // Without a root not a single path can be resolved: the .car says
    // "models/players/...", and only the folder tree knows where that starts.
    // The command line has long done this on drag-and-drop; not doing it here
    // meant making the user search by hand for something the program already
    // knows.
    if (settings_.baseDir.empty() || settings_.referenceGla.empty()) {
        const auto guess = car::guessPaths(d.script, path);
        if (settings_.baseDir.empty() && !guess.baseDir.empty()) {
            settings_.baseDir = guess.baseDir;
            log(LogLine::Kind::Good, trf(S::LogAssetRootFound, guess.baseDir.c_str()));
        }
        if (settings_.referenceGla.empty() && !guess.referenceGla.empty()) {
            settings_.referenceGla = guess.referenceGla;
            log(LogLine::Kind::Good, trf(S::LogRefGlaFound, guess.referenceGla.c_str()));
        }
    }
    log(LogLine::Kind::Good, trf(S::LogOpened, d.title.c_str(), d.script.grabs.size()));
    docs_.push_back(std::move(d));
    activate(static_cast<int>(docs_.size()) - 1);
    refreshTabTitles();
    return true;
}

// Unique tab titles.
//
// In a model tree all scripts are called "_humanoid.car" - twenty tabs with
// the same text are worthless. They can be told apart by their folder, and
// that is exactly what becomes the title. Only where the file name is already
// unique does it stay.
void App::refreshTabTitles() {
    std::map<std::string, int> nameCount;
    for (const auto& d : docs_) nameCount[fs::path(d.path).filename().string()]++;

    for (auto& d : docs_) {
        const fs::path p(d.path);
        const std::string file = p.filename().string();
        if (nameCount[file] <= 1) {
            d.title = file;
            continue;
        }
        const std::string parent = p.parent_path().filename().string();
        d.title = parent.empty() ? file : parent;
    }
}

// --- Second mode: GLA back to dotXSI ---------------------------------------

bool App::loadGlaForExtract(const std::string& path) {
    try {
        extract_.gla = readMdxa(readWholeFileBytes(path));
        extract_.glaPath = path;
        extract_.loaded = true;
        extract_.error.clear();
        extract_.seqs.clear();
        extract_.selected.clear();
        extract_.framesPath.clear();
        extract_.cfgPath.clear();
        extract_.withMotion = 0;

        // Detect -origin. If it isn't put back in on export, rebuilding
        // subtracts it a SECOND time, and the model ends up 24 units off.
        extract_.origin = xsiexp::detectOrigin(extract_.gla);

        // Without animation.cfg there is only one block. Better than nothing:
        // at least you can see that the file was read.
        ExtractSeq all;
        all.name = fs::path(path).stem().string();
        all.start = 0;
        all.count = extract_.gla.numFrames;
        extract_.seqs.push_back(all);
        extract_.selected.assign(1, 0);

        log(LogLine::Kind::Good,
            trf(S::LogGlaOpened, fs::path(path).filename().string().c_str(),
                extract_.gla.numFrames, extract_.gla.skeleton.bones.size()));

        findCompanionFiles(path);
        return true;
    } catch (const std::exception& e) {
        extract_.loaded = false;
        extract_.error = e.what();
        log(LogLine::Kind::Bad, trf(S::LogGlaUnreadable, e.what()));
        return false;
    }
}

bool App::loadFramesFile(const std::string& path) {
    if (!extract_.loaded) return false;
    std::size_t n = 0;
    for (const auto& s : extract_.seqs)
        if (xsiexp::readAverageVec(path, s.name)) ++n;
    extract_.framesPath = path;
    extract_.withMotion = n;
    log(LogLine::Kind::Good,
        trf(S::LogFramesLoaded, n, fs::path(path).filename().string().c_str()));
    return true;
}

void App::findCompanionFiles(const std::string& glaPath) {
    std::error_code ec;
    const fs::path dir = fs::path(glaPath).parent_path();
    const std::string stem = fs::path(glaPath).stem().string();
    if (dir.empty() || !fs::is_directory(dir, ec)) return;

    // Ordered by frequency: Raven's name first, then our own output name,
    // then the last resort "exactly one file of this kind in the folder".
    const auto pick = [&](const std::vector<std::string>& names, const char* ext) -> std::string {
        for (const auto& n : names) {
            const fs::path p = dir / n;
            if (fs::exists(p, ec)) return p.string();
        }
        // Exactly one? Then that's the one meant. Several? Then don't guess.
        std::string found;
        int count = 0;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return {};
        for (const auto& e : it) {
            std::error_code eec;
            if (!e.is_regular_file(eec)) continue;
            std::string x = e.path().extension().string();
            std::transform(x.begin(), x.end(), x.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (x != ext) continue;
            found = e.path().string();
            ++count;
        }
        return count == 1 ? found : std::string();
    };

    const std::string cfg =
        pick({"animation.cfg", stem + "_animation.cfg", stem + ".cfg"}, ".cfg");
    if (!cfg.empty()) {
        if (loadAnimationCfg(cfg))
            log(LogLine::Kind::Good, trf(S::LogFound, fs::path(cfg).filename().string().c_str()));
    } else {
        log(LogLine::Kind::Warn, tr(S::LogNoCfgNearby));
    }

    // The .frames is optional: it only contains the root motion, and few
    // sequences have that anyway.
    const std::string fr = pick({stem + ".frames", "animation.frames"}, ".frames");
    if (!fr.empty() && loadFramesFile(fr))
        log(LogLine::Kind::Good, trf(S::LogFound, fs::path(fr).filename().string().c_str()));
}

namespace {
// Make sequence names comparable: capitalization and surrounding whitespace
// are not consistent in animation.cfg files.
std::string seqKey(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}
}  // namespace

bool App::loadCompareCfg(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        log(LogLine::Kind::Bad, trf(S::LogCannotRead, path.c_str()));
        return false;
    }
    std::set<std::string> names;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '/') continue;
        std::istringstream is(line);
        std::string nm;
        int a = 0, b = 0, c = 0, d = 0;
        if (!(is >> nm >> a >> b >> c >> d)) continue;
        names.insert(seqKey(nm));
    }
    if (names.empty()) {
        log(LogLine::Kind::Warn, trf(S::LogNoSeqIn, path.c_str()));
        return false;
    }
    extract_.compareNames = std::move(names);
    extract_.comparePath = path;

    char msg[256];
    std::snprintf(msg, sizeof(msg), tr(S::CompareLoaded),
                  fs::path(path).filename().string().c_str(), extract_.compareNames.size());
    log(LogLine::Kind::Good, msg);

    std::size_t fehlt = 0;
    for (const auto& s : extract_.seqs)
        if (!existsInCompare(s.name)) ++fehlt;
    std::snprintf(msg, sizeof(msg), tr(S::CompareCount), fehlt);
    log(LogLine::Kind::Info, msg);
    return true;
}

bool App::existsInCompare(const std::string& name) const {
    return extract_.compareNames.count(seqKey(name)) != 0;
}

std::size_t App::selectMissing() {
    if (extract_.compareNames.empty()) return 0;
    std::fill(extract_.selected.begin(), extract_.selected.end(), char{0});
    std::size_t n = 0;
    for (std::size_t i = 0; i < extract_.seqs.size(); ++i)
        if (!existsInCompare(extract_.seqs[i].name)) {
            extract_.selected[i] = 1;
            ++n;
        }
    return n;
}

bool App::loadAnimationCfg(const std::string& path) {
    if (!extract_.loaded) {
        log(LogLine::Kind::Warn, tr(S::LogOpenGlaFirst));
        return false;
    }
    std::ifstream f(path);
    if (!f) {
        log(LogLine::Kind::Bad, trf(S::LogCannotRead, path.c_str()));
        return false;
    }

    std::vector<ExtractSeq> out;
    std::size_t skipped = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '/') continue;
        std::istringstream is(line);
        ExtractSeq s;
        if (!(is >> s.name >> s.start >> s.count >> s.loopFrame >> s.fps)) continue;

        // Skip sequences outside the file instead of showing them: they
        // couldn't be exported anyway, and in the list they would look
        // usable.
        if (s.count <= 0 || s.start < 0 || s.start + s.count > extract_.gla.numFrames) {
            ++skipped;
            continue;
        }
        out.push_back(std::move(s));
    }

    // Not a single matching sequence: the cfg belongs to a different GLA.
    // Then keep the current list - an empty list crashed the preview, and
    // nothing could be exported with it anyway.
    if (out.empty()) {
        log(LogLine::Kind::Bad, trf(S::LogNoSeqIn, path.c_str()));
        return false;
    }

    extract_.seqs = std::move(out);
    extract_.selected.assign(extract_.seqs.size(), 0);
    extract_.cfgPath = path;
    const std::string rest = skipped ? trf(S::LogSeqsSkipped, skipped) : std::string();
    log(skipped ? LogLine::Kind::Warn : LogLine::Kind::Good,
        trf(S::LogSeqsRead, extract_.seqs.size(), rest.c_str()));
    return true;
}

std::size_t App::exportSequences(const std::vector<std::size_t>& rows, const std::string& dir) {
    if (!extract_.loaded || rows.empty()) return 0;

    std::error_code ec;
    fs::create_directories(dir, ec);

    xsiexp::ExportOptions opt;
    opt.scale = extract_.gla.skeleton.scale > 0.0f ? extract_.gla.skeleton.scale : 0.64f;
    opt.origin = extract_.origin;
    opt.basePose = extract_.basePose;
    opt.version = extract_.xsiVersion;

    std::size_t ok = 0, failed = 0;
    for (const std::size_t r : rows) {
        if (r >= extract_.seqs.size()) continue;
        const ExtractSeq& s = extract_.seqs[r];
        opt.fps = s.fps > 0 ? s.fps : 20;
        // The .frames first, if there is one. If it's missing - or this
        // sequence is missing from it - the root motion can be reconstructed
        // from the GLA itself: it is stored there as a linear ramp on the
        // root bone. Checked against Raven's own _humanoid.frames: the value
        // matches for all 178 sequences with motion.
        //
        // Previously the value was then read from the .frames a second time
        // and overwrote the reconstruction with "nothing" whenever the
        // sequence was missing there.
        opt.rootMotionPerFrame =
            extract_.framesPath.empty()
                ? std::nullopt
                : xsiexp::readAverageVec(extract_.framesPath, s.name);
        if (!opt.rootMotionPerFrame)
            opt.rootMotionPerFrame =
                xsiexp::detectRootMotion(extract_.gla, {s.name, s.start, s.count});
        try {
            const std::string text =
                xsiexp::exportSequence(extract_.gla, {s.name, s.start, s.count}, opt);
            std::string low = s.name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            writeFileChecked((fs::path(dir) / (low + ".xsi")).string(), text);
            ++ok;
        } catch (const std::exception& e) {
            if (failed < 3) log(LogLine::Kind::Bad, s.name + ": " + e.what());
            ++failed;
        }
    }
    char msg[128];
    std::snprintf(msg, sizeof(msg), tr(S::Exported), ok);
    log(failed ? LogLine::Kind::Warn : LogLine::Kind::Good, msg);
    return ok;
}

std::size_t App::exportAllWithScript(const std::string& dir, const std::string& carPath,
                                     const std::string& xsiPrefix) {
    if (!extract_.loaded || extract_.seqs.empty()) return 0;

    // Merge sub-ranges: if every sequence were exported as its own file, the
    // new GLA would have more frames than the old one and the animation.cfg
    // would no longer match.
    std::vector<xsiexp::CfgSequence> in;
    for (const auto& s : extract_.seqs) in.push_back({s.name, s.start, s.count, s.loopFrame, s.fps});
    const auto g = xsiexp::groupSequences(in);

    char msg[160];
    std::snprintf(msg, sizeof(msg), tr(S::Grouped), g.masters.size(),
                  in.size() - g.masters.size() - g.partial);
    log(LogLine::Kind::Info, msg);
    if (g.partial) {
        std::snprintf(msg, sizeof(msg), tr(S::PartialWarn), g.partial);
        log(LogLine::Kind::Warn, msg);
    }

    // Write only the masters. The row index has to match the sequence list,
    // so it is looked up again by name.
    std::vector<std::size_t> rows;
    for (const auto& m : g.masters)
        for (std::size_t i = 0; i < extract_.seqs.size(); ++i)
            if (extract_.seqs[i].name == m.self.name && extract_.seqs[i].start == m.self.start) {
                rows.push_back(i);
                break;
            }

    const std::size_t n = exportSequences(rows, dir);

    // The offset MUST be the same as on export, otherwise rebuilding doesn't
    // subtract it again.
    // Skeleton path from the GLA header: it holds exactly the value Carcass
    // was given as -makeskel back then.
    const auto sc = xsiexp::buildScript(g, xsiPrefix, extract_.origin,
                                        extract_.gla.skeleton.scale, false,
                                        extract_.gla.skeleton.name);
    try {
        // An existing .car may be handwritten - e.g. when the model folder
        // itself was chosen as the target. Back it up before replacing, and
        // do so every time: the latest version is the one you want back.
        std::error_code ec;
        if (fs::exists(carPath, ec)) {
            const std::string bak = carPath + ".bak";
            fs::copy_file(carPath, bak, fs::copy_options::overwrite_existing, ec);
            if (ec) throw std::runtime_error(trf(S::LogCannotWrite, bak.c_str()));
            log(LogLine::Kind::Info, trf(S::LogBackedUp, bak.c_str()));
        }
        writeFileChecked(carPath, car::writeScript(sc));
        std::snprintf(msg, sizeof(msg), tr(S::CarWritten), carPath.c_str());
        log(LogLine::Kind::Good, msg);
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, trf(S::LogScriptNotWritable, e.what()));
    }
    return n;
}

namespace {
std::string lowerName(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}
}  // namespace

// Before exporting, check what's already in the target folder.
//
// The export used to silently overwrite .xsi files of the same name and the
// .car. Anyone who accidentally picked their source folder as the target lost
// the original animations - the exported ones are quantized, so not the same.
void App::exportWithConfirm(std::vector<std::size_t> rows, bool withCar) {
    const auto& ex = extract_;
    if (!ex.loaded || ex.outDir.empty()) return;

    std::vector<std::string> names;
    if (withCar) {
        std::vector<xsiexp::CfgSequence> in;
        for (const auto& s : ex.seqs) in.push_back({s.name, s.start, s.count, s.loopFrame, s.fps});
        for (const auto& m : xsiexp::groupSequences(in).masters) names.push_back(m.self.name);
    } else {
        for (const std::size_t r : rows)
            if (r < ex.seqs.size()) names.push_back(ex.seqs[r].name);
    }

    std::vector<std::string> existing;
    std::error_code ec;
    for (const auto& n : names) {
        const fs::path p = fs::path(ex.outDir) / (lowerName(n) + ".xsi");
        if (fs::exists(p, ec)) existing.push_back(p.string());
    }
    if (withCar) {
        const fs::path car = fs::path(ex.outDir) / (fs::path(ex.glaPath).stem().string() + ".car");
        if (fs::exists(car, ec)) existing.push_back(car.string() + "  (.bak)");
    }

    pendingExport_ = {std::move(rows), withCar, std::move(existing), true};
    if (pendingExport_.existing.empty()) {
        // Nothing gets overwritten: go ahead without asking.
        const PendingExport p = pendingExport_;
        pendingExport_ = {};
        runExport(p.rows, p.withCar);
    }
}

void App::runExport(const std::vector<std::size_t>& rows, bool withCar) {
    const std::string outDir = extract_.outDir;
    if (!withCar) {
        exportSequences(rows, outDir);
        return;
    }
    // Derive the path prefix from the target folder: everything from "models/".
    const std::string norm = fs::path(outDir).generic_string();
    const std::size_t m = norm.rfind("/models/");
    const std::string prefix = (m == std::string::npos) ? std::string() : norm.substr(m + 1) + "/";
    const std::string carOut =
        (fs::path(outDir) / (fs::path(extract_.glaPath).stem().string() + ".car")).string();
    exportAllWithScript(outDir, carOut, prefix);
}

void App::drawOverwriteDialog() {
    if (!pendingExport_.active || pendingExport_.existing.empty()) return;
    if (!ImGui::IsPopupOpen("###overwrite")) ImGui::OpenPopup("###overwrite");
    if (!ImGui::BeginPopupModal((std::string(tr(S::Overwrite)) + "###overwrite").c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const auto& list = pendingExport_.existing;
    ImGui::TextUnformatted(trf(S::OverwriteHead, list.size()).c_str());
    for (std::size_t i = 0; i < list.size() && i < 12; ++i) ImGui::BulletText("%s", list[i].c_str());
    if (list.size() > 12) ImGui::BulletText("... +%zu", list.size() - 12);
    ImGui::Spacing();

    if (ImGui::Button(tr(S::Overwrite))) {
        const PendingExport p = pendingExport_;
        pendingExport_ = {};
        runExport(p.rows, p.withCar);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(S::No)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        pendingExport_ = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool App::openPath(const std::string& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return openFolder(path) > 0;
    if (!fs::exists(path, ec)) {
        log(LogLine::Kind::Warn, trf(S::LogNotFound, path.c_str()));
        return false;
    }
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".car") return openCar(path);
    if (ext == ".h") return loadEnums(path);
    if (ext == ".gla") {
        settings_.referenceGla = path;
        log(LogLine::Kind::Good, trf(S::LogRefGlaSet, path.c_str()));
        return true;
    }
    if (ext == ".xsi") return addXsiFiles({path}, false) > 0;
    log(LogLine::Kind::Warn, trf(S::LogHowToOpen, path.c_str()));
    return false;
}

std::size_t App::openFolder(const std::string& root) {
    const auto found = car::scanDirectory(root);
    log(LogLine::Kind::Info, trf(S::LogCarsUnder, found.size(), root.c_str()));
    std::size_t opened = 0;
    for (const auto& f : found)
        if (openCar(f.path)) ++opened;
    return opened;
}

void App::closeDocument(std::size_t index) {
    if (index >= docs_.size()) return;
    const std::string path = docs_[index].path;
    // Close dialogs that refer to this script right along with it.
    if (editDocPath_ == path) {
        editOpen_ = false;
        editRow_ = -1;
    }
    if (pendingDeleteDocPath_ == path) pendingDelete_.clear();

    // Keep the active tab if it isn't the one being closed. Previously,
    // closing a tab to the left of the active one made the selection jump
    // to the neighbor.
    if (active_ > static_cast<int>(index)) --active_;
    docs_.erase(docs_.begin() + static_cast<long>(index));
    if (active_ >= static_cast<int>(docs_.size())) active_ = static_cast<int>(docs_.size()) - 1;
    if (active_ < 0) active_ = 0;
    // ImGui picks a neighbor on its own after closing. Enforce the one chosen
    // here so that the display and the program agree.
    if (!docs_.empty()) activate(active_);
    refreshTabTitles();
}

void App::requestClose(std::vector<std::size_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    bool anyDirty = false;
    for (const std::size_t i : indices)
        if (i < docs_.size() && docs_[i].dirty) anyDirty = true;

    if (!anyDirty) {
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) closeDocument(*it);
        return;
    }
    pendingClose_.clear();
    for (const std::size_t i : indices)
        if (i < docs_.size()) pendingClose_.push_back(docs_[i].path);
}

bool App::requestQuit() {
    bool anyDirty = false;
    for (const auto& d : docs_)
        if (d.dirty) anyDirty = true;
    if (!anyDirty) {
        quitApproved_ = true;
        return true;
    }
    quitRequested_ = true;
    pendingClose_.clear();
    for (const auto& d : docs_) pendingClose_.push_back(d.path);
    return false;
}

// Unsaved changes: save, discard or cancel.
void App::drawCloseDialog() {
    if (pendingClose_.empty()) return;
    if (!ImGui::IsPopupOpen("###closedlg")) ImGui::OpenPopup("###closedlg");
    if (!ImGui::BeginPopupModal((std::string(tr(S::CloseTab)) + "###closedlg").c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    std::vector<std::size_t> dirty;
    for (std::size_t i = 0; i < docs_.size(); ++i)
        for (const auto& p : pendingClose_)
            if (docs_[i].path == p && docs_[i].dirty) dirty.push_back(i);

    ImGui::TextUnformatted(trf(S::UnsavedHead, dirty.size()).c_str());
    for (const std::size_t i : dirty) ImGui::BulletText("%s", docs_[i].path.c_str());
    ImGui::Spacing();

    const auto finish = [&](bool save) {
        bool ok = true;
        if (save)
            for (const std::size_t i : dirty) ok = saveDocument(i) && ok;
        if (!ok) {
            // Saving failed: close nothing, the reason is in the log.
            quitRequested_ = false;
            restartRequested_ = false;
        } else if (quitRequested_) {
            quitApproved_ = true;
        } else {
            for (std::size_t i = docs_.size(); i-- > 0;)
                for (const auto& p : pendingClose_)
                    if (docs_[i].path == p) {
                        closeDocument(i);
                        break;
                    }
        }
        pendingClose_.clear();
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::Button(withIcon(ICON_SAVE, tr(S::Save)))) finish(true);
    ImGui::SameLine();
    if (ImGui::Button(tr(S::Discard))) finish(false);
    ImGui::SameLine();
    if (ImGui::Button(tr(S::No)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        pendingClose_.clear();
        quitRequested_ = false;
        restartRequested_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

std::size_t App::addXsiFiles(const std::vector<std::string>& files, bool toAll) {
    if (docs_.empty() || files.empty()) return 0;

    // First check what's coming in.
    //
    // A folder or a file that isn't an .xsi can be appended and only shows up
    // during the build - where the error list then contains a path nobody can
    // make sense of. Better to reject it here, where it's clear what the user
    // just did.
    std::vector<std::string> good;
    std::size_t rejected = 0;
    for (const std::string& f : files) {
        std::error_code ec;
        if (fs::is_directory(f, ec)) {
            log(LogLine::Kind::Warn, tr(S::NotAFile) + std::string(" ") + f);
            ++rejected;
            continue;
        }
        std::string ext = fs::path(f).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".xsi") {
            log(LogLine::Kind::Warn, tr(S::NotAnXsi) + std::string(" ") + f);
            ++rejected;
            continue;
        }
        if (!fs::exists(f, ec)) {
            log(LogLine::Kind::Warn, tr(S::FileGone) + std::string(" ") + f);
            ++rejected;
            continue;
        }
        good.push_back(f);
    }
    if (good.empty()) return 0;

    const auto appendTo = [&](Document& d) {
        for (const std::string& f : good) {
            car::GrabDirective g;
            // Store relative to basedir if possible - otherwise the .car
            // contains absolute paths and is no longer usable on any other
            // machine.
            g.file = f;
            if (!settings_.baseDir.empty()) {
                std::error_code ec;
                const auto rel = fs::relative(f, settings_.baseDir, ec);
                if (!ec && !rel.empty() && rel.string().rfind("..", 0) != 0)
                    g.file = rel.generic_string();
            }
            d.script.grabs.push_back(std::move(g));
        }
        d.dirty = true;
        d.validated = false;
        d.syncSelection();
    };

    std::size_t touched = 0;
    if (toAll) {
        for (std::size_t k = 0; k < docs_.size(); ++k) {
            if (!docs_[k].loadError.empty()) continue;
            appendTo(docs_[k]);
            keepRootLast(k);
            ++touched;
        }
    } else if (active_ >= 0 && active_ < static_cast<int>(docs_.size())) {
        appendTo(docs_[static_cast<std::size_t>(active_)]);
        keepRootLast(static_cast<std::size_t>(active_));
        touched = 1;
    }

    log(LogLine::Kind::Good, trf(S::LogAddedFiles, files.size(), touched));
    return touched;
}

// Write the script back.
//
// A backup is made first. The .car is years of handwork; overwriting it
// without a safety net would be reckless, especially since comments and
// indentation get lost when writing.
bool App::saveDocument(std::size_t index) {
    if (index >= docs_.size()) return false;
    Document& d = docs_[index];
    if (!d.loadError.empty()) return false;

    std::error_code ec;
    if (settings_.keepBackup) {
        const fs::path bak = fs::path(d.path).string() + ".bak";
        if (!fs::exists(bak, ec) && fs::exists(d.path, ec)) {
            fs::copy_file(d.path, bak, ec);
            // Don't overwrite without a backup: the user turned it on and
            // relies on it.
            if (ec) {
                log(LogLine::Kind::Bad, trf(S::LogCannotWrite, bak.string().c_str()));
                return false;
            }
        }
    }

    try {
        writeFileChecked(d.path, car::writeScript(d.script));
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, e.what());
        return false;
    }
    d.dirty = false;
    log(LogLine::Kind::Good,
        trf(S::LogSaved, d.title.c_str(),
            settings_.keepBackup ? (d.title + ".bak").c_str() : "-"));
    return true;
}

std::size_t App::saveAllDocuments() {
    std::size_t n = 0;
    for (std::size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i].dirty && saveDocument(i)) ++n;
    if (n == 0) log(LogLine::Kind::Info, tr(S::LogNothingToSave));
    return n;
}

std::size_t App::addXsiFolder(const std::string& folder, bool toAll) {
    // Collect all .xsi of a tree, sorted - otherwise the order of the
    // sequences depends on how the file system returns them.
    std::vector<std::string> files;
    std::error_code ec;
    std::vector<fs::path> stack{fs::path(folder)};
    while (!stack.empty()) {
        const fs::path dir = stack.back();
        stack.pop_back();
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        for (const auto& e : it) {
            std::error_code eec;
            if (e.is_symlink(eec)) continue;
            if (e.is_directory(eec) && !eec) { stack.push_back(e.path()); continue; }
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext == ".xsi") files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        log(LogLine::Kind::Warn, trf(S::LogNoXsiIn, folder.c_str()));
        return 0;
    }
    log(LogLine::Kind::Info, trf(S::LogXsiInFolder, files.size(), folder.c_str()));
    return addXsiFiles(files, toAll);
}

int App::frameCountOf(const std::string& relPath) const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    const auto it = frameCounts_.find(relPath);
    return it == frameCounts_.end() ? -1 : it->second;
}

void App::stopFrameWorker() {
    frameWorkerStop_.store(true);
    if (frameWorker_.joinable()) frameWorker_.join();
    frameWorkerRunning_.store(false);
    frameWorkerStop_.store(false);
}

// Load frame counts in the background.
//
// The first time, every .xsi has to be read - with 1289 files that takes a
// while. In the foreground the window would be frozen that whole time, so it
// runs alongside and the column fills in gradually. The second time
// everything comes from the cache and is there practically instantly.
void App::startFrameWorker(const Document& d) {
    if (frameWorkerRunning_.load()) return;
    if (settings_.baseDir.empty()) return;

    // Asset root changed? Then the previous counts are no longer valid.
    // If the root was guessed wrong at first, "missing" showed everywhere -
    // and stayed that way until restart, even after it was corrected.
    if (!frameWorkerBaseDir_.empty() && frameWorkerBaseDir_ != settings_.baseDir) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frameCounts_.clear();
    }
    frameWorkerBaseDir_ = settings_.baseDir;

    // Collect the missing ones so the thread doesn't have to access the
    // document list - it can change underneath it.
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        for (const auto& g : d.script.grabs)
            if (!frameCounts_.count(g.file)) todo.push_back(g.file);
    }
    if (todo.empty()) return;

    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());

    const std::string baseDir = settings_.baseDir;
    const std::string carDir = fs::path(d.path).parent_path().string();
    const std::string cacheDir =
        settings_.useCache ? (fs::path(baseDir) / "g2c_cache").string() : std::string();

    frameWorkerRunning_.store(true);
    frameWorkerStop_.store(false);

    if (frameWorker_.joinable()) frameWorker_.join();
    frameWorker_ = std::thread([this, todo = std::move(todo), baseDir, carDir, cacheDir] {
        AnimCache cache(cacheDir);
        std::size_t missing = 0;
        for (const auto& rel : todo) {
            if (frameWorkerStop_.load()) break;
            int frames = -2;   // not found
            const std::string full = car::resolveAssetPath(rel, baseDir, carDir);
            if (!full.empty()) {
                try {
                    const xsi::AnimFile a =
                        cache.enabled() ? cache.loadOrParse(full) : xsi::loadAnimationFile(full);
                    frames = a.frameCount();
                } catch (const std::exception&) {
                    frames = -2;
                }
            }
            if (frames == -2) ++missing;
            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                frameCounts_[rel] = frames;
            }
        }
        if (missing == todo.size() && !todo.empty())
            log(LogLine::Kind::Warn, trf(S::LogFramesNotFound, todo.size()));
        frameWorkerRunning_.store(false);
    });
}

std::size_t App::assignDefaultOutputs(bool onlyEmpty) {
    std::size_t n = 0;
    for (auto& d : docs_) {
        if (onlyEmpty && !d.outputDir.empty()) continue;
        if (!d.loadError.empty()) continue;
        d.outputDir = (fs::path(d.path).parent_path() / "g2c_out").string();
        ++n;
    }
    if (n) log(LogLine::Kind::Good, trf(S::LogOutputsSet, n));
    return n;
}

// --- Reordering and deleting sequences -------------------------------------

bool App::moveGrab(std::size_t docIndex, std::size_t from, std::size_t to) {
    if (docIndex >= docs_.size()) return false;
    Document& d = docs_[docIndex];
    const std::size_t n = d.script.grabs.size();
    if (from >= n || to >= n || from == to) return false;

    auto& g = d.script.grabs;
    const auto item = g[from];
    g.erase(g.begin() + static_cast<long>(from));
    g.insert(g.begin() + static_cast<long>(to), item);
    closeEditorOf(docIndex);

    // The selection has to move along. Otherwise, after moving, it points at
    // a different sequence, and the next click on "Delete" hits the wrong
    // one.
    if (d.selected.size() == n) {
        const char sel = d.selected[from];
        d.selected.erase(d.selected.begin() + static_cast<long>(from));
        d.selected.insert(d.selected.begin() + static_cast<long>(to), sel);
    }

    d.dirty = true;
    d.validated = false;
    return true;
}

bool App::isRootGrab(const car::GrabDirective& g) {
    const std::string name = g.enumName ? *g.enumName : g.derivedName();
    if (name == "ROOT" || name == "root") return true;
    std::string stem = fs::path(g.file).stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem == "root";
}

std::size_t App::moveGrabs(std::size_t docIndex, std::vector<std::size_t> rows,
                           std::size_t before) {
    if (docIndex >= docs_.size() || rows.empty()) return 0;
    Document& d = docs_[docIndex];
    const std::size_t n = d.script.grabs.size();
    if (before > n) return 0;

    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.back() >= n) return 0;

    // Remove first, then insert. The target position has to be reduced by
    // the rows removed BEFORE it - otherwise the block lands too far back,
    // by exactly the number of rows moved.
    std::size_t target = before;
    for (const std::size_t r : rows)
        if (r < before) --target;

    std::vector<car::GrabDirective> block;
    std::vector<char> blockSel;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        block.push_back(d.script.grabs[*it]);
        blockSel.push_back(*it < d.selected.size() ? d.selected[*it] : 0);
        d.script.grabs.erase(d.script.grabs.begin() + static_cast<long>(*it));
        if (*it < d.selected.size()) d.selected.erase(d.selected.begin() + static_cast<long>(*it));
    }
    std::reverse(block.begin(), block.end());
    std::reverse(blockSel.begin(), blockSel.end());

    d.script.grabs.insert(d.script.grabs.begin() + static_cast<long>(target), block.begin(),
                          block.end());
    d.selected.insert(d.selected.begin() + static_cast<long>(target), blockSel.begin(),
                      blockSel.end());

    d.dirty = true;
    d.validated = false;
    closeEditorOf(docIndex);
    keepRootLast(docIndex);
    return block.size();
}

bool App::keepRootLast(std::size_t docIndex) {
    if (docIndex >= docs_.size()) return false;
    Document& d = docs_[docIndex];
    auto& g = d.script.grabs;
    if (g.size() < 2) return false;

    for (std::size_t i = 0; i + 1 < g.size(); ++i) {
        if (!isRootGrab(g[i])) continue;
        // Without moveGrab, otherwise the two would call each other.
        const auto item = g[i];
        g.erase(g.begin() + static_cast<long>(i));
        g.push_back(item);
        if (i < d.selected.size()) {
            const char sel = d.selected[i];
            d.selected.erase(d.selected.begin() + static_cast<long>(i));
            d.selected.push_back(sel);
        }
        d.dirty = true;
        closeEditorOf(docIndex);
        return true;
    }
    return false;
}

std::size_t App::deleteGrabs(std::size_t docIndex, std::vector<std::size_t> rows,
                             bool keepComments) {
    if (docIndex >= docs_.size() || rows.empty()) return 0;
    Document& d = docs_[docIndex];

    // Descending, so the remaining indices don't shift underneath the loop.
    std::sort(rows.begin(), rows.end(), std::greater<>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    std::size_t n = 0;
    for (const std::size_t r : rows) {
        if (r >= d.script.grabs.size()) continue;

        // Leave separators and headings above the sequence in place.
        //
        // Technically they are attached to the following grab; for the user
        // they are separate rows - they can be dragged and deleted
        // individually. Previously they disappeared along with the sequence
        // below them, without the confirmation prompt mentioning them.
        auto& moved = d.script.grabs[r].commentsBefore;
        if (keepComments && !moved.empty()) {
            // Deleted in descending order: the grab after it is the next one
            // that stays.
            auto& target = (r + 1 < d.script.grabs.size()) ? d.script.grabs[r + 1].commentsBefore
                                                          : d.script.trailingComments;
            target.insert(target.begin(), moved.begin(), moved.end());
        }
        d.script.grabs.erase(d.script.grabs.begin() + static_cast<long>(r));
        if (r < d.selected.size()) d.selected.erase(d.selected.begin() + static_cast<long>(r));
        ++n;
    }
    if (n) {
        d.dirty = true;
        d.validated = false;
        // An open edit dialog could point at a deleted row.
        closeEditorOf(docIndex);
    }
    return n;
}

std::size_t App::copyGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows) {
    if (docIndex >= docs_.size() || rows.empty()) return 0;
    const Document& d = docs_[docIndex];

    clipboard_.clear();
    // In row order, not selection order - otherwise the result depends on
    // the order in which things were clicked.
    std::vector<std::size_t> sorted = rows;
    std::sort(sorted.begin(), sorted.end());
    for (const std::size_t r : sorted)
        if (r < d.script.grabs.size()) clipboard_.push_back(d.script.grabs[r]);
    return clipboard_.size();
}

std::size_t App::cutGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows) {
    const std::size_t n = copyGrabs(docIndex, rows);
    // The comments go into the clipboard too and come back on paste.
    if (n) deleteGrabs(docIndex, rows, false);
    return n;
}

std::size_t App::pasteGrabs(std::size_t docIndex, std::size_t before) {
    if (docIndex >= docs_.size() || clipboard_.empty()) return 0;
    Document& d = docs_[docIndex];
    if (before > d.script.grabs.size()) before = d.script.grabs.size();

    // What gets inserted is a row of this script itself, even if the source
    // came from an $include file - otherwise the writer would omit it.
    std::vector<car::GrabDirective> items = clipboard_;
    for (auto& g : items) g.fromInclude = -1;
    d.script.grabs.insert(d.script.grabs.begin() + static_cast<long>(before), items.begin(),
                          items.end());
    d.selected.insert(d.selected.begin() + static_cast<long>(before), items.size(), 0);

    d.dirty = true;
    d.validated = false;
    closeEditorOf(docIndex);
    keepRootLast(docIndex);
    return items.size();
}

// The sequence dialog remembers a row number. If the list shifts underneath
// it, it points at a different sequence - the dialog then silently edited
// someone else's row. So close it as soon as the order changes in the same
// script.
void App::closeEditorOf(std::size_t docIndex) {
    if (docIndex < docs_.size() && docs_[docIndex].path == editDocPath_) {
        editOpen_ = false;
        editRow_ = -1;
    }
}

bool App::newCar(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        log(LogLine::Kind::Bad, trf(S::LogExists, path.c_str()));
        return false;
    }

    // An empty but complete script: init and finalize enclose the grabs, the
    // convert directive names the target model. Without those three the file
    // wouldn't be a valid Carcass script.
    car::Script sc;
    car::addGrabFrame(sc);

    car::ConvertDirective cv;
    cv.root = "root";
    cv.makeSkel = "models/players/" + fs::path(path).parent_path().filename().string() + "/"
                  + fs::path(path).stem().string();
    sc.convert = cv;

    try {
        writeFileChecked(path, car::writeScript(sc));
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, e.what());
        return false;
    }

    if (!openCar(path)) return false;
    {
        char msg[600];
        std::snprintf(msg, sizeof(msg), tr(S::NewCarCreated), path.c_str());
        log(LogLine::Kind::Good, msg);
    }
    return true;
}

bool App::loadEnums(const std::string& path) {
    try {
        enums_ = anim::parseEnumHeaderFile(path);
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, trf(S::LogCannotRead, e.what()));
        return false;
    }
    settings_.enumPath = path;
    log(LogLine::Kind::Good, trf(S::LogEnumsLoaded, enums_.size()));
    for (auto& d : docs_) d.validated = false;
    return true;
}

void App::validateDocument(std::size_t index) {
    if (index >= docs_.size()) return;
    Document& d = docs_[index];
    if (!d.loadError.empty()) return;

    car::ValidateOptions vo;
    vo.baseDir = settings_.baseDir;
    vo.readFrameCounts = settings_.readFrameCounts;
    vo.threads = 0;   // all cores
    if (!enums_.empty()) vo.enums = &enums_;
    if (settings_.useCache && !settings_.baseDir.empty())
        vo.cacheDir = (fs::path(settings_.baseDir) / "g2c_cache").string();

    d.validation = car::validate(d.script, d.path, vo);
    d.validated = true;
}

void App::validateAll() {
    for (std::size_t i = 0; i < docs_.size(); ++i) validateDocument(i);
    std::size_t err = 0, warn = 0;
    for (const auto& d : docs_) { err += d.validation.errors; warn += d.validation.warnings; }
    log(err ? LogLine::Kind::Bad : LogLine::Kind::Good, trf(S::LogValidation, err, warn));
}

// --- Building --------------------------------------------------------------

bool App::writeOutput(const std::string& title, const std::string& path, const std::string& data) {
    try {
        writeFileChecked(path, data);
        return true;
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, trf(S::LogWriteFailed, title.c_str(), e.what()));
        return false;
    }
}

void App::buildOne(const Document& d, const Settings& st) {
    if (!d.loadError.empty()) return;
    try {
        if (d.outputDir.empty())
            throw std::runtime_error(tr(S::LogNoOutputDir));
        if (st.referenceGla.empty())
            throw std::runtime_error(tr(S::LogNoRefGla));

        const MdxaFile ref = readMdxa(readWholeFileBytes(st.referenceGla));

        car::BuildOptions bo;
        bo.baseDir = st.baseDir;
        bo.threads = 0;   // all cores
        bo.carcassCompatible = st.carcassCompat;
        if (st.useCache && !st.baseDir.empty())
            bo.cacheDir = (fs::path(st.baseDir) / "g2c_cache").string();

        // Report missing files up front - translated and with attribution.
        //
        // The library's exception is in German and carries the full text;
        // instead, a message in the configured language is built here from
        // the structured data, saying WHICH sequence is affected and where the
        // file was expected.
        {
            car::BuildOptions probe = bo;
            probe.skipMissing = true;

            // The probe run must NOT throw. If all files are missing, build
            // aborts with "not a single one readable" - then the detailed
            // message would never be produced, and the user would see exactly
            // the useless sentence it is meant to replace.
            std::vector<car::BuildResult::MissingFile> missing;
            try {
                missing = car::build(d.script, ref.skeleton, d.path, probe).missing;
            } catch (const std::exception&) {
                // Everything is missing: build the list ourselves.
                for (std::size_t i = 0; i < d.script.grabs.size(); ++i) {
                    const auto& g = d.script.grabs[i];
                    if (!car::resolveAssetPath(g.file, st.baseDir,
                                               fs::path(d.path).parent_path().string())
                             .empty())
                        continue;
                    car::BuildResult::MissingFile m;
                    m.file = g.file;
                    m.sequence = g.enumName ? *g.enumName : g.derivedName();
                    m.grabIndex = i;
                    m.line = g.line;
                    missing.push_back(std::move(m));
                }
            }

            const struct { const std::vector<car::BuildResult::MissingFile>& missing; } chk{missing};
            if (!chk.missing.empty() && !bo.skipMissing) {
                char head[160];
                std::snprintf(head, sizeof(head), tr(S::MissingHead), chk.missing.size(),
                              d.script.grabs.size());
                log(LogLine::Kind::Bad, d.title + ": " + head);
                if (!st.baseDir.empty())
                    log(LogLine::Kind::Bad,
                        std::string(tr(S::MissingSearched)) + " " + st.baseDir);
                log(LogLine::Kind::Bad, tr(S::MissingList));

                for (std::size_t i = 0; i < chk.missing.size() && i < 8; ++i) {
                    const auto& m = chk.missing[i];
                    std::string line = "  " + m.file + "   [" + tr(S::SeqLabel) + " " +
                                       m.sequence + "]";
                    if (m.line) line += " (.car " + std::to_string(m.line) + ")";
                    log(LogLine::Kind::Bad, line);
                    if (!st.baseDir.empty())
                        log(LogLine::Kind::Info,
                            "      " +
                                (fs::path(st.baseDir) / m.file).lexically_normal().string());
                }
                if (chk.missing.size() > 8) {
                    char more[80];
                    std::snprintf(more, sizeof(more), tr(S::MissingMore), chk.missing.size() - 8);
                    log(LogLine::Kind::Bad, more);
                }
                log(LogLine::Kind::Warn, tr(S::MissingHintBase));
                log(LogLine::Kind::Warn, tr(S::MissingHintSkip));
                return;
            }
        }

        // Does the build write onto the reference GLA?
        //
        // Then the same file is read and written, and Windows locks it:
        // "Cannot open ...", again for every grab. The message names the path
        // but not the reason - you then look for missing permissions instead
        // of the overlap.
        if (!d.outputDir.empty() && !st.referenceGla.empty()) {
            std::error_code rec;
            const fs::path refPath(st.referenceGla);
            const fs::path refDir = refPath.parent_path();
            if (!refDir.empty() && fs::exists(refDir, rec) && fs::exists(d.outputDir, rec) &&
                fs::equivalent(refDir, d.outputDir, rec) && !rec) {
                log(LogLine::Kind::Bad, trf(S::RefIsTarget, d.title.c_str()));
            }
        }

        // Modified scripts were already saved in startBuild, on the main
        // thread. Doing it here would be wrong: d is a copy, and docs_
        // belongs to the UI.

        const car::BuildResult br = car::build(d.script, ref.skeleton, d.path, bo);
        for (const auto& w : br.warnings) log(LogLine::Kind::Warn, d.title + ": " + w);

        // Report duplicate names on BUILD too, not only on validation.
        //
        // Anyone who builds without validating first would otherwise get an
        // animation.cfg in which one animation is unreachable - and only
        // notice in the game, where nothing points to it.
        {
            std::map<std::string, int> gesehen;
            for (const auto& sq : br.sequences) {
                std::string n = sq.name;
                for (auto& c : n)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                ++gesehen[n];
            }
            std::vector<std::string> doppelt;
            for (const auto& [name, anzahl] : gesehen)
                if (anzahl > 1) {
                    log(LogLine::Kind::Bad,
                        trf(S::DupInCfg, d.title.c_str(), name.c_str(), anzahl));
                    doppelt.push_back(name);
                }

            // Do NOT write while names are duplicated.
            //
            // A GLA with unreachable animations looks finished and only
            // shows up in the game - where nothing then points to the cause.
            // Better to write nothing at all and say so here.
            if (!doppelt.empty()) {
                // Pass the names along: the caller enters them as messages so
                // that clicking one jumps to the rows.
                {
                    std::lock_guard<std::mutex> lock(dupMutex_);
                    pendingDuplicates_ = doppelt;
                    pendingDupDoc_ = d.title;
                }
                throw std::runtime_error(trf(S::BuildStoppedDup, doppelt.size()));
            }
        }

        MdxaWriteOptions wo;
        wo.threads = 0;   // all cores
        if (st.carcassCompat) {
            wo.compress.rounding = Rounding::Legacy;
            wo.compress.optimizeQuat = false;
            wo.compress.canonicalizeSign = false;
        }
        // The GLA name in the header comes from -makeskel, NOT from the
        // reference.
        //
        // It is stored as a string in the file and tells the engine which
        // skeleton this is. When we took it from the reference, every built
        // file carried "models/players/_humanoid/_humanoid" - no matter where
        // it belonged. In the game a custom humanoid then identified itself as
        // the standard humanoid, and the engine used that one's animation.cfg.
        Skeleton outSkel = ref.skeleton;
        if (d.script.convert && !d.script.convert->makeSkel.empty()) {
            std::string ms = d.script.convert->makeSkel;
            std::replace(ms.begin(), ms.end(), '\\', '/');
            if (ms != outSkel.name)
                log(LogLine::Kind::Info, trf(S::GlaNameFromMakeSkel, ms.c_str()));
            outSkel.name = ms;
        }

        const auto res = writeMdxa(outSkel, br.frames, wo);

        const fs::path outDir(d.outputDir);
        std::error_code ec;
        fs::create_directories(outDir, ec);

        std::string stem = "out";
        if (d.script.convert && !d.script.convert->makeSkel.empty()) {
            const std::string& ms = d.script.convert->makeSkel;
            const std::size_t sl = ms.find_last_of("/\\");
            stem = (sl == std::string::npos) ? ms : ms.substr(sl + 1);
        }

        // Every output goes through writeOutput: first into a side file, then
        // replaced in one go, errors go to the log. Previously unchecked
        // ofstreams wrote directly to the target - if the GLA was locked (game
        // or ModView open) or the disk full, the log still said "built", and
        // the old file had already been truncated to zero.
        const fs::path gla = outDir / (stem + ".gla");
        if (!writeOutput(d.title, gla.string(),
                         std::string(reinterpret_cast<const char*>(res.data.data()),
                                     res.data.size())))
            return;
        log(LogLine::Kind::Good, d.title + " -> " + gla.string() + " (" +
                                     std::to_string(br.totalFrames()) + " Frames)");

        {
            std::ostringstream head;
            head << br.totalFrames() << " frames; " << br.sequences.size()
                 << " sequences; erzeugt von g2c";
            const std::string cfg = car::writeAnimationCfg(br.sequences, head.str());
            // No .bak: the animation.cfg is regenerated completely on every
            // build, a backup of it would be worthless and just clutter the
            // folder. For .car files, which are edited by hand, that's
            // different - the backup stays there.
            if (!writeOutput(d.title, (outDir / "animation.cfg").string(), cfg)) return;
        }
        log(LogLine::Kind::Warn, tr(S::CfgBelongsWithGla));

        // Remember the output folder so it can be opened.
        {
            std::lock_guard<std::mutex> lock(outputDirMutex_);
            lastOutputDir_ = outDir.string();
        }

        if (st.writeFrames && !br.frameBlocks.empty()) {
            std::vector<FrameEntry> fe;
            for (const auto& b : br.frameBlocks) {
                FrameEntry e;
                e.sourcePath = fs::absolute(b.sourcePath).generic_string();
                e.startFrame = b.startFrame;
                e.duration = b.duration;
                e.fps = b.fps;
                for (int k = 0; k < 3; ++k) e.averageVec[k] = b.averageVec[k];
                fe.push_back(std::move(e));
            }
            writeOutput(d.title, (outDir / (stem + ".frames")).string(), writeFrames(fe));
        }

        if (st.writeMesh && d.script.convert && !d.script.convert->root.empty()) {
            const std::string carDir = fs::path(d.path).parent_path().string();
            std::string xsi;
            for (const char* ext : {".xsi", ".XSI"}) {
                xsi = car::resolveAssetPath(d.script.convert->root + ext, st.baseDir, carDir);
                if (!xsi.empty()) break;
            }
            if (xsi.empty()) {
                log(LogLine::Kind::Warn, trf(S::LogMeshSourceMissing, d.title.c_str()));
            } else {
                xsi::MeshImportOptions mo;
                mo.scale = ref.skeleton.scale > 0.0f ? ref.skeleton.scale : 1.0f;
                mo.animName = ref.skeleton.name;
                mo.modelName = stem + ".glm";
                for (const auto& b : ref.skeleton.bones) mo.boneNames.push_back(b.name);
                const auto mr = xsi::importMeshFile(xsi, mo);
                const auto mw = writeMdxm(mr.mesh);
                if (writeOutput(d.title, (outDir / (stem + ".glm")).string(),
                                std::string(reinterpret_cast<const char*>(mw.data.data()),
                                            mw.data.size()))) {
                    log(LogLine::Kind::Good,
                        d.title + " -> " + stem + ".glm (" + std::to_string(mr.stats.surfaces) +
                            " Surfaces, " + std::to_string(mr.stats.vertices) + " Verts)");
                    if (st.writeSkin)
                        writeOutput(d.title, (outDir / (stem + ".skin")).string(),
                                    writeSkin(mr.mesh));
                }
            }
        }
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, d.title + ": " + e.what());
    }
}

void App::startBuild(bool allTabs) {
    if (job_.running.load()) return;
    job_.join();

    std::vector<std::size_t> which;
    if (allTabs) {
        for (std::size_t i = 0; i < docs_.size(); ++i)
            if (docs_[i].loadError.empty()) which.push_back(i);
    } else if (active_ >= 0 && active_ < static_cast<int>(docs_.size())) {
        which.push_back(static_cast<std::size_t>(active_));
    }

    // Save modified scripts before building - here, on the main thread.
    //
    // What gets built is the in-memory state. If the file on disk stayed the
    // old one, after closing you had a GLA that no longer matched any .car,
    // and on the next open the change was gone.
    //
    // Previously this happened on the worker thread via "&d - docs_.data()" -
    // but d was a copy there, so the pointer difference was undefined. It
    // practically never saved, and in the worst case saved a different script.
    for (const std::size_t i : which) {
        if (!docs_[i].dirty || docs_[i].path.empty()) continue;
        if (!docs_[i].outputDir.empty() && saveDocument(i))
            log(LogLine::Kind::Info, trf(S::SavedBeforeBuild, docs_[i].title.c_str()));
    }

    std::vector<Document> targets;
    for (const std::size_t i : which) targets.push_back(docs_[i]);
    if (targets.empty()) {
        log(LogLine::Kind::Warn, tr(S::LogNothingToBuild));
        return;
    }

    // Check up front instead of aborting halfway: anyone kicking off twenty
    // scripts wants to know beforehand that one has no target, not after ten
    // minutes.
    std::vector<std::string> noTarget;
    for (const auto& t : targets)
        if (t.outputDir.empty()) noTarget.push_back(t.title);
    if (!noTarget.empty()) {
        std::string list;
        for (std::size_t i = 0; i < noTarget.size() && i < 6; ++i)
            list += (i ? ", " : "") + noTarget[i];
        if (noTarget.size() > 6) list += " und " + std::to_string(noTarget.size() - 6) + " weitere";
        log(LogLine::Kind::Bad, trf(S::LogNoOutputAt, list.c_str()));
        return;
    }

    // Do two scripts write to the same folder?
    //
    // Then the second GLA overwrites the first, and worse: the animation.cfg
    // then belongs to the second, while the first GLA is gone. In the game
    // that looks like swapped animations, and the cause can't be seen from
    // the outside.
    //
    // Only warn, don't abort: there are cases where exactly that is intended -
    // e.g. when one script is explicitly supposed to replace another.
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].outputDir.empty()) continue;
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            if (targets[j].outputDir.empty()) continue;
            std::error_code ec;
            const bool gleich =
                fs::equivalent(targets[i].outputDir, targets[j].outputDir, ec) && !ec;
            if (gleich)
                log(LogLine::Kind::Warn,
                    trf(S::SameOutDir, targets[i].title.c_str(), targets[j].title.c_str()));
        }
    }

    job_.cancel.store(false);
    job_.done.store(0);
    job_.total.store(targets.size());
    job_.running.store(true);

    // Copies of the documents AND the settings, so the UI stays usable during
    // the build and nothing gets edited away underneath the worker thread.
    job_.worker = std::thread([this, targets = std::move(targets), st = settings_] {
        for (const auto& d : targets) {
            if (job_.cancel.load()) break;
            {
                std::lock_guard<std::mutex> lock(job_.currentMutex);
                job_.current = d.title;
            }
            buildOne(d, st);
            job_.done.fetch_add(1);
        }
        job_.running.store(false);
    });
}

// --- Appearance ------------------------------------------------------------

void App::applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Start from the defaults every time.
    //
    // ScaleAllSizes multiplies whatever is currently set. Previously only some
    // fields were reset; the rest grew by the DPI factor on every switch
    // between light and dark: after six switches at 150 % the spacing was 63
    // instead of 6 pixels, and the windows' resize grip covered the buttons
    // along their bottom edge.
    s = ImGuiStyle();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowPadding = ImVec2(12, 10);
    s.CellPadding = ImVec2(8, 4);
    s.ScrollbarSize = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;

    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;

    if (settings_.darkMode) {
        ImGui::StyleColorsDark();
        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.22f, 0.28f, 0.40f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.36f, 0.52f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.40f, 0.52f, 1.00f);
        c[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.34f, 0.46f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.28f, 0.40f, 1.00f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(0.17f, 0.17f, 0.20f, 1.00f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
        c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.29f, 1.00f);
    } else {
        ImGui::StyleColorsLight();
    }

    // Scale all sizes along. The font is loaded at the right size in the
    // window backend; here spacing, rounding and scrollbars follow suit.
    s.ScaleAllSizes(k);
    styleApplied_ = true;
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu(tr(S::MenuFile))) {
        if (ImGui::MenuItem(withIcon(ICON_ADD, tr(S::NewCar)), "Ctrl+N")) newCarDialog();
        if (ImGui::MenuItem(withIcon(ICON_OPEN_FILE, tr(S::OpenScript)), "Ctrl+O"))
            openScriptDialog();
        if (ImGui::MenuItem(withIcon(ICON_FOLDER_OPEN, tr(S::OpenFolder)), "Ctrl+Shift+O"))
            openFolderDialog();
        ImGui::Separator();
        if (ImGui::MenuItem(withIcon(ICON_SAVE, tr(S::Save)), "Ctrl+S", false, !docs_.empty()))
            saveDocument(static_cast<std::size_t>(active_));
        if (ImGui::MenuItem(withIcon(ICON_SAVE_ALL, tr(S::SaveAll)), "Ctrl+Shift+S", false, !docs_.empty()))
            saveAllDocuments();
        ImGui::Separator();
        if (ImGui::MenuItem(tr(S::CloseTab), "Ctrl+W", false, !docs_.empty()))
            requestClose({static_cast<std::size_t>(active_)});
        if (ImGui::MenuItem(tr(S::CloseAll), nullptr, false, !docs_.empty())) {
            std::vector<std::size_t> all(docs_.size());
            for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
            requestClose(all);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr(S::MenuBuild))) {
        if (ImGui::MenuItem(tr(S::BuildCurrent), "F5", false, !docs_.empty() && !buildRunning()))
            startBuild(false);
        if (ImGui::MenuItem(tr(S::BuildAll), "Shift+F5", false, !docs_.empty() && !buildRunning()))
            startBuild(true);
        ImGui::Separator();
        if (ImGui::MenuItem(tr(S::ValidateAll), "F7", false, !docs_.empty())) validateAll();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr(S::MenuView))) {
        if (ImGui::MenuItem(tr(S::Dark), nullptr, settings_.darkMode)) {
            settings_.darkMode = true;
            applyStyle();
        }
        if (ImGui::MenuItem(tr(S::Light), nullptr, !settings_.darkMode)) {
            settings_.darkMode = false;
            applyStyle();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(withIcon(ICON_GLOBE, tr(S::Language)))) {
            for (int l = 0; l < static_cast<int>(Lang::Count); ++l) {
                const Lang cand = static_cast<Lang>(l);
                if (ImGui::MenuItem(langName(cand), nullptr, language() == cand)) {
                    setLanguage(cand);
                    settings_.language = l;
                    // The glyphs for Chinese and Japanese aren't loaded at
                    // startup. The window backend rebuilds the font atlas as
                    // soon as it sees this.
                    fontsDirty_ = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem(withIcon(ICON_SETTINGS, tr(S::Settings)), nullptr, &showSettings_);
        ImGui::Separator();
        if (updater_ && ImGui::MenuItem(tr(S::UpdCheckNow), nullptr, false, !updater_->busy()))
            checkForUpdates(true);
        ImGui::MenuItem(tr(S::About), nullptr, &showAbout_);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::drawToolbar() {
    const bool busy = buildRunning();
    const bool have = !docs_.empty();

    ImGui::BeginDisabled(busy || !have);
    if (iconButton(ICON_SAVE, kIconInfo, tr(S::BtnSave))) saveDocument(static_cast<std::size_t>(active_));
    ImGui::SameLine();
    {
        std::size_t dirty = 0;
        for (const auto& d : docs_) if (d.dirty) ++dirty;
        ImGui::BeginDisabled(dirty == 0);
        if (ImGui::Button(dirty ? (std::string(tr(S::BtnSaveAll)) + " (" + std::to_string(dirty) + ")").c_str()
                                : tr(S::BtnSaveAll)))
            saveAllDocuments();
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (iconButton(ICON_BUILD, kIconGood, tr(S::BtnBuild))) startBuild(false);
    ImGui::SameLine();
    if (iconButton(ICON_BUILD, kIconGood, tr(S::BtnBuildAll))) startBuild(true);
    ImGui::SameLine();
    if (iconButton(ICON_VALIDATE, kIconWarn, tr(S::BtnValidate))) validateAll();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    ImGui::BeginDisabled(busy);
    if (iconButton(ICON_FOLDER_OPEN, kIconAccent, tr(S::BtnOpenFolder))) openFolderDialog();
    ImGui::SameLine();
    ImGui::BeginDisabled(!have);
    if (iconButton(ICON_ADD, kIconInfo, tr(S::BtnAddXsi)) && platform_.openFiles) {
        const auto files = askFiles("xsi", tr(S::DlgTitleXsi), "dotXSI (*.xsi)\0*.xsi\0Alle\0*.*\0", true);
        if (!files.empty()) addXsiFiles(files, false);
    }
    ImGui::SameLine();
    if (iconButton(ICON_FOLDER, kIconInfo, tr(S::BtnAddXsiFolder)) &&
        (platform_.pickFolders || platform_.pickFolder)) {
        // Several folders at once: for a model with animations from ten
        // sources, clicking ten times is the actual work.
        const auto dirs = askFolders("xsifolder", tr(S::DlgTitleXsiFolder));
        for (const auto& d : dirs) addXsiFolder(d, false);
        if (dirs.size() > 1)
            log(LogLine::Kind::Info, trf(S::FoldersAdded, dirs.size(), std::size_t{0}));
    }
    ImGui::SameLine();
    if (iconButton(ICON_FOLDER, kIconAccent, tr(S::BtnAddXsiFolderAll)) &&
        (platform_.pickFolders || platform_.pickFolder)) {
        const auto dirs = askFolders("xsifolder", tr(S::DlgTitleXsiFolder));
        for (const auto& d : dirs) addXsiFolder(d, true);
        if (dirs.size() > 1)
            log(LogLine::Kind::Info, trf(S::FoldersAdded, dirs.size(), std::size_t{0}));
    }
    ImGui::SameLine();
    if (iconButton(ICON_ADD, kIconAccent, tr(S::BtnAddXsiAll)) && platform_.openFiles) {
        const auto files = askFiles("xsi", tr(S::DlgTitleXsi), "dotXSI (*.xsi)\0*.xsi\0Alle\0*.*\0", true);
        if (!files.empty()) addXsiFiles(files, true);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button(withIcon(ICON_CANCEL, tr(S::BtnCancel)))) job_.cancel.store(true);
    }
}

void App::drawSettingsPanel() {
    if (!showSettings_) return;
    // Scale the width too, otherwise the paths get cut off on high-resolution
    // screens. Scrollable, so the content never overflows.
    const float sw = 360.0f * (settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f);
    ImGui::BeginChild("settings", ImVec2(sw, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::SeparatorText(tr(S::SecPaths));

    // purpose: key for the folder memory. Each row gets its own, so that
    // anims.h, asset root and reference GLA don't overwrite each other's
    // last used folder.
    const auto pathRow = [&](const char* purpose, const char* label, std::string& value,
                             bool folder, const char* filter) {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        ImGui::SetNextItemWidth(-46 * (settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f));
        if (ImGui::InputText("##v", buf, sizeof(buf))) value = buf;
        ImGui::SameLine();
        if (ImGui::Button(iconsAvailable() ? ICON_FOLDER_OPEN : "...")) {
            if (folder && platform_.pickFolder) {
                const std::string p = askFolder(purpose, label);
                if (!p.empty()) value = p;
            } else if (!folder && platform_.openFiles) {
                const auto f = askFiles(purpose, label, filter, false);
                if (!f.empty()) value = f.front();
            }
        }
        ImGui::PopID();
    };

    pathRow("assetroot", tr(S::AssetRoot), settings_.baseDir, true, nullptr);
    pathRow("refgla", tr(S::ReferenceGla), settings_.referenceGla, false,
            "GLA (*.gla)\0*.gla\0Alle\0*.*\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::ReferenceGlaTooltip));

    std::string before = settings_.enumPath;
    pathRow("enums", tr(S::EnumTable), settings_.enumPath, false, "Header (*.h)\0*.h\0Alle\0*.*\0");
    if (settings_.enumPath != before && !settings_.enumPath.empty()) loadEnums(settings_.enumPath);
    if (!enums_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(tr(S::ChooserCount), enums_.size());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(S::OutputPerScript));
    if (ImGui::Button(withIcon(ICON_FOLDER, tr(S::AssignDefaultOutputs)))) assignDefaultOutputs(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::AssignTooltip));

    ImGui::SeparatorText(tr(S::SecOutput));
    ImGui::Checkbox(tr(S::WriteFrames), &settings_.writeFrames);
    ImGui::Checkbox(tr(S::WriteMesh), &settings_.writeMesh);
    ImGui::BeginDisabled(!settings_.writeMesh);
    ImGui::Checkbox(tr(S::WriteSkin), &settings_.writeSkin);
    ImGui::EndDisabled();

    ImGui::SeparatorText(tr(S::SecProcessing));
    ImGui::Checkbox(tr(S::UseCache), &settings_.useCache);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::CacheTooltip));
    ImGui::Checkbox(tr(S::CarcassMode), &settings_.carcassCompat);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::CarcassTooltip));
    ImGui::Checkbox(tr(S::ReadFrameCounts), &settings_.readFrameCounts);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::ReadFrameCountsTooltip));

    // No thread setting anymore.
    //
    // There was never a good reason to use fewer than all cores: the output
    // is demonstrably bit-identical regardless of the thread count, and a
    // setting that can only be set wrong is no setting. Threads stays fixed
    // at 0, which means "all cores".
    ImGui::Spacing();
    ImGui::TextDisabled(tr(S::CoresInUse), g2::defaultThreadCount());

    drawUpdateSettings();

    ImGui::EndChild();
}

void App::drawUpdateSettings() {
    if (!updater_) return;
    ImGui::SeparatorText(tr(S::SecUpdates));
    ImGui::Checkbox(tr(S::UpdAuto), &settings_.checkUpdates);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::UpdAutoTip));

    const char* names[] = {tr(S::UpdStable), tr(S::UpdSnapshot)};
    int ch = static_cast<int>(updateChannel());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##updchannel", &ch, names, 2)) settings_.updateChannel = ch;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::UpdChannelTip));

    ImGui::TextDisabled(tr(S::AboutVersion), update::displayName(platform_.build).c_str());
    ImGui::BeginDisabled(updater_->busy());
    if (ImGui::Button(tr(S::UpdCheckNow))) checkForUpdates(true);
    ImGui::EndDisabled();
}

std::string describeUpdateError(const update::Status& st) {
    switch (st.error) {
        case update::Error::Network: return trf(S::UpdErrNetwork, st.detail.c_str());
        case update::Error::NoRelease: return tr(S::UpdErrNoRelease);
        case update::Error::BadAnswer: return tr(S::UpdErrBadAnswer);
        case update::Error::NoAsset: return trf(S::UpdErrNoAsset, st.detail.c_str());
        case update::Error::Untrusted: return tr(S::UpdErrUntrusted);
        case update::Error::Checksum: return trf(S::UpdErrChecksum, st.detail.c_str());
        case update::Error::Write: return trf(S::UpdErrWrite, st.detail.c_str());
        case update::Error::Cancelled: return tr(S::UpdCancelled);
        case update::Error::None: break;
    }
    return {};
}

// Bar below the toolbar: new version, download progress, restart.
//
// Not a modal dialog: an update is never urgent enough to interrupt a build
// or an edit, and the bar stays until it is answered.
void App::drawUpdateBanner() {
    if (!updater_) return;
    const update::Status st = updater_->status();
    const bool skipped = !st.manual && !settings_.skippedUpdate.empty() &&
                         update::releaseId(st.release, updateChannel()) == settings_.skippedUpdate;
    switch (st.phase) {
        case update::Phase::Idle: return;
        case update::Phase::Checking:
        case update::Phase::UpToDate:
            if (!st.manual) return;
            break;
        case update::Phase::Available:
            if (updateBannerHidden_ || skipped) return;
            break;
        case update::Phase::Failed:
            // A startup check without network is nobody's business.
            if (!st.manual) return;
            break;
        default: break;
    }

    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
    const bool  bad = st.phase == update::Phase::Failed;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bad ? ImVec4(0.45f, 0.16f, 0.16f, 0.55f)
                                                : ImVec4(0.16f, 0.36f, 0.62f, 0.45f));
    ImGui::BeginChild("updbanner", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();

    const std::string mine = update::displayName(platform_.build);
    const std::string theirs = update::displayName(st.release, updateChannel());
    const auto mb = [](std::uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };

    switch (st.phase) {
        case update::Phase::Checking:
            ImGui::TextUnformatted(tr(S::UpdChecking));
            break;
        case update::Phase::UpToDate:
            iconText(ICON_CHECK, kIconGood, trf(S::UpdUpToDate, mine.c_str()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::DlgClose))) updater_->dismiss();
            break;
        case update::Phase::Available:
            iconText(ICON_INFO, kIconInfo,
                     trf(S::UpdAvailable, theirs.c_str(), mine.c_str()).c_str());
            ImGui::SameLine(0, 16 * k);
            if (ImGui::SmallButton(tr(S::UpdInstall))) updater_->install();
            if (!st.release.htmlUrl.empty() && platform_.openUrl) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(S::UpdNotes))) platform_.openUrl(st.release.htmlUrl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::UpdLater))) updateBannerHidden_ = true;
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::UpdSkip))) {
                settings_.skippedUpdate = update::releaseId(st.release, updateChannel());
                updateBannerHidden_ = true;
            }
            break;
        case update::Phase::Downloading: {
            ImGui::TextUnformatted(trf(S::UpdDownloading, theirs.c_str()).c_str());
            ImGui::SameLine();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB", mb(st.done), mb(st.total));
            const float frac =
                st.total ? static_cast<float>(st.done) / static_cast<float>(st.total) : 0.0f;
            ImGui::ProgressBar(frac, ImVec2(220 * k, 0), overlay);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::BtnCancel))) updater_->cancel();
            break;
        }
        case update::Phase::Installed:
            iconText(ICON_CHECK, kIconGood, trf(S::UpdInstalled, theirs.c_str()).c_str());
            ImGui::SameLine(0, 16 * k);
            if (ImGui::SmallButton(tr(S::UpdRestart))) {
                restartRequested_ = true;
                requestQuit();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::UpdLater))) updater_->dismiss();
            if (!st.detail.empty()) ImGui::TextDisabled("%s", st.detail.c_str());
            break;
        case update::Phase::Failed: {
            iconText(ICON_ERROR, kIconBad, describeUpdateError(st).c_str());
            ImGui::SameLine(0, 16 * k);
            if (platform_.openUrl && ImGui::SmallButton(tr(S::UpdOpenPage)))
                platform_.openUrl(std::string("https://github.com/") + update::kRepo + "/releases");
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::DlgClose))) updater_->dismiss();
            break;
        }
        case update::Phase::Idle: break;
    }
    ImGui::EndChild();
}

namespace {

// Drag and drop in the sequence table: BEFORE or AFTER the row?
//
// When dragging over, ImGui outlined the whole row, and the drop always went
// before it. So you couldn't see where the sequence would land, and dragging
// into the lower half of a row still meant "before".
//
// Now the mouse position decides: upper half before, lower half after - and
// a line shows exactly that spot.
constexpr ImGuiDragDropFlags kDropFlags =
    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;

bool dropBelow() {
    const float mid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
    return ImGui::GetMousePos().y > mid;
}

// Insertion line at the top or bottom edge of the last drawn item. On the
// foreground layer, because the table clips each column to its own area;
// limited to the table window.
void drawDropLine(bool below, float k) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float y = below ? b.y : a.y;
    const ImU32 col = IM_COL32(90, 165, 255, 255);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), true);
    dl->AddLine(ImVec2(a.x + 4.0f * k, y), ImVec2(b.x - 4.0f * k, y), col, 3.0f * k);
    dl->AddCircleFilled(ImVec2(a.x + 5.0f * k, y), 4.5f * k, col);
    dl->PopClipRect();
}

}  // namespace

// Same rule as when drawing: the name or the file contains the filter text.
bool App::rowVisible(const Document& d, std::size_t i) const {
    if (filter_[0] == '\0') return true;
    if (i >= d.script.grabs.size()) return false;
    const auto& g = d.script.grabs[i];
    const std::string name = g.enumName ? *g.enumName : g.derivedName();
    return name.find(filter_) != std::string::npos || g.file.find(filter_) != std::string::npos;
}

void App::drawSequenceTable(Document& d) {
    if (!d.loadError.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", tr(S::LogUnreadable));
        ImGui::TextWrapped("%s", d.loadError.c_str());
        return;
    }

    // Output location of this script.
    //
    // What matters is that you SEE whether something of its own is set here
    // or the global value shines through. A gray placeholder looks like a set
    // value and is therefore worse than no display at all.
    {
        const bool own = !d.outputDir.empty();

        ImGui::TextUnformatted(tr(S::OutputTo));
        ImGui::SameLine();
        if (own) {
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", d.outputDir.c_str());
        } else {
            // Translated, and without an em dash: it lies outside the glyph
            // range of the German font and showed up as "?".
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1), "%s", tr(S::NotSet));
        }

        ImGui::SameLine();
        if (ImGui::SmallButton(withIcon(ICON_FOLDER_OPEN, tr(S::ChooseFolder))) && platform_.pickFolder) {
            const std::string p = askFolder("output", tr(S::DlgTitleOutput));
            if (!p.empty()) d.outputDir = p;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(S::DefaultFolder))) {
            d.outputDir = (fs::path(d.path).parent_path() / "g2c_out").string();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr(S::TipDefaultFolder));
        if (own) {
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::ClearFolder))) d.outputDir.clear();
        }
    }

    ImGui::SetNextItemWidth(220 * settings_.dpiScale);
    ImGui::InputTextWithHint("##filter", tr(S::FilterHint), filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::TextDisabled(tr(S::GrabCount), d.script.grabs.size());
    {
        std::size_t nsel = 0;
        for (const char c : d.selected)
            if (c) ++nsel;
        if (nsel) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), tr(S::SelectionHint), nsel);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S::CutSelection)))
                std::fill(d.selected.begin(), d.selected.end(), char{0});
        } else if (filter_[0] == '\0') {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(S::DragHint));
        }
        if (!clipboard_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1), tr(S::ClipHint), clipboard_.size());
        }
    }
    if (frameWorkerRunning_.load()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr(S::ReadingFrames));
    }
    if (d.validated) {
        ImGui::SameLine();
        if (d.validation.errors)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), tr(S::NErrors), d.validation.errors);
        else
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s", tr(S::Validated));
        if (d.validation.warnings) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), tr(S::NWarnings), d.validation.warnings);
        }
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    // Explicitly the remaining height. With -1, depending on the environment,
    // space was left over and an empty gap gaped below.
    // Eighth column: the row comment. The number MUST match the number of
    // TableSetupColumn calls - otherwise ImGui reports
    // "Called TableSetupColumn() too many times", and the extra column slips
    // into a row of its own.
    if (!ImGui::BeginTable("seqs", 8, flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(S::ColSequence), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(tr(S::ColFrames), ImGuiTableColumnFlags_WidthFixed, 65);
    ImGui::TableSetupColumn(tr(S::ColLoop), ImGuiTableColumnFlags_WidthFixed, 55);
    ImGui::TableSetupColumn(tr(S::ColSpeed), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(tr(S::ColExtra), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(tr(S::ColSource), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(tr(S::ColEnum), ImGuiTableColumnFlags_WidthFixed, 70);
    // Row comment on the far right: it's a note, not information you need
    // when skimming.
    ImGui::TableSetupColumn(tr(S::ColComment), ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableHeadersRow();
    if (ImGui::TableGetHoveredColumn() == 6 && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(S::TipEnumColumn));

    const std::string needle = filter_;
    const bool hasFilter = !needle.empty();
    if (d.selected.size() != d.script.grabs.size()) d.syncSelection();

    for (std::size_t i = 0; i < d.script.grabs.size(); ++i) {
        auto& g = d.script.grabs[i];
        const std::string name = g.enumName ? *g.enumName : g.derivedName();
        if (!needle.empty() && name.find(needle) == std::string::npos &&
            g.file.find(needle) == std::string::npos)
            continue;

        // Show comment lines above the sequence.
        //
        // Without this you can't see in the table where the structure is -
        // and it would only be visible in the finished animation.cfg, i.e.
        // too late to fix.
        for (std::size_t ci = 0; ci < g.commentsBefore.size(); ++ci) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool bearbeite = editCommentGrab_ == static_cast<int>(i) &&
                                   editCommentLine_ == static_cast<int>(ci);
            // Own ID from grab and line. Previously i*1000+ci: that collided
            // from grab 900 on with the trailing comments (900000+ti) and from
            // grab 1 on with the row IDs of large scripts.
            char cid[48];
            std::snprintf(cid, sizeof(cid), "cmt%zu_%zu", i, ci);
            ImGui::PushID(cid);

            if (bearbeite) {
                // Edit directly in the row.
                ImGui::SetNextItemWidth(-1);
                // Request focus once when editing starts - not every frame
                // while nothing is active. Right after the double-click the
                // mouse still holds the item underneath, and the repeated
                // request never got its turn: the field appeared, but typed
                // text went nowhere.
                if (focusEditField_) {
                    ImGui::SetKeyboardFocusHere();
                    focusEditField_ = false;
                }
                const bool fertig = ImGui::InputText(
                    "##cedit", editCommentBuf_, sizeof(editCommentBuf_),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                // Apply on Enter OR when focus leaves - otherwise the input
                // is lost when someone clicks elsewhere.
                if (fertig || (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())) {
                    std::string neu = editCommentBuf_;
                    if (neu.empty()) {
                        // Empty = delete the line. An empty comment line
                        // would serve no purpose in the cfg.
                        d.script.grabs[i].commentsBefore.erase(
                            d.script.grabs[i].commentsBefore.begin() + static_cast<long>(ci));
                    } else {
                        d.script.grabs[i].commentsBefore[ci] = neu;
                    }
                    d.dirty = true;
                    editCommentGrab_ = -1;
                    editCommentLine_ = -1;
                }
            } else {
                const std::string& c = g.commentsBefore[ci];
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.68f, 0.95f, 0.90f));
                ImGui::Selectable(c.empty() ? " " : c.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick);
                ImGui::PopStyleColor();

                // Drag like a sequence.
                //
                // Separate ID "g2c_cmt": a separator must not be swappable with
                // a sequence, and vice versa.
                if (!hasFilter &&
                    ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    const std::size_t payload[3] = {static_cast<std::size_t>(active_), i, ci};
                    ImGui::SetDragDropPayload("g2c_cmt", payload, sizeof(payload));
                    ImGui::TextUnformatted(c.empty() ? " " : c.c_str());
                    ImGui::EndDragDropSource();
                }
                if (!hasFilter && ImGui::BeginDragDropTarget()) {
                    // Above comment line ci or below it.
                    const bool below = dropBelow();
                    const std::size_t at = below ? ci + 1 : ci;
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_cmt", kDropFlags)) {
                        const auto* q = static_cast<const std::size_t*>(pl->Data);
                        if (q[0] == static_cast<std::size_t>(active_)) {
                            drawDropLine(below, settings_.dpiScale);
                            if (pl->IsDelivery()) pendingCommentMove_ = {q[1], q[2], i, at, true};
                        } else if (pl->IsDelivery()) {
                            log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                        }
                    }
                    // A sequence between two separator lines: the lines above
                    // the drop line go to the inserted sequence.
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_row", kDropFlags)) {
                        const auto* q = static_cast<const std::size_t*>(pl->Data);
                        if (q[0] == static_cast<std::size_t>(active_)) {
                            drawDropLine(below, settings_.dpiScale);
                            if (pl->IsDelivery()) {
                                std::vector<std::size_t> rows;
                                for (std::size_t r = 0; r < d.selected.size(); ++r)
                                    if (d.selected[r]) rows.push_back(r);
                                if (std::find(rows.begin(), rows.end(), q[1]) == rows.end())
                                    rows = {q[1]};
                                pendingBlock_ = {rows, i, at};
                            }
                        } else if (pl->IsDelivery()) {
                            log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    editCommentGrab_ = static_cast<int>(i);
                    focusEditField_ = true;
                    editCommentLine_ = static_cast<int>(ci);
                    std::snprintf(editCommentBuf_, sizeof(editCommentBuf_), "%s", c.c_str());
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::CommentEditHint));

                // Right-click on the comment line: delete.
                if (ImGui::BeginPopupContextItem("cctx")) {
                    // The separator can be moved independently of the
                    // animations - it belongs to no sequence, even though in
                    // the file format it is attached to one.
                    if (ImGui::MenuItem(tr(S::MoveUp)))
                        moveComment(static_cast<std::size_t>(active_), i, ci, true);
                    if (ImGui::MenuItem(tr(S::MoveDown)))
                        moveComment(static_cast<std::size_t>(active_), i, ci, false);
                    ImGui::Separator();
                    if (ImGui::MenuItem(tr(S::DeleteSeq))) {
                        d.script.grabs[i].commentsBefore.erase(
                            d.script.grabs[i].commentsBefore.begin() + static_cast<long>(ci));
                        d.dirty = true;
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }

        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(i));

        ImGui::TableNextColumn();
        bool sel = d.selected[i] != 0;
        const bool isJumpTarget = (jumpToRow_ == static_cast<int>(i));
        if (isJumpTarget) {
            ImGui::SetScrollHereY(0.4f);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImVec4(0.45f, 0.32f, 0.10f, 1.0f)));
            jumpToRow_ = -1;
        }
        // On press it is decided what the drag means: on an already selected
        // row a move, otherwise a new selection. That's how Explorer does it.
        const bool wasSelected = sel;

        // AllowOverlap: the row spans all columns but must not cover the items
        // inside it. Without this no click reached the row comment - a
        // double-click on it opened the sequence dialog instead - and the
        // tooltips of the enum column never appeared.
        if (ImGui::Selectable(name.c_str(), sel || isJumpTarget,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick |
                                  ImGuiSelectableFlags_AllowOverlap)) {
            // Multi-selection like in any file manager:
            //   plain click       - only this row
            //   Ctrl + click      - add or remove a single row
            //   Shift + click     - range from the anchor to here
            const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl);
            const bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);

            if (shift && selAnchor_ >= 0) {
                std::size_t a = static_cast<std::size_t>(selAnchor_), b = i;
                if (a > b) std::swap(a, b);
                if (!ctrl) std::fill(d.selected.begin(), d.selected.end(), char{0});
                // Only visible rows. With a filter, hidden sequences lay in
                // between and got selected too - and "Delete" then hit rows
                // you couldn't see.
                for (std::size_t k = a; k <= b && k < d.selected.size(); ++k)
                    if (rowVisible(d, k)) d.selected[k] = 1;
            } else if (ctrl) {
                d.selected[i] = static_cast<char>(!sel);
                selAnchor_ = static_cast<int>(i);
            } else {
                std::fill(d.selected.begin(), d.selected.end(), char{0});
                d.selected[i] = 1;
                selAnchor_ = static_cast<int>(i);
            }

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                editRow_ = static_cast<int>(i);
                editOpen_ = true;
                editDocPath_ = d.path;
            }
        }

        // Drag out a selection with the mouse button held.
        //
        // Only starts on a NON-selected row - otherwise you could no longer
        // move an existing selection without losing it first.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !wasSelected && !ImGui::IsKeyDown(ImGuiMod_Ctrl) &&
            !ImGui::IsKeyDown(ImGuiMod_Shift)) {
            rangeSelecting_ = true;
            selAnchor_ = static_cast<int>(i);
        }
        if (rangeSelecting_ && ImGui::IsItemHovered() && selAnchor_ >= 0 &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            std::size_t a = static_cast<std::size_t>(selAnchor_), b = i;
            if (a > b) std::swap(a, b);
            std::fill(d.selected.begin(), d.selected.end(), char{0});
            for (std::size_t k = a; k <= b && k < d.selected.size(); ++k)
                if (rowVisible(d, k)) d.selected[k] = 1;
        }

        // Reorder by dragging.
        //
        // Only without a filter: the table then shows all rows, and the target
        // position is unambiguous. With a filter "here" would be ambiguous,
        // because hidden sequences lie in between.
        if (!hasFilter) {
            // The payload carries the script along. ImGui switches tabs when
            // dragging over them; without this, a drag from tab A to tab B moved
            // the row with the same number in B.
            if (wasSelected &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                const std::size_t payload[2] = {static_cast<std::size_t>(active_), i};
                ImGui::SetDragDropPayload("g2c_row", payload, sizeof(payload));
                // With several selected rows, say that all of them come along.
                const std::size_t nsel =
                    static_cast<std::size_t>(std::count(d.selected.begin(), d.selected.end(), 1));
                if (nsel > 1)
                    ImGui::Text("%s  (+%zu)", name.c_str(), nsel - 1);
                else
                    ImGui::TextUnformatted(name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                // Upper half: before, i.e. between this sequence's comment
                // lines and the sequence itself. Lower half: after, before the
                // comment lines of the next one.
                const bool below = dropBelow();
                const std::size_t nKomm = g.commentsBefore.size();
                if (const ImGuiPayload* pc = ImGui::AcceptDragDropPayload("g2c_cmt", kDropFlags)) {
                    const auto* q = static_cast<const std::size_t*>(pc->Data);
                    if (q[0] == static_cast<std::size_t>(active_)) {
                        drawDropLine(below, settings_.dpiScale);
                        if (pc->IsDelivery())
                            pendingCommentMove_ = below ? PendingCommentMove{q[1], q[2], i + 1, 0, true}
                                                        : PendingCommentMove{q[1], q[2], i, kAnhaengen, true};
                    } else if (pc->IsDelivery()) {
                        log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                    }
                }
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_row", kDropFlags)) {
                    const auto* q = static_cast<const std::size_t*>(pl->Data);
                    if (q[0] != static_cast<std::size_t>(active_)) {
                        if (pl->IsDelivery()) log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                    } else {
                        drawDropLine(below, settings_.dpiScale);
                        if (pl->IsDelivery()) {
                            const std::size_t from = q[1];
                            std::vector<std::size_t> rows;
                            for (std::size_t r = 0; r < d.selected.size(); ++r)
                                if (d.selected[r]) rows.push_back(r);
                            if (std::find(rows.begin(), rows.end(), from) == rows.end()) rows = {from};
                            // Before: this sequence's headings sit above the
                            // line and go to the inserted one.
                            pendingBlock_ = below ? PendingBlock{rows, i + 1, 0}
                                                  : PendingBlock{rows, i, nKomm};
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Right mouse button: reorder, edit, delete.
        if (ImGui::BeginPopupContextItem(("ctx" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem(withIcon(ICON_EDIT, tr(S::CtxEdit)))) {
                editRow_ = static_cast<int>(i);
                editOpen_ = true;
                editDocPath_ = d.path;
            }
            ImGui::Separator();

            // All selected rows are affected; if the clicked one isn't among
            // them, only it applies.
            std::vector<std::size_t> rows;
            for (std::size_t k = 0; k < d.selected.size(); ++k)
                if (d.selected[k]) rows.push_back(k);
            if (std::find(rows.begin(), rows.end(), i) == rows.end()) rows = {i};

            ImGui::BeginDisabled(hasFilter);
            const std::size_t first = rows.front(), last = rows.back();
            if (ImGui::MenuItem(tr(S::MoveTop), nullptr, false, first > 0))
                pendingBlock_ = {rows, 0};
            if (ImGui::MenuItem(tr(S::MoveUp), nullptr, false, first > 0))
                pendingBlock_ = {rows, first - 1};
            if (ImGui::MenuItem(tr(S::MoveDown), nullptr, false,
                                last + 1 < d.script.grabs.size()))
                pendingBlock_ = {rows, last + 2};
            if (ImGui::MenuItem(tr(S::MoveBottom), nullptr, false,
                                last + 1 < d.script.grabs.size()))
                pendingBlock_ = {rows, d.script.grabs.size()};
            ImGui::EndDisabled();
            if (hasFilter) ImGui::TextDisabled("%s", tr(S::FilterBlocksMove));

            // Move the selected rows here - the right-click says WHERE.
            //
            // Together with multi-selection this is the most convenient way to
            // gather scattered sequences: first collect them with Ctrl, then
            // right-click at the target spot.
            {
                std::vector<std::size_t> selOnly;
                for (std::size_t k = 0; k < d.selected.size(); ++k)
                    if (d.selected[k]) selOnly.push_back(k);
                const bool clickedIsSelected =
                    std::find(selOnly.begin(), selOnly.end(), i) != selOnly.end();

                if (!selOnly.empty() && !clickedIsSelected) {
                    ImGui::Separator();
                    ImGui::BeginDisabled(hasFilter);
                    char lbl[160];
                    std::snprintf(lbl, sizeof(lbl), tr(S::InsertHereCount), selOnly.size());
                    if (ImGui::MenuItem(lbl)) pendingBlock_ = {selOnly, i};
                    ImGui::EndDisabled();
                }
            }

            // Shortcut for structuring, without opening the dialog.
            if (ImGui::MenuItem(tr(S::AddDivider))) {
                d.script.grabs[i].commentsBefore.push_back(
                    "//////////////////////////////////////////");
                d.dirty = true;
            }
            if (ImGui::MenuItem(tr(S::AddComment))) {
                // Create an empty line and open it for editing right away -
                // without a detour through the dialog.
                d.script.grabs[i].commentsBefore.emplace_back();
                editCommentGrab_ = static_cast<int>(i);
                focusEditField_ = true;
                editCommentLine_ =
                    static_cast<int>(d.script.grabs[i].commentsBefore.size()) - 1;
                editCommentBuf_[0] = '\0';
                d.dirty = true;
            }

            ImGui::Separator();
            if (ImGui::MenuItem(tr(S::Copy))) {
                const std::size_t n = copyGrabs(static_cast<std::size_t>(active_), rows);
                log(LogLine::Kind::Info, trf(S::Copied, n));
            }
            if (ImGui::MenuItem(tr(S::Cut))) {
                const std::size_t n = copyGrabs(static_cast<std::size_t>(active_), rows);
                log(LogLine::Kind::Info, trf(S::Copied, n));
                pendingDelete_ = rows;
                pendingDeleteDocPath_ = d.path;
                pendingDeleteIsCut_ = true;
            }
            if (!clipboard_.empty()) {
                char lbl[128];
                std::snprintf(lbl, sizeof(lbl), tr(S::PasteBefore), clipboard_.size());
                if (ImGui::MenuItem(lbl)) pendingPaste_ = static_cast<long>(i);
                std::snprintf(lbl, sizeof(lbl), tr(S::PasteAfter), clipboard_.size());
                if (ImGui::MenuItem(lbl)) pendingPaste_ = static_cast<long>(i) + 1;
            }

            ImGui::Separator();
            if (rows.size() > 1) {
                char lbl[128];
                std::snprintf(lbl, sizeof(lbl), tr(S::DeleteSelected), rows.size());
                if (ImGui::MenuItem(withIcon(ICON_DELETE, lbl))) {
                    pendingDelete_ = rows;
                    pendingDeleteDocPath_ = d.path;
                    pendingDeleteIsCut_ = false;
                }
            } else if (ImGui::MenuItem(withIcon(ICON_DELETE, tr(S::DeleteSeq)))) {
                pendingDelete_ = {i};
                pendingDeleteDocPath_ = d.path;
                pendingDeleteIsCut_ = false;
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        {
            const int fc = frameCountOf(g.file);
            if (fc == -1) ImGui::TextDisabled("...");
            else if (fc == -2) iconText(ICON_CANCEL, kIconBad, tr(S::EnumMissing));
            else ImGui::Text("%d", fc);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", g.loop.value_or(0));
        ImGui::TableNextColumn();
        if (g.frameSpeed) ImGui::Text("%d", *g.frameSpeed);
        else ImGui::TextDisabled("%s", tr(S::SpeedAuto));
        ImGui::TableNextColumn();
        if (g.additional.empty()) ImGui::TextDisabled("-");
        else ImGui::Text("%zu", g.additional.size());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(g.file.c_str());
        ImGui::TableNextColumn();
        if (enums_.empty()) {
            ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr(S::EnumTooltipNoTable));
        } else if (enums_.contains(name)) {
            iconText(ICON_CHECK, kIconGood, tr(S::EnumOk));
        } else {
            iconText(ICON_WARNING, kIconWarn, tr(S::EnumMissing));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(tr(S::EnumTooltipMissing), name.c_str());
        }

        // Row comment: display it and edit it via double-click.
        //
        // In the .car it sits after the grab line and moves into the
        // animation.cfg at the same spot - that's how Raven does it too.
        ImGui::TableNextColumn();
        {
            const bool bearbeite = editTrailGrab_ == static_cast<int>(i);
            if (bearbeite) {
                ImGui::SetNextItemWidth(-1);
                // Request focus once when editing starts - not every frame
                // while nothing is active. Right after the double-click the
                // mouse still holds the item underneath, and the repeated
                // request never got its turn: the field appeared, but typed
                // text went nowhere.
                if (focusEditField_) {
                    ImGui::SetKeyboardFocusHere();
                    focusEditField_ = false;
                }
                const bool fertig =
                    ImGui::InputText("##tc", editTrailBuf_, sizeof(editTrailBuf_),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
                if (fertig || (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())) {
                    std::string neu = editTrailBuf_;
                    // Without a leading "//" it wouldn't be a comment line in
                    // the file, but garbage after the numbers.
                    if (!neu.empty() && neu.compare(0, 2, "//") != 0) neu = "// " + neu;
                    d.script.grabs[i].trailingComment = neu;
                    d.dirty = true;
                    editTrailGrab_ = -1;
                }
            } else {
                const std::string& tc = d.script.grabs[i].trailingComment;
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.55f, 0.58f, tc.empty() ? 0.45f : 0.95f));
                ImGui::Selectable(tc.empty() ? "..." : tc.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    editTrailGrab_ = static_cast<int>(i);
                    focusEditField_ = true;
                    std::snprintf(editTrailBuf_, sizeof(editTrailBuf_), "%s", tc.c_str());
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::TrailCommentTip));
            }
        }

        ImGui::PopID();
    }
    // Drop row at the very bottom.
    //
    // Without it a separator could only be dragged to the end if one is
    // already there - i.e. precisely not when you want to place the first
    // one.
    if (!d.script.grabs.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID("dropend");
        ImGui::Selectable("##dropend", false, ImGuiSelectableFlags_SpanAllColumns);
        if (ImGui::BeginDragDropTarget()) {
            // The line is always at the top: right after the last animation,
            // before the trailing comments.
            const std::size_t ende = d.script.grabs.size();
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_cmt", kDropFlags)) {
                const auto* q = static_cast<const std::size_t*>(pl->Data);
                if (q[0] == static_cast<std::size_t>(active_)) {
                    drawDropLine(false, settings_.dpiScale);
                    if (pl->IsDelivery()) pendingCommentMove_ = {q[1], q[2], ende, 0, true};
                } else if (pl->IsDelivery()) {
                    log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                }
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_row", kDropFlags)) {
                const auto* q = static_cast<const std::size_t*>(pl->Data);
                if (q[0] == static_cast<std::size_t>(active_)) {
                    drawDropLine(false, settings_.dpiScale);
                    if (pl->IsDelivery()) {
                        std::vector<std::size_t> rows;
                        for (std::size_t r = 0; r < d.selected.size(); ++r)
                            if (d.selected[r]) rows.push_back(r);
                        if (std::find(rows.begin(), rows.end(), q[1]) == rows.end()) rows = {q[1]};
                        pendingBlock_ = {rows, ende, 0};
                    }
                } else if (pl->IsDelivery()) {
                    log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    // Comments after the last animation.
    //
    // They aren't attached to any grab and would otherwise only show up in
    // the finished animation.cfg - i.e. where they can no longer be edited.
    for (std::size_t ti = 0; ti < d.script.trailingComments.size(); ++ti) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID("trail");
        ImGui::PushID(static_cast<int>(ti));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.68f, 0.95f, 0.90f));
        const std::string& c = d.script.trailingComments[ti];
        ImGui::Selectable(c.empty() ? " " : c.c_str(), false,
                          ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor();

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            // Trailing lines aren't attached to a grab - the origin is
            // encoded with the grab index "at the very end".
            const std::size_t payload[3] = {static_cast<std::size_t>(active_), d.script.grabs.size(), ti};
            ImGui::SetDragDropPayload("g2c_cmt", payload, sizeof(payload));
            ImGui::TextUnformatted(c.empty() ? " " : c.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            const bool below = dropBelow();
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_cmt", kDropFlags)) {
                const auto* q = static_cast<const std::size_t*>(pl->Data);
                if (q[0] == static_cast<std::size_t>(active_)) {
                    drawDropLine(below, settings_.dpiScale);
                    if (pl->IsDelivery())
                        pendingCommentMove_ = {q[1], q[2], d.script.grabs.size(),
                                               below ? ti + 1 : ti, true};
                } else if (pl->IsDelivery()) {
                    log(LogLine::Kind::Warn, tr(S::DragOtherTab));
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem("tctx")) {
            if (ImGui::MenuItem(tr(S::MoveUp)) && !d.script.grabs.empty()) {
                // Back to the last grab.
                d.script.grabs.back().commentsBefore.push_back(c);
                d.script.trailingComments.erase(
                    d.script.trailingComments.begin() + static_cast<long>(ti));
                d.dirty = true;
            }
            if (ImGui::MenuItem(tr(S::DeleteSeq))) {
                d.script.trailingComments.erase(
                    d.script.trailingComments.begin() + static_cast<long>(ti));
                d.dirty = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    ImGui::EndTable();

    // Re-attach the moved separator now - not in the middle of drawing.
    if (pendingCommentMove_.aktiv) {
        const auto m = pendingCommentMove_;
        pendingCommentMove_ = {};

        // Fetch the text and remove it from the old spot.
        std::string text;
        bool gefunden = false;
        if (m.vonGrab >= d.script.grabs.size()) {
            if (m.vonZeile < d.script.trailingComments.size()) {
                text = d.script.trailingComments[m.vonZeile];
                d.script.trailingComments.erase(
                    d.script.trailingComments.begin() + static_cast<long>(m.vonZeile));
                gefunden = true;
            }
        } else {
            auto& von = d.script.grabs[m.vonGrab].commentsBefore;
            if (m.vonZeile < von.size()) {
                text = von[m.vonZeile];
                von.erase(von.begin() + static_cast<long>(m.vonZeile));
                gefunden = true;
            }
        }

        if (gefunden) {
            auto& ziel = m.zuGrab < d.script.grabs.size() ? d.script.grabs[m.zuGrab].commentsBefore
                                                          : d.script.trailingComments;
            std::size_t pos = m.zuZeile;
            // Removed from the same list and inserted after it: the target
            // position has shifted forward by one.
            const bool gleicheListe =
                (m.vonGrab >= d.script.grabs.size() && m.zuGrab >= d.script.grabs.size()) ||
                m.vonGrab == m.zuGrab;
            if (pos != kAnhaengen && gleicheListe && m.vonZeile < pos) --pos;
            if (pos == kAnhaengen || pos > ziel.size()) pos = ziel.size();
            ziel.insert(ziel.begin() + static_cast<long>(pos), text);
            d.dirty = true;
            d.validated = false;
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) rangeSelecting_ = false;

    // Only perform the reordering HERE, after the table.
    //
    // Rearranging the list that is currently being iterated in the middle of
    // drawing is the classic way to crash. That's why the context menu only
    // remembers WHAT to do, and it is done afterwards.
    // Pasting only AFTER the table, for the same reason as reordering:
    // changing the list that is currently being iterated in the middle of
    // drawing is the classic way to crash.
    if (pendingPaste_ >= 0) {
        const std::size_t n =
            pasteGrabs(static_cast<std::size_t>(active_), static_cast<std::size_t>(pendingPaste_));
        if (n) log(LogLine::Kind::Good, trf(S::Pasted, n));
        pendingPaste_ = -1;
    }

    if (!pendingBlock_.rows.empty()) {
        PendingBlock pb = std::move(pendingBlock_);
        pendingBlock_ = {};
        std::sort(pb.rows.begin(), pb.rows.end());
        pb.rows.erase(std::unique(pb.rows.begin(), pb.rows.end()), pb.rows.end());

        // Comments above the drop line then belong to the first moved
        // sequence - re-attach them before moving, then they travel along and
        // land exactly where the line was.
        auto& grabs = d.script.grabs;
        const bool valid = pb.before <= grabs.size() && pb.rows.back() < grabs.size() &&
                           std::find(pb.rows.begin(), pb.rows.end(), pb.before) == pb.rows.end();
        if (valid && pb.splitAt > 0) {
            auto& anchor = pb.before < grabs.size() ? grabs[pb.before].commentsBefore
                                                    : d.script.trailingComments;
            const std::size_t k = std::min(pb.splitAt, anchor.size());
            auto& first = grabs[pb.rows.front()].commentsBefore;
            first.insert(first.begin(), anchor.begin(), anchor.begin() + static_cast<long>(k));
            anchor.erase(anchor.begin(), anchor.begin() + static_cast<long>(k));
        }

        const std::size_t n = moveGrabs(static_cast<std::size_t>(active_), pb.rows, pb.before);
        if (n) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), tr(S::Moved), std::to_string(n).c_str());
            log(LogLine::Kind::Info, msg);
        }
    }
}

void App::drawTabs() {
    if (docs_.empty()) {
        ImGui::TextDisabled("%s", tr(S::NoScriptOpen));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr(S::HintOpenFolder1));
        ImGui::TextDisabled("%s", tr(S::HintOpenFolder2));
        return;
    }

    if (!ImGui::BeginTabBar("cars", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton |
                                        ImGuiTabBarFlags_FittingPolicyScroll))
        return;

    std::vector<std::size_t> closeClicked;
    if (selectTab_ >= static_cast<int>(docs_.size())) selectTab_ = -1;
    for (std::size_t i = 0; i < docs_.size();) {
        Document& d = docs_[i];
        std::string label = d.title;
        if (d.dirty) label += " *";
        label += "###" + d.path;

        bool open = true;
        // Evaluate tab close buttons only after the loop: shrinking the list
        // that is currently being iterated in the middle of drawing doesn't
        // end well. The tab only selects; the table is drawn ONCE below.
        //
        // Inside the tab, the table would have its own ID per tab, and ImGui
        // would then remember the column widths twenty times separately.
        // Outside, the ID is stable, and a width set once applies to all
        // scripts and survives a restart.
        // Tab selected from outside (double-click on a .car, restore at
        // startup, opening an already open script).
        //
        // Previously only active_ was set for this. ImGui knew nothing about
        // it, kept showing its own tab - at startup the first one - and
        // overwrote active_ with it again in the same frame. Anyone who
        // double-clicked a .car that was in the 24th tab ended up in the first.
        const bool soll = static_cast<int>(i) == selectTab_;
        if (ImGui::BeginTabItem(label.c_str(), &open,
                                soll ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
            // While the selection is still pending, ImGui shows the old tab for
            // one frame. That one must not reset active_.
            if (selectTab_ < 0 || soll) {
                active_ = static_cast<int>(i);
                selectTab_ = -1;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", d.path.c_str());
        if (!open) closeClicked.push_back(i);
        ++i;
    }
    ImGui::EndTabBar();

    // Only close after the tab bar - and with a confirmation if the script
    // has changes. The close button used to discard them without warning.
    if (!closeClicked.empty()) requestClose(closeClicked);

    // The sequence dialog belongs to one script. If another tab becomes
    // active, it closes - otherwise it would edit the same row number in the
    // new script.
    if (editOpen_ && (active_ < 0 || active_ >= static_cast<int>(docs_.size()) ||
                      docs_[static_cast<std::size_t>(active_)].path != editDocPath_)) {
        editOpen_ = false;
        editRow_ = -1;
    }

    if (active_ >= 0 && active_ < static_cast<int>(docs_.size())) {
        Document& d = docs_[static_cast<std::size_t>(active_)];
        startFrameWorker(d);
        ImGui::TextDisabled("%s", d.path.c_str());
        drawSequenceTable(d);
    }
}

// Skeleton preview.
//
// Drawing is done with ImGui's draw list, not with DirectX. A skeleton needs
// no graphics API: ImGui draws a few hundred lines per frame effortlessly,
// and the code thus stays in one file that can be checked.
void App::drawPreviewPanel() {
    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
    auto& ex = extract_;

    if (!ex.loaded) {
        ImGui::TextDisabled("%s", tr(S::PreviewNoGla));
        return;
    }

    // Sequence selection.
    // Without a sequence there is nothing to play. The access to seqs[0] used
    // to run anyway - with an empty list, out of bounds.
    if (ex.seqs.empty()) {
        ImGui::TextDisabled("%s", tr(S::PreviewNoGla));
        return;
    }
    if (previewSeq_ < 0 || previewSeq_ >= static_cast<int>(ex.seqs.size())) previewSeq_ = 0;
    const ExtractSeq& seq = ex.seqs[static_cast<std::size_t>(previewSeq_)];

    ImGui::SetNextItemWidth(320 * k);
    if (ImGui::BeginCombo(tr(S::PreviewSeq), seq.name.c_str())) {
        // With a thousand sequences, show only those matching the filter.
        const std::string needle = extractFilter_;
        for (std::size_t i = 0; i < ex.seqs.size(); ++i) {
            if (!needle.empty() && ex.seqs[i].name.find(needle) == std::string::npos) continue;
            if (ImGui::Selectable(ex.seqs[i].name.c_str(),
                                  static_cast<int>(i) == previewSeq_)) {
                previewSeq_ = static_cast<int>(i);
                playback_.frame = 0;
                playback_.accumulator = 0.0f;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180 * k);
    ImGui::InputTextWithHint("##pvfilter", tr(S::FilterHint), extractFilter_,
                             sizeof(extractFilter_));

    // Controls.
    if (ImGui::Button(playback_.playing ? withIcon(ICON_CANCEL, tr(S::PreviewPause))
                                        : withIcon(ICON_BUILD, tr(S::PreviewPlay))))
        playback_.playing = !playback_.playing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-260 * k);
    int f = playback_.frame;
    if (ImGui::SliderInt("##pvframe", &f, 0, std::max(0, seq.count - 1))) {
        playback_.frame = f;
        playback_.playing = false;
    }
    ImGui::SameLine();
    ImGui::Text(tr(S::PreviewFrame), playback_.frame + 1, seq.count);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr(S::PreviewReset))) previewCam_ = PreviewCamera{};

    // Advance time. The rate comes from the animation.cfg, not from the UI
    // frame rate: an animation at 20 frames per second should run at 20 even
    // at 144 Hz.
    const double now = ImGui::GetTime();
    const float dt = previewLastTime_ > 0.0
                         ? static_cast<float>(std::min(0.1, now - previewLastTime_))
                         : 0.0f;
    previewLastTime_ = now;
    advancePlayback(playback_, dt, seq.fps, seq.count);
    if (playback_.frame >= seq.count) playback_.frame = 0;

    // World poses of the current frame.
    const int glaFrame = seq.start + playback_.frame;
    const auto world = boneWorldMatrices(ex.gla, glaFrame);
    if (world.empty()) return;

    // Canvas.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, std::max(120.0f * k, avail.y));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pvcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    // On the first frame and after a reset, fit everything into view.
    if (previewCam_.distance <= 0.0f || previewCam_.distance == PreviewCamera{}.distance)
        frameAll(previewCam_, computeBounds(world));

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        previewCam_.yawDeg -= d.x * 0.4f;
        previewCam_.pitchDeg += d.y * 0.4f;
        // Not past the pole: otherwise the view flips over.
        previewCam_.pitchDeg = std::clamp(previewCam_.pitchDeg, -89.0f, 89.0f);
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        previewCam_.distance *= std::pow(0.9f, ImGui::GetIO().MouseWheel);
        previewCam_.distance = std::max(1.0f, previewCam_.distance);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(24, 26, 30, 255));

    const auto lines = buildBoneLines(ex.gla, world, previewCam_, size.x, size.y);
    for (const auto& l : lines) {
        // Depth as brightness: what's farther away gets darker. Without this,
        // for a skeleton seen from the front you can't tell which arm is in
        // front.
        const float t = std::clamp(l.depth / (previewCam_.distance * 1.6f), 0.0f, 1.0f);
        const int v = static_cast<int>(230.0f - 130.0f * t);
        dl->AddLine(ImVec2(origin.x + l.x0, origin.y + l.y0),
                    ImVec2(origin.x + l.x1, origin.y + l.y1),
                    IM_COL32(v, v, static_cast<int>(static_cast<float>(v) * 0.85f), 255),
                    1.6f * k);
    }
    for (const auto& l : lines)
        dl->AddCircleFilled(ImVec2(origin.x + l.x1, origin.y + l.y1), 2.0f * k,
                            IM_COL32(120, 190, 255, 255));

    // Header line in the corner.
    char info[128];
    std::snprintf(info, sizeof(info), tr(S::PreviewBones), world.size());
    dl->AddText(ImVec2(origin.x + 8 * k, origin.y + 6 * k), IM_COL32(160, 160, 165, 255), info);
    dl->AddText(ImVec2(origin.x + 8 * k, origin.y + 22 * k), IM_COL32(120, 120, 125, 255),
                tr(S::PreviewHint));
}

// Error list of the active script.
//
// "9 errors" alone is worthless - you need to see WHICH ones and where. A
// click jumps into the table and highlights the row.
// Mode bar on the left edge.
//
// Two buttons instead of tabs at the top: switching changes the WHOLE UI,
// not just one section. On the edge it's visible enough that you don't
// accidentally work in the wrong mode.
void App::drawModeBar() {
    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;

    const struct {
        Mode        m;
        const char* icon;
        S           label;
        S           hint;
        IconColor   col;
    } modes[] = {
        {Mode::Build, ICON_BUILD, S::ModeBuild, S::ModeBuildHint, kIconGood},
        {Mode::Extract, ICON_OPEN_FILE, S::ModeExtract, S::ModeExtractHint, kIconAccent},
        {Mode::Preview, ICON_DOCUMENT, S::ModePreview, S::ModePreviewHint, kIconInfo},
    };

    // Determine the width from the TEXT, don't guess.
    //
    // A fixed number fits at best one language and one screen resolution.
    // "XSI -> GLA" was cut off in English, while the same number would have
    // been too wide for the shorter Chinese labels.
    float textW = 0.0f;
    for (const auto& m : modes)
        textW = std::max(textW, ImGui::CalcTextSize(withIcon(m.icon, tr(m.label))).x);

    const ImGuiStyle& st = ImGui::GetStyle();
    const float barW = textW + st.FramePadding.x * 2.0f + st.WindowPadding.x * 2.0f + 8.0f * k;

    ImGui::BeginChild("modebar", ImVec2(barW, 0), ImGuiChildFlags_Borders);

    for (const auto& m : modes) {
        const bool active = mode_ == m.m;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.48f, 0.76f, 1.0f));
        }
        // Draw the button empty, then put icon and text on top - that way
        // the icon gets its own color.
        //
        // For the active mode it stays white: on the blue background a second
        // color would look busy, and the background already tells which mode
        // is running.
        const ImVec2 bpos = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(m.m));
        const bool hit = ImGui::Button("##mode", ImVec2(-1, 44 * k));
        ImGui::PopID();
        if (hit) mode_ = m.m;

        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 tS = ImGui::CalcTextSize(tr(m.label));
            const bool hasIcon = iconsAvailable() && m.icon && *m.icon;
            const ImVec2 iS = hasIcon ? ImGui::CalcTextSize(m.icon) : ImVec2(0, 0);
            const float gap = hasIcon ? ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
            float x = bpos.x + ImGui::GetStyle().FramePadding.x + 4.0f * k;
            const float y = bpos.y + (44 * k - tS.y) * 0.5f;
            if (hasIcon) {
                const ImU32 ic =
                    active ? IM_COL32_WHITE
                           : IM_COL32(static_cast<int>(m.col.r * 255),
                                      static_cast<int>(m.col.g * 255),
                                      static_cast<int>(m.col.b * 255), 255);
                dl->AddText(ImVec2(x, y), ic, m.icon);
                x += iS.x + gap;
            }
            dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), tr(m.label));
        }
        if (active) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(m.hint));
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

// UI for the reverse direction.
void App::drawExtractPanel() {
    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
    auto& ex = extract_;

    if (iconButton(ICON_OPEN_FILE, kIconAccent, tr(S::OpenGla))) {
        const auto f = askFiles("extractgla", tr(S::DlgTitleGla),
                                "GLA (*.gla)\0*.gla\0Alle\0*.*\0", false);
        if (!f.empty()) loadGlaForExtract(f.front());
    }
    // The two companion files are searched for automatically on open. The
    // buttons remain for the case that they live somewhere else.
    ImGui::SameLine();
    ImGui::BeginDisabled(!ex.loaded);
    if (ImGui::Button(withIcon(ICON_DOCUMENT, tr(S::OpenCfg)))) {
        const auto f = askFiles("extractcfg", tr(S::OpenCfg),
                                "animation.cfg\0*.cfg\0Alle\0*.*\0", false);
        if (!f.empty()) loadAnimationCfg(f.front());
    }
    ImGui::SameLine();
    if (ImGui::Button(withIcon(ICON_DOCUMENT, tr(S::OpenFrames)))) {
        const auto f = askFiles("extractframes", tr(S::OpenFrames),
                                ".frames\0*.frames\0Alle\0*.*\0", false);
        if (!f.empty()) loadFramesFile(f.front());
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!ex.loaded || ex.seqs.size() < 2);
    if (iconButton(ICON_CHECK, kIconAccent, tr(S::Compare))) {
        const auto f = askFiles("comparecfg", tr(S::Compare),
                                "animation.cfg\0*.cfg\0Alle\0*.*\0", false);
        if (!f.empty()) loadCompareCfg(f.front());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::CompareTip));
    ImGui::EndDisabled();

    if (!ex.loaded) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr(S::NoGlaOpen));
        ImGui::TextDisabled("%s", tr(S::NoGlaHint));
        if (!ex.error.empty())
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", ex.error.c_str());
        return;
    }

    ImGui::TextDisabled("%s", ex.glaPath.c_str());
    ImGui::Text(tr(S::GlaInfo), ex.gla.numFrames, ex.gla.skeleton.bones.size());
    // Make visible what was found - otherwise you're left wondering whether
    // the companion files took effect.
    ImGui::SameLine();
    if (ex.cfgPath.empty())
        iconText(ICON_WARNING, kIconWarn, tr(S::NoCfgWarning));
    else
        iconText(ICON_CHECK, kIconGood, fs::path(ex.cfgPath).filename().string().c_str());

    ImGui::SameLine();
    if (ex.framesPath.empty())
        ImGui::TextDisabled("%s", tr(S::NoFramesHint));
    else
        iconText(ICON_CHECK, kIconGood, fs::path(ex.framesPath).filename().string().c_str());

    ImGui::TextUnformatted(tr(S::OriginLabel));
    ImGui::SameLine();
    if (ex.origin)
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), tr(S::OriginDetected), (*ex.origin)[0],
                           (*ex.origin)[1], (*ex.origin)[2]);
    else
        ImGui::TextDisabled("%s", tr(S::OriginNone));

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ex.framesPath.empty())
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr(S::FramesMissing));
    else
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), tr(S::FramesLoaded), ex.withMotion);

    // Target folder.
    ImGui::TextUnformatted(tr(S::ExportTarget));
    ImGui::SameLine();
    if (ex.outDir.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1), "%s", tr(S::NotSet));
    else
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", ex.outDir.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(withIcon(ICON_FOLDER_OPEN, tr(S::ChooseFolder)))) {
        const std::string p = askFolder("extractout", tr(S::ExportTarget));
        if (!p.empty()) ex.outDir = p;
    }

    // Comparison baseline.
    if (!ex.compareNames.empty()) {
        std::size_t fehlt = 0;
        for (const auto& q : ex.seqs)
            if (!existsInCompare(q.name)) ++fehlt;

        ImGui::TextDisabled("%s:", fs::path(ex.comparePath).filename().string().c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), tr(S::CompareCount), fehlt);
        ImGui::SameLine();
        ImGui::Checkbox(tr(S::OnlyMissing), &ex.onlyMissing);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(S::SelectMissing))) {
            const std::size_t n = selectMissing();
            char m[96];
            std::snprintf(m, sizeof(m), tr(S::SelectionHint), n);
            log(LogLine::Kind::Info, m);
        }
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(220 * k);
    ImGui::InputTextWithHint("##exfilter", tr(S::FilterHint), extractFilter_,
                             sizeof(extractFilter_));
    ImGui::SameLine();
    ImGui::TextDisabled(tr(S::SeqCount), ex.seqs.size());

    std::size_t nsel = 0;
    for (const char c : ex.selected)
        if (c) ++nsel;

    ImGui::SameLine();
    ImGui::BeginDisabled(ex.outDir.empty() || nsel == 0);
    char lbl[128];
    std::snprintf(lbl, sizeof(lbl), tr(S::ExportSelected), nsel);
    if (ImGui::Button(withIcon(ICON_SAVE, lbl))) {
        std::vector<std::size_t> rows;
        for (std::size_t i = 0; i < ex.selected.size(); ++i)
            if (ex.selected[i]) rows.push_back(i);
        exportWithConfirm(rows, false);
    }
    ImGui::EndDisabled();

    // Bind pose selection.
    //
    // Deliberately placed here next to the export buttons and not in the
    // settings: it only affects the export, and whoever needs it looks for it
    // here.
    // dotXSI version.
    ImGui::SameLine();
    ImGui::TextDisabled("dotXSI");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * k);
    {
        const char* vn[] = {"3.0", "3.5"};
        int cur = ex.xsiVersion == xsiexp::ExportOptions::Version::V30 ? 0 : 1;
        if (ImGui::Combo("##xsiver", &cur, vn, 2))
            ex.xsiVersion = cur == 0 ? xsiexp::ExportOptions::Version::V30
                                     : xsiexp::ExportOptions::Version::V35;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::XsiVersionTip));

    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(S::BasePoseLabel));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * k);
    {
        const char* namen[] = {tr(S::BasePoseWorld), tr(S::BasePoseLocal), tr(S::BasePoseNone)};
        int cur = ex.basePose == xsiexp::ExportOptions::BasePose::World   ? 0
                  : ex.basePose == xsiexp::ExportOptions::BasePose::Local ? 1
                                                                          : 2;
        if (ImGui::Combo("##basepose", &cur, namen, 3))
            ex.basePose = cur == 0   ? xsiexp::ExportOptions::BasePose::World
                          : cur == 1 ? xsiexp::ExportOptions::BasePose::Local
                                     : xsiexp::ExportOptions::BasePose::None;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::BasePoseTip));

    ImGui::SameLine();
    ImGui::BeginDisabled(ex.outDir.empty() || ex.cfgPath.empty());
    if (iconButton(ICON_DOCUMENT, kIconGood, tr(S::ExportAllWithCar))) {
        exportWithConfirm({}, true);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::ExportAllWithCarTip));
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(ex.outDir.empty());
    if (iconButton(ICON_SAVE_ALL, kIconInfo, tr(S::ExportAll))) {
        std::vector<std::size_t> rows(ex.seqs.size());
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = i;
        exportWithConfirm(rows, false);
    }
    ImGui::EndDisabled();

    const ImGuiTableFlags fl = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_Reorderable;
        const int excols = ex.compareNames.empty() ? 5 : 6;
    if (!ImGui::BeginTable("extract", excols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(S::ColSequence), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(tr(S::ColStart), ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn(tr(S::ColCount), ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn(tr(S::ColLoopFrame), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(tr(S::ColFps), ImGuiTableColumnFlags_WidthFixed, 60);
    if (!ex.compareNames.empty())
        ImGui::TableSetupColumn(tr(S::CompareCol), ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableHeadersRow();

    const std::string needle = extractFilter_;
    for (std::size_t i = 0; i < ex.seqs.size(); ++i) {
        const ExtractSeq& s = ex.seqs[i];
        if (!needle.empty() && s.name.find(needle) == std::string::npos) continue;
        const bool inOther = ex.compareNames.empty() || existsInCompare(s.name);
        if (ex.onlyMissing && !ex.compareNames.empty() && inOther) continue;

        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextColumn();
        const bool sel = ex.selected[i] != 0;
        if (ImGui::Selectable(s.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
            const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl);
            const bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);
            if (shift && extractAnchor_ >= 0) {
                std::size_t a = static_cast<std::size_t>(extractAnchor_), b = i;
                if (a > b) std::swap(a, b);
                if (!ctrl) std::fill(ex.selected.begin(), ex.selected.end(), char{0});
                // Only what's currently visible: filter and "missing only"
                // hide rows that would otherwise be silently exported too.
                for (std::size_t j = a; j <= b && j < ex.selected.size(); ++j) {
                    const auto& q = ex.seqs[j];
                    if (!needle.empty() && q.name.find(needle) == std::string::npos) continue;
                    if (ex.onlyMissing && !ex.compareNames.empty() && existsInCompare(q.name))
                        continue;
                    ex.selected[j] = 1;
                }
            } else if (ctrl) {
                ex.selected[i] = static_cast<char>(!sel);
                extractAnchor_ = static_cast<int>(i);
            } else {
                std::fill(ex.selected.begin(), ex.selected.end(), char{0});
                ex.selected[i] = 1;
                extractAnchor_ = static_cast<int>(i);
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", s.start);
        ImGui::TableNextColumn();
        ImGui::Text("%d", s.count);
        ImGui::TableNextColumn();
        if (s.loopFrame < 0) ImGui::TextDisabled("-");
        else ImGui::Text("%d", s.loopFrame);
        ImGui::TableNextColumn();
        ImGui::Text("%d", s.fps);
        if (!ex.compareNames.empty()) {
            ImGui::TableNextColumn();
            // Icon before the word: the eye takes in the column without
            // reading.
            if (inOther) iconText(ICON_CHECK, kIconMuted, tr(S::PresentHere));
            else iconText(ICON_WARNING, kIconWarn, tr(S::MissingHere));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::drawIssues() {
    if (active_ < 0 || active_ >= static_cast<int>(docs_.size())) return;
    Document& d = docs_[static_cast<std::size_t>(active_)];

    // Take over duplicate names from the last build attempt as messages.
    //
    // That way the error ends up where you can click it - a log line alone
    // doesn't jump anywhere.
    {
        std::lock_guard<std::mutex> lock(dupMutex_);
        if (!pendingDuplicates_.empty() && pendingDupDoc_ == d.title) {
            for (const auto& name : pendingDuplicates_) {
                car::Issue is;
                is.level = car::Issue::Level::Error;
                is.sequence = name;
                is.message = trf(S::DupIssue, name.c_str());
                d.validation.issues.push_back(std::move(is));
                ++d.validation.errors;
            }
            d.validated = true;
            pendingDuplicates_.clear();
            pendingDupDoc_.clear();
        }
    }

    if (!d.validated) {
        ImGui::TextDisabled("%s", tr(S::NotValidatedHint));
        return;
    }
    if (d.validation.issues.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s", tr(S::NoIssues));
        return;
    }

    const ImGuiTableFlags fl = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("issues", 3, fl, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(S::ColLevel), ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn(tr(S::ColSequence), ImGuiTableColumnFlags_WidthFixed, 220);
    ImGui::TableSetupColumn(tr(S::ColMessage), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < d.validation.issues.size(); ++i) {
        const auto& is = d.validation.issues[i];
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(i));

        ImGui::TableNextColumn();
        ImVec4 col(0.8f, 0.8f, 0.82f, 1);
        switch (is.level) {
            case car::Issue::Level::Error: col = ImVec4(1.0f, 0.40f, 0.40f, 1); break;
            case car::Issue::Level::Warning: col = ImVec4(1.0f, 0.80f, 0.30f, 1); break;
            default: break;
        }
        const bool sel = (issueSelected_ == static_cast<int>(i));
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns)) {
            issueSelected_ = static_cast<int>(i);
            // Jump to the affected sequences.
            //
            // Collect ALL matches, not just the first: with a duplicate name
            // there are two, and you want to see both to decide which one
            // stays.
            if (!is.sequence.empty()) {
                // Uppercase both sides. The check reports names in their
                // original spelling ("BOTH_Walk1_galen"); compared with the
                // uppercased row, the click never jumped.
                std::string want = is.sequence;
                for (auto& c : want)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::vector<std::size_t> treffer;
                for (std::size_t g = 0; g < d.script.grabs.size(); ++g) {
                    const auto& gr = d.script.grabs[g];
                    std::string nm = gr.enumName ? *gr.enumName : gr.derivedName();
                    for (auto& c : nm)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    bool hit = (nm == want);
                    for (const auto& a : gr.additional) {
                        std::string an = a.name;
                        for (auto& c : an)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (an == want) hit = true;
                    }
                    if (hit) treffer.push_back(g);
                }

                if (!treffer.empty()) {
                    // Mark them all so they are visible in the table.
                    std::fill(d.selected.begin(), d.selected.end(), char{0});
                    for (const std::size_t g : treffer)
                        if (g < d.selected.size()) d.selected[g] = 1;

                    // And jump to them in turn: clicking again goes to the
                    // next occurrence, then back to the first.
                    if (issueCycleFor_ != static_cast<int>(i)) {
                        issueCycleFor_ = static_cast<int>(i);
                        issueCycleIdx_ = 0;
                    } else {
                        issueCycleIdx_ = (issueCycleIdx_ + 1) % static_cast<int>(treffer.size());
                    }
                    jumpToRow_ = static_cast<int>(treffer[static_cast<std::size_t>(issueCycleIdx_)]);
                }
            }
        }
        ImGui::SameLine(0, 0);
        ImGui::TextColored(col, "%s", is.levelName());

        ImGui::TableNextColumn();
        if (is.sequence.empty()) ImGui::TextDisabled("-");
        else ImGui::TextUnformatted(is.sequence.c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(is.message.c_str());
        if (is.line) {
            ImGui::SameLine();
            ImGui::TextDisabled(tr(S::ColLine), is.line);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// Selection list of the enums.
//
// The name is NOT typed freely. A typo would otherwise create a sequence
// that doesn't exist in the game code - and that only shows up in the game,
// as an animation that doesn't play. Assimilate does it the same way:
// pick or delete, nothing in between.
bool App::drawEnumChooser(const char* popupId, std::string& target) {
    bool chosen = false;
    if (!ImGui::BeginPopup(popupId)) return false;

    if (enums_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr(S::ChooserNoTable));
        ImGui::TextDisabled("%s", tr(S::EnumsAvailable));
        ImGui::EndPopup();
        return false;
    }

    ImGui::SetNextItemWidth(320 * settings_.dpiScale);
    ImGui::InputTextWithHint("##ef", tr(S::ChooserFilter), enumFilter_, sizeof(enumFilter_));
    ImGui::SameLine();
    ImGui::TextDisabled(tr(S::ChooserCount), enums_.size());

    const std::string needle = enumFilter_;
    ImGui::BeginChild("elist", ImVec2(420 * settings_.dpiScale, 320 * settings_.dpiScale));
    std::size_t shown = 0;
    for (const auto& e : enums_.names) {
        if (e.rfind("MAX_", 0) == 0) continue;
        if (!needle.empty() && e.find(needle) == std::string::npos) continue;
        if (++shown > 500) {
            ImGui::TextDisabled("%s", tr(S::ChooserNarrow));
            break;
        }
        if (ImGui::Selectable(e.c_str(), e == target)) {
            target = e;
            chosen = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
    return chosen;
}

// Edit dialog for a sequence, opened by double-click as in Assimilate.
void App::drawSequenceDialog(Document& d) {
    if (!editOpen_) return;
    if (editRow_ < 0 || editRow_ >= static_cast<int>(d.script.grabs.size())) {
        editOpen_ = false;
        return;
    }
    // Only for the script the dialog was opened for.
    if (d.path != editDocPath_) {
        editOpen_ = false;
        return;
    }
    car::GrabDirective& g = d.script.grabs[static_cast<std::size_t>(editRow_)];
    const std::string name = g.enumName ? *g.enumName : g.derivedName();

    ImGui::SetNextWindowSize(ImVec2(760 * settings_.dpiScale, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin((trf(S::DlgSeqTitle, name.c_str()) + "###seqdlg").c_str(), &editOpen_,
                      ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", tr(S::DlgSource));
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", g.file.c_str());
        ImGui::SetNextItemWidth(-90 * settings_.dpiScale);
        ImGui::InputText("##file", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button((std::string(tr(S::DlgChoose)) + "##f").c_str()) && platform_.openFiles) {
            const auto f = askFiles("xsi", tr(S::DlgTitleXsi), "dotXSI (*.xsi)\0*.xsi\0Alle\0*.*\0", false);
            if (!f.empty()) {
                std::string rel = f.front();
                if (!settings_.baseDir.empty()) {
                    std::error_code ec;
                    const auto r = fs::relative(f.front(), settings_.baseDir, ec);
                    if (!ec && !r.empty() && r.string().rfind("..", 0) != 0) rel = r.generic_string();
                }
                g.file = rel;
                d.dirty = true;
                d.validated = false;
            }
        }
    }
    if (const int fc = frameCountOf(g.file); fc >= 0)
        ImGui::TextDisabled(tr(S::DlgFrameCount), fc);

    ImGui::SeparatorText(tr(S::DlgMaster));
    {
        // Display only, no typing.
        ImGui::SetNextItemWidth(300 * settings_.dpiScale);
        char nb[128];
        std::snprintf(nb, sizeof(nb), "%s", name.c_str());
        ImGui::InputText("##enum", nb, sizeof(nb), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button((std::string(tr(S::DlgChoose)) + "##m").c_str())) {
            enumFilter_[0] = '\0';
            ImGui::OpenPopup("enum_master");
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(tr(S::DlgClear)) + "##m").c_str())) {
            g.enumName.reset();
            d.dirty = true;
            d.validated = false;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Enum");

        std::string picked = name;
        if (drawEnumChooser("enum_master", picked)) {
            g.enumName = picked;
            d.dirty = true;
            d.validated = false;
        }

        if (!enums_.empty()) {
            if (enums_.contains(name)) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s", tr(S::DlgInAnims));
            else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr(S::DlgNotInAnims));
        }

        int loop = g.loop.value_or(-1);
        ImGui::SetNextItemWidth(120 * settings_.dpiScale);
        if (ImGui::InputInt(tr(S::DlgLoopFrame), &loop)) { g.loop = loop; d.dirty = true; }

        int speed = g.frameSpeed.value_or(0);
        ImGui::SetNextItemWidth(120 * settings_.dpiScale);
        if (ImGui::InputInt(tr(S::DlgFrameSpeed), &speed)) {
            if (speed == 0) g.frameSpeed.reset();
            else g.frameSpeed = speed;
            d.dirty = true;
        }
    }

    ImGui::SeparatorText(tr(S::DlgExtra));
    ImGui::TextDisabled("%s", tr(S::DlgExtraHint));

    if (ImGui::BeginTable("adds", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr(S::ColEnum), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(tr(S::DlgStart), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(tr(S::ColFrames), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(tr(S::ColLoop), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(tr(S::ColSpeed), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();

        int remove = -1;
        for (std::size_t a = 0; a < g.additional.size(); ++a) {
            auto& ad = g.additional[a];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(a));

            ImGui::TableNextColumn();
            char nb[128];
            std::snprintf(nb, sizeof(nb), "%s", ad.name.c_str());
            ImGui::SetNextItemWidth(-90 * settings_.dpiScale);
            ImGui::InputText("##n", nb, sizeof(nb), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::SmallButton(withIcon(ICON_FOLDER_OPEN, tr(S::ChooseFolder)))) {
                enumFilter_[0] = '\0';
                ImGui::OpenPopup("enum_add");
            }
            if (drawEnumChooser("enum_add", ad.name)) { d.dirty = true; d.validated = false; }
            if (!enums_.empty() && !enums_.contains(ad.name)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "!");
            }

            const auto intCol = [&](const char* id, int& v) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt(id, &v, 0, 0)) { d.dirty = true; d.validated = false; }
            };
            intCol("##s", ad.targetOffset);
            intCol("##c", ad.frameCount);
            intCol("##l", ad.loopFrame);
            intCol("##p", ad.frameSpeed);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(iconsAvailable() ? ICON_DELETE : "X")) remove = static_cast<int>(a);
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (remove >= 0) {
            g.additional.erase(g.additional.begin() + remove);
            d.dirty = true;
            d.validated = false;
        }
    }

    // Comment that stands BEFORE this sequence.
    //
    // It is stored in the script and ends up in the generated animation.cfg -
    // that way a list with a thousand entries can be structured instead of
    // leaving behind a wall of numbers.
    ImGui::Separator();
    ImGui::TextUnformatted(tr(S::DlgComment));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::DlgCommentTip));
    {
        // Edit the lines as one text, one entry per line.
        std::string joined;
        for (const auto& c : g.commentsBefore) {
            if (!joined.empty()) joined += "\n";
            joined += c;
        }
        // Size the buffer by content, with room for typing. With a fixed 2048
        // characters the first keystroke cut off a longer block.
        std::vector<char> buf(joined.size() + 4096, '\0');
        std::memcpy(buf.data(), joined.data(), joined.size());
        if (ImGui::InputTextMultiline("##comment", buf.data(), buf.size(),
                                      ImVec2(-1, 70 * settings_.dpiScale))) {
            g.commentsBefore.clear();
            std::istringstream is{std::string(buf.data())};
            std::string line;
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                g.commentsBefore.push_back(line);
            }
            // Remove trailing empty lines: they come from typing and would
            // serve no purpose in the cfg.
            while (!g.commentsBefore.empty() && g.commentsBefore.back().empty())
                g.commentsBefore.pop_back();
            d.dirty = true;
        }
    }
    if (ImGui::Button(tr(S::AddDivider))) {
        g.commentsBefore.push_back("//////////////////////////////////////////");
        d.dirty = true;
    }
    ImGui::Separator();

    if (ImGui::Button(tr(S::DlgAddExtra))) {
        car::GrabDirective::Additional a;
        a.name = name + "_2";
        a.frameCount = 1;
        a.loopFrame = -1;
        a.frameSpeed = g.frameSpeed.value_or(20);
        g.additional.push_back(std::move(a));
        d.dirty = true;
        d.validated = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(S::DlgClose))) editOpen_ = false;

    ImGui::End();
}

std::string App::logAsText() const {
    std::ostringstream o;

    // Header with what would otherwise be asked for in follow-up questions.
    o << "g2c - Protokoll\n";
    o << "Gebaut: " << __DATE__ << " " << __TIME__ << ", " << (sizeof(void*) * 8) << " Bit, "
      << g2::defaultThreadCount() << " Kerne\n";
    o << "Laufzeit: " << (staticRuntime() ? "fest eingebaut" : "dynamisch gebunden") << "\n";
    if (!settings_.baseDir.empty()) o << "Assetwurzel: " << settings_.baseDir << "\n";
    if (!settings_.referenceGla.empty()) o << "Referenz-GLA: " << settings_.referenceGla << "\n";
    o << "Skripte offen: " << docs_.size() << "\n";
    o << "----------------------------------------\n";

    std::lock_guard<std::mutex> lock(logMutex_);
    for (const auto& l : log_) {
        // Prepend the kind: the color is lost when copying, and it's exactly
        // what distinguishes a note from an error.
        const char* k = l.kind == LogLine::Kind::Bad    ? "[FEHLER]  "
                        : l.kind == LogLine::Kind::Warn ? "[WARNUNG] "
                        : l.kind == LogLine::Kind::Good ? "[OK]      "
                                                        : "          ";
        o << k << l.text << "\n";
    }
    return o.str();
}

void App::iconText(const char* icon, const IconColor& col, const char* text) {
    if (iconsAvailable() && icon && *icon) {
        ImGui::TextColored(ImVec4(col.r, col.g, col.b, col.a), "%s", icon);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    if (text && *text) ImGui::TextUnformatted(text);
}

bool App::iconButton(const char* icon, const IconColor& col, const char* text, bool small) {
    const bool haveIcon = iconsAvailable() && icon && *icon;
    const ImGuiStyle& st = ImGui::GetStyle();

    // Compute the width ourselves: icon, spacing, text.
    const ImVec2 tSize = ImGui::CalcTextSize(text);
    const ImVec2 iSize = haveIcon ? ImGui::CalcTextSize(icon) : ImVec2(0, 0);
    const float gap = haveIcon ? st.ItemInnerSpacing.x : 0.0f;
    const float w = iSize.x + gap + tSize.x + st.FramePadding.x * 2.0f;
    const float h = tSize.y + (small ? 0.0f : st.FramePadding.y * 2.0f);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    const bool pressed = ImGui::Button("##ib", ImVec2(w, h));
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = pos.y + (h - tSize.y) * 0.5f;
    float x = pos.x + st.FramePadding.x;
    if (haveIcon) {
        dl->AddText(ImVec2(x, y), IM_COL32(static_cast<int>(col.r * 255),
                                           static_cast<int>(col.g * 255),
                                           static_cast<int>(col.b * 255), 255),
                    icon);
        x += iSize.x + gap;
    }
    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), text);
    return pressed;
}

void App::drawLog() {
    // Button bar above the log.
    if (ImGui::SmallButton(withIcon(ICON_DOCUMENT, tr(S::CopyLog)))) {
        const std::string txt = logAsText();
        ImGui::SetClipboardText(txt.c_str());
        std::size_t zeilen = 0;
        for (const char c : txt)
            if (c == '\n') ++zeilen;
        log(LogLine::Kind::Info, trf(S::LogCopied, zeilen));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::CopyLogTip));

    // Open the output folder - only makes sense once something was built.
    std::string outDir;
    {
        std::lock_guard<std::mutex> lock(outputDirMutex_);
        outDir = lastOutputDir_;
    }
    if (!outDir.empty() && platform_.revealInExplorer) {
        ImGui::SameLine();
        if (ImGui::SmallButton(withIcon(ICON_FOLDER_OPEN, tr(S::OpenOutputDir))))
            platform_.revealInExplorer(outDir);
    }

    ImGui::SameLine();
    if (ImGui::SmallButton(withIcon(ICON_DELETE, tr(S::ClearLog)))) {
        std::lock_guard<std::mutex> lock(logMutex_);
        log_.clear();
    }

    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        ImGui::TextDisabled("%zu", log_.size());
    }

    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::lock_guard<std::mutex> lock(logMutex_);
    for (const auto& l : log_) {
        ImVec4 col(0.80f, 0.80f, 0.82f, 1.0f);
        switch (l.kind) {
            case LogLine::Kind::Good: col = ImVec4(0.40f, 0.90f, 0.50f, 1.0f); break;
            case LogLine::Kind::Warn: col = ImVec4(1.00f, 0.80f, 0.30f, 1.0f); break;
            case LogLine::Kind::Bad: col = ImVec4(1.00f, 0.40f, 0.40f, 1.0f); break;
            default: break;
        }
        // Icon by kind, then the text in the same color. The icon alone
        // already tells what it's about.
        const char* sym = l.kind == LogLine::Kind::Good   ? ICON_CHECK
                          : l.kind == LogLine::Kind::Warn ? ICON_WARNING
                          : l.kind == LogLine::Kind::Bad  ? ICON_CANCEL
                                                          : nullptr;
        if (sym && iconsAvailable()) {
            ImGui::TextColored(col, "%s", sym);
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        }
        ImGui::TextColored(col, "%s", l.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::drawStatusBar() {
    if (buildRunning()) {
        const std::size_t done = job_.done.load(), total = job_.total.load();
        std::string cur;
        {
            std::lock_guard<std::mutex> lock(job_.currentMutex);
            cur = job_.current;
        }
        ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
                           ImVec2(240, 0));
        ImGui::SameLine();
        ImGui::Text("%zu/%zu  %s", done, total, cur.c_str());
    } else {
        ImGui::TextDisabled(tr(S::ScriptsOpen), docs_.size());
        if (!settings_.referenceGla.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled((std::string("| ") + tr(S::Reference) + ": %s").c_str(),
                                fs::path(settings_.referenceGla).filename().string().c_str());
        }
    }
}

// Keyboard shortcuts - all of those labeled in the menu.
//
// Previously only Ctrl+N and Ctrl+S were implemented. Ctrl+O, Ctrl+Shift+O,
// Ctrl+W, F5, Shift+F5 and F7 were shown in the menu but did nothing.
void App::handleShortcuts() {
    // While a confirmation prompt is open, no further actions.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    const bool have = !docs_.empty();

    if (ctrl && !shift && pressed(ImGuiKey_N)) newCarDialog();
    if (ctrl && pressed(ImGuiKey_O)) {
        if (shift) openFolderDialog();
        else openScriptDialog();
    }
    if (ctrl && pressed(ImGuiKey_S) && have) {
        if (shift) saveAllDocuments();
        else saveDocument(static_cast<std::size_t>(active_));
    }
    if (ctrl && !shift && pressed(ImGuiKey_W) && have)
        requestClose({static_cast<std::size_t>(active_)});
    if (!ctrl && pressed(ImGuiKey_F5) && have && !buildRunning()) startBuild(shift);
    if (!ctrl && !shift && pressed(ImGuiKey_F7) && have) validateAll();
}

void App::draw() {
    if (!styleApplied_) applyStyle();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    handleShortcuts();

    if (ImGui::Begin("##main", nullptr, flags)) {
        drawMenuBar();
        if (mode_ == Mode::Build) drawToolbar();
        drawUpdateBanner();
        ImGui::Separator();

        // Compute the heights instead of placing two different ones side by
        // side.
        //
        // Previously the settings panel took the full remaining height, while
        // the table next to it got a fixed height. The two didn't match: an
        // empty area remained below, and depending on the screen you had to
        // scroll to see the log.
        //
        // Now both share the same area, and the lower part depends on the
        // actually available height.
        const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
        const float statusH = ImGui::GetFrameHeightWithSpacing();
        const float avail = ImGui::GetContentRegionAvail().y;

        float bottomH = avail * 0.28f;
        bottomH = std::clamp(bottomH, 150.0f * k, 340.0f * k);
        const float mainH = std::max(120.0f * k, avail - bottomH - statusH);

        ImGui::BeginChild("upper", ImVec2(0, mainH));
        {
            drawModeBar();
            ImGui::SameLine();

            if (mode_ == Mode::Build) {
                drawSettingsPanel();
                if (showSettings_) ImGui::SameLine();
                ImGui::BeginChild("docs", ImVec2(0, 0));
                drawTabs();
                ImGui::EndChild();
            } else if (mode_ == Mode::Extract) {
                ImGui::BeginChild("extract", ImVec2(0, 0));
                drawExtractPanel();
                ImGui::EndChild();
            } else {
                ImGui::BeginChild("preview", ImVec2(0, 0));
                drawPreviewPanel();
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        ImGui::BeginChild("bottom", ImVec2(0, bottomH));
        if (ImGui::BeginTabBar("bottomtabs")) {
            if (ImGui::BeginTabItem(tr(S::TabIssues))) {
                drawIssues();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr(S::TabLog))) {
                drawLog();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        drawStatusBar();

        if (active_ >= 0 && active_ < static_cast<int>(docs_.size()))
            drawSequenceDialog(docs_[static_cast<std::size_t>(active_)]);

        // Information about the program itself.
        //
        // The most important line in it is the runtime: without it statically
        // linked, the program doesn't start at all on other machines, and the
        // recipient can't do anything about it. It should be possible to
        // answer that question without installing tools.
        if (showAbout_) {
            ImGui::SetNextWindowSize(ImVec2(560 * settings_.dpiScale, 0), ImGuiCond_Appearing);
            if (ImGui::Begin(tr(S::About), &showAbout_, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(tr(S::AboutVersion), update::displayName(platform_.build).c_str());
                if (!platform_.build.commit.empty())
                    ImGui::TextDisabled("%s %s", update::kRepo, platform_.build.commit.c_str());
                ImGui::Text(tr(S::AboutBuilt), __DATE__, __TIME__);
                ImGui::Text(tr(S::AboutBits), sizeof(void*) * 8, g2::defaultThreadCount());

                // Where the startup log lives. With "doesn't start" that's the
                // first question - the answer is right here instead of having
                // to ask for it.
                if (!logPath_.empty()) {
                    ImGui::Text(tr(S::AboutLogPath), logPath_.c_str());
                    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(logPath_.c_str());
                }
                ImGui::Spacing();
                if (staticRuntime())
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s", tr(S::AboutRuntimeOk));
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1), "%s",
                                       tr(S::AboutRuntimeBad));
                ImGui::Spacing();
                if (ImGui::Button(tr(S::DlgClose))) showAbout_ = false;
            }
            ImGui::End();
        }

        // Confirm deletion. There is no undo, and a slipped right-click
        // shouldn't cost a sequence.
        if (!pendingDelete_.empty()) ImGui::OpenPopup("confirmdel");
        if (ImGui::BeginPopupModal("confirmdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(tr(S::ConfirmDelete), pendingDelete_.size());
            ImGui::Spacing();
            if (ImGui::Button(withIcon(ICON_DELETE, tr(S::DeleteSeq)))) {
                // In the script the confirmation was asked for - not the one
                // currently active. A script dragged onto the window in the
                // meantime becomes active and would otherwise have lost the
                // rows.
                std::size_t n = 0;
                for (std::size_t di = 0; di < docs_.size(); ++di)
                    if (docs_[di].path == pendingDeleteDocPath_)
                        n = deleteGrabs(di, pendingDelete_, !pendingDeleteIsCut_);
                char msg[128];
                std::snprintf(msg, sizeof(msg), tr(S::Deleted), n);
                log(LogLine::Kind::Info, msg);
                pendingDelete_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr(S::No))) {
                pendingDelete_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        drawCloseDialog();
        drawOverwriteDialog();
    }
    ImGui::End();
}

}  // namespace g2::gui
