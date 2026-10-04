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
        {"$cfgnamefromcar", Cmd::CfgNameFromCar},
    };
    return t;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Does a comment start at position k (at the start of a token)? Carcass's
// tokenizer (Quake scriplib) knows "//", ";", "#" and "/* ... */".
bool commentAt(const std::string& line, std::size_t k) {
    const char ch = line[k];
    if (ch == ';' || ch == '#') return true;
    return ch == '/' && k + 1 < line.size() && (line[k + 1] == '/' || line[k + 1] == '*');
}

// Splits a line into tokens. Quotes keep paths containing spaces together;
// Carcass can't do that, but it does no harm.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        if (commentAt(line, i)) break;
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
    // The same rule as tokenize(): "//" starts a comment only at the start of
    // a token and outside quotes. "models/p//x.xsi" is a path, not a comment -
    // it used to be taken as one, written into animation.cfg, and grew on
    // every edit of the line.
    std::string c;
    bool quoted = false;
    for (std::size_t k = 0; k < line.size(); ++k) {
        if (line[k] == '"') quoted = !quoted;
        if (quoted || !commentAt(line, k)) continue;
        if (k > 0 && !std::isspace(static_cast<unsigned char>(line[k - 1]))) continue;
        if (line.find_first_not_of(" \t") == k) return {};   // the whole line is a comment
        c = line.substr(k);
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
        return c;
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
            g.frameSpeed = st.argNumber(++i, "-framespeed");
        else if (f == "-enum" && i + 1 < st.args.size())
            g.enumName = st.args[++i];
        else if (f == "-qdskipstart") { inQd = true; g.hasQdSkip = true; }
        else if (f == "-qdskipstop") inQd = false;
        else if (f == "-additional" && i + 5 < st.args.size()) {
            GrabDirective::Additional a;
            a.targetOffset = static_cast<int>(st.argNumber(i + 1, "-additional"));
            a.frameCount   = static_cast<int>(st.argNumber(i + 2, "-additional"));
            a.loopFrame    = static_cast<int>(st.argNumber(i + 3, "-additional"));
            a.frameSpeed   = st.argNumber(i + 4, "-additional");
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
    int pendingBlank = 0;
    bool inBlockComment = false;
    // $basedir applies to the $aseanimgrab lines that FOLLOW it (Carcass).
    std::string currentBaseDir;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        {
            // Pure comment or blank line? Remember it instead of discarding it.
            std::string t = line;
            const std::size_t a = t.find_first_not_of(" \t");
            if (a == std::string::npos) {
                // Blank line. Inside a comment block it is part of the block;
                // in front of it, it is layout and counted separately - so it
                // comes back on save (it used to vanish) without turning into
                // an empty comment row in the table and in animation.cfg.
                if (pendingComments.empty()) ++pendingBlank;
                else pendingComments.emplace_back();
                continue;
            }
            // Inside a /* ... */ block every line is comment text.
            if (inBlockComment) {
                pendingComments.push_back(t);
                if (t.find("*/") != std::string::npos) inBlockComment = false;
                continue;
            }
            if (commentAt(t, a)) {
                // With its indentation - that is part of the author's layout.
                pendingComments.push_back(t);
                if (t.compare(a, 2, "/*") == 0 && t.find("*/", a + 2) == std::string::npos)
                    inBlockComment = true;
                continue;
            }
        }

        // Split off the end-of-line comment and remember it.
        //
        // tokenize() throws away everything from "//" on - correct for
        // evaluation, but the text should be preserved and reappear later in
        // animation.cfg.
        const std::string zeilenKommentar = trailingCommentOf(line);
        // A block comment opened at the end of a command line.
        {
            const std::size_t open = zeilenKommentar.find("/*");
            if (open != std::string::npos && zeilenKommentar.find("*/", open + 2) == std::string::npos)
                inBlockComment = true;
        }

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        Statement st = statementFromTokens(tokens, lineNo, originName);
        st.sourceLine = line;
        st.fromInclude = fromInclude;

        switch (st.cmd) {
            case Cmd::BaseDir:
                script.baseDir = st.arg(0, "$basedir");
                currentBaseDir = script.baseDir;
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
                g.blankBefore = pendingBlank;
                pendingBlank = 0;
                g.trailingComment = zeilenKommentar;
                g.sourceLine = line;
                g.fromInclude = fromInclude;
                g.baseDir = currentBaseDir;
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
                st.blankBefore = pendingBlank;
                pendingBlank = 0;
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
                // Carcass reads $include relative to the folder ABOVE "base"
                // ("$include base/models/..."); also try that and the asset
                // root itself.
                if (!fs::exists(p) && fs::path(rel).is_relative()) {
                    for (fs::path up = fs::absolute(fs::path(originName)).parent_path(); !up.empty();
                         up = up.parent_path()) {
                        const fs::path cand = up / rel;
                        if (fs::exists(cand)) {
                            p = cand;
                            break;
                        }
                        if (up == up.parent_path()) break;
                    }
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
                st.blankBefore = pendingBlank;
                pendingBlank = 0;
                script.statements.push_back(st);
                // Carcass reads no further - but the lines stay in the file.
                if (depth == 0)
                    while (std::getline(in, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        script.afterExit.push_back(line);
                    }
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
        //
        // Only directly after a grab, though: with $scale/$pcj in between (the
        // layout of Raven's _humanoid.car), a comment above the finalize line
        // used to jump up behind the last grab on saving.
        const bool afterGrab = !script.statements.empty() &&
                               script.statements.back().cmd == Cmd::AseAnimGrab;
        if (st.cmd == Cmd::AseAnimGrabFinalize && !pendingComments.empty() && afterGrab) {
            if (fromInclude < 0) {
                script.trailingComments.insert(script.trailingComments.end(),
                                               static_cast<std::size_t>(pendingBlank), std::string());
                for (auto& c : pendingComments) script.trailingComments.push_back(std::move(c));
            }
            pendingBlank = 0;
        } else if (st.cmd != Cmd::AseAnimGrab) {
            st.commentsBefore = std::move(pendingComments);
            st.blankBefore = pendingBlank;
            pendingBlank = 0;
        }
        pendingComments.clear();

        script.statements.push_back(std::move(st));
    }

    // Whatever follows the last command belongs at the end of the file.
    if (depth == 0) {
        script.endComments.assign(static_cast<std::size_t>(pendingBlank), std::string());
        script.endComments.insert(script.endComments.end(), pendingComments.begin(),
                                  pendingComments.end());
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
        case Cmd::CfgNameFromCar: return "$CFGNameFromCAR";
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
    // "1e10", "inf", "nan" are numbers to from_chars, but end up in int
    // fields - undefined behaviour, in practice INT_MIN in animation.cfg.
    if (!std::isfinite(out) || std::fabs(out) > 2.0e9)
        throw std::runtime_error(std::string(what) + " in \"" + file + "\" Zeile " +
                                 std::to_string(line) + ": \"" + s + "\" liegt ausserhalb des Zahlenbereichs");
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
                const std::size_t a = c.find_first_not_of(" \t");
                if (a == std::string::npos) {
                    os << "\r\n";   // a blank line stays blank, not "//"
                } else if (c.compare(a, 2, "//") == 0) {
                    os << c.substr(a) << "\r\n";
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
    // Plain LF only if the file has no CRLF at all; mixed files stay CRLF.
    if (text.find('\n') != std::string::npos && text.find("\r\n") == std::string::npos) s.newline = "\n";
    s.noFinalNewline = !text.empty() && text.back() != '\n';
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

// --- Model settings -----------------------------------------------------------

ModelSettings modelSettingsOf(const Script& s) {
    // Own lines only. Counting the $include's lines too made the dialog copy
    // them into the main script: "$pcj $flatten" from an include came back as
    // a second line in the main file, and removing it there did nothing.
    ModelSettings m;
    for (const auto& st : s.statements) {
        if (st.fromInclude >= 0) continue;
        if (st.cmd == Cmd::Scale && !st.args.empty()) {
            try {
                m.scale = st.argNumber(0, "$scale");
            } catch (const std::exception&) {
            }
        } else if (st.cmd == Cmd::KeepMotion) {
            m.keepMotion = true;
        } else if (st.cmd == Cmd::Pcj && !st.args.empty()) {
            m.pcj.push_back(st.args[0]);
        }
    }
    return m;
}

namespace {

bool own(const Statement& st) { return st.fromInclude < 0; }

Statement makeStatement(Cmd cmd, std::vector<std::string> args) {
    Statement st;
    st.cmd = cmd;
    st.raw = cmdName(cmd);
    st.args = std::move(args);
    return st;
}

// New arguments, same end-of-line comment.
void rewrite(Statement& st, std::vector<std::string> args) {
    const std::string comment = trailingCommentOf(st.sourceLine);
    st.args = std::move(args);
    std::string line = st.raw;
    for (const auto& a : st.args) line += " " + a;
    if (!comment.empty()) line += "  " + comment;
    st.sourceLine = line;
}

// Removes a statement without losing the comment lines above it: they move to
// whatever comes next.
// Puts comment lines in front of statement pos. A grab statement is only a
// placeholder for s.grabs - comments given to it were never written, so they
// go to the grab itself.
void attachComments(Script& s, std::size_t pos, const std::vector<std::string>& comments) {
    if (comments.empty()) return;
    std::vector<std::string>* dst = &s.endComments;
    if (pos < s.statements.size()) {
        dst = &s.statements[pos].commentsBefore;
        if (s.statements[pos].cmd == Cmd::AseAnimGrab) {
            const std::size_t gi = static_cast<std::size_t>(
                std::count_if(s.statements.begin(), s.statements.begin() + static_cast<long>(pos),
                              [](const Statement& st) { return st.cmd == Cmd::AseAnimGrab; }));
            dst = gi < s.grabs.size() ? &s.grabs[gi].commentsBefore : &s.trailingComments;
        }
    }
    dst->insert(dst->begin(), comments.begin(), comments.end());
}

void eraseKeepingComments(Script& s, std::size_t i) {
    std::vector<std::string> comments = std::move(s.statements[i].commentsBefore);
    s.statements.erase(s.statements.begin() + static_cast<long>(i));
    attachComments(s, i, comments);
}

// Where new header lines go: before $aseanimgrabfinalize, as in Raven's
// scripts; otherwise after the last grab; otherwise before the conversion;
// otherwise at the end.
std::size_t headerInsertPos(const Script& s) {
    std::size_t lastGrab = s.statements.size();
    for (std::size_t i = 0; i < s.statements.size(); ++i) {
        const auto& st = s.statements[i];
        if (!own(st)) continue;
        if (st.cmd == Cmd::AseAnimGrabFinalize) return i;
        if (st.cmd == Cmd::AseAnimGrab) lastGrab = i;
    }
    if (lastGrab < s.statements.size()) return lastGrab + 1;
    for (std::size_t i = 0; i < s.statements.size(); ++i)
        if (own(s.statements[i]) && (s.statements[i].cmd == Cmd::AseAnimConvertMdx ||
                                     s.statements[i].cmd == Cmd::AseAnimConvertMdxNoAsk))
            return i;
    return s.statements.size();
}

// Position right after the last own statement of one of the given kinds, or
// headerInsertPos if there is none.
std::size_t afterLastOf(const Script& s, std::initializer_list<Cmd> kinds) {
    std::size_t pos = s.statements.size() + 1;
    for (std::size_t i = 0; i < s.statements.size(); ++i)
        if (own(s.statements[i]) &&
            std::find(kinds.begin(), kinds.end(), s.statements[i].cmd) != kinds.end())
            pos = i + 1;
    return pos <= s.statements.size() ? pos : headerInsertPos(s);
}

std::string formatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

}  // namespace

void applyModelSettings(Script& s, const ModelSettings& m) {
    // Only what actually changes is touched. A value that comes from an
    // $include file and stays the same must not get a second line here.
    const ModelSettings cur = modelSettingsOf(s);

    // --- $scale ---
    if (cur.scale != m.scale) {
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < s.statements.size(); ++i)
            if (own(s.statements[i]) && s.statements[i].cmd == Cmd::Scale) idx.push_back(i);
        if (!m.scale) {
            for (std::size_t k = idx.size(); k-- > 0;) eraseKeepingComments(s, idx[k]);
        } else if (idx.empty()) {
            s.statements.insert(s.statements.begin() + static_cast<long>(headerInsertPos(s)),
                                makeStatement(Cmd::Scale, {formatNumber(*m.scale)}));
        } else {
            // The last one is the one Carcass uses; earlier ones stay as they are.
            Statement& st = s.statements[idx.back()];
            bool same = false;
            try {
                same = !st.args.empty() && std::stod(st.args[0]) == *m.scale;
            } catch (...) {
            }
            if (!same) rewrite(st, {formatNumber(*m.scale)});
        }
    }

    // --- $keepmotion ---
    if (cur.keepMotion != m.keepMotion) {
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < s.statements.size(); ++i)
            if (own(s.statements[i]) && s.statements[i].cmd == Cmd::KeepMotion) idx.push_back(i);
        if (!m.keepMotion) {
            for (std::size_t k = idx.size(); k-- > 0;) eraseKeepingComments(s, idx[k]);
        } else if (idx.empty()) {
            s.statements.insert(s.statements.begin() + static_cast<long>(afterLastOf(s, {Cmd::Scale})),
                                makeStatement(Cmd::KeepMotion, {}));
        }
    }

    // --- $pcj ---
    if (cur.pcj != m.pcj) {
        std::vector<std::size_t> idx;
        std::vector<std::string> current;
        for (std::size_t i = 0; i < s.statements.size(); ++i)
            if (own(s.statements[i]) && s.statements[i].cmd == Cmd::Pcj) {
                idx.push_back(i);
                current.push_back(s.statements[i].args.empty() ? std::string()
                                                               : s.statements[i].args[0]);
            }
        if (current != m.pcj) {
            // Rebuild the block where it was; unchanged entries keep their line.
            std::vector<Statement> old;
            for (const std::size_t i : idx) old.push_back(s.statements[i]);
            std::vector<std::string> comments;
            std::size_t pos = idx.empty() ? s.statements.size() + 1 : idx.front();
            for (std::size_t k = idx.size(); k-- > 0;) {
                auto& c = s.statements[idx[k]].commentsBefore;
                comments.insert(comments.begin(), c.begin(), c.end());
                s.statements.erase(s.statements.begin() + static_cast<long>(idx[k]));
            }
            if (pos > s.statements.size()) pos = afterLastOf(s, {Cmd::Scale, Cmd::KeepMotion});
            std::vector<Statement> fresh;
            for (const auto& name : m.pcj) {
                auto it = std::find_if(old.begin(), old.end(), [&](const Statement& st) {
                    return !st.args.empty() && st.args[0] == name;
                });
                if (it != old.end()) {
                    Statement st = *it;
                    st.commentsBefore.clear();
                    fresh.push_back(std::move(st));
                    old.erase(it);
                } else {
                    fresh.push_back(makeStatement(Cmd::Pcj, {name}));
                }
            }
            if (!fresh.empty()) {
                fresh.front().commentsBefore = std::move(comments);
                s.statements.insert(s.statements.begin() + static_cast<long>(pos), fresh.begin(),
                                    fresh.end());
            } else {
                attachComments(s, pos, comments);
            }
        }
    }

    // The values the build reads, recomputed from ALL lines the same way the
    // parser does - includes too. Setting them from m alone left memory and
    // file disagreeing whenever an include carried one of these lines.
    s.scale.reset();
    s.keepMotion = false;
    s.pcjBones.clear();
    s.pcjFlatten = false;
    for (const auto& st : s.statements) {
        if (st.cmd == Cmd::Scale && !st.args.empty()) {
            try {
                s.scale = st.argNumber(0, "$scale");
            } catch (const std::exception&) {
            }
        } else if (st.cmd == Cmd::KeepMotion) {
            s.keepMotion = true;
        } else if (st.cmd == Cmd::Pcj && !st.args.empty()) {
            if (toLower(st.args[0]) == "$flatten") s.pcjFlatten = true;
            else s.pcjBones.push_back(st.args[0]);
        }
    }
}

}  // namespace g2::car
