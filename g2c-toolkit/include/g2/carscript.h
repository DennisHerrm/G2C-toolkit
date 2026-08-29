// g2/carscript.h — Parser fuer Carcass' .car-Skripte.
//
// Der Befehlssatz stammt aus der Dispatch-Funktion des Originalbinaries bei
// 0x41d980 (Kette aus 21 strcmp-Vergleichen). Die Argumentsignaturen sind
// dagegen NICHT aus dem Binary rekonstruiert, sondern aus dem beobachteten
// Gebrauch abgeleitet — deshalb werden unbekannte Befehle mit ihren rohen
// Argumenten durchgereicht statt verworfen.

#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace g2::car {

enum class Cmd {
    BaseDir,
    ModelName,
    Origin,
    Scale,
    Flatten,
    KeepMotion,
    BoneHierCap,
    Include,
    Pcj,
    AseAnimGrabInit,
    AseAnimGrab,
    AseAnimGrabFinalize,
    AseAnimGrabGla,
    AseAnimRefGla,
    AseConvert,
    AseAnimConvert,
    AseAnimConvertMdx,
    AseAnimConvertMdxNoAsk,
    Exit,
    Unknown,
};

const char* cmdName(Cmd c);
Cmd         cmdFromString(const std::string& s);

// Ein Eintrag, wie ihn Carcass in die animation.cfg schreibt. Das Format ist
// im Kopf jeder erzeugten Datei dokumentiert:
//     enum, targetFrame, frameCount, loopFrame, frameSpeed
struct Sequence {
    std::string  name;
    int          targetFrame = 0;
    int          frameCount = 0;
    int          loopFrame = -1;
    int          frameSpeed = 0;
    std::string  sourceFile;   // die .xsi, aus der die Frames stammen
    bool         fromAdditional = false;
    bool         insideQdSkip = false;
};

// $aseanimgrab <datei.xsi> [-loop N] [-framespeed N] [-enum NAME]
//              [-qdskipstart] [-additional t c l s NAME]... [-qdskipstop]
struct GrabDirective {
    std::string              file;
    std::optional<int>       loop;
    std::optional<int>       frameSpeed;
    std::optional<std::string> enumName;   // ueberschreibt den Namen aus dem Dateinamen
    bool                     hasQdSkip = false;
    struct Additional {
        int         targetOffset = 0;
        int         frameCount = 0;
        int         loopFrame = -1;
        int         frameSpeed = 0;
        std::string name;
        bool        insideQdSkip = false;
    };
    std::vector<Additional> additional;
    std::size_t             line = 0;

    // Sequenzname, wenn kein -enum angegeben ist: Dateiname ohne Pfad und
    // Endung, in Grossbuchstaben. Gegen die echte _humanoid.car geprueft.
    std::string derivedName() const;
};

// $aseanimconvertmdx[_noask] <root> [-makeskel <pfad>] [-origin x y z]
struct ConvertDirective {
    std::string                          root;
    std::string                          makeSkel;
    std::optional<std::array<double, 3>> origin;
    bool                                 noAsk = false;
};

struct Statement {
    Cmd                      cmd = Cmd::Unknown;
    std::string              raw;      // Befehl wie im Skript, inkl. '$'
    std::vector<std::string> args;
    std::size_t              line = 0;
    std::string              file;     // Herkunft, wichtig bei $include

    // Bequemlichkeiten mit klarer Fehlermeldung statt stiller Nullen.
    const std::string& arg(std::size_t i, const char* what) const;
    double             argNumber(std::size_t i, const char* what) const;
};

struct Script {
    std::vector<Statement> statements;

    // Aufgeloeste Werte der einfachen Zuweisungsbefehle, der Bequemlichkeit
    // halber vorextrahiert.
    std::string                    baseDir;
    std::string                    modelName;
    std::optional<std::array<double, 3>> origin;
    std::optional<double>          scale;
    bool                           flatten = false;
    bool                           keepMotion = false;

    // $pcj — "player controlled joints", Bones die die Engine zur Laufzeit
    // selbst drehen darf (Ragdoll, Blickrichtung). In der echten _humanoid.car
    // 17 Eintraege. Der erste ist "$flatten", also ein Schalter und kein
    // Bonename; er wird hier nicht in die Liste aufgenommen.
    std::vector<std::string>       pcjBones;
    bool                           pcjFlatten = false;

    std::vector<GrabDirective>     grabs;
    std::optional<ConvertDirective> convert;

    // Loest die Grabs in die Sequenzliste auf, wie sie in die animation.cfg
    // geht. frameCounts muss fuer jede Grab-Datei die Anzahl der Frames
    // liefern; ohne diese Information (sie steckt in den .xsi-Dateien) bleiben
    // targetFrame und frameCount auf 0.
    std::vector<Sequence> buildSequences(
        const std::function<int(const std::string&)>& frameCountOf = {}) const;
};

// Schreibt eine animation.cfg im Format von Carcass.
std::string writeAnimationCfg(const std::vector<Sequence>& seqs, const std::string& headerComment);

struct ParseOptions {
    // Carcass begrenzt die Verschachtelung ueber MAX_INCLUDES (geprueft bei
    // 0x41e4f0). Der genaue Wert steht nicht im Binary-Text; 16 ist ein
    // vernuenftiger Vorgabewert.
    int maxIncludeDepth = 16;

    // $include-Dateien tatsaechlich laden und einsetzen.
    bool followIncludes = true;
};

Script parse(const std::string& text, const std::string& originName = "<memory>",
             const ParseOptions& opt = {});
Script parseFile(const std::string& path, const ParseOptions& opt = {});

}  // namespace g2::car
