// g2/xsi.h — dotXSI-Parser.
//
// dotXSI ist ein ASCII-Format aus der DirectX-.X-Familie. Die Grammatik ist
// ueber alle Versionen gleich:
//
//     xsi 0101txt 0032
//
//     SI_Model MDL-root {
//         SI_Transform SRT-root {
//             1.0; 0.0; 0.0;
//             ...
//         }
//         SI_Mesh MSH-body {
//             ...
//         }
//     }
//
// Template = Bezeichner [Instanzname] "{" Inhalt "}", wobei Inhalt entweder
// weitere Templates oder Werte sind. Trennzeichen sind Semikolon, Komma und
// Whitespace, alle gleichwertig. Kommentare mit // und slash-stern.
//
// Was sich zwischen den Versionen unterscheidet, sind die Templatenamen und
// deren Bedeutung — nicht die Syntax. Deshalb ist dieser Parser
// versionsunabhaengig und liefert einen rohen Baum; die semantische Deutung
// passiert eine Ebene darueber.
//
// Carcass unterstuetzt 1.1, 1.3, 3.0 und 3.5 (erkennbar an den Headerkennungen
// "0101txt", "0103txt", "0300txt", "0350txt").

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g2::xsi {

struct Version {
    int  major = 0;
    int  minor = 0;
    bool binary = false;   // "bin" statt "txt" — wird hier nicht unterstuetzt
    std::string raw;

    std::string toString() const;
};

// Ein Wert im Template-Body. dotXSI unterscheidet Zahlen und Strings nicht
// streng, deshalb wird der Rohtext behalten und bei Bedarf konvertiert.
class Value {
public:
    explicit Value(std::string text) : text_(std::move(text)) {}

    const std::string& text() const { return text_; }

    // Liefert nullopt, wenn der Text keine gueltige Zahl ist.
    std::optional<double> asNumber() const;
    std::optional<long>   asInt() const;

    // Wirft mit Positionsangabe, wenn die Konvertierung scheitert.
    double asNumberOrThrow(const char* context) const;
    long   asIntOrThrow(const char* context) const;

private:
    std::string text_;
};

struct Template {
    std::string             type;       // z.B. "SI_Model"
    std::string             name;       // Instanzname, oft leer
    std::vector<Value>      values;     // direkte Werte im Body
    std::vector<Template>   children;   // verschachtelte Templates
    std::size_t             line = 0;   // Zeile des oeffnenden Bezeichners

    // Erstes Kind mit passendem Typ, oder nullptr.
    const Template* find(std::string_view type) const;

    // Alle Kinder mit passendem Typ.
    std::vector<const Template*> findAll(std::string_view type) const;

    // Rekursive Suche in die Tiefe.
    const Template* findDeep(std::string_view type) const;

    std::size_t countDeep() const;
};

struct Document {
    Version               version;
    std::vector<Template> roots;

    const Template* find(std::string_view type) const;
    const Template* findDeep(std::string_view type) const;
    std::size_t     templateCount() const;
};

struct ParseError {
    std::size_t line = 0;
    std::size_t column = 0;
    std::string message;

    std::string what() const;
};

// Parst dotXSI aus dem Speicher. Wirft std::runtime_error mit
// Zeilen- und Spaltenangabe.
Document parse(std::string_view text);
Document parseFile(const std::string& path);

// Uebersicht ueber die vorkommenden Templatetypen, absteigend nach Haeufigkeit.
// Damit laesst sich eine unbekannte Datei erkunden, ohne sie zu lesen.
struct TypeCount {
    std::string type;
    std::size_t count;
    std::size_t maxDepth;
};
std::vector<TypeCount> summarize(const Document& doc);

}  // namespace g2::xsi
