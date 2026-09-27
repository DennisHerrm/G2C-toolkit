#include "g2/carvalidate.h"

#include "g2/carbuild.h"
#include "g2/parallel.h"
#include "g2/xsi_anim.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace g2::car {
namespace fs = std::filesystem;
namespace {

void add(ValidateResult& r, Issue::Level lvl, std::string msg, std::string seq = {},
         std::size_t line = 0) {
    Issue i;
    i.level = lvl;
    i.message = std::move(msg);
    i.sequence = std::move(seq);
    i.line = line;
    switch (lvl) {
        case Issue::Level::Error: ++r.errors; break;
        case Issue::Level::Warning: ++r.warnings; break;
        case Issue::Level::Info: ++r.infos; break;
    }
    r.issues.push_back(std::move(i));
}

}  // namespace

ValidateResult validate(const Script& script, const std::string& carPath,
                        const ValidateOptions& opt) {
    ValidateResult r;
    r.grabs = script.grabs.size();

    const std::string carDir = fs::path(carPath).parent_path().string();
    const std::string baseDir = opt.baseDir.empty() ? script.baseDir : opt.baseDir;

    if (script.grabs.empty())
        add(r, Issue::Level::Error, "Das Skript enthaelt keine $aseanimgrab-Anweisungen");

    // --- 0a. Groesse der animation.cfg ------------------------------------
    //
    // Die Engine hat einen FESTEN Puffer und bricht ab:
    //
    //     UI_ParseAnimationFile: File ... too long (172308 > 159999)
    //
    // Das faellt erst beim Starten des Spiels auf, und die Meldung nennt
    // keine Sequenz — man sucht dann in einer 2463 Zeilen langen Datei nach
    // etwas, das gar kein einzelner Eintrag ist.
    //
    // Geschaetzt wird grosszuegig: der Name plus vier Zahlen plus Trenner.
    {
        std::size_t bytes = 200;   // Kopfzeilen
        std::size_t n = 0;
        for (const auto& g : script.grabs) {
            const std::string nm = g.enumName ? *g.enumName : g.derivedName();
            bytes += std::max<std::size_t>(nm.size(), 20) + 24;
            for (const auto& c : g.commentsBefore) bytes += c.size() + 6;
            n += 1 + g.additional.size();
            for (const auto& a : g.additional) bytes += std::max<std::size_t>(a.name.size(), 20) + 24;
        }
        constexpr std::size_t kLimit = 159999;
        if (bytes > kLimit)
            add(r, Issue::Level::Error,
                "Die animation.cfg wird etwa " + std::to_string(bytes) +
                    " Byte gross - die Engine liest hoechstens " + std::to_string(kLimit) +
                    ". Weniger Sequenzen oder kuerzere Namen sind noetig.");
        else if (bytes > kLimit * 9 / 10)
            add(r, Issue::Level::Warning,
                "Die animation.cfg wird etwa " + std::to_string(bytes) +
                    " Byte gross - die Grenze der Engine liegt bei " + std::to_string(kLimit) +
                    ".");
    }

    // --- 0. Doppelte Sequenznamen -----------------------------------------
    //
    // Die Engine schlaegt in animation.cfg nach dem NAMEN nach. Steht einer
    // zweimal drin, gewinnt der letzte Eintrag — die erste Animation ist
    // dann unerreichbar, obwohl ihre Frames in der GLA liegen und Platz
    // belegen.
    //
    // Das faellt im Spiel als "die Animation tut nichts" auf, und niemand
    // sucht die Ursache in der cfg. Beim Zusammenfuehren mehrerer Quellen
    // passiert es leicht.
    {
        std::map<std::string, std::vector<std::size_t>> gesehen;
        for (std::size_t i = 0; i < script.grabs.size(); ++i) {
            const auto& g = script.grabs[i];
            std::string n = g.enumName ? *g.enumName : g.derivedName();
            for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            gesehen[n].push_back(i);
            for (const auto& a : g.additional) {
                std::string an = a.name;
                for (auto& c : an)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                gesehen[an].push_back(i);
            }
        }
        for (const auto& [name, wo] : gesehen) {
            if (wo.size() < 2) continue;
            std::string zeilen;
            for (const std::size_t k : wo) {
                if (!zeilen.empty()) zeilen += ", ";
                zeilen += std::to_string(script.grabs[k].line);
            }
            add(r, Issue::Level::Error,
                name + " steht " + std::to_string(wo.size()) +
                    "x im Skript (Zeilen " + zeilen +
                    "). Die Engine nimmt den letzten - die uebrigen sind unerreichbar.",
                name, script.grabs[wo.front()].line);
        }
    }

    // --- 1. Quelldateien vorhanden ----------------------------------------
    // Fehlende Dateien werden zusammengefasst gemeldet. Bei einem falschen
    // -basedir fehlen alle 1289 auf einmal, und 1289 gleichlautende Zeilen
    // verdraengen jede andere Meldung — dabei ist die eigentliche Aussage
    // dann "der Pfad stimmt nicht", nicht "diese Datei fehlt".
    std::vector<std::string> resolved(script.grabs.size());
    for (std::size_t i = 0; i < script.grabs.size(); ++i) {
        resolved[i] = resolveAssetPath(script.grabs[i].file, baseDir, carDir);
        if (resolved[i].empty()) {
            ++r.missingFiles;
            if (r.missingFiles <= 8)
                add(r, Issue::Level::Error, "Datei nicht gefunden: " + script.grabs[i].file, {},
                    script.grabs[i].line);
        }
    }
    if (r.missingFiles > 8) {
        add(r, Issue::Level::Error,
            std::to_string(r.missingFiles - 8) + " weitere Dateien fehlen");
        if (r.missingFiles == script.grabs.size())
            add(r, Issue::Level::Info,
                "ALLE Quelldateien fehlen — vermutlich stimmt -basedir nicht"
                + (baseDir.empty() ? std::string(" (kein basedir gesetzt)")
                                   : (" (aktuell: " + baseDir + ")")));
    }

    // --- 2. Sequenznamen ---------------------------------------------------
    //
    // Ein Name darf nur einmal vorkommen. Assimilate meldet das als
    // "animation enum %s is used %d times"; im Ergebnis ueberschreibt die
    // spaetere Sequenz die fruehere in der animation.cfg.
    std::map<std::string, int> nameCount;
    std::vector<std::string> allNames;
    for (const auto& g : script.grabs) {
        allNames.push_back(g.enumName ? *g.enumName : g.derivedName());
        for (const auto& a : g.additional) allNames.push_back(a.name);
    }
    r.sequences = allNames.size();
    for (const auto& n : allNames) ++nameCount[n];
    for (const auto& [n, c] : nameCount)
        if (c > 1)
            add(r, Issue::Level::Error,
                "Sequenzname " + std::to_string(c) + "-mal vergeben", n);

    // --- 3. Namen gegen die Enumtabelle -----------------------------------
    //
    // Unbekannte Namen sind eine WARNUNG, kein Fehler. Ein Mod erweitert die
    // Tabelle laufend; Assimilates harte Ablehnung wuerde die Arbeit daran
    // blockieren. Bei Movie Duels betrifft das 165 Sequenzen.
    if (opt.enums && !opt.enums->empty()) {
        std::size_t unknown = 0;
        for (const auto& n : allNames) {
            if (opt.enums->contains(n)) continue;
            ++unknown;
            if (unknown <= opt.maxEnumWarnings)
                add(r, Issue::Level::Warning, "Kein Enum in " + opt.enums->sourcePath, n);
        }
        if (unknown > opt.maxEnumWarnings)
            add(r, Issue::Level::Warning,
                std::to_string(unknown - opt.maxEnumWarnings) + " weitere Sequenzen ohne Enum");

        // Gegenrichtung: Enums, fuer die keine Sequenz gebaut wird. Assimilate
        // prueft das nicht. Im Spiel aeussert sich so ein Fall als Figur, die
        // eine Animation nicht abspielt, ohne jede Fehlermeldung.
        const std::set<std::string> used(allNames.begin(), allNames.end());
        std::size_t noSeq = 0;
        for (const auto& e : opt.enums->names) {
            if (e.rfind("MAX_", 0) == 0) continue;   // Zaehlmarken
            if (!used.count(e)) ++noSeq;
        }
        if (noSeq)
            add(r, Issue::Level::Info,
                std::to_string(noSeq) + " Enums der Tabelle haben keine Sequenz");
    }

    // --- 4. Loopframe innerhalb der Sequenz -------------------------------
    for (const auto& g : script.grabs) {
        const std::string name = g.enumName ? *g.enumName : g.derivedName();
        for (const auto& a : g.additional) {
            if (a.frameCount <= 0) {
                add(r, Issue::Level::Error, "Framezahl ist " + std::to_string(a.frameCount),
                    a.name, g.line);
                continue;
            }
            if (a.loopFrame >= a.frameCount)
                add(r, Issue::Level::Error,
                    "Loopframe " + std::to_string(a.loopFrame) + " liegt ausserhalb von 0.." +
                        std::to_string(a.frameCount - 1),
                    a.name, g.line);
            if (a.targetOffset < 0)
                add(r, Issue::Level::Error,
                    "Negativer Versatz " + std::to_string(a.targetOffset), a.name, g.line);
        }
    }

    // --- 5. -makeskel zusammen mit einer GLA-Sequenz -----------------------
    //
    // Assimilate: "Model has both a GLA sequence and a '-makeskel' path, this
    // is meaningless". Entweder das Skelett kommt aus einer vorhandenen GLA
    // oder es wird erzeugt — beides zugleich ergibt keinen Sinn.
    {
        bool hasGlaSeq = false;
        for (const auto& st : script.statements)
            if (st.cmd == Cmd::AseAnimGrabGla || st.cmd == Cmd::AseAnimRefGla) hasGlaSeq = true;
        if (hasGlaSeq && script.convert && !script.convert->makeSkel.empty())
            add(r, Issue::Level::Error,
                "-makeskel zusammen mit einer GLA-Sequenz ist widerspruechlich");
    }

    // --- 6. Framezahlen gegen die Quelldateien ----------------------------
    //
    // Nur auf Wunsch: dafuer muss jede .xsi gelesen werden. Geprueft wird, ob
    // die -additional-Bereiche in die tatsaechliche Laenge passen.
    if (opt.readFrameCounts) {
        std::mutex m;
        parallelFor(
            script.grabs.size(),
            [&](std::size_t i) {
                if (resolved[i].empty()) return;
                if (script.grabs[i].additional.empty()) return;
                int frames = 0;
                try {
                    AnimCache cache(opt.cacheDir);
                    const xsi::AnimFile a = cache.enabled()
                                                ? cache.loadOrParse(resolved[i])
                                                : xsi::loadAnimationFile(resolved[i]);
                    frames = a.frameCount();
                } catch (const std::exception&) {
                    return;
                }
                for (const auto& ad : script.grabs[i].additional) {
                    if (ad.targetOffset + ad.frameCount > frames) {
                        std::lock_guard<std::mutex> lock(m);
                        add(r, Issue::Level::Error,
                            "Bereich " + std::to_string(ad.targetOffset) + ".." +
                                std::to_string(ad.targetOffset + ad.frameCount - 1) +
                                " liegt ausserhalb der " + std::to_string(frames) +
                                " Frames von \"" + script.grabs[i].file + "\"",
                            ad.name, script.grabs[i].line);
                    }
                }
            },
            opt.threads);
    }

    return r;
}

void addGrabFrame(Script& s) {
    const auto has = [&](Cmd c) {
        for (const auto& st : s.statements)
            if (st.cmd == c) return true;
        return false;
    };
    if (!has(Cmd::AseAnimGrabInit)) {
        Statement init;
        init.cmd = Cmd::AseAnimGrabInit;
        init.raw = "$aseanimgrabinit";
        s.statements.push_back(std::move(init));
    }
    if (!has(Cmd::AseAnimGrabFinalize)) {
        Statement fin;
        fin.cmd = Cmd::AseAnimGrabFinalize;
        fin.raw = "$aseanimgrabfinalize";
        s.statements.push_back(std::move(fin));
    }
}

namespace {

constexpr const char* kNl = "\r\n";

void writeComments(std::ostringstream& os, const std::vector<std::string>& cs) {
    for (const auto& c : cs) {
        if (c.empty()) os << kNl;
        else if (c.size() >= 2 && c[0] == '/' && c[1] == '/') os << c << kNl;
        else os << "// " << c << kNl;
    }
}

// Argumente mit Leerzeichen in Anfuehrungszeichen. Der Leser haelt sie
// damit zusammen; ohne wuerde "models/my anims/walk.xsi" beim naechsten
// Einlesen zu "models/my" und einem unbekannten Flag.
std::string quoted(const std::string& a) {
    if (a.empty() || a.find_first_of(" \t") != std::string::npos) return "\"" + a + "\"";
    return a;
}

std::string commentSuffix(const std::string& c) {
    if (c.empty()) return {};
    return (c.size() >= 2 && c[0] == '/' && c[1] == '/') ? "  " + c : "  // " + c;
}

bool sameAdditional(const GrabDirective::Additional& a, const GrabDirective::Additional& b) {
    return a.targetOffset == b.targetOffset && a.frameCount == b.frameCount &&
           a.loopFrame == b.loopFrame && a.frameSpeed == b.frameSpeed && a.name == b.name &&
           a.insideQdSkip == b.insideQdSkip;
}

bool sameGrab(const GrabDirective& a, const GrabDirective& b) {
    if (a.file != b.file || a.loop != b.loop || a.frameSpeed != b.frameSpeed ||
        a.enumName != b.enumName || a.hasQdSkip != b.hasQdSkip ||
        a.extraArgs != b.extraArgs || a.trailingComment != b.trailingComment ||
        a.additional.size() != b.additional.size())
        return false;
    for (std::size_t i = 0; i < a.additional.size(); ++i)
        if (!sameAdditional(a.additional[i], b.additional[i])) return false;
    return true;
}

std::string grabText(const GrabDirective& g) {
    // Unveraendert? Dann die Zeile genau so, wie sie in der Datei stand.
    if (!g.sourceLine.empty() && sameGrab(parseGrabLine(g.sourceLine), g)) return g.sourceLine;

    std::ostringstream os;
    os << "$aseanimgrab " << quoted(g.file);
    if (g.loop) os << " -loop " << *g.loop;
    if (g.frameSpeed) os << " -framespeed " << *g.frameSpeed;
    if (g.enumName) os << " -enum " << quoted(*g.enumName);

    // Die qdskip-Klammer genau um die Eintraege legen, die in ihr standen.
    // Frueher wurde sie erst hinter dem letzten -additional geschlossen; ein
    // Eintrag, der ausserhalb stand, rutschte beim naechsten Einlesen hinein.
    bool qd = false;
    bool anyInside = false;
    for (const auto& a : g.additional) {
        if (a.insideQdSkip != qd) {
            os << (a.insideQdSkip ? " -qdskipstart" : " -qdskipstop");
            qd = a.insideQdSkip;
        }
        anyInside = anyInside || a.insideQdSkip;
        os << " -additional " << a.targetOffset << " " << a.frameCount << " " << a.loopFrame << " "
           << a.frameSpeed << " " << quoted(a.name);
    }
    if (qd) os << " -qdskipstop";
    else if (g.hasQdSkip && !anyInside) os << " -qdskipstart -qdskipstop";
    for (const auto& x : g.extraArgs) os << " " << quoted(x);
    os << commentSuffix(g.trailingComment);
    return os.str();
}

bool sameConvert(const ConvertDirective& a, const ConvertDirective& b) {
    return a.root == b.root && a.makeSkel == b.makeSkel && a.origin == b.origin &&
           a.noAsk == b.noAsk && a.makeSkin == b.makeSkin && a.extraArgs == b.extraArgs &&
           a.trailingComment == b.trailingComment;
}

std::string convertText(const ConvertDirective& c) {
    if (!c.sourceLine.empty() && sameConvert(parseConvertLine(c.sourceLine), c)) return c.sourceLine;

    std::ostringstream os;
    os << (c.noAsk ? "$aseanimconvertmdx_noask " : "$aseanimconvertmdx ") << quoted(c.root);
    if (c.makeSkin) os << " -makeskin";
    if (!c.makeSkel.empty()) os << " -makeskel " << quoted(c.makeSkel);
    if (c.origin)
        os << " -origin " << (*c.origin)[0] << " " << (*c.origin)[1] << " " << (*c.origin)[2];
    for (const auto& x : c.extraArgs) os << " " << quoted(x);
    os << commentSuffix(c.trailingComment);
    return os.str();
}

std::string statementText(const Statement& st) {
    if (!st.sourceLine.empty()) return st.sourceLine;
    std::string s = st.raw;
    for (const auto& a : st.args) s += " " + quoted(a);
    return s;
}

}  // namespace

std::string writeScript(const Script& s) {
    std::ostringstream os;

    // $include-Dateien bleiben $include-Zeilen. Ihre Grabs stehen zum Bauen
    // mit in s.grabs, gehoeren aber der anderen Datei — ins Hauptskript
    // kopiert, stuenden sie beim naechsten Bau doppelt da.
    std::set<int> includeWritten;
    const auto writeInclude = [&](int id) {
        if (!includeWritten.insert(id).second) return;
        for (const auto& st : s.statements)
            if (st.cmd == Cmd::Include && st.includeId == id) {
                writeComments(os, st.commentsBefore);
                os << statementText(st) << kNl;
            }
    };
    const auto includeHasGrabs = [&](int id) {
        for (const auto& g : s.grabs)
            if (g.fromInclude == id) return true;
        return false;
    };

    // $include-Zeilen ohne eingebundene Grabs (nicht verfolgt, oder die Datei
    // enthaelt keine), die in der Datei zwischen zwei Grabs standen: sie
    // gehoeren wieder an dieselbe Stelle der Grab-Liste, nicht hinter sie.
    std::size_t firstGrabStmt = s.statements.size(), lastGrabStmt = 0;
    for (std::size_t i = 0; i < s.statements.size(); ++i)
        if (s.statements[i].cmd == Cmd::AseAnimGrab && s.statements[i].fromInclude < 0) {
            firstGrabStmt = std::min(firstGrabStmt, i);
            lastGrabStmt = i;
        }
    std::vector<const Statement*> anchored;
    for (std::size_t i = 0; i < s.statements.size(); ++i) {
        const Statement& st = s.statements[i];
        if (st.cmd == Cmd::Include && st.fromInclude < 0 && !includeHasGrabs(st.includeId) &&
            i > firstGrabStmt && i < lastGrabStmt)
            anchored.push_back(&st);
    }
    const auto isAnchored = [&](const Statement& st) {
        return std::find(anchored.begin(), anchored.end(), &st) != anchored.end();
    };

    const auto writeGrabs = [&] {
        std::size_t own = 0;
        const auto flushAnchored = [&](bool all) {
            for (const Statement* st : anchored)
                if (all || st->grabsBefore <= own) writeInclude(st->includeId);
        };
        for (const auto& g : s.grabs) {
            if (g.fromInclude >= 0) {
                // An der Stelle des ersten eingebundenen Grabs steht das
                // $include — so bleibt die Reihenfolge der Sequenzen gleich.
                writeInclude(g.fromInclude);
                continue;
            }
            flushAnchored(false);
            writeComments(os, g.commentsBefore);
            os << grabText(g) << kNl;
            ++own;
        }
        flushAnchored(true);
        // Kommentare NACH dem letzten Grab.
        writeComments(os, s.trailingComments);
    };

    // Die Grabs stehen als Block dort, wo in der Datei der erste stand.
    //
    // In Ravens Skripten folgen auf $aseanimgrabinit erst $scale und
    // $keepmotion, dann die Grabs, danach die $pcj-Zeilen. Direkt hinter
    // $aseanimgrabinit geschrieben, waeren all diese Zeilen umgezogen.
    bool hasGrabStatement = false;
    for (const auto& st : s.statements)
        if (st.cmd == Cmd::AseAnimGrab && st.fromInclude < 0) hasGrabStatement = true;

    bool grabsWritten = false;
    bool convertWritten = false;
    for (const auto& st : s.statements) {
        if (st.fromInclude >= 0) continue;          // steht in der anderen Datei
        if (st.cmd == Cmd::AseAnimGrab) {           // kommt aus s.grabs
            if (!grabsWritten) {
                writeGrabs();
                grabsWritten = true;
            }
            continue;
        }
        if (st.cmd == Cmd::Include && (includeHasGrabs(st.includeId) || isAnchored(st))) {
            // Wird zwischen den Grabs geschrieben, wo es in der Datei stand.
            if (grabsWritten) writeInclude(st.includeId);
            continue;
        }

        writeComments(os, st.commentsBefore);
        if (st.cmd == Cmd::AseAnimConvertMdx || st.cmd == Cmd::AseAnimConvertMdxNoAsk) {
            // An ihrer alten Stelle, aber aus den aktuellen Daten.
            if (s.convert && !convertWritten) os << convertText(*s.convert) << kNl;
            convertWritten = true;
            continue;
        }
        os << statementText(st) << kNl;
        // Ohne Grab in der Datei (neues Skript): direkt hinter den Anfang.
        if (st.cmd == Cmd::AseAnimGrabInit && !grabsWritten && !hasGrabStatement) {
            writeGrabs();
            grabsWritten = true;
        }
    }

    // Kein $aseanimgrabinit im Skript: den Rahmen selbst setzen, statt die
    // Grabs stillschweigend zu verlieren. Frueher war die Datei danach leer.
    if (!grabsWritten && !s.grabs.empty()) {
        os << "$aseanimgrabinit" << kNl;
        writeGrabs();
        os << "$aseanimgrabfinalize" << kNl;
    }
    // Noch keine Stelle dafuer, etwa bei einem neu angelegten Skript: ans Ende.
    // Stammt sie aus einer $include-Datei, steht sie dort schon.
    if (s.convert && !convertWritten && s.convert->fromInclude < 0)
        os << convertText(*s.convert) << kNl;

    writeComments(os, s.endComments);
    return os.str();
}

std::vector<FoundCar> scanDirectory(const std::string& root, std::size_t maxDepth) {
    std::vector<FoundCar> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;

    // Eigener Stapel statt recursive_directory_iterator.
    //
    // Dessen increment(ec) rueckt bei einem Fehler NICHT zuverlaessig vor —
    // die Norm laesst das offen, und sowohl Boost als auch die
    // Microsoft-Implementierung bleiben in dem Fall stehen. Eine Schleife
    // ueber den Iterator laeuft dann endlos, ohne Fehlermeldung, ohne
    // Absturz: sie haengt einfach.
    //
    // Dazu kommen Verzeichnisverknuepfungen, die auf einen Vorfahren zeigen.
    // Unter Windows sind Junctions in Benutzerprofilen ueblich, und der
    // Iterator folgt ihnen standardmaessig — auch das ergibt eine
    // Endlosschleife.
    //
    // Mit eigenem Stapel ist beides ausgeschlossen: jedes Verzeichnis wird
    // einmal geoeffnet, die Tiefe ist begrenzt, bereits besuchte Pfade werden
    // an ihrer kanonischen Form erkannt, und Verknuepfungen werden gar nicht
    // erst verfolgt.
    struct Pending {
        fs::path    dir;
        std::size_t depth;
    };
    std::vector<Pending> stack{{fs::path(root), 0}};
    std::set<std::string> visited;

    // Letzte Sicherung. Wenn trotz allem etwas im Kreis laeuft, bricht die
    // Suche ab und meldet es, statt das Programm haengen zu lassen.
    constexpr std::size_t kMaxEntries = 2'000'000;
    std::size_t seen = 0;

    while (!stack.empty()) {
        const Pending cur = stack.back();
        stack.pop_back();

        // Kanonisch aufloesen, damit derselbe Ordner ueber verschiedene Wege
        // nur einmal betrachtet wird.
        std::error_code cec;
        const fs::path canon = fs::weakly_canonical(cur.dir, cec);
        const std::string key = (cec ? cur.dir : canon).lexically_normal().string();
        if (!visited.insert(key).second) continue;

        fs::directory_iterator it(cur.dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;   // unlesbares Verzeichnis ueberspringen, nicht abbrechen

        for (const auto& entry : it) {
            if (++seen > kMaxEntries) {
                stack.clear();
                break;
            }

            std::error_code eec;
            if (entry.is_symlink(eec)) continue;   // Verknuepfungen nicht verfolgen

            if (entry.is_directory(eec) && !eec) {
                if (cur.depth < maxDepth) stack.push_back({entry.path(), cur.depth + 1});
                continue;
            }
            if (!entry.is_regular_file(eec) || eec) continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext != ".car") continue;

            FoundCar f;
            f.path = entry.path().lexically_normal().make_preferred().string();
            try {
                ParseOptions po;
                po.followIncludes = false;
                const Script sc = parseFile(f.path, po);
                f.grabs = sc.grabs.size();
                if (sc.convert) f.modelName = sc.convert->makeSkel;
            } catch (const std::exception&) {
                // Unlesbare Dateien trotzdem auflisten — der Nutzer soll
                // sehen, dass sie da sind.
            }
            out.push_back(std::move(f));
        }
    }

    std::sort(out.begin(), out.end(),
              [](const FoundCar& a, const FoundCar& b) { return a.path < b.path; });
    return out;
}

}  // namespace g2::car
