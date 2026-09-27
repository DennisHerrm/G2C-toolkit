#include "g2/animenums.h"

#include "g2/readfile.h"

#include <cctype>

namespace g2::anim {
namespace {

// Strip comments so that commented-out enums are not picked up.
std::string stripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = (i + 1 < s.size()) ? i + 2 : s.size();
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

bool isEnumChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

EnumTable parseEnumHeader(const std::string& text, const std::string& sourcePath) {
    EnumTable t;
    t.sourcePath = sourcePath;

    const std::string clean = stripComments(text);
    const std::size_t start = clean.find("typedef enum");
    if (start == std::string::npos) return t;

    const std::size_t open = clean.find('{', start);
    if (open == std::string::npos) return t;

    // Up to the closing brace of the enum list.
    std::size_t depth = 0, end = open;
    for (std::size_t i = open; i < clean.size(); ++i) {
        if (clean[i] == '{') ++depth;
        else if (clean[i] == '}') {
            if (--depth == 0) { end = i; break; }
        }
    }

    std::size_t i = open + 1;
    while (i < end) {
        // Collect the identifier.
        if (!isEnumChar(clean[i]) || (clean[i] >= '0' && clean[i] <= '9')) { ++i; continue; }
        const std::size_t b = i;
        while (i < end && isEnumChar(clean[i])) ++i;
        std::string name = clean.substr(b, i - b);

        // An assignment may follow.
        std::size_t j = i;
        while (j < end && (clean[j] == ' ' || clean[j] == '\t')) ++j;
        if (j < end && clean[j] == '=') {
            while (j < end && clean[j] != ',' && clean[j] != '\n' && clean[j] != '}') ++j;
        }

        // Do NOT require a comma.
        //
        // Raven's anims.h lists many entries without a separator:
        //
        //     BOTH_STAND1		//# Standing idle, no weapon, hands down
        //     BOTH_STAND1IDLE1	//# Random standing idle
        //
        // That is invalid C, but it is what the file contains - apparently it
        // is only read by the tools, never compiled. A parser that insists on
        // the comma therefore misses a large part of the table and later
        // reports perfectly valid sequences as unknown.
        //
        // So any uppercase identifier at the start of a line inside the block
        // is accepted, followed by a comma, end of line, comment or closing
        // brace.
        bool accept = false;
        if (j >= end) accept = true;
        else if (clean[j] == ',' || clean[j] == '}') accept = true;
        else if (clean[j] == '\n' || clean[j] == '\r') accept = true;

        if (accept) {
            if (!t.index.count(name)) {
                t.index.emplace(name, static_cast<int>(t.names.size()));
                t.names.push_back(std::move(name));
            }
            i = (j < end && clean[j] == ',') ? j + 1 : j;
        }
    }
    return t;
}

EnumTable parseEnumHeaderFile(const std::string& path) {
    return parseEnumHeader(readWholeFile(path), path);
}

}  // namespace g2::anim
