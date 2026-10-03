// g2/carjob.h - One complete Carcass run.
//
// carbuild.h turns the $aseanimgrab lines into frames. This file does the
// rest of what carcass.exe does for a .car: decide what kind of script it is,
// find the reference skeleton, build the GLA and/or the GLM and write every
// file next to where Carcass wrote it - except that each output can be
// switched off. Command line and GUI both go through here, so they can no
// longer drift apart (they did: the GUI wrote the GLM with the reference's
// GLA name, the command line wrote into the current directory).

#pragma once

#include "g2/carbuild.h"
#include "g2/model.h"
#include "g2/xsi_mesh.h"

#include <functional>
#include <string>
#include <vector>

namespace g2::car {

// What a script asks for.
//   Animation: $aseanimgrab lines -> GLA + animation.cfg (+ GLM of the mesh
//              source on the $aseanimconvertmdx line, as Carcass does).
//   Model:     $aseanimgrab_gla <existing.gla> -> only the GLM; the GLA is
//              used, not written ("Uses existing anim file").
//   Mdr:       $aseconvert / $aseanimconvert -> an MDR from a 3ds Max ASE
//              (.ask), animated by the $aseanimgrab files.
enum class ScriptKind { Animation, Model, Mdr, Empty };
ScriptKind scriptKind(const Script& script);

// Every file a run can produce. Carcass always wrote all of them; here each
// one can be switched off - for animation work usually only the GLA and the
// animation.cfg matter.
struct JobOutputs {
    bool gla = true;
    bool animationCfg = true;
    bool frames = true;
    bool glm = true;
    // Only written if the script (or the caller) asks for it with -makeskin,
    // as in Carcass. This switch can only suppress it.
    bool skin = true;
    bool info = true;   // <name>_info.txt
};

enum class JobLog { Info, Good, Warn, Bad };

struct JobOptions {
    BuildOptions build;

    // Skeleton source. Empty = from the script: $aseanimref_gla,
    // $aseanimgrab_gla or <asset root>/<-makeskel>.gla.
    std::string referenceGla;

    // Where the files go. Empty = where Carcass writes them: the GLA under
    // <asset root>/<-makeskel>, the GLM next to the .car.
    std::string outputDir;
    // Explicit GLA path (command line -o); wins over outputDir for the GLA
    // and the files that belong next to it.
    std::string glaPath;

    JobOutputs outputs;

    // Same as Carcass's command line switches; they add to what the
    // script's $aseanimconvertmdx line says.
    bool makeSkin = false;
    bool smooth = false;
    bool loseDupVerts = false;

    // Build the skeleton anew from the source files, as Carcass always did,
    // even if a GLA already exists under -makeskel. Without it, an existing
    // GLA is the skeleton (its bone order and base poses stay exactly as
    // they are); a new one is only built when there is none yet, or when the
    // script has an $aseanimref_gla line.
    bool newSkeleton = false;
    // Carcass's -flatten: like "$pcj $flatten" before the script.
    bool flatten = false;

    // Keep one <file>.bak before replacing the GLA and the animation.cfg
    // that sit where the reference was read from.
    bool backup = false;

    // Progress and messages. May be empty.
    std::function<void(JobLog, const std::string&)> log;
};

struct JobFile {
    std::string what;   // "GLA", "animation.cfg", ".frames", "GLM", ".skin", "_info.txt"
    std::string path;
    std::size_t bytes = 0;
};

struct JobResult {
    ScriptKind               kind = ScriptKind::Empty;
    std::string              referenceGla;   // what was actually used
    bool                     skeletonBuilt = false;   // new skeleton from the sources
    std::vector<JobFile>     written;
    std::vector<std::string> warnings;

    // Sequence names that occur more than once. If not empty, NOTHING was
    // written: one of the two would be unreachable in the game.
    std::vector<std::string> duplicates;

    BuildResult              anim;    // empty for model scripts
    // Of the GLA written (zero if none was).
    std::size_t              poolEntries = 0;
    double                   dedupeRatio = 0.0;
    std::string              compression;   // CompressStats::summary()
    xsi::MeshImportStats     mesh;    // empty without a GLM
    bool                     meshBuilt = false;
};

// Everything a run decides before it reads a single animation: what kind of
// script, which skeleton, and where every file would go. runJob works from
// this; the "up to date" check of the Carcass mode uses it without building.
struct JobPlan {
    ScriptKind  kind = ScriptKind::Empty;
    std::string carPath;        // absolute
    std::string baseDir;
    std::string referenceGla;   // empty = none found
    std::string glaName;        // name in the GLA/GLM header; empty = the reference's
    std::string glaPath;        // where the GLA (and animation.cfg, .frames) go
    std::string meshSource;     // resolved root .xsi / .ask; empty = none / not found
    bool        meshIsAse = false;   // $aseanimconvertmdx without _noask reads ASE
    std::string glmStem;
    std::string glmPath;        // empty if the script has no mesh source
    bool        skip = false;   // -framestep build ("_skip" names)
};
JobPlan planJob(const Script& script, const std::string& carPath, const JobOptions& opt);

// Where Carcass would put the GLA for this script: <asset root>/<-makeskel>.gla.
// Empty if the script has no -makeskel.
std::string defaultGlaPath(const Script& script, const std::string& baseDir);

// The reference skeleton as the script names it (see JobOptions::referenceGla),
// resolved against the asset root. Empty if the script names none or the file
// does not exist.
std::string scriptReferenceGla(const Script& script, const std::string& baseDir,
                               const std::string& carPath);

// Runs the script. Throws on errors that stop the run (missing reference,
// unreadable mesh source, ...); warnings go to JobResult::warnings and log.
JobResult runJob(const Script& script, const std::string& carPath, const JobOptions& opt);

}  // namespace g2::car
