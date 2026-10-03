#include "g2/xsi.h"

#include "g2/readfile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace g2::xsi {
namespace {

// Hand-written scanner instead of regex or istream. According to the analysis
// of the original binary, the dotXSI parser is the number two runtime hotspot
// (0x448f90, 3385 instructions, loop nesting depth 6). A character scanner
// over a contiguous buffer is orders of magnitude faster here than any
// stream-based solution.
class Scanner {
public:
    explicit Scanner(std::string_view s) : s_(s) {}

    struct Pos {
        std::size_t line = 1;
        std::size_t column = 1;
    };

    bool eof() const { return i_ >= s_.size(); }
    Pos  pos() const { return {line_, i_ - lineStart_ + 1}; }

    [[noreturn]] void fail(const std::string& msg) const {
        ParseError e;
        e.line = line_;
        e.column = i_ - lineStart_ + 1;
        e.message = msg;
        throw std::runtime_error(e.what());
    }

    void skipTrivia() {
        for (;;) {
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == '\n') {
                    ++line_;
                    ++i_;
                    lineStart_ = i_;
                } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                    ++i_;
                } else {
                    break;
                }
            }
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '*') {
                i_ += 2;
                while (i_ + 1 < s_.size() && !(s_[i_] == '*' && s_[i_ + 1] == '/')) {
                    if (s_[i_] == '\n') { ++line_; lineStart_ = i_ + 1; }
                    ++i_;
                }
                if (i_ + 1 >= s_.size()) fail("Blockkommentar nicht geschlossen");
                i_ += 2;
                continue;
            }
            // Semicolon and comma are pure separators with no meaning.
            if (i_ < s_.size() && (s_[i_] == ';' || s_[i_] == ',')) {
                ++i_;
                continue;
            }
            break;
        }
    }

    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    void advance() { if (i_ < s_.size()) ++i_; }

    // One token: identifier, number, or quoted string.
    std::string token() {
        if (peek() == '"') {
            advance();
            const std::size_t start = i_;
            while (i_ < s_.size() && s_[i_] != '"') {
                if (s_[i_] == '\n') fail("Zeichenkette ueber Zeilenende hinaus");
                ++i_;
            }
            if (i_ >= s_.size()) fail("Zeichenkette nicht geschlossen");
            std::string out(s_.substr(start, i_ - start));
            advance();
            return out;
        }
        const std::size_t start = i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '{' || c == '}' || c == ';' || c == ',' || c == '"' || std::isspace(
                    static_cast<unsigned char>(c)))
                break;
            ++i_;
        }
        if (i_ == start) fail(std::string("Unerwartetes Zeichen '") + peek() + "'");
        return std::string(s_.substr(start, i_ - start));
    }

    std::size_t line() const { return line_; }

private:
    std::string_view s_;
    std::size_t      i_ = 0;
    std::size_t      line_ = 1;
    std::size_t      lineStart_ = 0;
};

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

void parseBody(Scanner& sc, Template& out, int depth) {
    if (depth > 256) sc.fail("Templates zu tief verschachtelt (moeglicher Zyklus)");

    for (;;) {
        sc.skipTrivia();
        if (sc.eof()) sc.fail("Unerwartetes Dateiende, '}' fehlt");
        if (sc.peek() == '}') {
            sc.advance();
            return;
        }

        const std::size_t line = sc.line();
        const bool quoted = sc.peek() == '"';
        std::string tok = sc.token();

        // What follows the token decides whether it was a template: then either
        // '{' follows directly, or an instance name and then '{'.
        if (!quoted && isIdentStart(tok.empty() ? '\0' : tok[0])) {
            const std::size_t save = 0;
            (void)save;
            sc.skipTrivia();
            if (sc.peek() == '{') {
                sc.advance();
                Template child;
                child.type = std::move(tok);
                child.line = line;
                parseBody(sc, child, depth + 1);
                out.children.push_back(std::move(child));
                continue;
            }
            if (!sc.eof() && sc.peek() != '}' && sc.peek() != '"') {
                // Ambiguity: in "NAME SI_Foo {", NAME could be a template with
                // instance name SI_Foo, or a bare value followed by the
                // template SI_Foo. dotXSI 1.x has bare identifiers as values
                // (POSITION, NORMAL, TEX_COORD_UV in SI_Shape), so this really
                // happens.
                //
                // Rule: template types in dotXSI consistently carry the prefix
                // SI_ or XSI_. A token with this prefix is therefore never an
                // instance name but starts a new template - and the previous
                // token was a value.
                Scanner probe = sc;
                std::string maybeName;
                bool ok = true;
                try {
                    maybeName = probe.token();
                    probe.skipTrivia();
                } catch (...) {
                    ok = false;
                }
                const bool looksLikeType =
                    maybeName.rfind("SI_", 0) == 0 || maybeName.rfind("XSI_", 0) == 0;
                if (ok && !looksLikeType && probe.peek() == '{') {
                    sc = probe;
                    sc.advance();
                    Template child;
                    child.type = std::move(tok);
                    child.name = std::move(maybeName);
                    child.line = line;
                    parseBody(sc, child, depth + 1);
                    out.children.push_back(std::move(child));
                    continue;
                }
            }
        }

        out.values.emplace_back(std::move(tok));
    }
}

Version parseHeader(Scanner& sc) {
    sc.skipTrivia();
    std::string magic = sc.token();
    // Editors like Notepad put a UTF-8 BOM in front when saving.
    if (magic.rfind("\xEF\xBB\xBF", 0) == 0) magic.erase(0, 3);
    if (magic != "xsi")
        throw std::runtime_error("Keine dotXSI-Datei: erwartet \"xsi\" am Anfang, gefunden \"" +
                                 magic + "\"");
    sc.skipTrivia();
    const std::string ver = sc.token();   // e.g. "0101txt"

    Version v;
    v.raw = ver;
    if (ver.size() < 7)
        throw std::runtime_error("Unverstaendliche dotXSI-Versionskennung \"" + ver + "\"");

    const std::string digits = ver.substr(0, 4);
    const std::string kind = ver.substr(4);
    if (kind == "bin") v.binary = true;
    else if (kind != "txt")
        throw std::runtime_error("Unbekannte dotXSI-Kodierung \"" + kind + "\"");

    // No stoi here either: the version tag is a pure digit sequence, but
    // consistent conversion avoids surprises later.
    const auto twoDigits = [&](std::size_t at) {
        int out = 0;
        std::from_chars(digits.data() + at, digits.data() + at + 2, out);
        return out;
    };
    v.major = twoDigits(0);
    v.minor = twoDigits(2);

    if (v.binary)
        throw std::runtime_error(
            "Binaeres dotXSI (" + ver +
            ") wird nicht unterstuetzt. Carcass kann es auch nicht — es akzeptiert nur "
            "0101txt, 0103txt, 0300txt und 0350txt.");

    // In all variants, the version tag is followed by one more number
    // (template counter or format detail). It isn't needed but must be
    // consumed - a template type never starts with a digit.
    sc.skipTrivia();
    if (std::isdigit(static_cast<unsigned char>(sc.peek()))) (void)sc.token();
    sc.skipTrivia();
    return v;
}

}  // namespace

std::string Version::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%d%s", major, minor, binary ? " (binaer)" : "");
    return buf;
}

std::string ParseError::what() const {
    std::ostringstream os;
    os << "dotXSI Zeile " << line << ", Spalte " << column << ": " << message;
    return os.str();
}

std::optional<double> Value::asNumber() const {
    if (text_.empty()) return std::nullopt;
    // from_chars is much faster than strtod and allows no locale surprises
    // (decimal point instead of comma).
    double out = 0;
    const char* begin = text_.data();
    const char* end = begin + text_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return out;
}

std::optional<long> Value::asInt() const {
    if (text_.empty()) return std::nullopt;
    long out = 0;
    const char* begin = text_.data();
    const char* end = begin + text_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return out;
}

double Value::asNumberOrThrow(const char* context) const {
    if (const auto v = asNumber()) return *v;
    throw std::runtime_error(std::string(context) + ": \"" + text_ + "\" ist keine Zahl");
}

long Value::asIntOrThrow(const char* context) const {
    if (const auto v = asInt()) return *v;
    throw std::runtime_error(std::string(context) + ": \"" + text_ + "\" ist keine ganze Zahl");
}

const Template* Template::find(std::string_view t) const {
    for (const auto& c : children)
        if (c.type == t) return &c;
    return nullptr;
}

std::vector<const Template*> Template::findAll(std::string_view t) const {
    std::vector<const Template*> out;
    for (const auto& c : children)
        if (c.type == t) out.push_back(&c);
    return out;
}

const Template* Template::findDeep(std::string_view t) const {
    for (const auto& c : children) {
        if (c.type == t) return &c;
        if (const Template* r = c.findDeep(t)) return r;
    }
    return nullptr;
}

std::size_t Template::countDeep() const {
    std::size_t n = 1;
    for (const auto& c : children) n += c.countDeep();
    return n;
}

const Template* Document::find(std::string_view t) const {
    for (const auto& r : roots)
        if (r.type == t) return &r;
    return nullptr;
}

const Template* Document::findDeep(std::string_view t) const {
    for (const auto& r : roots) {
        if (r.type == t) return &r;
        if (const Template* x = r.findDeep(t)) return x;
    }
    return nullptr;
}

std::size_t Document::templateCount() const {
    std::size_t n = 0;
    for (const auto& r : roots) n += r.countDeep();
    return n;
}

Document parse(std::string_view text) {
    Scanner sc(text);
    Document doc;
    doc.version = parseHeader(sc);

    for (;;) {
        sc.skipTrivia();
        if (sc.eof()) break;
        if (sc.peek() == '}') sc.fail("Unerwartetes '}' auf oberster Ebene");

        const std::size_t line = sc.line();
        std::string type = sc.token();
        sc.skipTrivia();

        Template t;
        t.type = std::move(type);
        t.line = line;

        // Take everything up to the brace as the name, not just one token.
        //
        // Softimage writes scene names containing spaces and dots there. A
        // parser that expects exactly one token aborts at that point - and
        // for Raven's cutscenes it reported "'{' erwartet nach Template
        // SI_Scene" at column 29, 37 or 52, depending on how long the name
        // was. Five of 1400 files failed this way.
        //
        // We don't care about the name anyway; it just must not cause an
        // abort. A limit prevents a broken file without a brace from
        // swallowing the whole file.
        {
            int wache = 0;
            while (sc.peek() != '{' && !sc.eof() && wache++ < 64) {
                const std::string tok = sc.token();
                if (tok.empty()) break;
                t.name += t.name.empty() ? tok : " " + tok;
                sc.skipTrivia();
            }
        }
        if (sc.peek() != '{') sc.fail("'{' erwartet nach Template \"" + t.type + "\"");
        sc.advance();
        parseBody(sc, t, 0);
        doc.roots.push_back(std::move(t));
    }
    return doc;
}

Document parseFile(const std::string& path) {
    return parse(readWholeFile(path));
}

namespace {
void tally(const Template& t, std::size_t depth, std::map<std::string, TypeCount>& acc) {
    auto& e = acc[t.type];
    e.type = t.type;
    ++e.count;
    e.maxDepth = std::max(e.maxDepth, depth);
    for (const auto& c : t.children) tally(c, depth + 1, acc);
}
}  // namespace

std::vector<TypeCount> summarize(const Document& doc) {
    std::map<std::string, TypeCount> acc;
    for (const auto& r : doc.roots) tally(r, 0, acc);

    std::vector<TypeCount> out;
    out.reserve(acc.size());
    for (auto& [k, v] : acc) out.push_back(v);
    std::sort(out.begin(), out.end(), [](const TypeCount& a, const TypeCount& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.type < b.type;
    });
    return out;
}

}  // namespace g2::xsi
