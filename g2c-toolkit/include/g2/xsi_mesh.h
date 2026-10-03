// g2/xsi_mesh.h - Reading a mesh from dotXSI.
//
// Counterpart to xsi_anim.h: there the animation, here the geometry.
// Verified against Raven's _humanoid.glm (84 surfaces, 2647 verts, 2846 tris).

#pragma once

#include "g2/model.h"
#include "g2/xsi.h"

#include <map>
#include <string>
#include <vector>

namespace g2::ase {
struct Scene;
}

namespace g2::xsi {

struct MeshImportOptions {
    // $scale from the .car. Must match the reference GLA.
    float scale = 1.0f;

    // Name of the GLM as written into the header.
    std::string modelName;

    // GLA reference without extension, e.g. "models/players/_humanoid/_humanoid".
    std::string animName;

    // Bone names of the reference skeleton. The envelope entries refer to
    // bones by name; without this list no index could be formed.
    std::vector<std::string> boneNames;

    // Renames reference -> dotXSI, as with the animation.
    std::map<std::string, std::string> aliases;

    // Carcass's -smooth: normals of vertices at the same place (within one
    // LOD, across surfaces) are averaged; implies loseDupVerts.
    bool smooth = false;
    // Carcass's -losedupverts: vertices that became identical are removed.
    bool loseDupVerts = false;
    // Smooth exactly as Carcass did, faults included: tags take part, and
    // opposite normals are added too (they cancel). For byte-identical files
    // with old builds; normally off.
    bool carcassSmooth = false;
};

struct MeshImportStats {
    std::size_t   surfaces = 0;
    std::size_t   vertices = 0;
    std::size_t   triangles = 0;
    std::size_t   tags = 0;          // surfaces with a * prefix
    std::size_t   offSurfaces = 0;   // surfaces with _off
    std::uint64_t splitVertices = 0; // created by attribute splitting
    std::size_t   lods = 0;
    // -losedupverts / -smooth: what was removed (for _info.txt).
    std::size_t   deletedDupVerts = 0;
    std::size_t   deletedDupWeights = 0;
    std::vector<std::size_t> deletedDupVertsPerLod;
    std::vector<std::size_t> deletedDupWeightsPerLod;
    std::vector<std::string> warnings;
    // Informational, not a problem: e.g. weights moved to a parent bone.
    std::vector<std::string> notes;
};

struct MeshImportResult {
    Mesh            mesh;
    MeshImportStats stats;
};

// Converts a surface name from the dotXSI into the GLM name, as Carcass:
//   - at most 63 characters, a LOD suffix "_<digit>" removed
//   - all lowercase: "Stupidtriangle_off" -> "stupidtriangle_off"
//   - prefix "bolt_" becomes "*":  "bolt_back" -> "*back"
//
// The second rule affects 46 of the 84 surfaces, and Raven's info file
// lists exactly 46 as "tags only".
std::string surfaceNameToGlm(const std::string& xsiName);

// Derives flags from the name:
//   * prefix     -> kSurfFlagIsBolt (1)
//   suffix _off  -> kSurfFlagOff (2)
// In the original file, 46 surfaces carry flag 1 and 17 carry flag 2 - both
// numbers appear exactly like that in Raven's info file.
std::uint32_t surfaceFlagsFromName(const std::string& glmName);

MeshImportResult importMesh(const Document& doc, const MeshImportOptions& opt);

// Names of all envelope deformers that carry at least one weight > 0, in
// file order without duplicates. These are the bones a new skeleton keeps
// (Carcass: "used by a surface").
std::vector<std::string> weightedDeformers(const Document& doc);
MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt);

// The same GLM from 3ds Max ASE (Carcass: "$aseanimconvertmdx" without
// _noask). Positions come from *MESH_BONE_VERTEX, normals are computed.
// Objects hang below their *NODE_PARENT, or below the first object - Carcass
// ignored the parent and could only build single-object models from ASE.
MeshImportResult importMeshAse(const ase::Scene& scene, const MeshImportOptions& opt);

}  // namespace g2::xsi
