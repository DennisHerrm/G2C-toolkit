#include "g2/mdxm.h"

#include "g2/bytebuf.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>

namespace g2 {
namespace {

// Picks up to four weights, sorted by magnitude, and renormalizes them.
std::vector<VertexWeight> selectWeights(const Vertex& v, MdxmWriteStats& stats) {
    std::vector<VertexWeight> w;
    for (const auto& x : v.weights)
        if (x.weight > 0.0f) w.push_back(x);

    if (w.empty()) {
        // Carcass reports "Vert (%d) on mesh \"%s\" has no weights" here and
        // carries on anyway. We bind the vertex to bone 0 with weight 1 so
        // that the model at least stays well-defined.
        w.push_back(VertexWeight{0, 1.0f});
    }

    std::sort(w.begin(), w.end(),
              [](const VertexWeight& a, const VertexWeight& b) { return a.weight > b.weight; });

    if (w.size() > static_cast<std::size_t>(fmt::kMaxWeightsPerVert)) {
        stats.weightsDropped += w.size() - fmt::kMaxWeightsPerVert;
        w.resize(fmt::kMaxWeightsPerVert);
    }

    float sum = 0.0f;
    for (const auto& x : w) sum += x.weight;
    if (sum > 0.0f && std::fabs(sum - 1.0f) > 1e-5f) {
        ++stats.weightsRenormalized;
        for (auto& x : w) x.weight /= sum;
    }
    return w;
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

                packed |= static_cast<std::uint32_t>(w.size() - 1) << 30;

                for (std::size_t k = 0; k < w.size(); ++k) {
                    const auto it = boneMap.find(w[k].boneIndex);
                    const auto localIdx = static_cast<std::uint32_t>(it->second);
                    packed |= (localIdx & 0x1f) << (fmt::kBitsPerBoneRef * k);

                    // 10-bit weight: lower 8 bits go into the byte array, upper
                    // 2 bits into the packed word at position 20 + 2*k.
                    auto q = static_cast<std::uint32_t>(std::lrintf(w[k].weight * 1023.0f));
                    q = std::min<std::uint32_t>(q, 1023u);
                    weightBytes[k] = static_cast<std::uint8_t>(q & 0xff);
                    packed |= (q & 0x300u) << (fmt::kBoneWeightTopBitsShift + 2 * k);
                }

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

}  // namespace g2
