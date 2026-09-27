// g2/carvalidate.h - Checking and writing .car scripts.
//
// Replicates what Assimilate checks before building, and adds the opposite
// direction, which is missing there.

#pragma once

#include "g2/animcache.h"
#include "g2/animenums.h"
#include "g2/carscript.h"

#include <string>
#include <vector>

namespace g2::car {

struct Issue {
    enum class Level { Error, Warning, Info };

    Level       level = Level::Error;
    std::string message;
    std::string sequence;   // empty if not sequence-related
    std::size_t line = 0;   // line in the .car, 0 = unknown

    const char* levelName() const {
        switch (level) {
            case Level::Error: return "Fehler";
            case Level::Warning: return "Warnung";
            case Level::Info: return "Hinweis";
        }
        return "?";
    }
};

struct ValidateOptions {
    // Without an enum table, the two name checks are skipped.
    const anim::EnumTable* enums = nullptr;

    std::string baseDir;

    // Read frame counts from the .xsi files. More accurate, but all files
    // have to be read - fast the second time thanks to the cache.
    bool        readFrameCounts = false;
    std::string cacheDir;

    unsigned threads = 0;

    // At most this many individual "No enum" messages, the rest as one
    // summary line. With a mod that has hundreds of its own sequences, the
    // list would otherwise drown in them. "validate -all" lifts the limit.
    std::size_t maxEnumWarnings = 20;
};

struct ValidateResult {
    std::vector<Issue> issues;

    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t infos = 0;

    std::size_t grabs = 0;
    std::size_t sequences = 0;
    std::size_t missingFiles = 0;

    bool ok() const { return errors == 0; }
};

// Checks a script. The order of the checks is the order of the messages;
// sequence-related messages carry the sequence name.
ValidateResult validate(const Script& script, const std::string& carPath,
                        const ValidateOptions& opt = {});

// --- Writing ---------------------------------------------------------------
//
// Outputs the script as text again. The basis for any editing: without
// writing, the tool remains a viewer.
//
// Unchanged lines come back exactly as they were in the file: flag order,
// spacing, end-of-line comments. Only grabs and the convert directive that
// have changed are regenerated. Comment lines before each command and at the
// end of the file are preserved; $include lines stay $include lines.
//
// Previously every save reformatted every grab line and lost header
// comments, unknown flags and -makeskin along the way.
std::string writeScript(const Script& script);

// Creates the framing directives between which the grabs are placed.
//
// writeScript outputs the grabs ONLY after an $aseanimgrabinit. A script
// assembled by hand without these statements silently loses them when
// written - the file looks complete and contains not a single sequence.
void addGrabFrame(Script& script);

// --- Scanning a directory -------------------------------------------------

struct FoundCar {
    std::string path;
    std::size_t grabs = 0;
    std::string modelName;   // from -makeskel, otherwise empty
};

// Searches recursively for .car files. Assimilate offers the same as
// "Validate ALL models?" over a directory tree.
std::vector<FoundCar> scanDirectory(const std::string& root, std::size_t maxDepth = 16);

}  // namespace g2::car
