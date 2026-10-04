#include "g2/carbuild.h"

#include "g2/parallel.h"

#include <algorithm>
#include <cmath>
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
        std::string leaf = dir.filename().string();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (leaf == "models") {   // "Models" is the same folder on Windows
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

std::vector<BuildResult::MissingFile> findMissingFiles(const Script& script,
                                                       const std::string& carPath,
                                                       const BuildOptions& opt,
                                                       std::vector<std::string>* resolvedOut) {
    const std::string carDir = fs::path(carPath).parent_path().string();
    const std::string baseDir = opt.baseDir.empty() ? script.baseDir : opt.baseDir;
    std::vector<BuildResult::MissingFile> out;
    if (resolvedOut) resolvedOut->assign(script.grabs.size(), std::string());
    for (std::size_t i = 0; i < script.grabs.size(); ++i) {
        const auto& g = script.grabs[i];
        // Carcass appends ".XSI" when the name has no ".xsi" in it.
        std::vector<std::string> names{g.file};
        {
            std::string low = g.file;
            for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low.find(".xsi") == std::string::npos) {
                names.push_back(g.file + ".XSI");
                names.push_back(g.file + ".xsi");
            }
        }
        std::string r;
        for (const auto& name : names) {
            // $basedir before the line: <basedir>/<file> first - absolute, or
            // relative to the .car's folder or the asset root.
            if (!g.baseDir.empty()) {
                const fs::path bd(g.baseDir);
                for (const fs::path& cand :
                     {bd / name, fs::path(carDir) / bd / name, fs::path(baseDir) / bd / name}) {
                    std::error_code ec;
                    if (!cand.empty() && fs::is_regular_file(cand, ec)) {
                        r = cand.lexically_normal().make_preferred().string();
                        break;
                    }
                }
            }
            if (r.empty()) r = resolveAssetPath(name, baseDir, carDir);
            if (!r.empty()) break;
        }
        if (resolvedOut) (*resolvedOut)[i] = r;
        if (!r.empty()) continue;
        BuildResult::MissingFile m;
        m.file = script.grabs[i].file;
        m.sequence = script.grabs[i].enumName ? *script.grabs[i].enumName
                                              : script.grabs[i].derivedName();
        m.grabIndex = i;
        m.line = script.grabs[i].line;
        out.push_back(std::move(m));
    }
    return out;
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
    res.missing = findMissingFiles(script, carPath, opt, &resolved);
    for (const auto& m : res.missing) res.missingFiles.push_back(m.file);
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
        std::vector<std::array<float, 3>> motionPath;
    };
    std::vector<Loaded> loaded(nGrabs);

    // -deltavecs per grab line (.frames only).
    std::vector<bool> wantsDeltas(nGrabs, false);
    bool anyDeltas = false;
    for (std::size_t i = 0; i < nGrabs; ++i)
        for (const auto& x : script.grabs[i].extraArgs) {
            std::string lx = x;
            for (auto& ch : lx) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lx == "-deltavecs") wantsDeltas[i] = anyDeltas = true;
        }
    // Carcass's delta0 is measured from the Motion bone in the very first
    // frame of the GLA, shifted by -origin as it stores it: (y, -x, -z).
    std::array<float, 3> deltaBase{0.0f, 0.0f, 0.0f};

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
                    xsi::EvalOptions ev1 = eval;
                    // The first file's path too: delta0 refers to it.
                    ev1.wantMotionPath = wantsDeltas[i] || (i == 0 && anyDeltas);
                    xsi::EvalResult ev = xsi::evaluate(reference, anim, ev1);
                    out.motionPath = std::move(ev.motionPath);
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

        // -framestep: thin out before concatenating.
        const int step = (opt.frameStep > 1 && L.frameCount > opt.frameStep) ? opt.frameStep : 1;
        const int fullCount = L.frameCount;
        if (step > 1) {
            std::vector<Mat3x4> kept;
            const int keptCount = (fullCount + step - 1) / step;
            kept.reserve(static_cast<std::size_t>(keptCount) * static_cast<std::size_t>(numBones));
            for (int f = 0; f < fullCount; f += step)
                kept.insert(kept.end(),
                            L.frames.matrices.begin() + static_cast<std::ptrdiff_t>(f) * numBones,
                            L.frames.matrices.begin() + static_cast<std::ptrdiff_t>(f + 1) * numBones);
            L.frames.matrices = std::move(kept);
            L.frameCount = keptCount;
        }
        // Scales a frame number / speed of this file by the step.
        const auto byStep = [step](int v) { return step > 1 ? v / step : v; };
        // Whole speeds stay whole (as in Carcass); the sign is kept - a
        // backwards sequence used to come out at speed 1.
        const auto speedByStep = [step](double v) {
            if (step <= 1) return v;
            const double m = v == std::floor(v) ? std::max(1.0, std::round(std::fabs(v) / step))
                                                : std::fabs(v) / step;
            return v < 0 ? -m : m;
        };

        res.frames.matrices.insert(res.frames.matrices.end(), L.frames.matrices.begin(),
                                   L.frames.matrices.end());
        L.frames.matrices.clear();
        L.frames.matrices.shrink_to_fit();

        double speed = opt.defaultFrameSpeed;
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
        if (s.loopFrame > 0) s.loopFrame = byStep(s.loopFrame);
        s.frameSpeed = speedByStep(speed);
        s.sourceFile = g.file;
        res.sequences.push_back(std::move(s));

        {
            FrameBlock fb;
            fb.sourcePath = L.resolvedPath;
            fb.startFrame = cursor;
            fb.duration = L.frameCount;
            fb.fps = L.frameRate > 0 ? static_cast<int>(speedByStep(L.frameRate)) : L.frameRate;
            // Per frame and with the sign flipped: the ramp on the root bone is
            // the counter-motion, averagevec is the motion itself. With
            // -framestep one kept frame covers `step` source frames.
            if (fullCount > 1)
                for (int k = 0; k < 3; ++k)
                    fb.averageVec[k] = -L.rootMotion[k] * static_cast<float>(step) /
                                       static_cast<float>(fullCount - 1);
            // -deltavecs: the step of the Motion bone per kept frame; delta0
            // is its position in the first frame (Carcass, measured on
            // BOTH_RUN1: delta0 = (0, -0.385, -28.419), then zeros).
            if (i == 0 && !L.motionPath.empty()) {
                deltaBase = L.motionPath[0];
                if (eval.origin) {
                    deltaBase[0] -= (*eval.origin)[1];
                    deltaBase[1] += (*eval.origin)[0];
                    deltaBase[2] += (*eval.origin)[2];
                }
            }
            if (wantsDeltas[i] && !L.motionPath.empty()) {
                std::array<float, 3> prev = deltaBase;
                for (int f = 0; f < fullCount; f += step) {
                    const auto& p = L.motionPath[static_cast<std::size_t>(f)];
                    fb.deltaVecs.push_back({p[0] - prev[0], p[1] - prev[1], p[2] - prev[2]});
                    prev = p;
                }
            }
            res.frameBlocks.push_back(std::move(fb));
        }

        for (const auto& a : g.additional) {
            // A part outside its file reaches into the neighbouring sequence.
            // Only the validation used to check this, and only with frame
            // counts switched on; the build wrote it without a word.
            if (a.targetOffset < 0 || a.frameCount <= 0 ||
                static_cast<long long>(a.targetOffset) + a.frameCount > fullCount)
                res.warnings.push_back(g.file + ": -additional " + a.name + " (Start " +
                                       std::to_string(a.targetOffset) + ", " +
                                       std::to_string(a.frameCount) + " Frames) liegt ausserhalb der " +
                                       std::to_string(fullCount) + " Frames der Datei");
            Sequence x;
            x.name = a.name;
            x.targetFrame = cursor + byStep(a.targetOffset);
            x.frameCount = a.frameCount;
            if (step > 1) {
                const int end = (a.targetOffset + a.frameCount + step - 1) / step;
                x.frameCount = std::max(1, end - byStep(a.targetOffset));
            }
            x.loopFrame = a.loopFrame > 0 ? byStep(a.loopFrame) : a.loopFrame;
            x.frameSpeed = speedByStep(a.frameSpeed);
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
