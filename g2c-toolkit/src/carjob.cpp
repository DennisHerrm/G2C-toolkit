#include "g2/carjob.h"

#include "g2/mdxa.h"
#include "g2/mdxm.h"
#include "g2/readfile.h"
#include "g2/sidefiles.h"
#include "g2/ase.h"
#include "g2/animcache.h"
#include "g2/parallel.h"
#include "g2/skelbuild.h"
#include "g2/xsi.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace g2::car {
namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// "models/players/_humanoid/_humanoid.gla" -> "models/players/_humanoid/_humanoid"
std::string withoutGla(std::string s) {
    s = slashes(s);
    if (s.size() > 4) {
        std::string ext = s.substr(s.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".gla") s.resize(s.size() - 4);
    }
    return s;
}

const Statement* firstOf(const Script& s, Cmd c) {
    for (const auto& st : s.statements)
        if (st.cmd == c && !st.args.empty()) return &st;
    return nullptr;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string stemOf(const std::string& path) {
    return fs::path(slashes(path)).stem().string();
}

// One .bak per file, replaced on every run: what you want back is the state
// before the last build.
void keepBackup(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    fs::path bak = p;
    bak += ".bak";
    fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec);
}

}  // namespace

ScriptKind scriptKind(const Script& script) {
    if (firstOf(script, Cmd::AseConvert) || firstOf(script, Cmd::AseAnimConvert)) return ScriptKind::Mdr;
    if (!script.grabs.empty()) return ScriptKind::Animation;
    if (firstOf(script, Cmd::AseAnimGrabGla)) return ScriptKind::Model;
    return ScriptKind::Empty;
}

std::string defaultGlaPath(const Script& script, const std::string& baseDir) {
    if (!script.convert || script.convert->makeSkel.empty()) return {};
    const fs::path rel = slashes(script.convert->makeSkel) + ".gla";
    return (baseDir.empty() ? rel : fs::path(baseDir) / rel).lexically_normal().make_preferred().string();
}

std::string scriptReferenceGla(const Script& script, const std::string& baseDir,
                               const std::string& carPath) {
    const std::string carDir = fs::path(carPath).parent_path().string();
    for (const Cmd c : {Cmd::AseAnimRefGla, Cmd::AseAnimGrabGla}) {
        if (const Statement* st = firstOf(script, c)) {
            std::string rel = slashes(st->args[0]);
            if (withoutGla(rel) == rel) rel += ".gla";
            return resolveAssetPath(rel, baseDir, carDir);
        }
    }
    const std::string p = defaultGlaPath(script, baseDir);
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec) ? p : std::string();
}

JobPlan planJob(const Script& script, const std::string& carPathIn, const JobOptions& opt) {
    JobPlan pl;
    std::error_code absEc;
    pl.carPath = fs::absolute(carPathIn, absEc).lexically_normal().string();
    pl.kind = scriptKind(script);
    const fs::path carDir = fs::path(pl.carPath).parent_path();

    pl.baseDir = opt.build.baseDir;
    if (pl.baseDir.empty()) pl.baseDir = guessBaseDir(script, pl.carPath);
    if (pl.baseDir.empty()) pl.baseDir = guessPaths(script, pl.carPath).baseDir;

    pl.referenceGla = opt.referenceGla.empty()
                          ? scriptReferenceGla(script, pl.baseDir, pl.carPath)
                          : opt.referenceGla;

    // The GLA name a GLM points to and a new GLA carries in its header.
    //   Model script: the GLA it uses, as written in $aseanimgrab_gla.
    //   Animation:    -makeskel - the GLA being built, NOT the reference. With
    //                 the reference's name a custom humanoid's GLM would bind
    //                 to the standard skeleton (and a GLA identify as it).
    //   Neither:      the reference's own name (filled in by runJob).
    if (pl.kind == ScriptKind::Model) {
        pl.glaName = withoutGla(firstOf(script, Cmd::AseAnimGrabGla)->args[0]);
    } else if (script.convert && !script.convert->makeSkel.empty()) {
        pl.glaName = slashes(script.convert->makeSkel);
    }
    // -framestep: the thinned-out build gets its own names ("_skip"), as in
    // Carcass - there only from a step of 3, a step of 2 overwrote the normal
    // files. Model scripts are not affected (they build no frames).
    pl.skip = pl.kind == ScriptKind::Animation && opt.build.frameStep > 1;
    const std::string sfx = pl.skip ? "_skip" : "";
    if (pl.skip && !pl.glaName.empty()) pl.glaName += sfx;

    // Where the GLA-side files go.
    const std::string glaStem =
        pl.glaName.empty() ? stemOf(pl.referenceGla.empty() ? "out" : pl.referenceGla) + sfx
                           : stemOf(pl.glaName);
    fs::path glaPath;
    if (!opt.glaPath.empty()) glaPath = opt.glaPath;
    else if (!opt.outputDir.empty()) glaPath = fs::path(opt.outputDir) / (glaStem + ".gla");
    else if (const std::string d = defaultGlaPath(script, pl.baseDir); !d.empty())
        glaPath = fs::path(d).replace_extension().string() + sfx + ".gla";
    else glaPath = carDir / (stemOf(pl.carPath) + sfx + ".gla");
    pl.glaPath = glaPath.lexically_normal().make_preferred().string();

    // Carcass names the GLM after the .car ("model.car" -> model.glm) and
    // puts it next to it.
    if (script.convert && !script.convert->root.empty()) {
        if (!script.convert->noAsk) {
            // Carcass: "$aseanimconvertmdx" reads <root>.ASK (3ds Max ASE).
            pl.meshIsAse = true;
            pl.meshSource = ase::findAseFile(script.convert->root, pl.baseDir, carDir.string());
        } else {
            const std::string low = lower(script.convert->root);
            if (low.size() > 4 && low.compare(low.size() - 4, 4, ".xsi") == 0)
                pl.meshSource = resolveAssetPath(script.convert->root, pl.baseDir, carDir.string());
            for (const char* ext : {".xsi", ".XSI"}) {
                if (!pl.meshSource.empty()) break;
                pl.meshSource = resolveAssetPath(script.convert->root + ext, pl.baseDir, carDir.string());
            }
        }
        pl.glmStem = stemOf(pl.carPath) + sfx;
        const fs::path glmDir = opt.outputDir.empty() ? carDir : fs::path(opt.outputDir);
        pl.glmPath = (glmDir / (pl.glmStem + ".glm")).lexically_normal().make_preferred().string();
    }
    return pl;
}

namespace {

// The bones the mesh source weights - dotXSI envelopes or ASE weights.
std::vector<std::string> meshWeightedBones(const JobPlan& pl) {
    if (pl.meshIsAse) return ase::referencedBones(ase::parseFile(pl.meshSource, pl.baseDir));
    return xsi::weightedDeformers(xsi::parseFile(pl.meshSource));
}

// Number of $aseanimgrab lines before a statement (same file: by line).
std::size_t grabsBefore(const Script& script, const Statement& st) {
    std::size_t n = 0;
    for (const auto& g : script.grabs)
        if (g.line < st.line) ++n;
    return n;
}

// The $bonehiercap lines of the script: cap bones directly, or "file <name>"
// for the caps of <asset root>/<name>.bonecap.
template <class Warn>
std::vector<std::string> scriptCaps(const Script& script, const JobPlan& pl, const Warn& warn) {
    std::vector<std::string> caps;
    for (const auto& st : script.statements) {
        if (st.cmd != Cmd::BoneHierCap || st.args.empty()) continue;
        if (lower(st.args[0]) == "file" && st.args.size() > 1) {
            std::string rel = slashes(st.args[1]);
            if (fs::path(rel).extension().empty()) rel += ".bonecap";
            const std::string p = resolveAssetPath(rel, pl.baseDir, fs::path(pl.carPath).parent_path().string());
            if (p.empty()) {
                // Carcass ignored a missing file silently.
                warn("$bonehiercap file: \"" + rel + "\" nicht gefunden");
                continue;
            }
            for (const auto& cap : readBoneCapCaps(readWholeFile(p))) caps.push_back(cap);
        } else {
            caps.push_back(st.args[0]);
        }
    }
    return caps;
}

std::vector<xsi::AnimFile> loadGrabbed(const Script& script, const std::vector<std::string>& resolved,
                                       const BuildOptions& bo) {
    std::vector<xsi::AnimFile> files(resolved.size());
    std::vector<std::string> errors(resolved.size());
    parallelFor(
        resolved.size(),
        [&](std::size_t i) {
            if (resolved[i].empty()) return;
            try {
                AnimCache cache(bo.cacheDir);
                files[i] = cache.enabled() ? cache.loadOrParse(resolved[i]) : xsi::loadAnimationFile(resolved[i]);
                files[i].sourcePath = resolved[i];
            } catch (const std::exception& e) {
                errors[i] = script.grabs[i].file + ": " + e.what();
            }
        },
        bo.threads);
    for (const auto& e : errors)
        if (!e.empty()) throw std::runtime_error("Lesefehler: " + e);
    return files;
}

// Per frame: every bone's model-space matrix relative to its base pose,
// from parent-relative GLA-style frames (F(b) = F(parent) * A(b)), turned 90
// degrees about Z as Carcass always does for an MDR.
std::vector<std::vector<Mat3x4>> absoluteFrames(const Skeleton& sk, const AnimationFrames& rel) {
    Mat3x4 rz{};
    rz.m[0][1] = -1.0f;
    rz.m[1][0] = 1.0f;
    rz.m[2][2] = 1.0f;
    const int nb = static_cast<int>(sk.bones.size());
    std::vector<std::vector<Mat3x4>> out(static_cast<std::size_t>(rel.frameCount()), std::vector<Mat3x4>(static_cast<std::size_t>(nb)));
    for (int f = 0; f < rel.frameCount(); ++f) {
        std::vector<bool> done(static_cast<std::size_t>(nb), false);
        std::function<void(int)> eval = [&](int b) {
            if (done[static_cast<std::size_t>(b)]) return;
            const int p = sk.bones[static_cast<std::size_t>(b)].parent;
            if (p >= 0) {
                eval(p);
                out[static_cast<std::size_t>(f)][static_cast<std::size_t>(b)] =
                    mul(out[static_cast<std::size_t>(f)][static_cast<std::size_t>(p)], rel.at(f, b));
            } else {
                out[static_cast<std::size_t>(f)][static_cast<std::size_t>(b)] = mul(rz, rel.at(f, b));
            }
            done[static_cast<std::size_t>(b)] = true;
        };
        for (int b = 0; b < nb; ++b) eval(b);
    }
    return out;
}

template <class Warn, class Say>
Skeleton newSkeletonFor(const Script& script, const JobPlan& pl, const JobOptions& opt, JobResult& res,
                        const Warn& warn, const Say& say,
                        std::vector<std::pair<std::string, std::vector<std::string>>>& capped) {
    say(JobLog::Info, "Skelett   : neu aus den Quelldateien (wie Carcass)");
    if (pl.glaName.empty())
        throw std::runtime_error("Fuer ein neues Skelett fehlt -makeskel <pfad/name> auf der "
                                 "$aseanimconvertmdx-Zeile - sonst hat die GLA keinen Namen");
    if (pl.meshSource.empty())
        throw std::runtime_error("Fuer ein neues Skelett wird die Mesh-Quelle gebraucht ($aseanimconvertmdx "
                                 "<root>) - sie bestimmt, welche Bones bleiben");

    BuildOptions bo = opt.build;
    bo.baseDir = pl.baseDir;
    std::vector<std::string> resolved;
    const auto missing = findMissingFiles(script, pl.carPath, bo, &resolved);
    if (!missing.empty() && !bo.skipMissing) {
        std::string list;
        for (std::size_t i = 0; i < missing.size() && i < 8; ++i) list += "\n  " + missing[i].file;
        throw std::runtime_error(std::to_string(missing.size()) + " von " +
                                 std::to_string(script.grabs.size()) +
                                 " Animationsdateien nicht gefunden:" + list);
    }

    // Read every grabbed file once (through the cache: the build right after
    // finds them there).
    std::vector<xsi::AnimFile> files = loadGrabbed(script, resolved, bo);
    std::vector<const xsi::AnimFile*> anims;
    for (std::size_t i = 0; i < files.size(); ++i)
        if (!resolved[i].empty()) anims.push_back(&files[i]);
    if (false) {
    std::vector<std::string> errors(resolved.size());
    parallelFor(
        resolved.size(),
        [&](std::size_t i) {
            if (resolved[i].empty()) return;
            try {
                AnimCache cache(bo.cacheDir);
                files[i] = cache.enabled() ? cache.loadOrParse(resolved[i]) : xsi::loadAnimationFile(resolved[i]);
                files[i].sourcePath = resolved[i];
            } catch (const std::exception& e) {
                errors[i] = script.grabs[i].file + ": " + e.what();
            }
        },
        bo.threads);
    for (const auto& e : errors)
        if (!e.empty()) throw std::runtime_error("Lesefehler: " + e);
    }

    SkeletonBuildOptions so;
    so.name = pl.glaName;
    so.scale = script.scale ? static_cast<float>(*script.scale) : 1.0f;
    so.keepMotion = script.keepMotion;
    if (opt.flatten) {
        so.pcj.push_back("$flatten");
        so.pcjFrom.push_back(0);
    }
    for (const auto& st : script.statements)
        if (st.cmd == Cmd::Pcj && !st.args.empty()) {
            so.pcj.push_back(st.args[0]);
            so.pcjFrom.push_back(grabsBefore(script, st));
        }
    MdxaFile refGla;
    if (const Statement* st = firstOf(script, Cmd::AseAnimRefGla)) {
        res.referenceGla = pl.referenceGla;
        if (res.referenceGla.empty())
            throw std::runtime_error("GLA aus $aseanimref_gla nicht gefunden: " + st->args[0]);
        refGla = readMdxa(readWholeFileBytes(res.referenceGla));
        so.reference = &refGla.skeleton;
        // The reference's own caps (Carcass loads <ref>.bonecap with it).
        {
            fs::path bc = res.referenceGla;
            bc.replace_extension(".bonecap");
            std::error_code ec;
            if (fs::exists(bc, ec))
                for (const auto& cap : readBoneCapCaps(readWholeFile(bc.string()))) so.caps.push_back(cap);
        }
        so.refBeforeGrab = grabsBefore(script, *st);
        say(JobLog::Info, "Referenz  : " + res.referenceGla + " (Reihenfolge und Bones)");
    }

    for (const auto& cap : scriptCaps(script, pl, warn)) so.caps.push_back(cap);
    const auto weighted = meshWeightedBones(pl);
    SkeletonBuildResult sb = buildSkeleton(anims, weighted, so);
    capped = sb.capped;
    for (const auto& w : sb.warnings) warn(w);
    res.skeletonBuilt = true;

    // An existing GLA with the same bones keeps its order: every GLM made
    // for it stores bone INDICES.
    std::error_code ec;
    if (!so.reference && fs::exists(pl.glaPath, ec)) {
        try {
            const MdxaFile old = readMdxa(readWholeFileBytes(pl.glaPath));
            if (!keepBoneOrder(sb.skeleton, old.skeleton))
                warn("Das neue Skelett hat andere Bones als die vorhandene GLA (" +
                     std::to_string(old.skeleton.bones.size()) + " -> " +
                     std::to_string(sb.skeleton.bones.size()) +
                     ") - Modelle (GLM) fuer die alte GLA muessen neu gebaut werden");
        } catch (const std::exception&) {
        }
    }
    say(JobLog::Info, "Skelett   : " + std::to_string(sb.skeleton.bones.size()) + " Bones");
    return sb.skeleton;
}


template <class Warn, class Say, class WriteOne>
void runMdr(const Script& script, const JobPlan& pl, const JobOptions& opt, JobResult& res, const Warn& warn,
            const Say& say, const WriteOne& writeOne) {
    (void)res;
    const Statement* st = firstOf(script, Cmd::AseAnimConvert);
    const bool anim = st != nullptr;
    if (!st) st = firstOf(script, Cmd::AseConvert);
    const std::string cmd = anim ? "$aseanimconvert" : "$aseconvert";

    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    bool weapon = false, player = false;
    for (std::size_t i = 1; i < st->args.size(); ++i) {
        const std::string a = lower(st->args[i]);
        if (a == "-origin") {
            if (i + 3 >= st->args.size()) throw std::runtime_error(cmd + ": -origin braucht drei Zahlen");
            const double x = st->argNumber(i + 1, "-origin"), y = st->argNumber(i + 2, "-origin"),
                         z = st->argNumber(i + 3, "-origin");
            origin = {static_cast<float>(y), static_cast<float>(-x), static_cast<float>(-z)};
            i += 3;
        } else if (a == "-maxtris" || a == "-scale") {
            if (i + 1 >= st->args.size()) throw std::runtime_error(cmd + ": " + a + " ohne Wert");
            ++i;   // Carcass reads both and uses neither for the MDR
        } else if (a == "-playerparms") {
            player = true;
            i += 2;
        } else if (a == "-weapon") {
            weapon = true;
        } else {
            warn(cmd + ": unbekannte Angabe \"" + st->args[i] + "\" wird uebergangen");
        }
    }
    if (player)
        throw std::runtime_error(cmd + " -playerparms (head/upper/lower.mdr) gibt es nicht: Carcass v2.2 brach "
                                 "dabei immer ab (es verlangt tag_-Objekte und lehnt sie zugleich ab)");
    if (weapon) {
        if (anim) throw std::runtime_error("$aseanimconvert -weapon: Waffenmodelle haben keine Animationen");
        warn("$aseconvert -weapon schreibt nichts - so war es auch bei Carcass");
        return;
    }

    const fs::path carDir = fs::path(pl.carPath).parent_path();
    const std::string asePath = ase::findAseFile(st->arg(0, cmd.c_str()), pl.baseDir, carDir.string());
    if (asePath.empty()) throw std::runtime_error(cmd + ": \"" + st->args[0] + "(.ask)\" nicht gefunden");
    say(JobLog::Info, "ASE       : " + asePath);
    const ase::Scene scene = ase::parseFile(asePath, pl.baseDir);
    for (const auto& w : scene.warnings) warn(w);

    // Skeleton and frames: from the grabbed animations, or from a GLA
    // ($aseanimgrab_gla - Carcass gave identity matrices there, SPEC B5).
    Skeleton sk;
    AnimationFrames rel;
    if (const Statement* gg = firstOf(script, Cmd::AseAnimGrabGla)) {
        std::string relPath = slashes(gg->args[0]);
        if (withoutGla(relPath) == relPath) relPath += ".gla";
        const std::string p = resolveAssetPath(relPath, pl.baseDir, carDir.string());
        if (p.empty()) throw std::runtime_error("GLA aus $aseanimgrab_gla nicht gefunden: " + gg->args[0]);
        const MdxaFile g = readMdxa(readWholeFileBytes(p));
        sk = g.skeleton;
        rel.resize(g.numFrames, static_cast<int>(sk.bones.size()));
        for (int f = 0; f < g.numFrames; ++f)
            for (int b = 0; b < static_cast<int>(sk.bones.size()); ++b) rel.at(f, b) = g.boneMatrix(f, b);
    } else {
        if (script.grabs.empty())
            throw std::runtime_error(cmd + ": ohne $aseanimgrab gibt es kein Skelett und keine Frames");
        BuildOptions bo = opt.build;
        bo.baseDir = pl.baseDir;
        std::vector<std::string> resolved;
        const auto missing = findMissingFiles(script, pl.carPath, bo, &resolved);
        if (!missing.empty()) throw std::runtime_error("Animationsdatei nicht gefunden: " + missing.front().file);
        const std::vector<xsi::AnimFile> files = loadGrabbed(script, resolved, bo);
        std::vector<const xsi::AnimFile*> anims;
        for (const auto& f : files) anims.push_back(&f);
        SkeletonBuildOptions so;
        so.name = "mdr";
        so.scale = 1.0f;   // the convert line resets $scale (Carcass)
        so.keepMotion = true;
        so.mdr = true;
        SkeletonBuildResult sb = buildSkeleton(anims, ase::referencedBones(scene), so);
        sk = sb.skeleton;
        xsi::EvalOptions eo;
        eo.scale = 1.0f;
        eo.extractRootMotion = false;   // no motion compensation for MDR
        eo.warnMissingBones = false;
        rel.numBones = static_cast<int>(sk.bones.size());
        for (const auto& a : files) {
            xsi::EvalResult ev = xsi::evaluate(sk, a, eo);
            const int n = ev.frames.frameCount();
            const int step = (opt.build.frameStep > 1 && n > opt.build.frameStep) ? opt.build.frameStep : 1;
            if (n == 1) throw std::runtime_error("\"" + a.sourcePath + "\" hat nur einen Frame");
            for (int f = 0; f < n; f += step)
                for (int b = 0; b < rel.numBones; ++b) rel.matrices.push_back(ev.frames.at(f, b));
        }
    }

    ase::MdrInput mi;
    mi.scene = &scene;
    mi.skeleton = &sk;
    mi.frames = absoluteFrames(sk, rel);
    mi.originAdjust = origin;
    // Carcass always wrote "test.mdr"; the name of the .ask is what was meant.
    const std::string stem = fs::path(asePath).stem().string();
    mi.name = stem + ".mdr";
    const ase::MdrResult mr = ase::buildMdr(mi);
    for (const auto& w : mr.warnings) warn(w);
    const fs::path dir = opt.outputDir.empty() ? fs::path(asePath).parent_path() : fs::path(opt.outputDir);
    if (opt.outputs.glm) writeOne("MDR", dir / (stem + ".mdr"), mr.data.data(), mr.data.size());
    if (opt.outputs.skin) writeOne(".skin", dir / (stem + "_default.skin"), mr.skin.data(), mr.skin.size());
    say(JobLog::Info, "MDR       : " + std::to_string(mr.bones) + " Bones, " + std::to_string(mr.frames) + " Frames, " +
                          std::to_string(mr.surfaces) + " Surfaces");
}

}  // namespace

JobResult runJob(const Script& script, const std::string& carPathIn, const JobOptions& opt) {
    JobResult res;
    const auto say = [&](JobLog l, const std::string& t) {
        if (opt.log) opt.log(l, t);
    };
    const auto warn = [&](const std::string& t) {
        res.warnings.push_back(t);
        say(JobLog::Warn, t);
    };

    const JobPlan pl = planJob(script, carPathIn, opt);
    const std::string& carPath = pl.carPath;
    res.kind = pl.kind;
    if (res.kind == ScriptKind::Empty)
        throw std::runtime_error("Das Skript enthaelt weder $aseanimgrab noch $aseanimgrab_gla - "
                                 "es gibt nichts zu bauen");
    const fs::path carDir = fs::path(carPath).parent_path();
    const std::string& baseDir = pl.baseDir;

    if (res.kind == ScriptKind::Mdr) {
        const auto writeOneMdr = [&](const char* what, const fs::path& target, const void* data, std::size_t n) {
            const fs::path p = target.lexically_normal().make_preferred();
            std::error_code ec;
            if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
            writeFileChecked(p.string(), data, n);
            res.written.push_back(JobFile{what, p.string(), n});
            say(JobLog::Good, std::string("Geschrieben: ") + p.string());
        };
        runMdr(script, pl, opt, res, warn, say, writeOneMdr);
        return res;
    }

    // What Carcass refuses on purpose - with the real reason instead of its
    // mangled messages.
    if (!script.grabs.empty() && firstOf(script, Cmd::AseAnimGrabGla))
        throw std::runtime_error("$aseanimgrab_gla (fertige GLA benutzen) und $aseanimgrab (Animationen "
                                 "lesen) im selben Skript - das eine schliesst das andere aus");
    bool skew90 = false;
    if (script.convert) {
        const auto& xa = script.convert->extraArgs;
        for (std::size_t i = 0; i < xa.size(); ++i) {
            std::string a = lower(xa[i]);
            if (a == "-maxtris") {
                if (i + 1 >= xa.size()) throw std::runtime_error("-maxtris ohne Zahl");
                ++i;   // only used by Carcass's MD3 writer; no effect on GLA/GLM
            } else if (a == "-smooth" || a == "-losedupverts") {
            } else if (a == "-skew90") {
                skew90 = true;
            } else if (a == "-noskew90") {
            } else if (a == "-scale") {
                throw std::runtime_error("-scale gehoert nicht auf die $aseanimconvertmdx-Zeile - "
                                         "stattdessen \"$scale <wert>\" nach $aseanimgrabinit");
            } else if (a == "-skel") {
                throw std::runtime_error("-skel gibt es nicht mehr - eine fertige GLA wird mit "
                                         "\"$aseanimgrab_gla <name.gla>\" benutzt");
            } else if (a == "-ignorebasedeviations") {
                throw std::runtime_error("-ignorebasedeviations ist ein Schalter der Kommandozeile, nicht "
                                         "der $aseanimconvertmdx-Zeile (und g2c braucht ihn nicht)");
            } else {
                throw std::runtime_error("Unbekannte Angabe \"" + xa[i] + "\" auf der $aseanimconvertmdx-Zeile");
            }
        }
        // Carcass: "-noskew90" anywhere switches it off again.
        for (const auto& a : xa)
            if (lower(a) == "-noskew90") skew90 = false;
        if (res.kind == ScriptKind::Model && !script.convert->makeSkel.empty())
            warn("-makeskel wird bei $aseanimgrab_gla nicht benutzt - die GLA ist schon da");
    }

    // --- Skeleton -------------------------------------------------------------
    //
    // Either an existing GLA (the reference), or - as Carcass always does - a
    // new one built from the grabbed files and the mesh source.
    const bool hasRefLine = firstOf(script, Cmd::AseAnimRefGla) != nullptr;
    const bool buildNew = res.kind == ScriptKind::Animation && opt.referenceGla.empty() &&
                          (opt.newSkeleton || hasRefLine || pl.referenceGla.empty());
    Skeleton skel;
    std::vector<std::pair<std::string, std::vector<std::string>>> capped;
    if (buildNew) {
        skel = newSkeletonFor(script, pl, opt, res, warn, say, capped);
    } else {
    res.referenceGla = pl.referenceGla;
    if (res.referenceGla.empty()) {
        if (const Statement* st = firstOf(script, Cmd::AseAnimGrabGla))
            throw std::runtime_error("GLA aus $aseanimgrab_gla nicht gefunden: " + st->args[0]);
        if (const Statement* st = firstOf(script, Cmd::AseAnimRefGla))
            throw std::runtime_error("GLA aus $aseanimref_gla nicht gefunden: " + st->args[0]);
        throw std::runtime_error("Keine Referenz-GLA: weder angegeben noch im Skript "
                                 "($aseanimref_gla, $aseanimgrab_gla oder -makeskel)");
    }
    const MdxaFile refFile = readMdxa(readWholeFileBytes(res.referenceGla));
    say(JobLog::Info, "Referenz  : " + res.referenceGla + " (" +
                          std::to_string(refFile.skeleton.bones.size()) + " Bones)");
    skel = refFile.skeleton;
    if (res.kind == ScriptKind::Animation) {
        const auto caps = scriptCaps(script, pl, warn);
        if (!caps.empty()) {
            const auto kids = skel.buildChildLists();
            for (const auto& cap : caps)
                for (std::size_t b = 0; b < skel.bones.size(); ++b)
                    if (lower(skel.bones[b].name) == lower(cap) && !kids[b].empty())
                        warn("$bonehiercap " + cap + ": die Referenz-GLA hat darunter noch Bones - "
                             "mit \"Skelett neu bauen\" (-newskel) wird gekappt");
        }
    }

    // Bones the mesh weights that the reference lacks: they would silently
    // lose their weights. Carcass would have rebuilt the skeleton.
    if (res.kind == ScriptKind::Animation && !pl.meshSource.empty() && opt.outputs.glm) {
        try {
            std::set<std::string> have;
            for (const auto& b : skel.bones) have.insert(lower(b.name));
            std::string extra;
            for (const auto& w : meshWeightedBones(pl))
                if (!have.count(lower(xsi::glaBoneName(w)))) extra += (extra.empty() ? "" : ", ") + w;
            if (!extra.empty())
                warn("Das Mesh gewichtet Bones, die die Referenz-GLA nicht hat (" + extra +
                     ") - mit \"Skelett neu bauen\" (-newskel) kommen sie ins Skelett");
        } catch (const std::exception&) {
        }
    }
    }

    std::string glaName = pl.glaName.empty() ? skel.name : pl.glaName;
    const bool skip = pl.skip;
    const std::string sfx = skip ? "_skip" : "";
    const fs::path glaPath = pl.glaPath;

    const auto writeOne = [&](const char* what, const fs::path& target, const void* data,
                              std::size_t n) {
        const fs::path p = target.lexically_normal().make_preferred();
        std::error_code ec;
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        writeFileChecked(p.string(), data, n);
        res.written.push_back(JobFile{what, p.string(), n});
        say(JobLog::Good, std::string("Geschrieben: ") + p.string());
    };
    const auto writeText = [&](const char* what, const fs::path& p, const std::string& t) {
        writeOne(what, p, t.data(), t.size());
    };

    std::vector<std::uint8_t> glaBytes;   // for _info.txt

    // --- Animations -----------------------------------------------------------
    if (res.kind == ScriptKind::Animation) {
        // The engine loads "<name>.gla" through a 64-byte buffer (MAX_QPATH).
        // Carcass stops here too, but only after it already wrote .frames.
        if (opt.outputs.gla && glaName.size() + 4 >= 64)
            throw std::runtime_error("GLA-Name \"" + glaName + ".gla\" hat " +
                                     std::to_string(glaName.size() + 4) +
                                     " Zeichen, das Spiel erlaubt hoechstens 63 (MAX_QPATH)");
        if (opt.outputs.frames && glaName.size() + 7 >= 64)
            warn("\"" + glaName + ".frames\" ist laenger als 63 Zeichen (MAX_QPATH)");

        res.anim = build(script, skel, carPath, opt.build);
        for (const auto& w : res.anim.warnings) warn(w);

        // -skew90: the whole model turned 90 degrees about Z, on the root
        // bone in every frame (Carcass writes model_root as w=z=0.7071).
        if (skew90 && res.anim.frames.numBones > 0) {
            Mat3x4 rz{};
            rz.m[0][1] = -1.0f;
            rz.m[1][0] = 1.0f;
            rz.m[2][2] = 1.0f;
            for (int f = 0; f < res.anim.frames.frameCount(); ++f)
                res.anim.frames.at(f, 0) = mul(rz, res.anim.frames.at(f, 0));
            say(JobLog::Info, "-skew90   : Modell um 90 Grad um Z gedreht");
        }

        // Two DIFFERENT files with the same name - Carcass warns "Sequence ...
        // is used %d times": one of them is almost certainly the wrong one.
        {
            std::map<std::string, std::set<std::string>> byStem;
            for (const auto& b : res.anim.frameBlocks) {
                std::string full = lower(fs::absolute(b.sourcePath).generic_string());
                byStem[lower(fs::path(full).stem().string())].insert(full);
            }
            for (const auto& [stem, paths] : byStem)
                if (paths.size() > 1)
                    warn("Sequenz \"" + stem + "\" kommt aus " + std::to_string(paths.size()) +
                         " verschiedenen Dateien gleichen Namens: " + *paths.begin() + ", " +
                         *std::next(paths.begin()));
        }

        std::map<std::string, int> seen;
        for (const auto& sq : res.anim.sequences) ++seen[upper(sq.name)];
        for (const auto& [name, n] : seen)
            if (n > 1) res.duplicates.push_back(name);
        if (!res.duplicates.empty()) {
            say(JobLog::Bad, std::to_string(res.duplicates.size()) +
                                 " Sequenzname(n) doppelt - nichts geschrieben");
            return res;
        }

        if (opt.outputs.gla || opt.outputs.animationCfg || opt.outputs.frames) {
            Skeleton outSkel = skel;
            outSkel.name = glaName;
            if (opt.outputs.gla) {
                MdxaWriteOptions wo;
                wo.threads = opt.build.threads;
                if (opt.build.carcassCompatible) {
                    wo.compress.rounding = Rounding::Legacy;
                    wo.compress.optimizeQuat = false;
                    wo.compress.canonicalizeSign = false;
                }
                const auto w = writeMdxa(outSkel, res.anim.frames, wo);
                res.poolEntries = w.poolEntries;
                res.dedupeRatio = w.dedupeRatio();
                res.compression = w.stats.summary();
                if (opt.backup) keepBackup(glaPath);
                writeOne("GLA", glaPath, w.data.data(), w.data.size());
                glaBytes = w.data;
                // Which bones $bonehiercap cut off - read again by every
                // script that uses this GLA. Carcass deleted an existing
                // .bonecap silently when there were no caps; it stays here.
                if (!capped.empty()) {
                    fs::path bc = glaPath;
                    bc.replace_extension(".bonecap");
                    writeText(".bonecap", bc, writeBoneCap(capped));
                }
            }
            if (opt.outputs.animationCfg) {
                std::ostringstream head;
                head << res.anim.totalFrames() << " frames; " << res.anim.sequences.size()
                     << " sequences; erzeugt von g2c";
                fs::path cfg = glaPath.parent_path() / "animation.cfg";
                // A thinned-out build next to the normal GLA must not replace
                // that GLA's animation.cfg - the frame numbers differ.
                if (skip) {
                    std::error_code se;
                    fs::path normal = glaPath;
                    normal.replace_filename(glaPath.stem().string().substr(
                                                0, glaPath.stem().string().size() - sfx.size()) +
                                            ".gla");
                    if (fs::exists(normal, se)) {
                        cfg = glaPath.parent_path() / "animation_skip.cfg";
                        warn("-framestep: animation_skip.cfg statt animation.cfg geschrieben, weil "
                             "daneben die normale GLA liegt. Fuer das Spiel zusammen mit der _skip-GLA "
                             "in einen eigenen Ordner legen und in animation.cfg umbenennen.");
                    }
                }
                if (opt.backup) keepBackup(cfg);
                writeText("animation.cfg", cfg, writeAnimationCfg(res.anim.sequences, head.str()));
            }
            if (opt.outputs.frames && !res.anim.frameBlocks.empty()) {
                std::vector<FrameEntry> fe;
                for (const auto& b : res.anim.frameBlocks) {
                    FrameEntry e;
                    // Lowercase, as Carcass writes it.
                    e.sourcePath = lower(fs::absolute(b.sourcePath).generic_string());
                    e.startFrame = b.startFrame;
                    e.duration = b.duration;
                    e.fps = b.fps;
                    for (int k = 0; k < 3; ++k) e.averageVec[k] = b.averageVec[k];
                    e.deltaVecs = b.deltaVecs;
                    fe.push_back(std::move(e));
                }
                fs::path fr = glaPath;
                fr.replace_extension(".frames");
                writeText(".frames", fr, writeFrames(fe));
            }
        }
    }

    // --- Mesh -----------------------------------------------------------------
    if (opt.outputs.glm && script.convert && !script.convert->root.empty()) {
        const std::string& xsiPath = pl.meshSource;
        if (xsiPath.empty()) {
            const std::string t = "Mesh-Quelle \"" + script.convert->root + (pl.meshIsAse ? ".ASK" : ".xsi") +
                                  "\" nicht gefunden";
            if (res.kind == ScriptKind::Model) throw std::runtime_error(t);
            warn(t + " - keine GLM");
        } else {
            const std::string& glmStem = pl.glmStem;
            const fs::path glmPath = pl.glmPath;
            const fs::path glmDir = glmPath.parent_path();

            xsi::MeshImportOptions mo;
            mo.scale = skel.scale > 0.0f ? skel.scale : 1.0f;
            // $scale: in an animation script the skeleton already carries it;
            // in a model script a $scale AFTER $aseanimgrab_gla overrides the
            // GLA's scale for the mesh (Carcass: loading the GLA sets the
            // scale, a later $scale replaces it).
            if (res.kind == ScriptKind::Model) {
                const Statement* gg = firstOf(script, Cmd::AseAnimGrabGla);
                for (const auto& st : script.statements)
                    if (st.cmd == Cmd::Scale && !st.args.empty() && gg && st.line > gg->line &&
                        st.file == gg->file)
                        mo.scale = static_cast<float>(st.argNumber(0, "$scale"));
            } else if (script.scale && *script.scale > 0.0) {
                mo.scale = static_cast<float>(*script.scale);
            }
            mo.animName = glaName;
            mo.modelName = glmStem + ".glm";
            for (const auto& b : skel.bones) mo.boneNames.push_back(b.name);
            // -smooth / -losedupverts from the convert line - and, unlike
            // Carcass (which reset them on every convert line), from the
            // command line too.
            for (const auto& a : script.convert->extraArgs) {
                if (lower(a) == "-smooth") mo.smooth = true;
                if (lower(a) == "-losedupverts") mo.loseDupVerts = true;
            }
            mo.smooth = mo.smooth || opt.smooth;
            mo.carcassSmooth = opt.build.carcassCompatible;
            mo.loseDupVerts = mo.loseDupVerts || opt.loseDupVerts;

            // The name the game loads the model by must fit MAX_QPATH.
            if (!baseDir.empty()) {
                std::error_code ec;
                const std::string rel = fs::relative(glmPath, baseDir, ec).generic_string();
                if (!ec && !rel.empty() && rel.rfind("..", 0) != 0 && rel.size() >= 64)
                    throw std::runtime_error("GLM-Pfad \"" + rel + "\" hat " +
                                             std::to_string(rel.size()) +
                                             " Zeichen, das Spiel erlaubt hoechstens 63 (MAX_QPATH)");
            }

            say(JobLog::Info, "Mesh      : " + xsiPath);
            auto mr = pl.meshIsAse ? xsi::importMeshAse(ase::parseFile(xsiPath, baseDir), mo)
                                   : xsi::importMeshFile(xsiPath, mo);
            res.mesh = mr.stats;
            for (const auto& w : mr.stats.warnings) warn(w);
            for (const auto& nt : mr.stats.notes) say(JobLog::Info, nt);

            std::vector<std::string> shaders;
            if (!mr.mesh.lods.empty())
                for (const auto& sf : mr.mesh.lods[0].surfaces) shaders.push_back(sf.shader);

            // -makeskin: the textures go into <car>_default.skin and the GLM
            // keeps empty shader names, as with Carcass - the skin is what
            // the game uses then. Only when the skin is really written: with
            // the skin switched off the GLM keeps its shaders.
            const bool skin = opt.outputs.skin && (script.convert->makeSkin || opt.makeSkin);
            std::string skinText;
            if (skin) {
                skinText = writeSkin(mr.mesh);
                for (auto& lod : mr.mesh.lods)
                    for (auto& sf : lod.surfaces) sf.shader.clear();
            }
            const auto w = writeMdxm(mr.mesh);
            writeOne("GLM", glmPath, w.data.data(), w.data.size());
            res.meshBuilt = true;
            if (skin) writeText(".skin", glmDir / (glmStem + "_default.skin"), skinText);

            if (opt.outputs.info) {
                InfoInput info;
                info.glm = w.data;
                info.shaders = shaders;
                info.gla = glaBytes;
                info.deletedDupVerts = mr.stats.deletedDupVertsPerLod;
                info.deletedDupWeights = mr.stats.deletedDupWeightsPerLod;
                if (res.kind == ScriptKind::Model) info.usesGla = glaName + ".gla";
                writeText("_info.txt", glmDir / (glmStem + "_info.txt"), writeInfoText(info));
            }
        }
    }
    return res;
}

}  // namespace g2::car
