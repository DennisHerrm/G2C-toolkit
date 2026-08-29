// g2/mdxm.h — GLM schreiben.

#pragma once

#include "g2/model.h"

#include <cstdint>
#include <vector>

namespace g2 {

struct MdxmWriteStats {
    std::uint64_t weightsDropped = 0;   // Vertices hatten mehr als 4 Gewichte
    std::uint64_t weightsRenormalized = 0;
    std::size_t   maxBoneRefsUsed = 0;
};

struct MdxmWriteResult {
    std::vector<std::uint8_t> data;
    MdxmWriteStats            stats;
};

// Schreibt eine GLM. Bone-Referenzen werden pro Surface automatisch gesammelt
// und auf lokale 5-Bit-Indizes gemappt.
MdxmWriteResult writeMdxm(const Mesh& mesh);

}  // namespace g2
