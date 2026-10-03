#include "g2/mdxm.h"

#include "g2/bytebuf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>

namespace g2 {
namespace {

// The weights of a vertex as Carcass writes them (0x43fdd0): sorted by
// weight, largest first, STABLE - equal weights keep their envelope order.
// Not renormalised: the engine reads the last written weight as 1 - the sum
// of the others anyway, and a model authored against Carcass's output looks
// exactly like that. More than four (Carcass refuses the model): the four
// largest.
std::vector<VertexWeight> selectWeights(const Vertex& v, MdxmWriteStats& stats) {
    std::vector<VertexWeight> w;
    for (const auto& x : v.weights)
        if (x.weight > 0.0f) w.push_back(x);

    if (w.empty()) {
        // Carcass refuses a vertex without weight. Bound to bone 0 with
        // weight 1 so the model at least stays well-defined.
        w.push_back(VertexWeight{0, 1.0f});
    }

    std::stable_sort(w.begin(), w.end(),
                     [](const VertexWeight& a, const VertexWeight& b) { return a.weight > b.weight; });

    if (w.size() > static_cast<std::size_t>(fmt::kMaxWeightsPerVert)) {
        stats.weightsDropped += w.size() - fmt::kMaxWeightsPerVert;
        w.resize(fmt::kMaxWeightsPerVert);
    }
    return w;
}

// 10-bit weight: floor(float(w * 1023 + 0.5)), at most 1023 (Carcass).
std::uint32_t quantWeight(float w) {
    const float x = static_cast<float>(static_cast<double>(w) * 1023.0 + 0.5);
    const double q = std::floor(static_cast<double>(x));
    return q < 0.0 ? 0u : static_cast<std::uint32_t>(std::min(q, 1023.0));
}

// Collects all bones referenced in this surface and returns the
// global -> local mapping (0..31).
std::map<int, int> collectBoneRefs(const Surface& surf, std::vector<std::int32_t>& refsOut,
                                   MdxmWriteStats& stats) {
    std::map<int, int> mapping;
    for (const auto& v : surf.vertices) {
        MdxmWriteStats scratch;
        for (const auto& w : selectWeights(v, scratch)) mapping.emplace(w.boneIndex, 0);
    }

    if (mapping.size() > static_cast<std::size_t>(fmt::kMaxBoneRefsPerSurface)) {
        std::ostringstream os;
        os << "Surface \"" << surf.name << "\" referenziert " << mapping.size()
           << " Bones, das Format erlaubt maximal " << fmt::kMaxBoneRefsPerSurface
           << " (5 Bit pro Referenz). Surface aufteilen oder Gewichte zusammenfassen.";
        throw std::runtime_error(os.str());
    }

    int local = 0;
    refsOut.clear();
    for (auto& [global, slot] : mapping) {
        slot = local++;
        refsOut.push_back(global);
    }
    stats.maxBoneRefsUsed = std::max(stats.maxBoneRefsUsed, mapping.size());
    return mapping;
}

}  // namespace

std::vector<std::string> Mesh::validate() const {
    std::vector<std::string> errs;
    if (lods.empty()) {
        errs.push_back("Mesh hat keine LODs");
        return errs;
    }
    const std::size_t n = lods.front().surfaces.size();
    if (n == 0) errs.push_back("LOD 0 hat keine Surfaces");

    for (std::size_t i = 1; i < lods.size(); ++i) {
        if (lods[i].surfaces.size() != n)
            errs.push_back("LOD " + std::to_string(i) + " hat " +
                           std::to_string(lods[i].surfaces.size()) + " Surfaces, LOD 0 hat " +
                           std::to_string(n) + " — alle LODs muessen gleich viele haben");
    }

    for (std::size_t l = 0; l < lods.size(); ++l) {
        for (std::size_t s = 0; s < lods[l].surfaces.size(); ++s) {
            const Surface& surf = lods[l].surfaces[s];
            const auto nv = static_cast<int>(surf.vertices.size());
            for (const auto& t : surf.triangles) {
                for (int k = 0; k < 3; ++k) {
                    if (t.indexes[k] < 0 || t.indexes[k] >= nv) {
                        errs.push_back("LOD " + std::to_string(l) + " Surface \"" + surf.name +
                                       "\": Dreiecksindex " + std::to_string(t.indexes[k]) +
                                       " ausserhalb 0.." + std::to_string(nv - 1));
                        goto nextSurface;
                    }
                }
            }
        nextSurface:;
        }
    }
    return errs;
}

MdxmWriteResult writeMdxm(const Mesh& mesh) {
    const auto errs = mesh.validate();
    if (!errs.empty()) {
        std::ostringstream os;
        os << "Mesh ist ungueltig:";
        for (const auto& e : errs) os << "\n  - " << e;
        throw std::runtime_error(os.str());
    }

    MdxmWriteResult result;
    const auto numSurfaces = static_cast<std::int32_t>(mesh.lods.front().surfaces.size());
    const auto numLODs = static_cast<std::int32_t>(mesh.lods.size());

    ByteBuf buf;
    buf.i32(static_cast<std::int32_t>(fmt::kMdxmIdent));
    buf.i32(fmt::kMdxmVersion);
    buf.fixedString(mesh.name, fmt::kMaxQPath, "GLM-Name");
    buf.fixedString(mesh.animName, fmt::kMaxQPath, "GLA-Referenz");
    buf.i32(0);                    // animIndex, filled in by the engine
    buf.i32(mesh.numBones);
    buf.i32(numLODs);
    const std::size_t patchOfsLODs = buf.reserveI32();
    buf.i32(numSurfaces);
    const std::size_t patchOfsSurfHierarchy = buf.reserveI32();
    const std::size_t patchOfsEnd = buf.reserveI32();

    const std::size_t headerEnd = buf.size();
    if (headerEnd != sizeof(fmt::MdxmHeader))
        throw std::logic_error("Header-Groesse stimmt nicht mit MdxmHeader ueberein");

    // Offset table of the surface hierarchy, relative to the end of the header.
    std::vector<std::size_t> patchHierOffsets(static_cast<std::size_t>(numSurfaces));
    for (auto& s : patchHierOffsets) s = buf.reserveI32();

    // Derive the child lists from parentIndex.
    std::vector<std::vector<int>> children(static_cast<std::size_t>(numSurfaces));
    for (std::int32_t i = 0; i < numSurfaces; ++i) {
        const int p = mesh.lods.front().surfaces[static_cast<std::size_t>(i)].parentIndex;
        if (p >= 0 && p < numSurfaces) children[static_cast<std::size_t>(p)].push_back(i);
    }

    buf.patchI32(patchOfsSurfHierarchy, static_cast<std::int32_t>(buf.size()));
    for (std::int32_t i = 0; i < numSurfaces; ++i) {
        buf.patchI32(patchHierOffsets[static_cast<std::size_t>(i)],
                     static_cast<std::int32_t>(buf.size() - headerEnd));

        const Surface& surf = mesh.lods.front().surfaces[static_cast<std::size_t>(i)];
        buf.fixedString(surf.name, fmt::kMaxQPath, "Surface-Name");
        buf.u32(surf.flags);
        buf.fixedString(surf.shader, fmt::kMaxQPath, "Shader-Name");
        buf.i32(0);   // shaderIndex, filled in by the engine
        buf.i32(surf.parentIndex);
        const auto& kids = children[static_cast<std::size_t>(i)];
        buf.i32(static_cast<std::int32_t>(kids.size()));
        for (int k : kids) buf.i32(k);
    }

    // --- LODs --------------------------------------------------------------
    buf.patchI32(patchOfsLODs, static_cast<std::int32_t>(buf.size()));

    for (const LOD& lod : mesh.lods) {
        const std::size_t lodStart = buf.size();
        const std::size_t patchLodEnd = buf.reserveI32();

        // IMPORTANT: The surface offsets are relative to the start of THIS
        // table, i.e. to lodStart + sizeof(mdxmLOD_t), not to lodStart itself.
        // Verified against the real _humanoid.glm: there surface 0 has the
        // value 336 = 84 * 4, and the first surface sits exactly 336 bytes
        // after the start of the table. With lodStart as the base, everything
        // would be shifted by 4 bytes.
        const std::size_t surfTableStart = buf.size();

        std::vector<std::size_t> patchSurfOffsets(static_cast<std::size_t>(numSurfaces));
        for (auto& s : patchSurfOffsets) s = buf.reserveI32();

        for (std::int32_t si = 0; si < numSurfaces; ++si) {
            const Surface& surf = lod.surfaces[static_cast<std::size_t>(si)];
            const std::size_t surfStart = buf.size();
            buf.patchI32(patchSurfOffsets[static_cast<std::size_t>(si)],
                         static_cast<std::int32_t>(surfStart - surfTableStart));

            std::vector<std::int32_t> boneRefs;
            const auto boneMap = collectBoneRefs(surf, boneRefs, result.stats);

            buf.i32(0);    // ident
            buf.i32(si);   // thisSurfaceIndex
            buf.i32(-static_cast<std::int32_t>(surfStart));   // ofsHeader, back to the start of the file
            buf.i32(static_cast<std::int32_t>(surf.vertices.size()));
            const std::size_t patchOfsVerts = buf.reserveI32();
            buf.i32(static_cast<std::int32_t>(surf.triangles.size()));
            const std::size_t patchOfsTris = buf.reserveI32();
            buf.i32(static_cast<std::int32_t>(boneRefs.size()));
            const std::size_t patchOfsBoneRefs = buf.reserveI32();
            const std::size_t patchSurfEnd = buf.reserveI32();

            // Triangles
            buf.patchI32(patchOfsTris, static_cast<std::int32_t>(buf.size() - surfStart));
            for (const auto& t : surf.triangles) {
                buf.i32(t.indexes[0]);
                buf.i32(t.indexes[1]);
                buf.i32(t.indexes[2]);
            }

            // Vertices (32 bytes), followed separately by the UVs (8 bytes)
            buf.patchI32(patchOfsVerts, static_cast<std::int32_t>(buf.size() - surfStart));
            for (const auto& v : surf.vertices) {
                const auto w = selectWeights(v, result.stats);

                buf.f32(v.normal[0]);
                buf.f32(v.normal[1]);
                buf.f32(v.normal[2]);
                buf.f32(v.position[0]);
                buf.f32(v.position[1]);
                buf.f32(v.position[2]);

                std::uint32_t packed = 0;
                std::uint8_t  weightBytes[fmt::kMaxWeightsPerVert] = {0, 0, 0, 0};

                // As Carcass: a weight that quantises to 0 ends the list. The
                // first one always counts - Carcass wrote 0xC0000000 then
                // ("four weights, all zero").
                std::size_t written = 0;
                for (std::size_t k = 0; k < w.size(); ++k) {
                    const auto it = boneMap.find(w[k].boneIndex);
                    const auto localIdx = static_cast<std::uint32_t>(it->second);
                    const std::uint32_t q = quantWeight(w[k].weight);
                    // Carcass puts the bone index in before it looks at the
                    // weight - unused bits, kept for byte-identical files.
                    packed |= (localIdx & 0x1f) << (fmt::kBitsPerBoneRef * k);
                    if (q == 0 && k > 0) break;
                    // 10-bit weight: lower 8 bits go into the byte array, upper
                    // 2 bits into the packed word at position 20 + 2*k.
                    weightBytes[k] = static_cast<std::uint8_t>(q & 0xff);
                    packed |= (q & 0x300u) << (fmt::kBoneWeightTopBitsShift + 2 * k);
                    ++written;
                }
                packed |= static_cast<std::uint32_t>(written - 1) << 30;

                buf.u32(packed);
                for (int k = 0; k < fmt::kMaxWeightsPerVert; ++k) buf.u8(weightBytes[k]);
            }
            for (const auto& v : surf.vertices) {
                buf.f32(v.uv[0]);
                buf.f32(v.uv[1]);
            }

            // Bone references
            buf.patchI32(patchOfsBoneRefs, static_cast<std::int32_t>(buf.size() - surfStart));
            for (std::int32_t r : boneRefs) buf.i32(r);

            buf.patchI32(patchSurfEnd, static_cast<std::int32_t>(buf.size() - surfStart));
        }

        buf.patchI32(patchLodEnd, static_cast<std::int32_t>(buf.size() - lodStart));
    }

    buf.patchI32(patchOfsEnd, static_cast<std::int32_t>(buf.size()));
    result.data = buf.bytes();
    return result;
}

// --- Reading ---------------------------------------------------------------

namespace {

class GlmReader {
public:
    explicit GlmReader(const std::vector<std::uint8_t>& d) : d_(d) {}

    // Every access goes through here. Offsets in a GLM are signed and partly
    // relative; a broken file must end in an exception, never in a read past
    // the buffer.
    void need(std::int64_t off, std::int64_t n, const char* what) const {
        if (off < 0 || n < 0 || off > static_cast<std::int64_t>(d_.size()) ||
            n > static_cast<std::int64_t>(d_.size()) - off)
            throw std::runtime_error(std::string("GLM beschaedigt oder abgeschnitten bei ") + what);
    }
    std::int32_t i32(std::int64_t off, const char* what) const {
        need(off, 4, what);
        const std::uint8_t* p = d_.data() + off;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                         (static_cast<std::uint32_t>(p[1]) << 8) |
                                         (static_cast<std::uint32_t>(p[2]) << 16) |
                                         (static_cast<std::uint32_t>(p[3]) << 24));
    }
    std::uint32_t u32(std::int64_t off, const char* what) const {
        return static_cast<std::uint32_t>(i32(off, what));
    }
    float f32(std::int64_t off, const char* what) const {
        const std::uint32_t bits = u32(off, what);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    std::uint8_t u8(std::int64_t off, const char* what) const {
        need(off, 1, what);
        return d_[static_cast<std::size_t>(off)];
    }
    std::string name(std::int64_t off, const char* what) const {
        need(off, fmt::kMaxQPath, what);
        const char* p = reinterpret_cast<const char*>(d_.data() + off);
        std::size_t n = 0;
        while (n < static_cast<std::size_t>(fmt::kMaxQPath) && p[n]) ++n;
        return std::string(p, n);
    }
    std::size_t size() const { return d_.size(); }

private:
    const std::vector<std::uint8_t>& d_;
};

// Counts are checked against the remaining file size before anything is
// allocated: a 200-byte file claiming two billion vertices must not ask for
// 64 GB.
void checkCount(std::int32_t n, std::size_t elemSize, std::size_t fileSize, const char* what) {
    if (n < 0 || static_cast<std::uint64_t>(n) * elemSize > fileSize)
        throw std::runtime_error(std::string("GLM beschaedigt: unsinnige Anzahl bei ") + what);
}

}  // namespace

bool isMdxm(const std::vector<std::uint8_t>& d) {
    return d.size() >= 4 && d[0] == '2' && d[1] == 'L' && d[2] == 'G' && d[3] == 'M';
}

MdxmFile readMdxm(const std::vector<std::uint8_t>& d) {
    const GlmReader r(d);
    r.need(0, sizeof(fmt::MdxmHeader), "Header");
    if (r.u32(0, "Ident") != fmt::kMdxmIdent)
        throw std::runtime_error("Keine GLM-Datei (Ident stimmt nicht, erwartet \"2LGM\")");

    MdxmFile out;
    out.fileSize = d.size();
    out.version = r.i32(4, "Version");
    if (out.version != fmt::kMdxmVersion)
        throw std::runtime_error("GLM-Version " + std::to_string(out.version) + ", erwartet " +
                                 std::to_string(fmt::kMdxmVersion));
    Mesh& m = out.mesh;
    m.name = r.name(8, "Name");
    m.animName = r.name(72, "GLA-Name");
    out.animIndex = r.i32(136, "animIndex");
    m.numBones = r.i32(140, "numBones");
    const std::int32_t numLODs = r.i32(144, "numLODs");
    const std::int32_t ofsLODs = r.i32(148, "ofsLODs");
    const std::int32_t numSurfaces = r.i32(152, "numSurfaces");
    const std::int32_t ofsHier = r.i32(156, "ofsSurfHierarchy");
    const std::int32_t ofsEnd = r.i32(160, "ofsEnd");
    if (ofsEnd != static_cast<std::int32_t>(d.size()))
        throw std::runtime_error("GLM: ofsEnd (" + std::to_string(ofsEnd) +
                                 ") passt nicht zur Dateigroesse (" + std::to_string(d.size()) + ")");
    checkCount(numLODs, 4, d.size(), "numLODs");
    checkCount(numSurfaces, sizeof(fmt::MdxmSurfHierarchy), d.size(), "numSurfaces");

    // Surface hierarchy: an offset table right after the header (relative to
    // the header end), then the entries.
    const std::int64_t headerEnd = sizeof(fmt::MdxmHeader);
    std::vector<Surface> proto(static_cast<std::size_t>(numSurfaces));
    for (std::int32_t i = 0; i < numSurfaces; ++i) {
        const std::int64_t e = headerEnd + r.i32(headerEnd + 4 * i, "Hierarchie-Tabelle");
        Surface& s = proto[static_cast<std::size_t>(i)];
        s.name = r.name(e, "Surface-Name");
        s.flags = r.u32(e + 64, "Surface-Flags");
        s.shader = r.name(e + 68, "Shader");
        s.parentIndex = r.i32(e + 136, "Parent");
        const std::int32_t kids = r.i32(e + 140, "numChildren");
        checkCount(kids, 4, d.size(), "numChildren");
        r.need(e + 144, 4LL * kids, "Kinderliste");
        if (s.parentIndex < -1 || s.parentIndex >= numSurfaces)
            throw std::runtime_error("GLM: Surface \"" + s.name + "\" hat ungueltigen Parent " +
                                     std::to_string(s.parentIndex));
    }
    (void)ofsHier;

    std::int64_t lodStart = ofsLODs;
    for (std::int32_t l = 0; l < numLODs; ++l) {
        const std::int32_t lodEnd = r.i32(lodStart, "LOD-Ende");
        const std::int64_t table = lodStart + 4;
        LOD lod;
        lod.surfaces = proto;
        std::vector<std::vector<std::int32_t>> lodRefs(static_cast<std::size_t>(numSurfaces));
        for (std::int32_t si = 0; si < numSurfaces; ++si) {
            const std::int64_t s0 = table + r.i32(table + 4 * si, "Surface-Tabelle");
            Surface& s = lod.surfaces[static_cast<std::size_t>(si)];
            const std::int32_t nv = r.i32(s0 + 12, "numVerts");
            const std::int32_t ofsV = r.i32(s0 + 16, "ofsVerts");
            const std::int32_t nt = r.i32(s0 + 20, "numTriangles");
            const std::int32_t ofsT = r.i32(s0 + 24, "ofsTriangles");
            const std::int32_t nb = r.i32(s0 + 28, "numBoneReferences");
            const std::int32_t ofsB = r.i32(s0 + 32, "ofsBoneReferences");
            checkCount(nv, sizeof(fmt::MdxmVertex), d.size(), "numVerts");
            checkCount(nt, sizeof(fmt::MdxmTriangle), d.size(), "numTriangles");
            checkCount(nb, 4, d.size(), "numBoneReferences");

            auto& refs = lodRefs[static_cast<std::size_t>(si)];
            for (std::int32_t k = 0; k < nb; ++k) refs.push_back(r.i32(s0 + ofsB + 4 * k, "Bone-Referenz"));

            s.triangles.resize(static_cast<std::size_t>(nt));
            for (std::int32_t t = 0; t < nt; ++t)
                for (int c = 0; c < 3; ++c)
                    s.triangles[static_cast<std::size_t>(t)].indexes[c] =
                        r.i32(s0 + ofsT + 12LL * t + 4 * c, "Dreieck");

            s.vertices.resize(static_cast<std::size_t>(nv));
            const std::int64_t uvBase = s0 + ofsV + 32LL * nv;
            for (std::int32_t v = 0; v < nv; ++v) {
                const std::int64_t p = s0 + ofsV + 32LL * v;
                Vertex& vx = s.vertices[static_cast<std::size_t>(v)];
                for (int k = 0; k < 3; ++k) vx.normal[k] = r.f32(p + 4 * k, "Normale");
                for (int k = 0; k < 3; ++k) vx.position[k] = r.f32(p + 12 + 4 * k, "Position");
                fmt::MdxmVertex raw{};
                raw.weightsAndBoneIndexes = r.u32(p + 24, "Gewichte");
                for (int k = 0; k < 4; ++k) raw.boneWeightings[k] = r.u8(p + 28 + k, "Gewichte");
                const int nw = fmt::getVertWeightCount(raw);
                for (int k = 0; k < nw; ++k) {
                    const int local = fmt::getVertBoneIndex(raw, k);
                    if (local >= nb)
                        throw std::runtime_error("GLM: Surface \"" + s.name + "\" Vertex " +
                                                 std::to_string(v) + " zeigt auf Bone-Referenz " +
                                                 std::to_string(local) + " von " + std::to_string(nb));
                    vx.weights.push_back(VertexWeight{
                        refs[static_cast<std::size_t>(local)],
                        static_cast<float>(fmt::getVertBoneWeightRaw(raw, k)) *
                            fmt::kBoneWeightReciprocal});
                }
                // The engine never reads the stored value of the LAST weight:
                // G2_GetVertBoneWeight takes 1 - sum(others). Mirror that, so
                // a dump shows what the game really uses.
                if (nw > 1) {
                    float sum = 0.0f;
                    for (int k = 0; k + 1 < nw; ++k) sum += vx.weights[static_cast<std::size_t>(k)].weight;
                    vx.weights.back().weight = 1.0f - sum;
                } else if (nw == 1) {
                    vx.weights.back().weight = 1.0f;
                }
                vx.uv[0] = r.f32(uvBase + 8LL * v, "UV");
                vx.uv[1] = r.f32(uvBase + 8LL * v + 4, "UV");
            }
        }
        m.lods.push_back(std::move(lod));
        out.boneRefs.push_back(std::move(lodRefs));
        if (lodEnd <= 0) throw std::runtime_error("GLM: LOD " + std::to_string(l) + " ohne Laenge");
        lodStart += lodEnd;
    }
    return out;
}

}  // namespace g2
