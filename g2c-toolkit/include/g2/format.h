// g2/format.h - On-disk layout of the Ghoul2 formats GLA (MDXA) and GLM (MDXM).
//
// Field order and meaning are taken 1:1 from Raven's mdx_format.h
// (Copyright 2000-2013 Raven Software / Activision, released under GPLv2).
// This file only redefines the binary layout; it contains no Raven code.
//
// Important: all offsets are relative to the address of the struct they are
// in, except for the cases explicitly documented below.

#pragma once

#include <cstdint>
#include <cstddef>

namespace g2::fmt {

// MAX_QPATH inherited from Q3. Do not change, it is baked into the file format.
inline constexpr int kMaxQPath = 64;

// The idents are stored in the file as ASCII "2LGM" / "2LGA", which read as a
// little-endian uint32 gives ('M'<<24)|('G'<<16)|('L'<<8)|'2'.
inline constexpr std::uint32_t kMdxmIdent = ('M' << 24) | ('G' << 16) | ('L' << 8) | '2';
inline constexpr std::uint32_t kMdxaIdent = ('A' << 24) | ('G' << 16) | ('L' << 8) | '2';

inline constexpr int kMdxmVersion = 6;
inline constexpr int kMdxaVersion = 6;

// Bone flags
inline constexpr std::uint32_t kBoneFlagAlwaysXform = 0x00000001;

// Surface flags. Carcass only produces the first two.
inline constexpr std::uint32_t kSurfFlagIsBolt = 0x00000001;
inline constexpr std::uint32_t kSurfFlagOff    = 0x00000002;

// Vertex weighting constants.
// 5 bits per bone reference => at most 32 bone references per surface.
// Exactly this limit shows up in the original binary at 0x43fc70 as `cmp eax, 0x20`.
inline constexpr int kBitsPerBoneRef       = 5;
inline constexpr int kMaxBoneRefsPerSurface = 1 << kBitsPerBoneRef;  // 32
inline constexpr int kMaxWeightsPerVert     = 4;

// Weights are stored as 10-bit values 0..1023: the lower 8 bits in
// BoneWeightings[i], the upper 2 bits packed into uiNmWeightsAndBoneIndexes
// at bit position 20+2*i.
inline constexpr int   kBoneWeightTopBitsShift = (kBitsPerBoneRef * kMaxWeightsPerVert) - 8;  // 12
inline constexpr int   kBoneWeightTopBitsMask  = 0x300;
inline constexpr float kBoneWeightReciprocal   = 1.0f / 1023.0f;

// ---------------------------------------------------------------------------
// GLA / MDXA
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct MdxaHeader {
    std::int32_t ident;              // kMdxaIdent
    std::int32_t version;            // kMdxaVersion
    char         name[kMaxQPath];    // e.g. "models/players/_humanoid/_humanoid" (no extension)
    float        scale;              // build scale; 0 in very old files
    std::int32_t numFrames;
    std::int32_t ofsFrames;          // -> array of 3-byte indices, absolute from file start
    std::int32_t numBones;
    std::int32_t ofsCompBonePool;    // -> pool of CompQuatBone, absolute from file start
    std::int32_t ofsSkel;            // -> first MdxaSkel, absolute from file start
    std::int32_t ofsEnd;             // = file size
};
static_assert(sizeof(MdxaHeader) == 100, "MdxaHeader muss 100 Bytes gross sein");

// 3x4 matrix, row-major: [row][col], col 3 is the translation.
struct MdxaBone {
    float matrix[3][4];
};
static_assert(sizeof(MdxaBone) == 48);

// Directly after the header comes an int[numBones]. The values are offsets
// relative to the END OF THE HEADER (i.e. to the start of this very array),
// not to the start of the file.
struct MdxaSkel {
    char         name[kMaxQPath];
    std::uint32_t flags;
    std::int32_t parent;             // -1 = root
    MdxaBone     basePoseMat;
    MdxaBone     basePoseMatInv;
    std::int32_t numChildren;
    // followed by: std::int32_t children[numChildren]
};
static_assert(sizeof(MdxaSkel) == 172);

// One compressed bone: 7 x uint16 little-endian.
//   [0..3] Quaternion w,x,y,z   -> f = raw/16383.0f - 2.0f
//   [4..6] Translation x,y,z    -> f = raw/64.0f    - 512.0f
struct CompQuatBone {
    std::uint8_t comp[14];
};
static_assert(sizeof(CompQuatBone) == 14);

#pragma pack(pop)

// Index of a (frame, bone) into the bone pool: 3 bytes little-endian.
// Byte offset = (frame * numBones + bone) * 3, relative to ofsFrames.
inline std::uint32_t readIndex24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16);
}
inline void writeIndex24(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
}

// The 24-bit index puts a hard limit on the bone pool.
inline constexpr std::uint32_t kMaxBonePoolEntries = 0xFFFFFF;

// ---------------------------------------------------------------------------
// GLM / MDXM
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct MdxmHeader {
    std::int32_t ident;                  // kMdxmIdent
    std::int32_t version;                // kMdxmVersion
    char         name[kMaxQPath];        // with extension, e.g. "models/players/x/model.glm"
    char         animName[kMaxQPath];    // no extension, e.g. "models/players/_humanoid/_humanoid"
    std::int32_t animIndex;              // filled in by the engine, Carcass writes 0
    std::int32_t numBones;
    std::int32_t numLODs;
    std::int32_t ofsLODs;                // absolute from file start
    std::int32_t numSurfaces;
    std::int32_t ofsSurfHierarchy;       // absolute from file start
    std::int32_t ofsEnd;                 // = file size
};
static_assert(sizeof(MdxmHeader) == 164, "MdxmHeader muss 164 Bytes gross sein");

// Directly after the header: int[numSurfaces], offsets relative to the end of the header.
struct MdxmSurfHierarchy {
    char          name[kMaxQPath];
    std::uint32_t flags;
    char          shader[kMaxQPath];
    std::int32_t  shaderIndex;   // engine field, Carcass writes 0
    std::int32_t  parentIndex;   // -1 = root
    std::int32_t  numChildren;
    // followed by: std::int32_t childIndexes[numChildren]
};
static_assert(sizeof(MdxmSurfHierarchy) == 144);  // 64+4+64+4+4+4, without childIndexes[]

struct MdxmLOD {
    std::int32_t ofsEnd;   // relative to the start of this MdxmLOD
};

// After each MdxmLOD: int[numSurfaces], offsets relative to the start of the MdxmLOD.

struct MdxmSurface {
    std::int32_t ident;              // Carcass writes 0
    std::int32_t thisSurfaceIndex;
    std::int32_t ofsHeader;          // negative, back to the MdxmHeader
    std::int32_t numVerts;
    std::int32_t ofsVerts;
    std::int32_t numTriangles;
    std::int32_t ofsTriangles;
    std::int32_t numBoneReferences;
    std::int32_t ofsBoneReferences;
    std::int32_t ofsEnd;             // relative to the start of this MdxmSurface
};
static_assert(sizeof(MdxmSurface) == 40);

struct MdxmTriangle {
    std::int32_t indexes[3];
};

// Deliberately kept at 32 bytes, for cache alignment in the engine.
struct MdxmVertex {
    float         normal[3];
    float         vertCoords[3];
    std::uint32_t weightsAndBoneIndexes;
    std::uint8_t  boneWeightings[kMaxWeightsPerVert];
};
static_assert(sizeof(MdxmVertex) == 32, "MdxmVertex muss 32 Bytes bleiben");

// The UVs live in a separate array AFTER all MdxmVertex entries, also for
// cache reasons.
struct MdxmVertexTexCoord {
    float texCoords[2];
};
static_assert(sizeof(MdxmVertexTexCoord) == 8);

#pragma pack(pop)

// --- Vertex weight packing -------------------------------------------------

inline int getVertWeightCount(const MdxmVertex& v) {
    return static_cast<int>(v.weightsAndBoneIndexes >> 30) + 1;
}

inline int getVertBoneIndex(const MdxmVertex& v, int weightNum) {
    return static_cast<int>((v.weightsAndBoneIndexes >> (kBitsPerBoneRef * weightNum)) &
                            ((1u << kBitsPerBoneRef) - 1));
}

// Returns the weight as a raw 10-bit value 0..1023.
inline int getVertBoneWeightRaw(const MdxmVertex& v, int weightNum) {
    int t = v.boneWeightings[weightNum];
    t |= (v.weightsAndBoneIndexes >> (kBoneWeightTopBitsShift + weightNum * 2)) & kBoneWeightTopBitsMask;
    return t;
}

}  // namespace g2::fmt
