// gui/icons.h - Symbole aus der Windows-eigenen Icon-Schrift.
//
// Keine mitgelieferte Datei noetig: "Segoe MDL2 Assets" gehoert seit
// Windows 10 zum Lieferumfang, "Segoe Fluent Icons" seit Windows 11. Die
// Glyphen liegen im privaten Unicode-Bereich (PUA) ab E700 und werden per
// MergeMode in denselben Zeichensatz gelegt wie Schrift und CJK.
//
// Der Haken daran, den Microsoft selbst nennt: PUA-Zeichen sind nicht
// standardisiert. Fehlt die Schrift, fehlen die Symbole. Deshalb prueft die
// Fensteranbindung, ob sie geladen werden konnte, und laesst die Symbole
// sonst weg, statt leere Kaesten anzuzeigen.

#pragma once

namespace g2::gui {

// Bereich, den die Icon-Schrift beisteuert.
constexpr unsigned kIconRangeMin = 0xE700;
constexpr unsigned kIconRangeMax = 0xF8FF;

// Der Ausschnitt, den wir wirklich benutzen.
//
// Die zwanzig Symbole unten liegen alle zwischen E70F und EA39. Den ganzen
// Bereich anzufordern hiesse, fuer 4608 Zeichen eine Nachschlagetabelle
// anzulegen, um zwanzig zu zeichnen.
//
// Wer ein Symbol ausserhalb hinzufuegt, muss diese Grenzen erweitern —
// tools/check_win32.py prueft das.
// Farben fuer Symbole.
//
// ImGui zeichnet Text einfarbig, also auch die Symbole. Ein Symbol allein
// — in einer Statusspalte oder vor einer Meldung — laesst sich aber
// einfaerben, und das hilft beim Erfassen: gruen heisst fertig, rot heisst
// Fehler, ohne dass man den Text lesen muss.
//
// Bewusst gedeckte Toene. Eine Oberflaeche, in der alles leuchtet, ist
// anstrengender als eine graue — und die Farbe verliert ihre Bedeutung,
// wenn sie ueberall ist.
struct IconColor {
    float r, g, b, a;
};

inline constexpr IconColor kIconGood{0.42f, 0.83f, 0.52f, 1.0f};    // gruen
inline constexpr IconColor kIconWarn{0.95f, 0.75f, 0.35f, 1.0f};    // gelb
inline constexpr IconColor kIconBad{0.92f, 0.45f, 0.45f, 1.0f};     // rot
inline constexpr IconColor kIconInfo{0.48f, 0.68f, 0.95f, 1.0f};    // blau
inline constexpr IconColor kIconMuted{0.55f, 0.55f, 0.58f, 1.0f};   // grau
inline constexpr IconColor kIconAccent{0.72f, 0.62f, 0.95f, 1.0f};  // violett

constexpr unsigned kIconUsedMin = 0xE70F;
constexpr unsigned kIconUsedMax = 0xEA39;

constexpr const char* ICON_FOLDER      = "\xEE\xA2\xB7";   // U+E8B7  Ordner
constexpr const char* ICON_FOLDER_OPEN = "\xEE\xA3\x9A";   // U+E8DA  Ordner oeffnen
constexpr const char* ICON_OPEN_FILE   = "\xEE\xA3\xA5";   // U+E8E5  Datei oeffnen
constexpr const char* ICON_SAVE        = "\xEE\x9D\x8E";   // U+E74E  Speichern
constexpr const char* ICON_SAVE_ALL    = "\xEE\x9E\x8C";   // U+E78C  Alle speichern
constexpr const char* ICON_BUILD       = "\xEE\x9D\xA8";   // U+E768  Bauen (Play)
constexpr const char* ICON_CHECK       = "\xEE\x9C\xBE";   // U+E73E  Haken
constexpr const char* ICON_VALIDATE    = "\xEE\xA7\x99";   // U+E9D9  Pruefen
constexpr const char* ICON_ADD         = "\xEE\x9C\x90";   // U+E710  Hinzufuegen
constexpr const char* ICON_CANCEL      = "\xEE\x9C\x91";   // U+E711  Abbrechen
constexpr const char* ICON_SETTINGS    = "\xEE\x9C\x93";   // U+E713  Einstellungen
constexpr const char* ICON_GLOBE       = "\xEE\x9D\xB4";   // U+E774  Sprache
constexpr const char* ICON_DELETE      = "\xEE\x9D\x8D";   // U+E74D  Loeschen
constexpr const char* ICON_EDIT        = "\xEE\x9C\x8F";   // U+E70F  Bearbeiten
constexpr const char* ICON_DOCUMENT    = "\xEE\xA2\xA5";   // U+E8A5  Dokument
constexpr const char* ICON_REFRESH     = "\xEE\x9C\xAC";   // U+E72C  Neu lesen
constexpr const char* ICON_FILTER      = "\xEE\x9C\x9C";   // U+E71C  Filter
constexpr const char* ICON_WARNING     = "\xEE\x9E\xBA";   // U+E7BA  Warnung
constexpr const char* ICON_ERROR       = "\xEE\xA8\xB9";   // U+EA39  Fehler
constexpr const char* ICON_INFO        = "\xEE\xA5\x86";   // U+E946  Hinweis

// Symbole sind abschaltbar, falls die Schrift fehlt.
bool iconsAvailable();
void setIconsAvailable(bool v);

// Symbol + Text, oder nur Text wenn keine Symbole da sind.
const char* withIcon(const char* icon, const char* text);

}  // namespace g2::gui
