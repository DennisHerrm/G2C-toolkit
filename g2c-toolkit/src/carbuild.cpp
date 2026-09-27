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
        // Some directory trees already have the "models/" in the base.
        candidates.push_back(fs::path(carDir).parent_path() / rel);
    }

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec))
            // make_preferred, because the paths from the .car use forward
            // slashes and the base uses backslashes on Windows. Both work,
            // but mixed together they look like an error.
            return c.lexically_normal().make_preferred().string();
    }
    return {};
}

AutoPaths guessPaths(const Script& script, const std::string& carPath) {
    AutoPaths out;
    std::error_code ec;

    // Walk upwards from the .car until a "models" folder shows up.
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
    // Otherwise: is there a "models" folder next to the .car?
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
    eval.warnMissingBones = false;   // otherwise the same message 1289 times

    // $scale from the script takes precedence over the reference GLA's value.
    //
    // Normally both are the same; if the script states a different one, that
    // is the author's intent and not carelessness.
    eval.scale = reference.scale > 0.0f ? reference.scale : 1.0f;
    if (script.scale && *script.scale > 0.0) eval.scale = static_cast<float>(*script.scale);

    // $keepmotion keeps the "Motion" bone in the skeleton. The root motion
    // is still factored out.
    //
    // That is how Raven's Carcass behaves (output: "Keeping motion bone",
    // followed by "Compensating for motion bone"), and that is how Raven's
    // shipped _humanoid.gla is built: its _humanoid.car contains $keepmotion,
    // and yet every sequence with net motion carries the ramp on model_root.
    // Measured on 1400 sequences against the GLA from assets1.pk3: with the
    // ramp the root matches in 1396, without it in only 1144 - for
    // BOTH_RUNSTRAFE_LEFT1/RIGHT1 the difference was 96 units.
    //
    // An earlier version disabled the ramp with $keepmotion. The character
    // then walked away from its center during the animation and snapped back
    // on the next loop.
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

    // --- Resolve paths (serial, cheap) ------------------------------------
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
            // Exactly where we looked - that is the detail that lets you find
            // the error without guessing.
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

    // --- Load and evaluate (parallel) -------------------------------------
    //
    // Each file is independent: read, parse, collect FCurves, compute frames.
    // Reading alone accounts for about four fifths of the run time, and that
    // is exactly what scales best across multiple threads.
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

    // The cache is not thread-safe, but it is used from multiple threads.
    // Instead of locking it - which would defeat the purpose of the
    // parallelization - each thread gets its own instance on the same
    // folder. The entries are individual files and are replaced atomically
    // via rename, so that works out.
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

    // Treat files that exist but cannot be read exactly like missing ones.
    //
    // They used to be noted as a warning and skipped. The result looked like
    // a successful build, but from the gap onwards every following sequence
    // sat at a different target frame than in the game's animation.cfg -
    // exactly what is supposed to be ruled out without -skipmissing.
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

    // --- Concatenate (serial, order matters) ------------------------------
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
        // Carry comments from the script over into the animation.cfg.
        //
        // Only for the main grab, not for the -additional sub-ranges:
        // they are on the same line and would have repeated the same
        // comment.
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
            // Per frame and with the sign flipped: the ramp on the root bone is
            // the counter-motion, averagevec is the motion itself.
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
        // Deliberately "in at least one file": the set is a union over all
        // files. A bone that is only missing from partial animations - say
        // ltail in a torso-only animation - ends up here as well, even though
        // it is animated properly elsewhere. The wording "in no animation
        // file" that used to be here was simply wrong and pointed suspicion
        // at the wrong place.
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
