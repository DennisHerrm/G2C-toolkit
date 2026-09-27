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

}  // namespace g2
