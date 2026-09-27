#include "g2/carscript.h"

#include "g2/readfile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace g2::car {
namespace {

const std::unordered_map<std::string, Cmd>& table() {
    static const std::unordered_map<std::string, Cmd> t = {
        {"$basedir", Cmd::BaseDir},
        {"$modelname", Cmd::ModelName},
        {"$origin", Cmd::Origin},
        {"$scale", Cmd::Scale},
        {"$flatten", Cmd::Flatten},
        {"$keepmotion", Cmd::KeepMotion},
        {"$bonehiercap", Cmd::BoneHierCap},
        {"$include", Cmd::Include},
        {"$pcj", Cmd::Pcj},
        {"$aseanimgrabinit", Cmd::AseAnimGrabInit},
        {"$aseanimgrab", Cmd::AseAnimGrab},
        {"$aseanimgrabfinalize", Cmd::AseAnimGrabFinalize},
        {"$aseanimgrab_gla", Cmd::AseAnimGrabGla},
        {"$aseanimref_gla", Cmd::AseAnimRefGla},
        {"$aseconvert", Cmd::AseConvert},
        {"$aseanimconvert", Cmd::AseAnimConvert},
        {"$aseanimconvertmdx", Cmd::AseAnimConvertMdx},
        {"$aseanimconvertmdx_noask", Cmd::AseAnimConvertMdxNoAsk},
        {"$exit", Cmd::Exit},
    };
    return t;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Splits a line into tokens. Quotes keep paths containing spaces together;
// Carcass can't do that, but it does no harm.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (line[i] == '"') {
            ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != '"') ++i;
            out.push_back(line.substr(start, i - start));
            if (i < line.size()) ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

// Comment at the end of the line, including "//". Empty if the line has none
// or is itself only a comment.
std::string trailingCommentOf(const std::string& line) {
    std::string c;
    const std::size_t k = line.find("//");
    if (k != std::string::npos && line.find_first_not_of(" \t") != k) {
        c = line.substr(k);
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
    }
    return c;
}

Statement statementFromTokens(const std::vector<std::string>& tokens, std::size_t lineNo,
                              const std::string& file) {
    Statement st;
    st.raw = tokens[0];
    st.line = lineNo;
    st.file = file;
    st.cmd = cmdFromString(toLower(tokens[0]));
    st.args.assign(tokens.begin() + 1, tokens.end());
    return st;
}

// Evaluates the arguments of an $aseanimgrab. The same function is used when
// saving to check whether the entry has changed - there must never be two
// different interpretations of the same line.
GrabDirective grabFromStatement(const Statement& st) {
    GrabDirective g;
    g.line = st.line;
    g.file = st.arg(0, "$aseanimgrab");
    bool inQd = false;
    for (std::size_t i = 1; i < st.args.size(); ++i) {
        const std::string f = toLower(st.args[i]);
        if (f == "-loop" && i + 1 < st.args.size())
            g.loop = static_cast<int>(st.argNumber(++i, "-loop"));
        else if (f == "-framespeed" && i + 1 < st.args.size())
            g.frameSpeed = static_cast<int>(st.argNumber(++i, "-framespeed"));
        else if (f == "-enum" && i + 1 < st.args.size())
            g.enumName = st.args[++i];
        else if (f == "-qdskipstart") { inQd = true; g.hasQdSkip = true; }
        else if (f == "-qdskipstop") inQd = false;
        else if (f == "-additional" && i + 5 < st.args.size()) {
            GrabDirective::Additional a;
            a.targetOffset = static_cast<int>(st.argNumber(i + 1, "-additional"));
            a.frameCount   = static_cast<int>(st.argNumber(i + 2, "-additional"));
            a.loopFrame    = static_cast<int>(st.argNumber(i + 3, "-additional"));
            a.frameSpeed   = static_cast<int>(st.argNumber(i + 4, "-additional"));
            a.name         = st.args[i + 5];
            a.insideQdSkip = inQd;
            g.additional.push_back(std::move(a));
            i += 5;
        } else {
            // Unknown: keep it so it isn't lost when saving.
            g.extraArgs.push_back(st.args[i]);
        }
    }
    return g;
}

ConvertDirective convertFromStatement(const Statement& st) {
    ConvertDirective c;
    c.noAsk = st.cmd == Cmd::AseAnimConvertMdxNoAsk;
    c.root = st.arg(0, "$aseanimconvertmdx");
    for (std::size_t i = 1; i < st.args.size(); ++i) {
        const std::string f = toLower(st.args[i]);
        if (f == "-makeskel" && i + 1 < st.args.size()) c.makeSkel = st.args[++i];
        else if (f == "-origin" && i + 3 < st.args.size()) {
            c.origin = std::array<double, 3>{st.argNumber(i + 1, "-origin"),
                                             st.argNumber(i + 2, "-origin"),
                                             st.argNumber(i + 3, "-origin")};
            i += 3;
        } else if (f == "-makeskin") {
            c.makeSkin = true;
        } else {
            c.extraArgs.push_back(st.args[i]);
        }
    }
    return c;
}

void parseInto(Script& script, const std::string& text, const std::string& originName,
               const ParseOptions& opt, int depth, const std::string& baseDirForIncludes,
               int fromInclude, int& nextIncludeId) {
    if (depth > opt.maxIncludeDepth)
        throw std::runtime_error("$include tiefer als " + std::to_string(opt.maxIncludeDepth) +
                                 " Ebenen verschachtelt (in \"" + originName + "\")");

    std::istringstream in(text);
    std::string line;
    std::size_t lineNo = 0;

    // Collect comment lines until a command comes.
    //
    // Previously they belonged to nothing and were lost on writing. Anyone
    // who structures their script wants to find that structure again in the
    // generated animation.cfg - with 1683 sequences the difference between a
    // structured file and a wall of numbers is considerable.
    std::vector<std::string> pendingComments;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        {
            // Pure comment or blank line? Remember it instead of discarding it.
            std::string t = line;
            const std::size_t a = t.find_first_not_of(" \t");
            if (a == std::string::npos) {
                // Blank line: keep it only if comments are already pending -
                // otherwise the blank lines between blocks pile up.
                if (!pendingComments.empty()) pendingComments.emplace_back();
                continue;
            }
            if (t[a] == '/' && a + 1 < t.size() && t[a + 1] == '/') {
                pendingComments.push_back(t.substr(a));
                continue;
            }
        }

        // Split off the end-of-line comment and remember it.
        //
        // tokenize() throws away everything from "//" on - correct for
        // evaluation, but the text should be preserved and reappear later in
        // animation.cfg.
        const std::string zeilenKommentar = trailingCommentOf(line);

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        Statement st = statementFromTokens(tokens, lineNo, originName);
        st.sourceLine = line;
        st.fromInclude = fromInclude;

        switch (st.cmd) {
            case Cmd::BaseDir:
                script.baseDir = st.arg(0, "$basedir");
                break;
            case Cmd::ModelName:
                script.modelName = st.arg(0, "$modelname");
                break;
            case Cmd::Scale:
                script.scale = st.argNumber(0, "$scale");
                break;
            case Cmd::Origin:
                script.origin = std::array<double, 3>{st.argNumber(0, "$origin X"),
                                                      st.argNumber(1, "$origin Y"),
                                                      st.argNumber(2, "$origin Z")};
                break;
            case Cmd::Flatten:
                script.flatten = true;
                break;
            case Cmd::Pcj:
                // The first entry of the real _humanoid.car is "$pcj $flatten",
                // i.e. a switch instead of a bone name.
                if (!st.args.empty()) {
                    if (toLower(st.args[0]) == "$flatten") script.pcjFlatten = true;
                    else script.pcjBones.push_back(st.args[0]);
                }
                break;
            case Cmd::AseAnimGrab: {
                // The collected comments belong to this grab.
                GrabDirective g = grabFromStatement(st);
                g.commentsBefore = std::move(pendingComments);
                pendingComments.clear();
                g.trailingComment = zeilenKommentar;
                g.sourceLine = line;
                g.fromInclude = fromInclude;
                script.grabs.push_back(std::move(g));
                break;
            }
            case Cmd::AseAnimConvertMdx:
            case Cmd::AseAnimConvertMdxNoAsk: {
                ConvertDirective c = convertFromStatement(st);
                c.sourceLine = line;
                c.trailingComment = zeilenKommentar;
                // A conversion from an $include file applies, but it doesn't
                // belong in the main script and is not written there.
                c.fromInclude = fromInclude;
                script.convert = std::move(c);
                break;
            }
            case Cmd::KeepMotion:
                script.keepMotion = true;
                break;
            case Cmd::Include: {
                const std::string rel = st.arg(0, "$include");
                if (depth == 0) {
                    st.includeId = nextIncludeId++;
                    st.grabsBefore = static_cast<std::size_t>(
                        std::count_if(script.grabs.begin(), script.grabs.end(),
                                      [](const GrabDirective& g) { return g.fromInclude < 0; }));
                }
                st.commentsBefore = std::move(pendingComments);
                pendingComments.clear();
                script.statements.push_back(st);
                // Already added: don't add it a second time after the switch.
                if (!opt.followIncludes) continue;

                namespace fs = std::filesystem;
                fs::path p(rel);
                if (p.is_relative()) {
                    const fs::path base = baseDirForIncludes.empty()
                                              ? fs::path(originName).parent_path()
                                              : fs::path(baseDirForIncludes);
                    p = base / p;
                }
                std::string sub;
                try {
                    sub = readWholeFile(p.string());
                } catch (const std::exception&) {
                    throw std::runtime_error("$include in \"" + originName + "\" Zeile " +
                                             std::to_string(lineNo) + ": kann \"" + p.string() +
                                             "\" nicht oeffnen");
                }
                parseInto(script, sub, p.string(), opt, depth + 1, baseDirForIncludes,
                          depth == 0 ? st.includeId : fromInclude, nextIncludeId);
                continue;
            }
            case Cmd::Exit:
                st.commentsBefore = std::move(pendingComments);
                pendingComments.clear();
                script.statements.push_back(st);
                return;
            case Cmd::Unknown:
                // Carcass reports unknown commands and keeps going. We keep
                // them in the script so the caller can decide.
                break;
            default:
                break;
        }
        // Comments before any other command belong to that command, not to
        // the next sequence - otherwise the script's header lines would move
        // to the first animation. Previously they were lost on saving.
        //
        // Exception: before $aseanimgrabfinalize. What stands there belongs
        // after the last animation, where the UI shows it for moving.
        if (st.cmd == Cmd::AseAnimGrabFinalize && !pendingComments.empty()) {
            if (fromInclude < 0)
                for (auto& c : pendingComments) script.trailingComments.push_back(std::move(c));
        } else if (st.cmd != Cmd::AseAnimGrab) {
            st.commentsBefore = std::move(pendingComments);
        }
        pendingComments.clear();

        script.statements.push_back(std::move(st));
    }

    // Whatever follows the last command belongs at the end of the file.
    if (depth == 0) {
        while (!pendingComments.empty() && pendingComments.back().empty())
            pendingComments.pop_back();
        script.endComments = std::move(pendingComments);
    }
}

}  // namespace

const char* cmdName(Cmd c) {
    switch (c) {
        case Cmd::BaseDir: return "$basedir";
        case Cmd::ModelName: return "$modelname";
        case Cmd::Origin: return "$origin";
        case Cmd::Scale: return "$scale";
        case Cmd::Flatten: return "$flatten";
        case Cmd::KeepMotion: return "$keepmotion";
        case Cmd::BoneHierCap: return "$bonehiercap";
        case Cmd::Include: return "$include";
        case Cmd::Pcj: return "$pcj";
        case Cmd::AseAnimGrabInit: return "$aseanimgrabinit";
        case Cmd::AseAnimGrab: return "$aseanimgrab";
        case Cmd::AseAnimGrabFinalize: return "$aseanimgrabfinalize";
        case Cmd::AseAnimGrabGla: return "$aseanimgrab_gla";
        case Cmd::AseAnimRefGla: return "$aseanimref_gla";
        case Cmd::AseConvert: return "$aseconvert";
        case Cmd::AseAnimConvert: return "$aseanimconvert";
        case Cmd::AseAnimConvertMdx: return "$aseanimconvertmdx";
        case Cmd::AseAnimConvertMdxNoAsk: return "$aseanimconvertmdx_noask";
        case Cmd::Exit: return "$exit";
        case Cmd::Unknown: return "<unbekannt>";
    }
    return "<unbekannt>";
}

Cmd cmdFromString(const std::string& s) {
    const auto it = table().find(s);
    return it == table().end() ? Cmd::Unknown : it->second;
}

const std::string& Statement::arg(std::size_t i, const char* what) const {
    if (i >= args.size())
        throw std::runtime_error(std::string(what) + " in \"" + file + "\" Zeile " +
                                 std::to_string(line) + ": Argument " + std::to_string(i + 1) +
                                 " fehlt");
    return args[i];
}

double Statement::argNumber(std::size_t i, const char* what) const {
    const std::string& s = arg(i, what);
    double out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        throw std::runtime_error(std::string(what) + " in \"" + file + "\" Zeile " +
                                 std::to_string(line) + ": \"" + s + "\" ist keine Zahl");
    return out;
}

std::string GrabDirective::derivedName() const {
    std::string base = file;
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return base;
}

std::vector<Sequence> Script::buildSequences(
    const std::function<int(const std::string&)>& frameCountOf) const {
    std::vector<Sequence> out;
    int cursor = 0;
    for (const auto& g : grabs) {
        const int n = frameCountOf ? frameCountOf(g.file) : 0;

        Sequence s;
        s.name = g.enumName ? *g.enumName : g.derivedName();
        s.targetFrame = cursor;
        s.frameCount = n;
        s.loopFrame = g.loop.value_or(-1);
        s.frameSpeed = g.frameSpeed.value_or(0);
        s.sourceFile = g.file;
        out.push_back(std::move(s));

        for (const auto& a : g.additional) {
            Sequence x;
            x.name = a.name;
            x.targetFrame = cursor + a.targetOffset;
            x.frameCount = a.frameCount;
            x.loopFrame = a.loopFrame;
            x.frameSpeed = a.frameSpeed;
            x.sourceFile = g.file;
            x.fromAdditional = true;
            x.insideQdSkip = a.insideQdSkip;
            out.push_back(std::move(x));
        }
        cursor += n;
    }
    return out;
}

std::string writeAnimationCfg(const std::vector<Sequence>& seqs, const std::string& headerComment) {
    std::ostringstream os;
    os << "// " << headerComment << "\r\n//\r\n"
       << "// Format:  enum, targetFrame, frameCount, loopFrame, frameSpeed\r\n//\r\n";
    bool erste = true;

    // Column widths from the actual content.
    //
    // Carcass always pads the name to a fixed 20 characters. That is enough
    // for "BOTH_STAND1", but not for
    // "BOTH_BOLT_BLOCK_TWO_HAND_BOTTOM_LEFT_ANAKIN" - there the numbers slip
    // out of their column and the file becomes unreadable.
    //
    // Instead, walk the list once and remember the widest value per column.
    // The engine splits on whitespace and doesn't care about alignment; it is
    // there for the human who opens the file.
    std::size_t wName = 20, wStart = 1, wCount = 1, wLoop = 1;
    std::size_t zeilen = 0;
    for (const auto& s : seqs) {
        wName = std::max(wName, s.name.size());
        wStart = std::max(wStart, std::to_string(s.targetFrame).size());
        wCount = std::max(wCount, std::to_string(s.frameCount).size());
        wLoop = std::max(wLoop, std::to_string(s.loopFrame).size());
        ++zeilen;
    }

    // The engine has a FIXED buffer for animation.cfg.
    //
    //     UI_ParseAnimationFile: File ... too long (172308 > 159999)
    //
    // Aligning to the longest name pads EVERY line to its width. With 2463
    // sequences and a 46-character name that is about 70000 bytes of spaces
    // alone - enough to push a file over the limit that fit before.
    //
    // Hence: calculate first, then align. If it doesn't fit, the name column
    // is reduced to Raven's width of 20 and long names overhang. Less pretty,
    // but the file stays usable - and prettiness that prevents loading isn't
    // pretty at all.
    constexpr std::size_t kEngineLimit = 159999;
    {
        const std::size_t proZeile = wName + 2 + wStart + 2 + wCount + 2 + wLoop + 2 + 3 + 2;
        if (zeilen * proZeile > kEngineLimit * 9 / 10) wName = 20;
    }
    for (const auto& s : seqs) {
        // Comments before the sequence, with some space before and after.
        //
        // Without them a file with 1683 entries is a wall of numbers. Raven's
        // own animation.cfg structures it with separators and headings, and
        // that should be preserved when rebuilding.
        //
        // The blank lines aren't decoration: a heading glued directly to the
        // line above looks like an addendum to the previous sequence rather
        // than the start of a new block.
        if (!s.commentsBefore.empty()) {
            if (!erste) os << "\r\n";   // not directly after the header
            for (const auto& c : s.commentsBefore) {
                if (c.empty()) {
                    os << "\r\n";   // a blank line stays blank, not "//"
                } else if (c.size() >= 2 && c[0] == '/' && c[1] == '/') {
                    os << c << "\r\n";
                } else {
                    os << "// " << c << "\r\n";
                }
            }
            os << "\r\n";
        }
        erste = false;

        // Name left-aligned, numbers right-aligned - that way the digits line
        // up vertically and can be compared.
        std::string name = s.name;
        if (name.size() < wName) name.append(wName - name.size(), ' ');

        const auto rechts = [](long v, std::size_t breite) {
            std::string t = std::to_string(v);
            return t.size() < breite ? std::string(breite - t.size(), ' ') + t : t;
        };

        os << name << "  " << rechts(s.targetFrame, wStart) << "  "
           << rechts(s.frameCount, wCount) << "  " << rechts(s.loopFrame, wLoop) << "  "
           << s.frameSpeed;
        if (!s.trailingComment.empty()) {
            const std::string& c = s.trailingComment;
            os << (c.size() >= 2 && c[0] == '/' && c[1] == '/' ? "  " : "  // ") << c;
        }
        os << "\r\n";
    }
    return os.str();
}

Script parse(const std::string& text, const std::string& originName, const ParseOptions& opt) {
    Script s;
    int nextIncludeId = 0;
    parseInto(s, text, originName, opt, 0, "", -1, nextIncludeId);
    return s;
}

// Evaluates a single line. Returns an empty entry (no file or no root,
// respectively) if the line isn't such a command or can't be parsed - the
// comparison on saving then fails and the line is regenerated. That is
// always the safe outcome.
GrabDirective parseGrabLine(const std::string& line) {
    const auto tokens = tokenize(line);
    if (tokens.empty() || toLower(tokens[0]) != "$aseanimgrab") return {};
    try {
        GrabDirective g = grabFromStatement(statementFromTokens(tokens, 0, "<zeile>"));
        g.trailingComment = trailingCommentOf(line);
        return g;
    } catch (const std::exception&) {
        return {};
    }
}

ConvertDirective parseConvertLine(const std::string& line) {
    const auto tokens = tokenize(line);
    if (tokens.empty()) return {};
    const Cmd c = cmdFromString(toLower(tokens[0]));
    if (c != Cmd::AseAnimConvertMdx && c != Cmd::AseAnimConvertMdxNoAsk) return {};
    try {
        ConvertDirective d = convertFromStatement(statementFromTokens(tokens, 0, "<zeile>"));
        d.trailingComment = trailingCommentOf(line);
        return d;
    } catch (const std::exception&) {
        return {};
    }
}

Script parseFile(const std::string& path, const ParseOptions& opt) {
    return parse(readWholeFile(path), path, opt);
}

}  // namespace g2::car
