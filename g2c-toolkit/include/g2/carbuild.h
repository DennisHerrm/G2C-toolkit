// g2/carbuild.h - Executing a .car script.
//
// Combines the parser from carscript.h and the evaluation from xsi_anim.h
// into what Carcass does when invoked: collect all $aseanimgrab entries,
// concatenate them, compress, write the GLA and animation.cfg.
//
// Variant 2: the skeleton comes from a reference GLA, not from the source
// files. The reason is explained in xsi_anim.h.

#pragma once

#include "g2/carscript.h"
#include "g2/model.h"
#include "g2/animcache.h"
#include "g2/xsi_anim.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace g2::car {

struct BuildOptions {
    // Root under which the $aseanimgrab paths live. The paths in the .car
    // look like "models/players/__new_anim/...", so they are relative to the
    // assets root. If this field is empty, $basedir from the script is used,
    // and if that is missing too, the .car's directory.
    std::string baseDir;

    // Overrides the -origin from $aseanimconvertmdx. If not given, the value
    // from the script is used.
    std::optional<std::array<float, 3>> originOverride;

    // Skip missing .xsi files instead of aborting. Off by default: a missing
    // file shifts all following target frames and thereby makes the whole
    // animation.cfg wrong.
    bool skipMissing = false;

    // Last fallback for framespeed: only applies if a grab line has no
    // -framespeed AND the .xsi has no SI_Scene with a frame rate.
    int defaultFrameSpeed = 30;

    // Quantize bit-compatibly with Carcass: truncate instead of rounding, no
    // candidate search, no sign canonicalization.
    //
    // Costs accuracy (rotation error roughly a factor of 3), but makes the
    // bone pool as dense as with Carcass again. Only useful if file size
    // really matters or you want to compare outputs byte by byte.
    bool carcassCompatible = false;

    // Carcass's -framestep <n>: keep every n-th frame of each file that is
    // longer than n frames (shorter ones stay whole, as in Carcass). Unlike
    // Carcass, the speeds are divided by n too - there a stepped animation
    // played n times too fast. Loop frames and -additional ranges are scaled
    // with it.
    int frameStep = 1;

    // 0 = all available cores. The files are read, parsed and evaluated
    // independently of each other; that parallelizes completely. Afterwards
    // they are concatenated in script order again so the target frames are
    // correct.
    unsigned threads = 0;

    // Directory for the cache of parsed .xsi files. Empty = off.
    // Measured, 96 % of build time goes to reading and parsing; exactly that
    // is skipped on the second run.
    std::string cacheDir;

    // Called after every loaded file (progress, total count, name). Called
    // from multiple threads and already serialized.
    std::function<void(std::size_t, std::size_t, const std::string&)> progress;
};

// One block of the .frames file, one entry per collected source file.
struct FrameBlock {
    std::string sourcePath;      // resolved, absolute path
    int         startFrame = 0;
    int         duration = 0;
    int         fps = 0;         // rate from SI_Scene, NOT the framespeed
    float       averageVec[3] = {0.0f, 0.0f, 0.0f};
    // "-deltavecs" on the grab line: the Motion bone's step per frame
    // (delta0 = its position in the first frame). Empty = averagevec.
    std::vector<std::array<float, 3>> deltaVecs;
};

struct BuildResult {
    AnimationFrames          frames;
    std::vector<FrameBlock>  frameBlocks;
    std::vector<Sequence>    sequences;
    // Missing files with their context.
    //
    // The path alone is not enough: with 1393 grabs, "a file is missing"
    // says nothing about which animation is affected. The sequence name is
    // what the user searches for in their .car.
    struct MissingFile {
        std::string file;       // as in the .car
        std::string sequence;   // enum or derived name
        std::size_t grabIndex = 0;
        std::size_t line = 0;   // line in the .car, 0 = unknown
    };
    std::vector<MissingFile> missing;

    // Just the paths, for existing code.
    std::vector<std::string> missingFiles;
    std::vector<std::string> warnings;
    bool                     carcassCompatible = false;
    AnimCacheStats           cache;

    int totalFrames() const { return frames.frameCount(); }
};

// Resolves a path from the script against the base. Tries in order:
// <base>/<path>, <path> directly, <directory of the .car>/<path>.
// Returns an empty string if nothing is found.
std::string resolveAssetPath(const std::string& relative, const std::string& baseDir,
                             const std::string& carDir);

// Tries to derive the asset root and reference GLA from the .car's location.
//
// The paths in a .car look like "models/players/...". If the .car itself
// lies somewhere below a "models" directory, that directory's parent is the
// root we are looking for. The reference skeleton is given in -makeskel,
// without an extension.
struct AutoPaths {
    std::string baseDir;        // empty if not found
    std::string referenceGla;   // empty if not found
    std::string note;           // what was found, for the output
};
AutoPaths guessPaths(const Script& script, const std::string& carPath);

// Finds the asset root by walking up from the .car's directory until the
// first $aseanimgrab path resolves.
//
// Example: if the .car lies under
//   C:/jka_animations/md/base/models/players/__new_anim/_humanoid/
// and the first grab points to
//   models/players/__new_anim/__original_anim/both_a1.xsi
// then the root is C:/jka_animations/md/base.
//
// Returns an empty string if nothing matches.
std::string guessBaseDir(const Script& script, const std::string& carPath, int maxLevels = 10);

// Derives the reference GLA from -makeskel: <base>/<makeskel>.gla
// Returns an empty string if the file does not exist.
std::string guessReferenceGla(const Script& script, const std::string& baseDir);

// The grab files that cannot be found - exactly the check build() makes
// first, without reading anything. resolvedOut (optional) receives the
// resolved path of every grab, empty for the missing ones.
std::vector<BuildResult::MissingFile> findMissingFiles(const Script& script,
                                                       const std::string& carPath,
                                                       const BuildOptions& opt,
                                                       std::vector<std::string>* resolvedOut = nullptr);

BuildResult build(const Script& script, const Skeleton& reference, const std::string& carPath,
                  const BuildOptions& opt = {});

}  // namespace g2::car
