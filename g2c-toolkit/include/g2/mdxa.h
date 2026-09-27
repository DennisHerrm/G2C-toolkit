// g2/mdxa.h - Writing and reading GLA.

#pragma once

#include "g2/compress.h"
#include "g2/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

// True affine inverse of a 3x4 matrix. Needed for MdxaSkel::basePoseMatInv;
// the transpose is not enough as soon as scaling is involved.
Mat3x4 affineInverse(const Mat3x4& m);

// Concatenation of two affine 3x4 matrices.
//
// Used to live locally in xsi_anim.cpp. When the exporter needed the same
// computation, a copy would have been the obvious route - and that is
// exactly how two versions come about, one of which later gets fixed.
Mat3x4 mul(const Mat3x4& a, const Mat3x4& b);

struct MdxaWriteOptions {
    CompressOptions compress{};

    // Identical compressed bones share one pool entry. This saves a lot for
    // idle animations and long holds; _humanoid.gla has thousands of frames
    // with motionless fingers.
    bool dedupeBonePool = true;

    // 0 = all available cores. Only the compression is parallelized; building
    // the pool stays serial so the entry order, and thus the generated file,
    // stays reproducible.
    unsigned threads = 0;
};

struct MdxaWriteResult {
    std::vector<std::uint8_t> data;
    CompressStats             stats;
    std::size_t               poolEntries = 0;
    std::size_t               poolEntriesBeforeDedupe = 0;

    double dedupeRatio() const {
        return poolEntriesBeforeDedupe
                   ? 1.0 - static_cast<double>(poolEntries) /
                               static_cast<double>(poolEntriesBeforeDedupe)
                   : 0.0;
    }
};

MdxaWriteResult writeMdxa(const Skeleton& skel, const AnimationFrames& frames,
                          const MdxaWriteOptions& opt = {});

// --- Reading ---------------------------------------------------------------

struct MdxaFile {
    Skeleton                       skeleton;
    int                            numFrames = 0;
    std::vector<fmt::CompQuatBone> bonePool;
    std::vector<std::uint32_t>     indices;   // numFrames * numBones

    Mat3x4 boneMatrix(int frame, int bone) const;
};

MdxaFile readMdxa(const std::vector<std::uint8_t>& data);

// World matrices of all bones for one frame.
//
// The GLA stores one matrix per bone RELATIVE to the parent bone, combined
// with the bind pose. The world pose is obtained from
//
//     root:    X(b) = A(b)·B(b)
//     other:   X(b) = X(p)·B(p)^-1·A(b)·B(b)
//
// The order must be topological, not by index: in Raven's _humanoid.gla,
// eight bones have their parent bone AFTER them.
//
// Used to exist three times in the source - in the exporter, in the
// residual motion and in the preview. That is exactly how versions come
// about, one of which later gets fixed.
std::vector<Mat3x4> boneWorldMatrices(const MdxaFile& gla, int frame);

// Reads a file, writes it back, compares. This is the basis for the
// regression tests against original assets.
struct RoundTripReport {
    int    frames = 0;
    int    bones = 0;
    double maxPositionError = 0.0;
    double maxRotationErrorDeg = 0.0;
    double meanPositionError = 0.0;
    std::string describe() const;
};

RoundTripReport compareRoundTrip(const MdxaFile& original, const MdxaWriteOptions& opt);

}  // namespace g2
