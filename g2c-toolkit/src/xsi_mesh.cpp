#include "g2/xsi_mesh.h"

#include "g2/readfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <unordered_map>

namespace g2::xsi {
namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

Mat3x4 eulerXYZ(float rx, float ry, float rz) {
    const double a = rx * kDeg2Rad, b = ry * kDeg2Rad, c = rz * kDeg2Rad;
    const double cx = std::cos(a), sx = std::sin(a);
    const double cy = std::cos(b), sy = std::sin(b);
    const double cz = std::cos(c), sz = std::sin(c);
    Mat3x4 m{};
    m.m[0][0] = static_cast<float>(cz * cy);
    m.m[0][1] = static_cast<float>(cz * sy * sx - sz * cx);
    m.m[0][2] = static_cast<float>(cz * sy * cx + sz * sx);
    m.m[1][0] = static_cast<float>(sz * cy);
    m.m[1][1] = static_cast<float>(sz * sy * sx + cz * cx);
    m.m[1][2] = static_cast<float>(sz * sy * cx - cz * sx);
    m.m[2][0] = static_cast<float>(-sy);
    m.m[2][1] = static_cast<float>(cy * sx);
    m.m[2][2] = static_cast<float>(cy * cx);
    return m;
}

// Y-up to Z-up, the same convention as for the bones.
void toGlmSpace(const float in[3], float scale, float out[3]) {
    out[0] = scale * in[0];
    out[1] = scale * -in[2];
    out[2] = scale * in[1];
}

std::string shortName(const std::string& full) {
    const std::size_t dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

std::string stripPrefix(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.rfind(prefix, 0) == 0 ? s.substr(n) : s;
}

// One attribute block from SI_Shape: count, identifier, then the values.
struct ShapeArray {
    std::string        kind;   // POSITION, NORMAL, TEX_COORD_UV0, ...
    std::vector<float> values;
    int                stride = 3;

    std::size_t count() const { return values.size() / static_cast<std::size_t>(stride); }
};

std::vector<ShapeArray> parseShape(const Template& shape) {
    // Layout: <arrayCount>, "ORDERED", then per array:
    //         <n>, "<kind>", <n*stride values>
    std::vector<ShapeArray> out;
    std::size_t i = 0;
    const auto& v = shape.values;

    // Skip over the leading count and "ORDERED".
    while (i < v.size() && !v[i].asNumber()) ++i;
    if (i < v.size()) ++i;
    while (i < v.size() && v[i].text() == "ORDERED") ++i;

    while (i + 1 < v.size()) {
        const auto n = v[i].asInt();
        if (!n || *n < 0) { ++i; continue; }
        const std::string kind = v[i + 1].text();
        if (kind.empty() || v[i + 1].asNumber()) { ++i; continue; }

        ShapeArray a;
        a.kind = kind;
        a.stride = (kind.rfind("TEX_COORD", 0) == 0) ? 2 : 3;
        const std::size_t need = static_cast<std::size_t>(*n) * static_cast<std::size_t>(a.stride);
        i += 2;

        // More strings may follow the count and identifier.
        // TEX_COORD_UV0 additionally carries the projection name:
        //
        //     609,
        //     "TEX_COORD_UV0",
        //     "Texture_Projection",     <-- additional
        //     0.127449,0.841402,
        //
        // If you don't skip it, everything from here on is read off by one.
        // Every corner then gets a wrong UV, and because vertex merging
        // requires equal UVs, nothing merges at all afterwards. The bug
        // disguises itself far downstream as an apparent tolerance problem:
        // no normal tolerance, however large, changes anything, because the
        // UV condition before it already blocks everything.
        //
        // Hence, in general: skip whatever is not a number.
        while (i < v.size() && !v[i].asNumber()) ++i;

        a.values.reserve(need);
        for (std::size_t k = 0; k < need && i < v.size(); ++k, ++i) {
            const auto d = v[i].asNumber();
            a.values.push_back(d ? static_cast<float>(*d) : 0.0f);
        }
        out.push_back(std::move(a));
    }
    return out;
}

const ShapeArray* findArray(const std::vector<ShapeArray>& arrays, const char* prefix) {
    for (const auto& a : arrays)
        if (a.kind.rfind(prefix, 0) == 0) return &a;
    return nullptr;
}

// One entry of the envelope list: which mesh, which bone, which
// vertices with which weight.
struct EnvelopeEntry {
    std::string      mesh;
    std::string      bone;
    std::vector<std::pair<int, float>> weights;   // position index, percent
};

std::vector<EnvelopeEntry> parseEnvelopes(const Document& doc) {
    std::vector<EnvelopeEntry> out;
    const Template* list = doc.findDeep("SI_EnvelopeList");
    if (!list) return out;

    for (const Template* e : list->findAll("SI_Envelope")) {
        if (e->values.size() < 3) continue;
        EnvelopeEntry en;
        en.mesh = shortName(stripPrefix(e->values[0].text(), "MDL-"));
        en.bone = shortName(stripPrefix(e->values[1].text(), "MDL-"));
        const auto n = e->values[2].asInt();
        if (!n || *n <= 0) continue;

        en.weights.reserve(static_cast<std::size_t>(*n));
        for (std::size_t i = 3; i + 1 < e->values.size(); i += 2) {
            const auto idx = e->values[i].asInt();
            const auto w = e->values[i + 1].asNumber();
            if (!idx || !w) continue;
            // Weights are given in percent. Zero weights are listed
            // explicitly and must be discarded.
            if (*w <= 0.0) continue;
            en.weights.emplace_back(static_cast<int>(*idx), static_cast<float>(*w * 0.01));
        }
        out.push_back(std::move(en));
    }
    return out;
}

// Shader from XSI_CustomPSet <mesh>.Game { "NODE", 1, "Shader","Text","<path>" }
std::map<std::string, std::string> parseShaders(const Template& t,
                                                std::map<std::string, std::string>& out) {
    if (t.type == "XSI_CustomPSet" && t.name.size() > 5) {
        std::string nm = t.name;
        const std::string suffix = ".Game";
        if (nm.size() > suffix.size() && nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) == 0) {
            nm = nm.substr(0, nm.size() - suffix.size());
            for (std::size_t i = 0; i + 2 < t.values.size(); ++i) {
                if (t.values[i].text() == "Shader" && t.values[i + 1].text() == "Text") {
                    // Some XSI_CustomPSet entries write Windows paths with
                    // backslashes. The GLM uses forward slashes throughout -
                    // otherwise the engine can't find the texture and ModView
                    // reports it as missing.
                    std::string path = t.values[i + 2].text();
                    std::replace(path.begin(), path.end(), '\\', '/');
                    out[shortName(nm)] = path;
                }
            }
        }
    }
    for (const auto& c : t.children) parseShaders(c, out);
    return out;
}

// Key for vertex merging.
//
// NOT the attribute indices: in Raven's files the normal index runs
// sequentially per corner (0,1,2 / 3,4,5 / ...), so merging by index never
// merges anything - "hips" would come out with 609 instead of 132 vertices,
// i.e. exactly three per triangle.
//
// Merging is by VALUE: same position, same normal, same UV yield the same
// vertex. The values are rounded to a fine grid for this, otherwise numerical
// noise in the last bit prevents any match.
struct VertexKey {
    int pos = 0;
    int nx = 0, ny = 0, nz = 0;
    int u = 0, v = 0;

    bool operator==(const VertexKey& o) const noexcept {
        return pos == o.pos && nx == o.nx && ny == o.ny && nz == o.nz && u == o.u && v == o.v;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const noexcept {
        std::size_t h = 1469598103934665603ull;
        for (int v : {k.pos, k.nx, k.ny, k.nz, k.u, k.v}) {
            h ^= static_cast<std::size_t>(static_cast<unsigned>(v));
            h *= 1099511628211ull;
        }
        return h;
    }
};

// Quantize normals to about 1/16384 and UVs to about 1/65536. Fine enough
// that real edges stay separate, coarse enough to absorb rounding noise.
inline int quantN(float f) { return static_cast<int>(std::lround(f * 16384.0f)); }
inline int quantUV(float f) { return static_cast<int>(std::lround(f * 65536.0f)); }

}  // namespace

std::string surfaceNameToGlm(const std::string& xsiName) {
    std::string s = xsiName;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.rfind("bolt_", 0) == 0) s = "*" + s.substr(5);
    return s;
}

std::uint32_t surfaceFlagsFromName(const std::string& glmName) {
    std::uint32_t f = 0;
    if (!glmName.empty() && glmName[0] == '*') f |= fmt::kSurfFlagIsBolt;
    const std::string suffix = "_off";
    if (glmName.size() > suffix.size() &&
        glmName.compare(glmName.size() - suffix.size(), suffix.size(), suffix) == 0)
        f |= fmt::kSurfFlagOff;
    return f;
}

MeshImportResult importMesh(const Document& doc, const MeshImportOptions& opt) {
    MeshImportResult res;
    res.mesh.name = opt.modelName;
    res.mesh.animName = opt.animName;
    res.mesh.numBones = static_cast<int>(opt.boneNames.size());

    // Bone name -> index. Aliases point from the reference into the dotXSI;
    // here the lookup goes the other way round.
    std::map<std::string, int> boneIndex;
    for (std::size_t i = 0; i < opt.boneNames.size(); ++i) {
        boneIndex[opt.boneNames[i]] = static_cast<int>(i);
        const auto al = opt.aliases.find(opt.boneNames[i]);
        if (al != opt.aliases.end()) boneIndex[al->second] = static_cast<int>(i);
    }

    const auto envelopes = parseEnvelopes(doc);
    std::map<std::string, std::string> shaders;
    for (const auto& r : doc.roots) parseShaders(r, shaders);

    // Group envelopes by mesh.
    std::map<std::string, std::vector<const EnvelopeEntry*>> envByMesh;
    for (const auto& e : envelopes) envByMesh[e.mesh].push_back(&e);

    // --- Walk the model tree ----------------------------------------------
    struct Found {
        std::string     xsiName;
        const Template* model = nullptr;
        int             parent = -1;   // index into this list
    };
    std::vector<Found> found;

    std::function<void(const Template&, int)> walk = [&](const Template& t, int parentSurface) {
        int self = parentSurface;
        if (t.type == "SI_Model") {
            const std::string nm = shortName(stripPrefix(t.name, "MDL-"));
            if (t.find("SI_Mesh") != nullptr) {
                Found f;
                f.xsiName = nm;
                f.model = &t;
                f.parent = parentSurface;
                self = static_cast<int>(found.size());
                found.push_back(std::move(f));
            }
        }
        for (const auto& c : t.children) walk(c, self);
    };
    for (const auto& r : doc.roots) walk(r, -1);

    if (found.empty()) throw std::runtime_error("Keine Mesh-Modelle in der dotXSI gefunden");

    LOD lod;
    lod.surfaces.reserve(found.size());

    // Which surface came from which found entry. A skipped mesh - no shape,
    // no triangles - has none; f.parent, however, points into the found list.
    // Without remapping, all parent references after it would shift by one,
    // and the shader inheritance below, which looks for the same parent,
    // would miss.
    std::vector<int> surfaceOf(found.size(), -1);

    for (std::size_t fi = 0; fi < found.size(); ++fi) {
        const Found& f = found[fi];
        const Template* mesh = f.model->find("SI_Mesh");
        const Template* shape = mesh ? mesh->findDeep("SI_Shape") : nullptr;
        const Template* tris = mesh ? mesh->findDeep("SI_TriangleList") : nullptr;
        if (!shape || !tris) {
            res.stats.warnings.push_back("Mesh \"" + f.xsiName + "\" ohne Shape oder TriangleList");
            continue;
        }

        Surface surf;
        surf.name = surfaceNameToGlm(f.xsiName);
        surf.flags = surfaceFlagsFromName(surf.name);
        surf.parentIndex = f.parent;

        const auto sh = shaders.find(f.xsiName);
        surf.shader = (sh == shaders.end()) ? "" : sh->second;

        const auto arrays = parseShape(*shape);
        if (const std::string dbg = envValue("G2C_DEBUG_SHAPE"); !dbg.empty())
            if (f.xsiName == dbg) {
                std::fprintf(stderr, "SI_Shape %s: %zu Arrays\n", f.xsiName.c_str(), arrays.size());
                for (const auto& a : arrays)
                    std::fprintf(stderr, "   %-16s count=%zu stride=%d erste=%g %g %g\n",
                                 a.kind.c_str(), a.count(), a.stride,
                                 a.values.size() > 0 ? a.values[0] : 0.0,
                                 a.values.size() > 1 ? a.values[1] : 0.0,
                                 a.values.size() > 2 ? a.values[2] : 0.0);
            }
        const ShapeArray* pos = findArray(arrays, "POSITION");
        const ShapeArray* nrm = findArray(arrays, "NORMAL");
        const ShapeArray* uv = findArray(arrays, "TEX_COORD");
        if (!pos) {
            res.stats.warnings.push_back("Mesh \"" + f.xsiName + "\" ohne POSITION");
            continue;
        }

        // Object transform of the mesh model.
        //
        // BASEPOSE, not SRT: BASEPOSE is the ABSOLUTE pose, SRT the local one
        // relative to the parent model. The mesh models are nested inside each
        // other (torso under hips under mesh_root), so the local value is wrong
        // unless accumulated over the whole chain.
        //
        // With SRT, 83 of 84 surfaces were off by exactly the same amount - a
        // constant offset that points precisely to a missing parent chain. The
        // same distinction was already decisive for the bones.
        Mat3x4 xf = Mat3x4::identity();
        for (const Template* x : f.model->findAll("SI_Transform")) {
            if (x->name.rfind("BASEPOSE-", 0) != 0 || x->values.size() < 9) continue;
            float v[9]{1, 1, 1, 0, 0, 0, 0, 0, 0};
            for (int k = 0; k < 9; ++k)
                if (const auto d = x->values[static_cast<std::size_t>(k)].asNumber())
                    v[k] = static_cast<float>(*d);
            xf = eulerXYZ(v[3], v[4], v[5]);
            for (int r = 0; r < 3; ++r) {
                xf.m[r][0] *= v[0];
                xf.m[r][1] *= v[1];
                xf.m[r][2] *= v[2];
            }
            xf.m[0][3] = v[6];
            xf.m[1][3] = v[7];
            xf.m[2][3] = v[8];
            break;
        }

        // --- Read the TriangleList ----------------------------------------
        // Layout: <count>, "<attributes>", "<material>", then one index
        // block each with count*3 values - positions, normals, UVs.
        if (tris->values.size() < 3) {
            res.stats.warnings.push_back("TriangleList von \"" + f.xsiName + "\" ist leer");
            continue;
        }
        const auto triCount = tris->values[0].asInt();
        if (!triCount || *triCount <= 0) continue;
        const std::string attribs = tris->values[1].text();
        const bool hasUv = attribs.find("TEX_COORD") != std::string::npos;
        const bool hasNormals = attribs.find("NORMAL") != std::string::npos;

        const std::size_t perBlock = static_cast<std::size_t>(*triCount) * 3;
        std::size_t at = 3;
        const auto readBlock = [&](std::vector<int>& dst) {
            dst.clear();
            dst.reserve(perBlock);
            for (std::size_t k = 0; k < perBlock && at < tris->values.size(); ++k, ++at) {
                const auto v = tris->values[at].asInt();
                dst.push_back(v ? static_cast<int>(*v) : 0);
            }
        };

        std::vector<int> posIdx, nrmIdx, uvIdx;
        readBlock(posIdx);
        if (hasNormals) readBlock(nrmIdx);
        if (hasUv) readBlock(uvIdx);

        if (posIdx.size() != perBlock) {
            res.stats.warnings.push_back("TriangleList von \"" + f.xsiName + "\" abgeschnitten");
            continue;
        }

        // Weights of this mesh: position index -> list of (bone, weight).
        std::map<int, std::vector<VertexWeight>> weightsByPos;
        const auto ev = envByMesh.find(f.xsiName);
        if (ev != envByMesh.end()) {
            for (const EnvelopeEntry* e : ev->second) {
                const auto bi = boneIndex.find(e->bone);
                if (bi == boneIndex.end()) continue;   // bone not in the skeleton
                for (const auto& [vi, w] : e->weights)
                    weightsByPos[vi].push_back(VertexWeight{bi->second, w});
            }
        }

        // --- Attribute resolution -----------------------------------------
        //
        // dotXSI has a separate index array per attribute; GLM knows only one
        // index per vertex. So create each occurring triple once and reuse
        // it. For "hips", 132 positions face 609 normals, and again exactly
        // 132 vertices result - there every normal merges with the same
        // position.
        // Candidates are grouped by position index. The search only runs
        // within one position - the rule requires no more than that, and the
        // linear search over all vertices becomes one over just a few.
        struct Proto {
            float uv[2];
            float normal[3];
            int   index;
        };
        std::unordered_map<int, std::vector<Proto>> byPos;
        byPos.reserve(perBlock / 2);

        surf.vertices.reserve(perBlock / 2);
        surf.triangles.reserve(static_cast<std::size_t>(*triCount));

        for (std::size_t t3 = 0; t3 < perBlock; t3 += 3) {
            Triangle tri;
            for (int c = 0; c < 3; ++c) {
                const std::size_t k = t3 + static_cast<std::size_t>(c);
                const int pi = posIdx[k];
                const int ni = hasNormals ? nrmIdx[k] : -1;
                const int ui = hasUv ? uvIdx[k] : -1;

                Vertex v;
                if (pos && static_cast<std::size_t>(pi) < pos->count()) {
                    const float* p = &pos->values[static_cast<std::size_t>(pi) * 3];
                    const float local[3] = {p[0], p[1], p[2]};
                    float world[3];
                    for (int r = 0; r < 3; ++r)
                        world[r] = xf.m[r][0] * local[0] + xf.m[r][1] * local[1] +
                                   xf.m[r][2] * local[2] + xf.m[r][3];
                    toGlmSpace(world, opt.scale, v.position);
                }
                if (nrm && ni >= 0 && static_cast<std::size_t>(ni) < nrm->count()) {
                    const float* n = &nrm->values[static_cast<std::size_t>(ni) * 3];
                    // Normals are only rotated, not translated, and the scale
                    // drops out in the subsequent normalization.
                    const float world[3] = {xf.m[0][0] * n[0] + xf.m[0][1] * n[1] + xf.m[0][2] * n[2],
                                            xf.m[1][0] * n[0] + xf.m[1][1] * n[1] + xf.m[1][2] * n[2],
                                            xf.m[2][0] * n[0] + xf.m[2][1] * n[1] + xf.m[2][2] * n[2]};
                    float g[3];
                    toGlmSpace(world, 1.0f, g);
                    const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    if (len > 1e-8f) { g[0] /= len; g[1] /= len; g[2] /= len; }
                    v.normal[0] = g[0];
                    v.normal[1] = g[1];
                    v.normal[2] = g[2];
                }
                if (uv && ui >= 0 && static_cast<std::size_t>(ui) < uv->count()) {
                    const float* u = &uv->values[static_cast<std::size_t>(ui) * 2];
                    v.uv[0] = u[0];
                    // dotXSI counts V from the bottom, GLM from the top.
                    v.uv[1] = 1.0f - u[1];
                }

                // Merging is by position AND UV, not by the normal.
                //
                // Position alone is not enough: at UV seams the same point
                // must occur twice, otherwise the texturing collapses. For
                // "torso" there are 143 positions but 159 vertices in the
                // original file.
                //
                // The normal, on the other hand, does NOT belong in the key:
                // in Raven's files every triangle corner has its own normal
                // index with a slightly different value. Include it and
                // nothing merges at all, and 132 vertices turn into 583.
                auto& bucket = byPos[pi];
                int hit = -1;
                for (const Proto& p : bucket) {
                    if (std::fabs(p.uv[0] - v.uv[0]) > opt.uvTolerance ||
                        std::fabs(p.uv[1] - v.uv[1]) > opt.uvTolerance)
                        continue;
                    if (std::fabs(p.normal[0] - v.normal[0]) >= opt.normalTolerance ||
                        std::fabs(p.normal[1] - v.normal[1]) >= opt.normalTolerance ||
                        std::fabs(p.normal[2] - v.normal[2]) >= opt.normalTolerance)
                        continue;
                    hit = p.index;
                    break;   // first hit wins, not the best one
                }
                if (hit >= 0) {
                    tri.indexes[c] = hit;
                    continue;
                }

                const auto w = weightsByPos.find(pi);
                if (w != weightsByPos.end()) v.weights = w->second;

                const int newIndex = static_cast<int>(surf.vertices.size());
                bucket.push_back(Proto{{v.uv[0], v.uv[1]},
                                       {v.normal[0], v.normal[1], v.normal[2]},
                                       newIndex});
                surf.vertices.push_back(std::move(v));
                tri.indexes[c] = newIndex;
            }
            // Reverse the winding order.
            //
            // dotXSI and GLM order the corners in opposite directions.
            // Measured on Raven's _humanoid.glm: there the face normal
            // computed from the positions points opposite to the averaged
            // vertex normal for ALL 2846 triangles - without reversal, all
            // 2846 come out the other way round.
            //
            // It shows up as seemingly flipped normals: the faces are
            // rendered from the inside. The normals themselves are perfectly
            // fine.
            std::swap(tri.indexes[1], tri.indexes[2]);
            surf.triangles.push_back(tri);
        }

        if (pos && surf.vertices.size() > pos->count())
            res.stats.splitVertices += surf.vertices.size() - pos->count();

        res.stats.vertices += surf.vertices.size();
        res.stats.triangles += surf.triangles.size();
        if (surf.flags & fmt::kSurfFlagIsBolt) ++res.stats.tags;
        if (surf.flags & fmt::kSurfFlagOff) ++res.stats.offSurfaces;

        surfaceOf[fi] = static_cast<int>(lod.surfaces.size());
        lod.surfaces.push_back(std::move(surf));
    }

    // Remap parent references from found indices to surface indices. If the
    // immediate parent is missing, the next existing one above it applies.
    for (std::size_t fi = 0; fi < found.size(); ++fi) {
        if (surfaceOf[fi] < 0) continue;
        int p = found[fi].parent;
        while (p >= 0 && surfaceOf[static_cast<std::size_t>(p)] < 0)
            p = found[static_cast<std::size_t>(p)].parent;
        lod.surfaces[static_cast<std::size_t>(surfaceOf[fi])].parentIndex =
            p < 0 ? -1 : surfaceOf[static_cast<std::size_t>(p)];
    }

    // Complete the shader assignment.
    //
    // Only 37 of the 84 mesh models carry an XSI_CustomPSet with a shader
    // name. The rest - above all the tags - inherit it from the **immediately
    // preceding sibling**: same parent, previous position in the surface
    // order. If there is none, "[nomaterial]" is used.
    //
    // NOT from the parent itself. The difference is clearly visible:
    // "*l_hand" hangs under "l_hand" (hand.tga) but carries "torso.tga" -
    // the shader of its predecessor "l_hand_sleeve". And "*l_arm_cap_l_hand"
    // hangs under "l_arm" (torso.tga) and carries "hand.tga" from its
    // predecessor "l_hand". Parent inheritance produces exactly the swapped
    // result.
    //
    // Checked against all 46 tags of Raven's _humanoid.glm: 44 follow the
    // rule, the remaining two ("*l_leg_calf", "*r_leg_calf") have no
    // preceding sibling and carry "[nomaterial]".
    //
    // An EMPTY shader name occurs nowhere in the original file. ModView
    // loads shaders by exactly this name and otherwise reports missing
    // textures.
    for (std::size_t i = 0; i < lod.surfaces.size(); ++i) {
        Surface& s = lod.surfaces[i];
        if (!s.shader.empty()) continue;
        for (std::size_t j = i; j-- > 0;) {
            if (lod.surfaces[j].parentIndex != s.parentIndex) continue;
            s.shader = lod.surfaces[j].shader;
            break;
        }
        if (s.shader.empty()) s.shader = "[nomaterial]";
    }

    res.stats.surfaces = lod.surfaces.size();
    res.mesh.lods.push_back(std::move(lod));
    return res;
}

MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt) {
    return importMesh(parseFile(path), opt);
}

}  // namespace g2::xsi
