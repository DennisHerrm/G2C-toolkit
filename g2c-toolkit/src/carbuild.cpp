#include "g2/carbuild.h"

#include "g2/parallel.h"

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace g2::car {
namespace fs = std::filesystem;

std::string resolveAssetPath(const std::string& relative, const std::string& baseDir,
                             const std::string& carDir) {
    const fs::path rel(relative);
    if (rel.is_absolute()) return fs::exists(rel) ? rel.string() : std::string();

    std::vector<fs::path> candidates;
    if (!baseDir.empty()) candidates.push_back(fs::path(baseDir) / rel);
    candidates.push_back(rel);
    if (!carDir.empty()) {
        candidates.push_back(fs::path(carDir) / rel);
        // Manche Verzeichnisbaeume haben das "models/" schon in der Basis.
        candidates.push_back(fs::path(carDir).parent_path() / rel);
    }

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec))
            // make_preferred, weil die Pfade aus der .car Schraegstriche
            // benutzen und die Basis unter Windows Backslashes. Beides
            // funktioniert, sieht gemischt aber nach Fehler aus.
            return c.lexically_normal().make_preferred().string();
    }
    return {};
}

AutoPaths guessPaths(const Script& script, const std::string& carPath) {
    AutoPaths out;
    std::error_code ec;

    // Von der .car aus nach oben laufen, bis ein Ordner "models" auftaucht.
    fs::path dir = fs::path(carPath).parent_path();
    for (int depth = 0; depth < 16 && !dir.empty(); ++depth) {
        if (dir.filename() == "models") {
            out.baseDir = dir.parent_path().string();
            break;
        }
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    // Sonst: liegt neben der .car ein "models"-Ordner?
    if (out.baseDir.empty()) {
        const fs::path here = fs::path(carPath).parent_path();
        if (fs::is_directory(here / "models", ec)) out.baseDir = here.string();
    }
    if (out.baseDir.empty()) return out;

    if (!script.baseDir.empty()) {
        const fs::path withScript = fs::path(out.baseDir) / script.baseDir;
        if (fs::is_directory(withScript, ec)) out.baseDir = withScript.string();
    }

    if (script.convert && !script.convert->makeSkel.empty()) {
        const fs::path gla = fs::path(out.baseDir) / (script.convert->makeSkel + ".gla");
        if (fs::is_regular_file(gla, ec)) out.referenceGla = gla.string();
        else out.note = "Referenz-GLA nicht gefunden unter " + gla.string();
    } else {
        out.note = "Das Skript nennt kein -makeskel, Referenz-GLA unbekannt";
    }
    return out;
}

std::string guessBaseDir(const Script& script, const std::string& carPath, int maxLevels) {
    if (script.grabs.empty()) return {};
    const fs::path probe(script.grabs.front().file);

    fs::path dir = fs::path(carPath).parent_path();
    for (int i = 0; i < maxLevels && !dir.empty(); ++i) {
        std::error_code ec;
        if (fs::exists(dir / probe, ec)) return dir.lexically_normal().make_preferred().string();
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return {};
}

std::string guessReferenceGla(const Script& script, const std::string& baseDir) {
    if (!script.convert || script.convert->makeSkel.empty() || baseDir.empty()) return {};
    const fs::path p = fs::path(baseDir) / (script.convert->makeSkel + ".gla");
    std::error_code ec;
    return fs::exists(p, ec) ? p.lexically_normal().make_preferred().string() : std::string();
}

BuildResult build(const Script& script, const Skeleton& reference, const std::string& carPath,
                  const BuildOptions& opt) {
    if (script.grabs.empty())
        throw std::runtime_error("Das Skript enthaelt keine $aseanimgrab-Anweisungen");

    const int numBones = static_cast<int>(reference.bones.size());
    if (numBones == 0) throw std::runtime_error("Referenzskelett hat keine Bones");

    const std::string carDir = fs::path(carPath).parent_path().string();
    const std::string baseDir = opt.baseDir.empty() ? script.baseDir : opt.baseDir;

    xsi::EvalOptions eval;
    eval.warnMissingBones = false;   // sonst 1289 mal dieselbe Meldung

    // $scale aus dem Skript hat Vorrang vor dem Wert der Referenz-GLA.
    //
    // Normalerweise sind beide gleich; steht im Skript ein anderer, ist das
    // eine Absicht des Autors und keine Nachlaessigkeit.
    eval.scale = reference.scale > 0.0f ? reference.scale : 1.0f;
    if (script.scale && *script.scale > 0.0) eval.scale = static_cast<float>(*script.scale);

    // $keepmotion behaelt den "Motion"-Bone im Skelett. Die Wurzelbewegung
    // wird trotzdem herausgerechnet.
    //
    // So verhaelt sich Ravens Carcass (Ausgabe: "Keeping motion bone", danach
    // "Compensating for motion bone"), und so ist Ravens ausgelieferte
    // _humanoid.gla gebaut: deren _humanoid.car enthaelt $keepmotion, und
    // trotzdem traegt jede Sequenz mit Nettobewegung die Rampe auf
    // model_root. Nachgemessen an 1400 Sequenzen gegen die GLA aus
    // assets1.pk3: mit Rampe stimmt die Wurzel bei 1396 ueberein, ohne bei
    // nur 1144 — bei BOTH_RUNSTRAFE_LEFT1/RIGHT1 lagen 96 Einheiten dazwischen.
    //
    // Eine fruehere Fassung schaltete die Rampe bei $keepmotion ab. Die Figur
    // lief dann waehrend der Animation aus ihrer Mitte heraus und sprang beim
    // naechsten Durchlauf zurueck.
    eval.extractRootMotion = true;

    if (opt.originOverride) {
        eval.origin = *opt.originOverride;
    } else if (script.convert && script.convert->origin) {
        const auto& o = *script.convert->origin;
        eval.origin = std::array<float, 3>{static_cast<float>(o[0]), static_cast<float>(o[1]),
                                           static_cast<float>(o[2])};
    }

    BuildResult res;
    res.frames.numBones = numBones;
    res.carcassCompatible = opt.carcassCompatible;

    // --- Pfade aufloesen (seriell, billig) --------------------------------
    const std::size_t nGrabs = script.grabs.size();
    std::vector<std::string> resolved(nGrabs);
    for (std::size_t i = 0; i < nGrabs; ++i) {
        resolved[i] = resolveAssetPath(script.grabs[i].file, baseDir, carDir);
        if (!resolved[i].empty()) continue;

        BuildResult::MissingFile m;
        m.file = script.grabs[i].file;
        m.sequence = script.grabs[i].enumName ? *script.grabs[i].enumName
                                              : script.grabs[i].derivedName();
        m.grabIndex = i;
        m.line = script.grabs[i].line;
        res.missing.push_back(std::move(m));
        res.missingFiles.push_back(script.grabs[i].file);
    }
    if (!res.missing.empty() && !opt.skipMissing) {
        std::ostringstream os;
        os << res.missing.size() << " von " << nGrabs << " Animationsdateien nicht gefunden.\n";
        if (!baseDir.empty()) os << "Gesucht unter: " << baseDir << "\n";
        os << "Es fehlen:";
        for (std::size_t i = 0; i < res.missing.size() && i < 8; ++i) {
            const auto& m = res.missing[i];
            os << "\n  " << m.file;
            os << "\n      Sequenz " << m.sequence;
            if (m.line) os << ", .car Zeile " << m.line;
            // Wo genau gesucht wurde — das ist die Angabe, mit der man den
            // Fehler ohne Raten findet.
            if (!baseDir.empty()) {
                std::error_code ec;
                os << "\n      erwartet: "
                   << (std::filesystem::path(baseDir) / m.file).lexically_normal().string();
            }
        }
        if (res.missing.size() > 8) os << "\n  ... und " << (res.missing.size() - 8) << " weitere";
        os << "\n\nMit -basedir <pfad> die Wurzel angeben, unter der \"models/\" liegt.\n"
              "Mit -skipmissing trotzdem bauen — Achtung, dann verschieben sich alle\n"
              "nachfolgenden Zielframes und die animation.cfg passt nicht mehr zur GLA.";
        throw std::runtime_error(os.str());
    }

    // --- Laden und auswerten (parallel) -----------------------------------
    //
    // Jede Datei ist unabhaengig: lesen, parsen, FCurves einsammeln, Frames
    // ausrechnen. Das Lesen allein macht rund vier Fuenftel der Laufzeit aus,
    // und genau das skaliert ueber mehrere Threads am besten.
    struct Loaded {
        AnimationFrames frames;
        int             frameCount = 0;
        int             frameRate = 0;
        float           rootMotion[3] = {0.0f, 0.0f, 0.0f};
        std::string     resolvedPath;
        bool            ok = false;
        std::string     warning;
        std::string     rangeNote;
        std::vector<std::string> missingBones;
    };
    std::vector<Loaded> loaded(nGrabs);

    std::mutex             progressMutex;
    std::atomic<std::size_t> done{0};

    // Der Cache ist nicht threadsicher, wird aber aus mehreren Threads
    // benutzt. Statt ihn zu sperren — was den Zweck der Parallelisierung
    // untergraebe — bekommt jeder Thread eine eigene Instanz auf denselben
    // Ordner. Die Eintraege sind einzelne Dateien und werden ueber
    // Umbenennen atomar ersetzt, das vertraegt sich.
    std::mutex cacheStatsMutex;

    parallelFor(
        nGrabs,
        [&](std::size_t i) {
            const auto& g = script.grabs[i];
            Loaded& out = loaded[i];

            if (!resolved[i].empty()) {
                try {
                    AnimCache cache(opt.cacheDir);
                    const xsi::AnimFile anim = cache.enabled()
                                                   ? cache.loadOrParse(resolved[i])
                                                   : xsi::loadAnimationFile(resolved[i]);
                    if (cache.enabled()) {
                        std::lock_guard<std::mutex> lock(cacheStatsMutex);
                        res.cache.hits += cache.stats().hits;
                        res.cache.misses += cache.stats().misses;
                        res.cache.bytesRead += cache.stats().bytesRead;
                        res.cache.bytesWritten += cache.stats().bytesWritten;
                    }
                    xsi::EvalResult ev = xsi::evaluate(reference, anim, eval);
                    out.frameCount = ev.frameCount;
                    out.frameRate = (anim.hasScene && anim.frameRate > 0.0f)
                                        ? static_cast<int>(anim.frameRate)
                                        : 0;
                    out.missingBones = std::move(ev.missingBones);
                    if (anim.sceneRangeDiffers())
                        out.rangeNote = g.file + ": SI_Scene nennt Frames " +
                                        std::to_string(anim.sceneFirst) + ".." +
                                        std::to_string(anim.sceneLast) + ", die Keys reichen von " +
                                        std::to_string(anim.firstFrame) + " bis " +
                                        std::to_string(anim.lastFrame) +
                                        " - gebaut mit den Keys. Carcass lehnt so eine Datei "
                                        "ab; neu exportieren.";
                    for (int k = 0; k < 3; ++k) out.rootMotion[k] = ev.rootMotion[k];
                    out.resolvedPath = resolved[i];
                    out.frames = std::move(ev.frames);
                    out.ok = true;
                } catch (const std::exception& e) {
                    out.warning = std::string("Lesefehler in \"") + g.file + "\": " + e.what();
                }
            } else {
                out.warning = "uebersprungen (Datei fehlt): " + g.file;
            }

            if (opt.progress) {
                const std::size_t n = done.fetch_add(1) + 1;
                std::lock_guard<std::mutex> lock(progressMutex);
                opt.progress(n, nGrabs, g.file);
            }
        },
        opt.threads);

    // Vorhandene, aber unlesbare Dateien genauso behandeln wie fehlende.
    //
    // Frueher wurden sie als Warnung vermerkt und uebersprungen. Das Ergebnis
    // sah wie ein gelungener Bau aus, aber ab der Luecke stand jede
    // nachfolgende Sequenz an einem anderen Zielframe als in der
    // animation.cfg des Spiels — genau das, was ohne -skipmissing
    // ausgeschlossen sein soll.
    if (!opt.skipMissing) {
        std::vector<std::string> unreadable;
        for (std::size_t i = 0; i < nGrabs; ++i)
            if (!resolved[i].empty() && !loaded[i].ok) unreadable.push_back(loaded[i].warning);
        if (!unreadable.empty()) {
            std::ostringstream os;
            os << unreadable.size() << " von " << nGrabs
               << " Animationsdateien konnten nicht gelesen werden:";
            for (std::size_t i = 0; i < unreadable.size() && i < 8; ++i)
                os << "\n  " << unreadable[i];
            if (unreadable.size() > 8) os << "\n  ... und " << (unreadable.size() - 8) << " weitere";
            os << "\n\nMit -skipmissing trotzdem bauen — Achtung, dann verschieben sich alle\n"
                  "nachfolgenden Zielframes und die animation.cfg passt nicht mehr zur GLA.";
            throw std::runtime_error(os.str());
        }
    }

    // --- Zusammenhaengen (seriell, Reihenfolge zaehlt) --------------------
    std::set<std::string> reportedMissingBones;
    int cursor = 0;

    for (std::size_t i = 0; i < nGrabs; ++i) {
        const auto& g = script.grabs[i];
        Loaded& L = loaded[i];

        if (!L.warning.empty()) res.warnings.push_back(L.warning);
        if (!L.ok) continue;
        if (!L.rangeNote.empty()) res.warnings.push_back(L.rangeNote);

        for (const auto& b : L.missingBones) reportedMissingBones.insert(b);
        res.frames.matrices.insert(res.frames.matrices.end(), L.frames.matrices.begin(),
                                   L.frames.matrices.end());
        L.frames.matrices.clear();
        L.frames.matrices.shrink_to_fit();

        int speed = opt.defaultFrameSpeed;
        if (g.frameSpeed) speed = *g.frameSpeed;
        else if (L.frameRate > 0) speed = L.frameRate;

        Sequence s;
        // Kommentare aus dem Skript in die animation.cfg uebernehmen.
        //
        // Nur beim Hauptgrab, nicht bei den -additional-Unterbereichen:
        // die stehen in derselben Zeile und haetten denselben Kommentar
        // wiederholt.
        s.commentsBefore = g.commentsBefore;
        s.trailingComment = g.trailingComment;
        s.name = g.enumName ? *g.enumName : g.derivedName();
        s.targetFrame = cursor;
        s.frameCount = L.frameCount;
        s.loopFrame = g.loop.value_or(0);
        s.frameSpeed = speed;
        s.sourceFile = g.file;
        res.sequences.push_back(std::move(s));

        {
            FrameBlock fb;
            fb.sourcePath = L.resolvedPath;
            fb.startFrame = cursor;
            fb.duration = L.frameCount;
            fb.fps = L.frameRate;
            // Pro Frame und mit umgekehrtem Vorzeichen: die Rampe auf dem
            // Wurzelbone ist die Gegenbewegung, averagevec die Bewegung selbst.
            if (L.frameCount > 1)
                for (int k = 0; k < 3; ++k)
                    fb.averageVec[k] = -L.rootMotion[k] / static_cast<float>(L.frameCount - 1);
            res.frameBlocks.push_back(std::move(fb));
        }

        for (const auto& a : g.additional) {
            Sequence x;
            x.name = a.name;
            x.targetFrame = cursor + a.targetOffset;
            x.frameCount = a.frameCount;
            x.loopFrame = a.loopFrame;
            x.frameSpeed = a.frameSpeed;
            x.sourceFile = g.file;
            x.fromAdditional = true;
            x.insideQdSkip = a.insideQdSkip;
            res.sequences.push_back(std::move(x));
        }
        cursor += L.frameCount;
    }

    if (!reportedMissingBones.empty()) {
        std::ostringstream os;
        // Bewusst "in mindestens einer Datei": die Menge ist eine Vereinigung
        // ueber alle Dateien. Ein Bone, der nur in Teilanimationen fehlt —
        // etwa ltail in einer reinen Torso-Animation — landet hier ebenfalls,
        // obwohl er anderswo sauber animiert wird. Die frueher hier stehende
        // Formulierung "in keiner Animationsdatei" war schlicht falsch und
        // hat den Verdacht auf die falsche Stelle gelenkt.
        os << reportedMissingBones.size()
           << " Bone(s) in mindestens einer Datei nicht animiert, dort in Ruhepose: ";
        std::size_t i = 0;
        for (const auto& b : reportedMissingBones) {
            if (i++) os << ", ";
            if (i > 6) { os << "..."; break; }
            os << b;
        }
        res.warnings.push_back(os.str());
    }

    if (res.frames.matrices.empty())
        throw std::runtime_error("Keine einzige Animationsdatei konnte gelesen werden");

    return res;
}

}  // namespace g2::car
