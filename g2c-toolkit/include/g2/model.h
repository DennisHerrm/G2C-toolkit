// g2/model.h — Zwischenrepraesentation.
//
// Bewusst unabhaengig von dotXSI. Wer aus 3ds Max, Blender, glTF oder FBX
// exportiert, fuellt diese Strukturen und ruft die Writer auf. Das ist der
// eigentliche Grund, warum ein Rewrite lohnt: Carcass haelt alles in ~65
// festen Slots zu je 1,59 MB im statischen BSS (104 MB gesamt), unabhaengig
// davon ob drei oder tausend Vertices drin liegen.

#pragma once

#include "g2/compress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

struct Bone {
    std::string   name;
    int           parent = -1;      // -1 = Wurzel
    Mat3x4        basePose = Mat3x4::identity();
    std::uint32_t flags = 0;
};

struct Skeleton {
    std::string       name;         // GLA-Name ohne Endung
    float             scale = 0.0f; // 0 = "unbekannt", wie bei alten Dateien
    std::vector<Bone> bones;

    // Kinderlisten aus den parent-Feldern ableiten.
    std::vector<std::vector<int>> buildChildLists() const;

    // Prueft Namenskollisionen, Parent-Indizes und Zyklen.
    // Carcass meldet das in 0x43a680 ("exists more than once in this file,
    // bad skeleton!"), laesst aber teils trotzdem weiterlaufen.
    std::vector<std::string> validate() const;
};

// Ein Frame haelt fuer jeden Bone eine 3x4-Matrix im Elternraum.
struct AnimationFrames {
    int                 numBones = 0;
    std::vector<Mat3x4> matrices;   // Groesse: numFrames * numBones

    int  frameCount() const { return numBones ? static_cast<int>(matrices.size()) / numBones : 0; }
    Mat3x4&       at(int frame, int bone)       { return matrices[static_cast<std::size_t>(frame) * numBones + bone]; }
    const Mat3x4& at(int frame, int bone) const { return matrices[static_cast<std::size_t>(frame) * numBones + bone]; }
    void resize(int frames, int bones);
};

// --- Mesh ------------------------------------------------------------------

struct VertexWeight {
    int   boneIndex = 0;    // globaler Bone-Index, wird beim Schreiben gemappt
    float weight = 0.0f;
};

struct Vertex {
    float                     position[3]{};
    float                     normal[3]{};
    float                     uv[2]{};
    std::vector<VertexWeight> weights;   // maximal 4 werden geschrieben
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
    std::vector<Surface> surfaces;   // muss in jeder LOD gleich lang sein
};

struct Mesh {
    std::string      name;       // mit Endung
    std::string      animName;   // GLA-Referenz, ohne Endung
    int              numBones = 0;
    std::vector<LOD> lods;

    std::vector<std::string> validate() const;
};

}  // namespace g2
