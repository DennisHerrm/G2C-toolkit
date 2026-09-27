#include "g2/mdxa.h"

#include "g2/bytebuf.h"
#include "g2/parallel.h"

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace g2 {
namespace {

struct CompBoneHash {
    std::size_t operator()(const fmt::CompQuatBone& b) const noexcept {
        // FNV-1a over 14 bytes.
        std::size_t h = 1469598103934665603ull;
        for (unsigned char c : b.comp) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }
};

struct CompBoneEq {
    bool operator()(const fmt::CompQuatBone& a, const fmt::CompQuatBone& b) const noexcept {
        return std::memcmp(a.comp, b.comp, sizeof(a.comp)) == 0;
    }
};

std::int32_t rdI32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                     (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) |
                                     (static_cast<std::uint32_t>(p[3]) << 24));
}

float rdF32(const std::uint8_t* p) {
    const std::uint32_t bits = static_cast<std::uint32_t>(rdI32(p));
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::string rdName(const std::uint8_t* p, std::size_t width) {
    std::size_t n = 0;
    while (n < width && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

void requireSize(const std::vector<std::uint8_t>& d, std::size_t off, std::size_t n,
                 const char* what) {
    // Written to avoid overflow: with a nonsensical offset, off + n can wrap
    // past the end of size_t and then look small.
    if (off > d.size() || n > d.size() - off)
        throw std::runtime_error(std::string("GLA abgeschnitten beim Lesen von ") + what);
}

// Offsets from the file header are signed. A negative value converted to
// size_t becomes huge - and small again once a length is added. So check
// each one individually before use.
std::size_t checkedOffset(std::int32_t v, const char* what) {
    if (v < 0)
        throw std::runtime_error(std::string("GLA beschaedigt: negativer Offset bei ") + what);
    return static_cast<std::size_t>(v);
}

}  // namespace

std::vector<std::vector<int>> Skeleton::buildChildLists() const {
    std::vector<std::vector<int>> kids(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int p = bones[i].parent;
        if (p >= 0 && static_cast<std::size_t>(p) < bones.size())
            kids[static_cast<std::size_t>(p)].push_back(static_cast<int>(i));
    }
    return kids;
}

std::vector<std::string> Skeleton::validate() const {
    std::vector<std::string> errs;
    if (bones.empty()) errs.push_back("Skelett hat keine Bones");

    std::map<std::string, int> seen;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const Bone& b = bones[i];
        if (b.name.empty()) {
            errs.push_back("Bone " + std::to_string(i) + " hat keinen Namen");
        } else if (b.name.size() >= fmt::kMaxQPath) {
            errs.push_back("Bone-Name zu lang: \"" + b.name + "\"");
        }
        auto [it, fresh] = seen.emplace(b.name, static_cast<int>(i));
        if (!fresh)
            errs.push_back("Bone \"" + b.name + "\" existiert mehrfach (Index " +
                           std::to_string(it->second) + " und " + std::to_string(i) + ")");
        if (b.parent >= static_cast<int>(bones.size()) || b.parent < -1)
            errs.push_back("Bone \"" + b.name + "\" hat ungueltigen Parent-Index " +
                           std::to_string(b.parent));
        if (b.parent == static_cast<int>(i))
            errs.push_back("Bone \"" + b.name + "\" ist sein eigener Parent");
    }

    // Find cycles. Carcass doesn't check for them and then runs into an
    // endless loop or produces a broken skeleton.
    for (std::size_t i = 0; i < bones.size() && errs.size() < 64; ++i) {
        int cur = bones[i].parent;
        std::size_t steps = 0;
        while (cur >= 0 && static_cast<std::size_t>(cur) < bones.size()) {
            if (static_cast<std::size_t>(cur) == i) {
                errs.push_back("Zyklus in der Bone-Hierarchie bei \"" + bones[i].name + "\"");
                break;
            }
            if (++steps > bones.size()) break;
            cur = bones[static_cast<std::size_t>(cur)].parent;
        }
    }
    return errs;
}

void AnimationFrames::resize(int frames, int bones) {
    numBones = bones;
    matrices.assign(static_cast<std::size_t>(frames) * bones, Mat3x4::identity());
}

Mat3x4 mul(const Mat3x4& a, const Mat3x4& b) {
    Mat3x4 o{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 3; ++k) v += a.m[r][k] * b.m[k][c];
            o.m[r][c] = v;
        }
        float t = a.m[r][3];
        for (int k = 0; k < 3; ++k) t += a.m[r][k] * b.m[k][3];
        o.m[r][3] = t;
    }
    return o;
}

Mat3x4 affineInverse(const Mat3x4& m) {
    const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
    const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
    const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];

    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f)
        throw std::runtime_error("Basispose ist singulaer und nicht invertierbar");
    const float id = 1.0f / det;

    Mat3x4 out;
    out.m[0][0] = (e * i - f * h) * id;
    out.m[0][1] = (c * h - b * i) * id;
    out.m[0][2] = (b * f - c * e) * id;
    out.m[1][0] = (f * g - d * i) * id;
    out.m[1][1] = (a * i - c * g) * id;
    out.m[1][2] = (c * d - a * f) * id;
    out.m[2][0] = (d * h - e * g) * id;
    out.m[2][1] = (b * g - a * h) * id;
    out.m[2][2] = (a * e - b * d) * id;

    for (int r = 0; r < 3; ++r) {
        float t = 0.0f;
        for (int k = 0; k < 3; ++k) t += out.m[r][k] * m.m[k][3];
        out.m[r][3] = -t;
    }
    return out;
}

MdxaWriteResult writeMdxa(const Skeleton& skel, const AnimationFrames& frames,
                          const MdxaWriteOptions& opt) {
    const auto errs = skel.validate();
    if (!errs.empty()) {
        std::ostringstream os;
        os << "Skelett ist ungueltig:";
        for (const auto& e : errs) os << "\n  - " << e;
        throw std::runtime_error(os.str());
    }

    const int numBones = static_cast<int>(skel.bones.size());
    if (frames.numBones != numBones)
        throw std::runtime_error("Frame-Daten haben " + std::to_string(frames.numBones) +
                                 " Bones, Skelett hat " + std::to_string(numBones));

    const int numFrames = frames.frameCount();
    if (numFrames <= 0) throw std::runtime_error("GLA braucht mindestens einen Frame");

    MdxaWriteResult result;

    // --- Build the bone pool -----------------------------------------------
    std::vector<fmt::CompQuatBone> pool;
    std::vector<std::uint32_t>     indices(static_cast<std::size_t>(numFrames) * numBones);
    std::unordered_map<fmt::CompQuatBone, std::uint32_t, CompBoneHash, CompBoneEq> lookup;

    // Phase 1: compress. Each bone instance is independent, so this spreads
    // cleanly across the cores. The candidate search costs about five times
    // as much as a plain quantization and dominates here.
    const std::size_t total = static_cast<std::size_t>(numFrames) * numBones;
    std::vector<fmt::CompQuatBone> compressed(total);

    const unsigned nThreads = opt.threads ? opt.threads : defaultThreadCount();
    std::vector<CompressStats> perThread(nThreads);

    {
        // Each thread gets its own statistics counter; without that the
        // counters would be a data race.
        //
        // The number comes from parallelForWorker. It used to live in a
        // thread_local - that outlives the call, and on a second build with
        // fewer threads the stored index pointed past the end of the array.
        parallelForWorker(
            static_cast<std::size_t>(numFrames),
            [&](std::size_t f, unsigned worker) {
                CompressStats& st = perThread[worker % perThread.size()];
                for (int b = 0; b < numBones; ++b)
                    compressed[f * static_cast<std::size_t>(numBones) + b] =
                        compressBone(frames.at(static_cast<int>(f), b), opt.compress, st);
            },
            nThreads);
    }
    for (const auto& st : perThread) {
        result.stats.quatClamped += st.quatClamped;
        result.stats.xlatClamped += st.xlatClamped;
        result.stats.nonUnitQuat += st.nonUnitQuat;
        result.stats.maxXlatSeen = std::max(result.stats.maxXlatSeen, st.maxXlatSeen);
    }

    // Phase 2: build the pool. Stays serial - the order of the entries
    // determines the file, and a parallel variant would no longer be
    // reproducible.
    pool.reserve(total / 2);
    lookup.reserve(total / 2);
    for (std::size_t i = 0; i < total; ++i) {
        const fmt::CompQuatBone& c = compressed[i];
        std::uint32_t idx;
        if (opt.dedupeBonePool) {
            auto [it, fresh] = lookup.emplace(c, static_cast<std::uint32_t>(pool.size()));
            if (fresh) pool.push_back(c);
            idx = it->second;
        } else {
            idx = static_cast<std::uint32_t>(pool.size());
            pool.push_back(c);
        }
        indices[i] = idx;
    }

    result.poolEntriesBeforeDedupe = static_cast<std::size_t>(numFrames) * numBones;
    result.poolEntries = pool.size();

    // The frame index is only 3 bytes wide. Carcass doesn't check this.
    if (pool.size() > fmt::kMaxBonePoolEntries)
        throw std::runtime_error("Bone-Pool hat " + std::to_string(pool.size()) +
                                 " Eintraege, der 24-Bit-Index erlaubt maximal " +
                                 std::to_string(fmt::kMaxBonePoolEntries));

    // --- Assemble the file ------------------------------------------------
    ByteBuf buf;
    buf.i32(static_cast<std::int32_t>(fmt::kMdxaIdent));
    buf.i32(fmt::kMdxaVersion);
    buf.fixedString(skel.name, fmt::kMaxQPath, "GLA-Name");
    buf.f32(skel.scale);
    buf.i32(numFrames);
    const std::size_t patchOfsFrames = buf.reserveI32();
    buf.i32(numBones);
    const std::size_t patchOfsCompBonePool = buf.reserveI32();
    const std::size_t patchOfsSkel = buf.reserveI32();
    const std::size_t patchOfsEnd = buf.reserveI32();

    const std::size_t headerEnd = buf.size();
    if (headerEnd != sizeof(fmt::MdxaHeader))
        throw std::logic_error("Header-Groesse stimmt nicht mit MdxaHeader ueberein");

    // Skel offset table. Values are relative to the end of the header.
    std::vector<std::size_t> patchSkelOffsets(numBones);
    for (int i = 0; i < numBones; ++i) patchSkelOffsets[i] = buf.reserveI32();

    const auto childLists = skel.buildChildLists();

    buf.patchI32(patchOfsSkel, static_cast<std::int32_t>(buf.size()));
    for (int i = 0; i < numBones; ++i) {
        buf.patchI32(patchSkelOffsets[i], static_cast<std::int32_t>(buf.size() - headerEnd));

        const Bone& b = skel.bones[static_cast<std::size_t>(i)];
        buf.fixedString(b.name, fmt::kMaxQPath, "Bone-Name");
        buf.u32(b.flags);
        buf.i32(b.parent);

        // Base pose and its inverse. The engine expects both so it doesn't
        // have to invert at runtime.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) buf.f32(b.basePose.m[r][c]);

        // A true affine inverse, NOT the transpose.
        //
        // The transpose would only be correct for an orthonormal rotation part.
        // But Carcass bakes the $scale value into the base poses - in the real
        // _humanoid that is 0.64, and the engine accordingly finds
        // 1/0.64 = 1.5625 in the inverse. Checked against the original file:
        // there M * MInv gives the identity matrix to within 5e-7, while the
        // transpose is off by 0.92.
        const Mat3x4 inv = affineInverse(b.basePose);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) buf.f32(inv.m[r][c]);

        const auto& kids = childLists[static_cast<std::size_t>(i)];
        buf.i32(static_cast<std::int32_t>(kids.size()));
        for (int k : kids) buf.i32(k);
    }

    // Frame indices, 3 bytes per (frame, bone).
    buf.patchI32(patchOfsFrames, static_cast<std::int32_t>(buf.size()));
    for (std::uint32_t idx : indices) buf.u24(idx);

    // Alignment before the pool.
    buf.alignTo(4);

    buf.patchI32(patchOfsCompBonePool, static_cast<std::int32_t>(buf.size()));
    for (const auto& c : pool) buf.raw(c.comp, sizeof(c.comp));

    buf.patchI32(patchOfsEnd, static_cast<std::int32_t>(buf.size()));
    result.data = buf.bytes();
    return result;
}

std::vector<Mat3x4> boneWorldMatrices(const MdxaFile& gla, int frame) {
    const auto& sk = gla.skeleton;
    const int n = static_cast<int>(sk.bones.size());
    std::vector<Mat3x4> X(static_cast<std::size_t>(n));
    if (n == 0 || frame < 0 || frame >= gla.numFrames) return X;

    std::vector<Mat3x4> B(static_cast<std::size_t>(n)), Bi(static_cast<std::size_t>(n));
    for (int b = 0; b < n; ++b) {
        B[static_cast<std::size_t>(b)] = sk.bones[static_cast<std::size_t>(b)].basePose;
        Bi[static_cast<std::size_t>(b)] = affineInverse(B[static_cast<std::size_t>(b)]);
    }

    // Topological order: parents before children.
    std::vector<char> done(static_cast<std::size_t>(n), 0);
    for (bool go = true; go;) {
        go = false;
        for (int b = 0; b < n; ++b) {
            const auto bu = static_cast<std::size_t>(b);
            if (done[bu]) continue;
            const int p = sk.bones[bu].parent;
            if (p >= 0 && !done[static_cast<std::size_t>(p)]) continue;
            const Mat3x4 A = gla.boneMatrix(frame, b);
            X[bu] = p < 0 ? mul(A, B[bu])
                          : mul(mul(mul(X[static_cast<std::size_t>(p)],
                                        Bi[static_cast<std::size_t>(p)]),
                                    A),
                                B[bu]);
            done[bu] = 1;
            go = true;
        }
    }
    return X;
}

MdxaFile readMdxa(const std::vector<std::uint8_t>& d) {
    requireSize(d, 0, sizeof(fmt::MdxaHeader), "Header");
    const std::uint8_t* p = d.data();

    if (static_cast<std::uint32_t>(rdI32(p)) != fmt::kMdxaIdent)
        throw std::runtime_error("Keine GLA-Datei (Ident stimmt nicht, erwartet \"2LGA\")");
    const int version = rdI32(p + 4);
    if (version != fmt::kMdxaVersion)
        throw std::runtime_error("GLA-Version " + std::to_string(version) + ", erwartet " +
                                 std::to_string(fmt::kMdxaVersion));

    MdxaFile out;
    out.skeleton.name = rdName(p + 8, fmt::kMaxQPath);
    out.skeleton.scale = rdF32(p + 8 + fmt::kMaxQPath);

    const int numFrames = rdI32(p + 76);
    const int ofsFrames = rdI32(p + 80);
    const int numBones = rdI32(p + 84);
    const int ofsCompBonePool = rdI32(p + 88);
    // ofsSkel at +92 is not needed, we go through the offset table.
    const int ofsEnd = rdI32(p + 96);

    if (numFrames < 0 || numBones < 0)
        throw std::runtime_error("GLA hat negative Frame- oder Bone-Anzahl");
    if (ofsEnd > 0 && static_cast<std::size_t>(ofsEnd) != d.size())
        throw std::runtime_error("ofsEnd (" + std::to_string(ofsEnd) +
                                 ") passt nicht zur Dateigroesse (" + std::to_string(d.size()) + ")");

    out.numFrames = numFrames;
    out.skeleton.bones.resize(static_cast<std::size_t>(numBones));

    const std::size_t headerEnd = sizeof(fmt::MdxaHeader);
    requireSize(d, headerEnd, static_cast<std::size_t>(numBones) * 4, "Skel-Offsettabelle");

    for (int i = 0; i < numBones; ++i) {
        const std::int32_t rel = rdI32(p + headerEnd + static_cast<std::size_t>(i) * 4);
        const std::size_t off = headerEnd + checkedOffset(rel, "Bone-Eintrag");
        requireSize(d, off, sizeof(fmt::MdxaSkel), "Bone-Eintrag");

        Bone& b = out.skeleton.bones[static_cast<std::size_t>(i)];
        b.name = rdName(p + off, fmt::kMaxQPath);
        b.flags = static_cast<std::uint32_t>(rdI32(p + off + 64));
        b.parent = rdI32(p + off + 68);

        // Every later access indexes with the parent: preview, evaluation,
        // export. A value outside the skeleton would read or write foreign
        // memory there.
        if (b.parent < -1 || b.parent >= numBones || b.parent == i)
            throw std::runtime_error("GLA beschaedigt: Bone \"" + b.name +
                                     "\" hat ungueltigen Parent-Index " + std::to_string(b.parent));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                b.basePose.m[r][c] = rdF32(p + off + 72 + static_cast<std::size_t>(r * 4 + c) * 4);
    }

    // Read the indices and determine the largest pool index at the same time,
    // because the pool size is not stored anywhere in the file.
    const std::size_t indexCount = static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(numBones);
    const std::size_t framesAt = checkedOffset(ofsFrames, "Frame-Indizes");
    if (indexCount > d.size() / 3)
        throw std::runtime_error("GLA abgeschnitten beim Lesen von Frame-Indizes");
    requireSize(d, framesAt, indexCount * 3, "Frame-Indizes");
    out.indices.resize(indexCount);
    std::uint32_t maxIndex = 0;
    for (std::size_t i = 0; i < indexCount; ++i) {
        const std::uint32_t v = fmt::readIndex24(p + framesAt + i * 3);
        out.indices[i] = v;
        maxIndex = std::max(maxIndex, v);
    }

    const std::size_t poolCount = indexCount ? maxIndex + 1u : 0u;
    const std::size_t poolAt = checkedOffset(ofsCompBonePool, "Bone-Pool");
    requireSize(d, poolAt, poolCount * 14, "Bone-Pool");
    out.bonePool.resize(poolCount);
    for (std::size_t i = 0; i < poolCount; ++i)
        std::memcpy(out.bonePool[i].comp, p + poolAt + i * 14, 14);

    return out;
}

Mat3x4 MdxaFile::boneMatrix(int frame, int bone) const {
    const int numBones = static_cast<int>(skeleton.bones.size());
    const std::uint32_t idx = indices[static_cast<std::size_t>(frame) * numBones + bone];
    return uncompressBone(bonePool[idx]);
}

std::string RoundTripReport::describe() const {
    std::ostringstream os;
    os << frames << " Frames x " << bones << " Bones\n"
       << "  max. Positionsfehler:  " << maxPositionError << " Einheiten\n"
       << "  mittl. Positionsfehler: " << meanPositionError << " Einheiten\n"
       << "  max. Rotationsfehler:  " << maxRotationErrorDeg << " Grad";
    return os.str();
}

RoundTripReport compareRoundTrip(const MdxaFile& original, const MdxaWriteOptions& opt) {
    RoundTripReport rep;
    rep.frames = original.numFrames;
    rep.bones = static_cast<int>(original.skeleton.bones.size());

    CompressStats stats;
    double sum = 0.0;
    std::size_t n = 0;

    for (int f = 0; f < rep.frames; ++f) {
        for (int b = 0; b < rep.bones; ++b) {
            const Mat3x4 src = original.boneMatrix(f, b);
            const Mat3x4 back = uncompressBone(compressBone(src, opt.compress, stats));

            double posErr = 0.0;
            for (int r = 0; r < 3; ++r)
                posErr = std::max(posErr, static_cast<double>(std::fabs(back.m[r][3] - src.m[r][3])));
            rep.maxPositionError = std::max(rep.maxPositionError, posErr);
            sum += posErr;
            ++n;

            rep.maxRotationErrorDeg = std::max(rep.maxRotationErrorDeg,
                                               angleBetweenDeg(matrixToQuat(src), matrixToQuat(back)));
        }
    }
    if (n) rep.meanPositionError = sum / static_cast<double>(n);
    return rep;
}

}  // namespace g2
