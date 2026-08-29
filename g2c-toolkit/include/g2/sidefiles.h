// g2/sidefiles.h — Die beiden Begleitdateien, die Carcass neben GLA und GLM
// erzeugt.
//
// Beide Formate sind an Ravens Originalen abgelesen: `model_red.skin` aus
// dem Luke-Modell und `_humanoid.frames` aus einem echten Carcass-Lauf.

#pragma once

#include "g2/carscript.h"
#include "g2/model.h"

#include <string>
#include <vector>

namespace g2 {

// --- .skin ----------------------------------------------------------------
//
// Zeilenweise "surfacename,texturpfad", Zeilenende CRLF:
//
//     hips,models/players/luke/boots_hips_red.tga
//     l_leg,models/players/luke/boots_hips_red.tga
//     l_leg_cap_hips_off,models/players/stormtrooper/caps.tga
//
// Eingetragen werden alle Surfaces, die KEINE Tags sind. An Ravens
// `model_red.skin` geprueft: 34 Eintraege bei 80 Surfaces, von denen 46 Tags
// sind — also genau die uebrigen 34, abzueglich `stupidtriangle_off`, das
// "[nomaterial]" traegt.
//
// Die Datei ist kein Ghoul2-Format im engeren Sinn, sondern eine
// Texturzuordnung, die das Spiel zur Laufzeit liest. Deshalb kommt es hier
// auf den Shadernamen an und nicht auf die Geometrie.
std::string writeSkin(const Mesh& mesh);

// --- .frames --------------------------------------------------------------
//
// Ein Block je eingesammelter Animationsdatei:
//
//     <leerzeile>
//     c:/pfad/zur/quelle.xsi
//     {
//         "startframe"  "0"
//         "duration"    "2"
//         "fps"         "60"
//         "averagevec"  "0.000 0.000 0.000"
//     }
//
// Schluessel und Werte sind jeweils in Anfuehrungszeichen und durch einen
// Tabulator getrennt, die Zeilen mit einem Tabulator eingerueckt.
//
// `fps` ist die Framerate aus SI_Scene der Quelldatei, NICHT der
// framespeed der animation.cfg: bei `face_alert.xsi` steht hier 60,
// waehrend die .car per -framespeed 1 vorgibt.
//
// `averagevec` ist die Wurzelbewegung **pro Frame**, mit umgekehrtem
// Vorzeichen zur Rampe auf dem Wurzelbone. Gegengeprueft:
//
//   both_strafe_left1   averagevec 3.520   Rampe -42.25/12 = -3.521
//   both_death17        averagevec 0.028   Rampe -3.444/124 = -0.0278
//   both_sit2tostand5   averagevec -0.084  Rampe +4.62/55  = +0.084
//   both_wall_flip_right averagevec 0      keine Bewegung
struct FrameEntry {
    std::string sourcePath;   // wie im Skript referenziert, absolut
    int         startFrame = 0;
    int         duration = 0;
    int         fps = 0;
    float       averageVec[3] = {0.0f, 0.0f, 0.0f};
};

std::string writeFrames(const std::vector<FrameEntry>& entries);

}  // namespace g2
