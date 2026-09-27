// g2/gladiff.h - Fully comparing two GLA files.
//
// Sampling only finds what is common. With 1.6 million bone instances, a
// deviation affecting exactly one sequence can vanish completely among 800
// random frames - and precisely such cases were the interesting ones in this
// project (the root motion affected 7 % of the frames, the SRT fallback on
// the other hand almost all of them).
//
// This comparison goes over every single instance and then attributes the
// deviations to bones and sequences, so you can see WHERE the problem is
// rather than just THAT one exists.

#pragma once

#include "g2/carscript.h"
#include "g2/mdxa.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

struct DiffOptions {
    // The deviation above which a bone instance counts as an outlier.
    //
    // Rotation is measured as a real angle in degrees, not as the difference
    // of individual matrix elements. The latter is hard to interpret: half a
    // quantization step per quaternion component propagates through the
    // products in the matrix into a multiple of itself, so practically every
    // bone looks like an outlier even though the actual angular error is far
    // below a hundredth of a degree.
    //
    // 0.1 degrees and two translation quantization steps are each clearly
    // above what two different rounding methods produce, and clearly below
    // what you can see.
    double toleranceTranslation = 2.0 / 64.0;
    double toleranceRotationDeg = 0.1;

    // Frame offset of the second file relative to the first. Useful when a
    // partial sequence is checked against a complete GLA.
    int frameOffsetB = 0;

    unsigned threads = 0;
};

struct BoneDiff {
    std::string name;
    std::uint64_t instances = 0;
    std::uint64_t outliers = 0;
    double        maxRotationDeg = 0.0;
    double        maxTranslation = 0.0;
    int           worstFrame = -1;

    double outlierPercent() const {
        return instances ? 100.0 * static_cast<double>(outliers) / static_cast<double>(instances) : 0.0;
    }
};

struct SequenceDiff {
    std::string   name;
    int           targetFrame = 0;
    int           frameCount = 0;
    std::uint64_t outliers = 0;
    double        maxDeviation = 0.0;      // translation, units
    double        maxRotationDeg = 0.0;
};

struct DiffResult {
    bool                     skeletonIdentical = false;
    std::vector<std::string> skeletonNotes;

    int frames = 0;
    int bones = 0;

    std::uint64_t instances = 0;
    std::uint64_t outliers = 0;
    double        maxRotationDeg = 0.0;
    double        maxTranslation = 0.0;
    double        meanTranslation = 0.0;
    double        meanRotationDeg = 0.0;

    std::vector<BoneDiff>     perBone;
    std::vector<SequenceDiff> perSequence;   // only if an animation.cfg was given

    double outlierPercent() const {
        return instances ? 100.0 * static_cast<double>(outliers) / static_cast<double>(instances) : 0.0;
    }
    bool clean() const { return outliers == 0; }
};

// Compares two GLAs. The sequence list is optional; without it, perSequence
// stays empty.
DiffResult diffMdxa(const MdxaFile& a, const MdxaFile& b,
                    const std::vector<car::Sequence>& sequences = {},
                    const DiffOptions& opt = {});

// Reads an animation.cfg back in. Only name, targetFrame, frameCount,
// loopFrame and frameSpeed; comment lines are skipped.
std::vector<car::Sequence> readAnimationCfg(const std::string& text);

}  // namespace g2
