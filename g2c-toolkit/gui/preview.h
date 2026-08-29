// gui/preview.h — Kamera und Projektion fuer die Skelettvorschau.
//
// Bewusst ohne DirectX: gezeichnet wird mit ImGuis Zeichenliste, also mit
// Linien in Bildschirmkoordinaten. Das hat zwei Gruende.
//
// Erstens landet der Code damit in einer Datei, die sich hier uebersetzen
// und testen laesst — anders als gui/main_win32.cpp, in der heute schon
// mehrere Fehler unbemerkt durchgerutscht sind.
//
// Zweitens braucht ein Skelett keine Grafikschnittstelle. Ein paar hundert
// Linien pro Bild zeichnet ImGui ohne Muehe, und wir sparen uns Shader,
// Puffer und einen zweiten Renderpfad.

#pragma once

#include "g2/compress.h"
#include "g2/mdxa.h"

#include <array>
#include <vector>

namespace g2::gui {

// Umlaufende Kamera: Blickpunkt in der Mitte, Winkel und Abstand darum.
struct PreviewCamera {
    float yawDeg = 30.0f;
    float pitchDeg = 15.0f;
    float distance = 100.0f;
    std::array<float, 3> target{{0.0f, 0.0f, 0.0f}};

    // Sichtfeld in Grad, senkrecht.
    float fovDeg = 45.0f;
};

// Achsenparalleler Kasten um eine Punktwolke.
struct Bounds {
    std::array<float, 3> min{{0, 0, 0}};
    std::array<float, 3> max{{0, 0, 0}};
    bool                 valid = false;

    std::array<float, 3> center() const;
    float                radius() const;
};

Bounds computeBounds(const std::vector<Mat3x4>& world);

// Setzt die Kamera so, dass alles ins Bild passt.
//
// Ohne das steht die Kamera bei einem 53-Bone-Modell entweder im Kopf oder
// hundert Einheiten daneben — je nachdem, wie gross das Modell ist. Die
// Skalierung der GLA ist nicht einheitlich (0,6 bis 0,64), und eigene
// Modelle koennen deutlich groesser sein.
void frameAll(PreviewCamera& cam, const Bounds& b);

// Punkt in Bildschirmkoordinaten.
//
// Liefert false, wenn der Punkt hinter der Kamera liegt — dann darf die
// Linie nicht gezeichnet werden, sonst erscheint sie gespiegelt auf der
// falschen Seite.
bool projectPoint(const std::array<float, 3>& p, const PreviewCamera& cam, float viewW,
                  float viewH, float& outX, float& outY, float& outDepth);

// Eine zu zeichnende Verbindung zwischen zwei Bones.
struct BoneLine {
    int   from = 0;
    int   to = 0;
    float depth = 0.0f;   // mittlere Tiefe, zum Sortieren
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// Alle sichtbaren Verbindungen fuer einen Frame, von hinten nach vorn
// sortiert. Wurzelbones ohne Elternteil ergeben keine Linie.
std::vector<BoneLine> buildBoneLines(const MdxaFile& gla, const std::vector<Mat3x4>& world,
                                     const PreviewCamera& cam, float viewW, float viewH);

// Naechster Frame beim Abspielen.
//
// Laeuft mit der Rate der Sequenz, nicht mit der Bildrate der Oberflaeche:
// eine Animation mit 20 Bildern je Sekunde soll auch bei 144 Hz mit 20
// laufen. Negative Raten bedeuten rueckwaerts — das kommt in Ravens
// animation.cfg vor, etwa bei BOTH_UNCROUCH1 mit -20.
struct Playback {
    int   frame = 0;
    float accumulator = 0.0f;
    bool  playing = false;
    bool  loop = true;
};

// dt in Sekunden, fps aus der animation.cfg (darf negativ sein).
void advancePlayback(Playback& pb, float dt, int fps, int frameCount);

}  // namespace g2::gui
