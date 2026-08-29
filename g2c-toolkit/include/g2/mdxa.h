// g2/mdxa.h — GLA schreiben und lesen.

#pragma once

#include "g2/compress.h"
#include "g2/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

// Echte affine Inverse einer 3x4-Matrix. Wird fuer MdxaSkel::basePoseMatInv
// gebraucht; die Transponierte reicht nicht, sobald Skalierung im Spiel ist.
Mat3x4 affineInverse(const Mat3x4& m);

// Verkettung zweier affiner 3x4-Matrizen.
//
// Lag frueher lokal in xsi_anim.cpp. Als der Exporteur dieselbe Rechnung
// brauchte, waere eine Kopie der naheliegende Weg gewesen — und genau so
// entstehen zwei Fassungen, von denen spaeter eine korrigiert wird.
Mat3x4 mul(const Mat3x4& a, const Mat3x4& b);

struct MdxaWriteOptions {
    CompressOptions compress{};

    // Identische komprimierte Bones teilen sich einen Pool-Eintrag. Bei
    // Idle-Animationen und langen Haltephasen spart das erheblich; bei
    // _humanoid.gla liegen tausende Frames mit unbewegten Fingern vor.
    bool dedupeBonePool = true;

    // 0 = alle verfuegbaren Kerne. Parallelisiert wird nur die Kompression;
    // der Poolaufbau bleibt seriell, damit die Eintragsreihenfolge und damit
    // die erzeugte Datei reproduzierbar bleibt.
    unsigned threads = 0;
};

struct MdxaWriteResult {
    std::vector<std::uint8_t> data;
    CompressStats             stats;
    std::size_t               poolEntries = 0;
    std::size_t               poolEntriesBeforeDedupe = 0;

    double dedupeRatio() const {
        return poolEntriesBeforeDedupe
                   ? 1.0 - static_cast<double>(poolEntries) /
                               static_cast<double>(poolEntriesBeforeDedupe)
                   : 0.0;
    }
};

MdxaWriteResult writeMdxa(const Skeleton& skel, const AnimationFrames& frames,
                          const MdxaWriteOptions& opt = {});

// --- Lesen -----------------------------------------------------------------

struct MdxaFile {
    Skeleton                       skeleton;
    int                            numFrames = 0;
    std::vector<fmt::CompQuatBone> bonePool;
    std::vector<std::uint32_t>     indices;   // numFrames * numBones

    Mat3x4 boneMatrix(int frame, int bone) const;
};

MdxaFile readMdxa(const std::vector<std::uint8_t>& data);

// Weltmatrizen aller Bones fuer einen Frame.
//
// Die GLA speichert je Bone eine Matrix RELATIV zum Elternbone, mit der
// Bindpose verrechnet. Die Weltpose ergibt sich aus
//
//     Wurzel:  X(b) = A(b)·B(b)
//     sonst:   X(b) = X(p)·B(p)^-1·A(b)·B(b)
//
// Die Reihenfolge muss topologisch sein, nicht nach Index: in Ravens
// _humanoid.gla haben acht Bones ihren Elternbone HINTER sich.
//
// Lag frueher dreimal im Quelltext — im Exporteur, in der Restbewegung und
// in der Vorschau. Genau so entstehen Fassungen, von denen spaeter eine
// korrigiert wird.
std::vector<Mat3x4> boneWorldMatrices(const MdxaFile& gla, int frame);

// Liest Datei, schreibt sie neu, vergleicht. Das ist die Grundlage fuer die
// Regressionstests gegen Originalassets.
struct RoundTripReport {
    int    frames = 0;
    int    bones = 0;
    double maxPositionError = 0.0;
    double maxRotationErrorDeg = 0.0;
    double meanPositionError = 0.0;
    std::string describe() const;
};

RoundTripReport compareRoundTrip(const MdxaFile& original, const MdxaWriteOptions& opt);

}  // namespace g2
