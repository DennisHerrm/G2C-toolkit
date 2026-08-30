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
#include <fstream>
#include <map>
#include <algorithm>
#include <cstdlib>
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

    // Zuletzt offene Skripte wiederherstellen. Verschwundene werden
    // stillschweigend uebergangen — eine Fehlerlawine beim Start waere
    // laestiger als das fehlende Tab.
    std::size_t restored = 0;
    for (const auto& t : savedTabs_) {
        std::error_code ec;
        if (!fs::exists(t, ec)) continue;
        if (openCar(t)) ++restored;
    }
    if (restored) {
        if (savedActive_ >= 0 && savedActive_ < static_cast<int>(docs_.size()))
            active_ = savedActive_;
        log(LogLine::Kind::Good, trf(S::LogRestored, restored));
    } else {
        log(LogLine::Kind::Info, tr(S::LogReady));
    }
}

App::~App() {
    stopFrameWorker();
    job_.join();
    saveSettings();
}

// --- Einstellungen ueber Sitzungen hinweg ----------------------------------
//
// Einfaches Schluessel=Wert-Format neben der ausfuehrbaren Datei. Kein INI-
// Parser, kein JSON: es sind zehn Werte, und ein eigenes Format waere hier
// mehr Aufwand als Nutzen.

// Wo die Einstellungen liegen.
//
// Nicht im Arbeitsverzeichnis: das haengt davon ab, WIE das Programm
// gestartet wurde. Per Doppelklick ist es der Ordner der Exe, aus einer
// Verknuepfung deren Arbeitsverzeichnis, aus der Eingabeaufforderung
// irgendein anderer. Die Einstellungen waren damit mal da und mal weg.
//
// Auch nicht unter "Dokumente": der Ordner gehoert dem Nutzer fuer eigene
// Dateien. Programme, die dort ihre Konfiguration ablegen, muellen ihn zu.
// Spiele legen dort Spielstaende ab, weil man die sichern und weitergeben
// will — Fensterpositionen und Haekchen will man das nicht.
//
// Richtig ist %APPDATA%\g2c\. Wer es lieber mitnehmbar haben will — Stick,
// Netzlaufwerk, mehrere Zweige nebeneinander —, legt neben die Exe eine
// leere Datei "g2c_portable.txt"; dann liegt alles daneben.
// Auswahldialog mit Gedaechtnis je Zweck.
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

std::string App::askFolder(const char* purpose, const char* title) {
    if (!platform_.pickFolder) return {};
    const auto it = lastDirs_.find(purpose);
    std::string out =
        platform_.pickFolder(title, it == lastDirs_.end() ? std::string() : it->second);
    if (!out.empty()) lastDirs_[purpose] = out;
    return out;
}

std::string App::configDir() {
    std::error_code ec;

    // Mitnehmbarer Betrieb: Markierungsdatei neben der Exe.
    const fs::path here = fs::current_path(ec);
    if (!ec && fs::exists(here / "g2c_portable.txt", ec)) return here.string();

    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) appdata = std::getenv("HOME");   // fuer Tests unter Linux
    if (!appdata || !*appdata) return here.string();

    const fs::path dir = fs::path(appdata) / "g2c";
    fs::create_directories(dir, ec);
    if (ec) return here.string();
    return dir.string();
}

std::string App::settingsPath() const {
    const fs::path target = fs::path(configDir()) / "g2c_settings.txt";

    // Einmalige Uebernahme aus dem alten Ort, damit niemand seine
    // Einstellungen verliert.
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        const fs::path old = fs::current_path(ec) / "g2c_settings.txt";
        if (!ec && fs::exists(old, ec) && old != target) fs::copy_file(old, target, ec);
    }
    return target.string();
}

void App::saveSettings() const {
    std::ofstream f(settingsPath());
    if (!f) return;
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

    // Die Bindepose-Wahl gehoert zum Export, nicht zum Skript — sie bleibt
    // deshalb ueber Sitzungen erhalten. Wer sie einmal umstellen musste,
    // will das nicht bei jedem Start wiederholen.
    f << "xsiver=" << (extract_.xsiVersion == xsiexp::ExportOptions::Version::V30 ? 30 : 35)
      << "\n";
    f << "basepose=" << (extract_.basePose == xsiexp::ExportOptions::BasePose::World   ? 0
                         : extract_.basePose == xsiexp::ExportOptions::BasePose::Local ? 1
                                                                                       : 2)
      << "\n";

    // Ausgabeorte je Skript. So bleibt die Zuordnung erhalten, auch wenn die
    // Tabs beim naechsten Start anders geoeffnet werden.
    for (const auto& d : docs_)
        if (!d.outputDir.empty()) f << "out:" << d.path << "=" << d.outputDir << "\n";

    // Offene Tabs und der aktive. Beim naechsten Start liegt derselbe Stand
    // wieder da — bei zwanzig Skripten ist das der Unterschied zwischen
    // "weiterarbeiten" und "erst mal alles wieder aufmachen".
    f << "active=" << active_ << "\n";
    for (const auto& d : docs_) f << "tab=" << d.path << "\n";

    // Zuletzt benutzter Ordner je Auswahldialog.
    for (const auto& [k, v] : lastDirs_) f << "dir:" << k << "=" << v << "\n";
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
        else if (key == "lang") {
            try {
                settings_.language = std::stoi(val);
                setLanguage(static_cast<Lang>(settings_.language));
            } catch (...) {
            }
        }
        // "threads" wird bewusst ignoriert: aeltere Einstellungsdateien
        // koennten eine Begrenzung enthalten, die es nicht mehr geben soll.
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
    // Nicht unbegrenzt wachsen lassen — bei einem Stapelbau ueber hunderte
    // Skripte kaeme sonst schnell mehr zusammen, als jemand liest.
    while (log_.size() > 2000) log_.pop_front();
}

// --- Dokumente -------------------------------------------------------------

bool App::openCar(const std::string& path) {
    for (std::size_t i = 0; i < docs_.size(); ++i) {
        if (docs_[i].path == path) {
            active_ = static_cast<int>(i);
            log(LogLine::Kind::Info, trf(S::LogAlreadyOpen, path.c_str()));
            return true;
        }
    }

    Document d;
    d.path = path;
    d.title = fs::path(path).filename().string();
    // Der endgueltige Titel entsteht unten in refreshTabTitles: bei gleichen
    // Dateinamen entscheidet der Ordner.
    try {
        car::ParseOptions po;
        po.followIncludes = false;
        d.script = car::parseFile(path, po);
    } catch (const std::exception& e) {
        d.loadError = e.what();
        log(LogLine::Kind::Bad, d.title + ": " + d.loadError);
        docs_.push_back(std::move(d));
        active_ = static_cast<int>(docs_.size()) - 1;
        return false;
    }
    d.syncSelection();
    if (const auto it = savedOutputs_.find(path); it != savedOutputs_.end())
        d.outputDir = it->second;

    // Assetwurzel und Referenz-GLA aus der Lage der .car ableiten, solange
    // nichts eingestellt ist.
    //
    // Ohne Wurzel laesst sich kein einziger Pfad aufloesen: die .car nennt
    // "models/players/...", und wo das beginnt, weiss nur der Ordnerbaum.
    // Die Kommandozeile macht das beim Draufziehen laengst; es hier nicht zu
    // tun hiess, den Nutzer etwas von Hand suchen zu lassen, was das Programm
    // selbst weiss.
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
    active_ = static_cast<int>(docs_.size()) - 1;
    refreshTabTitles();
    return true;
}

// Eindeutige Tabtitel.
//
// In einem Modellbaum heissen alle Skripte "_humanoid.car" — zwanzig Tabs
// mit demselben Text sind wertlos. Unterscheidbar sind sie am Ordner, und
// genau der wird dann zum Titel. Nur wo der Dateiname schon eindeutig ist,
// bleibt er stehen.
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

// --- Zweiter Modus: GLA zurueck nach dotXSI --------------------------------

bool App::loadGlaForExtract(const std::string& path) {
    try {
        extract_.gla = readMdxa(readWholeFileBytes(path));
        extract_.glaPath = path;
        extract_.loaded = true;
        extract_.error.clear();
        extract_.seqs.clear();
        extract_.selected.clear();
        extract_.framesPath.clear();

        // Versatz schaetzen und anzeigen. Er muss beim Export wieder
        // eingesetzt werden, sonst zieht das Neubauen ihn ein zweites Mal
        // ab — und das faellt erst im Spiel auf.
        extract_.origin = xsiexp::detectOrigin(extract_.gla);
        extract_.framesPath.clear();
        extract_.withMotion = 0;

        // -origin erkennen. Wird es beim Export nicht wieder eingesetzt,
        // zieht das Neubauen es ein ZWEITES Mal ab, und das Modell steht 24
        // Einheiten daneben.
        extract_.origin = xsiexp::detectOrigin(extract_.gla);

        // Ohne animation.cfg gibt es nur einen Block. Besser als nichts:
        // man sieht wenigstens, dass die Datei gelesen wurde.
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

    // Nach Haeufigkeit geordnet: Ravens Name zuerst, dann unser eigener
    // Ausgabename, dann der Notnagel "genau eine Datei dieser Art im Ordner".
    const auto pick = [&](const std::vector<std::string>& names, const char* ext) -> std::string {
        for (const auto& n : names) {
            const fs::path p = dir / n;
            if (fs::exists(p, ec)) return p.string();
        }
        // Genau eine? Dann ist sie gemeint. Mehrere? Dann nicht raten.
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

    // Die .frames ist optional: sie enthaelt nur die Wurzelbewegung, und die
    // haben ohnehin die wenigsten Sequenzen.
    const std::string fr = pick({stem + ".frames", "animation.frames"}, ".frames");
    if (!fr.empty() && loadFramesFile(fr))
        log(LogLine::Kind::Good, trf(S::LogFound, fs::path(fr).filename().string().c_str()));
}

namespace {
// Sequenznamen vergleichbar machen: Grossschreibung und Leerzeichen am Rand
// sind in animation.cfg-Dateien nicht einheitlich.
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
    std::fill(extract_.selected.begin(), extract_.selected.end(), 0);
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

        // Sequenzen ausserhalb der Datei ueberspringen statt anzuzeigen: sie
        // liessen sich ohnehin nicht exportieren, und in der Liste wuerde
        // man sie fuer brauchbar halten.
        if (s.count <= 0 || s.start < 0 || s.start + s.count > extract_.gla.numFrames) {
            ++skipped;
            continue;
        }
        out.push_back(std::move(s));
    }

    extract_.seqs = std::move(out);
    extract_.selected.assign(extract_.seqs.size(), 0);
    extract_.cfgPath = path;
    log(skipped ? LogLine::Kind::Warn : LogLine::Kind::Good,
        std::to_string(extract_.seqs.size()) + " Sequenzen gelesen" +
            (skipped ? ", " + std::to_string(skipped) + " ausserhalb der GLA uebersprungen" : ""));
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
    opt.origin = extract_.origin;

    std::size_t ok = 0, failed = 0;
    for (const std::size_t r : rows) {
        if (r >= extract_.seqs.size()) continue;
        const ExtractSeq& s = extract_.seqs[r];
        opt.fps = s.fps > 0 ? s.fps : 20;
        // Erst die .frames, wenn es eine gibt. Fehlt sie, laesst sich die
        // Wurzelbewegung aus der GLA selbst rekonstruieren — sie steht dort
        // als lineare Rampe auf dem Wurzelbone. Gegen Ravens eigene
        // _humanoid.frames geprueft: bei allen 178 Sequenzen mit Bewegung
        // stimmt der Wert ueberein.
        opt.rootMotionPerFrame =
            extract_.framesPath.empty()
                ? std::nullopt
                : xsiexp::readAverageVec(extract_.framesPath, s.name);
        if (!opt.rootMotionPerFrame)
            opt.rootMotionPerFrame =
                xsiexp::detectRootMotion(extract_.gla, {s.name, s.start, s.count});
        try {
            xsiexp::ExportOptions one = opt;
            if (!extract_.framesPath.empty())
                one.rootMotionPerFrame = xsiexp::readAverageVec(extract_.framesPath, s.name);
            const std::string text =
                xsiexp::exportSequence(extract_.gla, {s.name, s.start, s.count}, one);
            std::string low = s.name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const fs::path out = fs::path(dir) / (low + ".xsi");
            std::ofstream o(out, std::ios::binary);
            o.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!o) throw std::runtime_error(tr(S::LogCannotWrite));
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

    // Unterbereiche zusammenfassen: exportierte man jede Sequenz als eigene
    // Datei, haette die neue GLA mehr Frames als die alte und die
    // animation.cfg passte nicht mehr.
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

    // Nur die Master schreiben. Der Zeilenindex muss zur Sequenzliste
    // passen, also ueber den Namen zurueckgesucht.
    std::vector<std::size_t> rows;
    for (const auto& m : g.masters)
        for (std::size_t i = 0; i < extract_.seqs.size(); ++i)
            if (extract_.seqs[i].name == m.self.name && extract_.seqs[i].start == m.self.start) {
                rows.push_back(i);
                break;
            }

    const std::size_t n = exportSequences(rows, dir);

    // Der Versatz MUSS derselbe sein wie beim Export, sonst zieht das
    // Neubauen ihn nicht wieder ab.
    // Skelettpfad aus dem Kopf der GLA: dort steht genau der Wert, den
    // Carcass seinerzeit als -makeskel bekommen hat.
    const auto sc = xsiexp::buildScript(g, xsiPrefix, extract_.origin,
                                        extract_.gla.skeleton.scale, false,
                                        extract_.gla.skeleton.name);
    try {
        writeFileChecked(carPath, car::writeScript(sc));
        std::snprintf(msg, sizeof(msg), tr(S::CarWritten), carPath.c_str());
        log(LogLine::Kind::Good, msg);
    } catch (const std::exception& e) {
        log(LogLine::Kind::Bad, trf(S::LogScriptNotWritable, e.what()));
    }
    return n;
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
    log(LogLine::Kind::Info,
        std::to_string(found.size()) + " .car-Dateien unter " + root);
    std::size_t opened = 0;
    for (const auto& f : found)
        if (openCar(f.path)) ++opened;
    return opened;
}

void App::closeDocument(std::size_t index) {
    if (index >= docs_.size()) return;
    docs_.erase(docs_.begin() + static_cast<long>(index));
    if (active_ >= static_cast<int>(docs_.size())) active_ = static_cast<int>(docs_.size()) - 1;
    if (active_ < 0) active_ = 0;
    refreshTabTitles();
}

std::size_t App::addXsiFiles(const std::vector<std::string>& files, bool toAll) {
    if (docs_.empty() || files.empty()) return 0;

    // Erst pruefen, was hereinkommt.
    //
    // Ein Ordner oder eine Datei, die keine .xsi ist, laesst sich anhaengen
    // und faellt erst beim Bauen auf — dort steht dann ein Pfad in der
    // Fehlerliste, mit dem niemand etwas anfangen kann. Besser hier
    // abweisen, wo klar ist, was der Nutzer gerade getan hat.
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
            // Relativ zum basedir speichern, wenn moeglich — die .car
            // enthaelt sonst absolute Pfade und ist auf keinem anderen
            // Rechner mehr brauchbar.
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

// Skript zurueckschreiben.
//
// Vorher wird eine Sicherung angelegt. Die .car ist Handarbeit von Jahren;
// sie ohne Netz zu ueberschreiben waere fahrlaessig, zumal beim Schreiben
// Kommentare und Einrueckungen verlorengehen.
bool App::saveDocument(std::size_t index) {
    if (index >= docs_.size()) return false;
    Document& d = docs_[index];
    if (!d.loadError.empty()) return false;

    std::error_code ec;
    if (settings_.keepBackup) {
        const fs::path bak = fs::path(d.path).string() + ".bak";
        if (!fs::exists(bak, ec)) fs::copy_file(d.path, bak, ec);
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
    // Alle .xsi eines Baums einsammeln, sortiert — sonst haengt die
    // Reihenfolge der Sequenzen davon ab, wie das Dateisystem sie liefert.
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

// Framezahlen im Hintergrund nachladen.
//
// Beim ersten Mal muss jede .xsi gelesen werden — bei 1289 Dateien dauert
// das. Im Vordergrund waere das Fenster solange eingefroren, also laeuft es
// nebenher und die Spalte fuellt sich nach und nach. Beim zweiten Mal kommt
// alles aus dem Zwischenspeicher und steht praktisch sofort.
void App::startFrameWorker(const Document& d) {
    if (frameWorkerRunning_.load()) return;
    if (settings_.baseDir.empty()) return;

    // Fehlende einsammeln, damit der Thread nicht auf die Dokumentliste
    // zugreifen muss — die kann sich unter ihm aendern.
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

    frameWorkerBaseDir_ = baseDir;
    frameWorkerRunning_.store(true);
    frameWorkerStop_.store(false);

    if (frameWorker_.joinable()) frameWorker_.join();
    frameWorker_ = std::thread([this, todo = std::move(todo), baseDir, carDir, cacheDir] {
        AnimCache cache(cacheDir);
        std::size_t missing = 0;
        for (const auto& rel : todo) {
            if (frameWorkerStop_.load()) break;
            int frames = -2;   // nicht auffindbar
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

// --- Sequenzen umordnen und loeschen ---------------------------------------

bool App::moveGrab(std::size_t docIndex, std::size_t from, std::size_t to) {
    if (docIndex >= docs_.size()) return false;
    Document& d = docs_[docIndex];
    const std::size_t n = d.script.grabs.size();
    if (from >= n || to >= n || from == to) return false;

    auto& g = d.script.grabs;
    const auto item = g[from];
    g.erase(g.begin() + static_cast<long>(from));
    g.insert(g.begin() + static_cast<long>(to), item);

    // Die Auswahl muss mitwandern. Sonst zeigt sie nach dem Verschieben auf
    // eine andere Sequenz, und der naechste Klick auf "Loeschen" trifft die
    // falsche.
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

    // Erst herausnehmen, dann einfuegen. Die Zielposition muss um die
    // Zeilen verringert werden, die VOR ihr entfernt wurden — sonst landet
    // der Block zu weit hinten, und zwar genau um die Anzahl der
    // verschobenen Zeilen.
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
        // Ohne moveGrab, sonst ruft sich das gegenseitig auf.
        const auto item = g[i];
        g.erase(g.begin() + static_cast<long>(i));
        g.push_back(item);
        if (i < d.selected.size()) {
            const char sel = d.selected[i];
            d.selected.erase(d.selected.begin() + static_cast<long>(i));
            d.selected.push_back(sel);
        }
        d.dirty = true;
        return true;
    }
    return false;
}

std::size_t App::deleteGrabs(std::size_t docIndex, std::vector<std::size_t> rows) {
    if (docIndex >= docs_.size() || rows.empty()) return 0;
    Document& d = docs_[docIndex];

    // Absteigend, damit sich die noch offenen Indizes nicht unter der
    // Schleife verschieben.
    std::sort(rows.begin(), rows.end(), std::greater<>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    std::size_t n = 0;
    for (const std::size_t r : rows) {
        if (r >= d.script.grabs.size()) continue;
        d.script.grabs.erase(d.script.grabs.begin() + static_cast<long>(r));
        if (r < d.selected.size()) d.selected.erase(d.selected.begin() + static_cast<long>(r));
        ++n;
    }
    if (n) {
        d.dirty = true;
        d.validated = false;
        // Ein offener Bearbeitungsdialog koennte auf eine geloeschte Zeile
        // zeigen.
        editOpen_ = false;
        editRow_ = -1;
    }
    return n;
}

std::size_t App::copyGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows) {
    if (docIndex >= docs_.size() || rows.empty()) return 0;
    const Document& d = docs_[docIndex];

    clipboard_.clear();
    // In der Reihenfolge der Zeilen, nicht der Auswahl — sonst haengt das
    // Ergebnis davon ab, in welcher Reihenfolge angeklickt wurde.
    std::vector<std::size_t> sorted = rows;
    std::sort(sorted.begin(), sorted.end());
    for (const std::size_t r : sorted)
        if (r < d.script.grabs.size()) clipboard_.push_back(d.script.grabs[r]);
    return clipboard_.size();
}

std::size_t App::cutGrabs(std::size_t docIndex, const std::vector<std::size_t>& rows) {
    const std::size_t n = copyGrabs(docIndex, rows);
    if (n) deleteGrabs(docIndex, rows);
    return n;
}

std::size_t App::pasteGrabs(std::size_t docIndex, std::size_t before) {
    if (docIndex >= docs_.size() || clipboard_.empty()) return 0;
    Document& d = docs_[docIndex];
    if (before > d.script.grabs.size()) before = d.script.grabs.size();

    d.script.grabs.insert(d.script.grabs.begin() + static_cast<long>(before), clipboard_.begin(),
                          clipboard_.end());
    d.selected.insert(d.selected.begin() + static_cast<long>(before), clipboard_.size(), 0);

    d.dirty = true;
    d.validated = false;
    keepRootLast(docIndex);
    return clipboard_.size();
}

bool App::newCar(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        log(LogLine::Kind::Bad, trf(S::LogExists, path.c_str()));
        return false;
    }

    // Ein leeres, aber vollstaendiges Skript: init und finalize umschliessen
    // die Grabs, die Konvertierungsanweisung nennt das Zielmodell. Ohne die
    // drei waere die Datei kein gueltiges Carcass-Skript.
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
    vo.threads = 0;   // alle Kerne
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

// --- Bauen -----------------------------------------------------------------

void App::buildOne(const Document& d) {
    if (!d.loadError.empty()) return;
    try {
        if (d.outputDir.empty())
            throw std::runtime_error(tr(S::LogNoOutputDir));
        if (settings_.referenceGla.empty())
            throw std::runtime_error(tr(S::LogNoRefGla));

        const MdxaFile ref = readMdxa(readWholeFileBytes(settings_.referenceGla));

        car::BuildOptions bo;
        bo.baseDir = settings_.baseDir;
        bo.threads = 0;   // alle Kerne
        bo.carcassCompatible = settings_.carcassCompat;
        if (settings_.useCache && !settings_.baseDir.empty())
            bo.cacheDir = (fs::path(settings_.baseDir) / "g2c_cache").string();

        // Fehlende Dateien vorab melden — uebersetzt und mit Zuordnung.
        //
        // Die Ausnahme aus der Bibliothek ist auf Deutsch und traegt den
        // vollen Text; hier wird stattdessen aus den strukturierten Daten
        // eine Meldung in der eingestellten Sprache gebaut, die sagt, WELCHE
        // Sequenz betroffen ist und wo die Datei erwartet wurde.
        {
            car::BuildOptions probe = bo;
            probe.skipMissing = true;

            // Der Probelauf darf NICHT werfen. Fehlen alle Dateien, bricht
            // build mit "keine einzige lesbar" ab — dann kaeme die
            // ausfuehrliche Meldung nie zustande, und der Nutzer saehe genau
            // den unbrauchbaren Satz, den sie ersetzen soll.
            std::vector<car::BuildResult::MissingFile> missing;
            try {
                missing = car::build(d.script, ref.skeleton, d.path, probe).missing;
            } catch (const std::exception&) {
                // Alles fehlt: die Liste selbst aufbauen.
                for (std::size_t i = 0; i < d.script.grabs.size(); ++i) {
                    const auto& g = d.script.grabs[i];
                    if (!car::resolveAssetPath(g.file, settings_.baseDir,
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
                if (!settings_.baseDir.empty())
                    log(LogLine::Kind::Bad,
                        std::string(tr(S::MissingSearched)) + " " + settings_.baseDir);
                log(LogLine::Kind::Bad, tr(S::MissingList));

                for (std::size_t i = 0; i < chk.missing.size() && i < 8; ++i) {
                    const auto& m = chk.missing[i];
                    std::string line = "  " + m.file + "   [" + tr(S::SeqLabel) + " " +
                                       m.sequence + "]";
                    if (m.line) line += " (.car " + std::to_string(m.line) + ")";
                    log(LogLine::Kind::Bad, line);
                    if (!settings_.baseDir.empty())
                        log(LogLine::Kind::Info,
                            "      " +
                                (fs::path(settings_.baseDir) / m.file).lexically_normal().string());
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

        // Geaenderte Skripte vor dem Bauen speichern.
        //
        // Gebaut wurde bisher der Zustand im Speicher, waehrend die Datei auf
        // der Platte die alte blieb. Wer das Programm danach schloss, hatte
        // eine GLA, die zu keiner .car mehr passte — und beim naechsten
        // Oeffnen war die Aenderung weg.
        //
        // Die Datei ist das, was gebaut wurde; beides auseinanderlaufen zu
        // lassen ist in keinem Fall richtig.
        if (d.dirty && !d.path.empty()) {
            if (saveDocument(static_cast<std::size_t>(&d - docs_.data())))
                log(LogLine::Kind::Info, trf(S::SavedBeforeBuild, d.title.c_str()));
        }

        const car::BuildResult br = car::build(d.script, ref.skeleton, d.path, bo);
        for (const auto& w : br.warnings) log(LogLine::Kind::Warn, d.title + ": " + w);

        MdxaWriteOptions wo;
        wo.threads = 0;   // alle Kerne
        if (settings_.carcassCompat) {
            wo.compress.rounding = Rounding::Legacy;
            wo.compress.optimizeQuat = false;
            wo.compress.canonicalizeSign = false;
        }
        // Der GLA-Name im Kopf kommt aus -makeskel, NICHT aus der Referenz.
        //
        // Er steht als Zeichenkette in der Datei und sagt der Engine, welches
        // Skelett das ist. Uebernahmen wir ihn von der Referenz, trug jede
        // gebaute Datei "models/players/_humanoid/_humanoid" — egal wohin sie
        // gehoerte. Im Spiel meldete sich ein eigener Humanoid dann als der
        // Standard-Humanoid, und die Engine nahm dessen animation.cfg.
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

        const fs::path gla = outDir / (stem + ".gla");
        {
            std::ofstream f(gla, std::ios::binary);
            f.write(reinterpret_cast<const char*>(res.data.data()),
                    static_cast<std::streamsize>(res.data.size()));
        }
        log(LogLine::Kind::Good, d.title + " -> " + gla.string() + " (" +
                                     std::to_string(br.totalFrames()) + " Frames)");

        {
            std::ostringstream head;
            head << br.totalFrames() << " frames; " << br.sequences.size()
                 << " sequences; erzeugt von g2c";
            const std::string cfg = car::writeAnimationCfg(br.sequences, head.str());
            // Keine .bak: die animation.cfg wird bei jedem Bau vollstaendig
            // neu erzeugt, eine Sicherung davon waere wertlos und liegt nur
            // im Ordner herum. Fuer .car-Dateien, die von Hand bearbeitet
            // werden, ist das anders — dort bleibt die Sicherung.
            const fs::path cfgPath = outDir / "animation.cfg";
            std::ofstream f(cfgPath, std::ios::binary);
            f.write(cfg.data(), static_cast<std::streamsize>(cfg.size()));
        }
        log(LogLine::Kind::Warn, tr(S::CfgBelongsWithGla));

        if (settings_.writeFrames && !br.frameBlocks.empty()) {
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
            const std::string txt = writeFrames(fe);
            std::ofstream f(outDir / (stem + ".frames"), std::ios::binary);
            f.write(txt.data(), static_cast<std::streamsize>(txt.size()));
        }

        if (settings_.writeMesh && d.script.convert && !d.script.convert->root.empty()) {
            const std::string carDir = fs::path(d.path).parent_path().string();
            std::string xsi;
            for (const char* ext : {".xsi", ".XSI"}) {
                xsi = car::resolveAssetPath(d.script.convert->root + ext, settings_.baseDir, carDir);
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
                std::ofstream f(outDir / (stem + ".glm"), std::ios::binary);
                f.write(reinterpret_cast<const char*>(mw.data.data()),
                        static_cast<std::streamsize>(mw.data.size()));
                log(LogLine::Kind::Good,
                    d.title + " -> " + stem + ".glm (" + std::to_string(mr.stats.surfaces) +
                        " Surfaces, " + std::to_string(mr.stats.vertices) + " Verts)");

                if (settings_.writeSkin) {
                    const std::string skin = writeSkin(mr.mesh);
                    std::ofstream sf(outDir / (stem + ".skin"), std::ios::binary);
                    sf.write(skin.data(), static_cast<std::streamsize>(skin.size()));
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

    std::vector<Document> targets;
    if (allTabs) {
        for (const auto& d : docs_)
            if (d.loadError.empty()) targets.push_back(d);
    } else if (active_ >= 0 && active_ < static_cast<int>(docs_.size())) {
        targets.push_back(docs_[static_cast<std::size_t>(active_)]);
    }
    if (targets.empty()) {
        log(LogLine::Kind::Warn, tr(S::LogNothingToBuild));
        return;
    }

    // Vorab pruefen statt mittendrin abbrechen: wer zwanzig Skripte anstoesst,
    // will vorher wissen, dass eines kein Ziel hat, nicht nach zehn Minuten.
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

    // Schreiben zwei Skripte in denselben Ordner?
    //
    // Dann ueberschreibt die zweite GLA die erste, und schlimmer: die
    // animation.cfg gehoert danach zur zweiten, waehrend die erste GLA weg
    // ist. Im Spiel sieht das aus wie vertauschte Animationen, und die
    // Ursache ist von aussen nicht erkennbar.
    //
    // Nur warnen, nicht abbrechen: es gibt Faelle, in denen genau das
    // gewollt ist — etwa wenn ein Skript ausdruecklich ein anderes ersetzen
    // soll.
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

    // Kopien der Dokumente, damit die Oberflaeche waehrend des Baus weiter
    // bedienbar bleibt und nichts unter dem Arbeitsthread wegeditiert wird.
    job_.worker = std::thread([this, targets = std::move(targets)] {
        for (const auto& d : targets) {
            if (job_.cancel.load()) break;
            {
                std::lock_guard<std::mutex> lock(job_.currentMutex);
                job_.current = d.title;
            }
            buildOne(d);
            job_.done.fetch_add(1);
        }
        job_.running.store(false);
    });
}

// --- Darstellung -----------------------------------------------------------

void App::applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
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

    // Alle Groessen mitskalieren. Die Schrift wird in der Fensteranbindung
    // passend geladen; hier gehen Abstaende, Rundungen und Rollbalken mit.
    s.ScaleAllSizes(k);
    styleApplied_ = true;
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu(tr(S::MenuFile))) {
        if (ImGui::MenuItem(withIcon(ICON_ADD, tr(S::NewCar)), "Ctrl+N")) {
            const auto f = askFiles("car", tr(S::DlgTitleNewCar),
                                    "Carcass-Skript (*.car)\0*.car\0", false);
            if (!f.empty()) {
                std::string p = f.front();
                if (fs::path(p).extension().empty()) p += ".car";
                newCar(p);
            }
        }
        if (ImGui::MenuItem(withIcon(ICON_OPEN_FILE, tr(S::OpenScript)), "Ctrl+O") && platform_.openFiles) {
            for (const auto& f :
                 askFiles("car", tr(S::DlgTitleCar), "Carcass-Skript (*.car)\0*.car\0Alle\0*.*\0", true))
                openCar(f);
        }
        if (ImGui::MenuItem(withIcon(ICON_FOLDER_OPEN, tr(S::OpenFolder)), "Ctrl+Shift+O") && platform_.pickFolder) {
            const std::string dir = askFolder("carfolder", tr(S::DlgTitleCarFolder));
            if (!dir.empty()) openFolder(dir);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(withIcon(ICON_SAVE, tr(S::Save)), "Ctrl+S", false, !docs_.empty()))
            saveDocument(static_cast<std::size_t>(active_));
        if (ImGui::MenuItem(withIcon(ICON_SAVE_ALL, tr(S::SaveAll)), "Ctrl+Shift+S", false, !docs_.empty()))
            saveAllDocuments();
        ImGui::Separator();
        if (ImGui::MenuItem(tr(S::CloseTab), "Ctrl+W", false, !docs_.empty()))
            closeDocument(static_cast<std::size_t>(active_));
        if (ImGui::MenuItem(tr(S::CloseAll), nullptr, false, !docs_.empty())) docs_.clear();
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
                    // Die Schriftzeichen fuer Chinesisch und Japanisch sind
                    // beim Start nicht geladen. Die Fensteranbindung baut den
                    // Zeichensatz neu auf, sobald sie das hier sieht.
                    fontsDirty_ = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem(withIcon(ICON_SETTINGS, tr(S::Settings)), nullptr, &showSettings_);
        ImGui::Separator();
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
    if (iconButton(ICON_FOLDER_OPEN, kIconAccent, tr(S::BtnOpenFolder)) && platform_.pickFolder) {
        const std::string dir = askFolder("carfolder", tr(S::DlgTitleCarFolder));
        if (!dir.empty()) openFolder(dir);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!have);
    if (iconButton(ICON_ADD, kIconInfo, tr(S::BtnAddXsi)) && platform_.openFiles) {
        const auto files = askFiles("xsi", tr(S::DlgTitleXsi), "dotXSI (*.xsi)\0*.xsi\0Alle\0*.*\0", true);
        if (!files.empty()) addXsiFiles(files, false);
    }
    ImGui::SameLine();
    if (iconButton(ICON_FOLDER, kIconInfo, tr(S::BtnAddXsiFolder)) && platform_.pickFolder) {
        const std::string dir = askFolder("xsifolder", tr(S::DlgTitleXsiFolder));
        if (!dir.empty()) addXsiFolder(dir, false);
    }
    ImGui::SameLine();
    if (iconButton(ICON_FOLDER, kIconAccent, tr(S::BtnAddXsiFolderAll)) && platform_.pickFolder) {
        const std::string dir = askFolder("xsifolder", tr(S::DlgTitleXsiFolder));
        if (!dir.empty()) addXsiFolder(dir, true);
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
    // Breite mitskalieren, sonst sind die Pfade auf hochaufloesenden
    // Bildschirmen abgeschnitten. Rollbar, damit der Inhalt nie ueberlaeuft.
    const float sw = 360.0f * (settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f);
    ImGui::BeginChild("settings", ImVec2(sw, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::SeparatorText(tr(S::SecPaths));

    // purpose: Schluessel fuer das Ordnergedaechtnis. Jede Zeile bekommt
    // ihren eigenen, damit anims.h, Assetwurzel und Referenz-GLA sich nicht
    // gegenseitig den zuletzt benutzten Ordner ueberschreiben.
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

    // Keine Threadeinstellung mehr.
    //
    // Es gab nie einen guten Grund, weniger als alle Kerne zu benutzen: die
    // Ausgabe ist nachweislich unabhaengig von der Threadzahl bitgleich, und
    // eine Einstellung, die man nur falsch stellen kann, ist keine
    // Einstellung. Threads bleibt fest auf 0, was "alle Kerne" bedeutet.
    ImGui::Spacing();
    ImGui::TextDisabled(tr(S::CoresInUse), g2::defaultThreadCount());

    ImGui::EndChild();
}

void App::drawSequenceTable(Document& d) {
    if (!d.loadError.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", tr(S::LogUnreadable));
        ImGui::TextWrapped("%s", d.loadError.c_str());
        return;
    }

    // Ausgabeort dieses Skripts.
    //
    // Wichtig ist, dass man SIEHT, ob hier etwas Eigenes steht oder der
    // globale Wert durchschlaegt. Ein Platzhalter in Grau sieht aus wie ein
    // gesetzter Wert und ist damit schlimmer als gar keine Anzeige.
    {
        const bool own = !d.outputDir.empty();

        ImGui::TextUnformatted(tr(S::OutputTo));
        ImGui::SameLine();
        if (own) {
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", d.outputDir.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1), "nicht gesetzt \xE2\x80\x94 so wird nicht gebaut");
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
                std::fill(d.selected.begin(), d.selected.end(), 0);
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
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%zu Fehler", d.validation.errors);
        else
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "geprueft");
        if (d.validation.warnings) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%zu Warnungen", d.validation.warnings);
        }
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    // Ausdruecklich die verbleibende Hoehe. Mit -1 blieb je nach Umgebung
    // Platz uebrig, und darunter klaffte eine Leerflaeche.
    if (!ImGui::BeginTable("seqs", 7, flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(S::ColSequence), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(tr(S::ColFrames), ImGuiTableColumnFlags_WidthFixed, 65);
    ImGui::TableSetupColumn(tr(S::ColLoop), ImGuiTableColumnFlags_WidthFixed, 55);
    ImGui::TableSetupColumn(tr(S::ColSpeed), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(tr(S::ColExtra), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(tr(S::ColSource), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(tr(S::ColEnum), ImGuiTableColumnFlags_WidthFixed, 70);
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
        // Beim Druecken entscheidet sich, was das Ziehen bedeutet: auf einer
        // bereits ausgewaehlten Zeile ein Verschieben, sonst eine neue
        // Auswahl. So macht es der Explorer.
        const bool wasSelected = sel;

        if (ImGui::Selectable(name.c_str(), sel || isJumpTarget,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
            // Mehrfachauswahl wie in jedem Dateimanager:
            //   einfacher Klick   - nur diese Zeile
            //   Strg + Klick      - einzelne dazu oder weg
            //   Umschalt + Klick  - Bereich vom Anker bis hierher
            const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl);
            const bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);

            if (shift && selAnchor_ >= 0) {
                std::size_t a = static_cast<std::size_t>(selAnchor_), b = i;
                if (a > b) std::swap(a, b);
                if (!ctrl) std::fill(d.selected.begin(), d.selected.end(), 0);
                for (std::size_t k = a; k <= b && k < d.selected.size(); ++k)
                    d.selected[k] = 1;
            } else if (ctrl) {
                d.selected[i] = static_cast<char>(!sel);
                selAnchor_ = static_cast<int>(i);
            } else {
                std::fill(d.selected.begin(), d.selected.end(), 0);
                d.selected[i] = 1;
                selAnchor_ = static_cast<int>(i);
            }

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                editRow_ = static_cast<int>(i);
                editOpen_ = true;
            }
        }

        // Auswahl mit gehaltener Maustaste aufziehen.
        //
        // Beginnt nur auf einer NICHT ausgewaehlten Zeile — sonst koennte man
        // eine getroffene Auswahl nicht mehr verschieben, ohne sie vorher zu
        // verlieren.
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
            std::fill(d.selected.begin(), d.selected.end(), 0);
            for (std::size_t k = a; k <= b && k < d.selected.size(); ++k) d.selected[k] = 1;
        }

        // Umordnen durch Ziehen.
        //
        // Nur ohne Filter: die Tabelle zeigt dann alle Zeilen, und die
        // Zielposition ist eindeutig. Mit Filter waere "hierhin" mehrdeutig,
        // weil dazwischen ausgeblendete Sequenzen liegen.
        if (!hasFilter) {
            if (wasSelected &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                const int payload = static_cast<int>(i);
                ImGui::SetDragDropPayload("g2c_row", &payload, sizeof(payload));
                ImGui::TextUnformatted(name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("g2c_row")) {
                    const int from = *static_cast<const int*>(pl->Data);
                    std::vector<std::size_t> rows;
                    for (std::size_t k = 0; k < d.selected.size(); ++k)
                        if (d.selected[k]) rows.push_back(k);
                    if (std::find(rows.begin(), rows.end(),
                                  static_cast<std::size_t>(from)) == rows.end())
                        rows = {static_cast<std::size_t>(from)};
                    pendingBlock_ = {rows, i};
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Rechte Maustaste: umordnen, bearbeiten, loeschen.
        if (ImGui::BeginPopupContextItem(("ctx" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem(withIcon(ICON_EDIT, tr(S::CtxEdit)))) {
                editRow_ = static_cast<int>(i);
                editOpen_ = true;
            }
            ImGui::Separator();

            // Betroffen sind alle ausgewaehlten Zeilen; ist die
            // angeklickte nicht darunter, gilt nur sie.
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

            // Ausgewaehlte hierhin verschieben — der Rechtsklick sagt WOHIN.
            //
            // Zusammen mit der Mehrfachauswahl ist das der bequemste Weg,
            // verstreute Sequenzen zusammenzufuehren: erst mit Strg
            // einsammeln, dann an der Zielstelle rechtsklicken.
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

            ImGui::Separator();
            if (ImGui::MenuItem(tr(S::Copy))) {
                const std::size_t n = copyGrabs(static_cast<std::size_t>(active_), rows);
                log(LogLine::Kind::Info, trf(S::Copied, n));
            }
            if (ImGui::MenuItem(tr(S::Cut))) {
                const std::size_t n = copyGrabs(static_cast<std::size_t>(active_), rows);
                log(LogLine::Kind::Info, trf(S::Copied, n));
                pendingDelete_ = rows;
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
                if (ImGui::MenuItem(withIcon(ICON_DELETE, lbl))) pendingDelete_ = rows;
            } else if (ImGui::MenuItem(withIcon(ICON_DELETE, tr(S::DeleteSeq)))) {
                pendingDelete_ = {i};
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

        ImGui::PopID();
    }
    ImGui::EndTable();

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) rangeSelecting_ = false;

    // Umordnen erst HIER ausfuehren, nach der Tabelle.
    //
    // Mitten im Zeichnen die Liste umzustellen, ueber die gerade iteriert
    // wird, ist der klassische Weg zum Absturz. Deshalb merkt sich das
    // Kontextmenue nur, WAS zu tun ist, und getan wird es danach.
    // Einfuegen erst NACH der Tabelle, aus demselben Grund wie das
    // Umordnen: mitten im Zeichnen die Liste zu veraendern, ueber die
    // gerade iteriert wird, ist der klassische Weg zum Absturz.
    if (pendingPaste_ >= 0) {
        const std::size_t n =
            pasteGrabs(static_cast<std::size_t>(active_), static_cast<std::size_t>(pendingPaste_));
        if (n) log(LogLine::Kind::Good, trf(S::Pasted, n));
        pendingPaste_ = -1;
    }

    if (!pendingBlock_.first.empty()) {
        const std::size_t n = moveGrabs(static_cast<std::size_t>(active_),
                                        pendingBlock_.first, pendingBlock_.second);
        if (n) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), tr(S::Moved), std::to_string(n).c_str());
            log(LogLine::Kind::Info, msg);
        }
        pendingBlock_.first.clear();
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

    for (std::size_t i = 0; i < docs_.size();) {
        Document& d = docs_[i];
        std::string label = d.title;
        if (d.dirty) label += " *";
        label += "###" + d.path;

        bool open = true;
        // Der Tab waehlt nur aus; die Tabelle wird unten EINMAL gezeichnet.
        //
        // Innerhalb des Tabs haette die Tabelle je Tab eine eigene Kennung,
        // und ImGui merkte sich die Spaltenbreiten dann zwanzigmal getrennt.
        // Ausserhalb ist die Kennung stabil, und eine einmal eingestellte
        // Breite gilt fuer alle Skripte und ueberlebt den Neustart.
        if (ImGui::BeginTabItem(label.c_str(), &open)) {
            active_ = static_cast<int>(i);
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", d.path.c_str());
        if (!open) { closeDocument(i); continue; }
        ++i;
    }
    ImGui::EndTabBar();

    if (active_ >= 0 && active_ < static_cast<int>(docs_.size())) {
        Document& d = docs_[static_cast<std::size_t>(active_)];
        startFrameWorker(d);
        ImGui::TextDisabled("%s", d.path.c_str());
        drawSequenceTable(d);
    }
}

// Skelettvorschau.
//
// Gezeichnet wird mit ImGuis Zeichenliste, nicht mit DirectX. Ein Skelett
// braucht keine Grafikschnittstelle: ein paar hundert Linien pro Bild
// zeichnet ImGui ohne Muehe, und der Code bleibt damit in einer Datei, die
// sich pruefen laesst.
void App::drawPreviewPanel() {
    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
    auto& ex = extract_;

    if (!ex.loaded) {
        ImGui::TextDisabled("%s", tr(S::PreviewNoGla));
        return;
    }

    // Sequenzauswahl.
    if (previewSeq_ < 0 || previewSeq_ >= static_cast<int>(ex.seqs.size())) previewSeq_ = 0;
    const ExtractSeq& seq = ex.seqs[static_cast<std::size_t>(previewSeq_)];

    ImGui::SetNextItemWidth(320 * k);
    if (ImGui::BeginCombo(tr(S::PreviewSeq), seq.name.c_str())) {
        // Bei tausend Sequenzen nur die zeigen, die zum Filter passen.
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

    // Steuerung.
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

    // Zeit fortschreiben. Die Rate kommt aus der animation.cfg, nicht aus
    // der Bildrate der Oberflaeche: eine Animation mit 20 Bildern je
    // Sekunde soll auch bei 144 Hz mit 20 laufen.
    const double now = ImGui::GetTime();
    const float dt = previewLastTime_ > 0.0
                         ? static_cast<float>(std::min(0.1, now - previewLastTime_))
                         : 0.0f;
    previewLastTime_ = now;
    advancePlayback(playback_, dt, seq.fps, seq.count);
    if (playback_.frame >= seq.count) playback_.frame = 0;

    // Weltposen des aktuellen Frames.
    const int glaFrame = seq.start + playback_.frame;
    const auto world = boneWorldMatrices(ex.gla, glaFrame);
    if (world.empty()) return;

    // Zeichenflaeche.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, std::max(120.0f * k, avail.y));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pvcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    // Beim ersten Bild und nach dem Zuruecksetzen alles ins Bild holen.
    if (previewCam_.distance <= 0.0f || previewCam_.distance == PreviewCamera{}.distance)
        frameAll(previewCam_, computeBounds(world));

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        previewCam_.yawDeg -= d.x * 0.4f;
        previewCam_.pitchDeg += d.y * 0.4f;
        // Nicht ueber den Pol hinaus: sonst kippt das Bild.
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
        // Tiefe als Helligkeit: was weiter weg ist, wird dunkler. Ohne das
        // ist bei einem Skelett von vorn nicht zu erkennen, welcher Arm
        // vorn liegt.
        const float t = std::clamp(l.depth / (previewCam_.distance * 1.6f), 0.0f, 1.0f);
        const int v = static_cast<int>(230.0f - 130.0f * t);
        dl->AddLine(ImVec2(origin.x + l.x0, origin.y + l.y0),
                    ImVec2(origin.x + l.x1, origin.y + l.y1),
                    IM_COL32(v, v, static_cast<int>(v * 0.85f), 255), 1.6f * k);
    }
    for (const auto& l : lines)
        dl->AddCircleFilled(ImVec2(origin.x + l.x1, origin.y + l.y1), 2.0f * k,
                            IM_COL32(120, 190, 255, 255));

    // Kopfzeile in der Ecke.
    char info[128];
    std::snprintf(info, sizeof(info), tr(S::PreviewBones), world.size());
    dl->AddText(ImVec2(origin.x + 8 * k, origin.y + 6 * k), IM_COL32(160, 160, 165, 255), info);
    dl->AddText(ImVec2(origin.x + 8 * k, origin.y + 22 * k), IM_COL32(120, 120, 125, 255),
                tr(S::PreviewHint));
}

// Fehlerliste des aktiven Skripts.
//
// "9 Fehler" allein ist wertlos — man muss sehen, WELCHE und wo. Ein Klick
// springt in die Tabelle und hebt die Zeile hervor.
// Modusleiste am linken Rand.
//
// Zwei Knoepfe statt Reiter oben: die Umschaltung wechselt die GANZE
// Oberflaeche, nicht nur einen Ausschnitt. Am Rand ist das sichtbar genug,
// um nicht versehentlich im falschen Modus zu arbeiten.
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

    // Breite aus dem TEXT bestimmen, nicht raten.
    //
    // Eine feste Zahl passt bestenfalls fuer eine Sprache und eine
    // Bildschirmaufloesung. "XSI -> GLA" war auf Englisch abgeschnitten,
    // waehrend dieselbe Zahl fuer die kuerzeren chinesischen Beschriftungen
    // zu breit gewesen waere.
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
        // Knopf leer zeichnen, Symbol und Text daraufsetzen — so bekommt
        // das Symbol seine eigene Farbe.
        //
        // Beim aktiven Modus bleibt es weiss: auf dem blauen Grund waere
        // eine zweite Farbe unruhig, und welcher Modus laeuft, sagt schon
        // der Hintergrund.
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

// Oberflaeche fuer die Gegenrichtung.
void App::drawExtractPanel() {
    const float k = settings_.dpiScale > 0.0f ? settings_.dpiScale : 1.0f;
    auto& ex = extract_;

    if (iconButton(ICON_OPEN_FILE, kIconAccent, tr(S::OpenGla))) {
        const auto f = askFiles("extractgla", tr(S::DlgTitleGla),
                                "GLA (*.gla)\0*.gla\0Alle\0*.*\0", false);
        if (!f.empty()) loadGlaForExtract(f.front());
    }
    // Die beiden Begleitdateien werden beim Oeffnen selbst gesucht. Die
    // Schaltflaechen bleiben fuer den Fall, dass sie woanders liegen.
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
    // Was gefunden wurde, sichtbar machen — sonst raetselt man, ob die
    // Begleitdateien gegriffen haben.
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

    // Zielordner.
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

    // Vergleichsstand.
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
        exportSequences(rows, ex.outDir);
    }
    ImGui::EndDisabled();

    // Bindepose-Auswahl.
    //
    // Steht bewusst hier bei den Exportknoepfen und nicht in den
    // Einstellungen: sie betrifft nur den Export, und wer sie braucht,
    // sucht sie hier.
    // dotXSI-Fassung.
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
        // Pfadpraefix aus dem Zielordner ableiten: alles ab "models/".
        const std::string norm = fs::path(ex.outDir).generic_string();
        const std::size_t m = norm.rfind("/models/");
        std::string prefix = (m == std::string::npos) ? std::string() : norm.substr(m + 1) + "/";
        const std::string carOut =
            (fs::path(ex.outDir) / (fs::path(ex.glaPath).stem().string() + ".car")).string();
        exportAllWithScript(ex.outDir, carOut, prefix);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::ExportAllWithCarTip));
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(ex.outDir.empty());
    if (iconButton(ICON_SAVE_ALL, kIconInfo, tr(S::ExportAll))) {
        std::vector<std::size_t> rows(ex.seqs.size());
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = i;
        exportSequences(rows, ex.outDir);
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
                if (!ctrl) std::fill(ex.selected.begin(), ex.selected.end(), 0);
                for (std::size_t j = a; j <= b && j < ex.selected.size(); ++j) ex.selected[j] = 1;
            } else if (ctrl) {
                ex.selected[i] = static_cast<char>(!sel);
                extractAnchor_ = static_cast<int>(i);
            } else {
                std::fill(ex.selected.begin(), ex.selected.end(), 0);
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
            // Symbol vor dem Wort: der Blick erfasst die Spalte, ohne zu
            // lesen.
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
            // Zur betroffenen Sequenz springen.
            if (!is.sequence.empty()) {
                for (std::size_t g = 0; g < d.script.grabs.size(); ++g) {
                    const auto& gr = d.script.grabs[g];
                    const std::string nm = gr.enumName ? *gr.enumName : gr.derivedName();
                    bool hit = (nm == is.sequence);
                    for (const auto& a : gr.additional)
                        if (a.name == is.sequence) hit = true;
                    if (hit) { jumpToRow_ = static_cast<int>(g); break; }
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

// Auswahlliste der Enums.
//
// Der Name wird NICHT frei getippt. Ein Tippfehler erzeugt sonst eine
// Sequenz, die es im Spielcode nicht gibt — und das faellt erst im Spiel
// auf, als Animation, die nicht abspielt. Assimilate macht es genauso:
// waehlen oder loeschen, nichts dazwischen.
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

// Bearbeitungsdialog fuer eine Sequenz, wie in Assimilate per Doppelklick.
void App::drawSequenceDialog(Document& d) {
    if (!editOpen_) return;
    if (editRow_ < 0 || editRow_ >= static_cast<int>(d.script.grabs.size())) {
        editOpen_ = false;
        return;
    }
    car::GrabDirective& g = d.script.grabs[static_cast<std::size_t>(editRow_)];
    const std::string name = g.enumName ? *g.enumName : g.derivedName();

    ImGui::SetNextWindowSize(ImVec2(760 * settings_.dpiScale, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(("Sequenz: " + name + "###seqdlg").c_str(), &editOpen_,
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
        // Nur anzeigen, nicht tippen.
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

    // Kopf mit dem, wonach sonst zurueckgefragt wird.
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
        // Art voranstellen: die Farbe geht beim Kopieren verloren, und
        // gerade sie unterscheidet Hinweis von Fehler.
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

    // Breite selbst ausrechnen: Symbol, Abstand, Text.
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
    // Knopfleiste ueber dem Protokoll.
    if (ImGui::SmallButton(withIcon(ICON_DOCUMENT, tr(S::CopyLog)))) {
        const std::string txt = logAsText();
        ImGui::SetClipboardText(txt.c_str());
        std::size_t zeilen = 0;
        for (const char c : txt)
            if (c == '\n') ++zeilen;
        log(LogLine::Kind::Info, trf(S::LogCopied, zeilen));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(S::CopyLogTip));

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
        // Symbol nach Art, dann der Text in derselben Farbe. Das Symbol
        // allein sagt schon, worum es geht.
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

void App::draw() {
    if (!styleApplied_) applyStyle();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    // Tastenkuerzel.
    if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        const auto f = askFiles("car", tr(S::DlgTitleNewCar), "Carcass-Skript (*.car)\0*.car\0",
                                false);
        if (!f.empty()) {
            std::string np = f.front();
            if (fs::path(np).extension().empty()) np += ".car";
            newCar(np);
        }
    }
    if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (ImGui::IsKeyDown(ImGuiMod_Shift)) saveAllDocuments();
        else if (!docs_.empty()) saveDocument(static_cast<std::size_t>(active_));
    }

    if (ImGui::Begin("##main", nullptr, flags)) {
        drawMenuBar();
        if (mode_ == Mode::Build) drawToolbar();
        ImGui::Separator();

        // Hoehen ausrechnen statt zwei verschiedene nebeneinanderstellen.
        //
        // Vorher nahm die Einstellungsleiste die volle Resthoehe, waehrend
        // die Tabelle daneben eine feste Hoehe bekam. Die beiden passten
        // nicht zusammen: darunter blieb eine Leerflaeche stehen, und je
        // nach Bildschirm musste man rollen, um das Protokoll zu sehen.
        //
        // Jetzt teilen sich beide denselben Bereich, und der untere Teil
        // haengt an der tatsaechlich verfuegbaren Hoehe.
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

        // Auskunft ueber das Programm selbst.
        //
        // Die wichtigste Zeile darin ist die Laufzeit: ohne fest eingebaute
        // startet das Programm auf fremden Rechnern gar nicht, und der
        // Empfaenger kann daran nichts aendern. Diese Frage soll man
        // beantworten koennen, ohne Werkzeuge zu installieren.
        if (showAbout_) {
            ImGui::SetNextWindowSize(ImVec2(560 * settings_.dpiScale, 0), ImGuiCond_Appearing);
            if (ImGui::Begin(tr(S::About), &showAbout_, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(tr(S::AboutBuilt), __DATE__, __TIME__);
                ImGui::Text(tr(S::AboutBits), sizeof(void*) * 8, g2::defaultThreadCount());

                // Wo das Startprotokoll liegt. Bei "startet nicht" ist das
                // die erste Frage — hier steht die Antwort, statt dass man
                // sie erfragen muss.
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

        // Loeschen bestaetigen. Rueckgaengig gibt es nicht, und ein
        // verrutschter Rechtsklick soll keine Sequenz kosten.
        if (!pendingDelete_.empty()) ImGui::OpenPopup("confirmdel");
        if (ImGui::BeginPopupModal("confirmdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(tr(S::ConfirmDelete), pendingDelete_.size());
            ImGui::Spacing();
            if (ImGui::Button(withIcon(ICON_DELETE, tr(S::DeleteSeq)))) {
                const std::size_t n =
                    deleteGrabs(static_cast<std::size_t>(active_), pendingDelete_);
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
    }
    ImGui::End();
}

}  // namespace g2::gui
