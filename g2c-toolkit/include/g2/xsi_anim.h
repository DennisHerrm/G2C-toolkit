// g2/xsi_anim.h — Animationen aus dotXSI gegen ein vorhandenes Skelett.
//
// Variante "Referenz-GLA": Das Skelett wird NICHT aus der dotXSI abgeleitet,
// sondern aus einer vorhandenen GLA uebernommen. Grund: die Bone-Auswahl ist
// aus den XSI-Dateien nicht ableitbar.
//
// In den vorliegenden Raven-Assets sieht die Kaskade so aus:
//
//     191 Bones im Rig der root.xsi
//     102 davon in den Animationsdateien animiert
//      66 davon mit SI_Envelope, verformen also Geometrie
//      53 in der fertigen GLA
//
// Weggefallen sind zwischen 66 und 53 unter anderem die IK-Effektoren
// (eff, eff1, larm_eff, lhand_tag_eff), das jeweils dritte Fingerglied
// (l_d1_j3, l_d2_j3, l_d4_j3), ltarsal und ltlip1 — Raven musste die
// Bone-Zahl von Jedi Outcast zu Jedi Academy wegen der Xbox senken.
// Diese Auswahl steckt in keiner der Quelldateien; sie kommt aus einer
// .bonecap-Datei ($bonehiercap) oder eben aus der Referenz-GLA.
//
// Fuer den haeufigsten Fall — Animationen zu einem bestehenden _humanoid
// hinzufuegen — ist das ohnehin richtig, weil das Skelett dabei unveraendert
// bleiben MUSS. Jede Abweichung wuerde alle vorhandenen Modelle brechen.

#pragma once

#include "g2/compress.h"
#include "g2/model.h"
#include "g2/xsi.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace g2::xsi {

// Ein animierbarer Knoten aus der dotXSI-Hierarchie.
struct AnimNode {
    std::string name;        // Bonename, also der Teil nach dem letzten Punkt
    int         parent = -1; // Index in AnimFile::nodes, -1 = Wurzel

    // Keyframes je Kanal. Kanalnamen wie in der Datei:
    // ROTATION-X/Y/Z, TRANSLATION-X/Y/Z, SCALING-X/Y/Z
    std::map<std::string, std::map<int, float>> channels;

    // Statische lokale Transformation aus SI_Transform SRT-<name>:
    // Skalierung XYZ, Rotation XYZ in Grad, Translation XYZ.
    //
    // Das ist der Ruhewert eines Bones. FCurves ueberschreiben ihn kanalweise;
    // wo keine FCurve existiert, gilt er weiter. In Ravens Animationsdateien
    // hat jeder Bone FCurves fuer alle benutzten Kanaele, sodass der Block
    // dort nie gebraucht wird. In root.xsi dagegen haben nur 126 von 276
    // Modellen FCurves — die uebrigen 150 haengen allein an SRT.
    std::array<float, 9> srt{{1, 1, 1, 0, 0, 0, 0, 0, 0}};
    bool                 hasSrt = false;

    bool animated() const { return !channels.empty() || hasSrt; }
};

struct AnimFile {
    std::string           sourcePath;
    std::vector<AnimNode> nodes;
    int                   firstFrame = 0;
    int                   lastFrame = 0;

    // Aus SI_Scene: { "FRAMES", start, end, framerate }
    //
    // Das ist die maszgebliche Quelle fuer Framebereich UND Abspielrate.
    // Gibt eine .car-Zeile kein -framespeed an, schreibt Carcass genau diese
    // Rate in die animation.cfg. An drei echten Dateien geprueft:
    // torso_handsignal2 hat 0..72 bei Rate 20 und steht in Ravens
    // animation.cfg mit frameCount 73 und frameSpeed 20.
    float frameRate = 0.0f;
    bool  hasScene = false;

    // Framebereich laut SI_Scene. Gebaut wird mit dem Bereich der Keys
    // (firstFrame..lastFrame); weichen beide voneinander ab, ist die Datei
    // fehlerhaft exportiert. Carcass bricht dann mit "Header # frames = N,
    // but I read in M!!" ab, g2c baut mit den Keys und warnt.
    int sceneFirst = 0;
    int sceneLast = 0;
    bool sceneRangeDiffers() const {
        return hasScene && (sceneFirst != firstFrame || sceneLast != lastFrame);
    }

    int frameCount() const { return lastFrame - firstFrame + 1; }

    const AnimNode* find(const std::string& bone) const;
    int             indexOf(const std::string& bone) const;

    // Lokale Matrix eines Knotens in einem Frame.
    //
    // Wichtig: SI_FCurve-Werte sind LOKAL, also relativ zum Elternmodell —
    // anders als SI_Transform BASEPOSE-*, das absolut ist. Mit der Annahme
    // "absolut" passt nichts.
    Mat3x4 localMatrix(int node, int frame) const;

    // Weltposen aller Knoten in einem Frame, ueber die XSI-Hierarchie.
    std::vector<Mat3x4> worldMatrices(int frame) const;
};

// Liest Hierarchie und FCurves aus einem geparsten dotXSI-Dokument.
AnimFile loadAnimation(const Document& doc, const std::string& sourcePath = {});
AnimFile loadAnimationFile(const std::string& path);

// --- Auswertung gegen ein Referenzskelett ---------------------------------

struct EvalOptions {
    // $scale aus der .car. Muss mit dem Wert uebereinstimmen, mit dem die
    // Referenz-GLA gebaut wurde, sonst passen die Basisposen nicht.
    float scale = 1.0f;

    // -origin aus der .car. Wird als negative Translation auf den Wurzelbone
    // gelegt; in der echten _humanoid.gla steht bei model_root (0,0,-24)
    // passend zu "-origin 0 0 24".
    std::optional<std::array<float, 3>> origin;

    // Bones des Referenzskeletts, die in der Animationsdatei fehlen, bleiben
    // in ihrer Ruhepose. Bei true wird jeder fehlende Bone einmal gemeldet.
    bool warnMissingBones = true;

    // Umbenennungen: Schluessel ist der Bonename in der Referenz-GLA, Wert der
    // in der dotXSI. Wird gebraucht, weil Raven zwischen Builds umbenannt hat —
    // in der vorliegenden _humanoid.gla heisst der Bone "face", in root.xsi und
    // in den Animationsdateien dagegen "face_always_". Ohne die Zuordnung
    // bleiben der Bone und alle acht Gesichtsknochen darunter in der Ruhepose.
    std::map<std::string, std::string> aliases;

    // Wurzelbewegung.
    //
    // Carcass legt auf den Wurzelbone eine LINEARE RAMPE, die ueber die
    // Sequenz genau die Gesamtverschiebung des Motion-Bones abbaut — als
    // Gegenbewegung, also mit umgekehrtem Vorzeichen zur normalen
    // Positionsumrechnung. Die Engine addiert die Verschiebung dann selbst.
    //
    // Wichtig: es ist eine Rampe, NICHT die tatsaechliche Kurve des
    // Motion-Bones. An BOTH_DEATH17 und BOTH_SIT2TOSTAND5 geprueft — dort
    // schwankt Motion stark, der Wurzelbone laeuft trotzdem schnurgerade.
    //
    //     ramp(f) = -scale * C * (W_motion(last) - W_motion(first)) * f/(n-1)
    //
    // Ohne das bleibt die Figur bei Sterbe- und Aufstehanimationen auf der
    // Stelle stehen, statt sich zu verschieben.
    bool        extractRootMotion = true;
    std::string motionBone = "Motion";
};

struct EvalResult {
    AnimationFrames          frames;

    // Gesamte Wurzelverschiebung im GLA-Raum ueber die Sequenz. Geteilt durch
    // die Zahl der Schritte ergibt das den Wert, den Carcass als
    // "averagevec" in die .frames schreibt — dort allerdings mit umgekehrtem
    // Vorzeichen, weil die Rampe eine Gegenbewegung ist.
    float rootMotion[3] = {0.0f, 0.0f, 0.0f};
    std::vector<std::string> missingBones;   // im Skelett, nicht in der XSI
    std::vector<std::string> extraBones;     // in der XSI, nicht im Skelett
    int                      frameCount = 0;
};

// Rechnet eine Animationsdatei in Frames fuer das Referenzskelett um.
//
// Die Formel folgt der Engine. tr_ghoul2.cpp wertet
//     W(bone) = W(parent) * A(bone)
// aus, und die Skinning-Matrix ist X * B^-1. Umgestellt:
//     A(b) = B(parent) * X(parent)^-1 * X(b) * B(b)^-1
//
// B ist die basePoseMat aus der Referenz-GLA, X die konvertierte Weltpose
// aus den FCurves. Gegen BOTH_attack10.xsi und die echte _humanoid.gla
// geprueft: 840 Vergleiche, max. Rotationsabweichung 1,77e-4 bei einer
// Quantisierungsschrittweite von 6,10e-5.
EvalResult evaluate(const Skeleton& reference, const AnimFile& anim, const EvalOptions& opt = {});

// Haengt mehrere Animationen hintereinander, wie es $aseanimgrab tut.
struct ConcatResult {
    AnimationFrames frames;
    struct Entry {
        std::string name;         // Sequenzname
        int         targetFrame;  // Startframe in der Gesamtanimation
        int         frameCount;
        std::string sourceFile;
    };
    std::vector<Entry>       sequences;
    std::vector<std::string> warnings;
};

ConcatResult concatenate(const Skeleton& reference,
                         const std::vector<std::pair<std::string, AnimFile>>& named,
                         const EvalOptions& opt = {});

}  // namespace g2::xsi
