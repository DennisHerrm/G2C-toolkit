// g2/carscript.h - Parser for Carcass .car scripts.
//
// The command set comes from the original binary's dispatch function at
// 0x41d980 (a chain of 21 strcmp comparisons). The argument signatures, on
// the other hand, were NOT reconstructed from the binary but derived from
// observed usage - which is why unknown commands are passed through with
// their raw arguments instead of being discarded.

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
    // Carcass reads it and does nothing with it.
    CfgNameFromCar,
    AseConvert,
    AseAnimConvert,
    AseAnimConvertMdx,
    AseAnimConvertMdxNoAsk,
    Exit,
    Unknown,
};

const char* cmdName(Cmd c);
Cmd         cmdFromString(const std::string& s);

// An entry as Carcass writes it into animation.cfg. The format is
// documented in the header of every generated file:
//     enum, targetFrame, frameCount, loopFrame, frameSpeed
struct Sequence {
    // Comment lines that should appear BEFORE this sequence.
    //
    // Raven's animation.cfg structures its 1683 sequences with separators and
    // headings - without them the file is a wall of numbers. They used to be
    // lost when building, because the cfg is regenerated.
    //
    // The lines are stored as they should be written; a "//" is prepended
    // when writing if it is missing.
    std::vector<std::string> commentsBefore;

    // Comment at the end of the line, after the numbers.
    std::string  trailingComment;

    std::string  name;
    int          targetFrame = 0;
    int          frameCount = 0;
    int          loopFrame = -1;
    // Frames per second; negative plays backwards. Not necessarily whole:
    // the engine reads it with atof, and hand-edited configs use e.g. 16.5.
    double       frameSpeed = 0;
    std::string  sourceFile;   // the .xsi the frames come from
    bool         fromAdditional = false;
    bool         insideQdSkip = false;
};

// $aseanimgrab <file.xsi> [-loop N] [-framespeed N] [-enum NAME]
//              [-qdskipstart] [-additional t c l s NAME]... [-qdskipstop]
struct GrabDirective {
    // The $basedir in force for this line (the last one before it), as
    // written in the script; empty = the asset root. Carcass reads the file
    // from <basedir><file> then.
    std::string baseDir;

    // Comment lines that appear BEFORE this grab in the script.
    //
    // They are carried over into the generated animation.cfg. Anyone who
    // structures their script - separators, headings, notes - finds that
    // again in the cfg instead of getting a wall of numbers.
    std::vector<std::string> commentsBefore;

    // Comment at the END of the grab line, after the arguments.
    //
    // Raven's animation.cfg uses this for per-animation notes:
    //
    //     BOTH_WALK1_ANI  36698  24  0  30  // only with Anakin's saber
    //
    // It is carried over to the same place in the generated cfg.
    std::string              trailingComment;

    std::string              file;
    std::optional<int>       loop;
    // May have decimals (-framespeed 16.5). Carcass read it with atoi and
    // dropped them; the engine does not, so g2c keeps them.
    std::optional<double>    frameSpeed;
    std::optional<std::string> enumName;   // overrides the name from the file name
    bool                     hasQdSkip = false;
    struct Additional {
        int         targetOffset = 0;
        int         frameCount = 0;
        int         loopFrame = -1;
        double      frameSpeed = 0;
        std::string name;
        bool        insideQdSkip = false;
    };
    std::vector<Additional> additional;
    std::size_t             line = 0;

    // Flags this parser does not know, in their original order. They are
    // preserved on save, even when the line is regenerated.
    std::vector<std::string> extraArgs;

    // The line as it appeared in the file (without the line ending).
    //
    // As long as nothing about this grab has changed, exactly this line is
    // written back. Without that, every save reformatted every line - flags
    // in a different order, different spacing - and a small change turned
    // into a diff of a thousand lines.
    std::string sourceLine;

    // Does this grab come from an $include file? Then this is the ID of the
    // $include in the main script, otherwise -1. Such grabs belong to the
    // other file and are not copied into the main script on save.
    int fromInclude = -1;

    // Blank lines in front of this grab that are layout, not comment: they are
    // written back on save but neither shown as comment rows nor carried into
    // animation.cfg.
    int blankBefore = 0;

    // Sequence name when no -enum is given: file name without path and
    // extension, in uppercase. Checked against the real _humanoid.car.
    std::string derivedName() const;
};

// $aseanimconvertmdx[_noask] <root> [-makeskel <path>] [-origin x y z]
//                            [-makeskin]
struct ConvertDirective {
    std::string                          root;
    std::string                          makeSkel;
    std::optional<std::array<double, 3>> origin;
    bool                                 noAsk = false;

    // Carcass then puts a .skin next to the GLM. This used to be lost on
    // save because the parser did not know the flag.
    bool                                 makeSkin = false;
    std::vector<std::string>             extraArgs;

    // As with grabs: the original line including its end-of-line comment.
    // The comment lines above it are attached to the corresponding Statement.
    std::string                          sourceLine;
    std::string                          trailingComment;
    int                                  fromInclude = -1;
};

struct Statement {
    Cmd                      cmd = Cmd::Unknown;
    std::string              raw;      // command as in the script, incl. '$'
    std::vector<std::string> args;
    std::size_t              line = 0;
    std::string              file;     // origin, important with $include

    // Original line and the comment lines above it. Both are written back
    // unchanged on save - a header comment above $scale or a note after
    // $keepmotion belongs to the author.
    std::string              sourceLine;
    std::vector<std::string> commentsBefore;

    // From an $include file: ID of the $include in the main script.
    int                      fromInclude = -1;
    // Only for an $include in the main script: the ID its contents carry.
    int                      includeId = -1;
    // How many of the script's own grabs came before it. If an $include sits
    // in the middle of the grabs, it is put back at this position on save.
    std::size_t              grabsBefore = 0;
    // Blank lines in front of it (layout only), see GrabDirective::blankBefore.
    int                      blankBefore = 0;

    // Conveniences with a clear error message instead of silent zeros.
    const std::string& arg(std::size_t i, const char* what) const;
    double             argNumber(std::size_t i, const char* what) const;
};

// Reads a single line as a grab or convert directive - the same rules as
// when reading a whole file. Also needed on save to determine whether an
// entry has changed.
GrabDirective    parseGrabLine(const std::string& line);
ConvertDirective parseConvertLine(const std::string& line);

struct Script {
    // Comment lines AFTER the last grab.
    //
    // They have no successor they could be attached to. Without a field of
    // their own, a separator could neither be moved to the end of the list
    // nor created there - and that is exactly what you want when the last
    // block should get a heading.
    std::vector<std::string> trailingComments;

    // Comment lines at the very end of the file, after the last command.
    std::vector<std::string> endComments;

    // Everything after $exit, exactly as it was. Carcass stops reading
    // there; people park disabled grabs behind it. Saving used to delete them.
    std::vector<std::string> afterExit;

    // Line ending of the file. A .car with plain LF used to come back with
    // CRLF on every line - a diff of the whole file.
    std::string newline = "\r\n";
    // The last line had no line break (a file saved without one came back
    // with one, a diff on the last line).
    bool noFinalNewline = false;

    std::vector<Statement> statements;

    // Resolved values of the simple assignment commands, pre-extracted for
    // convenience.
    std::string                    baseDir;
    std::string                    modelName;
    std::optional<std::array<double, 3>> origin;
    std::optional<double>          scale;
    bool                           flatten = false;
    bool                           keepMotion = false;

    // $pcj - "player controlled joints", bones the engine may rotate itself
    // at runtime (ragdoll, look direction). 17 entries in the real
    // _humanoid.car. The first one is "$flatten", i.e. a switch rather than a
    // bone name; it is not added to the list here.
    std::vector<std::string>       pcjBones;
    bool                           pcjFlatten = false;

    std::vector<GrabDirective>     grabs;
    std::optional<ConvertDirective> convert;

    // Resolves the grabs into the sequence list as it goes into
    // animation.cfg. frameCounts must supply the frame count for every grab
    // file; without that information (it lives in the .xsi files),
    // targetFrame and frameCount stay at 0.
    std::vector<Sequence> buildSequences(
        const std::function<int(const std::string&)>& frameCountOf = {}) const;
};

// The header commands Assimilate's "Model" dialog edits. The convert line
// (-makeskel, -origin, -makeskin, ...) is edited directly in Script::convert.
struct ModelSettings {
    std::optional<double>    scale;            // $scale; nullopt = no such line
    bool                     keepMotion = false;
    // $pcj entries in file order. "$flatten" is an entry like any other, as
    // in Assimilate's PCJ list.
    std::vector<std::string> pcj;

    bool operator==(const ModelSettings&) const = default;
};

// The settings in the script's OWN lines - what applyModelSettings edits.
// Lines from $include files are not part of it: they live in another file.
// (Script::scale/keepMotion/pcjBones are the effective values the build
// uses, includes and all.)
ModelSettings modelSettingsOf(const Script& s);

// Writes the settings into the script's statements. Only lines whose value
// changes are rewritten (keeping their end-of-line comment); new lines go
// where Raven's scripts have them - after the grabs, before
// $aseanimgrabfinalize - in the order $scale, $keepmotion, $pcj. Lines from
// $include files are not touched.
void applyModelSettings(Script& s, const ModelSettings& m);

// Writes an animation.cfg in Carcass's format.
std::string writeAnimationCfg(const std::vector<Sequence>& seqs, const std::string& headerComment);

struct ParseOptions {
    // Carcass limits nesting via MAX_INCLUDES (checked at 0x41e4f0). The
    // exact value is not in the binary's text; 16 is a sensible default.
    int maxIncludeDepth = 16;

    // Actually load and insert $include files.
    bool followIncludes = true;
};

Script parse(const std::string& text, const std::string& originName = "<memory>",
             const ParseOptions& opt = {});
Script parseFile(const std::string& path, const ParseOptions& opt = {});

}  // namespace g2::car
