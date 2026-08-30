// g2c — Kommandozeilenwerkzeug fuer Ghoul2-Dateien.
//
//   g2c info  <datei.gla>    Header, Skelett und Poolstatistik ausgeben
//   g2c check <datei.gla>    Struktur pruefen und Requantisierungsfehler messen

#include "g2/mdxa.h"
#include "g2/xsi.h"
#include "g2/carscript.h"
#include "g2/readfile.h"
#include "g2/xsi_anim.h"
#include "g2/xsi_export.h"
#include "g2/carbuild.h"
#include "g2/parallel.h"
#include "g2/gladiff.h"
#include "g2/animcache.h"
#include "g2/animenums.h"
#include "g2/carvalidate.h"
#include "g2/xsi_mesh.h"
#include "g2/sidefiles.h"
#include "g2/mdxm.h"
#include "g2/mdxm.h"
#include <cmath>

#include <chrono>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <array>
#include <cctype>
#include <charconv>
#include <locale>
#include <string_view>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) { return g2::readWholeFileBytes(path); }

// Zahlen aus der Kommandozeile ebenfalls mit from_chars lesen.
//
// std::stof und Verwandte gehen ueber strtof und haengen damit am C-Locale
// (LC_NUMERIC). Auf einem deutschen System kann "0.64" dort als 0 ankommen,
// sobald irgendetwas setlocale aufruft. Genau daran ist das alte Carcass
// gescheitert: es lief nur mit amerikanischen Regionseinstellungen.
// from_chars kennt kein Locale und akzeptiert immer den Punkt.
float argFloat(const char* s, const char* what) {
    float v = 0.0f;
    const std::string_view sv(s);
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
        throw std::runtime_error(std::string(what) + ": \"" + s + "\" ist keine Zahl");
    return v;
}

long argInt(const char* s, const char* what) {
    long v = 0;
    const std::string_view sv(s);
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
        throw std::runtime_error(std::string(what) + ": \"" + s + "\" ist keine ganze Zahl");
    return v;
}

void printTree(const g2::Skeleton& s, const std::vector<std::vector<int>>& kids, int bone,
               int depth, int& printed, int limit) {
    if (printed >= limit) return;
    std::printf("  %*s%s\n", depth * 2, "", s.bones[static_cast<std::size_t>(bone)].name.c_str());
    ++printed;
    for (int k : kids[static_cast<std::size_t>(bone)]) printTree(s, kids, k, depth + 1, printed, limit);
}

// Kurzauskunft zum Programm selbst.
// Pfad der animation.cfg neben einer Ausgabedatei.
//
// Immer "animation.cfg" — so sucht die Engine sie. Eine Sicherung der
// vorhandenen wird angelegt, bevor sie ersetzt wird.
std::string cfgNextTo(const std::string& outPath) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(outPath).parent_path();
    return (dir.empty() ? fs::path("animation.cfg") : dir / "animation.cfg").string();
}

int cmdAbout() {
    std::printf("g2c - Ghoul2 Toolkit\n");
    std::printf("  Gebaut       : %s %s\n", __DATE__, __TIME__);
    std::printf("  Adressbreite : %zu Bit\n", sizeof(void*) * 8);
    std::printf("  Kerne        : %u\n", g2::defaultThreadCount());
    // Wo das Startprotokoll liegt.
    //
    // Bei einer Fehlermeldung ist das die erste Frage, und sie laesst sich
    // hier beantworten statt im Forum. Ueber argv[0] statt GetModuleFileName,
    // damit die Datei ohne <windows.h> auskommt und sich auch hier
    // uebersetzen laesst.
    {
        std::error_code ec;
        const auto exe = std::filesystem::canonical("/proc/self/exe", ec);
        std::filesystem::path dir = ec ? std::filesystem::path() : exe.parent_path();
        if (dir.empty()) dir = std::filesystem::current_path(ec);
        if (!dir.empty())
            std::printf("  Protokoll    : %s\n",
                        (dir / "g2c-startup.log").string().c_str());
    }
    if (g2::staticRuntime())
        std::printf("  Laufzeit     : fest eingebaut - laeuft auf jedem Windows 10/11\n"
                    "                 ohne Visual C++ Redistributable.\n");
    else
        std::printf("  Laufzeit     : DYNAMISCH GEBUNDEN.\n"
                    "                 Auf Rechnern ohne Visual C++ Redistributable\n"
                    "                 startet dieses Programm NICHT.\n"
                    "                 In CMakeLists.txt CMAKE_MSVC_RUNTIME_LIBRARY\n"
                    "                 auf \"MultiThreaded\" setzen.\n");
    return 0;
}

int cmdInfo(const std::string& path) {
    const auto data = readFile(path);
    const g2::MdxaFile f = g2::readMdxa(data);

    std::printf("Datei        : %s\n", path.c_str());
    std::printf("Groesse      : %zu Bytes\n", data.size());
    std::printf("GLA-Name     : %s\n", f.skeleton.name.c_str());
    std::printf("Scale        : %g\n", f.skeleton.scale);
    std::printf("Frames       : %d\n", f.numFrames);
    std::printf("Bones        : %zu\n", f.skeleton.bones.size());
    std::printf("Pool         : %zu Eintraege (%zu Bytes)\n", f.bonePool.size(),
                f.bonePool.size() * 14);

    const std::size_t naive = f.indices.size();
    if (naive) {
        std::printf("Dedupe       : %zu von %zu Bone-Instanzen eingespart (%.1f%%)\n",
                    naive - f.bonePool.size(), naive,
                    100.0 * (1.0 - double(f.bonePool.size()) / double(naive)));
    }

    const auto kids = f.skeleton.buildChildLists();
    std::printf("\nHierarchie (erste 30 Bones):\n");
    int printed = 0;
    for (std::size_t i = 0; i < f.skeleton.bones.size(); ++i)
        if (f.skeleton.bones[i].parent < 0)
            printTree(f.skeleton, kids, static_cast<int>(i), 0, printed, 30);
    if (f.skeleton.bones.size() > 30) std::printf("  ... (%zu weitere)\n", f.skeleton.bones.size() - 30);

    return 0;
}

int cmdCheck(const std::string& path) {
    const auto data = readFile(path);
    const g2::MdxaFile f = g2::readMdxa(data);

    std::printf("Datei: %s\n\n", path.c_str());

    const auto errs = f.skeleton.validate();
    if (errs.empty()) {
        std::printf("Skelett: in Ordnung (%zu Bones)\n", f.skeleton.bones.size());
    } else {
        std::printf("Skelett: %zu Problem(e)\n", errs.size());
        for (const auto& e : errs) std::printf("  - %s\n", e.c_str());
    }

    // Wie gross waere der Fehler, wenn man die dekodierten Matrizen neu
    // quantisiert? Bei korrektem Runden sollte das nahezu verlustfrei sein,
    // weil die Werte bereits auf dem Gitter liegen. Grosse Abweichungen
    // deuten auf beschaedigte oder ungewoehnliche Daten hin.
    g2::MdxaWriteOptions opt;
    const auto rep = g2::compareRoundTrip(f, opt);
    std::printf("\nRequantisierung (Nearest):\n%s\n", rep.describe().c_str());

    g2::MdxaWriteOptions legacy;
    legacy.compress.rounding = g2::Rounding::Legacy;
    legacy.compress.canonicalizeSign = false;
    const auto repL = g2::compareRoundTrip(f, legacy);
    std::printf("\nRequantisierung (Legacy/Carcass):\n%s\n", repL.describe().c_str());

    if (repL.maxPositionError > 0 && rep.maxPositionError >= 0) {
        std::printf("\nVerhaeltnis max. Positionsfehler Legacy/Nearest: %.2fx\n",
                    rep.maxPositionError > 0 ? repL.maxPositionError / rep.maxPositionError : 0.0);
    }
    return errs.empty() ? 0 : 1;
}

int cmdXsi(const std::string& path) {
    const auto t0 = std::chrono::steady_clock::now();
    const g2::xsi::Document doc = g2::xsi::parseFile(path);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const double mb = double(f.tellg()) / (1024.0 * 1024.0);

    std::printf("Datei     : %s\n", path.c_str());
    std::printf("Groesse   : %.2f MB\n", mb);
    std::printf("Version   : dotXSI %s (%s)\n", doc.version.toString().c_str(),
                doc.version.raw.c_str());
    std::printf("Templates : %zu\n", doc.templateCount());
    std::printf("Parsezeit : %.1f ms  (%.1f MB/s)\n", ms, ms > 0 ? mb / (ms / 1000.0) : 0.0);

    std::printf("\nVorkommende Templatetypen:\n");
    std::printf("  %-32s %10s %7s\n", "Typ", "Anzahl", "Tiefe");
    for (const auto& tc : g2::xsi::summarize(doc))
        std::printf("  %-32s %10zu %7zu\n", tc.type.c_str(), tc.count, tc.maxDepth);
    return 0;
}

int cmdCar(const std::string& path, bool argc_verbose) {
    const g2::car::Script s = g2::car::parseFile(path);

    std::printf("Datei      : %s\n", path.c_str());
    std::printf("$basedir   : %s\n", s.baseDir.empty() ? "(nicht gesetzt)" : s.baseDir.c_str());
    std::printf("$modelname : %s\n", s.modelName.empty() ? "(nicht gesetzt)" : s.modelName.c_str());
    if (s.scale) std::printf("$scale     : %g\n", *s.scale);
    if (s.origin)
        std::printf("$origin    : %g %g %g\n", (*s.origin)[0], (*s.origin)[1], (*s.origin)[2]);
    std::printf("$flatten   : %s\n", s.flatten ? "ja" : "nein");
    std::printf("$keepmotion: %s\n", s.keepMotion ? "ja" : "nein");

    std::printf("$pcj       : %zu Bones%s\n", s.pcjBones.size(),
                s.pcjFlatten ? " (+ $flatten)" : "");
    if (s.convert) {
        std::printf("Konvertierung: root=%s\n", s.convert->root.c_str());
        if (!s.convert->makeSkel.empty())
            std::printf("  -makeskel  : %s\n", s.convert->makeSkel.c_str());
        if (s.convert->origin)
            std::printf("  -origin    : %g %g %g\n", (*s.convert->origin)[0],
                        (*s.convert->origin)[1], (*s.convert->origin)[2]);
    }

    const auto seqs = s.buildSequences();
    std::size_t add = 0, qd = 0;
    for (const auto& q : seqs) { if (q.fromAdditional) ++add; if (q.insideQdSkip) ++qd; }
    std::printf("\n$aseanimgrab : %zu\n", s.grabs.size());
    std::printf("Sequenzen    : %zu (davon %zu aus -additional, %zu in -qdskip-Regionen)\n",
                seqs.size(), add, qd);

    std::size_t unknown = 0;
    if (argc_verbose) {
        std::printf("\n%zu Anweisungen:\n", s.statements.size());
    for (const auto& st : s.statements) {
        std::printf("  %4zu  %-26s", st.line, st.raw.c_str());
        for (const auto& a : st.args) std::printf(" %s", a.c_str());
        if (st.cmd == g2::car::Cmd::Unknown) { std::printf("   <-- unbekannter Befehl"); ++unknown; }
        std::printf("\n");
    }
    }
    if (unknown) std::printf("\n%zu unbekannte Befehle — bitte melden.\n", unknown);
    return 0;
}



// g2c anim <referenz.gla> <ausgabe.gla> <anim1.xsi> [anim2.xsi ...]
int cmdAnim(int argc, char** argv) {
    const std::string refPath = argv[2];
    const std::string outPath = argv[3];

    const g2::MdxaFile ref = g2::readMdxa(readFile(refPath));
    std::printf("Referenz  : %s\n", refPath.c_str());
    std::printf("            %zu Bones, Scale %g, GLA-Name \"%s\"\n",
                ref.skeleton.bones.size(), ref.skeleton.scale, ref.skeleton.name.c_str());

    g2::xsi::EvalOptions opt;
    opt.scale = ref.skeleton.scale > 0.0f ? ref.skeleton.scale : 1.0f;

    std::vector<std::pair<std::string, g2::xsi::AnimFile>> anims;
    for (int i = 4; i < argc; ++i) {
        std::string path = argv[i];

        // -origin x y z  (wie in der .car)
        if (path == "-origin" && i + 3 < argc) {
            opt.origin = std::array<float, 3>{argFloat(argv[i + 1], "-origin X"),
                                              argFloat(argv[i + 2], "-origin Y"),
                                              argFloat(argv[i + 3], "-origin Z")};
            i += 3;
            continue;
        }
        // -alias glaName=xsiName
        if (path == "-alias" && i + 1 < argc) {
            const std::string a = argv[++i];
            const std::size_t eq = a.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "-alias braucht die Form glaName=xsiName\n");
                return 1;
            }
            opt.aliases[a.substr(0, eq)] = a.substr(eq + 1);
            continue;
        }
        auto anim = g2::xsi::loadAnimationFile(path);

        // Sequenzname wie Carcass: Dateiname ohne Pfad und Endung, gross.
        std::string name = path;
        const std::size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const std::size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        std::printf("  + %-28s %3d Frames, %3zu Knoten\n", name.c_str(), anim.frameCount(),
                    anim.nodes.size());
        anims.emplace_back(name, std::move(anim));
    }
    if (anims.empty()) {
        std::fprintf(stderr, "Keine Animationsdateien angegeben.\n");
        return 1;
    }

    const auto cat = g2::xsi::concatenate(ref.skeleton, anims, opt);
    for (const auto& w : cat.warnings) std::printf("  ! %s\n", w.c_str());

    const auto res = g2::writeMdxa(ref.skeleton, cat.frames);
    std::printf("\nErgebnis  : %d Frames, %zu Pool-Eintraege (%.1f%% dedupliziert)\n",
                cat.frames.frameCount(), res.poolEntries, res.dedupeRatio() * 100.0);
    std::printf("Kompression: %s\n", res.stats.summary().c_str());

    // Zielverzeichnis anlegen, falls es noch nicht existiert. Sonst
    // scheitert ein Aufruf wie -o neu/_humanoid.gla nach getaner Arbeit.
    {
        std::error_code ec;
        const auto dir = std::filesystem::path(outPath).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    g2::writeFileChecked(outPath, res.data.data(), res.data.size());
    std::printf("Geschrieben: %s (%zu Bytes)\n", outPath.c_str(), res.data.size());

    // Passende animation.cfg daneben legen.
    std::vector<g2::car::Sequence> seqs;
    for (const auto& s : cat.sequences)
        seqs.push_back({s.name, s.targetFrame, s.frameCount, -1, 20, s.sourceFile, false, false});
    // Sie heisst "animation.cfg" und liegt neben der GLA.
    //
    // Frueher hiess sie "<name>_animation.cfg". Das ueberschreibt nichts und
    // liegt harmlos daneben — aber die Engine sucht "animation.cfg", und wer
    // den Ausgabeordner ins Spiel kopiert, hat weiterhin die alte.
    //
    // Das ist keine Kleinigkeit: beim Einfuegen von Animationen verschieben
    // sich ALLE nachfolgenden Zielframes. Passt die cfg nicht zur GLA, laeuft
    // bei jedem Namen die Animation, die zufaellig an dieser Stelle steht.
    const std::string cfgPath = cfgNextTo(outPath);
    const std::string cfg = g2::car::writeAnimationCfg(
        seqs, std::to_string(cat.frames.frameCount()) + " frames; " +
                  std::to_string(seqs.size()) + " sequences; erzeugt von g2c");
    g2::writeFileChecked(cfgPath, cfg);
    std::printf("Geschrieben: %s\n", cfgPath.c_str());
    std::printf("             Diese Datei gehoert ZUSAMMEN mit der GLA kopiert.\n"
                "             Ohne sie spielt das Spiel an jeder Stelle die Animation ab,\n"
                "             die dort zufaellig steht.\n");
    std::printf("\nHinweis: frameSpeed und loopFrame stehen auf Vorgabewerten (20 / -1).\n"
                "Die echten Werte kommen aus den -framespeed/-loop Flags der .car.\n");
    return 0;
}

// g2c build <datei.car> -ref <referenz.gla> [-basedir <pfad>] [-o <ausgabe.gla>]
int writeMeshFromScript(const g2::car::Script& script, const g2::MdxaFile& ref,
                        const std::string& baseDir, const std::string& carPath,
                        const std::string& outDir);
int runBuild(const std::string& carPath, const std::string& refPath, std::string outPath,
             g2::car::BuildOptions bo, const g2::car::Script& script, const g2::MdxaFile& ref,
             bool noMesh);

int cmdBuild(int argc, char** argv) {
    const std::string carPath = argv[2];
    std::string refPath, outPath, baseDir;
    g2::car::BuildOptions bo;
    bool noMesh = false;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-ref" && i + 1 < argc) refPath = argv[++i];
        else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else if (a == "-basedir" && i + 1 < argc) baseDir = argv[++i];
        else if (a == "-skipmissing") bo.skipMissing = true;
        else if (a == "-carcass") bo.carcassCompatible = true;
        else if (a == "-threads" && i + 1 < argc)
            bo.threads = static_cast<unsigned>(argInt(argv[++i], "-threads"));
        else if (a == "-cache" && i + 1 < argc) bo.cacheDir = argv[++i];
        else if (a == "-nocache") bo.cacheDir = "(aus)";
        else if (a == "-clearcache") bo.cacheDir = "(leeren)";
        else if (a == "-nomesh") noMesh = true;
        else if (a == "-framespeed" && i + 1 < argc)
            bo.defaultFrameSpeed = static_cast<int>(argInt(argv[++i], "-framespeed"));
        else if (a == "-origin" && i + 3 < argc) {
            bo.originOverride = std::array<float, 3>{
                argFloat(argv[i + 1], "-origin X"), argFloat(argv[i + 2], "-origin Y"),
                argFloat(argv[i + 3], "-origin Z")};
            i += 3;
        } else {
            std::fprintf(stderr, "Unbekannte Option: %s\n", a.c_str());
            return 1;
        }
    }
    if (refPath.empty()) {
        std::fprintf(stderr,
                     "-ref <referenz.gla> fehlt.\n"
                     "Das Skelett wird aus einer vorhandenen GLA uebernommen; aus den\n"
                     "Quelldateien allein laesst es sich nicht ableiten.\n");
        return 1;
    }
    bo.baseDir = baseDir;

    const g2::car::Script script = g2::car::parseFile(carPath);

    // Vorgabe: ein Ordner neben der .car. Damit funktioniert der Cache ohne
    // Zutun und liegt dort, wo auch die Ausgabe entsteht.
    if (bo.cacheDir == "(aus)") {
        bo.cacheDir.clear();
    } else if (bo.cacheDir == "(leeren)") {
        bo.cacheDir = (std::filesystem::path(carPath).parent_path() / "g2c_cache").string();
        g2::AnimCache c(bo.cacheDir);
        std::printf("Cache geleert: %zu Eintraege entfernt\n", c.clear());
    } else if (bo.cacheDir.empty()) {
        bo.cacheDir = (std::filesystem::path(carPath).parent_path() / "g2c_cache").string();
    }
    const g2::MdxaFile ref = g2::readMdxa(readFile(refPath));
    return runBuild(carPath, refPath, outPath, bo, script, ref, noMesh);
}

int runBuild(const std::string& carPath, const std::string& refPath, std::string outPath,
             g2::car::BuildOptions bo, const g2::car::Script& script, const g2::MdxaFile& ref,
             bool noMesh) {
    std::printf("Skript    : %s\n", carPath.c_str());
    std::printf("Referenz  : %s (%zu Bones, Scale %g)\n", refPath.c_str(),
                ref.skeleton.bones.size(), ref.skeleton.scale);
    std::printf("$aseanimgrab: %zu\n", script.grabs.size());
    std::printf("Threads   : %u\n", bo.threads ? bo.threads : g2::defaultThreadCount());
    if (script.convert && script.convert->origin)
        std::printf("-origin   : %g %g %g (aus dem Skript)\n", (*script.convert->origin)[0],
                    (*script.convert->origin)[1], (*script.convert->origin)[2]);
    std::printf("\n");

    std::size_t lastPct = 999;
    bo.progress = [&](std::size_t i, std::size_t n, const std::string&) {
        const std::size_t pct = n ? (i * 100 / n) : 100;
        if (pct != lastPct && pct % 5 == 0) {
            lastPct = pct;
            std::printf("\r  %3zu%%  (%zu/%zu)", pct, i, n);
            std::fflush(stdout);
        }
    };

    const g2::car::BuildResult br = g2::car::build(script, ref.skeleton, carPath, bo);
    std::printf("\r                            \r");

    if (!bo.cacheDir.empty()) {
        const g2::AnimCache c(bo.cacheDir);
        std::printf("Cache     : %llu Treffer, %llu neu (%.1f%%), %.1f MB auf Platte\n",
                    static_cast<unsigned long long>(br.cache.hits),
                    static_cast<unsigned long long>(br.cache.misses), br.cache.hitPercent(),
                    static_cast<double>(c.sizeOnDisk()) / (1024.0 * 1024.0));
    }

    for (const auto& w : br.warnings) std::printf("  ! %s\n", w.c_str());
    if (!br.warnings.empty()) std::printf("\n");

    g2::MdxaWriteOptions wo;
    if (br.carcassCompatible) {
        wo.compress.rounding = g2::Rounding::Legacy;
        wo.compress.optimizeQuat = false;
        wo.compress.canonicalizeSign = false;
        std::printf("Quantisierung: Carcass-kompatibel (Abschneiden)\n");
    }
    wo.threads = bo.threads;

    // Der GLA-Name im Kopf kommt aus -makeskel, NICHT aus der Referenz.
    //
    // Er steht als Zeichenkette in der Datei und sagt der Engine, welches
    // Skelett das hier ist. Uebernahmen wir ihn von der Referenz-GLA, trug
    // jede gebaute Datei "models/players/_humanoid/_humanoid" — egal wohin
    // sie gehoerte.
    //
    // Die Folge im Spiel: ein eigener Humanoid meldete sich als der
    // Standard-Humanoid. Die Engine nahm dann dessen animation.cfg und
    // spielte an jeder Stelle die Animation ab, die dort zufaellig stand.
    // Genau danach sah es aus, und genau deshalb war es an den Animationen
    // selbst nie zu finden.
    g2::Skeleton outSkel = ref.skeleton;
    if (script.convert && !script.convert->makeSkel.empty()) {
        std::string ms = script.convert->makeSkel;
        std::replace(ms.begin(), ms.end(), '\\', '/');
        if (ms != outSkel.name) {
            std::printf("GLA-Name  : %s\n", ms.c_str());
            std::printf("            (aus -makeskel; die Referenz heisst \"%s\")\n",
                        outSkel.name.c_str());
        }
        outSkel.name = ms;
    }

    const auto res = g2::writeMdxa(outSkel, br.frames, wo);

    std::printf("Frames    : %d\n", br.totalFrames());
    std::printf("Sequenzen : %zu\n", br.sequences.size());
    std::printf("Pool      : %zu Eintraege (%.1f%% dedupliziert)\n", res.poolEntries,
                res.dedupeRatio() * 100.0);
    std::printf("Kompression: %s\n\n", res.stats.summary().c_str());

    // Ausgabepfade. Ohne -o richtet sich der Name nach -makeskel im Skript.
    if (outPath.empty()) {
        if (script.convert && !script.convert->makeSkel.empty()) {
            const std::string& ms = script.convert->makeSkel;
            const std::size_t slash = ms.find_last_of("/\\");
            outPath = (slash == std::string::npos ? ms : ms.substr(slash + 1)) + ".gla";
        } else {
            outPath = "out.gla";
        }
    }
    // Zielverzeichnis anlegen, falls es noch nicht existiert. Sonst
    // scheitert ein Aufruf wie -o neu/_humanoid.gla nach getaner Arbeit.
    {
        std::error_code ec;
        const auto dir = std::filesystem::path(outPath).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    g2::writeFileChecked(outPath, res.data.data(), res.data.size());
    std::printf("Geschrieben: %s (%zu Bytes)\n", outPath.c_str(), res.data.size());

    const std::size_t dot = outPath.find_last_of('.');
    const std::string cfgPath = cfgNextTo(outPath);
    std::ostringstream head;
    head << br.totalFrames() << " frames; " << br.sequences.size() << " sequences; erzeugt von g2c";
    const std::string cfg = g2::car::writeAnimationCfg(br.sequences, head.str());
    g2::writeFileChecked(cfgPath, cfg);
    std::printf("Geschrieben: %s\n", cfgPath.c_str());

    // Deutlich sagen, dass beide zusammengehoeren.
    //
    // Beim Einfuegen von Animationen verschieben sich ALLE nachfolgenden
    // Zielframes. Bleibt im Spiel die alte animation.cfg liegen, laeuft bei
    // jedem Namen die Animation, die dort zufaellig steht — und der Fehler
    // sieht wie ein kaputtes Werkzeug aus, nicht wie eine vergessene Datei.
    std::printf("\n"
                "WICHTIG: %s gehoert ZUSAMMEN mit der GLA kopiert.\n"
                "         Die Zielframes haben sich gegenueber der alten Fassung\n"
                "         verschoben. Bleibt die alte Datei im Spiel liegen, wird bei\n"
                "         jedem Namen die Animation abgespielt, die dort zufaellig steht.\n"
                "         Das betrifft auch jedes andere Modell, das dieselbe GLA nutzt.\n\n",
                std::filesystem::path(cfgPath).filename().string().c_str());

    // .frames danebenlegen, wie Carcass es tut.
    if (!br.frameBlocks.empty()) {
        std::vector<g2::FrameEntry> fe;
        fe.reserve(br.frameBlocks.size());
        for (const auto& b : br.frameBlocks) {
            g2::FrameEntry e;
            // Carcass schreibt absolute Pfade mit Schraegstrichen.
            e.sourcePath = std::filesystem::absolute(b.sourcePath).generic_string();
            e.startFrame = b.startFrame;
            e.duration = b.duration;
            e.fps = b.fps;
            for (int k = 0; k < 3; ++k) e.averageVec[k] = b.averageVec[k];
            fe.push_back(std::move(e));
        }
        const std::string framesPath =
            (dot == std::string::npos ? outPath : outPath.substr(0, dot)) + ".frames";
        const std::string txt = g2::writeFrames(fe);
        {
            g2::writeFileChecked(framesPath, txt);
            std::printf("Geschrieben: %s (%zu Bloecke)\n", framesPath.c_str(), fe.size());
        }
    }

    if (!noMesh) {
        const std::string outDir = std::filesystem::path(outPath).parent_path().string();
        writeMeshFromScript(script, ref, bo.baseDir, carPath, outDir.empty() ? "." : outDir);
    }
    return 0;
}


// Aus dem Skript die Mesh-Quelle ableiten und die GLM daneben schreiben.
//
// Carcass erzeugt in einem Lauf GLA UND GLM. Die Mesh-Quelle steht in
// $aseanimconvertmdx als <root> ohne Endung; im Log des Originals steht
// entsprechend "Processing 'c:/.../_humanoid/root.XSI'". Die GLM landet
// neben dieser Datei und traegt den Namen ihres Verzeichnisses.
int writeMeshFromScript(const g2::car::Script& script, const g2::MdxaFile& ref,
                        const std::string& baseDir, const std::string& carPath,
                        const std::string& outDir) {
    if (!script.convert || script.convert->root.empty()) {
        std::printf("\nKeine Mesh-Quelle im Skript ($aseanimconvertmdx ohne <root>).\n");
        return 0;
    }
    namespace fs = std::filesystem;
    const std::string carDir = fs::path(carPath).parent_path().string();

    // <root> hat keine Endung; .xsi und .XSI probieren.
    std::string xsiPath;
    for (const char* ext : {".xsi", ".XSI"}) {
        xsiPath = g2::car::resolveAssetPath(script.convert->root + ext, baseDir, carDir);
        if (!xsiPath.empty()) break;
    }
    if (xsiPath.empty()) {
        std::printf("\nMesh-Quelle \"%s.xsi\" nicht gefunden — GLA wurde trotzdem geschrieben.\n",
                    script.convert->root.c_str());
        return 0;
    }

    // Name wie bei Carcass: nach dem Verzeichnis der Mesh-Quelle.
    std::string stem = fs::path(xsiPath).parent_path().filename().string();
    if (stem.empty()) stem = "model";
    const fs::path outGlm = fs::path(outDir) / (stem + ".glm");

    g2::xsi::MeshImportOptions mo;
    mo.scale = ref.skeleton.scale > 0.0f ? ref.skeleton.scale : 1.0f;
    mo.animName = ref.skeleton.name;
    mo.modelName = stem + ".glm";
    for (const auto& b : ref.skeleton.bones) mo.boneNames.push_back(b.name);

    try {
        std::printf("\nMesh      : %s\n", xsiPath.c_str());
        const auto r = g2::xsi::importMeshFile(xsiPath, mo);
        std::printf("Surfaces  : %zu  (davon %zu Tags, %zu auf OFF)\n", r.stats.surfaces,
                    r.stats.tags, r.stats.offSurfaces);
        std::printf("Vertices  : %zu, Dreiecke: %zu\n", r.stats.vertices, r.stats.triangles);
        for (std::size_t i = 0; i < r.stats.warnings.size() && i < 5; ++i)
            std::printf("  ! %s\n", r.stats.warnings[i].c_str());

        const auto w = g2::writeMdxm(r.mesh);
        std::error_code ec;
        fs::create_directories(outDir, ec);
        g2::writeFileChecked(outGlm.string(), w.data.data(), w.data.size());
        std::printf("Geschrieben: %s (%zu Bytes)\n", outGlm.string().c_str(), w.data.size());

        const auto outSkin = fs::path(outDir) / (stem + ".skin");
        const std::string skin = g2::writeSkin(r.mesh);
        g2::writeFileChecked(outSkin.string(), skin);
        {
            std::size_t lines = 0;
            for (char c : skin)
                if (c == '\n') ++lines;
            std::printf("Geschrieben: %s (%zu Eintraege)\n", outSkin.string().c_str(), lines);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Mesh fehlgeschlagen: %s\n", e.what());
        std::printf("Die GLA wurde trotzdem geschrieben.\n");
        return 1;
    }
    return 0;
}

// g2c mesh <root.xsi> -ref <referenz.gla> [-o <aus.glm>] [-compare <ref.glm>]
int cmdMesh(int argc, char** argv) {
    const std::string xsiPath = argv[2];
    std::string refPath, outPath, comparePath;
    bool writeSkinFile = false;
    g2::xsi::MeshImportOptions mo;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-ref" && i + 1 < argc) refPath = argv[++i];
        else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else if (a == "-compare" && i + 1 < argc) comparePath = argv[++i];
        else if (a == "-normaltol" && i + 1 < argc) mo.normalTolerance = argFloat(argv[++i], "-normaltol");
        else if (a == "-uvtol" && i + 1 < argc) mo.uvTolerance = argFloat(argv[++i], "-uvtol");
        else if (a == "-makeskin") writeSkinFile = true;
        else if (a == "-alias" && i + 1 < argc) {
            const std::string x = argv[++i];
            const std::size_t eq = x.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "-alias braucht glaName=xsiName\n"); return 1; }
            mo.aliases[x.substr(0, eq)] = x.substr(eq + 1);
        } else { std::fprintf(stderr, "Unbekannte Option: %s\n", a.c_str()); return 1; }
    }
    if (refPath.empty()) {
        std::fprintf(stderr, "-ref <referenz.gla> fehlt. Das Skelett und der Scale kommen von dort.\n");
        return 1;
    }

    const g2::MdxaFile ref = g2::readMdxa(readFile(refPath));
    mo.scale = ref.skeleton.scale > 0.0f ? ref.skeleton.scale : 1.0f;
    mo.animName = ref.skeleton.name;
    for (const auto& b : ref.skeleton.bones) mo.boneNames.push_back(b.name);

    if (outPath.empty()) outPath = "out.glm";
    {
        std::string base = outPath;
        const std::size_t sl = base.find_last_of("/\\");
        if (sl != std::string::npos) base = base.substr(sl + 1);
        mo.modelName = base;
    }

    std::printf("Quelle    : %s\n", xsiPath.c_str());
    std::printf("Referenz  : %s (%zu Bones, Scale %g)\n", refPath.c_str(),
                ref.skeleton.bones.size(), ref.skeleton.scale);
    std::printf("Toleranzen: Normale %g, UV %g\n\n", mo.normalTolerance, mo.uvTolerance);

    const auto r = g2::xsi::importMeshFile(xsiPath, mo);
    std::printf("Surfaces  : %zu  (davon %zu Tags, %zu auf OFF)\n", r.stats.surfaces,
                r.stats.tags, r.stats.offSurfaces);
    std::printf("Vertices  : %zu\n", r.stats.vertices);
    std::printf("Dreiecke  : %zu\n", r.stats.triangles);
    for (std::size_t i = 0; i < r.stats.warnings.size() && i < 8; ++i)
        std::printf("  ! %s\n", r.stats.warnings[i].c_str());

    if (!comparePath.empty()) {
        // Gegen eine vorhandene GLM stellen: Surfacenamen und Vertexzahlen.
        const auto d = readFile(comparePath);
        const auto i32 = [&](std::size_t o) {
            std::int32_t v;
            std::memcpy(&v, d.data() + o, 4);
            return v;
        };
        const int nS = i32(152), lod = i32(148);
        std::map<std::string, int> want;
        for (int i = 0; i < nS; ++i) {
            const int so = i32(static_cast<std::size_t>(lod + 4 + i * 4));
            const int ho = 164 + i32(static_cast<std::size_t>(164 + i * 4));
            want[std::string(reinterpret_cast<const char*>(d.data() + ho))] =
                i32(static_cast<std::size_t>(lod + 4 + so + 12));
        }
        int ok = 0, refTot = 0, mine = 0;
        std::vector<std::string> diffs;
        for (const auto& [k, v] : want) refTot += v;
        for (const auto& s : r.mesh.lods[0].surfaces) {
            mine += static_cast<int>(s.vertices.size());
            const auto it = want.find(s.name);
            if (it == want.end()) { diffs.push_back(s.name + ": in der Referenz nicht vorhanden"); continue; }
            if (it->second == static_cast<int>(s.vertices.size())) ++ok;
            else diffs.push_back(s.name + ": " + std::to_string(s.vertices.size()) + " gegen " +
                                 std::to_string(it->second));
        }
        std::printf("\nVergleich mit %s:\n", comparePath.c_str());
        std::printf("  Surfaces mit exakter Vertexzahl: %d von %zu\n", ok, want.size());
        std::printf("  Vertices gesamt: %d gegen %d\n", mine, refTot);
        for (std::size_t i = 0; i < diffs.size() && i < 10; ++i)
            std::printf("    %s\n", diffs[i].c_str());
    }

    const auto w = g2::writeMdxm(r.mesh);
    {
        std::error_code ec;
        const auto dir = std::filesystem::path(outPath).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    g2::writeFileChecked(outPath, w.data.data(), w.data.size());
    std::printf("\nGeschrieben: %s (%zu Bytes)\n", outPath.c_str(), w.data.size());

    if (writeSkinFile) {
        const std::size_t dot = outPath.find_last_of('.');
        const std::string skinPath =
            (dot == std::string::npos ? outPath : outPath.substr(0, dot)) + ".skin";
        const std::string skin = g2::writeSkin(r.mesh);
        g2::writeFileChecked(skinPath, skin);
        {
            std::size_t lines = 0;
            for (char c : skin)
                if (c == '\n') ++lines;
            std::printf("Geschrieben: %s (%zu Eintraege)\n", skinPath.c_str(), lines);
        }
    }

    if (w.stats.weightsDropped)
        std::printf("  %llu Gewichte verworfen (mehr als vier je Vertex)\n",
                    static_cast<unsigned long long>(w.stats.weightsDropped));
    std::printf("  max. Bone-Referenzen je Surface: %zu von %d\n", w.stats.maxBoneRefsUsed,
                g2::fmt::kMaxBoneRefsPerSurface);
    return 0;
}

// g2c validate <datei.car> [-enums anims.h] [-basedir <pfad>] [-frames] [-all]
int cmdValidate(int argc, char** argv, int firstOpt) {
    std::vector<std::string> cars;
    std::string enumPath;
    g2::car::ValidateOptions vo;
    bool showAll = false;

    for (int i = firstOpt; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-enums" && i + 1 < argc) enumPath = argv[++i];
        else if (a == "-basedir" && i + 1 < argc) vo.baseDir = argv[++i];
        else if (a == "-frames") vo.readFrameCounts = true;
        else if (a == "-all") showAll = true;
        else if (a == "-cache" && i + 1 < argc) vo.cacheDir = argv[++i];
        else if (a == "-threads" && i + 1 < argc)
            vo.threads = static_cast<unsigned>(argInt(argv[++i], "-threads"));
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "Unbekannte Option: %s\n", a.c_str());
            return 1;
        } else cars.push_back(a);
    }
    if (cars.empty()) { std::fprintf(stderr, "Keine .car angegeben.\n"); return 1; }

    g2::anim::EnumTable table;
    if (!enumPath.empty()) {
        table = g2::anim::parseEnumHeaderFile(enumPath);
        std::printf("Enumtabelle: %s (%zu Eintraege)\n\n", enumPath.c_str(), table.size());
        vo.enums = &table;
    }

    std::size_t totalErrors = 0;
    for (const std::string& car : cars) {
        const g2::car::Script script = g2::car::parseFile(car);
        const auto r = g2::car::validate(script, car, vo);
        totalErrors += r.errors;

        std::printf("%s\n", car.c_str());
        std::printf("  %zu Grabs, %zu Sequenzen  ->  %zu Fehler, %zu Warnungen, %zu Hinweise\n",
                    r.grabs, r.sequences, r.errors, r.warnings, r.infos);

        std::size_t shown = 0;
        for (const auto& is : r.issues) {
            if (!showAll && shown >= 15) {
                std::printf("    ... (%zu weitere, mit -all sichtbar)\n", r.issues.size() - shown);
                break;
            }
            std::printf("    %-8s", is.levelName());
            if (!is.sequence.empty()) std::printf(" %-28s", is.sequence.c_str());
            std::printf(" %s", is.message.c_str());
            if (is.line) std::printf("  (Zeile %zu)", is.line);
            std::printf("\n");
            ++shown;
        }
        std::printf("\n");
    }
    return totalErrors ? 2 : 0;
}

// g2c scan <ordner> [-enums anims.h] [-validate]
int cmdScan(int argc, char** argv) {
    const std::string root = argv[2];
    std::string enumPath;
    bool doValidate = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-enums" && i + 1 < argc) enumPath = argv[++i];
        else if (a == "-validate") doValidate = true;
    }

    const auto found = g2::car::scanDirectory(root);
    std::printf("%zu .car-Dateien unter %s\n\n", found.size(), root.c_str());
    std::printf("  %-8s %-34s %s\n", "Grabs", "Modell (-makeskel)", "Pfad");
    for (const auto& f : found)
        std::printf("  %-8zu %-34s %s\n", f.grabs,
                    f.modelName.empty() ? "-" : f.modelName.c_str(), f.path.c_str());

    if (!doValidate || found.empty()) return 0;

    std::printf("\n");
    std::vector<char*> argv2;
    std::string dash = "-enums";
    std::vector<std::string> keep;
    for (const auto& f : found) keep.push_back(f.path);
    if (!enumPath.empty()) { keep.push_back(dash); keep.push_back(enumPath); }
    for (auto& k : keep) argv2.push_back(k.data());
    return cmdValidate(static_cast<int>(argv2.size()), argv2.data(), 0);
}

// g2c diff <a.gla> <b.gla> [-cfg animation.cfg] [-offset N] [-all]
int cmdDiff(int argc, char** argv) {
    const g2::MdxaFile A = g2::readMdxa(readFile(argv[2]));
    const g2::MdxaFile B = g2::readMdxa(readFile(argv[3]));

    g2::DiffOptions opt;
    std::vector<g2::car::Sequence> seqs;
    bool showAll = false;

    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-cfg" && i + 1 < argc) seqs = g2::readAnimationCfg(g2::readWholeFile(argv[++i]));
        else if (a == "-offset" && i + 1 < argc) opt.frameOffsetB = static_cast<int>(argInt(argv[++i], "-offset"));
        else if (a == "-all") showAll = true;
        else if (a == "-tol" && i + 1 < argc) {
            const double m = argFloat(argv[++i], "-tol");
            opt.toleranceRotationDeg *= m;
            opt.toleranceTranslation *= m;
        }
        else if (a == "-threads" && i + 1 < argc) opt.threads = static_cast<unsigned>(argInt(argv[++i], "-threads"));
        else { std::fprintf(stderr, "Unbekannte Option: %s\n", a.c_str()); return 1; }
    }

    std::printf("A: %s  (%d Frames, %zu Bones)\n", argv[2], A.numFrames, A.skeleton.bones.size());
    std::printf("B: %s  (%d Frames, %zu Bones)\n\n", argv[3], B.numFrames, B.skeleton.bones.size());

    const g2::DiffResult r = g2::diffMdxa(A, B, seqs, opt);

    if (r.skeletonIdentical) {
        std::printf("Skelett: identisch\n\n");
    } else {
        std::printf("Skelett: %zu Abweichung(en)\n", r.skeletonNotes.size());
        for (std::size_t i = 0; i < r.skeletonNotes.size() && i < 12; ++i)
            std::printf("  - %s\n", r.skeletonNotes[i].c_str());
        if (r.skeletonNotes.size() > 12) std::printf("  ... (%zu weitere)\n", r.skeletonNotes.size() - 12);
        std::printf("\n");
    }

    std::printf("Verglichen: %d Frames x %d Bones = %llu Instanzen\n", r.frames, r.bones,
                static_cast<unsigned long long>(r.instances));
    std::printf("  ausserhalb der Toleranz : %llu  (%.3f%%)\n",
                static_cast<unsigned long long>(r.outliers), r.outlierPercent());
    std::printf("  max. Rotation : %10.5f Grad   (Toleranz %.3f)\n", r.maxRotationDeg,
                opt.toleranceRotationDeg);
    std::printf("  max. Translation: %10.5f Einh.  (Toleranz %.5f)\n", r.maxTranslation,
                opt.toleranceTranslation);
    std::printf("  Mittel          : %10.5f Grad / %.5f Einheiten\n", r.meanRotationDeg,
                r.meanTranslation);

    if (r.clean()) {
        std::printf("\nKeine Abweichung ueber der Quantisierungsgrenze.\n");
        return 0;
    }

    std::printf("\nBones mit Abweichung:\n");
    std::printf("  %-18s %9s %12s %11s %10s\n", "Bone", "Anteil", "max Rot/Grad", "max Trans",
                "bei Frame");
    std::size_t shown = 0;
    for (const auto& bd : r.perBone) {
        if (!bd.outliers) continue;
        if (!showAll && shown >= 12) { std::printf("  ... (weitere mit -all)\n"); break; }
        std::printf("  %-18s %8.2f%% %12.5f %11.5f %10d\n", bd.name.c_str(), bd.outlierPercent(),
                    bd.maxRotationDeg, bd.maxTranslation, bd.worstFrame);
        ++shown;
    }

    if (!r.perSequence.empty()) {
        std::printf("\nSequenzen mit Abweichung: %zu\n", r.perSequence.size());
        std::printf("  %-30s %7s %8s %10s %9s\n", "Sequenz", "Frames", "Treffer", "max Trans",
                    "max Grad");
        shown = 0;
        for (const auto& sd : r.perSequence) {
            if (!showAll && shown >= 12) { std::printf("  ... (weitere mit -all)\n"); break; }
            std::printf("  %-30s %7d %8llu %10.5f %9.5f\n", sd.name.c_str(), sd.frameCount,
                        static_cast<unsigned long long>(sd.outliers), sd.maxDeviation,
                        sd.maxRotationDeg);
            ++shown;
        }
    } else if (seqs.empty()) {
        std::printf("\nMit -cfg animation.cfg wird die Abweichung auch Sequenzen zugeordnet.\n");
    }
    return r.clean() ? 0 : 2;
}

// Dateityp am Inhalt erkennen, nicht an der Endung. Die Endung sagt bei
// Modding-Assets erfahrungsgemaess wenig.
enum class FileKind { Gla, Glm, Xsi, Car, Unknown };

FileKind sniff(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return FileKind::Unknown;
    char head[8] = {};
    f.read(head, sizeof(head));
    const std::streamsize got = f.gcount();
    if (got >= 4) {
        if (std::memcmp(head, "2LGA", 4) == 0) return FileKind::Gla;
        if (std::memcmp(head, "2LGM", 4) == 0) return FileKind::Glm;
        if (std::memcmp(head, "xsi ", 4) == 0) return FileKind::Xsi;
    }
    // Textdatei: als .car behandeln, wenn ein $-Befehl auftaucht.
    for (int i = 0; i < got; ++i)
        if (head[i] == '$') return FileKind::Car;

    std::ifstream t(path);
    std::string line;
    int scanned = 0;
    while (std::getline(t, line) && scanned++ < 40) {
        const std::size_t d = line.find_first_not_of(" \t");
        if (d != std::string::npos && line[d] == '$') return FileKind::Car;
    }
    return FileKind::Unknown;
}

// Nach der Analyse einer gezogenen .car: Assetwurzel und Referenz-GLA selbst
// suchen und, wenn beides da ist, den Bau anbieten.
int offerBuild(const std::string& carPath) {
    namespace fs = std::filesystem;

    const g2::car::Script script = g2::car::parseFile(carPath);
    const std::string baseDir = g2::car::guessBaseDir(script, carPath);
    const std::string refPath = g2::car::guessReferenceGla(script, baseDir);

    std::printf("\n------------------------------------------------------------\n");
    if (baseDir.empty()) {
        std::printf("Assetwurzel : nicht gefunden\n");
        std::printf("\nDie Pfade aus $aseanimgrab gehen von keinem uebergeordneten\n"
                    "Verzeichnis aus auf. Bau von Hand:\n"
                    "  g2c build \"%s\" -ref <referenz.gla> -basedir <pfad>\n",
                    carPath.c_str());
        return 0;
    }
    std::printf("Assetwurzel : %s\n", baseDir.c_str());

    if (refPath.empty()) {
        std::printf("Referenz-GLA: nicht gefunden\n");
        std::printf("\nErwartet wurde sie laut -makeskel unter:\n  %s\\%s.gla\n"
                    "\nBau mit ausdruecklicher Referenz:\n"
                    "  g2c build \"%s\" -ref <referenz.gla> -basedir \"%s\"\n",
                    baseDir.c_str(),
                    script.convert ? script.convert->makeSkel.c_str() : "?", carPath.c_str(),
                    baseDir.c_str());
        return 0;
    }
    std::printf("Referenz-GLA: %s\n", refPath.c_str());

    // Ausgabe in einen eigenen Ordner neben der .car. Die Referenz wird
    // niemals ueberschrieben — waere sie kaputt, waere alles kaputt.
    const fs::path outDir = fs::path(carPath).parent_path() / "g2c_out";
    std::string stem = "out";
    if (script.convert && !script.convert->makeSkel.empty()) {
        const std::string& ms = script.convert->makeSkel;
        const std::size_t sl = ms.find_last_of("/\\");
        stem = (sl == std::string::npos) ? ms : ms.substr(sl + 1);
    }
    const fs::path outGla = outDir / (stem + ".gla");
    std::printf("Ausgabe     : %s\n", outGla.string().c_str());
    std::printf("------------------------------------------------------------\n");
    std::printf("\n%zu Animationsdateien werden gelesen. Das dauert etwas.\n",
                script.grabs.size());
    std::printf("Jetzt bauen? [j/N] ");
    std::fflush(stdout);

    int answer = std::getchar();
    while (answer != '\n' && answer != EOF) {
        const int extra = std::getchar();
        if (extra == '\n' || extra == EOF) break;
    }
    if (answer != 'j' && answer != 'J' && answer != 'y' && answer != 'Y') {
        std::printf("Abgebrochen.\n");
        return 0;
    }
    std::printf("\n");

    try {
        const g2::MdxaFile ref = g2::readMdxa(readFile(refPath));
        g2::car::BuildOptions bo;
        bo.baseDir = baseDir;
        std::printf("Threads     : %u\n\n", g2::defaultThreadCount());
        std::size_t lastPct = 999;
        bo.progress = [&](std::size_t i, std::size_t n, const std::string&) {
            const std::size_t pct = n ? (i * 100 / n) : 100;
            if (pct != lastPct) {
                lastPct = pct;
                std::printf("\r  %3zu%%  (%zu/%zu)", pct, i, n);
                std::fflush(stdout);
            }
        };

        const g2::car::BuildResult br = g2::car::build(script, ref.skeleton, carPath, bo);
        std::printf("\r                              \r");
        for (const auto& w : br.warnings) std::printf("  ! %s\n", w.c_str());

        const auto res = g2::writeMdxa(ref.skeleton, br.frames);

        std::error_code ec;
        fs::create_directories(outDir, ec);
        g2::writeFileChecked(outGla.string(), res.data.data(), res.data.size());

        const fs::path outCfg = outDir / "animation.cfg";
        std::ostringstream head;
        head << br.totalFrames() << " frames; " << br.sequences.size()
             << " sequences; erzeugt von g2c";
        const std::string cfg = g2::car::writeAnimationCfg(br.sequences, head.str());
        g2::writeFileChecked(outCfg.string(), cfg);

        std::printf("\nFrames    : %d\n", br.totalFrames());
        std::printf("Sequenzen : %zu\n", br.sequences.size());
        std::printf("Pool      : %zu Eintraege (%.1f%% dedupliziert)\n", res.poolEntries,
                    res.dedupeRatio() * 100.0);
        std::printf("Kompression: %s\n\n", res.stats.summary().c_str());
        std::printf("Geschrieben: %s (%zu Bytes)\n", outGla.string().c_str(), res.data.size());
        std::printf("Geschrieben: %s\n", outCfg.string().c_str());

        if (!br.frameBlocks.empty()) {
            std::vector<g2::FrameEntry> fe;
            for (const auto& b : br.frameBlocks) {
                g2::FrameEntry e;
                e.sourcePath = std::filesystem::absolute(b.sourcePath).generic_string();
                e.startFrame = b.startFrame; e.duration = b.duration; e.fps = b.fps;
                for (int k = 0; k < 3; ++k) e.averageVec[k] = b.averageVec[k];
                fe.push_back(std::move(e));
            }
            const auto outFrames = outDir / (stem + ".frames");
            const std::string txt = g2::writeFrames(fe);
            g2::writeFileChecked(outFrames.string(), txt);
            std::printf("Geschrieben: %s (%zu Bloecke)\n", outFrames.string().c_str(), fe.size());
        }

        // Wie Carcass: GLA und GLM in einem Lauf.
        writeMeshFromScript(script, ref, baseDir, carPath, outDir.string());

        std::printf("\nDie Referenz-GLA wurde nicht angeruehrt. Zum Testen die Dateien\n"
                    "zusammen kopieren — GLA und animation.cfg gehoeren immer als Paar\n"
                    "zusammen, die GLM zum Modell.\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nFehler: %s\n", e.what());
        return 1;
    }
    return 0;
}

// Aufruf per Drag and Drop: eine oder mehrere Dateien auf g2c.exe gezogen.
int cmdDropped(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        if (i > 1) std::printf("\n");
        std::printf("============================================================\n");

        try {
            switch (sniff(path)) {
                case FileKind::Gla:
                    std::printf("Erkannt: GLA (Skelett und Animationen)\n");
                    std::printf("============================================================\n");
                    rc |= cmdInfo(path);
                    break;
                case FileKind::Xsi:
                    std::printf("Erkannt: dotXSI\n");
                    std::printf("============================================================\n");
                    rc |= cmdXsi(path);
                    break;
                case FileKind::Car:
                    std::printf("Erkannt: Carcass-Skript\n");
                    std::printf("============================================================\n");
                    rc |= cmdCar(path, false);
                    rc |= offerBuild(path);
                    break;
                case FileKind::Glm:
                    std::printf("Erkannt: GLM (Mesh)\n");
                    std::printf("============================================================\n");
                    std::printf("Fuer GLM gibt es noch keinen Reader.\n");
                    break;
                case FileKind::Unknown:
                    std::printf("Unbekanntes Format: %s\n", path.c_str());
                    std::printf("============================================================\n");
                    std::printf("Erkannt werden GLA (2LGA), GLM (2LGM), dotXSI und .car.\n");
                    rc |= 1;
                    break;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Fehler bei \"%s\": %s\n", path.c_str(), e.what());
            rc |= 1;
        }
    }

    // Beim Ziehen auf die exe gehoert die Konsole uns allein und wuerde
    // sofort verschwinden. Also warten.
    std::printf("\nEnter zum Schliessen...");
    std::fflush(stdout);
    (void)std::getchar();
    return rc;
}

void usage() {
    std::printf(
        "g2c \xE2\x80\x94 Ghoul2-Werkzeug\n"
        "\n"
        "BAUEN\n"
        "  g2c build <datei.car> -ref <referenz.gla> [-basedir <pfad>]\n"
        "        Ganzes Carcass-Skript abarbeiten, GLA und animation.cfg schreiben\n"
        "        -o <aus.gla>      Ausgabename (Vorgabe aus -makeskel)\n"
        "        -basedir <pfad>   Wurzel, unter der \"models/\" liegt\n"
        "        -threads N        Vorgabe: alle Kerne\n"
        "        -cache <ordner>   Vorgabe: g2c_cache neben der .car\n"
        "        -nocache          ohne Zwischenspeicher\n"
        "        -clearcache       Cache vorher leeren\n"
        "        -skipmissing      fehlende .xsi ueberspringen\n"
        "        -carcass          wie Carcass quantisieren (kleiner, ungenauer)\n"
        "        -origin x y z     ueberschreibt das Skript\n"
        "        -framespeed N     Rueckfall ohne -framespeed und ohne SI_Scene\n"
        "        -nomesh           nur GLA, keine GLM\n"
        "\n"
        "  g2c anim  <referenz.gla> <aus.gla> <anim1.xsi> [anim2.xsi ...]\n"
        "        Einzelne Animationen gegen ein vorhandenes Skelett bauen\n"
        "        -origin x y z     wie in der .car\n"
        "        -alias gla=xsi    Bone-Umbenennung, z.B. face=face_always_\n"
        "\n"
        "  g2c mesh  <root.xsi> -ref <referenz.gla>\n"
        "        GLM aus dotXSI bauen\n"
        "        -o <aus.glm>      Ausgabename\n"
        "        -compare <ref.glm> gegen eine vorhandene GLM pruefen\n"
        "        -normaltol F      Normaltoleranz (Vorgabe 0.05)\n"
        "        -uvtol F          UV-Toleranz (Vorgabe 0.002)\n"
        "        -makeskin         .skin danebenlegen\n"
        "        -alias gla=xsi    Bone-Umbenennung\n"
        "\n"
        "PRUEFEN\n"
        "  g2c validate <datei.car> [weitere.car ...]\n"
        "        Skript pruefen wie Assimilate, plus die Gegenrichtung\n"
        "        -enums <anims.h>  Enumtabelle fuer die Namenspruefung\n"
        "        -basedir <pfad>   Wurzel fuer die Dateipruefung\n"
        "        -frames           Framezahlen aus den .xsi lesen (langsamer)\n"
        "        -all              alle Meldungen statt der ersten fuenfzehn\n"
        "  g2c scan  <ordner> [-enums <anims.h>] [-validate]\n"
        "        Verzeichnisbaum nach .car durchsuchen\n"
        "  g2c info  <datei.gla>   Header, Skelett und Poolstatistik\n"
        "  g2c check <datei.gla>   Strukturpruefung und Fehlermessung\n"
        "  g2c diff  <a.gla> <b.gla>\n"
        "        Zwei GLA vollstaendig vergleichen, Bone fuer Bone\n"
        "        -cfg <animation.cfg> Abweichungen Sequenzen zuordnen\n"
        "        -offset N         Frameversatz der zweiten Datei\n"
        "        -tol M            Toleranzen skalieren\n"
        "        -all              alle Treffer statt der ersten zwoelf\n"
        "\n"
        "ANSEHEN\n"
        "  g2c xsi   <datei.xsi>   dotXSI parsen, Templatetypen und Tempo\n"
        "  g2c car   <datei.car>   Carcass-Skript aufloesen (-v fuer alle Zeilen)\n"
        "\n"
        "Dateien koennen auch direkt auf g2c.exe gezogen werden; das Format\n"
        "wird am Inhalt erkannt. Bei einer .car werden Assetwurzel und\n"
        "Referenz-GLA selbst gesucht und der Bau angeboten.\n");
}

}  // namespace

// Einstiegspunkt der Kommandozeile.
//
// Heisst nicht "main", damit die Oberflaeche ihn aufrufen kann: bei der
// Windows-Fassung stecken beide in EINER Exe, und dort ist wWinMain der
// Einstieg. Unter Linux und fuer den reinen Kommandozeilenbau steht das
// main() weiter unten.
// Animationen aus einer GLA zurueck nach dotXSI.
// Aus einem Ordner voller .xsi ein .car-Skript erzeugen.
//
// Fuer Modelle, zu denen es nie eines gab: viele fremde Modelle liegen nur
// als Sammlung von .xsi vor. Fast alles laesst sich daraus ableiten — der
// Sequenzname steht im Dateinamen, die Bildrate in SI_Scene.
//
// NICHT ableitbar und deshalb auf Vorgabewerten:
//   -loop         welcher Frame die Schleife beginnt, steht nirgends
//   -additional   Unterbereiche einer Datei sind eine reine Autorenangabe
// Beides wird ausdruecklich gemeldet, damit niemand annimmt, es sei
// rekonstruiert worden.
int cmdMakeCar(int argc, char** argv) {
    namespace fs = std::filesystem;
    const std::string folder = argv[2];
    std::string outPath, baseDir, enumsPath, rootFile, makeSkel;
    std::optional<std::array<double, 3>> origin;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else if (a == "-basedir" && i + 1 < argc) baseDir = argv[++i];
        else if (a == "-enums" && i + 1 < argc) enumsPath = argv[++i];
        else if (a == "-root" && i + 1 < argc) rootFile = argv[++i];
        else if (a == "-makeskel" && i + 1 < argc) makeSkel = argv[++i];
        else if (a == "-origin" && i + 3 < argc) {
            origin = std::array<double, 3>{std::stod(argv[i + 1]), std::stod(argv[i + 2]),
                                           std::stod(argv[i + 3])};
            i += 3;
        }
    }

    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        std::fprintf(stderr, "%s ist kein Ordner\n", folder.c_str());
        return 1;
    }

    // Assetwurzel: der Ordner oberhalb von "models", sonst der Ordner selbst.
    if (baseDir.empty()) {
        fs::path cur = fs::absolute(folder, ec);
        while (!cur.empty() && cur.has_parent_path()) {
            if (cur.filename() == "models") { baseDir = cur.parent_path().string(); break; }
            cur = cur.parent_path();
        }
        if (baseDir.empty()) baseDir = fs::absolute(folder, ec).string();
    }

    // Alle .xsi einsammeln, sortiert — sonst haengt die Reihenfolge der
    // Sequenzen davon ab, wie das Dateisystem sie liefert.
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec),
         end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".xsi") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "Keine .xsi in %s\n", folder.c_str());
        return 1;
    }
    std::printf("Gefunden     : %zu .xsi\n", files.size());

    const auto rel = [&](const fs::path& p) {
        std::error_code e2;
        const fs::path r = fs::relative(fs::absolute(p, e2), baseDir, e2);
        std::string out = (e2 || r.empty()) ? p.filename().string() : r.generic_string();
        return out;
    };

    // Die Datei mit dem Modell finden: "root.xsi", sonst die einzige, deren
    // Name nach keiner Animation aussieht.
    fs::path rootPath;
    if (!rootFile.empty()) {
        rootPath = rootFile;
    } else {
        for (const auto& f : files) {
            std::string stem = f.stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (stem == "root" || stem == "_humanoid") { rootPath = f; break; }
        }
    }
    if (rootPath.empty()) {
        std::fprintf(stderr,
                     "Keine root.xsi gefunden. Ohne Modelldatei laesst sich kein\n"
                     "$aseanimconvertmdx schreiben — mit -root <datei> angeben.\n");
        return 1;
    }
    std::printf("Modelldatei  : %s\n", rel(rootPath).c_str());

    g2::anim::EnumTable enums;
    if (!enumsPath.empty()) {
        try {
            enums = g2::anim::parseEnumHeaderFile(enumsPath);
            std::printf("Enumtabelle  : %zu Eintraege\n", enums.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "anims.h nicht lesbar: %s\n", e.what());
        }
    }

    g2::car::Script sc;
    std::size_t unknown = 0;
    for (const auto& f : files) {
        if (fs::equivalent(f, rootPath, ec)) continue;
        g2::car::GrabDirective g;
        g.file = rel(f);
        // Kein -framespeed: ohne ihn gilt die Rate aus SI_Scene, und die hat
        // der Autor gesetzt. Ein Vorgabewert waere hier eine Erfindung.
        sc.grabs.push_back(std::move(g));
        if (!enums.empty() && !enums.contains(sc.grabs.back().derivedName())) ++unknown;
    }

    // ROOT ans Ende: root.xsi liefert die Basispose, und in Ravens Skripten
    // steht der Grab zuletzt.
    for (std::size_t i = 0; i + 1 < sc.grabs.size(); ++i) {
        std::string stem = fs::path(sc.grabs[i].file).stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem == "root") {
            auto tmp = sc.grabs[i];
            sc.grabs.erase(sc.grabs.begin() + static_cast<long>(i));
            sc.grabs.push_back(std::move(tmp));
            break;
        }
    }

    // Rahmen anlegen, sonst schreibt writeScript die Grabs nicht.
    g2::car::addGrabFrame(sc);

    g2::car::ConvertDirective cv;
    cv.noAsk = true;
    cv.root = rel(rootPath);
    // Endung weg: die Anweisung nennt den Namen ohne .xsi.
    if (cv.root.size() > 4) cv.root = cv.root.substr(0, cv.root.size() - 4);
    cv.makeSkel = makeSkel.empty()
                      ? (fs::path(cv.root).parent_path() / "_humanoid").generic_string()
                      : makeSkel;
    cv.origin = origin;
    sc.convert = cv;

    const std::string text = g2::car::writeScript(sc);
    const std::string out = outPath.empty() ? (fs::path(folder) / "_humanoid.car").string() : outPath;
    if (fs::exists(out, ec)) {
        std::fprintf(stderr, "%s gibt es schon — nicht ueberschrieben.\n", out.c_str());
        return 1;
    }
    g2::writeFileChecked(out, text);

    std::printf("Geschrieben  : %s (%zu Grabs)\n", out.c_str(), sc.grabs.size());
    std::printf("Skelett      : %s\n", cv.makeSkel.c_str());
    if (!enums.empty())
        std::printf("Enums        : %zu von %zu Sequenznamen stehen NICHT in der anims.h\n",
                    unknown, sc.grabs.size());
    std::printf("Nicht ableitbar und deshalb offen:\n"
                "  -loop        welcher Frame die Schleife beginnt, steht in keiner .xsi\n"
                "  -additional  Unterbereiche einer Datei sind eine reine Autorenangabe\n"
                "  -origin      %s\n",
                origin ? "angegeben" : "nicht angegeben (ueblich: -origin 0 0 24)");
    return 0;
}

int cmdExport(int argc, char** argv) {
    const std::string glaPath = argv[2];
    std::string cfgPath, outDir = "xsi_out", only, framesPath, makeCarPath;
    float scale = 0.0f;
    std::optional<std::array<float, 3>> origin;
    bool noOrigin = false;
    std::string carPath, xsiPrefix;
    bool noDetectMotion = false;
    bool keepMotion = false;
    std::string makeSkel;
    g2::xsiexp::ExportOptions::BasePose basePose = g2::xsiexp::ExportOptions::BasePose::World;
    bool writeScale = true;
    g2::xsiexp::ExportOptions::Version xsiVersion = g2::xsiexp::ExportOptions::Version::V30;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-noorigin") noOrigin = true;
        else if (a == "-cfg" && i + 1 < argc) cfgPath = argv[++i];
        else if (a == "-o" && i + 1 < argc) outDir = argv[++i];
        else if (a == "-only" && i + 1 < argc) only = argv[++i];
        else if (a == "-frames" && i + 1 < argc) framesPath = argv[++i];
        else if (a == "-car" && i + 1 < argc) carPath = argv[++i];
        else if (a == "-prefix" && i + 1 < argc) xsiPrefix = argv[++i];
        else if (a == "-noscale") writeScale = false;
        else if (a == "-xsi" && i + 1 < argc) {
            const std::string v = argv[++i];
            xsiVersion = (v == "3.5" || v == "35" || v == "0350")
                             ? g2::xsiexp::ExportOptions::Version::V35
                             : g2::xsiexp::ExportOptions::Version::V30;
        }
        else if (a == "-basepose" && i + 1 < argc) {
            // Nur zum Ausprobieren gegen Carcass: welche Bindepose gehoert
            // in den BASEPOSE-Block? Siehe ExportOptions::BasePose.
            const std::string v = argv[++i];
            if (v == "local") basePose = g2::xsiexp::ExportOptions::BasePose::Local;
            else if (v == "none") basePose = g2::xsiexp::ExportOptions::BasePose::None;
            else basePose = g2::xsiexp::ExportOptions::BasePose::World;
        }
        else if (a == "-makecar" && i + 1 < argc) makeCarPath = argv[++i];
        else if (a == "-scale" && i + 1 < argc) scale = std::stof(argv[++i]);
        else if (a == "-origin" && i + 3 < argc) {
            origin = std::array<float, 3>{std::stof(argv[i + 1]), std::stof(argv[i + 2]),
                                          std::stof(argv[i + 3])};
            i += 3;
        }
    }

    const auto data = readFile(glaPath);
    const g2::MdxaFile gla = g2::readMdxa(data);
    std::printf("GLA          : %d Frames, %zu Bones\n", gla.numFrames, gla.skeleton.bones.size());

    g2::xsiexp::ExportOptions opt;
    opt.basePose = basePose;
    opt.version = xsiVersion;
    opt.scale = scale > 0.0f ? scale : (gla.skeleton.scale > 0.0f ? gla.skeleton.scale : 0.64f);
    opt.origin = origin;

    // Ohne Angabe schaetzen — aber laut sagen, was herauskam.
    //
    // Der Wert MUSS wieder eingesetzt werden, sonst zieht das Neubauen ihn
    // ein zweites Mal ab und das Modell steht um genau diesen Betrag daneben.
    // Das faellt erst im Spiel auf, und dann sucht man an der falschen Stelle.
    if (!origin && !noOrigin) {
        opt.origin = g2::xsiexp::detectOrigin(gla);
        if (opt.origin)
            std::printf("Origin       : %.2f %.2f %.2f (geschaetzt, mit -origin ueberschreibbar)\n",
                        (*opt.origin)[0], (*opt.origin)[1], (*opt.origin)[2]);
    } else if (origin) {
        std::printf("Origin       : %.2f %.2f %.2f (angegeben)\n", (*origin)[0], (*origin)[1],
                    (*origin)[2]);
    }
    if (framesPath.empty() && !noDetectMotion)
        std::printf("Wurzelbewegung: keine .frames angegeben, wird aus der GLA rekonstruiert.\n");

    struct CfgEntry {
        std::string name;
        int start = 0, count = 0, loop = -1, fps = 20;
    };
    std::vector<CfgEntry> cfg;

    std::vector<g2::xsiexp::Sequence> seqs;
    if (!cfgPath.empty()) {
        std::ifstream f(cfgPath);
        if (!f) { std::fprintf(stderr, "Kann %s nicht lesen\n", cfgPath.c_str()); return 1; }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '/') continue;
            std::istringstream is(line);
            CfgEntry e;
            if (!(is >> e.name >> e.start >> e.count >> e.loop >> e.fps)) continue;
            if (e.count <= 0 || e.start < 0 || e.start + e.count > gla.numFrames) continue;
            cfg.push_back(e);
            seqs.push_back({e.name, e.start, e.count});
        }
        std::printf("animation.cfg: %zu Sequenzen\n", seqs.size());
    } else {
        seqs.push_back({"all", 0, gla.numFrames});
        std::printf("Keine -cfg angegeben: alles als eine Datei\n");
    }

    // Ordnername fuer die Pfade in der .car: der letzte Teil des
    // Ausgabeordners. Damit passt das Skript zu dem, was tatsaechlich
    // geschrieben wurde.
    const std::string modelDir = std::filesystem::path(outDir).filename().string();

    // --- Unterbereiche zusammenfassen -------------------------------------
    //
    // Viele Sequenzen sind Ausschnitte einer laengeren. Exportierte man jede
    // als eigene Datei, haette die neue GLA mehr Frames als die alte und die
    // animation.cfg passte nicht mehr. Deshalb wird nur der jeweils
    // groesste, sich nicht ueberschneidende Bereich als Datei geschrieben;
    // was darin liegt, wird im Skript zu -additional.
    g2::xsiexp::Grouping grouping;
    if (!carPath.empty() && !cfg.empty()) {
        std::vector<g2::xsiexp::CfgSequence> in;
        for (const auto& e : cfg) in.push_back({e.name, e.start, e.count, e.loop, e.fps});
        grouping = g2::xsiexp::groupSequences(in);

        // Nur die Master exportieren.
        seqs.clear();
        for (const auto& m : grouping.masters)
            seqs.push_back({m.self.name, m.self.start, m.self.count});
        std::printf("Zusammengefasst: %zu Master, %zu Unterbereiche als -additional\n",
                    grouping.masters.size(),
                    cfg.size() - grouping.masters.size() - grouping.partial);
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::size_t written = 0, failed = 0, withMotion = 0, noMotion = 0, residual = 0;
    for (const auto& s : seqs) {
        if (!only.empty() && s.name.find(only) == std::string::npos) continue;
        try {
            g2::xsiexp::ExportOptions one = opt;
            // Erst die .frames, wenn es eine gibt — sie ist die Quelle aus
            // erster Hand. Fehlt sie, laesst sich die Bewegung aus der GLA
            // selbst rekonstruieren: Carcass rechnet sie als lineare Rampe
            // auf den Wurzelbone, und die steht dort weiterhin.
            if (!framesPath.empty()) {
                one.rootMotionPerFrame = g2::xsiexp::readAverageVec(framesPath, s.name);
            }
            if (!one.rootMotionPerFrame && !noDetectMotion)
                one.rootMotionPerFrame = g2::xsiexp::detectRootMotion(gla, s);
            if (one.rootMotionPerFrame) ++withMotion;
            else ++noMotion;
            if (g2::xsiexp::residualMotion(gla, s) > 0.05f) ++residual;
            const std::string text = g2::xsiexp::exportSequence(gla, s, one);
            std::string low = s.name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const std::filesystem::path out = std::filesystem::path(outDir) / (low + ".xsi");
            std::ofstream o(out, std::ios::binary);
            o.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!o) throw std::runtime_error("Schreiben fehlgeschlagen");
            ++written;
        } catch (const std::exception& e) {
            if (failed < 5) std::fprintf(stderr, "  %s: %s\n", s.name.c_str(), e.what());
            ++failed;
        }
    }
    // --- Passendes .car erzeugen ------------------------------------------
    //
    // Beim Export ist mehr bekannt als bei einem nackten Ordner: die
    // animation.cfg nennt Loopframe und Framespeed, und mehrere Eintraege im
    // selben Framebereich sind genau das, was in der .car als -additional
    // steht. Damit laesst sich ein Skript schreiben, das die GLA wieder
    // aufbaut — nicht nur ungefaehr.
    if (!makeCarPath.empty() && !cfg.empty()) {
        // Masterbloecke bestimmen.
        //
        // Ein Block ist ein zusammenhaengender Framebereich, der aus EINER
        // .xsi stammt. Am sichersten steht das in der .frames; ohne sie wird
        // es aus der cfg abgeleitet: ein Eintrag ist Master, wenn kein
        // anderer ihn vollstaendig enthaelt.
        std::vector<CfgEntry> sorted = cfg;
        std::sort(sorted.begin(), sorted.end(), [](const CfgEntry& a, const CfgEntry& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.count > b.count;   // laengster zuerst: der ist der Master
        });

        struct Block {
            CfgEntry              master;
            std::vector<CfgEntry> extra;
        };
        std::vector<Block> blocks;
        for (const auto& e : sorted) {
            Block* host = nullptr;
            for (auto& b : blocks) {
                const int bEnd = b.master.start + b.master.count;
                if (e.start >= b.master.start && e.start + e.count <= bEnd) { host = &b; break; }
            }
            if (host) host->extra.push_back(e);
            else blocks.push_back({e, {}});
        }
        std::printf("Bloecke      : %zu Master, %zu als -additional\n", blocks.size(),
                    cfg.size() - blocks.size());

        g2::car::Script sc;
        g2::car::addGrabFrame(sc);
        std::string rootRel;

        for (const auto& b : blocks) {
            std::string low = b.master.name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string relPath = "models/players/" + modelDir + "/" + low + ".xsi";
            if (b.master.name == "ROOT") rootRel = "models/players/" + modelDir + "/" + low;

            g2::car::GrabDirective g;
            g.file = relPath;
            g.enumName = b.master.name;
            g.loop = b.master.loop;
            g.frameSpeed = b.master.fps;
            for (const auto& e : b.extra) {
                g2::car::GrabDirective::Additional a;
                a.targetOffset = e.start - b.master.start;
                a.frameCount = e.count;
                a.loopFrame = e.loop;
                a.frameSpeed = e.fps;
                a.name = e.name;
                g.additional.push_back(std::move(a));
            }
            sc.grabs.push_back(std::move(g));
        }

        g2::car::ConvertDirective cv;
        cv.noAsk = true;
        cv.root = rootRel.empty() ? ("models/players/" + modelDir + "/root") : rootRel;
        cv.makeSkel = "models/players/" + modelDir + "/_humanoid";
        cv.origin = opt.origin
                        ? std::optional<std::array<double, 3>>{{(*opt.origin)[0], (*opt.origin)[1],
                                                                (*opt.origin)[2]}}
                        : std::nullopt;
        sc.convert = cv;

        g2::writeFileChecked(makeCarPath, g2::car::writeScript(sc));
        std::printf("Skript       : %s (%zu Grabs)\n", makeCarPath.c_str(), sc.grabs.size());
        if (rootRel.empty())
            std::printf("Hinweis      : keine ROOT-Sequenz in der cfg — $aseanimconvertmdx zeigt\n"
                        "               auf root, die Datei muss vorhanden sein.\n");
    }

    // --- .car erzeugen -----------------------------------------------------
    //
    // Aus GLA und animation.cfg laesst sich ein Skript schreiben, das die
    // GLA wieder genauso erzeugt. Loopframe und Rate stehen in der cfg,
    // muessen also nicht geraten werden — das ist der Unterschied zu
    // "makecar", das nur einen Ordner voller .xsi sieht.
    //
    // Der Kniff sind die Unterbereiche: viele Sequenzen sind Ausschnitte
    // einer laengeren. Exportierte man jede als eigene Datei, haette die
    // neue GLA mehr Frames als die alte und die animation.cfg passte nicht
    // mehr. Deshalb werden nur die groessten, sich nicht ueberschneidenden
    // Bereiche als Datei geschrieben; alles, was darin liegt, wird zu
    // -additional.
    if (!carPath.empty() && !only.empty())
        std::printf("Achtung      : -only zusammen mit -car. Das Skript nennt ALLE Sequenzen,\n"
                    "               exportiert wurden aber nur die gefilterten. So laesst es\n"
                    "               sich nicht bauen.\n");
    if (!carPath.empty() && !grouping.masters.empty()) {
        // Scale aus der GLA mitschreiben; $keepmotion nur, wenn ausdruecklich
        // verlangt — aus der GLA laesst sich nicht ablesen, ob die Bewegung
        // seinerzeit entfernt wurde oder nie da war.
        const auto sc = g2::xsiexp::buildScript(grouping, xsiPrefix, opt.origin,
                                                writeScale ? gla.skeleton.scale : 0.0f, keepMotion,
                                                makeSkel.empty() ? gla.skeleton.name : makeSkel);
        g2::writeFileChecked(carPath, g2::car::writeScript(sc));
        std::size_t withAdd = 0;
        for (const auto& m2 : grouping.masters)
            if (!m2.inside.empty()) ++withAdd;
        std::printf("Skript       : %s (%zu Grabs, %zu mit -additional)\n", carPath.c_str(),
                    grouping.masters.size(), withAdd);
        if (grouping.partial)
            std::printf("Achtung      : %zu Sequenzen ueberlappen nur teilweise und fehlen im "
                        "Skript.\n", grouping.partial);
    }

    std::printf("Geschrieben  : %zu .xsi nach %s\n", written, outDir.c_str());
    if (!framesPath.empty()) {
        // "0 von 3" klingt nach Fehlschlag, ist aber meist keiner: von
        // Ravens 1289 Sequenzen haben nur 178 ueberhaupt eine Bewegung. Alle
        // uebrigen laufen an Ort und Stelle, das Spiel bewegt die Figur.
        std::printf("Wurzelbewegung: %zu wiederhergestellt, %zu ohne Bewegung in der .frames\n",
                    withMotion, noMotion);
    }
    if (residual)
        std::printf("Hinweis      : %zu Sequenz(en) sind Ausschnitte laengerer Animationen und\n"
                    "               tragen einen Rest Wurzelbewegung. Beim Neubauen wandert der\n"
                    "               in die .frames — die Animation bleibt heil, aber die neue\n"
                    "               GLA ist dort nicht bitgleich zur Quelle.\n",
                    residual);
    if (failed) std::printf("Fehlgeschlagen: %zu\n", failed);
    return failed ? 2 : 0;
}

int g2cMain(int argc, char** argv) {
    // Ausgabe ausdruecklich auf das klassische Locale festnageln. Sonst
    // koennte eine Umgebung mit deutschen Regionseinstellungen Zahlen mit
    // Dezimalkomma ausgeben, und die waeren nicht mehr einlesbar.
    std::locale::global(std::locale::classic());

    if (argc < 2) {
        usage();
        return 1;
    }
    if (argc < 3) {
        // Einzelnes Argument: ein Unterbefehl ohne Datei, eine gezogene
        // Datei, oder Unfug.
        if (std::string(argv[1]) == "about") return cmdAbout();
        std::ifstream probe(argv[1], std::ios::binary);
        if (probe) return cmdDropped(argc, argv);
        usage();
        return 1;
    }
    const std::string cmd = argv[1];

    // Drag and Drop: erstes Argument ist kein Unterbefehl, sondern eine Datei.
    {
        static const char* kCommands[] = {"info", "check", "xsi", "car", "anim", "build", "diff", "mesh", "validate", "scan", "export", "makecar", "about"};
        bool isCommand = false;
        for (const char* c : kCommands)
            if (cmd == c) isCommand = true;
        if (!isCommand) {
            std::ifstream probe(cmd, std::ios::binary);
            if (probe) return cmdDropped(argc, argv);
        }
    }

    try {
        if (cmd == "info") return cmdInfo(argv[2]);
        if (cmd == "check") return cmdCheck(argv[2]);
        if (cmd == "xsi") return cmdXsi(argv[2]);
        if (cmd == "car") return cmdCar(argv[2], argc > 3);
        if (cmd == "validate") {
            if (argc < 3) { usage(); return 1; }
            return cmdValidate(argc, argv, 2);
        }
        if (cmd == "about") return cmdAbout();
        if (cmd == "makecar") {
            if (argc < 3) { usage(); return 1; }
            return cmdMakeCar(argc, argv);
        }
        if (cmd == "export") {
            if (argc < 3) { usage(); return 1; }
            return cmdExport(argc, argv);
        }
        if (cmd == "scan") {
            if (argc < 3) { usage(); return 1; }
            return cmdScan(argc, argv);
        }
        if (cmd == "mesh") {
            if (argc < 4) { usage(); return 1; }
            return cmdMesh(argc, argv);
        }
        if (cmd == "diff") {
            if (argc < 4) { usage(); return 1; }
            return cmdDiff(argc, argv);
        }
        if (cmd == "build") {
            if (argc < 4) { usage(); return 1; }
            return cmdBuild(argc, argv);
        }
        if (cmd == "anim") {
            if (argc < 5) { usage(); return 1; }
            return cmdAnim(argc, argv);
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fehler: %s\n", e.what());
        return 1;
    }
}

#ifndef G2C_NO_MAIN
int main(int argc, char** argv) { return g2cMain(argc, argv); }
#endif
