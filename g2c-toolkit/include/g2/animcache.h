// g2/animcache.h — Zwischenspeicher fuer geparste Animationsdateien.
//
// Carcass macht dasselbe mit seiner CARPET-Datei. Im Build-Log des Originals
// steht "( Reading 249.63MB CARPET file )" — die zehn Sekunden, in denen es
// 1289 Dateien "verarbeitet", sind in Wahrheit das Einlesen dieses Caches.
// Ohne ihn muesste auch Carcass jede .xsi neu parsen.
//
// Gemessen entfallen rund 96 % der Bauzeit auf Lesen und Parsen der
// Quelldateien (80 % Datei-I/O, 12 % Parser, 4 % FCurves einsammeln). Genau
// dieses Ergebnis wird hier abgelegt.
//
// Unterschiede zu CARPET, bewusst:
//
//   - **Eine Datei je Quelle**, nicht ein Sammelarchiv. Ein beschaedigter
//     Eintrag kostet eine Datei statt den ganzen Cache, und parallele Laeufe
//     koennen sich nicht in die Quere kommen.
//   - **Schluessel aus Pfad, Groesse und Aenderungszeit.** Wird eine .xsi
//     angefasst, faellt ihr Eintrag automatisch weg. Kein manuelles Leeren.
//   - **Versionsnummer im Kopf.** Aendert sich das Einleseverhalten, werden
//     alte Eintraege verworfen statt still falsche Daten zu liefern.

#pragma once

#include "g2/xsi_anim.h"

#include <cstdint>
#include <optional>
#include <string>

namespace g2 {

// Erhoehen, sobald sich aendert, WAS aus einer .xsi gelesen wird. Sonst
// liefert der Cache Daten nach altem Verstaendnis zurueck — ein Fehler, der
// sich als "der Fix wirkt nicht" tarnt und schwer zu finden ist.
// 4: SI_Scene-Bereich mitgespeichert, fuer die Warnung bei Abweichung.
inline constexpr std::uint32_t kAnimCacheVersion = 4;

struct AnimCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t bytesRead = 0;

    double hitPercent() const {
        const std::uint64_t total = hits + misses;
        return total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    }
};

class AnimCache {
public:
    // Ein leerer Ordner schaltet den Cache ab.
    explicit AnimCache(std::string directory) : dir_(std::move(directory)) {}

    bool enabled() const { return !dir_.empty(); }

    // Liefert den Eintrag, wenn Groesse und Aenderungszeit der Quelle passen.
    std::optional<xsi::AnimFile> load(const std::string& sourcePath);

    // Fehler beim Schreiben sind nie fatal: ein Cache ist eine Optimierung,
    // kein Zustand. Voller Datentraeger oder fehlende Rechte duerfen keinen
    // Bau abbrechen.
    void store(const std::string& sourcePath, const xsi::AnimFile& anim);

    // Laedt aus dem Cache oder parst und legt ab.
    xsi::AnimFile loadOrParse(const std::string& sourcePath);

    const AnimCacheStats& stats() const { return stats_; }

    // Alle Eintraege loeschen. Liefert die Anzahl.
    std::size_t clear();

    // Gesamtgroesse des Caches in Bytes.
    std::uint64_t sizeOnDisk() const;

private:
    std::string    dir_;
    AnimCacheStats stats_;

    std::string entryPath(const std::string& sourcePath) const;
};

}  // namespace g2
