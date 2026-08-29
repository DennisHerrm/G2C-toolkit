// g2/carbuild.h — Ein .car-Skript abarbeiten.
//
// Setzt den Parser aus carscript.h und die Auswertung aus xsi_anim.h zu dem
// zusammen, was Carcass beim Aufruf tut: alle $aseanimgrab einsammeln,
// hintereinanderhaengen, komprimieren, GLA und animation.cfg schreiben.
//
// Variante 2: Das Skelett kommt aus einer Referenz-GLA, nicht aus den
// Quelldateien. Warum, steht in xsi_anim.h.

#pragma once

#include "g2/carscript.h"
#include "g2/model.h"
#include "g2/animcache.h"
#include "g2/xsi_anim.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace g2::car {

struct BuildOptions {
    // Wurzel, unter der die Pfade aus $aseanimgrab liegen. Die Pfade in der
    // .car sehen aus wie "models/players/__new_anim/...", sind also relativ
    // zum Assets-Stamm. Bleibt das Feld leer, wird $basedir aus dem Skript
    // benutzt, und falls auch das fehlt, das Verzeichnis der .car.
    std::string baseDir;

    // Ueberschreibt das -origin aus $aseanimconvertmdx. Ohne Angabe wird der
    // Wert aus dem Skript genommen.
    std::optional<std::array<float, 3>> originOverride;

    // Fehlende .xsi-Dateien ueberspringen statt abzubrechen. Standardmaessig
    // aus: eine fehlende Datei verschiebt alle nachfolgenden Zielframes und
    // macht damit die gesamte animation.cfg falsch.
    bool skipMissing = false;

    // Letzter Rueckfall fuer framespeed: greift nur, wenn eine Grab-Zeile
    // kein -framespeed hat UND die .xsi kein SI_Scene mit Framerate.
    int defaultFrameSpeed = 30;

    // Bitkompatibel zu Carcass quantisieren: Abschneiden statt Runden, keine
    // Kandidatensuche, kein Vorzeichen-Kanonisieren.
    //
    // Kostet Genauigkeit (Rotationsfehler etwa Faktor 3), macht den Bone-Pool
    // aber wieder so dicht wie bei Carcass. Nur sinnvoll, wenn die Dateigroesse
    // wirklich zaehlt oder man Ausgaben byteweise vergleichen will.
    bool carcassCompatible = false;

    // 0 = alle verfuegbaren Kerne. Die Dateien werden unabhaengig voneinander
    // gelesen, geparst und ausgewertet; das laesst sich vollstaendig
    // parallelisieren. Zusammengehaengt wird danach wieder in Skriptreihenfolge,
    // damit die Zielframes stimmen.
    unsigned threads = 0;

    // Ordner fuer den Zwischenspeicher der geparsten .xsi. Leer = aus.
    // Gemessen entfallen 96 % der Bauzeit auf Lesen und Parsen; genau das
    // faellt beim zweiten Lauf weg.
    std::string cacheDir;

    // Wird nach jeder geladenen Datei aufgerufen (Fortschritt, Gesamtzahl,
    // Name). Wird aus mehreren Threads gerufen und ist bereits serialisiert.
    std::function<void(std::size_t, std::size_t, const std::string&)> progress;
};

// Ein Block der .frames-Datei, ein Eintrag je eingesammelter Quelldatei.
struct FrameBlock {
    std::string sourcePath;      // aufgeloester, absoluter Pfad
    int         startFrame = 0;
    int         duration = 0;
    int         fps = 0;         // Rate aus SI_Scene, NICHT der framespeed
    float       averageVec[3] = {0.0f, 0.0f, 0.0f};
};

struct BuildResult {
    AnimationFrames          frames;
    std::vector<FrameBlock>  frameBlocks;
    std::vector<Sequence>    sequences;
    // Fehlende Dateien mit Zuordnung.
    //
    // Nur der Pfad reicht nicht: bei 1393 Grabs sagt "eine Datei fehlt"
    // nichts darueber, welche Animation betroffen ist. Der Sequenzname ist
    // das, wonach der Nutzer in seiner .car sucht.
    struct MissingFile {
        std::string file;       // wie in der .car
        std::string sequence;   // Enum bzw. abgeleiteter Name
        std::size_t grabIndex = 0;
        std::size_t line = 0;   // Zeile in der .car, 0 = unbekannt
    };
    std::vector<MissingFile> missing;

    // Nur die Pfade, fuer bestehenden Code.
    std::vector<std::string> missingFiles;
    std::vector<std::string> warnings;
    bool                     carcassCompatible = false;
    AnimCacheStats           cache;

    int totalFrames() const { return frames.frameCount(); }
};

// Loest einen Pfad aus dem Skript gegen die Basis auf. Probiert der Reihe
// nach: <basis>/<pfad>, <pfad> direkt, <verzeichnis der .car>/<pfad>.
// Liefert einen leeren String, wenn nichts gefunden wird.
std::string resolveAssetPath(const std::string& relative, const std::string& baseDir,
                             const std::string& carDir);

// Versucht, Assetwurzel und Referenz-GLA aus der Lage der .car abzuleiten.
//
// Die Pfade in einer .car sehen aus wie "models/players/...". Liegt die .car
// selbst irgendwo unterhalb eines "models"-Ordners, ist dessen Elternordner
// die gesuchte Wurzel. Das Referenzskelett steht in -makeskel, dort ohne
// Endung.
struct AutoPaths {
    std::string baseDir;        // leer, wenn nicht gefunden
    std::string referenceGla;   // leer, wenn nicht gefunden
    std::string note;           // was gefunden wurde, fuer die Ausgabe
};
AutoPaths guessPaths(const Script& script, const std::string& carPath);

// Sucht die Assetwurzel, indem vom Verzeichnis der .car aus nach oben
// gegangen wird, bis der erste $aseanimgrab-Pfad aufgeht.
//
// Beispiel: liegt die .car unter
//   C:/jka_animations/md/base/models/players/__new_anim/_humanoid/
// und zeigt der erste Grab auf
//   models/players/__new_anim/__original_anim/both_a1.xsi
// dann ist die Wurzel C:/jka_animations/md/base.
//
// Liefert einen leeren String, wenn nichts passt.
std::string guessBaseDir(const Script& script, const std::string& carPath, int maxLevels = 10);

// Leitet die Referenz-GLA aus -makeskel ab: <basis>/<makeskel>.gla
// Liefert einen leeren String, wenn die Datei nicht existiert.
std::string guessReferenceGla(const Script& script, const std::string& baseDir);

BuildResult build(const Script& script, const Skeleton& reference, const std::string& carPath,
                  const BuildOptions& opt = {});

}  // namespace g2::car
