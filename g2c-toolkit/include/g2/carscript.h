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
    // Kommentarzeilen, die VOR dieser Sequenz stehen sollen.
    //
    // Ravens animation.cfg gliedert die 1683 Sequenzen mit Trennern und
    // Ueberschriften — ohne sie ist die Datei eine Wand aus Zahlen. Beim
    // Bauen gingen sie bisher verloren, weil die cfg neu erzeugt wird.
    //
    // Die Zeilen stehen so, wie sie geschrieben werden sollen; ein "//"
    // wird beim Schreiben vorangestellt, falls es fehlt.
    std::vector<std::string> commentsBefore;

    // Kommentar am Ende der Zeile, hinter den Zahlen.
    std::string  trailingComment;

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
    // Kommentarzeilen, die im Skript VOR diesem Grab stehen.
    //
    // Sie wandern in die erzeugte animation.cfg. Wer sein Skript gliedert
    // — Trenner, Ueberschriften, Hinweise —, findet das in der cfg wieder,
    // statt eine Wand aus Zahlen zu bekommen.
    std::vector<std::string> commentsBefore;

    // Kommentar am ENDE der Grab-Zeile, hinter den Angaben.
    //
    // Ravens animation.cfg nutzt das fuer Hinweise je Animation:
    //
    //     BOTH_WALK1_ANI  36698  24  0  30  // nur mit Anakins Schwert
    //
    // Er wandert in die erzeugte cfg an dieselbe Stelle.
    std::string              trailingComment;

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

    // Flags, die dieser Parser nicht kennt, in ihrer Reihenfolge. Sie
    // bleiben beim Speichern erhalten, auch wenn die Zeile neu entsteht.
    std::vector<std::string> extraArgs;

    // Die Zeile, wie sie in der Datei stand (ohne Zeilenende).
    //
    // Solange sich an diesem Grab nichts geaendert hat, wird genau sie
    // zurueckgeschrieben. Ohne das formte jedes Speichern jede Zeile um —
    // Flags in anderer Reihenfolge, andere Abstaende —, und aus einer
    // kleinen Aenderung wurde ein Unterschied in tausend Zeilen.
    std::string sourceLine;

    // Stammt dieser Grab aus einer $include-Datei? Dann Kennung des
    // $include im Hauptskript, sonst -1. Solche Grabs gehoeren der anderen
    // Datei und werden beim Speichern nicht ins Hauptskript kopiert.
    int fromInclude = -1;

    // Sequenzname, wenn kein -enum angegeben ist: Dateiname ohne Pfad und
    // Endung, in Grossbuchstaben. Gegen die echte _humanoid.car geprueft.
    std::string derivedName() const;
};

// $aseanimconvertmdx[_noask] <root> [-makeskel <pfad>] [-origin x y z]
//                            [-makeskin]
struct ConvertDirective {
    std::string                          root;
    std::string                          makeSkel;
    std::optional<std::array<double, 3>> origin;
    bool                                 noAsk = false;

    // Carcass legt dann eine .skin neben die GLM. Ging beim Speichern
    // frueher verloren, weil der Parser das Flag nicht kannte.
    bool                                 makeSkin = false;
    std::vector<std::string>             extraArgs;

    // Wie beim Grab: Originalzeile samt Zeilenendkommentar. Die
    // Kommentarzeilen darueber haengen an der zugehoerigen Statement.
    std::string                          sourceLine;
    std::string                          trailingComment;
    int                                  fromInclude = -1;
};

struct Statement {
    Cmd                      cmd = Cmd::Unknown;
    std::string              raw;      // Befehl wie im Skript, inkl. '$'
    std::vector<std::string> args;
    std::size_t              line = 0;
    std::string              file;     // Herkunft, wichtig bei $include

    // Originalzeile und die Kommentarzeilen darueber. Beides wird beim
    // Speichern unveraendert wieder ausgegeben — ein Kopfkommentar ueber
    // $scale oder ein Hinweis hinter $keepmotion gehoert dem Autor.
    std::string              sourceLine;
    std::vector<std::string> commentsBefore;

    // Aus einer $include-Datei: Kennung des $include im Hauptskript.
    int                      fromInclude = -1;
    // Nur bei $include im Hauptskript: die Kennung, die seine Inhalte tragen.
    int                      includeId = -1;
    // Wie viele eigene Grabs davor standen. Steht ein $include mitten
    // zwischen den Grabs, wird es beim Speichern wieder an diese Stelle
    // gesetzt.
    std::size_t              grabsBefore = 0;

    // Bequemlichkeiten mit klarer Fehlermeldung statt stiller Nullen.
    const std::string& arg(std::size_t i, const char* what) const;
    double             argNumber(std::size_t i, const char* what) const;
};

// Eine einzelne Zeile als Grab bzw. Konvertierungsanweisung lesen — dieselben
// Regeln wie beim Einlesen einer ganzen Datei. Wird auch beim Speichern
// gebraucht, um festzustellen, ob sich ein Eintrag geaendert hat.
GrabDirective    parseGrabLine(const std::string& line);
ConvertDirective parseConvertLine(const std::string& line);

struct Script {
    // Kommentarzeilen NACH dem letzten Grab.
    //
    // Sie haben keinen Nachfolger, an dem sie haengen koennten. Ohne ein
    // eigenes Feld liesse sich ein Trenner ans Ende der Liste weder
    // schieben noch dort anlegen — und genau das will man, wenn der letzte
    // Block eine Ueberschrift bekommen soll.
    std::vector<std::string> trailingComments;

    // Kommentarzeilen ganz am Ende der Datei, hinter dem letzten Befehl.
    std::vector<std::string> endComments;

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
