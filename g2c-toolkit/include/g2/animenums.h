// g2/animenums.h — Die Enumtabelle aus anims.h.
//
// Assimilate laedt genau eine solche Datei und prueft die Sequenznamen der
// .car dagegen. Welche Datei das ist, gehoert in die Einstellungen: Ravens
// Tabelle passt fuer Ravens Datenbestand, ein Mod erweitert sie.
//
// An Movie Duels gemessen: die OpenJK-anims.h kennt 1603 Enums, die dortige
// animation.cfg fuehrt 1683 Sequenzen — 266 davon (BOTH_MD_*, BOTH_BOLT_*,
// BOTH_BLOCK_*) sind eigene Animationen des Mods. Wer unbekannte Namen als
// Fehler behandelt, blockiert damit die gesamte Arbeit an einem Mod. Sie
// gehoeren als Warnung gemeldet, nicht als Abbruch.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace g2::anim {

struct EnumTable {
    std::string              sourcePath;
    std::vector<std::string> names;   // in Deklarationsreihenfolge
    std::map<std::string, int> index;

    bool empty() const { return names.empty(); }
    std::size_t size() const { return names.size(); }

    bool contains(const std::string& name) const { return index.count(name) != 0; }

    // -1, wenn unbekannt.
    int indexOf(const std::string& name) const {
        const auto it = index.find(name);
        return it == index.end() ? -1 : it->second;
    }
};

// Liest die Enums aus einem C-Header.
//
// Erkannt wird der erste `typedef enum`-Block; darin jeder Bezeichner aus
// Grossbuchstaben, Ziffern und Unterstrichen, der von einem Komma gefolgt
// wird. Zuweisungen (`FOO = 3,`) sind erlaubt, Kommentare werden ignoriert.
//
// Bewusst nachsichtig: die Datei ist Quellcode eines fremden Projekts und
// aendert sich. Ein Parser, der an einem unerwarteten Makro scheitert, waere
// hier schlechter als einer, der ein paar Zeilen ueberspringt.
EnumTable parseEnumHeader(const std::string& text, const std::string& sourcePath = {});
EnumTable parseEnumHeaderFile(const std::string& path);

}  // namespace g2::anim
