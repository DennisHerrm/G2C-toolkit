#include "g2/gladiff.h"

#include "g2/parallel.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace g2 {
namespace {

double rotationDiff(const Mat3x4& a, const Mat3x4& b) {
    return angleBetweenDeg(matrixToQuat(a), matrixToQuat(b));
}

double translationDiff(const Mat3x4& a, const Mat3x4& b) {
    double worst = 0.0;
    for (int r = 0; r < 3; ++r)
        worst = std::max(worst, std::fabs(static_cast<double>(a.m[r][3] - b.m[r][3])));
    return worst;
}

}  // namespace

std::vector<car::Sequence> readAnimationCfg(const std::string& text) {
    std::vector<car::Sequence> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line.compare(first, 2, "//") == 0) continue;

        std::istringstream ls(line);
        car::Sequence s;
        if (!(ls >> s.name >> s.targetFrame >> s.frameCount >> s.loopFrame >> s.frameSpeed))
            continue;
        out.push_back(std::move(s));
    }
    return out;
}

DiffResult diffMdxa(const MdxaFile& a, const MdxaFile& b,
                    const std::vector<car::Sequence>& sequences, const DiffOptions& opt) {
    DiffResult res;

    // --- Skeleton first. If that doesn't match, the rest is meaningless. ---
    const int nbA = static_cast<int>(a.skeleton.bones.size());
    const int nbB = static_cast<int>(b.skeleton.bones.size());

    if (nbA != nbB)
        res.skeletonNotes.push_back("Bone-Anzahl: " + std::to_string(nbA) + " gegen " +
                                    std::to_string(nbB));
    const int nb = std::min(nbA, nbB);
    for (int i = 0; i < nb; ++i) {
        const Bone& x = a.skeleton.bones[static_cast<std::size_t>(i)];
        const Bone& y = b.skeleton.bones[static_cast<std::size_t>(i)];
        if (x.name != y.name)
            res.skeletonNotes.push_back("Bone " + std::to_string(i) + ": \"" + x.name +
                                        "\" gegen \"" + y.name + "\"");
        else if (x.parent != y.parent)
            res.skeletonNotes.push_back("Bone \"" + x.name + "\": Parent " +
                                        std::to_string(x.parent) + " gegen " +
                                        std::to_string(y.parent));
        else {
            double worst = 0.0;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    worst = std::max(worst, std::fabs(static_cast<double>(x.basePose.m[r][c] -
                                                                         y.basePose.m[r][c])));
            if (worst > 1e-4)
                res.skeletonNotes.push_back("Bone \"" + x.name + "\": Basispose weicht ab (" +
                                            std::to_string(worst) + ")");
        }
    }
    if (std::fabs(a.skeleton.scale - b.skeleton.scale) > 1e-6)
        res.skeletonNotes.push_back("Scale: " + std::to_string(a.skeleton.scale) + " gegen " +
                                    std::to_string(b.skeleton.scale));
    res.skeletonIdentical = res.skeletonNotes.empty();

    // --- Frames ---------------------------------------------------------
    const int framesA = a.numFrames;
    const int framesB = b.numFrames;

    // A negative offset would read B before its first frame. It is never
    // needed either: instead of shifting B backwards, shift A by swapping the
    // files.
    if (opt.frameOffsetB < 0)
        throw std::runtime_error("Frameversatz darf nicht negativ sein (Dateien vertauschen)");

    int frames = framesA;
    if (opt.frameOffsetB != 0 || framesA != framesB)
        frames = std::min(framesA, framesB - opt.frameOffsetB);
    frames = std::max(0, frames);

    res.frames = frames;
    res.bones = nb;
    if (frames == 0 || nb == 0) return res;

    res.perBone.resize(static_cast<std::size_t>(nb));
    for (int i = 0; i < nb; ++i)
        res.perBone[static_cast<std::size_t>(i)].name = a.skeleton.bones[static_cast<std::size_t>(i)].name;

    // Collected per bone so the threads don't get in each other's way:
    // each thread processes whole frames, and merging goes through a mutex
    // only once per frame.
    std::mutex merge;
    double sumTrans = 0.0;
    double sumRot = 0.0;

    parallelFor(
        static_cast<std::size_t>(frames),
        [&](std::size_t fu) {
            const int f = static_cast<int>(fu);
            std::vector<BoneDiff> local(static_cast<std::size_t>(nb));
            double localSum = 0.0;
            double localRot = 0.0;

            for (int i = 0; i < nb; ++i) {
                const Mat3x4 ma = a.boneMatrix(f, i);
                const Mat3x4 mb = b.boneMatrix(f + opt.frameOffsetB, i);
                const double dr = rotationDiff(ma, mb);
                const double dt = translationDiff(ma, mb);

                BoneDiff& L = local[static_cast<std::size_t>(i)];
                L.instances = 1;
                L.maxRotationDeg = dr;
                L.maxTranslation = dt;
                L.worstFrame = f;
                if (dr > opt.toleranceRotationDeg || dt > opt.toleranceTranslation) L.outliers = 1;
                localSum += dt;
                localRot += dr;
            }

            std::lock_guard<std::mutex> lock(merge);
            sumTrans += localSum;
            sumRot += localRot;
            for (int i = 0; i < nb; ++i) {
                BoneDiff& G = res.perBone[static_cast<std::size_t>(i)];
                const BoneDiff& L = local[static_cast<std::size_t>(i)];
                G.instances += 1;
                G.outliers += L.outliers;
                if (std::max(L.maxRotationDeg * 0.15, L.maxTranslation) >
                    std::max(G.maxRotationDeg * 0.15, G.maxTranslation))
                    G.worstFrame = L.worstFrame;
                G.maxRotationDeg = std::max(G.maxRotationDeg, L.maxRotationDeg);
                G.maxTranslation = std::max(G.maxTranslation, L.maxTranslation);
            }
        },
        opt.threads);

    for (const auto& bd : res.perBone) {
        res.instances += bd.instances;
        res.outliers += bd.outliers;
        res.maxRotationDeg = std::max(res.maxRotationDeg, bd.maxRotationDeg);
        res.maxTranslation = std::max(res.maxTranslation, bd.maxTranslation);
    }
    if (res.instances) {
        res.meanTranslation = sumTrans / static_cast<double>(res.instances);
        res.meanRotationDeg = sumRot / static_cast<double>(res.instances);
    }

    // --- Mapping to sequences --------------------------------------------
    if (!sequences.empty()) {
        for (const auto& s : sequences) {
            if (s.frameCount <= 0 || s.targetFrame >= frames) continue;
            SequenceDiff sd;
            sd.name = s.name;
            sd.targetFrame = s.targetFrame;
            sd.frameCount = s.frameCount;

            const int end = std::min(frames, s.targetFrame + s.frameCount);
            for (int f = s.targetFrame; f < end; ++f) {
                for (int i = 0; i < nb; ++i) {
                    const Mat3x4 ma = a.boneMatrix(f, i);
                    const Mat3x4 mb = b.boneMatrix(f + opt.frameOffsetB, i);
                    const double dr = rotationDiff(ma, mb);
                    const double dt = translationDiff(ma, mb);
                    if (dr > opt.toleranceRotationDeg || dt > opt.toleranceTranslation) ++sd.outliers;
                    sd.maxDeviation = std::max(sd.maxDeviation, dt);
                    sd.maxRotationDeg = std::max(sd.maxRotationDeg, dr);
                }
            }
            if (sd.outliers) res.perSequence.push_back(std::move(sd));
        }
        std::sort(res.perSequence.begin(), res.perSequence.end(),
                  [](const SequenceDiff& x, const SequenceDiff& y) {
                      return x.maxDeviation > y.maxDeviation;
                  });
    }

    std::sort(res.perBone.begin(), res.perBone.end(), [](const BoneDiff& x, const BoneDiff& y) {
        return std::max(x.maxRotationDeg * 0.15, x.maxTranslation) >
               std::max(y.maxRotationDeg * 0.15, y.maxTranslation);
    });

    return res;
}

}  // namespace g2
