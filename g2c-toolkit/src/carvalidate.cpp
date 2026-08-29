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
            if (unknown <= 20)
                add(r, Issue::Level::Warning, "Kein Enum in " + opt.enums->sourcePath, n);
        }
        if (unknown > 20)
            add(r, Issue::Level::Warning,
                std::to_string(unknown - 20) + " weitere Sequenzen ohne Enum");

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

std::string writeScript(const Script& s) {
    std::ostringstream os;
    const char* nl = "\r\n";

    for (const auto& st : s.statements) {
        switch (st.cmd) {
            case Cmd::AseAnimGrab:
            case Cmd::AseAnimConvertMdx:
            case Cmd::AseAnimConvertMdxNoAsk:
                // Diese werden unten aus den strukturierten Daten erzeugt,
                // damit Aenderungen daran auch ankommen.
                continue;
            default:
                break;
        }
        os << st.raw;
        for (const auto& a : st.args) os << " " << a;
        os << nl;
        if (st.cmd == Cmd::AseAnimGrabInit) {
            // Direkt nach $aseanimgrabinit folgen die Grabs.
            for (const auto& g : s.grabs) {
                os << "$aseanimgrab " << g.file;
                if (g.loop) os << " -loop " << *g.loop;
                if (g.frameSpeed) os << " -framespeed " << *g.frameSpeed;
                if (g.enumName) os << " -enum " << *g.enumName;
                bool qd = false;
                for (const auto& a : g.additional) {
                    if (a.insideQdSkip && !qd) { os << " -qdskipstart"; qd = true; }
                    os << " -additional " << a.targetOffset << " " << a.frameCount << " "
                       << a.loopFrame << " " << a.frameSpeed << " " << a.name;
                }
                if (qd) os << " -qdskipstop";
                else if (g.hasQdSkip) os << " -qdskipstart -qdskipstop";
                os << nl;
            }
        }
    }

    if (s.convert) {
        os << (s.convert->noAsk ? "$aseanimconvertmdx_noask " : "$aseanimconvertmdx ")
           << s.convert->root;
        if (!s.convert->makeSkel.empty()) os << " -makeskel " << s.convert->makeSkel;
        if (s.convert->origin)
            os << " -origin " << (*s.convert->origin)[0] << " " << (*s.convert->origin)[1] << " "
               << (*s.convert->origin)[2];
        os << nl;
    }
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
