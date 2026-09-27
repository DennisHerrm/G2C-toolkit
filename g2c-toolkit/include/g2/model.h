// g2/model.h - Intermediate representation.
//
// Deliberately independent of dotXSI. Anyone exporting from 3ds Max,
// Blender, glTF or FBX fills these structures and calls the writers. That is
// the real reason a rewrite is worthwhile: Carcass keeps everything in ~65
// fixed slots of 1.59 MB each in static BSS (104 MB total), regardless of
// whether they hold three or a thousand vertices.

#pragma once

#include "g2/compress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

struct Bone {
    std::string   name;
    int           parent = -1;      // -1 = root
    Mat3x4        basePose = Mat3x4::identity();
    std::uint32_t flags = 0;
};

struct Skeleton {
    std::string       name;         // GLA name without extension
    float             scale = 0.0f; // 0 = "unknown", as in old files
    std::vector<Bone> bones;

    // Derive child lists from the parent fields.
    std::vector<std::vector<int>> buildChildLists() const;

    // Checks name collisions, parent indices and cycles.
    // Carcass reports this at 0x43a680 ("exists more than once in this file,
    // bad skeleton!"), but in some cases keeps running anyway.
    std::vector<std::string> validate() const;
};

// A frame holds a 3x4 matrix in parent space for every bone.
struct AnimationFrames {
    int                 numBones = 0;
    std::vector<Mat3x4> matrices;   // size: numFrames * numBones

    int  frameCount() const { return numBones ? static_cast<int>(matrices.size()) / numBones : 0; }
    Mat3x4&       at(int frame, int bone)       { return matrices[static_cast<std::size_t>(frame) * numBones + bone]; }
    const Mat3x4& at(int frame, int bone) const { return matrices[static_cast<std::size_t>(frame) * numBones + bone]; }
    void resize(int frames, int bones);
};

// --- Mesh ------------------------------------------------------------------

struct VertexWeight {
    int   boneIndex = 0;    // global bone index, mapped when writing
    float weight = 0.0f;
};

struct Vertex {
    float                     position[3]{};
    float                     normal[3]{};
    float                     uv[2]{};
    std::vector<VertexWeight> weights;   // at most 4 are written
};

struct Triangle {
    int indexes[3]{};
};

struct Surface {
    std::string           name;
    std::string           shader;
    int                   parentIndex = -1;
    std::uint32_t         flags = 0;
    std::vector<Vertex>   vertices;
    std::vector<Triangle> triangles;
};

struct LOD {
    std::vector<Surface> surfaces;   // must be the same length in every LOD
};

struct Mesh {
    std::string      name;       // with extension
    std::string      animName;   // GLA reference, without extension
    int              numBones = 0;
    std::vector<LOD> lods;

    std::vector<std::string> validate() const;
};

}  // namespace g2
