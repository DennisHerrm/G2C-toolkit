// g2/format.h — On-Disk-Layout der Ghoul2-Formate GLA (MDXA) und GLM (MDXM).
//
// Die Feldreihenfolge und -bedeutung stammt 1:1 aus Ravens mdx_format.h
// (Copyright 2000-2013 Raven Software / Activision, freigegeben unter GPLv2).
// Diese Datei definiert nur das Binaerlayout neu; sie enthaelt keinen Raven-Code.
//
// Wichtig: Alle Offsets sind relativ zur Adresse der Struktur, in der sie stehen,
// mit den Ausnahmen die unten explizit dokumentiert sind.

#pragma once

#include <cstdint>
#include <cstddef>

namespace g2::fmt {

// MAX_QPATH aus dem Q3-Erbe. Nicht aendern, steckt im Dateiformat.
inline constexpr int kMaxQPath = 64;

// Die Idents stehen im File als ASCII "2LGM" / "2LGA", was als Little-Endian
// uint32 gelesen ('M'<<24)|('G'<<16)|('L'<<8)|'2' ergibt.
inline constexpr std::uint32_t kMdxmIdent = ('M' << 24) | ('G' << 16) | ('L' << 8) | '2';
inline constexpr std::uint32_t kMdxaIdent = ('A' << 24) | ('G' << 16) | ('L' << 8) | '2';

inline constexpr int kMdxmVersion = 6;
inline constexpr int kMdxaVersion = 6;

// Bone-Flags
inline constexpr std::uint32_t kBoneFlagAlwaysXform = 0x00000001;

// Surface-Flags. Carcass erzeugt nur die ersten beiden.
inline constexpr std::uint32_t kSurfFlagIsBolt = 0x00000001;
inline constexpr std::uint32_t kSurfFlagOff    = 0x00000002;

// Vertex-Weighting-Konstanten.
// 5 Bit pro Bone-Referenz => maximal 32 Bone-Referenzen pro Surface.
// Genau dieses Limit taucht im Original-Binary bei 0x43fc70 als `cmp eax, 0x20` auf.
inline constexpr int kBitsPerBoneRef       = 5;
inline constexpr int kMaxBoneRefsPerSurface = 1 << kBitsPerBoneRef;  // 32
inline constexpr int kMaxWeightsPerVert     = 4;

// Gewichte werden als 10-Bit-Werte 0..1023 abgelegt: die unteren 8 Bit in
// BoneWeightings[i], die oberen 2 Bit gepackt in uiNmWeightsAndBoneIndexes
// an Bitposition 20+2*i.
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
    char         name[kMaxQPath];    // z.B. "models/players/_humanoid/_humanoid" (ohne Endung)
    float        scale;              // Buildscale; 0 bei sehr alten Dateien
    std::int32_t numFrames;
    std::int32_t ofsFrames;          // -> Array aus 3-Byte-Indizes, absolut ab Dateianfang
    std::int32_t numBones;
    std::int32_t ofsCompBonePool;    // -> Pool aus CompQuatBone, absolut ab Dateianfang
    std::int32_t ofsSkel;            // -> erste MdxaSkel, absolut ab Dateianfang
    std::int32_t ofsEnd;             // = Dateigroesse
};
static_assert(sizeof(MdxaHeader) == 100, "MdxaHeader muss 100 Bytes gross sein");

// 3x4-Matrix, zeilenweise: [row][col], col 3 ist die Translation.
struct MdxaBone {
    float matrix[3][4];
};
static_assert(sizeof(MdxaBone) == 48);

// Direkt hinter dem Header folgt ein int[numBones]. Die Werte sind Offsets
// relativ zum ENDE DES HEADERS (also zum Anfang genau dieses Arrays),
// nicht zum Dateianfang.
struct MdxaSkel {
    char         name[kMaxQPath];
    std::uint32_t flags;
    std::int32_t parent;             // -1 = Wurzel
    MdxaBone     basePoseMat;
    MdxaBone     basePoseMatInv;
    std::int32_t numChildren;
    // danach: std::int32_t children[numChildren]
};
static_assert(sizeof(MdxaSkel) == 172);

// Ein komprimierter Bone: 7 x uint16 little-endian.
//   [0..3] Quaternion w,x,y,z   -> f = raw/16383.0f - 2.0f
//   [4..6] Translation x,y,z    -> f = raw/64.0f    - 512.0f
struct CompQuatBone {
    std::uint8_t comp[14];
};
static_assert(sizeof(CompQuatBone) == 14);

#pragma pack(pop)

// Index eines (Frame, Bone) in den Bone-Pool: 3 Byte little-endian.
// Byteoffset = (frame * numBones + bone) * 3, relativ zu ofsFrames.
inline std::uint32_t readIndex24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16);
}
inline void writeIndex24(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
}

// Der 24-Bit-Index begrenzt den Bone-Pool hart.
inline constexpr std::uint32_t kMaxBonePoolEntries = 0xFFFFFF;

// ---------------------------------------------------------------------------
// GLM / MDXM
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct MdxmHeader {
    std::int32_t ident;                  // kMdxmIdent
    std::int32_t version;                // kMdxmVersion
    char         name[kMaxQPath];        // mit Endung, z.B. "models/players/x/model.glm"
    char         animName[kMaxQPath];    // ohne Endung, z.B. "models/players/_humanoid/_humanoid"
    std::int32_t animIndex;              // fuellt die Engine, Carcass schreibt 0
    std::int32_t numBones;
    std::int32_t numLODs;
    std::int32_t ofsLODs;                // absolut ab Dateianfang
    std::int32_t numSurfaces;
    std::int32_t ofsSurfHierarchy;       // absolut ab Dateianfang
    std::int32_t ofsEnd;                 // = Dateigroesse
};
static_assert(sizeof(MdxmHeader) == 164, "MdxmHeader muss 164 Bytes gross sein");

// Direkt hinter dem Header: int[numSurfaces], Offsets relativ zum Ende des Headers.
struct MdxmSurfHierarchy {
    char          name[kMaxQPath];
    std::uint32_t flags;
    char          shader[kMaxQPath];
    std::int32_t  shaderIndex;   // Engine-Feld, Carcass schreibt 0
    std::int32_t  parentIndex;   // -1 = Wurzel
    std::int32_t  numChildren;
    // danach: std::int32_t childIndexes[numChildren]
};
static_assert(sizeof(MdxmSurfHierarchy) == 144);  // 64+4+64+4+4+4, ohne childIndexes[]

struct MdxmLOD {
    std::int32_t ofsEnd;   // relativ zum Anfang dieser MdxmLOD
};

// Hinter jeder MdxmLOD: int[numSurfaces], Offsets relativ zum Anfang der MdxmLOD.

struct MdxmSurface {
    std::int32_t ident;              // Carcass schreibt 0
    std::int32_t thisSurfaceIndex;
    std::int32_t ofsHeader;          // negativ, zurueck zum MdxmHeader
    std::int32_t numVerts;
    std::int32_t ofsVerts;
    std::int32_t numTriangles;
    std::int32_t ofsTriangles;
    std::int32_t numBoneReferences;
    std::int32_t ofsBoneReferences;
    std::int32_t ofsEnd;             // relativ zum Anfang dieser MdxmSurface
};
static_assert(sizeof(MdxmSurface) == 40);

struct MdxmTriangle {
    std::int32_t indexes[3];
};

// Bewusst auf 32 Byte gehalten, wegen Cache-Alignment in der Engine.
struct MdxmVertex {
    float         normal[3];
    float         vertCoords[3];
    std::uint32_t weightsAndBoneIndexes;
    std::uint8_t  boneWeightings[kMaxWeightsPerVert];
};
static_assert(sizeof(MdxmVertex) == 32, "MdxmVertex muss 32 Bytes bleiben");

// Die UVs liegen in einem separaten Array HINTER allen MdxmVertex, ebenfalls
// aus Cache-Gruenden.
struct MdxmVertexTexCoord {
    float texCoords[2];
};
static_assert(sizeof(MdxmVertexTexCoord) == 8);

#pragma pack(pop)

// --- Vertex-Weight-Packing -------------------------------------------------

inline int getVertWeightCount(const MdxmVertex& v) {
    return static_cast<int>(v.weightsAndBoneIndexes >> 30) + 1;
}

inline int getVertBoneIndex(const MdxmVertex& v, int weightNum) {
    return static_cast<int>((v.weightsAndBoneIndexes >> (kBitsPerBoneRef * weightNum)) &
                            ((1u << kBitsPerBoneRef) - 1));
}

// Liefert das Gewicht als 10-Bit-Rohwert 0..1023.
inline int getVertBoneWeightRaw(const MdxmVertex& v, int weightNum) {
    int t = v.boneWeightings[weightNum];
    t |= (v.weightsAndBoneIndexes >> (kBoneWeightTopBitsShift + weightNum * 2)) & kBoneWeightTopBitsMask;
    return t;
}

}  // namespace g2::fmt
