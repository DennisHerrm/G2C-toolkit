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

// Kommentar am Zeilenende, samt "//". Leer, wenn die Zeile keinen hat oder
// selbst nur ein Kommentar ist.
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

// Argumente eines $aseanimgrab auswerten. Dieselbe Funktion dient beim
// Speichern als Vergleich, ob sich der Eintrag geaendert hat — zwei
// verschiedene Lesarten derselben Zeile darf es nicht geben.
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
            // Unbekannt: aufheben, damit es beim Speichern nicht verloren geht.
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

    // Kommentarzeilen sammeln, bis ein Befehl kommt.
    //
    // Sie gehoerten bisher zu nichts und gingen beim Schreiben verloren.
    // Wer sein Skript gliedert, will das in der erzeugten animation.cfg
    // wiederfinden — bei 1683 Sequenzen ist der Unterschied zwischen einer
    // gegliederten Datei und einer Wand aus Zahlen erheblich.
    std::vector<std::string> pendingComments;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        {
            // Reine Kommentar- oder Leerzeile? Merken statt verwerfen.
            std::string t = line;
            const std::size_t a = t.find_first_not_of(" \t");
            if (a == std::string::npos) {
                // Leerzeile: nur behalten, wenn schon Kommentare anliegen —
                // sonst sammeln sich die Leerzeilen zwischen den Bloecken an.
                if (!pendingComments.empty()) pendingComments.emplace_back();
                continue;
            }
            if (t[a] == '/' && a + 1 < t.size() && t[a + 1] == '/') {
                pendingComments.push_back(t.substr(a));
                continue;
            }
        }

        // Kommentar am Zeilenende abtrennen und merken.
        //
        // tokenize() wirft alles ab "//" weg — richtig fuer die
        // Auswertung, aber der Text soll erhalten bleiben und spaeter in
        // der animation.cfg wieder auftauchen.
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
                // Erster Eintrag der echten _humanoid.car ist "$pcj $flatten",
                // also ein Schalter statt eines Bonenamens.
                if (!st.args.empty()) {
                    if (toLower(st.args[0]) == "$flatten") script.pcjFlatten = true;
                    else script.pcjBones.push_back(st.args[0]);
                }
                break;
            case Cmd::AseAnimGrab: {
                // Gesammelte Kommentare gehoeren zu diesem Grab.
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
                // Eine Konvertierung aus einer $include-Datei gilt, gehoert
                // aber nicht ins Hauptskript und wird dort nicht geschrieben.
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
                // Bereits eingetragen: nicht nach dem switch ein zweites Mal.
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
                // Carcass meldet unbekannte Befehle und laeuft weiter. Wir
                // behalten sie im Skript, damit der Aufrufer entscheiden kann.
                break;
            default:
                break;
        }
        // Kommentare vor einem anderen Befehl gehoeren zu diesem Befehl, nicht
        // zur naechsten Sequenz — sonst wanderten Kopfzeilen des Skripts an
        // die erste Animation. Frueher gingen sie beim Speichern verloren.
        //
        // Ausnahme: vor $aseanimgrabfinalize. Was dort steht, gehoert hinter
        // die letzte Animation, wo die Oberflaeche es zum Verschieben zeigt.
        if (st.cmd == Cmd::AseAnimGrabFinalize && !pendingComments.empty()) {
            if (fromInclude < 0)
                for (auto& c : pendingComments) script.trailingComments.push_back(std::move(c));
        } else if (st.cmd != Cmd::AseAnimGrab) {
            st.commentsBefore = std::move(pendingComments);
        }
        pendingComments.clear();

        script.statements.push_back(std::move(st));
    }

    // Was nach dem letzten Befehl steht, gehoert ans Dateiende.
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

    // Spaltenbreiten aus dem tatsaechlichen Inhalt.
    //
    // Carcass fuellt den Namen fest auf 20 Zeichen auf. Das reicht fuer
    // "BOTH_STAND1", aber nicht fuer
    // "BOTH_BOLT_BLOCK_TWO_HAND_BOTTOM_LEFT_ANAKIN" — dort rutschen die
    // Zahlen aus der Spalte, und die Datei wird unlesbar.
    //
    // Stattdessen einmal durch die Liste gehen und die breiteste Angabe je
    // Spalte merken. Die Engine trennt an Leerraum, die Ausrichtung ist ihr
    // gleichgueltig; sie ist fuer den Menschen da, der die Datei aufmacht.
    std::size_t wName = 20, wStart = 1, wCount = 1, wLoop = 1;
    std::size_t zeilen = 0;
    for (const auto& s : seqs) {
        wName = std::max(wName, s.name.size());
        wStart = std::max(wStart, std::to_string(s.targetFrame).size());
        wCount = std::max(wCount, std::to_string(s.frameCount).size());
        wLoop = std::max(wLoop, std::to_string(s.loopFrame).size());
        ++zeilen;
    }

    // Die Engine hat einen FESTEN Puffer fuer animation.cfg.
    //
    //     UI_ParseAnimationFile: File ... too long (172308 > 159999)
    //
    // Die Ausrichtung am laengsten Namen fuellt JEDE Zeile auf dessen
    // Breite auf. Bei 2463 Sequenzen und einem 46 Zeichen langen Namen sind
    // das rund 70000 Byte allein an Leerzeichen — genug, um eine Datei ueber
    // die Grenze zu heben, die vorher hineinpasste.
    //
    // Deshalb: erst rechnen, dann ausrichten. Passt es nicht, wird die
    // Namensspalte auf Ravens Mass von 20 zurueckgenommen und lange Namen
    // stehen ueber. Unschoener, aber die Datei bleibt brauchbar — und
    // Schoenheit, die das Laden verhindert, ist keine.
    constexpr std::size_t kEngineLimit = 159999;
    {
        const std::size_t proZeile = wName + 2 + wStart + 2 + wCount + 2 + wLoop + 2 + 3 + 2;
        if (zeilen * proZeile > kEngineLimit * 9 / 10) wName = 20;
    }
    for (const auto& s : seqs) {
        // Kommentare vor der Sequenz, mit Luft davor und danach.
        //
        // Ohne sie ist eine Datei mit 1683 Eintraegen eine Wand aus Zahlen.
        // Ravens eigene animation.cfg gliedert sie mit Trennern und
        // Ueberschriften, und das soll beim Neubauen erhalten bleiben.
        //
        // Die Leerzeilen sind kein Zierrat: eine Ueberschrift, die direkt
        // an der Zeile darueber klebt, wirkt wie ein Nachtrag zur vorigen
        // Sequenz statt wie der Anfang eines neuen Blocks.
        if (!s.commentsBefore.empty()) {
            if (!erste) os << "\r\n";   // nicht gleich hinter dem Kopf
            for (const auto& c : s.commentsBefore) {
                if (c.empty()) {
                    os << "\r\n";   // Leerzeile bleibt leer, nicht "//"
                } else if (c.size() >= 2 && c[0] == '/' && c[1] == '/') {
                    os << c << "\r\n";
                } else {
                    os << "// " << c << "\r\n";
                }
            }
            os << "\r\n";
        }
        erste = false;

        // Name linksbuendig, Zahlen rechtsbuendig — so stehen die Ziffern
        // untereinander und lassen sich vergleichen.
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

// Eine einzelne Zeile auswerten. Liefert einen leeren Eintrag (ohne Datei
// bzw. ohne Wurzel), wenn die Zeile kein solcher Befehl ist oder sich nicht
// lesen laesst — der Vergleich beim Speichern schlaegt dann fehl, und die
// Zeile wird neu erzeugt. Das ist immer der sichere Ausgang.
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
