// g2/mdxm.h - Writing GLM.

#pragma once

#include "g2/model.h"

#include <cstdint>
#include <vector>

namespace g2 {

struct MdxmWriteStats {
    std::uint64_t weightsDropped = 0;   // vertices had more than 4 weights
    std::uint64_t weightsRenormalized = 0;
    std::size_t   maxBoneRefsUsed = 0;
};

struct MdxmWriteResult {
    std::vector<std::uint8_t> data;
    MdxmWriteStats            stats;
};

// Writes a GLM. Bone references are collected automatically per surface and
// mapped to local 5-bit indices.
MdxmWriteResult writeMdxm(const Mesh& mesh);

// A GLM as read from disk: the mesh with global bone indices in the weights,
// plus the header fields the mesh structure has no place for.
struct MdxmFile {
    Mesh         mesh;
    std::int32_t animIndex = 0;
    std::int32_t version = 0;
    std::size_t  fileSize = 0;
    // Per LOD and surface: the bone reference table as stored (local -> global).
    std::vector<std::vector<std::vector<std::int32_t>>> boneRefs;
};

// Reads a GLM completely, every offset bounds-checked. Throws on a broken file.
MdxmFile readMdxm(const std::vector<std::uint8_t>& data);

// True if the data starts with the GLM ident "2LGM".
bool isMdxm(const std::vector<std::uint8_t>& data);

}  // namespace g2
