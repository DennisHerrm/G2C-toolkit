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

    // --- 0a. Size of animation.cfg ----------------------------------------
    //
    // The engine has a FIXED buffer and aborts:
    //
    //     UI_ParseAnimationFile: File ... too long (172308 > 159999)
    //
    // This only shows up when the game starts, and the message names no
    // sequence - you then search a 2463-line file for something that isn't
    // a single entry at all.
    //
    // The estimate is generous: the name plus four numbers plus separators.
    {
        std::size_t bytes = 200;   // header lines
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

    // --- 0. Duplicate sequence names --------------------------------------
    //
    // The engine looks entries up in animation.cfg by NAME. If one appears
    // twice, the last entry wins - the first animation then becomes
    // unreachable, even though its frames are in the GLA and take up space.
    //
    // In the game this shows up as "the animation does nothing", and nobody
    // looks for the cause in the cfg. It happens easily when merging several
    // sources.
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

    // --- 1. Source files present ------------------------------------------
    // Missing files are reported in summary. With a wrong -basedir all 1289
    // are missing at once, and 1289 identical lines crowd out every other
    // message - even though the real message then is "the path is wrong",
    // not "this file is missing".
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

    // --- 2. Sequence names -------------------------------------------------
    //
    // A name may occur only once. Assimilate reports this as
    // "animation enum %s is used %d times"; as a result the later sequence
    // overwrites the earlier one in animation.cfg.
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

    // --- 3. Names against the enum table ----------------------------------
    //
    // Unknown names are a WARNING, not an error. A mod extends the table
    // continuously; Assimilate's hard rejection would block that work. For
    // Movie Duels this affects 165 sequences.
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

        // Reverse direction: enums for which no sequence is built. Assimilate
        // doesn't check this. In the game such a case shows up as a character
        // that doesn't play an animation, without any error message.
        const std::set<std::string> used(allNames.begin(), allNames.end());
        std::size_t noSeq = 0;
        for (const auto& e : opt.enums->names) {
            if (e.rfind("MAX_", 0) == 0) continue;   // count markers
            if (!used.count(e)) ++noSeq;
        }
        if (noSeq)
            add(r, Issue::Level::Info,
                std::to_string(noSeq) + " Enums der Tabelle haben keine Sequenz");
    }

    // --- 4. Loop frame within the sequence -------------------------------
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

    // --- 5. -makeskel together with a GLA sequence ------------------------
    //
    // Assimilate: "Model has both a GLA sequence and a '-makeskel' path, this
    // is meaningless". Either the skeleton comes from an existing GLA or it is
    // generated - both at once makes no sense.
    {
        bool hasGlaSeq = false;
        for (const auto& st : script.statements)
            if (st.cmd == Cmd::AseAnimGrabGla || st.cmd == Cmd::AseAnimRefGla) hasGlaSeq = true;
        if (hasGlaSeq && script.convert && !script.convert->makeSkel.empty())
            add(r, Issue::Level::Error,
                "-makeskel zusammen mit einer GLA-Sequenz ist widerspruechlich");
    }

    // --- 6. Frame counts against the source files -------------------------
    //
    // Only on request: this requires reading every .xsi. It checks whether
    // the -additional ranges fit within the actual length.
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

// Arguments containing spaces go in quotes. That way the reader keeps them
// together; without quotes, "models/my anims/walk.xsi" would become
// "models/my" plus an unknown flag on the next read.
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
    // Unchanged? Then return the line exactly as it was in the file.
    if (!g.sourceLine.empty() && sameGrab(parseGrabLine(g.sourceLine), g)) return g.sourceLine;

    std::ostringstream os;
    os << "$aseanimgrab " << quoted(g.file);
    if (g.loop) os << " -loop " << *g.loop;
    if (g.frameSpeed) os << " -framespeed " << *g.frameSpeed;
    if (g.enumName) os << " -enum " << quoted(*g.enumName);

    // Put the qdskip bracket exactly around the entries that were inside it.
    // Previously it was only closed after the last -additional; an entry that
    // was outside slipped into it on the next read.
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

    // $include files stay $include lines. Their grabs are in s.grabs for the
    // build, but belong to the other file - copied into the main script, they
    // would appear twice on the next build.
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

    // $include lines without included grabs (not followed, or the file
    // contains none) that stood between two grabs in the file: they belong
    // back at the same position in the grab list, not after it.
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
                // The $include goes where the first included grab is - that
                // way the order of the sequences stays the same.
                writeInclude(g.fromInclude);
                continue;
            }
            flushAnchored(false);
            writeComments(os, g.commentsBefore);
            os << grabText(g) << kNl;
            ++own;
        }
        flushAnchored(true);
        // Comments AFTER the last grab.
        writeComments(os, s.trailingComments);
    };

    // The grabs are written as a block where the first one was in the file.
    //
    // In Raven's scripts $aseanimgrabinit is followed first by $scale and
    // $keepmotion, then the grabs, then the $pcj lines. Written directly after
    // $aseanimgrabinit, all of these lines would have moved.
    bool hasGrabStatement = false;
    for (const auto& st : s.statements)
        if (st.cmd == Cmd::AseAnimGrab && st.fromInclude < 0) hasGrabStatement = true;

    bool grabsWritten = false;
    bool convertWritten = false;
    for (const auto& st : s.statements) {
        if (st.fromInclude >= 0) continue;          // lives in the other file
        if (st.cmd == Cmd::AseAnimGrab) {           // comes from s.grabs
            if (!grabsWritten) {
                writeGrabs();
                grabsWritten = true;
            }
            continue;
        }
        if (st.cmd == Cmd::Include && (includeHasGrabs(st.includeId) || isAnchored(st))) {
            // Written between the grabs, where it was in the file.
            if (grabsWritten) writeInclude(st.includeId);
            continue;
        }

        writeComments(os, st.commentsBefore);
        if (st.cmd == Cmd::AseAnimConvertMdx || st.cmd == Cmd::AseAnimConvertMdxNoAsk) {
            // At its old position, but from the current data.
            if (s.convert && !convertWritten) os << convertText(*s.convert) << kNl;
            convertWritten = true;
            continue;
        }
        os << statementText(st) << kNl;
        // No grab in the file (new script): right after the start.
        if (st.cmd == Cmd::AseAnimGrabInit && !grabsWritten && !hasGrabStatement) {
            writeGrabs();
            grabsWritten = true;
        }
    }

    // No $aseanimgrabinit in the script: add the frame ourselves instead of
    // silently losing the grabs. Previously the file was empty afterwards.
    if (!grabsWritten && !s.grabs.empty()) {
        os << "$aseanimgrabinit" << kNl;
        writeGrabs();
        os << "$aseanimgrabfinalize" << kNl;
    }
    // No position for it yet, e.g. in a newly created script: at the end.
    // If it comes from an $include file, it is already there.
    if (s.convert && !convertWritten && s.convert->fromInclude < 0)
        os << convertText(*s.convert) << kNl;

    writeComments(os, s.endComments);
    return os.str();
}

std::vector<FoundCar> scanDirectory(const std::string& root, std::size_t maxDepth) {
    std::vector<FoundCar> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;

    // Our own stack instead of recursive_directory_iterator.
    //
    // Its increment(ec) does NOT reliably advance on an error - the standard
    // leaves this open, and both Boost and the Microsoft implementation stay
    // put in that case. A loop over the iterator then runs forever, with no
    // error message and no crash: it simply hangs.
    //
    // On top of that there are directory links pointing to an ancestor. On
    // Windows, junctions in user profiles are common, and the iterator
    // follows them by default - which also results in an endless loop.
    //
    // With our own stack both are ruled out: every directory is opened once,
    // the depth is limited, already visited paths are recognized by their
    // canonical form, and links are not followed at all.
    struct Pending {
        fs::path    dir;
        std::size_t depth;
    };
    std::vector<Pending> stack{{fs::path(root), 0}};
    std::set<std::string> visited;

    // Last safeguard. If something still runs in circles despite all this,
    // the search aborts and reports it instead of letting the program hang.
    constexpr std::size_t kMaxEntries = 2'000'000;
    std::size_t seen = 0;

    while (!stack.empty()) {
        const Pending cur = stack.back();
        stack.pop_back();

        // Resolve canonically so the same folder reached via different paths
        // is only considered once.
        std::error_code cec;
        const fs::path canon = fs::weakly_canonical(cur.dir, cec);
        const std::string key = (cec ? cur.dir : canon).lexically_normal().string();
        if (!visited.insert(key).second) continue;

        fs::directory_iterator it(cur.dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;   // skip unreadable directory, don't abort

        for (const auto& entry : it) {
            if (++seen > kMaxEntries) {
                stack.clear();
                break;
            }

            std::error_code eec;
            if (entry.is_symlink(eec)) continue;   // don't follow links

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
                // List unreadable files anyway - the user should see that
                // they exist.
            }
            out.push_back(std::move(f));
        }
    }

    std::sort(out.begin(), out.end(),
              [](const FoundCar& a, const FoundCar& b) { return a.path < b.path; });
    return out;
}

}  // namespace g2::car
