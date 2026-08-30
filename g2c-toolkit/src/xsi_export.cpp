#include "g2/xsi_export.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <functional>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <filesystem>

namespace g2::xsiexp {
namespace {
namespace fs = std::filesystem;
}
namespace {

constexpr double kRad2Deg = 57.29577951308232;

// Achsentausch zwischen dotXSI und GLA, wie in xsi_anim.cpp.
Mat3x4 axisC() {
    Mat3x4 c{};
    c.m[0][0] = 1;
    c.m[1][2] = -1;
    c.m[2][1] = 1;
    return c;
}

Mat3x4 axisCinv() {
    Mat3x4 c{};
    c.m[0][0] = 1;
    c.m[1][2] = 1;
    c.m[2][1] = -1;
    return c;
}

// Baut die Rotation genauso zusammen wie der Importeur.
//
// Muss Zeichen fuer Zeichen zu eulerXYZ in xsi_anim.cpp passen — sonst
// misst die Guetepruefung unten etwas anderes als das, was spaeter
// tatsaechlich herauskommt.
Mat3x4 composeEuler(float rxDeg, float ryDeg, float rzDeg) {
    constexpr double kDeg2Rad = 0.017453292519943295;
    const double rx = rxDeg * kDeg2Rad, ry = ryDeg * kDeg2Rad, rz = rzDeg * kDeg2Rad;
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);

    Mat3x4 o{};
    o.m[0][0] = static_cast<float>(cz * cy);
    o.m[0][1] = static_cast<float>(cz * sy * sx - sz * cx);
    o.m[0][2] = static_cast<float>(cz * sy * cx + sz * sx);
    o.m[1][0] = static_cast<float>(sz * cy);
    o.m[1][1] = static_cast<float>(sz * sy * sx + cz * cx);
    o.m[1][2] = static_cast<float>(sz * sy * cx - cz * sx);
    o.m[2][0] = static_cast<float>(-sy);
    o.m[2][1] = static_cast<float>(cy * sx);
    o.m[2][2] = static_cast<float>(cy * cx);
    return o;
}

double rotError(const Mat3x4& a, const Mat3x4& b) {
    double e = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            e = std::max(e, std::fabs(static_cast<double>(a.m[r][c]) - b.m[r][c]));
    return e;
}

// Zerlegt die Rotation aus R = Rz(rz)·Ry(ry)·Rx(rx) — genau die Reihenfolge,
// die eulerXYZ in xsi_anim.cpp aufbaut. Wird hier eine andere Reihenfolge
// angenommen, sieht die Datei richtig aus und die Animation ist verdreht.
//
// Warum das Ergebnis geprueft und notfalls ersetzt wird:
//
// Nahe Gimbal Lock — wenn cos(ry) gegen null geht — sind rx und rz nicht
// mehr sauber trennbar. Die uebliche Formel liefert dort zwar Winkel, aber
// nach dem Zusammensetzen weichen die Matrixelemente um bis zu 0,003 ab.
// Das trifft nur 0,06 % zufaelliger Rotationen, faellt aber massiv ins
// Gewicht, sobald ein Bone weit von seinem Elternbone entfernt ist: in JK2s
// _humanoid.gla haengen 46 der 72 Bones direkt am Brustkorb, und dort wird
// aus 0,003 im Winkel ueber einen Meter Hebel ein sichtbarer Versatz.
//
// Deshalb werden beide gueltigen Loesungen durchgerechnet — die uebliche und
// die entartete mit rz = 0 — und die genommen, die sich besser
// zurueckrechnen laesst. Das kann nie schlechter sein als eine feste Wahl.
void decomposeEuler(const Mat3x4& m, float& rxDeg, float& ryDeg, float& rzDeg) {
    const double sy = -static_cast<double>(m.m[2][0]);
    const double clamped = sy > 1.0 ? 1.0 : (sy < -1.0 ? -1.0 : sy);
    const double ry = std::asin(clamped);

    struct Cand {
        float rx, ry, rz;
    };
    Cand best{};
    double bestErr = 1e30;

    const auto tryCand = [&](double rxr, double ryr, double rzr) {
        const Cand c{static_cast<float>(rxr * kRad2Deg), static_cast<float>(ryr * kRad2Deg),
                     static_cast<float>(rzr * kRad2Deg)};
        const double e = rotError(m, composeEuler(c.rx, c.ry, c.rz));
        if (e < bestErr) {
            bestErr = e;
            best = c;
        }
    };

    // Die uebliche Loesung.
    tryCand(std::atan2(static_cast<double>(m.m[2][1]), static_cast<double>(m.m[2][2])), ry,
            std::atan2(static_cast<double>(m.m[1][0]), static_cast<double>(m.m[0][0])));

    // Die entartete: rz auf null, alles in rx. Bei echtem Gimbal Lock ist
    // sie exakt, sonst schlechter — die Pruefung entscheidet.
    tryCand(std::atan2(-static_cast<double>(m.m[1][2]), static_cast<double>(m.m[1][1])), ry, 0.0);

    // Die zweite Loesung des Arkussinus: ry gespiegelt an pi/2.
    constexpr double kPi = 3.141592653589793;
    const double ry2 = (ry >= 0.0 ? kPi - ry : -kPi - ry);
    tryCand(std::atan2(-static_cast<double>(m.m[2][1]), -static_cast<double>(m.m[2][2])), ry2,
            std::atan2(-static_cast<double>(m.m[1][0]), -static_cast<double>(m.m[0][0])));

    rxDeg = best.rx;
    ryDeg = best.ry;
    rzDeg = best.rz;
}

// Naechstgelegene Rotation zu einer 3x3-Matrix (Polarzerlegung).
//
// Iteration R <- (R + R^-T)/2. Konvergiert quadratisch; sechs Schritte
// genuegen fuer float weit ueber die noetige Genauigkeit hinaus.
Mat3x4 nearestRotation(const Mat3x4& m) {
    double R[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r][c] = m.m[r][c];

    for (int it = 0; it < 8; ++it) {
        // Inverse Transponierte bilden.
        const double det =
            R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
            R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
            R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
        if (std::fabs(det) < 1e-12) break;

        double N[3][3];
        N[0][0] = (R[1][1] * R[2][2] - R[1][2] * R[2][1]) / det;
        N[0][1] = (R[1][2] * R[2][0] - R[1][0] * R[2][2]) / det;
        N[0][2] = (R[1][0] * R[2][1] - R[1][1] * R[2][0]) / det;
        N[1][0] = (R[0][2] * R[2][1] - R[0][1] * R[2][2]) / det;
        N[1][1] = (R[0][0] * R[2][2] - R[0][2] * R[2][0]) / det;
        N[1][2] = (R[0][1] * R[2][0] - R[0][0] * R[2][1]) / det;
        N[2][0] = (R[0][1] * R[1][2] - R[0][2] * R[1][1]) / det;
        N[2][1] = (R[0][2] * R[1][0] - R[0][0] * R[1][2]) / det;
        N[2][2] = (R[0][0] * R[1][1] - R[0][1] * R[1][0]) / det;

        double diff = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const double next = 0.5 * (R[r][c] + N[r][c]);
                diff = std::max(diff, std::fabs(next - R[r][c]));
                R[r][c] = next;
            }
        if (diff < 1e-12) break;
    }

    Mat3x4 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.m[r][c] = static_cast<float>(R[r][c]);
    return o;
}

Mat3x4 scaled(const Mat3x4& m, float s) {
    Mat3x4 o = m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] *= s;
    return o;
}

std::string num(double v) {
    // Sechs Nachkommastellen wie in Ravens Dateien. Weniger verliert
    // sichtbar Genauigkeit, mehr blaeht die Datei ohne Nutzen auf.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

}  // namespace

std::optional<std::array<float, 3>> readAverageVec(const std::string& framesPath,
                                                   const std::string& sequenceOrFile) {
    std::ifstream f(framesPath);
    if (!f) return std::nullopt;

    // Vergleich ueber den Dateinamen ohne Endung, kleingeschrieben. Die
    // .frames nennt vollstaendige Pfade mit Laufwerksbuchstabe; die passen
    // auf keinem anderen Rechner.
    auto key = [](std::string v) {
        const std::size_t slash = v.find_last_of("/\\");
        if (slash != std::string::npos) v = v.substr(slash + 1);
        const std::size_t dot = v.find_last_of('.');
        if (dot != std::string::npos) v = v.substr(0, dot);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = key(sequenceOrFile);

    std::string line, current;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t q = line.find('"');
        if (q == std::string::npos) {
            if (line.find('{') == std::string::npos && line.find('}') == std::string::npos &&
                !line.empty())
                current = key(line);
            continue;
        }
        if (current != want) continue;
        if (line.find("averagevec") == std::string::npos) continue;

        // Zweites Anfuehrungszeichenpaar der Zeile: "averagevec" "x y z".
        //
        // Nicht ab einem festen Versatz suchen — der landet genau auf dem
        // schliessenden Anfuehrungszeichen des Schluessels, und gelesen wird
        // dann der Tabulator dazwischen.
        std::size_t p1 = line.find('"');
        std::size_t p2 = line.find('"', p1 + 1);
        std::size_t p3 = line.find('"', p2 + 1);
        std::size_t p4 = line.find('"', p3 + 1);
        if (p3 == std::string::npos || p4 == std::string::npos) continue;
        std::istringstream is(line.substr(p3 + 1, p4 - p3 - 1));
        std::array<float, 3> v{};
        if (!(is >> v[0] >> v[1] >> v[2])) continue;
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) return std::nullopt;
        return v;
    }
    return std::nullopt;
}

std::optional<std::array<float, 3>> detectOrigin(const MdxaFile& gla) {
    if (gla.numFrames <= 0 || gla.skeleton.bones.empty()) return std::nullopt;

    // Haeufigsten Wert je Achse suchen. Der Versatz ist konstant, die
    // Wurzelbewegung nicht — deshalb ist der haeufigste Wert der Versatz.
    std::array<float, 3> best{};
    for (int axis = 0; axis < 3; ++axis) {
        std::map<int, int> hist;   // gerundet auf Viertel-Einheiten
        const int step = gla.numFrames > 4000 ? gla.numFrames / 2000 : 1;
        int total = 0;
        for (int f = 0; f < gla.numFrames; f += step) {
            const float v = gla.boneMatrix(f, 0).m[axis][3];
            hist[static_cast<int>(std::lround(v * 4.0f))]++;
            ++total;
        }
        int bestKey = 0, bestCount = 0;
        for (const auto& [k, c] : hist)
            if (c > bestCount) { bestCount = c; bestKey = k; }

        // Nur uebernehmen, wenn der Wert wirklich vorherrscht.
        best[static_cast<std::size_t>(axis)] =
            (bestCount * 10 >= total * 8) ? -static_cast<float>(bestKey) / 4.0f : 0.0f;
    }
    if (best[0] == 0.0f && best[1] == 0.0f && best[2] == 0.0f) return std::nullopt;
    return best;
}

float residualMotion(const MdxaFile& gla, const Sequence& seq) {
    const auto& sk = gla.skeleton;
    const int n = static_cast<int>(sk.bones.size());
    if (n == 0 || seq.frameCount < 2) return 0.0f;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames) return 0.0f;

    // Gross- und Kleinschreibung ignorieren: eigene Modelle schreiben den
    // Bewegungsbone oft "motion" statt "Motion".
    int mi = -1;
    for (int b = 0; b < n; ++b) {
        std::string nm = sk.bones[static_cast<std::size_t>(b)].name;
        std::transform(nm.begin(), nm.end(), nm.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (nm == "motion") mi = b;
    }
    if (mi < 0) return 0.0f;

    const auto a = boneWorldMatrices(gla, seq.startFrame);
    const auto b = boneWorldMatrices(gla, seq.startFrame + seq.frameCount - 1);
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r)
        worst = std::max(worst, std::fabs(b[static_cast<std::size_t>(mi)].m[r][3] -
                                          a[static_cast<std::size_t>(mi)].m[r][3]));
    return worst;
}

Grouping groupSequences(const std::vector<CfgSequence>& cfg) {
    Grouping out;
    if (cfg.empty()) return out;

    std::vector<CfgSequence> sorted = cfg;
    std::sort(sorted.begin(), sorted.end(), [](const CfgSequence& a, const CfgSequence& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.count > b.count;   // der laengste Bereich wird Master
    });

    int end = -1;
    for (const auto& q : sorted) {
        if (q.start >= end) {
            out.masters.push_back({q, {}});
            end = q.start + q.count;
        } else if (q.start + q.count <= end) {
            out.masters.back().inside.push_back(q);
        } else {
            // Teilweise ueberlappend liesse sich nicht als -additional
            // ausdruecken. In Ravens Dateien kommt das nicht vor; wenn doch,
            // lieber melden als still verbiegen.
            ++out.partial;
        }
    }
    return out;
}

std::optional<std::array<float, 3>> detectRootMotion(const MdxaFile& gla, const Sequence& seq) {
    if (seq.frameCount < 2) return std::nullopt;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames) return std::nullopt;
    if (gla.skeleton.bones.empty()) return std::nullopt;

    const Mat3x4 a = gla.boneMatrix(seq.startFrame, 0);
    const Mat3x4 b = gla.boneMatrix(seq.startFrame + seq.frameCount - 1, 0);
    const float steps = static_cast<float>(seq.frameCount - 1);

    std::array<float, 3> v{};
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        v[static_cast<std::size_t>(k)] = -(b.m[k][3] - a.m[k][3]) / steps;
        if (std::fabs(v[static_cast<std::size_t>(k)]) > 1e-4f) any = true;
    }
    return any ? std::optional<std::array<float, 3>>(v) : std::nullopt;
}

car::Script buildScript(const Grouping& g, const std::string& xsiPrefix,
                        const std::optional<std::array<float, 3>>& origin, float scale,
                        bool keepMotion, const std::string& makeSkel) {
    car::Script sc;
    car::addGrabFrame(sc);

    // $scale und $keepmotion stehen in Ravens Skripten und gehoeren hier
    // ebenfalls hinein.
    //
    // NICHT rekonstruierbar ist dagegen $pcj — die Liste der Bones, die die
    // Engine zur Laufzeit selbst drehen darf. Sie steht nur in der .car und
    // hinterlaesst in der GLA keine Spur: in Ravens _humanoid.gla tragen nur
    // zwei von 53 Bones ueberhaupt ein Flag. Wer eine bestehende .car
    // ersetzt, sollte den $pcj-Block von dort uebernehmen.
    if (scale > 0.0f) {
        car::Statement st;
        st.cmd = car::Cmd::Scale;
        st.raw = "$scale";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(scale));
        st.args.push_back(buf);
        sc.statements.insert(sc.statements.begin(), std::move(st));
        sc.scale = scale;
    }
    if (keepMotion) {
        car::Statement st;
        st.cmd = car::Cmd::KeepMotion;
        st.raw = "$keepmotion";
        sc.statements.insert(sc.statements.begin() + (scale > 0.0f ? 1 : 0), std::move(st));
        sc.keepMotion = true;
    }

    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };

    for (const auto& m : g.masters) {
        car::GrabDirective gd;
        gd.file = xsiPrefix + lower(m.self.name) + ".xsi";
        gd.enumName = m.self.name;
        gd.loop = m.self.loop;
        gd.frameSpeed = m.self.fps;
        for (const auto& q : m.inside) {
            car::GrabDirective::Additional a;
            a.targetOffset = q.start - m.self.start;
            a.frameCount = q.count;
            a.loopFrame = q.loop;
            a.frameSpeed = q.fps;
            a.name = q.name;
            gd.additional.push_back(std::move(a));
        }
        sc.grabs.push_back(std::move(gd));
    }

    car::ConvertDirective cv;
    cv.noAsk = true;
    cv.root = xsiPrefix + "root";

    // Der Skelettpfad kommt aus dem Kopf der GLA — dort steht genau der
    // Wert, den Carcass seinerzeit als -makeskel bekommen hat. Nur wenn er
    // fehlt, wird er aus dem Praefix abgeleitet.
    //
    // Ohne Praefix darf dabei kein fuehrender Schraegstrich entstehen:
    // "/_humanoid" ist ein absoluter Pfad und wird nicht gefunden.
    if (!makeSkel.empty()) {
        cv.makeSkel = makeSkel;
    } else {
        const std::string dir = fs::path(xsiPrefix + "x").parent_path().generic_string();
        cv.makeSkel = dir.empty() ? "_humanoid" : dir + "/_humanoid";
    }

    if (origin) {
        // Negative Null vermeiden: aus der Schaetzung kommt -0.0, und
        // "-origin -0 -0 24" sieht nach einem Fehler aus, obwohl es keiner
        // ist.
        const auto clean = [](float v) { return v == 0.0f ? 0.0 : static_cast<double>(v); };
        cv.origin = std::array<double, 3>{clean((*origin)[0]), clean((*origin)[1]),
                                          clean((*origin)[2])};
    }
    sc.convert = cv;
    return sc;
}

std::string exportSequence(const MdxaFile& gla, const Sequence& seq,
                           const ExportOptions& opt) {
    const auto& skel = gla.skeleton;
    const int numBones = static_cast<int>(skel.bones.size());
    if (numBones == 0) throw std::runtime_error("GLA ohne Skelett");
    if (seq.frameCount <= 0) throw std::runtime_error("Sequenz ohne Frames");

    // Eine Ein-Frame-Sequenz wird als ZWEI identische Frames geschrieben.
    //
    // Carcass weist eine .xsi mit nur einem Frame ab:
    //
    //   XSI file-format doesn't support 1-frames files 100% legally,
    //   re-export this file please!
    //
    // Ravens eigene 58 Ein-Frame-Sequenzen sind denn auch alle
    // "-additional"-Unterbereiche laengerer Dateien; einzelne
    // Ein-Frame-Dateien gibt es dort nicht.
    //
    // Der zweite Frame ist eine Kopie des ersten, die Animation also
    // unveraendert. Wer das Skript ueber "Alles + .car" erzeugt, bekommt
    // ohnehin -additional und merkt davon nichts; wer eine einzelne Sequenz
    // exportiert, kann sie mit dieser Datei bauen statt gar nicht.
    const int outFrames = seq.frameCount == 1 ? 2 : seq.frameCount;
    if (seq.startFrame < 0 || seq.startFrame + seq.frameCount > gla.numFrames)
        throw std::runtime_error("Sequenzbereich liegt ausserhalb der GLA");

    const float scale = opt.scale > 0.0f ? opt.scale : 1.0f;
    const Mat3x4 C = axisC();
    const Mat3x4 Cinv = axisCinv();

    // Bindposen und ihre Inversen.
    std::vector<Mat3x4> B(static_cast<std::size_t>(numBones));
    std::vector<Mat3x4> Binv(static_cast<std::size_t>(numBones));
    for (int b = 0; b < numBones; ++b) {
        B[static_cast<std::size_t>(b)] = skel.bones[static_cast<std::size_t>(b)].basePose;
        Binv[static_cast<std::size_t>(b)] = affineInverse(B[static_cast<std::size_t>(b)]);
    }

    // Lokale Transformationen je Frame und Bone, in dotXSI-Konvention.
    struct Key {
        float t[3];
        float r[3];
        float s[3];
    };
    std::vector<std::vector<Key>> keys(static_cast<std::size_t>(numBones),
                                       std::vector<Key>(static_cast<std::size_t>(outFrames)));

    // Topologische Reihenfolge: Eltern vor Kindern.
    //
    // Die Indexreihenfolge reicht NICHT. In Ravens _humanoid.gla haben acht
    // Bones ihren Elternbone hinter sich — "ceyebrow", "jaw" und weitere
    // haengen an Bone 52. Rechnet man in Indexreihenfolge, ist X[parent] bei
    // diesen Bones noch uninitialisiert, und alles darunter wird Unsinn.
    std::vector<int> topo;
    topo.reserve(static_cast<std::size_t>(numBones));
    {
        std::vector<char> done(static_cast<std::size_t>(numBones), 0);
        bool progress = true;
        while (progress && topo.size() < static_cast<std::size_t>(numBones)) {
            progress = false;
            for (int b = 0; b < numBones; ++b) {
                const auto bu = static_cast<std::size_t>(b);
                if (done[bu]) continue;
                const int p = skel.bones[bu].parent;
                if (p >= 0 && !done[static_cast<std::size_t>(p)]) continue;
                topo.push_back(b);
                done[bu] = 1;
                progress = true;
            }
        }
        if (topo.size() != static_cast<std::size_t>(numBones))
            throw std::runtime_error("Skelett enthaelt einen Zyklus");
    }

    for (int f = 0; f < outFrames; ++f) {
        // Bei einer Ein-Frame-Sequenz zeigen beide Ausgabeframes auf
        // denselben Quellframe.
        const int src = seq.startFrame + std::min(f, seq.frameCount - 1);

        // Schritt 1: X aus A zurueckrechnen. Eltern zuerst, denn X(b) haengt
        // an X(parent).
        std::vector<Mat3x4> X(static_cast<std::size_t>(numBones));
        for (const int b : topo) {
            const auto bu = static_cast<std::size_t>(b);
            Mat3x4 A = gla.boneMatrix(src, b);
            const int p = skel.bones[bu].parent;

            if (p < 0) {
                // Rueckgaengig machen, was das Bauen dem Wurzelbone angetan
                // hat. Reihenfolge umgekehrt zur Vorwaertsrichtung:
                //   vorwaerts: A = X·B^-1;  A -= origin;  A += rampe·t
                //   rueckwaerts: A += origin;  A -= rampe·t;  X = A·B
                // Mit rampe·t = -averagevec·f wird aus dem Minus ein Plus.
                if (opt.origin) {
                    A.m[0][3] += (*opt.origin)[0];
                    A.m[1][3] += (*opt.origin)[1];
                    A.m[2][3] += (*opt.origin)[2];
                }
                if (opt.rootMotionPerFrame) {
                    const float ff = static_cast<float>(f);
                    A.m[0][3] += (*opt.rootMotionPerFrame)[0] * ff;
                    A.m[1][3] += (*opt.rootMotionPerFrame)[1] * ff;
                    A.m[2][3] += (*opt.rootMotionPerFrame)[2] * ff;
                }
                X[bu] = mul(A, B[bu]);
            } else {
                const auto pu = static_cast<std::size_t>(p);
                X[bu] = mul(mul(mul(X[pu], Binv[pu]), A), B[bu]);
            }
        }

        // Schritt 2: zurueck in den dotXSI-Raum und lokal machen.
        std::vector<Mat3x4> wx(static_cast<std::size_t>(numBones));
        for (int b = 0; b < numBones; ++b) {
            const auto bu = static_cast<std::size_t>(b);
            wx[bu] = mul(mul(Cinv, scaled(X[bu], 1.0f / scale)), C);
        }

        for (int b = 0; b < numBones; ++b) {
            const auto bu = static_cast<std::size_t>(b);
            const int p = skel.bones[bu].parent;
            const Mat3x4 local =
                p < 0 ? wx[bu] : mul(affineInverse(wx[static_cast<std::size_t>(p)]), wx[bu]);

            Key& k = keys[bu][static_cast<std::size_t>(f)];
            k.t[0] = local.m[0][3];
            k.t[1] = local.m[1][3];
            k.t[2] = local.m[2][3];

            // Skalierung abtrennen, BEVOR die Rotation zerlegt wird.
            //
            // Nicht jede lokale Transformation ist massstabstreu: die
            // Gesichtsbones unter "face" tragen in Ravens Skelett eine
            // Skalierung von 1,087. Verwirft man sie, kommt beim
            // Rueckimport genau der Kehrwert heraus — 0,92 statt 1,0 — und
            // die Gesichtsbones sitzen falsch.
            //
            // Der Importeur setzt m = R·diag(s) zusammen, also sind die
            // Spaltenlaengen genau die Skalierung.
            //
            // Die Spalten nur zu normieren reicht nicht: sind sie nicht
            // genau senkrecht zueinander — und Ravens Bindposen sind das
            // nicht ganz —, ist das Ergebnis keine Rotation, und die
            // Euler-Zerlegung verliert daran. Bei JK2s flacher Hierarchie,
            // wo 46 Bones direkt am Brustkorb haengen, wird aus diesem
            // kleinen Winkelfehler ueber den langen Hebel ein sichtbarer
            // Versatz an der Fingerspitze.
            //
            // Die Polarzerlegung liefert stattdessen die Rotation, die der
            // Matrix am naechsten kommt. Die Scherung selbst ist nicht
            // darstellbar — der Importeur baut m = R·diag(s) —, aber der
            // verbleibende Fehler wird so klein wie moeglich.
            const Mat3x4 rot = nearestRotation(local);
            for (int c = 0; c < 3; ++c) {
                // Skalierung entlang der GEDREHTEN Achse messen, nicht die
                // rohe Spaltenlaenge.
                double proj = 0.0;
                for (int r = 0; r < 3; ++r)
                    proj += static_cast<double>(rot.m[r][c]) * local.m[r][c];
                k.s[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
            }
            decomposeEuler(rot, k.r[0], k.r[1], k.r[2]);
        }
    }

    // --- Schreiben ---------------------------------------------------------
    std::ostringstream o;
    o << (opt.version == ExportOptions::Version::V30 ? "xsi 0300txt 0032\n\n"
                                                     : "xsi 0350txt 0032\n\n");
    o << "SI_CoordinateSystem coord {\n  1,\n  0,\n  1,\n  0,\n  2,\n  5,\n}\n\n";

    // SI_Scene: der Framebereich, den der Importeur liest. Die Rate wird
    // beim Einlesen abgeschnitten, also gleich ganzzahlig schreiben.
    o << "SI_Scene scene {\n  \"FRAMES\",\n  0,\n  " << (outFrames - 1) << ",\n  "
      << opt.fps << ",\n}\n\n";

    // Verschachtelte SI_Model, damit die Hierarchie erhalten bleibt.
    std::vector<std::vector<int>> children(static_cast<std::size_t>(numBones));
    std::vector<int> roots;
    for (int b = 0; b < numBones; ++b) {
        const int p = skel.bones[static_cast<std::size_t>(b)].parent;
        if (p < 0) roots.push_back(b);
        else children[static_cast<std::size_t>(p)].push_back(b);
    }

    enum class Part { Rot, Trans, Scale };
    const auto curve = [&](std::ostringstream& s, const std::string& bone, const char* channel,
                           int component, Part part, const std::vector<Key>& kk,
                           const std::string& ind) {
        // v3.0 benennt die Templates, v3.5 nicht. Der Inhalt ist identisch —
        // der Bonename steht ohnehin als erster Wert im Block.
        if (opt.version == ExportOptions::Version::V30)
            s << ind << "SI_FCurve " << bone << "-" << channel << " {\n";
        else
            s << ind << "SI_FCurve {\n";
        s << ind << "  \"" << bone << "\",\n";
        s << ind << "  \"" << channel << "\",\n";
        s << ind << "  \"CONSTANT\",\n";
        s << ind << "  1,\n";
        s << ind << "  1,\n";
        s << ind << "  " << kk.size() << ",\n";
        for (std::size_t f = 0; f < kk.size(); ++f) {
            const float v = part == Part::Rot     ? kk[f].r[component]
                            : part == Part::Trans ? kk[f].t[component]
                                                  : kk[f].s[component];
            s << ind << "  " << f << ", " << num(v) << ",\n";
        }
        s << ind << "}\n";
    };

    std::function<void(int, const std::string&)> writeBone = [&](int b, const std::string& ind) {
        const auto bu = static_cast<std::size_t>(b);
        const std::string& name = skel.bones[bu].name;
        const auto& kk = keys[bu];

        o << ind << "SI_Model MDL-" << name << " {\n";

        // SRT als Ruhepose: der erste Frame. Ein Bone ohne Kurve stuende
        // sonst in der Identitaet statt an seinem Platz.
        // SI_Transform ist die RUHELAGE, nicht Frame 0 der Animation.
        //
        // Carcass liest genau diesen Block als Bindepose, verkettet ihn ueber
        // die Hierarchie und vergleicht das Ergebnis mit dem Zielskelett.
        // Stand hier die animierte Pose von Frame 0, meldete es fuer jeden
        // Bone "Basepose for bone ... differs" — bei lower_lumbar etwa um
        // 41,35, und das ist exakt die Z-Hoehe der Bindepose.
        //
        // Der eigene Importeur nahm die Werte nie, weil er die FCurves
        // auswertet; der Fehler blieb deshalb unbemerkt, bis jemand die
        // Datei durch Ravens Carcass schickte.
        {
            const Mat3x4& Bb = skel.bones[bu].basePose;
            const int par = skel.bones[bu].parent;
            // Lokal gegen den Elternbone, dann in XSI-Koordinaten.
            const Mat3x4 localBind =
                par < 0 ? Bb : mul(affineInverse(skel.bones[static_cast<std::size_t>(par)].basePose), Bb);
            // NICHT durch scale teilen.
            //
            // Die Skelettskalierung steckt in jeder Bindepose; beim Bilden
            // der LOKALEN Pose — Eltern^-1 mal Kind — kuerzt sie sich
            // heraus. Wer hier nochmals teilt, schreibt Skalierung 1,5625
            // statt 1,0 und Translation 14,06 statt 9,0.
            //
            // Gegengeprueft an Ravens eigener Both_forcelandleft1.xsi: dort
            // steht fuer lower_lumbar exakt Skalierung 1,0 und Translation
            // 9,0 — genau das, was die lokale Bindepose ergibt.
            // Bei der WURZEL die Skelettskalierung herausteilen.
            //
            // Sie steckt in jeder Bindepose. Beim Bilden der lokalen Pose —
            // Eltern^-1 mal Kind — kuerzt sie sich heraus, aber die Wurzel
            // hat keinen Elternbone: dort bleibt sie stehen.
            //
            // Carcass multipliziert die gelesene Ruhelage selbst mit der
            // Skalierung. Blieb sie drin, kam 0,64 mal 0,64 = 0,4096 heraus,
            // erwartet wurden 0,64 — und die Meldung lautete "Basepose for
            // bone model_root differs by 0.230400", also genau
            // 0,64 minus 0,4096. Weil die ganze Kette darauf aufbaut, war
            // danach JEDER Bone falsch.
            //
            // Ravens eigene Dateien bestaetigen es: dort steht fuer pelvis
            // Skalierung 1,0, nicht 0,64.
            Mat3x4 localFixed = localBind;
            if (par < 0 && scale > 0.0f)
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) localFixed.m[r][c] /= scale;

            const Mat3x4 wx = mul(mul(Cinv, localFixed), C);

            float rs[3], rr[3], rt[3];
            const Mat3x4 rot = nearestRotation(wx);
            for (int c = 0; c < 3; ++c) {
                double proj = 0.0;
                for (int r = 0; r < 3; ++r) proj += static_cast<double>(rot.m[r][c]) * wx.m[r][c];
                rs[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
            }
            decomposeEuler(rot, rr[0], rr[1], rr[2]);
            for (int c = 0; c < 3; ++c) rt[c] = wx.m[c][3];

            // BASEPOSE: das ist der Block, den Carcass als Bindepose liest.
            //
            // Ravens root.xsi hat pro Bone ZWEI Transform-Bloecke — "SRT-"
            // fuer die Pose und "BASEPOSE-" fuer die Bindepose. Wir schrieben
            // nur den ersten, und deshalb meldete Carcass fuer jeden Bone
            // "Basepose ... differs": es fand keinen und verglich gegen das,
            // was zufaellig dastand.
            //
            // Der Inhalt ist die WELT-Bindepose in dotXSI-Koordinaten, durch
            // die Skelettskalierung geteilt. Gegengeprueft an Ravens eigener
            // Datei: fuer lfemurYZ steht dort 5.643987, 55.604065, 0.322196 —
            // und genau das ergibt die Rechnung.
            // Die WURZEL bekommt keinen BASEPOSE-Block.
            //
            // Ravens Dateien haben durchweg genau DREI SRT-Bloecke mehr als
            // BASEPOSE-Bloecke — in root.xsi (277/274) wie in jeder
            // Animationsdatei (98/95). Nachgesehen, welche fehlen:
            // model_root, mesh_root und skeleton_root, also die
            // Wurzelknoten.
            //
            // Wir schrieben fuer model_root einen, und Carcass verkettete
            // ihn mit der Kette darunter — daher "non-uniform scaling" mit
            // Werten um 0,4096, also 0,64 zum Quadrat.
            //
            // mesh_root und skeleton_root gibt es bei uns nicht; die GLA
            // kennt sie nicht.
            if (opt.basePose != ExportOptions::BasePose::None && par >= 0) {
                // World: die Weltpose, wie in Ravens Dateien.
                // Local:  gegen den Elternbone — falls Carcass selbst
                //         verkettet, waere die Weltpose doppelt skaliert.
                Mat3x4 world = opt.basePose == ExportOptions::BasePose::Local ? localBind : Bb;
                if (scale > 0.0f) {
                    // Bei der lokalen Pose kuerzt sich die Skalierung schon
                    // heraus, ausser bei der Wurzel.
                    const bool teilen =
                        opt.basePose == ExportOptions::BasePose::World || par < 0;
                    if (teilen)
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c) world.m[r][c] /= scale;
                }
                const Mat3x4 bx = mul(mul(Cinv, world), C);

                float bs[3], br[3], bt[3];
                const Mat3x4 brot = nearestRotation(bx);
                for (int c = 0; c < 3; ++c) {
                    double proj = 0.0;
                    for (int r = 0; r < 3; ++r)
                        proj += static_cast<double>(brot.m[r][c]) * bx.m[r][c];
                    bs[c] = static_cast<float>(std::fabs(proj) > 1e-9 ? proj : 1.0);
                }
                decomposeEuler(brot, br[0], br[1], br[2]);
                for (int c = 0; c < 3; ++c) bt[c] = bx.m[c][3];

                o << ind << "  SI_Transform BASEPOSE-" << name << " {\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(bs[k]) << ",\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(br[k]) << ",\n";
                for (int k = 0; k < 3; ++k) o << ind << "    " << num(bt[k]) << ",\n";
                o << ind << "  }\n";
            }

            o << ind << "  SI_Transform SRT-" << name << " {\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rs[k]) << ",\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rr[k]) << ",\n";
            for (int k = 0; k < 3; ++k) o << ind << "    " << num(rt[k]) << ",\n";
            o << ind << "  }\n";
        }

        // SI_FCurve MUSS direktes Kind von SI_Model sein.
        //
        // Der Importeur sucht mit findAll nur unter den unmittelbaren
        // Kindern. Ein umschliessender SI_Animation-Block macht die Kurven
        // unsichtbar: es kamen null Kanaele an, jeder Bone blieb auf seiner
        // SRT-Ruhepose stehen, und deshalb stimmte ausgerechnet Frame 0
        // immer und alles danach nicht. Ravens eigene Dateien legen die
        // Kurven ebenfalls direkt unter SI_Model.
        const char* scCh[3] = {"SCALING-X", "SCALING-Y", "SCALING-Z"};
        const char* rotCh[3] = {"ROTATION-X", "ROTATION-Y", "ROTATION-Z"};
        const char* trCh[3] = {"TRANSLATION-X", "TRANSLATION-Y", "TRANSLATION-Z"};
        for (int k = 0; k < 3; ++k) curve(o, name, scCh[k], k, Part::Scale, kk, ind + "  ");
        for (int k = 0; k < 3; ++k) curve(o, name, rotCh[k], k, Part::Rot, kk, ind + "  ");
        for (int k = 0; k < 3; ++k) curve(o, name, trCh[k], k, Part::Trans, kk, ind + "  ");

        for (const int c : children[bu]) writeBone(c, ind + "  ");
        o << ind << "}\n";
    };

    for (const int r : roots) writeBone(r, "");
    return o.str();
}

}  // namespace g2::xsiexp
