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

    // Tolerances for vertex merging.
    //
    // dotXSI has a separate index array per attribute, GLM only one index
    // per vertex. Merging is done by position index, UV and normal - but NOT
    // via a hash key, rather by a greedy search with tolerance: the first
    // matching candidate wins.
    //
    // The difference is significant. With exact equality too many vertices
    // are created, and no rounding grid can compensate for that - the merge
    // is order-dependent, not value-discrete. mrwonko's Blender exporter uses
    // the same rule.
    float normalTolerance = 0.05f;
    // 0.002 instead of exact equality: measured across all 84 surfaces, the
    // best value (81 exact vs. 77 with exact equality). Larger values do hit
    // the overall total, but only through errors that cancel out.
    float uvTolerance = 0.002f;
};

struct MeshImportStats {
    std::size_t   surfaces = 0;
    std::size_t   vertices = 0;
    std::size_t   triangles = 0;
    std::size_t   tags = 0;          // surfaces with a * prefix
    std::size_t   offSurfaces = 0;   // surfaces with _off
    std::uint64_t splitVertices = 0; // created by attribute splitting
    std::vector<std::string> warnings;
};

struct MeshImportResult {
    Mesh            mesh;
    MeshImportStats stats;
};

// Converts a surface name from the dotXSI into the GLM name.
//
// Two rules, derived from Raven's _humanoid.glm:
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
MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt);

}  // namespace g2::xsi
