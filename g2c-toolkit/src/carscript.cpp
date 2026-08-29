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

// Zerlegt eine Zeile in Tokens. Anfuehrungszeichen halten Pfade mit
// Leerzeichen zusammen; Carcass kann das nicht, aber es schadet nicht.
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

void parseInto(Script& script, const std::string& text, const std::string& originName,
               const ParseOptions& opt, int depth, const std::string& baseDirForIncludes) {
    if (depth > opt.maxIncludeDepth)
        throw std::runtime_error("$include tiefer als " + std::to_string(opt.maxIncludeDepth) +
                                 " Ebenen verschachtelt (in \"" + originName + "\")");

    std::istringstream in(text);
    std::string line;
    std::size_t lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        Statement st;
        st.raw = tokens[0];
        st.line = lineNo;
        st.file = originName;
        st.cmd = cmdFromString(toLower(tokens[0]));
        st.args.assign(tokens.begin() + 1, tokens.end());

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
                // Erster Eintrag der echten _humanoid.car ist "$pcj $flatten",
                // also ein Schalter statt eines Bonenamens.
                if (!st.args.empty()) {
                    if (toLower(st.args[0]) == "$flatten") script.pcjFlatten = true;
                    else script.pcjBones.push_back(st.args[0]);
                }
                break;
            case Cmd::AseAnimGrab: {
                GrabDirective g;
                g.line = lineNo;
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
                    }
                }
                script.grabs.push_back(std::move(g));
                break;
            }
            case Cmd::AseAnimConvertMdx:
            case Cmd::AseAnimConvertMdxNoAsk: {
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
                    }
                }
                script.convert = std::move(c);
                break;
            }
            case Cmd::KeepMotion:
                script.keepMotion = true;
                break;
            case Cmd::Include: {
                const std::string rel = st.arg(0, "$include");
                script.statements.push_back(st);
                if (!opt.followIncludes) break;

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
                parseInto(script, sub, p.string(), opt, depth + 1, baseDirForIncludes);
                continue;
            }
            case Cmd::Exit:
                script.statements.push_back(st);
                return;
            case Cmd::Unknown:
                // Carcass meldet unbekannte Befehle und laeuft weiter. Wir
                // behalten sie im Skript, damit der Aufrufer entscheiden kann.
                break;
            default:
                break;
        }
        script.statements.push_back(std::move(st));
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
    for (const auto& s : seqs) {
        std::string name = s.name;
        // Carcass fuellt den Namen auf 20 Zeichen auf, danach ein Tabulator.
        if (name.size() < 20) name.append(20 - name.size(), ' ');
        os << name << "\t" << s.targetFrame << "\t" << s.frameCount << "\t" << s.loopFrame
           << "\t" << s.frameSpeed << "\r\n";
    }
    return os.str();
}

Script parse(const std::string& text, const std::string& originName, const ParseOptions& opt) {
    Script s;
    parseInto(s, text, originName, opt, 0, "");
    return s;
}

Script parseFile(const std::string& path, const ParseOptions& opt) {
    return parse(readWholeFile(path), path, opt);
}

}  // namespace g2::car
