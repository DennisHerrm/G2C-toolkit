// include/g2/xsi_export.h - Animations from a GLA back to dotXSI.
//
// The inverse of xsi_anim.cpp. The forward formula there is
//
//     root:    A(b) = X(b)·B(b)^-1              [+ origin, + root ramp]
//     other:   A(b) = B(p)·X(p)^-1·X(b)·B(b)^-1
//
// and can be rearranged unambiguously:
//
//     root:    X(b) = A(b)·B(b)
//     other:   X(b) = X(p)·B(p)^-1·A(b)·B(b)
//
// Then back into dotXSI space via wx = C^-1·(X/scale)·C, made local relative
// to the parent bone, and decomposed into translation plus Euler angles.
//
// What is NOT lossless here: the rotations are stored in the GLA as 16-bit
// quaternions. The error stays below 0.01 degrees and thus far below
// anything visible in the game - but the result will not be bit-identical
// to the original file.

#pragma once

#include "g2/compress.h"
#include "g2/carscript.h"
#include "g2/carvalidate.h"
#include "g2/mdxa.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace g2::xsiexp {

struct ExportOptions {
    // Must match the values the build was done with - otherwise the file
    // comes out at the wrong scale.
    float                              scale = 0.64f;
    std::optional<std::array<float, 3>> origin;

    // Frame rate for SI_Scene.
    int fps = 20;

    // Root motion per frame, from the .frames file ("averagevec").
    //
    // Without it, a run animation looks right but comes out RUNNING IN
    // PLACE: Carcass removes the root motion when building and puts it into
    // the .frames file so the game engine applies it. It is no longer in the
    // GLA.
    //
    // If it is applied here, the result is a .xsi that behaves like an
    // original file: rebuilding removes the motion again and writes the same
    // averagevec.
    std::optional<std::array<float, 3>> rootMotionPerFrame;

    // Which bind pose goes into the BASEPOSE block?
    //
    // Raven's root.xsi contains the WORLD pose there - recomputed, all nine
    // numbers match. Still, Carcass reports "non-uniform scaling" for our
    // files with values around 0.4096, i.e. 0.64 squared: the scaling is
    // applied twice.
    //
    // The suspicion: Carcass concatenates the BASEPOSE blocks along the
    // hierarchy. With Raven's files this doesn't show, because there are
    // grouping nodes in between (lleg_root, rd1root ...) whose bind pose is
    // neutral. Our files don't have those, because the GLA doesn't know them.
    //
    // Which variant is correct can only be decided with Carcass itself. So
    // offer both instead of guessing.
    enum class BasePose {
        World,   // as in Raven's files
        Local,   // relative to the parent bone, in case Carcass concatenates itself
        None,    // no block at all - as it was before the fix
    };
    BasePose basePose = BasePose::World;

    // Which dotXSI version goes into the header.
    //
    // The difference is not just the number: v3.0 NAMES its templates
    // ("SI_FCurve <bone>-SCALING-X { ... }"), v3.5 leaves them unnamed
    // ("SI_FCurve { \"<bone>\", \"SCALING-X\", ... }").
    //
    // Until now we wrote named templates under a 3.5 header - i.e. 3.0
    // content with a 3.5 label. Any tolerant parser reads that, but it is
    // not what the label says.
    //
    // Raven's JK2 model file is v3.0, the JKA animation files are v3.5.
    // Exporting for older tools wants 3.0; matching newer ones, 3.5. Being
    // able to output both costs little.
    enum class Version {
        V30,   // named templates, like Raven's root.xsi
        V35,   // unnamed templates, like Raven's animation files
    };
    Version version = Version::V30;
};

// Reads a sequence's root motion from a .frames file.
// The key is the source file's path as it appears in the .car.
std::optional<std::array<float, 3>> readAverageVec(const std::string& framesPath,
                                                   const std::string& sequenceOrFile);

// Estimates the -origin offset from the GLA.
//
// The offset is constant across all frames, the root motion is not - so
// the most frequent value per axis is the offset. It remains an estimate,
// though: if it yields something, the value is printed so it can be
// checked, and -origin overrides it at any time.
std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla);


// Looks for a constant offset in the root bone translation.
//
// "-origin 0 0 24" shifts the whole model when building, and the offset is
// then contained in every frame matrix. If it is not added back on export,
// rebuilding subtracts it a SECOND time - the model then stands 24 units
// off.
std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla);


// A section of the GLA as a standalone animation.
struct Sequence {
    std::string name;
    int         startFrame = 0;
    int         frameCount = 0;
};

// Residual motion of the Motion bone within a section.
//
// Carcass removes the root motion per ANIMATION. A sequence that is only a
// section of a longer animation - 16 % in JK2's animation.cfg - therefore
// carries a remainder of it.
//
// On rebuild, this remainder is removed again and ends up in the .frames
// file. The animation stays intact; only the place where the motion is
// stored changes. The new GLA is then not bit-identical to the source.
float residualMotion(const MdxaFile& gla, const Sequence& seq);

// Recovers the root motion from the GLA itself.
//
// The .frames file is NOT needed for this. Carcass computes the motion as a
// linear ramp on the root bone; so it is still in the GLA - as the
// translation difference between the first and last frame of the sequence,
// divided by the number of steps.
//
// Checked against Raven's _humanoid.frames: for all 178 sequences with
// motion, the value obtained this way matches "averagevec".
//
// Empty if the sequence has no motion - then there is nothing to restore.
std::optional<std::array<float, 3>> detectRootMotion(const MdxaFile& gla, const Sequence& seq);

// --- Turning a GLA back into a buildable script -----------------------------
//
// One line of animation.cfg.
struct CfgSequence {
    std::string name;
    int         start = 0;
    int         count = 0;
    int         loop = -1;
    double      fps = 20;   // may have decimals (hand-edited configs)
};

// A range that is written as its own .xsi, together with the sequences that
// lie inside it.
struct MasterGroup {
    CfgSequence              self;
    std::vector<CfgSequence> inside;
};

struct Grouping {
    std::vector<MasterGroup> masters;
    std::size_t              partial = 0;   // only partially overlapping
};

// Merges sub-ranges.
//
// Many sequences are sections of a longer one - 268 of 989 in JK2's
// animation.cfg. If each were exported as its own file, the new GLA would
// have more frames than the old one and animation.cfg would no longer fit.
//
// Therefore only the largest non-overlapping range in each case is written
// as a file; whatever lies inside it becomes -additional in the script -
// exactly the structure Raven itself uses.
Grouping groupSequences(const std::vector<CfgSequence>& cfg);

// Builds a .car script from it.
//
// xsiPrefix is the path prepended to the file names, as it should later
// appear in the .car (e.g. "models/players/jk2/").
//
// origin MUST be the same value that was used for the export: the export
// bakes it into the .xsi; if it is missing from the script, rebuilding does
// not subtract it again, and the whole model is off by that amount.
// makeSkel is the last line of the script: it says WHERE the GLA is created
// and what it is called. The value is exactly the one in the source GLA's
// header - for Raven's _humanoid.gla it is
// "models/players/_humanoid/_humanoid", and that is exactly what its .car
// says too. Empty = derive from xsiPrefix, which is only a stopgap.
// scale = 0 omits "$scale".
//
// Important for Carcass: the BASEPOSE values in the .xsi are already divided
// by the skeleton scale (as in Raven's files). If Carcass additionally
// applies "$scale 0.64", the scaling happens twice - and exactly 0.64 times
// 0.64 = 0.4096 is what it reports as "non-uniform scaling".
car::Script buildScript(const Grouping& g, const std::string& xsiPrefix,
                        const std::optional<std::array<float, 3>>& origin, float scale = 0.0f,
                        bool keepMotion = false, const std::string& makeSkel = {});

// Produces the contents of a dotXSI file for the given section.
std::string exportSequence(const MdxaFile& gla, const Sequence& seq,
                           const ExportOptions& opt);

}  // namespace g2::xsiexp
