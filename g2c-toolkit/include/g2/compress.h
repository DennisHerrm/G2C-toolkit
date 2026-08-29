// g2/compress.h — Quantisierung der Bone-Matrizen in das 14-Byte-Format.
//
// Das ist die Stelle, an der Carcass v2.2 zwei echte Fehler hat. Details in
// docs/BUGS.md; die Kurzfassung:
//
//   B2  Carcass konvertiert mit _ftol, das Richtung Null abschneidet. Da vorher
//       konstant +2.0 bzw. +512.0 addiert wird, ist der Rundungsfehler immer
//       einseitig -> gerichtete Drift statt symmetrischem Rauschen.
//       Korrektes Runden halbiert den Fehler und ist voll formatkompatibel.
//
//   B3  Werte ausserhalb des Bereichs geben bei Carcass 0 zurueck, was beim
//       Dekodieren zu -2.0 (Quaternion) bzw. -512 Einheiten (Translation) wird.
//       Ein einzelner Ausreisser schleudert den Bone also ans absolute Extrem,
//       statt ihn zu klemmen. Zusaetzlich wird nur EINMAL pro Programmlauf
//       gewarnt, danach laeuft alles stumm durch.
//
// Nicht behebbar ohne Engine-Aenderung: der Quaternion-Wertebereich ist im
// Format auf -2..+2 festgelegt, obwohl Einheits-Quaternionen nur -1..+1
// brauchen. Ein Bit liegt also dauerhaft brach. Wer das aendert, muss auch
// MC_UnCompressQuat in der Engine anfassen und braucht eine neue GLA-Version.

#pragma once

#include "g2/format.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

// Ist die C++-Laufzeit fest eingebaut?
//
// Das ist die Frage, ob das Programm auf einem fremden Rechner ueberhaupt
// startet. Ohne fest eingebaute Laufzeit meldet Windows dort
// "VCRUNTIME140.dll wurde nicht gefunden", und der Empfaenger kann daran
// nichts aendern.
//
// MSVC setzt bei /MD (dynamisch) sowohl _MT als auch _DLL, bei /MT
// (statisch) nur _MT. Die Auskunft kostet nichts und muss nicht mit
// externen Werkzeugen erfragt werden.
constexpr bool staticRuntime() {
#if defined(_MSC_VER)
#if defined(_DLL)
    return false;
#else
    return true;
#endif
#else
    // Auf anderen Systemen stellt sich die Frage nicht so.
    return true;
#endif
}

// Datei schreiben und das Ergebnis wirklich pruefen.
//
// Ein blosses "if (!f)" nach dem Oeffnen genuegt nicht. Es faengt nur den
// Fall ab, dass die Datei gar nicht angelegt werden kann. Geht beim
// SCHREIBEN etwas schief — Platte voll, Netzlaufwerk weg, Kontingent
// erreicht —, meldet der Stream das erst beim naechsten Zugriff, und ein
// Teil der Daten steht schon auf der Platte. Das Ergebnis ist eine halbe
// GLA, die aussieht wie eine ganze und im Spiel als kaputtes Modell
// auffaellt.
//
// Deshalb: nach dem Schreiben schliessen und DANACH den Zustand pruefen.
// Erst close() leert den Puffer, also treten Schreibfehler oft genau dort
// zutage.
//
// Wirft std::runtime_error mit dem Pfad im Text.
void writeFileChecked(const std::string& path, const void* data, std::size_t size);
void writeFileChecked(const std::string& path, const std::string& text);

// 3x4-Matrix: Rotation in den Spalten 0..2, Translation in Spalte 3.
struct Mat3x4 {
    float m[3][4]{};

    static Mat3x4 identity() {
        Mat3x4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
        return r;
    }
};

struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class Rounding {
    Nearest,  // korrekt: rundet zur naechsten darstellbaren Stufe
    Legacy,   // reproduziert Carcass' _ftol-Truncation samt Null-Rueckgabe
};

// Zaehlt, was beim Komprimieren schiefging. Anders als bei Carcass geht keine
// einzige Verletzung verloren.
struct CompressStats {
    std::uint64_t quatClamped = 0;   // Quaternionkomponente ausserhalb -2..2
    std::uint64_t xlatClamped = 0;   // Translation ausserhalb -511..511
    std::uint64_t nonUnitQuat = 0;   // Quaternion war nicht normiert
    float         maxXlatSeen = 0.0f;

    bool clean() const { return quatClamped == 0 && xlatClamped == 0; }
    std::string summary() const;
};

// --- Skalare Quantisierung -------------------------------------------------

// Quaternionkomponente -> uint16.  raw = (f + 2.0) * 16383.0
std::uint16_t squashQuatComponent(float f, Rounding r, CompressStats& stats);

// Translationskomponente -> uint16.  raw = (f + 512.0) * 64.0
std::uint16_t squashXlatComponent(float f, Rounding r, CompressStats& stats);

float unsquashQuatComponent(std::uint16_t raw);
float unsquashXlatComponent(std::uint16_t raw);

// --- Matrix <-> Quaternion -------------------------------------------------

// Konvertiert den Rotationsteil in ein Quaternion. Verwendet Shepperds Methode,
// also den betragsmaessig groessten Term als Pivot, was bei Rotationen nahe 180
// Grad numerisch deutlich stabiler ist als die naive Trace-Formel.
Quat matrixToQuat(const Mat3x4& mat);

// Baut den Rotationsteil aus dem Quaternion. Entspricht exakt der Konvention,
// die die Engine in MC_UnCompressQuat verwendet.
void quatToMatrix(const Quat& q, Mat3x4& out);

Quat normalize(const Quat& q);
float dot(const Quat& a, const Quat& b);

// Winkel zwischen zwei Rotationen in Grad.
//
// Bewusst NICHT ueber 2*acos(dot): acos hat bei Argumenten nahe 1 eine
// unendliche Ableitung, sodass der Rundungsfehler von float bereits eine
// Messuntergrenze von rund 0.03 Grad erzeugt — deutlich mehr als der Fehler,
// den man eigentlich messen will. Stattdessen die stabile Form
// 2*atan2(|qa-qb|, |qa+qb|) in doppelter Genauigkeit.
double angleBetweenDeg(const Quat& a, const Quat& b);

// --- Bone-Kompression ------------------------------------------------------

struct CompressOptions {
    Rounding rounding = Rounding::Nearest;

    // Bei true wird das Vorzeichen des Quaternions so gewaehlt, dass w >= 0.
    // q und -q beschreiben dieselbe Rotation und dekodieren zur selben Matrix,
    // erzeugen aber unterschiedliche 14-Byte-Eintraege. Kanonisieren verbessert
    // damit die Trefferquote der Pool-Deduplizierung, ohne das Ergebnis zu
    // veraendern. Fuer byte-exakten Vergleich mit Carcass abschalten.
    bool canonicalizeSign = true;

    // Fehlerminimierende Quantisierung.
    //
    // MC_UnCompressQuat normalisiert das dekodierte Quaternion NICHT. Ein
    // Quantisierungsfehler aendert damit nicht nur die Rotation, sondern macht
    // die resultierende Matrix auch leicht nicht-orthonormal — die Bones werden
    // minimal geschert und skaliert. Das ist der Grund, warum der gemessene
    // Rotationsfehler rund achtmal groesser ausfaellt, als die Schrittweite
    // allein erwarten laesst.
    //
    // Komponentenweises Runden ist deshalb nicht optimal. Bei aktivierter
    // Option werden die 81 Kandidaten im Umkreis von +/-1 Stufe je Komponente
    // durchprobiert und derjenige gewaehlt, dessen dekodierte Matrix am
    // wenigsten von der Zielrotation abweicht. Kostet ca. 2500 Flops pro Bone
    // und veraendert das Dateiformat nicht.
    bool optimizeQuat = true;
};

fmt::CompQuatBone compressBone(const Mat3x4& mat, const CompressOptions& opt, CompressStats& stats);
Mat3x4            uncompressBone(const fmt::CompQuatBone& c);

// Maximaler absoluter Positionsfehler, den eine Kompression erzeugt hat.
// Nuetzlich fuer Regressionstests gegen Referenzmodelle.
float boneRoundTripError(const Mat3x4& original, const CompressOptions& opt);

}  // namespace g2
