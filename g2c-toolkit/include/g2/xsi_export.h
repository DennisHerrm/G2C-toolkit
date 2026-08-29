// include/g2/xsi_export.h — Animationen aus einer GLA zurueck nach dotXSI.
//
// Die Umkehrung von xsi_anim.cpp. Die Vorwaertsformel dort lautet
//
//     Wurzel:  A(b) = X(b)·B(b)^-1              [+ origin, + Wurzelrampe]
//     sonst:   A(b) = B(p)·X(p)^-1·X(b)·B(b)^-1
//
// und laesst sich eindeutig umstellen:
//
//     Wurzel:  X(b) = A(b)·B(b)
//     sonst:   X(b) = X(p)·B(p)^-1·A(b)·B(b)
//
// Danach zurueck in den dotXSI-Raum ueber wx = C^-1·(X/scale)·C, lokal
// machen gegen den Elternbone, und in Translation plus Euler zerlegen.
//
// Was dabei NICHT verlustfrei ist: die Rotationen liegen in der GLA als
// 16-Bit-Quaternionen vor. Der Fehler bleibt unter 0,01 Grad und damit weit
// unter allem, was im Spiel sichtbar waere — aber bitgleich zur
// Originaldatei wird das Ergebnis nicht.

#pragma once

#include "g2/compress.h"
#include "g2/carscript.h"
#include "g2/carvalidate.h"
#include "g2/mdxa.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace g2::xsiexp {

struct ExportOptions {
    // Muessen zu den Werten passen, mit denen gebaut wurde — sonst kommt
    // eine massstaeblich falsche Datei heraus.
    float                              scale = 0.64f;
    std::optional<std::array<float, 3>> origin;

    // Bildrate fuer SI_Scene.
    int fps = 20;

    // Wurzelbewegung pro Frame, aus der .frames-Datei ("averagevec").
    //
    // Ohne sie kommt eine Laufanimation zwar richtig aussehend, aber AUF DER
    // STELLE heraus: Carcass entfernt die Wurzelbewegung beim Bauen und legt
    // sie in die .frames, damit die Spiel-Engine sie anwendet. In der GLA
    // steht sie nicht mehr.
    //
    // Wird sie hier eingesetzt, entsteht eine .xsi, die sich wie eine
    // Originaldatei verhaelt: das Neubauen entfernt die Bewegung wieder und
    // schreibt dieselbe averagevec.
    std::optional<std::array<float, 3>> rootMotionPerFrame;

    // Welche Bindepose in den BASEPOSE-Block?
    //
    // Ravens root.xsi enthaelt dort die WELTpose — nachgerechnet stimmen
    // alle neun Zahlen. Trotzdem meldet Carcass bei unseren Dateien
    // "non-uniform scaling" mit Werten um 0,4096, also 0,64 zum Quadrat:
    // die Skalierung wird zweimal angewandt.
    //
    // Der Verdacht: Carcass verkettet die BASEPOSE-Bloecke ueber die
    // Hierarchie. Bei Raven faellt das nicht auf, weil dort
    // Gruppierungsknoten (lleg_root, rd1root ...) dazwischenliegen, deren
    // Bindepose neutral ist. Unsere Dateien haben die nicht, weil die GLA
    // sie nicht kennt.
    //
    // Welche Variante richtig ist, laesst sich nur mit Carcass selbst
    // entscheiden. Deshalb beide anbieten, statt zu raten.
    enum class BasePose {
        World,   // wie in Ravens Dateien
        Local,   // gegen den Elternbone, falls Carcass selbst verkettet
        None,    // gar kein Block — so war es vor der Korrektur
    };
    BasePose basePose = BasePose::World;
};

// Liest die Wurzelbewegung einer Sequenz aus einer .frames-Datei.
// Der Schluessel ist der Pfad der Quelldatei, wie er in der .car steht.
std::optional<std::array<float, 3>> readAverageVec(const std::string& framesPath,
                                                   const std::string& sequenceOrFile);

// Schaetzt den -origin-Versatz aus der GLA.
//
// Der Versatz ist ueber alle Frames konstant, die Wurzelbewegung nicht —
// deshalb ist der haeufigste Wert je Achse der Versatz. Eine Schaetzung
// bleibt es trotzdem: liefert sie etwas, wird der Wert ausgegeben, damit man
// ihn pruefen kann, und -origin ueberschreibt ihn jederzeit.
std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla);


// Sucht einen konstanten Versatz in der Wurzelbone-Translation.
//
// "-origin 0 0 24" verschiebt beim Bauen das ganze Modell, und der Versatz
// steckt danach in jeder Frame-Matrix. Wird er beim Export nicht wieder
// hinzugefuegt, zieht das Neubauen ihn ein ZWEITES Mal ab — das Modell steht
// dann 24 Einheiten daneben.
std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla);


// Ein Ausschnitt der GLA als eigenstaendige Animation.
struct Sequence {
    std::string name;
    int         startFrame = 0;
    int         frameCount = 0;
};

// Restbewegung des Motion-Bones innerhalb eines Ausschnitts.
//
// Carcass entfernt die Wurzelbewegung je ANIMATION. Eine Sequenz, die nur
// ein Ausschnitt einer laengeren Animation ist — in JK2s animation.cfg sind
// das 16 % —, traegt deshalb einen Rest davon.
//
// Beim Neubauen wird dieser Rest wieder entfernt und landet in der .frames.
// Die Animation bleibt heil; nur der Ort, an dem die Bewegung steht,
// wechselt. Bitgleich zur Quelle wird die neue GLA dann nicht.
float residualMotion(const MdxaFile& gla, const Sequence& seq);

// Holt die Wurzelbewegung aus der GLA selbst.
//
// Die .frames-Datei ist dafuer NICHT noetig. Carcass rechnet die Bewegung
// als lineare Rampe auf den Wurzelbone; sie steht damit weiterhin in der
// GLA — als Translationsdifferenz zwischen erstem und letztem Frame der
// Sequenz, geteilt durch die Zahl der Schritte.
//
// Gegen Ravens _humanoid.frames geprueft: bei allen 178 Sequenzen mit
// Bewegung stimmt der so gewonnene Wert mit "averagevec" ueberein.
//
// Leer, wenn die Sequenz keine Bewegung hat — dann gibt es nichts
// wiederherzustellen.
std::optional<std::array<float, 3>> detectRootMotion(const MdxaFile& gla, const Sequence& seq);

// --- Aus einer GLA wieder ein baubares Skript machen ------------------------
//
// Eine Zeile der animation.cfg.
struct CfgSequence {
    std::string name;
    int         start = 0;
    int         count = 0;
    int         loop = -1;
    int         fps = 20;
};

// Ein Bereich, der als eigene .xsi geschrieben wird, samt der Sequenzen, die
// darin liegen.
struct MasterGroup {
    CfgSequence              self;
    std::vector<CfgSequence> inside;
};

struct Grouping {
    std::vector<MasterGroup> masters;
    std::size_t              partial = 0;   // nur teilweise ueberlappend
};

// Fasst Unterbereiche zusammen.
//
// Viele Sequenzen sind Ausschnitte einer laengeren — in JK2s animation.cfg
// 268 von 989. Exportierte man jede als eigene Datei, haette die neue GLA
// mehr Frames als die alte und die animation.cfg passte nicht mehr.
//
// Deshalb wird nur der jeweils groesste, sich nicht ueberschneidende Bereich
// als Datei geschrieben; was darin liegt, wird im Skript zu -additional —
// genau die Struktur, die Raven selbst benutzt.
Grouping groupSequences(const std::vector<CfgSequence>& cfg);

// Baut daraus ein .car-Skript.
//
// xsiPrefix ist der Pfad, der den Dateinamen vorangestellt wird, so wie er
// spaeter in der .car stehen soll (z.B. "models/players/jk2/").
//
// origin MUSS derselbe Wert sein, mit dem exportiert wurde: der Export
// rechnet ihn in die .xsi ein; fehlt er im Skript, zieht das Neubauen ihn
// nicht wieder ab, und das ganze Modell steht um diesen Betrag daneben.
// makeSkel ist die letzte Zeile des Skripts: sie sagt, WO die GLA entsteht
// und wie sie heisst. Der Wert steht exakt im Kopf der Quell-GLA — bei
// Ravens _humanoid.gla ist es "models/players/_humanoid/_humanoid", und
// genau das steht auch in ihrer .car. Leer = aus xsiPrefix ableiten, was
// nur ein Notbehelf ist.
// scale = 0 laesst "$scale" weg.
//
// Wichtig fuer Carcass: die BASEPOSE-Werte in der .xsi sind bereits durch
// die Skelettskalierung geteilt (so wie in Ravens Dateien). Wendet Carcass
// zusaetzlich "$scale 0.64" an, wird zweimal skaliert — und genau 0,64 mal
// 0,64 = 0,4096 meldet es als "non-uniform scaling".
car::Script buildScript(const Grouping& g, const std::string& xsiPrefix,
                        const std::optional<std::array<float, 3>>& origin, float scale = 0.0f,
                        bool keepMotion = false, const std::string& makeSkel = {});

// Erzeugt den Inhalt einer dotXSI-Datei fuer den angegebenen Ausschnitt.
std::string exportSequence(const MdxaFile& gla, const Sequence& seq,
                           const ExportOptions& opt);

}  // namespace g2::xsiexp
