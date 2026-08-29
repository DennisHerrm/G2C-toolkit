// g2/carvalidate.h — Pruefungen und Schreiben von .car-Skripten.
//
// Bildet nach, was Assimilate vor dem Bauen prueft, und ergaenzt es um die
// Gegenrichtung, die dort fehlt.

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
    std::string sequence;   // leer, wenn nicht sequenzbezogen
    std::size_t line = 0;   // Zeile in der .car, 0 = unbekannt

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
    // Ohne Enumtabelle entfallen die beiden Namenspruefungen.
    const anim::EnumTable* enums = nullptr;

    std::string baseDir;

    // Framezahlen aus den .xsi lesen. Genauer, aber es muessen alle Dateien
    // gelesen werden — mit Cache beim zweiten Mal schnell.
    bool        readFrameCounts = false;
    std::string cacheDir;

    unsigned threads = 0;
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

// Prueft ein Skript. Reihenfolge der Pruefungen ist die Reihenfolge der
// Meldungen; sequenzbezogene Meldungen tragen den Namen mit.
ValidateResult validate(const Script& script, const std::string& carPath,
                        const ValidateOptions& opt = {});

// --- Schreiben -------------------------------------------------------------
//
// Gibt das Skript wieder als Text aus. Grundlage fuer jede Bearbeitung: ohne
// Schreiben bleibt das Werkzeug ein Betrachter.
//
// Die Ausgabe ist bewusst nicht zeichengleich mit der Eingabe — Einrueckungen
// und Kommentare gehen verloren. Was erhalten bleibt, ist die Bedeutung:
// Reihenfolge der Anweisungen, alle Flags, alle -additional-Eintraege.
std::string writeScript(const Script& script);

// Legt die Rahmenanweisungen an, zwischen denen die Grabs stehen.
//
// writeScript gibt die Grabs NUR nach einem $aseanimgrabinit aus. Ein von
// Hand zusammengesetztes Skript ohne diese Statements verliert sie beim
// Schreiben stillschweigend — die Datei sieht vollstaendig aus und enthaelt
// keine einzige Sequenz.
void addGrabFrame(Script& script);

// --- Verzeichnis durchsuchen ----------------------------------------------

struct FoundCar {
    std::string path;
    std::size_t grabs = 0;
    std::string modelName;   // aus -makeskel, sonst leer
};

// Sucht rekursiv nach .car-Dateien. Assimilate bietet dasselbe als
// "Validate ALL models?" ueber einen Verzeichnisbaum an.
std::vector<FoundCar> scanDirectory(const std::string& root, std::size_t maxDepth = 16);

}  // namespace g2::car
