#include "g2/xsi_anim.h"

#include "g2/mdxa.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace g2::xsi {
namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

// Axis convention Y-up (SoftImage) to Z-up (Quake/Ghoul2).
// Determined empirically and verified against all 53 bones of the real
// _humanoid.gla.
Mat3x4 axisChange() {
    Mat3x4 c{};
    c.m[0][0] = 1.0f;
    c.m[1][2] = -1.0f;
    c.m[2][1] = 1.0f;
    return c;
}

Mat3x4 axisChangeInv() {
    Mat3x4 c{};
    c.m[0][0] = 1.0f;
    c.m[1][2] = 1.0f;
    c.m[2][1] = -1.0f;
    return c;
}


Mat3x4 scaled(const Mat3x4& m, float s) {
    Mat3x4 o = m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] *= s;
    return o;
}

// Rotation from Euler angles in degrees, order XYZ extrinsic,
// i.e. Rz * Ry * Rx. Also determined empirically.
Mat3x4 eulerXYZ(float rxDeg, float ryDeg, float rzDeg) {
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

std::string shortName(const std::string& full) {
    const std::size_t dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

// Recursively collect all SI_Model templates, recording the nesting as the
// parent relationship.
void collectModels(const Template& t, int parent, AnimFile& out) {
    int self = parent;
    if (t.type == "SI_Model") {
        AnimNode n;
        // The instance name looks like "MDL-<rig>.<bone>".
        std::string nm = t.name;
        if (nm.rfind("MDL-", 0) == 0) nm = nm.substr(4);
        n.name = shortName(nm);
        n.parent = parent;
        self = static_cast<int>(out.nodes.size());
        out.nodes.push_back(std::move(n));

        // SI_Transform SRT-<name>: static local transform.
        // BASEPOSE-<name> is something else (absolute bind pose) and is
        // deliberately not used here.
        for (const Template* xf : t.findAll("SI_Transform")) {
            if (xf->name.rfind("SRT-", 0) != 0) continue;
            if (xf->values.size() < 9) continue;
            auto& node = out.nodes[static_cast<std::size_t>(self)];
            bool ok = true;
            for (int k = 0; k < 9; ++k) {
                const auto d = xf->values[static_cast<std::size_t>(k)].asNumber();
                if (!d) { ok = false; break; }
                node.srt[static_cast<std::size_t>(k)] = static_cast<float>(*d);
            }
            node.hasSrt = ok;
            break;
        }

        for (const Template* fc : t.findAll("SI_FCurve")) {
            // Layout: "<target>", "<channel>", "<interpolation>", a, b, keyCount,
            //         frame, value, frame, value, ...
            if (fc->values.size() < 6) continue;
            const std::string channel = fc->values[1].text();
            const auto keyCount = fc->values[5].asInt();
            if (!keyCount || *keyCount <= 0) continue;

            auto& target = out.nodes[static_cast<std::size_t>(self)].channels[channel];
            const std::size_t first = 6;
            const std::size_t need = first + static_cast<std::size_t>(*keyCount) * 2;
            if (fc->values.size() < need) continue;

            for (std::size_t i = first; i + 1 < need; i += 2) {
                // Do NOT read the frame number with asInt(). 3ds Max writes
                // it as "1.000000", Raven as "1" - asInt() fails on the
                // decimal places, and the key would be silently lost.
                // This is exactly why every animation exported from Max
                // failed: channels were recognized, keys were not, and every
                // bone stayed in the rest pose.
                const auto f = fc->values[i].asNumber();
                const auto v = fc->values[i + 1].asNumber();
                if (!f || !v) continue;
                const int frame = static_cast<int>(std::lround(*f));
                target[frame] = static_cast<float>(*v);
                out.firstFrame = out.nodes.size() == 1 && target.size() == 1
                                     ? frame
                                     : std::min(out.firstFrame, frame);
                out.lastFrame = std::max(out.lastFrame, frame);
            }
        }
    }
    for (const auto& c : t.children) collectModels(c, self, out);
}

// Value of a channel at a frame. Between keyframes it interpolates linearly;
// Carcass relies on a key existing for every frame, but robustness costs
// nothing here.
float channelAt(const std::map<int, float>& keys, int frame, float fallback) {
    if (keys.empty()) return fallback;
    const auto exact = keys.find(frame);
    if (exact != keys.end()) return exact->second;

    const auto hi = keys.lower_bound(frame);
    if (hi == keys.begin()) return hi->second;
    if (hi == keys.end()) return std::prev(hi)->second;
    const auto lo = std::prev(hi);
    const float span = static_cast<float>(hi->first - lo->first);
    if (span <= 0.0f) return lo->second;
    const float t = static_cast<float>(frame - lo->first) / span;
    return lo->second + (hi->second - lo->second) * t;
}

}  // namespace

const AnimNode* AnimFile::find(const std::string& bone) const {
    for (const auto& n : nodes)
        if (n.name == bone) return &n;
    return nullptr;
}

int AnimFile::indexOf(const std::string& bone) const {
    // Exact first, then case-insensitive.
    //
    // Raven's own models spell the motion bone "Motion", custom models often
    // "motion" - the SBD humanoid is exactly such a case. With an exact
    // comparison only, the ramp computation doesn't find its bone, and the
    // root motion is NOT removed during the build. That only shows up in
    // game, when the character slides away while running.
    //
    // Exact first, so that with two bones differing only in case, the exact
    // match wins.
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == bone) return static_cast<int>(i);

    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = lower(bone);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (lower(nodes[i].name) == want) return static_cast<int>(i);
    return -1;
}

Mat3x4 AnimFile::localMatrix(int node, int frame) const {
    const AnimNode& n = nodes[static_cast<std::size_t>(node)];
    if (n.channels.empty() && !n.hasSrt) return Mat3x4::identity();

    // The fallback is the bone's SRT value, not neutral. Otherwise a bone
    // without an FCurve would sit at identity instead of its rest pose.
    const auto ch = [&](const char* name, std::size_t srtIndex) {
        const float fallback = n.srt[srtIndex];
        const auto it = n.channels.find(name);
        return it == n.channels.end() ? fallback : channelAt(it->second, frame, fallback);
    };

    Mat3x4 m = eulerXYZ(ch("ROTATION-X", 3), ch("ROTATION-Y", 4), ch("ROTATION-Z", 5));

    const float sx = ch("SCALING-X", 0);
    const float sy = ch("SCALING-Y", 1);
    const float sz = ch("SCALING-Z", 2);
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        for (int r = 0; r < 3; ++r) {
            m.m[r][0] *= sx;
            m.m[r][1] *= sy;
            m.m[r][2] *= sz;
        }
    }

    m.m[0][3] = ch("TRANSLATION-X", 6);
    m.m[1][3] = ch("TRANSLATION-Y", 7);
    m.m[2][3] = ch("TRANSLATION-Z", 8);
    return m;
}

std::vector<Mat3x4> AnimFile::worldMatrices(int frame) const {
    std::vector<Mat3x4> w(nodes.size());
    // nodes is built in pre-order, so parents always come first.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Mat3x4 l = localMatrix(static_cast<int>(i), frame);
        const int p = nodes[i].parent;
        w[i] = (p < 0) ? l : mul(w[static_cast<std::size_t>(p)], l);
    }
    return w;
}

AnimFile loadAnimation(const Document& doc, const std::string& sourcePath) {
    AnimFile out;
    out.sourcePath = sourcePath;
    out.firstFrame = 0;
    out.lastFrame = 0;
    bool any = false;

    // SI_Scene first: it holds the frame range and rate.
    if (const Template* scene = doc.findDeep("SI_Scene")) {
        // Layout: "FRAMES", start, end, framerate
        std::vector<double> nums;
        for (const auto& v : scene->values)
            if (const auto d = v.asNumber()) nums.push_back(*d);
        if (nums.size() >= 3) {
            out.firstFrame = static_cast<int>(nums[0]);
            out.lastFrame = static_cast<int>(nums[1]);
            out.frameRate = static_cast<float>(nums[2]);
            out.hasScene = true;
            out.sceneFirst = out.firstFrame;
            out.sceneLast = out.lastFrame;
        }
    }

    // collectModels updates the frame range while collecting the keys: the
    // start is the smallest key, the end the larger of the SI_Scene end and
    // the largest key. For every cleanly exported file this matches SI_Scene
    // (verified on all 1854 files of a _humanoid.car); if it differs,
    // sceneRangeDiffers() reports it.
    for (const auto& r : doc.roots) collectModels(r, -1, out);

    if (out.hasScene) {
        bool haveKeys = false;
        for (const auto& n : out.nodes)
            if (n.animated()) haveKeys = true;
        if (!haveKeys)
            throw std::runtime_error("Keine SI_FCurve-Daten in \"" +
                                     (sourcePath.empty() ? std::string("<memory>") : sourcePath) +
                                     "\"");
        return out;
    }

    int lo = 0, hi = 0;
    for (const auto& n : out.nodes) {
        for (const auto& [chan, keys] : n.channels) {
            (void)chan;
            for (const auto& [f, v] : keys) {
                (void)v;
                if (!any) { lo = hi = f; any = true; }
                lo = std::min(lo, f);
                hi = std::max(hi, f);
            }
        }
    }
    if (!any)
        throw std::runtime_error("Keine SI_FCurve-Daten in \"" +
                                 (sourcePath.empty() ? std::string("<memory>") : sourcePath) + "\"");
    out.firstFrame = lo;
    out.lastFrame = hi;
    return out;
}

AnimFile loadAnimationFile(const std::string& path) {
    return loadAnimation(parseFile(path), path);
}

EvalResult evaluate(const Skeleton& reference, const AnimFile& anim, const EvalOptions& opt) {
    const int numBones = static_cast<int>(reference.bones.size());
    const int numFrames = anim.frameCount();
    if (numBones == 0) throw std::runtime_error("Referenzskelett hat keine Bones");
    if (numFrames <= 0) throw std::runtime_error("Animation hat keine Frames");

    EvalResult res;
    res.frameCount = numFrames;
    res.frames.resize(numFrames, numBones);

    const Mat3x4 C = axisChange();
    const Mat3x4 Cinv = axisChangeInv();

    // Mapping reference bone -> node in the animation file.
    std::vector<int> nodeOf(static_cast<std::size_t>(numBones), -1);
    for (int b = 0; b < numBones; ++b) {
        const std::string& refName = reference.bones[static_cast<std::size_t>(b)].name;
        const auto al = opt.aliases.find(refName);
        const std::string& lookup = (al == opt.aliases.end()) ? refName : al->second;
        nodeOf[static_cast<std::size_t>(b)] = anim.indexOf(lookup);
        if (nodeOf[static_cast<std::size_t>(b)] < 0) res.missingBones.push_back(refName);
    }
    {
        std::set<std::string> refNames;
        for (const auto& b : reference.bones) {
            const auto al = opt.aliases.find(b.name);
            refNames.insert(al == opt.aliases.end() ? b.name : al->second);
        }
        for (const auto& n : anim.nodes)
            if (n.animated() && !refNames.count(n.name)) res.extraBones.push_back(n.name);
    }

    // Inverse base poses, computed once up front.
    std::vector<Mat3x4> B(static_cast<std::size_t>(numBones));
    std::vector<Mat3x4> Binv(static_cast<std::size_t>(numBones));
    for (int b = 0; b < numBones; ++b) {
        B[static_cast<std::size_t>(b)] = reference.bones[static_cast<std::size_t>(b)].basePose;
        Binv[static_cast<std::size_t>(b)] = affineInverse(B[static_cast<std::size_t>(b)]);
    }

    // Topological order over the GLA hierarchy. The bone order in the file
    // is NOT topological - in the real _humanoid.gla, bone 16 (ceyebrow) has
    // parent 52 (face).
    std::vector<int> topo;
    {
        std::vector<char> done(static_cast<std::size_t>(numBones), 0);
        topo.reserve(static_cast<std::size_t>(numBones));
        bool progress = true;
        while (static_cast<int>(topo.size()) < numBones && progress) {
            progress = false;
            for (int b = 0; b < numBones; ++b) {
                if (done[static_cast<std::size_t>(b)]) continue;
                const int p = reference.bones[static_cast<std::size_t>(b)].parent;
                if (p < 0 || done[static_cast<std::size_t>(p)]) {
                    topo.push_back(b);
                    done[static_cast<std::size_t>(b)] = 1;
                    progress = true;
                }
            }
        }
        if (static_cast<int>(topo.size()) < numBones)
            throw std::runtime_error("Referenzskelett enthaelt einen Zyklus");
    }

    // Determine the root motion as a ramp up front.
    float rootRamp[3] = {0.0f, 0.0f, 0.0f};
    bool  haveRamp = false;
    if (opt.extractRootMotion && numFrames > 1) {
        const int mi = anim.indexOf(opt.motionBone);
        if (mi >= 0) {
            const auto wFirst = anim.worldMatrices(anim.firstFrame);
            const auto wLast = anim.worldMatrices(anim.lastFrame);
            const float dx = wLast[static_cast<std::size_t>(mi)].m[0][3] -
                             wFirst[static_cast<std::size_t>(mi)].m[0][3];
            const float dy = wLast[static_cast<std::size_t>(mi)].m[1][3] -
                             wFirst[static_cast<std::size_t>(mi)].m[1][3];
            const float dz = wLast[static_cast<std::size_t>(mi)].m[2][3] -
                             wFirst[static_cast<std::size_t>(mi)].m[2][3];
            // Axis swap as everywhere else (x, -z, y), then negated.
            rootRamp[0] = -opt.scale * dx;
            rootRamp[1] = -opt.scale * (-dz);
            rootRamp[2] = -opt.scale * dy;
            haveRamp = (rootRamp[0] != 0.0f || rootRamp[1] != 0.0f || rootRamp[2] != 0.0f);
            for (int k = 0; k < 3; ++k) res.rootMotion[k] = rootRamp[k];
        }
    }

    for (int f = 0; f < numFrames; ++f) {
        const int srcFrame = anim.firstFrame + f;
        const std::vector<Mat3x4> wx = anim.worldMatrices(srcFrame);

        // World poses in GLA space.
        //
        // Missing bones keep their bind-pose relationship to the parent bone:
        //     X(b) = X(parent) * B(parent)^-1 * B(b)
        // This makes A(b) exactly the identity matrix, which is precisely
        // what Carcass writes - in the real _humanoid.gla the non-animated
        // bone "face" has the identity matrix.
        //
        // The obvious fallback X(b) = B(b) ("world rest pose") is wrong as
        // soon as the parent bone is animated: the bone would then stay put
        // in space instead of following the head.
        std::vector<Mat3x4> X(static_cast<std::size_t>(numBones));
        for (int b : topo) {
            const auto bu = static_cast<std::size_t>(b);
            const int nd = nodeOf[bu];
            if (nd >= 0) {
                X[bu] = scaled(mul(mul(C, wx[static_cast<std::size_t>(nd)]), Cinv), opt.scale);
                continue;
            }
            const int p = reference.bones[bu].parent;
            if (p < 0) {
                X[bu] = B[bu];
            } else {
                const auto pu = static_cast<std::size_t>(p);
                X[bu] = mul(mul(X[pu], Binv[pu]), B[bu]);
            }
        }

        for (int b = 0; b < numBones; ++b) {
            const int p = reference.bones[static_cast<std::size_t>(b)].parent;
            Mat3x4 A;
            if (p < 0) {
                A = mul(X[static_cast<std::size_t>(b)], Binv[static_cast<std::size_t>(b)]);
                if (opt.origin) {
                    // -origin shifts the model; in the real _humanoid.gla,
                    // model_root has (0,0,-24) for "-origin 0 0 24".
                    A.m[0][3] -= (*opt.origin)[0];
                    A.m[1][3] -= (*opt.origin)[1];
                    A.m[2][3] -= (*opt.origin)[2];
                }
                if (haveRamp) {
                    const float t = static_cast<float>(f) / static_cast<float>(numFrames - 1);
                    A.m[0][3] += rootRamp[0] * t;
                    A.m[1][3] += rootRamp[1] * t;
                    A.m[2][3] += rootRamp[2] * t;
                }
            } else {
                const auto pu = static_cast<std::size_t>(p);
                A = mul(mul(mul(B[pu], affineInverse(X[pu])), X[static_cast<std::size_t>(b)]),
                        Binv[static_cast<std::size_t>(b)]);
            }
            res.frames.at(f, b) = A;
        }
    }
    return res;
}

ConcatResult concatenate(const Skeleton& reference,
                         const std::vector<std::pair<std::string, AnimFile>>& named,
                         const EvalOptions& opt) {
    ConcatResult out;
    const int numBones = static_cast<int>(reference.bones.size());
    out.frames.numBones = numBones;

    int cursor = 0;
    for (const auto& [name, anim] : named) {
        const EvalResult r = evaluate(reference, anim, opt);

        out.frames.matrices.insert(out.frames.matrices.end(), r.frames.matrices.begin(),
                                   r.frames.matrices.end());
        out.sequences.push_back({name, cursor, r.frameCount, anim.sourcePath});
        cursor += r.frameCount;

        if (opt.warnMissingBones && !r.missingBones.empty()) {
            std::string w = name + ": " + std::to_string(r.missingBones.size()) +
                            " Bone(s) nicht animiert, bleiben in Ruhepose (";
            for (std::size_t i = 0; i < r.missingBones.size() && i < 4; ++i) {
                if (i) w += ", ";
                w += r.missingBones[i];
            }
            if (r.missingBones.size() > 4) w += ", ...";
            out.warnings.push_back(w + ")");
        }
    }
    return out;
}

}  // namespace g2::xsi
