// g2/gladiff.h — Zwei GLA-Dateien vollstaendig vergleichen.
//
// Stichproben finden nur, was haeufig ist. Bei 1,6 Millionen Bone-Instanzen
// kann eine Abweichung, die genau eine Sequenz betrifft, in 800 zufaelligen
// Frames komplett untergehen — und genau solche Faelle waren in diesem
// Projekt die interessanten (die Wurzelbewegung betraf 7 % der Frames, der
// SRT-Rueckfall dagegen fast alle).
//
// Dieser Vergleich geht ueber jede einzelne Instanz und ordnet die
// Abweichungen anschliessend Bones und Sequenzen zu, damit man sieht, WO das
// Problem sitzt statt nur DASS eines existiert.

#pragma once

#include "g2/carscript.h"
#include "g2/mdxa.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

struct DiffOptions {
    // Ab welcher Abweichung gilt eine Bone-Instanz als auffaellig.
    //
    // Die Rotation wird als echter Winkel in Grad gemessen, nicht als
    // Differenz einzelner Matrixelemente. Letzteres ist kaum zu deuten: ein
    // halber Quantisierungsschritt je Quaternionkomponente pflanzt sich ueber
    // die Produkte in der Matrix zu einem Vielfachen fort, sodass praktisch
    // jeder Bone auffaellig erscheint, obwohl der tatsaechliche Winkelfehler
    // weit unter einem Hundertstelgrad liegt.
    //
    // 0,1 Grad und zwei Quantisierungsstufen der Translation liegen jeweils
    // klar ueber dem, was zwei unterschiedliche Rundungsverfahren erzeugen,
    // und klar unter dem, was man sehen kann.
    double toleranceTranslation = 2.0 / 64.0;
    double toleranceRotationDeg = 0.1;

    // Frameversatz der zweiten Datei gegenueber der ersten. Nuetzlich, wenn
    // eine Teilsequenz gegen eine vollstaendige GLA geprueft wird.
    int frameOffsetB = 0;

    unsigned threads = 0;
};

struct BoneDiff {
    std::string name;
    std::uint64_t instances = 0;
    std::uint64_t outliers = 0;
    double        maxRotationDeg = 0.0;
    double        maxTranslation = 0.0;
    int           worstFrame = -1;

    double outlierPercent() const {
        return instances ? 100.0 * static_cast<double>(outliers) / static_cast<double>(instances) : 0.0;
    }
};

struct SequenceDiff {
    std::string   name;
    int           targetFrame = 0;
    int           frameCount = 0;
    std::uint64_t outliers = 0;
    double        maxDeviation = 0.0;      // Translation, Einheiten
    double        maxRotationDeg = 0.0;
};

struct DiffResult {
    bool                     skeletonIdentical = false;
    std::vector<std::string> skeletonNotes;

    int frames = 0;
    int bones = 0;

    std::uint64_t instances = 0;
    std::uint64_t outliers = 0;
    double        maxRotationDeg = 0.0;
    double        maxTranslation = 0.0;
    double        meanTranslation = 0.0;
    double        meanRotationDeg = 0.0;

    std::vector<BoneDiff>     perBone;
    std::vector<SequenceDiff> perSequence;   // nur wenn eine animation.cfg vorlag

    double outlierPercent() const {
        return instances ? 100.0 * static_cast<double>(outliers) / static_cast<double>(instances) : 0.0;
    }
    bool clean() const { return outliers == 0; }
};

// Vergleicht zwei GLA. Die Sequenzliste ist optional; ohne sie bleibt
// perSequence leer.
DiffResult diffMdxa(const MdxaFile& a, const MdxaFile& b,
                    const std::vector<car::Sequence>& sequences = {},
                    const DiffOptions& opt = {});

// Liest eine animation.cfg zurueck. Nur Name, targetFrame, frameCount,
// loopFrame und frameSpeed; Kommentarzeilen werden uebersprungen.
std::vector<car::Sequence> readAnimationCfg(const std::string& text);

}  // namespace g2
