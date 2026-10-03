// g2/ase.h - 3ds Max ASCII export (ASE / .ask) with Raven's weight
// extension, and the Raven MDR model Carcass builds from it.
//
// Carcass reads ASE in two places: "$aseanimconvertmdx" (without _noask)
// builds a GLM from it, "$aseconvert" / "$aseanimconvert" build an MDR. Both
// take the vertex positions from Raven's *MESH_WEIGHTS block, not from
// *MESH_VERTEX (re/mdr/SPEC.md). Carcass's faults are not copied: quoted names
// keep their first character and their blanks, ".ase" files are accepted,
// objects without weights are an error instead of collapsing to the origin,
// and an MDR is written in the format the MDR loader actually reads.

#pragma once

#include "g2/model.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace g2::ase {

struct BoneWeight {
    int   bone = 0;      // index into Scene::bones
    float weight = 0.0f;
};

struct Object {
    std::string name;       // as Carcass uses it: lower case, tag names trimmed
    std::string rawName;
    std::string parent;     // *NODE_PARENT, lower case ("" = none)
    std::vector<std::array<int, 3>>   faces;    // *MESH_FACE A B C
    std::vector<std::array<int, 3>>   tfaces;   // *MESH_TFACE
    std::vector<std::array<float, 2>> tverts;   // (u, 1 - v)
    std::vector<std::array<float, 3>> meshVerts;   // *MESH_VERTEX (only for the mismatch check)
    struct BoneVertex {
        std::array<float, 3>    p{};
        std::vector<BoneWeight> w;   // in file order, up to the first negative bone
    };
    std::vector<BoneVertex> boneVerts;   // *MESH_BONE_VERTEX - the positions that count
    bool hasWeights = false;
    int  materialRef = -1;
};

struct Scene {
    std::vector<std::string> materials;   // material name per *MATERIAL, in order
    std::vector<std::string> bones;       // all bone names, first spelling
    std::vector<Object>      objects;     // without ignore_ / Bip objects
    std::vector<std::string> warnings;
};

// baseDir: the asset root; a *BITMAP below it becomes "models/.../x.tga".
Scene parse(const std::string& text, const std::string& baseDir);
Scene parseFile(const std::string& path, const std::string& baseDir);

// The ASE file a convert line names: <token> as given (".ask"/".ase"), else
// with ".ASK"/".ask"/".ase" appended. Empty if none exists.
std::string findAseFile(const std::string& token, const std::string& baseDir, const std::string& carDir);

// Bones the objects of LOD 0..9 reference (weight entries before the first
// negative bone, weight 0 included - Carcass counts those as use).
std::vector<std::string> referencedBones(const Scene& scene);

// --- MDR --------------------------------------------------------------------

struct MdrInput {
    const Scene*    scene = nullptr;
    const Skeleton* skeleton = nullptr;
    // Per frame and skeleton bone: the bone's model-space matrix relative to
    // its base pose (world * basePose^-1), with the 90 degree turn Carcass
    // always applies to MDR.
    std::vector<std::vector<Mat3x4>> frames;
    std::array<float, 3> originAdjust{0.0f, 0.0f, 0.0f};   // -origin as (y, -x, -z)
    std::string          name = "test.mdr";                // header name
};

struct MdrResult {
    std::vector<std::uint8_t> data;
    std::string               skin;     // <surface>,<shader> lines, CRLF
    std::vector<std::string>  warnings;
    std::size_t               bones = 0, frames = 0, surfaces = 0, vertices = 0, triangles = 0;
};

// Builds the MDR as Carcass's "mode 0" (no -playerparms/-weapon): one LOD,
// every object except LOD copies "_2".."_9"/"_0", no tags, uncompressed
// frames (Carcass's compressed ones are unreadable, SPEC B1).
MdrResult buildMdr(const MdrInput& in);

}  // namespace g2::ase
