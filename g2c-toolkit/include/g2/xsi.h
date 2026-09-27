// g2/xsi.h - dotXSI parser.
//
// dotXSI is an ASCII format from the DirectX .X family. The grammar is the
// same across all versions:
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
// Template = identifier [instance name] "{" content "}", where content is
// either further templates or values. Separators are semicolon, comma and
// whitespace, all equivalent. Comments with // and slash-star.
//
// What differs between versions are the template names and their meaning -
// not the syntax. That is why this parser is version-independent and returns
// a raw tree; the semantic interpretation happens one level up.
//
// Carcass supports 1.1, 1.3, 3.0 and 3.5 (recognizable by the header tags
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
    bool binary = false;   // "bin" instead of "txt" - not supported here
    std::string raw;

    std::string toString() const;
};

// A value in a template body. dotXSI does not strictly distinguish numbers
// from strings, so the raw text is kept and converted on demand.
class Value {
public:
    explicit Value(std::string text) : text_(std::move(text)) {}

    const std::string& text() const { return text_; }

    // Returns nullopt if the text is not a valid number.
    std::optional<double> asNumber() const;
    std::optional<long>   asInt() const;

    // Throws with position information if the conversion fails.
    double asNumberOrThrow(const char* context) const;
    long   asIntOrThrow(const char* context) const;

private:
    std::string text_;
};

struct Template {
    std::string             type;       // e.g. "SI_Model"
    std::string             name;       // instance name, often empty
    std::vector<Value>      values;     // direct values in the body
    std::vector<Template>   children;   // nested templates
    std::size_t             line = 0;   // line of the opening identifier

    // First child of the matching type, or nullptr.
    const Template* find(std::string_view type) const;

    // All children of the matching type.
    std::vector<const Template*> findAll(std::string_view type) const;

    // Recursive depth-first search.
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

// Parses dotXSI from memory. Throws std::runtime_error with line and
// column information.
Document parse(std::string_view text);
Document parseFile(const std::string& path);

// Overview of the template types that occur, in descending order of
// frequency. This lets you explore an unknown file without reading it.
struct TypeCount {
    std::string type;
    std::size_t count;
    std::size_t maxDepth;
};
std::vector<TypeCount> summarize(const Document& doc);

}  // namespace g2::xsi
