#include "g2/xsi_export.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <functional>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <filesystem>

namespace g2::xsiexp {
namespace {
namespace fs = std::filesystem;
}
namespace {

constexpr double kRad2Deg = 57.29577951308232;

// Axis swap between dotXSI and GLA, as in xsi_anim.cpp.
Mat3x4 axisC() {
    Mat3x4 c{};
    c.m[0][0] = 1;
    c.m[1][2] = -1;
    c.m[2][1] = 1;
    return c;
}

Mat3x4 axisCinv() {
    Mat3x4 c{};
    c.m[0][0] = 1;
    c.m[1][2] = 1;
    c.m[2][1] = -1;
    return c;
}

// Composes the rotation exactly the way the importer does.
//
// Must match eulerXYZ in xsi_anim.cpp character for character - otherwise the
// quality check below measures something other than what actually comes out
// later.
Mat3x4 composeEuler(float rxDeg, float ryDeg, float rzDeg) {
    constexpr double kDeg2Rad = 0.017453292519943295;
    const double rx = rxDeg * kDeg2Rad, ry = ryDeg * kDeg2Rad, rz = rzDeg * kDeg2Rad;
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);

    Mat3x4 o{};
    o.m[0][0] = static_cast<float>(cz * cy);
    o.m[0][1] = static_cast<float>(cz * sy * sx - sz * cx);
    o.m[0][2] = static_cast<float>(cz * sy * cx + sz * sx);
    o.m[1][0] = static_cast<float>(sz * cy);
    o.m[1][1] = static_cast<float>(sz * sy * sx + cz * cx);
    o.m[1][2] = static_cast<float>(sz * sy * cx - cz * sx);
    o.m[2][0] = static_cast<float>(-sy);
    o.m[2][1] = static_cast<float>(cy * sx);
    o.m[2][2] = static_cast<float>(cy * cx);
    return o;
}

double rotError(const Mat3x4& a, const Mat3x4& b) {
    double e = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            e = std::max(e, std::fabs(static_cast<double>(a.m[r][c]) - b.m[r][c]));
    return e;
}

// Decomposes the rotation from R = Rz(rz)·Ry(ry)·Rx(rx) - exactly the order that
// eulerXYZ in xsi_anim.cpp builds. If a different order were assumed here, the
// file would look correct but the animation would be twisted.
//
// Why the result is checked and replaced if necessary:
//
// Near gimbal lock - when cos(ry) approaches zero - rx and rz can no longer be
// cleanly separated. The usual formula still yields angles there, but after
// recomposition the matrix elements deviate by up to 0.003. That only affects
// 0.06 % of random rotations, but it matters a lot as soon as a bone is far
// from its parent bone: in JK2's _humanoid.gla, 46 of the 72 bones hang
// directly off the rib cage, and there an angular error of 0.003 over a
// one-meter lever becomes a visible offset.
//
// Therefore both valid solutions are evaluated - the usual one and the
// degenerate one with rz = 0 - and the one that recomposes better is taken.
// That can never be worse than a fixed choice.
void decomposeEuler(const Mat3x4& m, float& rxDeg, float& ryDeg, float& rzDeg) {
    const double sy = -static_cast<double>(m.m[2][0]);
    const double clamped = sy > 1.0 ? 1.0 : (sy < -1.0 ? -1.0 : sy);
    const double ry = std::asin(clamped);

    struct Cand {
        float rx, ry, rz;
    };
    Cand best{};
    double bestErr = 1e30;

    const auto tryCand = [&](double rxr, double ryr, double rzr) {
        const Cand c{static_cast<float>(rxr * kRad2Deg), static_cast<float>(ryr * kRad2Deg),
                     static_cast<float>(rzr * kRad2Deg)};
        const double e = rotError(m, composeEuler(c.rx, c.ry, c.rz));
        if (e < bestErr) {
            bestErr = e;
            best = c;
        }
    };

    // The usual solution.
    tryCand(std::atan2(static_cast<double>(m.m[2][1]), static_cast<double>(m.m[2][2])), ry,
            std::atan2(static_cast<double>(m.m[1][0]), static_cast<double>(m.m[0][0])));

    // The degenerate one: rz set to zero, everything in rx. Under true gimbal
    // lock it is exact, otherwise worse - the check decides.
    tryCand(std::atan2(-static_cast<double>(m.m[1][2]), static_cast<double>(m.m[1][1])), ry, 0.0);

    // The second solution of the arcsine: ry mirrored about pi/2.
    constexpr double kPi = 3.141592653589793;
    const double ry2 = (ry >= 0.0 ? kPi - ry : -kPi - ry);
    tryCand(std::atan2(-static_cast<double>(m.m[2][1]), -static_cast<double>(m.m[2][2])), ry2,
            std::atan2(-static_cast<double>(m.m[1][0]), -static_cast<double>(m.m[0][0])));

    rxDeg = best.rx;
    ryDeg = best.ry;
    rzDeg = best.rz;
}

// Nearest rotation to a 3x3 matrix (polar decomposition).
//
// Iteration R <- (R + R^-T)/2. Converges quadratically; six steps are enough
// for float, well beyond the required precision.
Mat3x4 nearestRotation(const Mat3x4& m) {
    double R[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r][c] = m.m[r][c];

    for (int it = 0; it < 8; ++it) {
        // Compute the inverse transpose.
        const double det =
            R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
            R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
            R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
        if (std::fabs(det) < 1e-12) break;

        double N[3][3];
        N[0][0] = (R[1][1] * R[2][2] - R[1][2] * R[2][1]) / det;
        N[0][1] = (R[1][2] * R[2][0] - R[1][0] * R[2][2]) / det;
        N[0][2] = (R[1][0] * R[2][1] - R[1][1] * R[2][0]) / det;
        N[1][0] = (R[0][2] * R[2][1] - R[0][1] * R[2][2]) / det;
        N[1][1] = (R[0][0] * R[2][2] - R[0][2] * R[2][0]) / det;
        N[1][2] = (R[0][1] * R[2][0] - R[0][0] * R[2][1]) / det;
        N[2][0] = (R[0][1] * R[1][2] - R[0][2] * R[1][1]) / det;
        N[2][1] = (R[0][2] * R[1][0] - R[0][0] * R[1][2]) / det;
        N[2][2] = (R[0][0] * R[1][1] - R[0][1] * R[1][0]) / det;

        double diff = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const double next = 0.5 * (R[r][c] + N[r][c]);
                diff = std::max(diff, std::fabs(next - R[r][c]));
                R[r][c] = next;
            }
        if (diff < 1e-12) break;
    }

    Mat3x4 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.m[r][c] = static_cast<float>(R[r][c]);
    return o;
}

Mat3x4 scaled(const Mat3x4& m, float s) {
    Mat3x4 o = m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] *= s;
    return o;
}

std::string num(double v) {
    // Six decimal places, as in Raven's files. Fewer visibly loses precision,
    // more bloats the file for no benefit.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

}  // namespace

std::optional<std::array<float, 3>> readAverageVec(const std::string& framesPath,
                                                   const std::string& sequenceOrFile) {
    std::ifstream f(framesPath);
    if (!f) return std::nullopt;

    // Compare by file name without extension, lowercased. The .frames file
    // lists full paths with drive letters; those don't match on any other
    // machine.
    auto key = [](std::string v) {
        const std::size_t slash = v.find_last_of("/\\");
        if (slash != std::string::npos) v = v.substr(slash + 1);
        const std::size_t dot = v.find_last_of('.');
        if (dot != std::string::npos) v = v.substr(0, dot);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = key(sequenceOrFile);

    std::string line, current;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t q = line.find('"');
        if (q == std::string::npos) {
            if (line.find('{') == std::string::npos && line.find('}') == std::string::npos &&
                !line.empty())
                current = key(line);
            continue;
        }
        if (current != want) continue;
        if (line.find("averagevec") == std::string::npos) continue;

        // Second pair of quotes on the line: "averagevec" "x y z".
        //
        // Don't search from a fixed offset - that lands exactly on the closing
        // quote of the key, and what gets read is then the tab in between.
        std::size_t p1 = line.find('"');
        std::size_t p2 = line.find('"', p1 + 1);
        std::size_t p3 = line.find('"', p2 + 1);
        std::size_t p4 = line.find('"', p3 + 1);
        if (p3 == std::string::npos || p4 == std::string::npos) continue;
        std::istringstream is(line.substr(p3 + 1, p4 - p3 - 1));
        std::array<float, 3> v{};
        if (!(is >> v[0] >> v[1] >> v[2])) continue;
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) return std::nullopt;
        return v;
    }
    return std::nullopt;
}

std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla) {
    if (gla.numFrames <= 0 || gla.skeleton.bones.empty()) return std::nullopt;

    // Find the most frequent value per axis. The offset is constant, the root
    // motion is not - so the most frequent value is the offset.
    std::array<float, 3> best{};
    for (int axis = 0; axis < 3; ++axis) {
        std::map<int, int> hist;   // rounded to quarter units
        const int step = gla.numFrames > 4000 ? gla.numFrames / 2000 : 1;
        int total = 0;
        for (int f = 0; f < gla.numFrames; f += step) {
            const float v = gla.boneMatrix(f, 0).m[axis][3];
            hist[static_cast<int>(std::lround(v * 4.0f))]++;
            ++total;
        }
        int bestKey = 0, bestCount = 0;
        for (const auto& [k, c] : hist)
            if (c > bestCount) { bestCount = c; bestKey = k; }

        // Only take it if the value really dominates.
        best[static_cast<std::size_t>(axis)] =
            (bestCount * 10 >= total * 8) ? -static_cast<float>(bestKey) / 4.0f : 0.0f;
    }
    if (best[0] == 0.0f && best[1] == 0.0f && best[2] == 0.0f) return std::nullopt;
    return best;
}

float residualMotion(const MdxaFile& gla, const Sequence& seq) {
    const auto& sk = gla.skeleton;
    const int n = static_cast<int>(sk.bones.size());
    if (n == 0 || seq.frameCount < 2) return 0.0f;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames) return 0.0f;

    // Ignore case: custom models often spell the motion bone "motion" instead
    // of "Motion".
    int mi = -1;
    for (int b = 0; b < n; ++b) {
        std::string nm = sk.bones[static_cast<std::size_t>(b)].name;
        std::transform(nm.begin(), nm.end(), nm.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (nm == "motion") mi = b;
    }
    if (mi < 0) return 0.0f;

    const auto a = boneWorldMatrices(gla, seq.startFrame);
    const auto b = boneWorldMatrices(gla, seq.startFrame + seq.frameCount - 1);
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r)
        worst = std::max(worst, std::fabs(b[static_cast<std::size_t>(mi)].m[r][3] -
                                          a[static_cast<std::size_t>(mi)].m[r][3]));
    return worst;
}

Grouping groupSequences(const std::vector<CfgSequence>& cfg) {
    Grouping out;
    if (cfg.empty()) return out;

    std::vector<CfgSequence> sorted = cfg;
    std::sort(sorted.begin(), sorted.end(), [](const CfgSequence& a, const CfgSequence& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.count > b.count;   // the longest range becomes master
    });

    int end = -1;
    for (const auto& q : sorted) {
        if (q.start >= end) {
            out.masters.push_back({q, {}});
            end = q.start + q.count;
        } else if (q.start + q.count <= end) {
            out.masters.back().inside.push_back(q);
        } else {
            // A partial overlap cannot be expressed as -additional. It never
            // occurs in Raven's files; if it does, better to report it than to
            // silently bend it.
            ++out.partial;
        }
    }
    return out;
}

std::optional<std::array<float, 3>> detectRootMotion(const MdxaFile& gla, const Sequence& seq) {
    if (seq.frameCount < 2) return std::nullopt;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames) return std::nullopt;
    if (gla.skeleton.bones.empty()) return std::nullopt;

    const Mat3x4 a = gla.boneMatrix(seq.startFrame, 0);
    const Mat3x4 b = gla.boneMatrix(seq.startFrame + seq.frameCount - 1, 0);
    const float steps = static_cast<float>(seq.frameCount - 1);

    std::array<float, 3> v{};
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        v[static_cast<std::size_t>(k)] = -(b.m[k][3] - a.m[k][3]) / steps;
        if (std::fabs(v[static_cast<std::size_t>(k)]) > 1e-4f) any = true;
    }
    return any ? std::optional<std::array<float, 3>>(v) : std::nullopt;
}

car::Script buildScript(const Grouping& g, const std::string& xsiPrefix,
                        const std::optional<std::array<float, 3>>& origin, float scale,
                        bool keepMotion, const std::string& makeSkel) {
    car::Script sc;
    car::addGrabFrame(sc);

    // $scale and $keepmotion appear in Raven's scripts and belong here as
    // well.
    //
    // What can NOT be reconstructed, on the other hand, is $pcj - the list of
    // bones the engine may rotate itself at runtime. It exists only in the
    // .car and leaves no trace in the GLA: in Raven's _humanoid.gla only two
    // of 53 bones carry any flag at all. Anyone replacing an existing .car
    // should copy the $pcj block from there.
    if (scale > 0.0f) {
        car::Statement st;
        st.cmd = car::Cmd::Scale;
        st.raw = "$scale";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(scale));
        st.args.push_back(buf);
        sc.statements.insert(sc.statements.begin(), std::move(st));
        sc.scale = scale;
    }
    if (keepMotion) {
        car::Statement st;
        st.cmd = car::Cmd::KeepMotion;
        st.raw = "$keepmotion";
        sc.statements.insert(sc.statements.begin() + (scale > 0.0f ? 1 : 0), std::move(st));
        sc.keepMotion = true;
    }

    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };

    for (const auto& m : g.masters) {
        car::GrabDirective gd;
        gd.file = xsiPrefix + lower(m.self.name) + ".xsi";
        gd.enumName = m.self.name;
        gd.loop = m.self.loop;
        gd.frameSpeed = m.self.fps;
        for (const auto& q : m.inside) {
            car::GrabDirective::Additional a;
            a.targetOffset = q.start - m.self.start;
            a.frameCount = q.count;
            a.loopFrame = q.loop;
            a.frameSpeed = q.fps;
            a.name = q.name;
            gd.additional.push_back(std::move(a));
        }
        sc.grabs.push_back(std::move(gd));
    }

    car::ConvertDirective cv;
    cv.noAsk = true;
    cv.root = xsiPrefix + "root";

    // The skeleton path comes from the GLA header - it holds exactly the value
    // Carcass was given as -makeskel back then. Only if it is missing is it
    // derived from the prefix.
    //
    // Without a prefix, no leading slash may be produced: "/_humanoid" is an
    // absolute path and will not be found.
    if (!makeSkel.empty()) {
        cv.makeSkel = makeSkel;
    } else {
        const std::string dir = fs::path(xsiPrefix + "x").parent_path().generic_string();
        cv.makeSkel = dir.empty() ? "_humanoid" : dir + "/_humanoid";
    }

    if (origin) {
        // Avoid negative zero: the estimate yields -0.0, and
        // "-origin -0 -0 24" looks like an error even though it isn't one.
        const auto clean = [](float v) { return v == 0.0f ? 0.0 : static_cast<double>(v); };
        cv.origin = std::array<double, 3>{clean((*origin)[0]), clean((*origin)[1]),
                                          clean((*origin)[2])};
    }
    sc.convert = cv;
    return sc;
}

std::string exportSequence(const MdxaFile& gla, const Sequence& seq,
                           const ExportOptions& opt) {
    const auto& skel = gla.skeleton;
    const int numBones = static_cast<int>(skel.bones.size());
    if (numBones == 0) throw std::runtime_error("GLA ohne Skelett");
    if (seq.frameCount <= 0) throw std::runtime_error("Sequenz ohne Frames");

    // A single-frame sequence is written as TWO identical frames.
    //
    // Carcass rejects an .xsi with only one frame:
    //
    //   XSI file-format doesn't support 1-frames files 100% legally,
    //   re-export this file please!
    //
    // Accordingly, all 58 of Raven's own single-frame sequences are
    // "-additional" subranges of longer files; there are no standalone
    // single-frame files there.
    //
    // The second frame is a copy of the first, so the animation is unchanged.
    // Anyone generating the script via "Alles + .car" gets -additional anyway
    // and never notices; anyone exporting a single sequence can build it with
    // this file instead of not at all.
    const int outFrames = seq.frameCount == 1 ? 2 : seq.frameCount;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames)
        throw std::runtime_error("Sequenzbereich liegt ausserhalb der GLA");

    const float scale = opt.scale > 0.0f ? opt.scale : 1.0f;
    const Mat3x4 C = axisC();
    const Mat3x4 Cinv = axisCinv();

    // Bind poses and their inverses.
    std::vector<Mat3x4> B(static_cast<std::size_t>(numBones));
    std::vector<Mat3x4> Binv(static_cast<std::size_t>(numBones));
    for (int b = 0; b < numBones; ++b) {
        B[static_cast<std::size_t>(b)] = skel.bones[static_cast<std::size_t>(b)].basePose;
        Binv[static_cast<std::size_t>(b)] = affineInverse(B[static_cast<std::size_t>(b)]);
    }

    // Local transforms per frame and bone, in dotXSI convention.
    struct Key {
        float t[3];
        float r[3];
        float s[3];
    };
    std::vector<std::vector<Key>> keys(static_cast<std::size_t>(numBones),
                                       std::vector<Key>(static_cast<std::size_t>(outFrames)));

    // Topological order: parents before children.
    //
    // Index order is NOT enough. In Raven's _humanoid.gla, eight bones have
    // their parent bone after them - "ceyebrow", "jaw" and others hang off
    // bone 52. Computing in index order, X[parent] is still uninitialized for
    // these bones, and everything below them becomes garbage.
    std::vector<int> topo;
    topo.reserve(static_cast<std::size_t>(numBones));
    {
        std::vector<char> done(static_cast<std::size_t>(numBones), 0);
        bool progress = true;
        while (progress && topo.size() < static_cast<std::size_t>(numBones)) {
            progress = false;
            for (int b = 0; b < numBones; ++b) {
                const auto bu = static_cast<std::size_t>(b);
                if (done[bu]) continue;
                const int p = skel.bones[bu].parent;
                if (p >= 0 && !done[static_cast<std::size_t>(p)]) continue;
                topo.push_back(b);
                done[bu] = 1;
                progress = true;
            }
        }
        if (topo.size() != static_cast<std::size_t>(numBones))
            throw std::runtime_error("Skelett enthaelt einen Zyklus");
    }

    for (int f = 0; f < outFrames; ++f) {
        // For a single-frame sequence, both output frames point to the same
        // source frame.
        const int src = seq.startFrame + std::min(f, seq.frameCount - 1);

        // Step 1: recover X from A. Parents first, because X(b) depends on
        // X(parent).
        std::vector<Mat3x4> X(static_cast<std::size_t>(numBones));
        for (const int b : topo) {
            const auto bu = static_cast<std::size_t>(b);
            Mat3x4 A = gla.boneMatrix(src, b);
            const int p = skel.bones[bu].parent;

            if (p < 0) {
                // Undo what the build did to the root bone. Order reversed
                // relative to the forward direction:
                //   forward:  A = X·B^-1;  A -= origin;  A += ramp·t
                //   backward: A += origin;  A -= ramp·t;  X = A·B
                // With ramp·t = -averagevec·f the minus turns into a plus.
                if (opt.origin) {
                    A.m[0][3] += (*opt.origin)[0];
                    A.m[1][3] += (*opt.origin)[1];
                    A.m[2][3] += (*opt.origin)[2];
                }
                if (opt.rootMotionPerFrame) {
                    const float ff = static_cast<float>(f);
                    A.m[0][3] += (*opt.rootMotionPerFrame)[0] * ff;
                    A.m[1][3] += (*opt.rootMotionPerFrame)[1] * ff;
                    A.m[2][3] += (*opt.rootMotionPerFrame)[2] * ff;
                }
                X[bu] = mul(A, B[bu]);
            } else {
                const auto pu = static_cast<std::size_t>(p);
                X[bu] = mul(mul(mul(X[pu], Binv[pu]), A), B[bu]);
            }
        }

        // Step 2: back into dotXSI space and make local.
        std::vector<Mat3x4> wx(static_cast<std::size_t>(numBones));
        for (int b = 0; b < numBones; ++b) {
            const auto bu = static_cast<std::size_t>(b);
            wx[bu] = mul(mul(Cinv, scaled(X[bu], 1.0f / scale)), C);
        }

        for (int b = 0; b < numBones; ++b) {
            const auto bu = static_cast<std::size_t>(b);
            const int p = skel.bones[bu].parent;
            const Mat3x4 local =
                p < 0 ? wx[bu] : mul(affineInverse(wx[static_cast<std::size_t>(p)]), wx[bu]);

            Key& k = keys[bu][static_cast<std::size_t>(f)];
            k.t[0] = local.m[0][3];
            k.t[1] = local.m[1][3];
            k.t[2] = local.m[2][3];

            // Split off the scale BEFORE decomposing the rotation.
            //
            // Not every local transform is scale-preserving: the face bones
            // under "face" carry a scale of 1.087 in Raven's skeleton. If it
            // is discarded, re-importing yields exactly the reciprocal -
            // 0.92 instead of 1.0 - and the face bones end up misplaced.
            //
            // The importer composes m = R·diag(s), so the column lengths are
            // exactly the scale.
            //
            // Merely normalizing the columns is not enough: if they are not
            // exactly perpendicular to each other - and Raven's bind poses
            // aren't quite - the result is not a rotation, and the Euler
            // decomposition suffers for it. With JK2's flat hierarchy, where
            // 46 bones hang directly off the rib cage, this small angular
            // error over the long lever turns into a visible offset at the
            // fingertip.
            //
            // The polar decomposition instead yields the rotation closest to
            // the matrix. The shear itself cannot be represented - the
            // importer builds m = R·diag(s) - but the remaining error is made
            // as small as possible.
            const Mat3x4 rot = nearestRotation(local);
            for (int c = 0; c < 3; ++c) {
                // Measure the scale along the ROTATED axis, not the raw
                // column length.
                double proj = 0.0;
                for (int r = 0; r < 3; ++r)
                    proj += static_cast<double>(rot.m[r][c]) * local.m[r][c];
                k.s[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
            }
            decomposeEuler(rot, k.r[0], k.r[1], k.r[2]);
        }
    }

    // --- Writing ----------------------------------------------------------
    std::ostringstream o;
    o << (opt.version == ExportOptions::Version::V30 ? "xsi 0300txt 0032\n\n"
                                                     : "xsi 0350txt 0032\n\n");
    o << "SI_CoordinateSystem coord {\n  1,\n  0,\n  1,\n  0,\n  2,\n  5,\n}\n\n";

    // SI_Scene: the frame range the importer reads. The rate is truncated on
    // reading, so write it as an integer right away.
    o << "SI_Scene scene {\n  \"FRAMES\",\n  0,\n  " << (outFrames - 1) << ",\n  "
      << opt.fps << ",\n}\n\n";

    // Nested SI_Model blocks, so the hierarchy is preserved.
    std::vector<std::vector<int>> children(static_cast<std::size_t>(numBones));
    std::vector<int> roots;
    for (int b = 0; b < numBones; ++b) {
        const int p = skel.bones[static_cast<std::size_t>(b)].parent;
        if (p < 0) roots.push_back(b);
        else children[static_cast<std::size_t>(p)].push_back(b);
    }

    enum class Part { Rot, Trans, Scale };
    const auto curve = [&](std::ostringstream& s, const std::string& bone, const char* channel,
                           int component, Part part, const std::vector<Key>& kk,
                           const std::string& ind) {
        // v3.0 names the templates, v3.5 doesn't. The content is identical -
        // the bone name is the first value in the block anyway.
        if (opt.version == ExportOptions::Version::V30)
            s << ind << "SI_FCurve " << bone << "-" << channel << " {\n";
        else
            s << ind << "SI_FCurve {\n";
        s << ind << "  \"" << bone << "\",\n";
        s << ind << "  \"" << channel << "\",\n";
        s << ind << "  \"CONSTANT\",\n";
        s << ind << "  1,\n";
        s << ind << "  1,\n";
        s << ind << "  " << kk.size() << ",\n";
        for (std::size_t f = 0; f < kk.size(); ++f) {
            const float v = part == Part::Rot     ? kk[f].r[component]
                            : part == Part::Trans ? kk[f].t[component]
                                                  : kk[f].s[component];
            s << ind << "  " << f << ", " << num(v) << ",\n";
        }
        s << ind << "}\n";
    };

    std::function<void(int, const std::string&)> writeBone = [&](int b, const std::string& ind) {
        const auto bu = static_cast<std::size_t>(b);
        const std::string& name = skel.bones[bu].name;
        const auto& kk = keys[bu];

        o << ind << "SI_Model MDL-" << name << " {\n";

        // SRT as rest pose: the first frame. Otherwise a bone without a curve
        // would sit at identity instead of in its place.
        // SI_Transform is the REST POSE, not frame 0 of the animation.
        //
        // Carcass reads exactly this block as the bind pose, chains it through
        // the hierarchy and compares the result with the target skeleton.
        // When this held the animated pose of frame 0, it reported
        // "Basepose for bone ... differs" for every bone - for lower_lumbar by
        // about 41.35, which is exactly the Z height of the bind pose.
        //
        // Our own importer never used these values because it evaluates the
        // FCurves; the bug therefore went unnoticed until someone ran the file
        // through Raven's Carcass.
        {
            const Mat3x4& Bb = skel.bones[bu].basePose;
            const int par = skel.bones[bu].parent;
            // Local relative to the parent bone, then into XSI coordinates.
            const Mat3x4 localBind =
                par < 0 ? Bb : mul(affineInverse(skel.bones[static_cast<std::size_t>(par)].basePose), Bb);
            // Do NOT divide by scale.
            //
            // The skeleton scale is contained in every bind pose; when forming
            // the LOCAL pose - parent^-1 times child - it cancels out. Dividing
            // again here writes scale 1.5625 instead of 1.0 and translation
            // 14.06 instead of 9.0.
            //
            // Cross-checked against Raven's own Both_forcelandleft1.xsi: it
            // has exactly scale 1.0 and translation 9.0 for lower_lumbar -
            // precisely what the local bind pose yields.
            // For the ROOT, divide out the skeleton scale.
            //
            // It is contained in every bind pose. When forming the local pose -
            // parent^-1 times child - it cancels out, but the root has no
            // parent bone: there it remains.
            //
            // Carcass itself multiplies the rest pose it reads by the scale. If
            // it stayed in, the result was 0.64 times 0.64 = 0.4096 where 0.64
            // was expected - and the message read "Basepose for bone
            // model_root differs by 0.230400", i.e. exactly 0.64 minus 0.4096.
            // Since the whole chain builds on it, EVERY bone was wrong after
            // that.
            //
            // Raven's own files confirm it: they have scale 1.0 for pelvis,
            // not 0.64.
            Mat3x4 localFixed = localBind;
            if (par < 0 && scale > 0.0f)
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) localFixed.m[r][c] /= scale;

            const Mat3x4 wx = mul(mul(Cinv, localFixed), C);

            float rs[3], rr[3], rt[3];
            const Mat3x4 rot = nearestRotation(wx);
            for (int c = 0; c < 3; ++c) {
                double proj = 0.0;
                for (int r = 0; r < 3; ++r) proj += static_cast<double>(rot.m[r][c]) * wx.m[r][c];
                rs[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
            }
            decomposeEuler(rot, rr[0], rr[1], rr[2]);
            for (int c = 0; c < 3; ++c) rt[c] = wx.m[c][3];

            // BASEPOSE: this is the block Carcass reads as the bind pose.
            //
            // Raven's root.xsi has TWO transform blocks per bone - "SRT-" for
            // the pose and "BASEPOSE-" for the bind pose. We wrote only the
            // first, and that's why Carcass reported "Basepose ... differs"
            // for every bone: it found none and compared against whatever
            // happened to be there.
            //
            // The content is the WORLD bind pose in dotXSI coordinates, divided
            // by the skeleton scale. Cross-checked against Raven's own file:
            // for lfemurYZ it has 5.643987, 55.604065, 0.322196 - and that is
            // exactly what the calculation yields.
            // The ROOT gets no BASEPOSE block.
            //
            // Raven's files consistently have exactly THREE more SRT blocks
            // than BASEPOSE blocks - in root.xsi (277/274) as in every
            // animation file (98/95). Checked which ones are missing:
            // model_root, mesh_root and skeleton_root, i.e. the root nodes.
            //
            // We wrote one for model_root, and Carcass chained it with the
            // chain below it - hence "non-uniform scaling" with values around
            // 0.4096, i.e. 0.64 squared.
            //
            // mesh_root and skeleton_root don't exist on our side; the GLA
            // doesn't know them.
            if (opt.basePose != ExportOptions::BasePose::None && par >= 0) {
                // World: the world pose, as in Raven's files.
                // Local:  relative to the parent bone - if Carcass chains
                //         itself, the world pose would be scaled twice.
                Mat3x4 world = opt.basePose == ExportOptions::BasePose::Local ? localBind : Bb;
                if (scale > 0.0f) {
                    // For the local pose the scale already cancels out,
                    // except at the root.
                    const bool teilen =
                        opt.basePose == ExportOptions::BasePose::World || par < 0;
                    if (teilen)
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c) world.m[r][c] /= scale;
                }
                const Mat3x4 bx = mul(mul(Cinv, world), C);

                float bs[3], br[3], bt[3];
                const Mat3x4 brot = nearestRotation(bx);
                for (int c = 0; c < 3; ++c) {
                    double proj = 0.0;
                    for (int r = 0; r < 3; ++r)
                        proj += static_cast<double>(brot.m[r][c]) * bx.m[r][c];
                    bs[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
                }
                decomposeEuler(brot, br[0], br[1], br[2]);
                for (int c = 0; c < 3; ++c) bt[c] = bx.m[c][3];

                o << ind << "  SI_Transform BASEPOSE-" << name << " {\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(bs[k]) << ",\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(br[k]) << ",\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(bt[k]) << ",\n";
                o << ind << "  }\n";
            }

            o << ind << "  SI_Transform SRT-" << name << " {\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rs[k]) << ",\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rr[k]) << ",\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rt[k]) << ",\n";
            o << ind << "  }\n";
        }

        // SI_FCurve MUST be a direct child of SI_Model.
        //
        // The importer's findAll searches only the immediate children. An
        // enclosing SI_Animation block makes the curves invisible: zero
        // channels arrived, every bone stayed at its SRT rest pose, and that's
        // why frame 0 of all frames was always right and everything after it
        // wasn't. Raven's own files likewise place the curves directly under
        // SI_Model.
        const char* scCh[3] = {"SCALING-X", "SCALING-Y", "SCALING-Z"};
        const char* rotCh[3] = {"ROTATION-X", "ROTATION-Y", "ROTATION-Z"};
        const char* trCh[3] = {"TRANSLATION-X", "TRANSLATION-Y", "TRANSLATION-Z"};
        for (int k = 0; k < 3; ++k) curve(o, name, scCh[k], k, Part::Scale, kk, ind + "  ");
        for (int k = 0; k < 3; ++k) curve(o, name, rotCh[k], k, Part::Rot, kk, ind + "  ");
        for (int k = 0; k < 3; ++k) curve(o, name, trCh[k], k, Part::Trans, kk, ind + "  ");

        for (const int c : children[bu]) writeBone(c, ind + "  ");
        o << ind << "}\n";
    };

    for (const int r : roots) writeBone(r, "");
    return o.str();
}

}  // namespace g2::xsiexp
