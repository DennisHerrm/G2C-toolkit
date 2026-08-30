// Minimaler Testrunner ohne externe Abhaengigkeiten.

#include "g2/mdxa.h"
#include "g2/parallel.h"
#include "g2/mdxm.h"
#include "g2/xsi.h"
#include "g2/xsi_anim.h"
#include "g2/xsi_export.h"
#include "g2/carbuild.h"
#include "g2/animcache.h"
#include "g2/animenums.h"
#include "g2/carvalidate.h"
#include "g2/carscript.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <locale>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

// Ein eigener Ordner je Lauf. Bleibt ein Testlauf haengen, haelt der Prozess
// seine Dateien offen; ein neuer Lauf im selben Ordner liefe unter Windows in
// die Sperren des alten. Mit Prozesskennung und Zeitstempel im Namen kann das
// nicht passieren.
std::string uniqueTestDir(const char* what) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("g2c_") + what + "_" +
           std::to_string(static_cast<unsigned long long>(now) & 0xffffffull);
}

// Feinschritt-Markierung. Sichtbar nur, wenn G2C_TRACE gesetzt ist — im
// Normalfall stoert sie, beim Suchen eines Haengers ist sie das Einzige, was
// hilft.
// Wo steckt der Lauf gerade? Der Wachhund liest das aus.
std::atomic<const char*> g_currentTest{"(Start)"};
std::atomic<const char*> g_currentStep{"(noch nichts)"};
std::atomic<bool>        g_watchdogStop{false};

bool g_trace = false;

void step(const char* what) {
    g_currentStep.store(what);
    if (g_trace) std::printf("    [%s]\n", what);
}

// Meldet sich, wenn ein Abschnitt haengt, und bricht danach ab.
//
// Ein haengender Test ist schlimmer als ein abstuerzender: er sagt gar
// nichts. Vorher musste man raten, welcher Schritt stehengeblieben ist —
// jetzt sagt es das Programm von selbst, ohne Schalter und ohne zweiten
// Lauf.
void watchdogMain() {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    int lastReport = 0;
    while (!g_watchdogStop.load()) {
        std::this_thread::sleep_for(milliseconds(200));
        const int secs = static_cast<int>(duration_cast<seconds>(steady_clock::now() - start).count());
        if (secs >= 10 && secs != lastReport && secs % 5 == 0) {
            lastReport = secs;
            std::printf("\n  [!] laeuft seit %d s in \"%s\", Schritt \"%s\"\n", secs,
                        g_currentTest.load(), g_currentStep.load());
            std::fflush(stdout);
        }
        if (secs >= 60) {
            std::printf("\n"
                        "============================================================\n"
                        " ABBRUCH: der Lauf haengt.\n"
                        "============================================================\n"
                        "  Test    : %s\n"
                        "  Schritt : %s\n"
                        "\n"
                        "  Diese beiden Angaben genuegen zur Eingrenzung.\n"
                        "  g2c.exe und g2c-gui.exe sind davon nicht betroffen.\n\n",
                        g_currentTest.load(), g_currentStep.load());
            std::fflush(stdout);
            std::_Exit(3);
        }
    }
}
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

void checkNear(double a, double b, double tol, const std::string& what) {
    ++g_checks;
    if (!(std::fabs(a - b) <= tol)) {
        ++g_failures;
        std::cout << "  FAIL: " << what << " (" << a << " vs " << b << ", tol " << tol << ")\n";
    }
}

void section(const std::string& s) { std::cout << "\n== " << s << " ==\n"; }

g2::Mat3x4 rotationZ(float rad) {
    g2::Mat3x4 m = g2::Mat3x4::identity();
    m.m[0][0] = std::cos(rad);  m.m[0][1] = -std::sin(rad);
    m.m[1][0] = std::sin(rad);  m.m[1][1] = std::cos(rad);
    return m;
}

// ---------------------------------------------------------------------------

void testQuantizationBias() {
    section("B2: Truncation vs. Rundung");

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    double sumLegacy = 0, sumNearest = 0;
    double maxLegacy = 0, maxNearest = 0;
    double biasLegacy = 0, biasNearest = 0;
    const int N = 200000;

    g2::CompressStats sl, sn;
    for (int i = 0; i < N; ++i) {
        const float f = dist(rng);

        const float backL = g2::unsquashQuatComponent(
            g2::squashQuatComponent(f, g2::Rounding::Legacy, sl));
        const float backN = g2::unsquashQuatComponent(
            g2::squashQuatComponent(f, g2::Rounding::Nearest, sn));

        const double eL = backL - f, eN = backN - f;
        biasLegacy += eL;  biasNearest += eN;
        sumLegacy += std::fabs(eL);  sumNearest += std::fabs(eN);
        maxLegacy = std::max(maxLegacy, std::fabs(eL));
        maxNearest = std::max(maxNearest, std::fabs(eN));
    }

    const double meanL = sumLegacy / N, meanN = sumNearest / N;
    const double step = 1.0 / 16383.0;

    std::printf("  Schrittweite            : %.3e\n", step);
    std::printf("  mittl. |Fehler| Legacy  : %.3e  (%.2f Stufen)\n", meanL, meanL / step);
    std::printf("  mittl. |Fehler| Nearest : %.3e  (%.2f Stufen)\n", meanN, meanN / step);
    std::printf("  max. Fehler Legacy      : %.3e\n", maxLegacy);
    std::printf("  max. Fehler Nearest     : %.3e\n", maxNearest);
    std::printf("  mittl. Bias Legacy      : %+.3e  <-- einseitig\n", biasLegacy / N);
    std::printf("  mittl. Bias Nearest     : %+.3e\n", biasNearest / N);
    std::printf("  Verbesserung            : Faktor %.2f beim mittleren Fehler\n", meanL / meanN);

    check(meanN < meanL * 0.6, "Rundung halbiert den mittleren Fehler");
    check(maxNearest < maxLegacy * 0.6, "Rundung halbiert den maximalen Fehler");
    check(std::fabs(biasLegacy / N) > 20.0 * std::fabs(biasNearest / N),
          "Legacy hat einen deutlich groesseren systematischen Bias");
    check(std::fabs(biasLegacy / N) > 0.4 * step, "Legacy-Bias liegt bei ~einer halben Stufe");
}

void testOutOfRangeClamping() {
    section("B3: Out-of-Range wird geklemmt statt genullt");

    g2::CompressStats stats;

    // Carcass gibt hier 0 zurueck, was zu -2.0 dekodiert.
    const float backQuat = g2::unsquashQuatComponent(
        g2::squashQuatComponent(5.0f, g2::Rounding::Nearest, stats));
    checkNear(backQuat, 2.0, 1e-3, "Quaternion 5.0 wird auf +2.0 geklemmt, nicht auf -2.0");
    check(stats.quatClamped == 1, "Klemmung wird gezaehlt");

    // Und hier: -512 Einheiten Translationssprung im Original.
    const float backXlat = g2::unsquashXlatComponent(
        g2::squashXlatComponent(9000.0f, g2::Rounding::Nearest, stats));
    checkNear(backXlat, 511.0, 1e-2, "Translation 9000 wird auf +511 geklemmt, nicht auf -512");
    check(stats.xlatClamped == 1, "Translationsklemmung wird gezaehlt");

    // NaN darf nicht durchrutschen.
    const float backNan = g2::unsquashQuatComponent(
        g2::squashQuatComponent(std::nanf(""), g2::Rounding::Nearest, stats));
    check(std::isfinite(backNan), "NaN erzeugt einen endlichen Wert");

    std::printf("  Statistik: %s\n", stats.summary().c_str());
}

void testQuatMatrixRoundTrip() {
    section("Matrix <-> Quaternion");

    // Auch nahe 180 Grad, wo die naive Trace-Formel zusammenbricht.
    const float angles[] = {0.0f, 0.1f, 1.0f, 3.0f, 3.14159f, -3.14159f, 2.5f};
    for (float a : angles) {
        const g2::Mat3x4 m = rotationZ(a);
        g2::Mat3x4 back;
        g2::quatToMatrix(g2::matrixToQuat(m), back);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                checkNear(back.m[r][c], m.m[r][c], 1e-4,
                          "Rotation " + std::to_string(a) + " Element " + std::to_string(r) +
                              "," + std::to_string(c));
    }
}

void testFullBoneRoundTrip() {
    section("Bone-Kompression gesamt");

    g2::Mat3x4 m = rotationZ(0.7f);
    m.m[0][3] = 12.5f;
    m.m[1][3] = -300.25f;
    m.m[2][3] = 64.0f;

    g2::CompressOptions optN;
    g2::CompressOptions optL;
    optL.rounding = g2::Rounding::Legacy;
    optL.canonicalizeSign = false;

    const float errN = g2::boneRoundTripError(m, optN);
    const float errL = g2::boneRoundTripError(m, optL);
    std::printf("  max. Elementfehler Legacy  : %.3e\n", errL);
    std::printf("  max. Elementfehler Nearest : %.3e\n", errN);
    check(errN <= errL, "Rundung ist nie schlechter als Truncation");
    check(errN < 0.02f, "Gesamtfehler bleibt unter 0.02 Einheiten");
}

g2::Skeleton makeSkeleton(int n) {
    g2::Skeleton s;
    s.name = "skeletons/test";
    s.scale = 1.0f;
    for (int i = 0; i < n; ++i) {
        g2::Bone b;
        b.name = "bone_" + std::to_string(i);
        b.parent = (i == 0) ? -1 : i - 1;
        b.basePose = g2::Mat3x4::identity();
        b.basePose.m[0][3] = static_cast<float>(i) * 4.0f;
        s.bones.push_back(b);
    }
    return s;
}

void testMdxaRoundTrip() {
    section("GLA schreiben und wieder lesen");

    const int nBones = 12, nFrames = 40;
    const g2::Skeleton skel = makeSkeleton(nBones);

    g2::AnimationFrames frames;
    frames.resize(nFrames, nBones);
    for (int f = 0; f < nFrames; ++f) {
        for (int b = 0; b < nBones; ++b) {
            g2::Mat3x4 m = rotationZ(0.02f * f * (b + 1));
            m.m[0][3] = 3.0f * b;
            m.m[1][3] = 0.5f * f;
            m.m[2][3] = 0.0f;
            frames.at(f, b) = m;
        }
    }

    const g2::MdxaWriteResult w = g2::writeMdxa(skel, frames);
    std::printf("  Dateigroesse            : %zu Bytes\n", w.data.size());
    std::printf("  Pool-Eintraege          : %zu von %zu (%.1f%% dedupliziert)\n",
                w.poolEntries, w.poolEntriesBeforeDedupe, w.dedupeRatio() * 100.0);
    std::printf("  Kompressionsstatistik   : %s\n", w.stats.summary().c_str());

    check(w.stats.clean(), "keine Klemmungen bei sauberen Eingangsdaten");

    const g2::MdxaFile f = g2::readMdxa(w.data);
    check(f.numFrames == nFrames, "Frame-Anzahl stimmt");
    check(static_cast<int>(f.skeleton.bones.size()) == nBones, "Bone-Anzahl stimmt");
    check(f.skeleton.name == skel.name, "GLA-Name stimmt");
    checkNear(f.skeleton.scale, 1.0, 1e-6, "Scale stimmt");

    for (int i = 0; i < nBones; ++i) {
        check(f.skeleton.bones[i].name == skel.bones[i].name, "Bone-Name " + std::to_string(i));
        check(f.skeleton.bones[i].parent == skel.bones[i].parent, "Parent " + std::to_string(i));
        checkNear(f.skeleton.bones[i].basePose.m[0][3], skel.bones[i].basePose.m[0][3], 1e-5,
                  "Basispose " + std::to_string(i));
    }

    double worstPos = 0, worstRot = 0;
    for (int fr = 0; fr < nFrames; ++fr) {
        for (int b = 0; b < nBones; ++b) {
            const g2::Mat3x4 got = f.boneMatrix(fr, b);
            const g2::Mat3x4& want = frames.at(fr, b);
            for (int r = 0; r < 3; ++r)
                worstPos = std::max(worstPos, std::fabs(static_cast<double>(got.m[r][3] - want.m[r][3])));
            worstRot = std::max(worstRot, g2::angleBetweenDeg(g2::matrixToQuat(got),
                                                              g2::matrixToQuat(want)));
        }
    }
    std::printf("  max. Positionsfehler    : %.5f Einheiten\n", worstPos);
    std::printf("  max. Rotationsfehler    : %.5f Grad\n", worstRot);
    check(worstPos < 0.01, "Positionsfehler unter 0.01 Einheiten");
    check(worstRot < 0.02, "Rotationsfehler unter 0.02 Grad");

    // Dieselben Daten im Carcass-Modus zum Vergleich.
    g2::MdxaWriteOptions legacy;
    legacy.compress.rounding = g2::Rounding::Legacy;
    legacy.compress.canonicalizeSign = false;
    legacy.compress.optimizeQuat = false;
    const auto wl = g2::writeMdxa(skel, frames, legacy);
    const g2::MdxaFile fl = g2::readMdxa(wl.data);

    double worstRotL = 0, worstPosL = 0;
    for (int fr = 0; fr < nFrames; ++fr) {
        for (int b = 0; b < nBones; ++b) {
            const g2::Mat3x4 got = fl.boneMatrix(fr, b);
            const g2::Mat3x4& want = frames.at(fr, b);
            for (int r = 0; r < 3; ++r)
                worstPosL = std::max(worstPosL, std::fabs(static_cast<double>(got.m[r][3] - want.m[r][3])));
            worstRotL = std::max(worstRotL, g2::angleBetweenDeg(g2::matrixToQuat(got),
                                                                g2::matrixToQuat(want)));
        }
    }
    std::printf("  ---- Carcass-Modus zum Vergleich ----\n");
    std::printf("  max. Positionsfehler    : %.5f Einheiten\n", worstPosL);
    std::printf("  max. Rotationsfehler    : %.5f Grad\n", worstRotL);
    std::printf("  Verbesserung Rotation   : Faktor %.2f\n", worstRotL / std::max(worstRot, 1e-12));
    check(worstRot < worstRotL, "neue Quantisierung schlaegt den Carcass-Modus");
}

void testAffineInverse() {
    section("Basispose-Inverse mit Skalierung");

    // Nachbau des echten Falls: $scale 0.64 in die Basispose eingebacken.
    g2::Mat3x4 m = rotationZ(0.6f);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m.m[r][c] *= 0.64f;
    m.m[0][3] = 3.6f; m.m[1][3] = -0.2f; m.m[2][3] = 35.6f;

    const g2::Mat3x4 inv = g2::affineInverse(m);

    // M * MInv muss die Einheitsmatrix ergeben.
    double worst = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += double(m.m[r][k]) * inv.m[k][c];
            worst = std::max(worst, std::fabs(v - (r == c ? 1.0 : 0.0)));
        }
        double t = inv.m[r][3];
        for (int k = 0; k < 3; ++k) t += double(m.m[r][k]) * 0;
        (void)t;
    }
    std::printf("  M * MInv Abweichung von der Einheitsmatrix: %.2e\n", worst);
    check(worst < 1e-5, "Inverse ist korrekt");

    // Die Transponierte waere hier deutlich falsch — genau der Fehler,
    // den die echte _humanoid.gla aufgedeckt hat.
    double transposeErr = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            transposeErr = std::max(transposeErr, std::fabs(double(inv.m[r][c]) - m.m[c][r]));
    std::printf("  Abweichung Transponierte vs. echte Inverse: %.4f\n", transposeErr);
    check(transposeErr > 0.5, "Transponierte waere bei Scale 0.64 grob falsch");

    // Und einmal durch den echten Schreibweg.
    g2::Skeleton skel;
    skel.name = "test";
    skel.bones.push_back({"root", -1, m, 0});
    g2::AnimationFrames fr;
    fr.resize(1, 1);
    const auto w = g2::writeMdxa(skel, fr);
    const auto rd = g2::readMdxa(w.data);
    checkNear(rd.skeleton.bones[0].basePose.m[0][0], m.m[0][0], 1e-5,
              "Basispose ueberlebt den Roundtrip");
}

void testDedupe() {
    section("Bone-Pool-Deduplizierung");

    const int nBones = 8, nFrames = 100;
    const g2::Skeleton skel = makeSkeleton(nBones);

    // Statische Pose ueber alle Frames: alles muss auf einen Eintrag fallen.
    g2::AnimationFrames frames;
    frames.resize(nFrames, nBones);

    g2::MdxaWriteOptions on;
    g2::MdxaWriteOptions off;
    off.dedupeBonePool = false;

    const auto a = g2::writeMdxa(skel, frames, on);
    const auto b = g2::writeMdxa(skel, frames, off);

    std::printf("  mit Dedupe   : %zu Pool-Eintraege, %zu Bytes\n", a.poolEntries, a.data.size());
    std::printf("  ohne Dedupe  : %zu Pool-Eintraege, %zu Bytes\n", b.poolEntries, b.data.size());
    std::printf("  Ersparnis    : %.1f%%\n", 100.0 * (1.0 - double(a.data.size()) / b.data.size()));

    check(a.poolEntries == 1, "identische Posen fallen auf einen Pool-Eintrag zusammen");
    check(b.poolEntries == static_cast<std::size_t>(nBones) * nFrames, "ohne Dedupe ein Eintrag pro Bone und Frame");
    check(a.data.size() < b.data.size(), "Dedupe verkleinert die Datei");

    const auto f = g2::readMdxa(a.data);
    check(f.numFrames == nFrames, "deduplizierte Datei laesst sich lesen");
}

void testSkeletonValidation() {
    section("Skelettpruefung");

    g2::Skeleton dup = makeSkeleton(3);
    dup.bones[2].name = dup.bones[1].name;
    check(!dup.validate().empty(), "doppelter Bone-Name wird erkannt");

    g2::Skeleton cyc = makeSkeleton(3);
    cyc.bones[0].parent = 2;   // 0 -> 2 -> 1 -> 0
    check(!cyc.validate().empty(), "Zyklus in der Hierarchie wird erkannt");

    g2::Skeleton bad = makeSkeleton(3);
    bad.bones[1].parent = 99;
    check(!bad.validate().empty(), "ungueltiger Parent-Index wird erkannt");

    check(makeSkeleton(5).validate().empty(), "sauberes Skelett passiert die Pruefung");
}

void testMdxmWrite() {
    section("GLM schreiben");

    g2::Mesh mesh;
    mesh.name = "models/players/test/model.glm";
    mesh.animName = "models/players/_humanoid/_humanoid";
    mesh.numBones = 6;

    g2::Surface surf;
    surf.name = "body";
    surf.shader = "models/players/test/body";
    surf.parentIndex = -1;

    for (int i = 0; i < 6; ++i) {
        g2::Vertex v;
        v.position[0] = static_cast<float>(i);
        v.position[1] = static_cast<float>(i % 3);
        v.position[2] = 1.0f;
        v.normal[2] = 1.0f;
        v.uv[0] = 0.1f * i;
        v.uv[1] = 0.2f * i;
        v.weights.push_back({i % 4, 0.7f});
        v.weights.push_back({(i + 1) % 4, 0.3f});
        surf.vertices.push_back(v);
    }
    surf.triangles.push_back({{0, 1, 2}});
    surf.triangles.push_back({{3, 4, 5}});

    mesh.lods.push_back(g2::LOD{{surf}});

    const auto w = g2::writeMdxm(mesh);
    std::printf("  Dateigroesse         : %zu Bytes\n", w.data.size());
    std::printf("  max. Bone-Referenzen : %zu von %d\n", w.stats.maxBoneRefsUsed,
                g2::fmt::kMaxBoneRefsPerSurface);

    check(w.data.size() > sizeof(g2::fmt::MdxmHeader), "Datei ist nicht leer");

    // Header zurueckparsen
    const auto rd32 = [&](std::size_t o) {
        return static_cast<std::int32_t>(w.data[o] | (w.data[o + 1] << 8) | (w.data[o + 2] << 16) |
                                         (std::uint32_t(w.data[o + 3]) << 24));
    };
    check(static_cast<std::uint32_t>(rd32(0)) == g2::fmt::kMdxmIdent, "Ident ist \"2LGM\"");
    check(rd32(4) == g2::fmt::kMdxmVersion, "Version ist 6");
    // Header-Offsets: numBones=140, numLODs=144, ofsLODs=148,
    // numSurfaces=152, ofsSurfHierarchy=156, ofsEnd=160
    check(rd32(140) == 6, "numBones ist 6");
    check(rd32(144) == 1, "numLODs ist 1");
    check(rd32(152) == 1, "numSurfaces ist 1");
    check(static_cast<std::size_t>(rd32(160)) == w.data.size(), "ofsEnd entspricht der Dateigroesse");

    // Vertex-Weight-Packing pruefen
    const std::int32_t ofsLODs = rd32(148);
    const std::int32_t lodSurfOfs = rd32(static_cast<std::size_t>(ofsLODs) + 4);
    const std::size_t surfStart =
        static_cast<std::size_t>(ofsLODs) + 4 + static_cast<std::size_t>(lodSurfOfs);
    const std::int32_t numVerts = rd32(surfStart + 12);
    const std::int32_t ofsVerts = rd32(surfStart + 16);
    check(numVerts == 6, "Surface hat 6 Vertices");

    const std::size_t v0 = surfStart + static_cast<std::size_t>(ofsVerts);
    g2::fmt::MdxmVertex mv{};
    std::memcpy(&mv, w.data.data() + v0, sizeof(mv));
    check(g2::fmt::getVertWeightCount(mv) == 2, "erster Vertex hat 2 Gewichte");

    // Offsetbasis der LOD-Surface-Tabelle: an der echten _humanoid.glm
    // verifiziert. offsets[0] muss numSurfaces*4 sein, weil die erste Surface
    // direkt hinter der Tabelle liegt.
    check(lodSurfOfs == 1 * 4, "LOD-Surface-Offset ist relativ zur Offsettabelle");
    check(rd32(surfStart + 8) == -static_cast<std::int32_t>(surfStart),
          "ofsHeader zeigt korrekt zum Dateianfang zurueck");

    const int raw0 = g2::fmt::getVertBoneWeightRaw(mv, 0);
    checkNear(raw0 * g2::fmt::kBoneWeightReciprocal, 0.7, 0.002, "Gewicht 0 dekodiert zu 0.7");
    check(g2::fmt::getVertBoneIndex(mv, 0) < g2::fmt::kMaxBoneRefsPerSurface,
          "lokaler Bone-Index liegt im 5-Bit-Bereich");
}

void testMdxmMultiLod() {
    section("GLM mit mehreren LODs");

    // Struktur an Ravens model.glm abgelesen: 4 LODs, 80 Surfaces,
    // offsets[0] = numSurfaces*4, und jeder LOD-Block endet dort, wo der
    // naechste beginnt.
    g2::Mesh m;
    m.name = "model.glm";
    m.animName = "models/players/_humanoid/_humanoid";
    m.numBones = 53;

    const int counts[4] = {12, 9, 6, 3};
    for (int L = 0; L < 4; ++L) {
        g2::LOD lod;
        for (int s = 0; s < 3; ++s) {
            g2::Surface surf;
            surf.name = s ? ("*tag" + std::to_string(s)) : "body";
            surf.shader = "models/players/luke/torso.tga";
            surf.parentIndex = s ? 0 : -1;
            surf.flags = s ? 1u : 0u;
            for (int v = 0; v < counts[L]; ++v) {
                g2::Vertex x;
                x.position[0] = static_cast<float>(v);
                x.normal[2] = 1.0f;
                x.weights.push_back({v % 4, 1.0f});
                surf.vertices.push_back(x);
            }
            for (int tri = 0; tri + 2 < counts[L]; ++tri)
                surf.triangles.push_back({{tri, tri + 1, tri + 2}});
            lod.surfaces.push_back(std::move(surf));
        }
        m.lods.push_back(std::move(lod));
    }

    const auto w = g2::writeMdxm(m);
    const auto rd = [&](std::size_t o) {
        std::int32_t v;
        std::memcpy(&v, w.data.data() + o, 4);
        return v;
    };
    const int nS = rd(152), nl = rd(144), ofsL = rd(148);
    check(nl == 4, "vier LODs im Header");
    check(nS == 3, "drei Surfaces");
    check(static_cast<std::size_t>(rd(160)) == w.data.size(), "ofsEnd stimmt");

    int lod = ofsL;
    bool allOk = true;
    for (int L = 0; L < nl; ++L) {
        // offsets[0] muss numSurfaces*4 sein: die erste Surface liegt direkt
        // hinter der Offsettabelle, und die Werte zaehlen ab deren Anfang.
        if (rd(static_cast<std::size_t>(lod) + 4) != nS * 4) allOk = false;
        for (int s = 0; s < nS; ++s) {
            const int o = lod + 4 + rd(static_cast<std::size_t>(lod + 4 + s * 4));
            if (rd(static_cast<std::size_t>(o) + 8) != -o) allOk = false;
            if (rd(static_cast<std::size_t>(o) + 4) != s) allOk = false;
        }
        lod += rd(static_cast<std::size_t>(lod));
    }
    check(allOk, "Offsets und Rueckverweise in allen LODs korrekt");
    check(static_cast<std::size_t>(lod) == w.data.size(),
          "LOD-Kette endet genau am Dateiende");
    std::printf("  4 LODs, %zu Bytes, Kette geschlossen\n", w.data.size());
}

void testMdxmLimits() {
    section("GLM-Grenzen");

    g2::Mesh mesh;
    mesh.name = "models/test.glm";
    mesh.animName = "models/players/_humanoid/_humanoid";
    mesh.numBones = 64;

    g2::Surface surf;
    surf.name = "toomany";
    surf.shader = "shader";

    // 40 verschiedene Bones referenzieren -> muss scheitern, nicht still kuerzen.
    for (int i = 0; i < 40; ++i) {
        g2::Vertex v;
        v.normal[2] = 1.0f;
        v.weights.push_back({i, 1.0f});
        surf.vertices.push_back(v);
    }
    surf.triangles.push_back({{0, 1, 2}});
    mesh.lods.push_back(g2::LOD{{surf}});

    bool threw = false;
    try {
        g2::writeMdxm(mesh);
    } catch (const std::exception& e) {
        threw = true;
        std::printf("  erwarteter Fehler: %s\n", e.what());
    }
    check(threw, "mehr als 32 Bone-Referenzen werden abgelehnt statt still gekuerzt");

    // Ungueltiger Dreiecksindex
    g2::Mesh bad;
    bad.name = "models/test.glm";
    bad.animName = "x";
    g2::Surface s2;
    s2.name = "s";
    s2.shader = "sh";
    g2::Vertex v;
    v.weights.push_back({0, 1.0f});
    s2.vertices.push_back(v);
    s2.triangles.push_back({{0, 1, 2}});
    bad.lods.push_back(g2::LOD{{s2}});
    check(!bad.validate().empty(), "Dreiecksindex ausserhalb der Vertexliste wird erkannt");
}

void testXsiParser() {
    section("dotXSI-Parser");

    const std::string src = R"(xsi 0103txt 0032

// Kommentar
SI_Model MDL-root {
    SI_Transform SRT-root {
        0.0; 0.0; 0.0;
        1.0; 1.0; 1.0;
        0.0; 0.0; 0.0;
    }
    SI_FrameBasePoseMatrix {
        1.0,0.0,0.0,0.0,
        0.0,1.0,0.0,0.0,
        0.0,0.0,1.0,0.0,
        0.0,0.0,0.0,1.0;
    }
    SI_Model MDL-pelvis {
        SI_Shape SHP-pelvis {
            2;
            POSITION
            SI_Mesh MSH-pelvis {
                3;
            }
        }
    }
    /* Blockkommentar
       ueber mehrere Zeilen */
    SI_Material {
        "textures/body.tga";
    }
}
)";

    const auto doc = g2::xsi::parse(src);
    check(doc.version.major == 1 && doc.version.minor == 3, "Version 1.3 erkannt");
    check(!doc.version.binary, "Textvariante erkannt");
    check(doc.roots.size() == 1, "genau ein Wurzeltemplate");

    const auto* root = doc.find("SI_Model");
    check(root != nullptr, "SI_Model gefunden");
    check(root && root->name == "MDL-root", "Instanzname MDL-root");

    const auto* xf = root ? root->find("SI_Transform") : nullptr;
    check(xf != nullptr, "SI_Transform gefunden");
    check(xf && xf->values.size() == 9, "SI_Transform hat 9 Werte");
    if (xf && xf->values.size() == 9)
        checkNear(xf->values[3].asNumber().value_or(-1), 1.0, 1e-9, "vierter Wert ist 1.0");

    const auto* bpm = root ? root->find("SI_FrameBasePoseMatrix") : nullptr;
    check(bpm && bpm->values.size() == 16, "Basispose-Matrix hat 16 Werte");

    // Der interessante Fall: POSITION ist ein Wert, SI_Mesh ein Template.
    const auto* shape = doc.findDeep("SI_Shape");
    check(shape != nullptr, "SI_Shape tief gefunden");
    if (shape) {
        bool hasPosition = false;
        for (const auto& v : shape->values)
            if (v.text() == "POSITION") hasPosition = true;
        check(hasPosition, "nackter Bezeichner POSITION bleibt ein Wert");
        check(shape->find("SI_Mesh") != nullptr, "SI_Mesh dahinter wird als Template erkannt");
        check(shape->find("POSITION") == nullptr, "POSITION wird NICHT als Template missdeutet");
    }

    const auto* mat = root ? root->find("SI_Material") : nullptr;
    check(mat && mat->values.size() == 1, "Materialtemplate hat einen Wert");
    check(mat && !mat->values.empty() && mat->values[0].text() == "textures/body.tga",
          "Anfuehrungszeichen werden entfernt");

    std::printf("  %zu Templates, %zu verschiedene Typen\n", doc.templateCount(),
                g2::xsi::summarize(doc).size());

    // Fehlerfaelle
    bool threw = false;
    try { g2::xsi::parse("nicht mal ein xsi header"); } catch (const std::exception&) { threw = true; }
    check(threw, "fehlender xsi-Header wird abgelehnt");

    threw = false;
    try { g2::xsi::parse("xsi 0101txt 0032\nSI_Model X {\n"); } catch (const std::exception& e) {
        threw = true;
        std::printf("  erwarteter Fehler: %s\n", e.what());
    }
    check(threw, "fehlende schliessende Klammer wird gemeldet");

    threw = false;
    try { g2::xsi::parse("xsi 0300bin 0032\n"); } catch (const std::exception&) { threw = true; }
    check(threw, "binaeres dotXSI wird klar abgelehnt");
}

void testAnimEval() {
    section("Animation gegen Referenzskelett");

    // Referenzskelett: root -> a -> b
    g2::Skeleton ref;
    ref.name = "ref";
    ref.scale = 1.0f;
    g2::Mat3x4 br = g2::Mat3x4::identity();
    g2::Mat3x4 ba = g2::Mat3x4::identity(); ba.m[2][3] = 10.0f;
    g2::Mat3x4 bb = g2::Mat3x4::identity(); bb.m[2][3] = 20.0f;
    ref.bones.push_back({"root", -1, br, 0});
    ref.bones.push_back({"a",     0, ba, 0});
    ref.bones.push_back({"b",     1, bb, 0});

    // Animationsdatei: nur "a" ist animiert, "b" fehlt ganz.
    const std::string src = R"(xsi 0350txt 0032
SI_Model MDL-rig.root {
    SI_Model MDL-rig.a {
        SI_FCurve { "rig.a", "ROTATION-Z", "LINEAR", 1, 1, 2, 1,0.000000, 2,30.000000, }
        SI_FCurve { "rig.a", "TRANSLATION-Z", "LINEAR", 1, 1, 2, 1,10.000000, 2,10.000000, }
    }
}
)";
    const auto anim = g2::xsi::loadAnimation(g2::xsi::parse(src), "test.xsi");
    check(anim.nodes.size() == 2, "zwei Knoten gelesen");
    check(anim.frameCount() == 2, "zwei Frames");

    // Framenummern mit Nachkommastellen. 3ds Max schreibt "1.000000", Raven
    // schreibt "1". Wer die Nummer als Ganzzahl liest, verliert bei
    // Max-Exporten JEDEN Keyframe — die Kanaele existieren dann zwar, sind
    // aber leer, und alle Bones bleiben stumm in der Ruhepose.
    const std::string maxStyle = R"(xsi 0300txt 0032
SI_Scene s { "FRAMES", 1.000000, 3.000000, 30.000000, }
SI_Model MDL-root {
    SI_Model MDL-a {
        SI_FCurve a-ROTATION-Z { "a", "ROTATION-Z", "LINEAR", 1, 1, 3,
            1.000000,0.000000,
            2.000000,15.000000,
            3.000000,30.000000,
        }
    }
}
)";
    const auto maxAnim = g2::xsi::loadAnimation(g2::xsi::parse(maxStyle), "max.xsi");
    const auto* na = maxAnim.find("a");
    check(na != nullptr, "Knoten aus dem Max-Export gefunden");
    check(na && na->animated(), "Kanal erkannt");
    if (na) {
        const auto it = na->channels.find("ROTATION-Z");
        check(it != na->channels.end(), "ROTATION-Z vorhanden");
        check(it != na->channels.end() && it->second.size() == 3,
              "alle drei Keys trotz Nachkommastellen gelesen");
        if (it != na->channels.end() && it->second.count(3))
            checkNear(it->second.at(3), 30.0, 1e-4, "Wert des letzten Keys");
    }
    check(maxAnim.hasScene && maxAnim.frameCount() == 3, "Framebereich aus SI_Scene");

    // SI_Transform SRT-<name> als Ruhewert. Bones ohne FCurve muessen ihre
    // statische Pose behalten, nicht in die Identitaet fallen. In root.xsi
    // haben nur 126 von 276 Modellen FCurves — ohne diesen Rueckfall steht
    // mehr als die Haelfte des Skeletts falsch.
    const std::string srtStyle = R"(xsi 0350txt 0032
SI_Scene s { "FRAMES", 1.000000, 2.000000, 20.000000, }
SI_Model MDL-root {
    SI_Model MDL-still {
        SI_Transform BASEPOSE-still { 1.0,1.0,1.0, 11.0,22.0,33.0, 4.0,5.0,6.0, }
        SI_Transform SRT-still      { 1.0,1.0,1.0,  0.0,0.0,90.0, 7.0,8.0,9.0, }
    }
    SI_Model MDL-moving {
        SI_Transform SRT-moving { 1.0,1.0,1.0, 0.0,0.0,0.0, 1.0,2.0,3.0, }
        SI_FCurve moving-TRANSLATION-X { "moving", "TRANSLATION-X", "LINEAR", 1, 1, 2,
            1.000000,50.000000,
            2.000000,60.000000,
        }
    }
}
)";
    const auto srtAnim = g2::xsi::loadAnimation(g2::xsi::parse(srtStyle), "srt.xsi");
    const int iStill = srtAnim.indexOf("still");
    const int iMoving = srtAnim.indexOf("moving");
    check(iStill >= 0 && iMoving >= 0, "beide Knoten gefunden");

    if (iStill >= 0) {
        check(srtAnim.nodes[iStill].hasSrt, "SRT-Block erkannt");
        check(srtAnim.nodes[iStill].channels.empty(), "Knoten hat keine FCurve");
        const g2::Mat3x4 m = srtAnim.localMatrix(iStill, 1);
        checkNear(m.m[0][3], 7.0, 1e-4, "SRT-Translation X statt 0");
        checkNear(m.m[1][3], 8.0, 1e-4, "SRT-Translation Y statt 0");
        checkNear(m.m[2][3], 9.0, 1e-4, "SRT-Translation Z statt 0");
        checkNear(m.m[0][0], std::cos(90.0 * 3.14159265358979 / 180.0), 1e-4,
                  "SRT-Rotation statt Identitaet");
    }
    if (iMoving >= 0) {
        // FCurve gewinnt in ihrem Kanal, SRT bleibt in den uebrigen.
        const g2::Mat3x4 m = srtAnim.localMatrix(iMoving, 2);
        checkNear(m.m[0][3], 60.0, 1e-4, "FCurve ueberschreibt den SRT-Kanal");
        checkNear(m.m[1][3], 2.0, 1e-4, "nicht animierter Kanal behaelt den SRT-Wert");
        checkNear(m.m[2][3], 3.0, 1e-4, "ebenso der dritte");
    }

    g2::xsi::EvalOptions opt;
    opt.scale = 1.0f;
    const auto res = g2::xsi::evaluate(ref, anim, opt);

    check(res.frameCount == 2, "zwei Frames erzeugt");
    check(res.missingBones.size() == 1 && res.missingBones[0] == "b",
          "fehlender Bone wird gemeldet");

    // Der nicht animierte Bone "b" muss die Einheitsmatrix bekommen — genau
    // das schreibt Carcass auch (in der echten _humanoid.gla steht beim nicht
    // animierten Bone "face" die Einheitsmatrix).
    for (int f = 0; f < 2; ++f) {
        const g2::Mat3x4& m = res.frames.at(f, 2);
        double worst = 0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, std::fabs(double(m.m[r][c]) - (r == c ? 1.0 : 0.0)));
        checkNear(worst, 0.0, 1e-5, "Frame " + std::to_string(f) + ": Bone \"b\" ist identisch");
    }

    // Frame 1 hat Rotation 0 -> auch "a" muss die Einheitsmatrix sein.
    {
        const g2::Mat3x4& m = res.frames.at(0, 1);
        checkNear(m.m[0][0], 1.0, 1e-4, "Frame 0: Bone \"a\" unrotiert");
        checkNear(m.m[0][1], 0.0, 1e-4, "Frame 0: Bone \"a\" ohne Scherung");
    }
    // Frame 2 hat 30 Grad um Z.
    {
        const g2::Mat3x4& m = res.frames.at(1, 1);
        checkNear(m.m[0][0], std::cos(30.0 * 3.14159265358979 / 180.0), 2e-3,
                  "Frame 1: Bone \"a\" um 30 Grad gedreht");
    }

    // Wurzelbewegung: Carcass legt auf den Wurzelbone eine LINEARE Rampe ueber
    // die Gesamtverschiebung des Motion-Bones, als Gegenbewegung. Bewusst
    // nicht die tatsaechliche Kurve — der Motion-Bone schwankt hier stark,
    // die Rampe muss trotzdem schnurgerade laufen.
    {
        const std::string src = R"(xsi 0350txt 0032
SI_Scene s { "FRAMES", 1.000000, 5.000000, 20.000000, }
SI_Model MDL-rig.root {
    SI_Model MDL-rig.Motion {
        SI_FCurve m-TRANSLATION-Z { "Motion", "TRANSLATION-Z", "LINEAR", 1, 1, 5,
            1.000000,0.000000,
            2.000000,90.000000,
            3.000000,-40.000000,
            4.000000,70.000000,
            5.000000,10.000000,
        }
    }
    SI_Model MDL-rig.a {
        SI_FCurve a-ROTATION-Z { "a", "ROTATION-Z", "LINEAR", 1, 1, 5,
            1.000000,0.000000, 2.000000,1.000000, 3.000000,2.000000,
            4.000000,3.000000, 5.000000,4.000000,
        }
    }
}
)";
        const auto ra = g2::xsi::loadAnimation(g2::xsi::parse(src), "root.xsi");
        g2::xsi::EvalOptions ro;
        ro.scale = 2.0f;
        const auto rr = g2::xsi::evaluate(ref, ra, ro);
        check(rr.frameCount == 5, "fuenf Frames");

        // Motion laeuft von z=0 auf z=10, also Gesamtverschiebung 10.
        // GLA-y = -scale * (-dz) = +scale*dz = 20, verteilt auf 4 Schritte.
        const double expect[5] = {0.0, 5.0, 10.0, 15.0, 20.0};
        for (int f = 0; f < 5; ++f)
            checkNear(rr.frames.at(f, 0).m[1][3], expect[f], 1e-3,
                      "Wurzelrampe Frame " + std::to_string(f));

        // Abschaltbar.
        g2::xsi::EvalOptions off = ro;
        off.extractRootMotion = false;
        const auto ro2 = g2::xsi::evaluate(ref, ra, off);
        checkNear(ro2.frames.at(4, 0).m[1][3], 0.0, 1e-4, "ohne Wurzelbewegung bleibt es bei 0");
    }

    // Origin-Versatz landet auf dem Wurzelbone.
    g2::xsi::EvalOptions o2 = opt;
    o2.origin = std::array<float, 3>{0.0f, 0.0f, 24.0f};
    const auto res2 = g2::xsi::evaluate(ref, anim, o2);
    checkNear(res2.frames.at(0, 0).m[2][3], -24.0, 1e-4, "-origin liegt negativ auf dem Wurzelbone");

    // Alias-Zuordnung
    g2::xsi::EvalOptions o3 = opt;
    o3.aliases["b"] = "a";
    const auto res3 = g2::xsi::evaluate(ref, anim, o3);
    check(res3.missingBones.empty(), "Alias loest den fehlenden Bone auf");

    std::printf("  %zu Bones, %d Frames, %zu fehlend, %zu ueberzaehlig\n", ref.bones.size(),
                res.frameCount, res.missingBones.size(), res.extraBones.size());
}

void testCarParser() {
    section(".car-Skriptparser");

    const std::string src =
        "$basedir base\n"
        "$modelname models/players/kyle\n"
        "$scale 1.0\n"
        "$origin 0 0 24\n"
        "$flatten\n"
        "// Kommentar\n"
        "$aseanimgrabinit\n"
        "$aseanimgrab root BOTH_STAND1 0 20\n"
        "$aseanimgrabfinalize\n"
        "$aseanimconvertmdx root\n"
        "$bloedsinn 1 2 3\n"
        "$exit\n"
        "$modelname wird-ignoriert\n";

    g2::car::ParseOptions opt;
    opt.followIncludes = false;
    const auto s = g2::car::parse(src, "test.car", opt);

    check(s.baseDir == "base", "$basedir gelesen");
    check(s.modelName == "models/players/kyle", "$modelname gelesen");
    check(s.scale.has_value() && std::fabs(*s.scale - 1.0) < 1e-9, "$scale gelesen");
    check(s.origin.has_value() && (*s.origin)[2] == 24.0, "$origin gelesen");
    check(s.flatten, "$flatten gesetzt");
    check(!s.keepMotion, "$keepmotion nicht gesetzt");

    bool sawUnknown = false, sawAfterExit = false;
    for (const auto& st : s.statements) {
        if (st.cmd == g2::car::Cmd::Unknown) sawUnknown = true;
        if (st.line == 13) sawAfterExit = true;
    }
    check(sawUnknown, "unbekannter Befehl wird durchgereicht statt verschluckt");
    check(!sawAfterExit, "nach $exit wird nichts mehr gelesen");

    const auto grab = std::find_if(s.statements.begin(), s.statements.end(),
                                   [](const g2::car::Statement& st) {
                                       return st.cmd == g2::car::Cmd::AseAnimGrab;
                                   });
    check(grab != s.statements.end(), "$aseanimgrab gefunden");
    check(grab != s.statements.end() && grab->args.size() == 4, "$aseanimgrab hat 4 Argumente");

    // Fehlende Argumente muessen eine brauchbare Meldung erzeugen.
    bool threw = false;
    try {
        g2::car::parse("$scale\n", "x.car", opt);
    } catch (const std::exception& e) {
        threw = true;
        std::printf("  erwarteter Fehler: %s\n", e.what());
    }
    check(threw, "$scale ohne Argument wird gemeldet");

    std::printf("  %zu Anweisungen geparst\n", s.statements.size());
}



void testCarBuild() {
    section(".car abarbeiten");

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "g2c_test_assets";
    { std::error_code ec; fs::remove_all(root, ec); }
    fs::create_directories(root / "models" / "anims");

    // Zwei winzige Animationsdateien anlegen.
    const auto writeAnim = [&](const std::string& name, int frames, float degPerFrame,
                               float rate = 0.0f) {
        std::ofstream f(root / "models" / "anims" / name);
        f << "xsi 0350txt 0032\n";
        if (rate > 0.0f)
            f << "SI_Scene testszene {\n  \"FRAMES\",\n  1.000000,\n  "
              << static_cast<float>(frames) << ",\n  " << rate << ",\n}\n";
        f << "SI_Model MDL-rig.root {\n"
          << "  SI_Model MDL-rig.a {\n"
          << "    SI_FCurve { \"rig.a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, " << frames << ",\n";
        for (int i = 1; i <= frames; ++i)
            f << "      " << i << "," << (degPerFrame * (i - 1)) << ",\n";
        f << "    }\n  }\n}\n";
    };
    writeAnim("first.xsi", 5, 3.0f);
    writeAnim("second.xsi", 8, 1.5f);
    writeAnim("scened.xsi", 6, 2.0f, 29.97f);  // mit SI_Scene, NTSC-Rate

    {
        std::ofstream c(root / "test.car");
        c << "$aseanimgrabinit\n"
             "$scale 1.0\n"
             "$aseanimgrab models/anims/first.xsi -loop -1 -framespeed 20\n"
             "$aseanimgrab models/anims/second.xsi -loop 3 -framespeed 30 "
             "-additional 2 4 -1 15 EXTRA_SEQ\n"
             // Ohne -loop und ohne -framespeed: Vorgaben muessen greifen.
             "$aseanimgrab models/anims/scened.xsi\n"
             "$aseanimgrabfinalize\n"
             "$aseanimconvertmdx_noask root -makeskel models/x/_humanoid -origin 0 0 24\n";
    }

    const auto script = g2::car::parseFile((root / "test.car").string());
    check(script.grabs.size() == 3, "drei Grabs gelesen");
    check(script.convert.has_value(), "Konvertierungsanweisung erkannt");

    g2::Skeleton ref;
    ref.name = "models/x/_humanoid";
    ref.scale = 1.0f;
    ref.bones.push_back({"root", -1, g2::Mat3x4::identity(), 0});
    g2::Mat3x4 ba = g2::Mat3x4::identity(); ba.m[2][3] = 10.0f;
    ref.bones.push_back({"a", 0, ba, 0});

    g2::car::BuildOptions bo;
    bo.baseDir = root.string();
    const auto br = g2::car::build(script, ref, (root / "test.car").string(), bo);

    std::printf("  %d Frames, %zu Sequenzen\n", br.totalFrames(), br.sequences.size());
    check(br.missingFiles.empty(), "alle Dateien aufgeloest");
    check(br.totalFrames() == 19, "5 + 8 + 6 = 19 Frames aneinandergehaengt");
    check(br.sequences.size() == 4, "drei Grabs plus ein -additional");

    check(br.sequences[0].name == "FIRST", "Name aus dem Dateinamen, gross");
    check(br.sequences[0].targetFrame == 0, "erste Sequenz beginnt bei 0");
    check(br.sequences[0].frameCount == 5, "erste Sequenz hat 5 Frames");
    check(br.sequences[0].frameSpeed == 20, "-framespeed uebernommen");

    check(br.sequences[1].name == "SECOND", "zweite Sequenz benannt");
    check(br.sequences[1].targetFrame == 5, "zweite Sequenz beginnt hinter der ersten");
    check(br.sequences[1].loopFrame == 3, "-loop uebernommen");
    check(br.sequences[1].frameSpeed == 30, "eigener framespeed");

    check(br.sequences[2].name == "EXTRA_SEQ", "-additional erzeugt eigene Sequenz");
    check(br.sequences[2].targetFrame == 7, "-additional-Versatz auf den Sequenzanfang addiert");
    check(br.sequences[2].frameCount == 4, "-additional Framezahl");
    check(br.sequences[2].fromAdditional, "als -additional markiert");

    // Vorgaben ohne Flags. Beide an Ravens animation.cfg abgelesen:
    // ohne -loop schreibt Carcass 0 (nicht -1), und ohne -framespeed die
    // Framerate aus SI_Scene der jeweiligen .xsi.
    const auto& sc = br.sequences[3];
    check(sc.name == "SCENED", "dritte Sequenz benannt");
    check(sc.loopFrame == 0, "ohne -loop ist der Loopframe 0, nicht -1");
    check(sc.frameSpeed == 29, "Framerate 29.97 wird abgeschnitten, nicht gerundet");
    check(sc.frameCount == 6, "Framezahl aus SI_Scene (1..6)");

    // -origin aus dem Skript muss auf dem Wurzelbone landen.
    checkNear(br.frames.at(0, 0).m[2][3], -24.0, 1e-4, "-origin aus dem Skript angewandt");

    // Fehlende Datei: harter Abbruch mit brauchbarer Meldung.
    {
        std::ofstream c(root / "bad.car");
        c << "$aseanimgrab models/anims/gibtsnicht.xsi\n";
    }
    bool threw = false;
    try {
        g2::car::build(g2::car::parseFile((root / "bad.car").string()), ref,
                       (root / "bad.car").string(), bo);
    } catch (const std::exception& e) {
        threw = true;
        const std::string msg = e.what();
        check(msg.find("gibtsnicht") != std::string::npos, "Fehlermeldung nennt die Datei");
    }
    check(threw, "fehlende Animationsdatei bricht ab statt still zu verschieben");

    // animation.cfg erzeugen und zurueckpruefen.
    const std::string cfg = g2::car::writeAnimationCfg(br.sequences, "test");
    check(cfg.find("EXTRA_SEQ") != std::string::npos, "animation.cfg enthaelt die Zusatzsequenz");
    check(cfg.find("\r\n") != std::string::npos, "animation.cfg nutzt CRLF");

    { std::error_code ec; fs::remove_all(root, ec); }
}

// Erzwingt Dezimalkomma statt Dezimalpunkt, ohne ein Systemlocale zu
// brauchen. Damit laesst sich der Fall nachstellen, an dem das alte Carcass
// scheiterte: es lief nur mit amerikanischen Regionseinstellungen.
struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
};

void testAnimCache() {
    section("Zwischenspeicher");

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / uniqueTestDir("cache");
    { std::error_code ec; fs::remove_all(root, ec); }
    { std::error_code ec; fs::create_directories(root, ec); }

    const fs::path src = root / "a.xsi";
    const auto write = [&](float lastValue) {
        std::ofstream f(src);
        f << "xsi 0350txt 0032\n"
             "SI_Scene s { \"FRAMES\", 1.000000, 3.000000, 24.000000, }\n"
             "SI_Model MDL-rig.root {\n"
             "  SI_Model MDL-rig.a {\n"
             "    SI_Transform SRT-a { 1.0,1.0,1.0, 0.0,0.0,0.0, 1.5,2.5,3.5, }\n"
             "    SI_FCurve a-ROTATION-Z { \"a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, 3,\n"
             "      1.000000,0.000000, 2.000000,10.000000, 3.000000," << lastValue << ",\n"
             "    }\n  }\n}\n";
    };
    write(20.0f);

    step("Cache anlegen");
    g2::AnimCache cache((root / "cache").string());
    check(cache.enabled(), "Cache ist aktiv");

    step("erster loadOrParse");
    const auto a1 = cache.loadOrParse(src.string());
    check(cache.stats().misses == 1 && cache.stats().hits == 0, "erster Zugriff geht in die Quelle");

    const auto a2 = cache.loadOrParse(src.string());
    check(cache.stats().hits == 1, "zweiter Zugriff kommt aus dem Cache");

    // Der Cacheeintrag muss alles enthalten, was der Parser liefert —
    // sonst wirkt er wie ein stiller Datenverlust.
    check(a2.nodes.size() == a1.nodes.size(), "gleiche Knotenzahl");
    check(a2.frameCount() == a1.frameCount(), "gleicher Framebereich");
    checkNear(a2.frameRate, a1.frameRate, 1e-6, "Framerate erhalten");
    check(a2.hasScene == a1.hasScene, "SI_Scene-Kennzeichen erhalten");
    const int ia = a2.indexOf("a");
    check(ia >= 0, "Knoten gefunden");
    if (ia >= 0) {
        check(a2.nodes[ia].hasSrt, "SRT-Kennzeichen erhalten");
        checkNear(a2.nodes[ia].srt[6], 1.5, 1e-6, "SRT-Werte erhalten");
        checkNear(a2.nodes[ia].srt[8], 3.5, 1e-6, "SRT-Werte erhalten (2)");
        const auto& ch = a2.nodes[ia].channels.at("ROTATION-Z");
        check(ch.size() == 3, "alle Keys erhalten");
        checkNear(ch.at(3), 20.0, 1e-4, "Keywerte erhalten");
        check(a2.nodes[ia].parent == a1.nodes[ia].parent, "Elternbeziehung erhalten");
    }

    // Aendert sich die Quelle, muss der Eintrag verfallen. Sonst liefert der
    // Cache alte Daten und ein Fix wirkt scheinbar nicht.
    step("Quelle aendern");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    write(99.0f);
    const auto a3 = cache.loadOrParse(src.string());
    check(cache.stats().misses == 2, "geaenderte Quelle wird neu gelesen");
    const int ia3 = a3.indexOf("a");
    if (ia3 >= 0) checkNear(a3.nodes[ia3].channels.at("ROTATION-Z").at(3), 99.0, 1e-4,
                            "neuer Wert kommt an, nicht der alte");

    check(cache.sizeOnDisk() > 0, "Cache belegt Platz");
    step("Cache leeren");
    const std::size_t removed = cache.clear();
    check(removed > 0, "Leeren entfernt Eintraege");
    check(cache.sizeOnDisk() == 0, "danach ist der Cache leer");

    // Abgeschalteter Cache darf nichts anlegen.
    g2::AnimCache off("");
    check(!off.enabled(), "leerer Ordner schaltet ab");
    const auto a4 = off.loadOrParse(src.string());
    check(a4.nodes.size() == a1.nodes.size(), "funktioniert auch ohne Cache");

    std::printf("  Eintrag mit %zu Knoten geschrieben und unveraendert zurueckgelesen\n",
                a2.nodes.size());
    step("aufraeumen");
    { std::error_code ec; fs::remove_all(root, ec); }
}

void testEnumTable() {
    section("Enumtabelle aus anims.h");

    // Ravens anims.h ist KEIN gueltiges C: viele Eintraege haben kein Komma.
    // Ein Parser, der eines verlangt, uebersieht sie stillschweigend — bei
    // der echten Datei waren das 102 von 1705 Eintraegen, und in der Folge
    // wurden voellig gueltige Sequenzen als unbekannt gemeldet.
    const std::string src = R"(
#ifndef __ANIMS_H__
typedef enum //# animNumber_e
{
	FACE_TALK0,		//# silent
	FACE_TALK1,		//# quiet
	//# #sep BOTH_ STANDING
	BOTH_STAND1		//# kein Komma, so steht es wirklich in der Datei
	BOTH_STAND1IDLE1	//# ebenfalls ohne
	BOTH_WALK1 = 100,	//# mit Zuweisung
	/* BOTH_AUSKOMMENTIERT, */
	MAX_ANIMATIONS
} animNumber_t;
#endif
)";
    const auto t = g2::anim::parseEnumHeader(src, "test.h");
    check(t.size() == 6, "sechs Eintraege gelesen");
    check(t.contains("FACE_TALK0"), "Eintrag mit Komma");
    check(t.contains("BOTH_STAND1"), "Eintrag OHNE Komma wird erkannt");
    check(t.contains("BOTH_STAND1IDLE1"), "zweiter Eintrag ohne Komma");
    check(t.contains("BOTH_WALK1"), "Eintrag mit Zuweisung");
    check(!t.contains("BOTH_AUSKOMMENTIERT"), "auskommentierter Eintrag zaehlt nicht");
    check(t.indexOf("FACE_TALK0") == 0, "Reihenfolge bleibt erhalten");
    check(t.indexOf("BOTH_STAND1") == 2, "Index nach den beiden ersten");
    check(t.indexOf("gibtsnicht") == -1, "unbekannter Name liefert -1");
    std::printf("  %zu Eintraege, davon %d ohne Komma erkannt\n", t.size(), 2);
}

void testCarValidate() {
    section("Geprueftes Schreiben");
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / uniqueTestDir("write");
        fs::create_directories(dir);

        const std::vector<unsigned char> data{1, 2, 3, 4, 5};
        g2::writeFileChecked((dir / "gut.bin").string(), data.data(), data.size());
        check(fs::file_size(dir / "gut.bin") == 5, "Datei mit erwarteter Groesse geschrieben");

        g2::writeFileChecked((dir / "text.txt").string(), std::string("hallo\n"));
        check(fs::file_size(dir / "text.txt") == 6, "Text geschrieben");

        g2::writeFileChecked((dir / "leer.bin").string(), nullptr, 0);
        check(fs::exists(dir / "leer.bin"), "leere Datei angelegt");

        // Der entscheidende Fall: ein Pfad, der sich nicht oeffnen laesst,
        // MUSS auffliegen statt stillschweigend nichts zu tun. Genau das war
        // vorher an drei Stellen der Fall.
        {
            const std::string blocker = (dir / "block").string();
            g2::writeFileChecked(blocker, std::string("x"));
            bool threw = false;
            try {
                g2::writeFileChecked(blocker + "/tief.bin", std::string("x"));
            } catch (const std::exception&) {
                threw = true;
            }
            check(threw, "unbeschreibbarer Pfad wirft eine Ausnahme");
        }

        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    section("Skriptpruefung");

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / uniqueTestDir("validate");
    { std::error_code ec; fs::remove_all(root, ec); }
    { std::error_code ec; fs::create_directories(root / "anims", ec); }

    {
        std::ofstream f(root / "anims" / "a.xsi");
        f << "xsi 0350txt 0032\n"
             "SI_Scene s { \"FRAMES\", 1.000000, 10.000000, 20.000000, }\n"
             "SI_Model MDL-rig.root {\n  SI_Model MDL-rig.a {\n"
             "    SI_FCurve a-ROTATION-Z { \"a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, 2,\n"
             "      1.000000,0.000000, 10.000000,90.000000,\n    }\n  }\n}\n";
    }
    {
        std::ofstream c(root / "bad.car");
        c << "$aseanimgrabinit\n"
             "$aseanimgrab anims/a.xsi -enum BOTH_STAND1\n"
             "$aseanimgrab anims/a.xsi -enum BOTH_STAND1\n"          // doppelt
             "$aseanimgrab anims/a.xsi -enum BOTH_UNBEKANNT\n"       // nicht in der Tabelle
             "$aseanimgrab anims/a.xsi -enum BOTH_WALK1 -additional 0 5 9 20 BOTH_RUN1\n"
             "$aseanimgrab anims/fehlt.xsi -enum BOTH_RUN2\n"        // fehlt
             "$aseanimgrabfinalize\n"
             "$aseanimconvertmdx_noask r -makeskel models/x/_humanoid -origin 0 0 24\n";
    }

    const auto script = g2::car::parseFile((root / "bad.car").string());
    const std::string enums =
        "typedef enum {\n BOTH_STAND1,\n BOTH_WALK1,\n BOTH_RUN1,\n BOTH_RUN2,\n"
        " BOTH_UNGENUTZT,\n MAX_ANIMATIONS\n} a;\n";
    const auto table = g2::anim::parseEnumHeader(enums, "test.h");

    g2::car::ValidateOptions vo;
    vo.enums = &table;
    vo.baseDir = root.string();
    vo.readFrameCounts = true;

    step("validate bad.car");
    const auto r = g2::car::validate(script, (root / "bad.car").string(), vo);
    std::printf("  %zu Grabs, %zu Sequenzen, %zu Fehler, %zu Warnungen\n", r.grabs, r.sequences,
                r.errors, r.warnings);

    const auto has = [&](g2::car::Issue::Level lvl, const std::string& needle) {
        for (const auto& i : r.issues)
            if (i.level == lvl &&
                (i.message.find(needle) != std::string::npos ||
                 i.sequence.find(needle) != std::string::npos))
                return true;
        return false;
    };

    check(!r.ok(), "fehlerhaftes Skript wird nicht durchgewunken");
    check(has(g2::car::Issue::Level::Error, "fehlt.xsi"), "fehlende Datei erkannt");
    check(has(g2::car::Issue::Level::Error, "BOTH_STAND1"), "doppelter Sequenzname erkannt");
    check(has(g2::car::Issue::Level::Warning, "BOTH_UNBEKANNT"),
          "unbekanntes Enum ist eine WARNUNG, kein Fehler");
    check(has(g2::car::Issue::Level::Error, "Loopframe"), "Loopframe ausserhalb erkannt");
    check(has(g2::car::Issue::Level::Info, "keine Sequenz"),
          "Gegenrichtung gemeldet: Enum ohne Sequenz");

    // Ein sauberes Skript darf keine Fehler erzeugen.
    step("validate good.car");
    {
        std::ofstream c(root / "good.car");
        c << "$aseanimgrabinit\n"
             "$aseanimgrab anims/a.xsi -enum BOTH_STAND1\n"
             "$aseanimgrab anims/a.xsi -enum BOTH_WALK1 -additional 0 5 -1 20 BOTH_RUN1\n"
             "$aseanimgrabfinalize\n"
             "$aseanimconvertmdx_noask r -makeskel models/x/_humanoid\n";
        c.close();
        const auto ok = g2::car::validate(g2::car::parseFile((root / "good.car").string()),
                                          (root / "good.car").string(), vo);
        check(ok.ok(), "sauberes Skript passiert ohne Fehler");
    }

    // Schreiben und wieder einlesen: die Bedeutung muss erhalten bleiben.
    step("writeScript roundtrip");
    {
        const std::string text = g2::car::writeScript(script);
        g2::car::ParseOptions po;
        po.followIncludes = false;
        const auto again = g2::car::parse(text, "roundtrip.car", po);
        check(again.grabs.size() == script.grabs.size(), "gleiche Zahl Grabs nach dem Schreiben");
        check(again.convert.has_value() && script.convert.has_value(),
              "Konvertierungsanweisung erhalten");
        if (again.convert && script.convert)
            check(again.convert->makeSkel == script.convert->makeSkel, "-makeskel erhalten");
        bool sameNames = true;
        for (std::size_t i = 0; i < again.grabs.size() && i < script.grabs.size(); ++i) {
            if (again.grabs[i].file != script.grabs[i].file) sameNames = false;
            if (again.grabs[i].additional.size() != script.grabs[i].additional.size())
                sameNames = false;
        }
        check(sameNames, "Dateinamen und -additional erhalten");
    }

    // Verzeichnis durchsuchen.
    step("scanDirectory");
    {
        const auto found = g2::car::scanDirectory(root.string());
        check(found.size() == 2, "beide .car gefunden");
        bool haveGrabs = true;
        for (const auto& f : found) if (f.grabs == 0) haveGrabs = false;
        check(haveGrabs, "Grabzahl je Datei ermittelt");

        // In Unterordnern ebenfalls finden.
        std::error_code ec2;
        fs::create_directories(root / "tief" / "tiefer", ec2);
        { std::ofstream c(root / "tief" / "tiefer" / "d.car"); c << "$aseanimgrabinit\n"; }
        const auto deep = g2::car::scanDirectory(root.string());
        check(deep.size() == 3, "auch in Unterordnern gefunden");

        // Tiefenbegrenzung greift.
        const auto shallow = g2::car::scanDirectory(root.string(), 0);
        check(shallow.size() == 2, "Tiefenbegrenzung 0 bleibt im Wurzelordner");

        // Eine Verzeichnisverknuepfung auf einen Vorfahren darf die Suche
        // NICHT in eine Endlosschleife schicken. Genau daran ist die
        // vorherige Fassung mit recursive_directory_iterator unter Windows
        // haengengeblieben.
        std::error_code lec;
        fs::create_directory_symlink(root, root / "tief" / "schleife", lec);
        if (!lec) {
            step("scanDirectory mit Schleife");
            const auto looped = g2::car::scanDirectory(root.string());
            check(looped.size() == 3, "Schleife bricht die Suche nicht");
        }
    }

    step("aufraeumen");
    { std::error_code ec; fs::remove_all(root, ec); }
}

void testLocaleIndependence() {
    section("Unabhaengigkeit von den Regionseinstellungen");

    const std::locale saved = std::locale();
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));

    // Gegenprobe: das Locale ist wirklich aktiv.
    {
        std::ostringstream os;
        os.imbue(std::locale());
        os << 1.5;
        check(os.str() == "1,5", "Testlocale schreibt tatsaechlich mit Komma");
    }

    bool threw = false;
    try {
        const std::string src = R"(xsi 0350txt 0032
SI_Scene s { "FRAMES", 1.000000, 3.000000, 29.970030, }
SI_Model MDL-rig.root {
    SI_Model MDL-rig.a {
        SI_Transform SRT-a { 1.0,1.0,1.0, 0.0,0.0,0.0, 0.5,0.25,0.125, }
        SI_FCurve a-ROTATION-Z { "a", "ROTATION-Z", "LINEAR", 1, 1, 3,
            1.000000,0.000000,
            2.000000,45.500000,
            3.000000,90.250000,
        }
    }
}
)";
        const auto doc = g2::xsi::parse(src);
        const auto anim = g2::xsi::loadAnimation(doc, "locale.xsi");

        checkNear(anim.frameRate, 29.97, 1e-4, "Framerate mit Punkt gelesen");
        check(anim.frameCount() == 3, "Framebereich gelesen");

        const int ia = anim.indexOf("a");
        check(ia >= 0, "Knoten gefunden");
        if (ia >= 0) {
            const auto& ch = anim.nodes[ia].channels.at("ROTATION-Z");
            check(ch.size() == 3, "alle Keys gelesen");
            checkNear(ch.at(2), 45.5, 1e-4, "Nachkommastelle korrekt");
            checkNear(ch.at(3), 90.25, 1e-4, "zwei Nachkommastellen korrekt");
            checkNear(anim.nodes[ia].srt[6], 0.5, 1e-6, "SRT-Wert korrekt");
            checkNear(anim.nodes[ia].srt[8], 0.125, 1e-6, "SRT-Wert mit drei Stellen");
        }

        // Und dasselbe fuer das .car-Skript.
        g2::car::ParseOptions po;
        po.followIncludes = false;
        const auto sc = g2::car::parse("$scale 0.64\n$origin 0 0 24.5\n", "l.car", po);
        check(sc.scale.has_value(), "$scale gelesen");
        if (sc.scale) checkNear(*sc.scale, 0.64, 1e-9, "$scale 0.64 trotz Dezimalkomma-Locale");
        check(sc.origin.has_value(), "$origin gelesen");
        if (sc.origin) checkNear((*sc.origin)[2], 24.5, 1e-9, "$origin mit Nachkommastelle");
    } catch (const std::exception& e) {
        threw = true;
        std::printf("  Ausnahme: %s\n", e.what());
    }
    check(!threw, "kein Fehler unter fremdem Locale");

    std::locale::global(saved);
    std::printf("  alle Zahlen unter Dezimalkomma-Locale korrekt gelesen\n");
}

}  // namespace

// Rueckweg: GLA -> dotXSI -> GLA.
//
// Das ist der Ablauf, um den es dem Nutzer geht: eine Animation aus einer
// fremden GLA herausloesen, mit eigenen Dateien mischen und neu bauen.
void testXsiExport() {
    std::cout << "== Export GLA -> dotXSI ==\n";

    // Ein kleines Skelett und eine Animation von Hand bauen, damit der Test
    // ohne die grossen Originaldateien laeuft.
    step("Skelett anlegen");
    g2::Skeleton sk;
    sk.name = "t";
    sk.scale = 0.64f;
    for (int i = 0; i < 4; ++i) {
        g2::Bone b;
        b.name = "b" + std::to_string(i);
        b.parent = i == 0 ? -1 : i - 1;
        b.basePose = g2::Mat3x4::identity();
        // Bindposen mit Versatz UND Skalierung: eine reine Identitaet wuerde
        // Fehler in beiden verstecken.
        b.basePose.m[0][3] = static_cast<float>(i) * 3.0f;
        for (int r = 0; r < 3; ++r) b.basePose.m[r][r] = 0.64f;
        sk.bones.push_back(b);
    }

    // Ein Bone mit dem Elternbone HINTER sich - genau der Fall, an dem die
    // Indexreihenfolge scheitert.
    {
        g2::Bone b;
        b.name = "spaet";
        b.parent = 4;   // zeigt auf den letzten, der noch kommt
        b.basePose = g2::Mat3x4::identity();
        for (int r = 0; r < 3; ++r) b.basePose.m[r][r] = 0.64f;
        sk.bones.insert(sk.bones.begin() + 1, b);
        for (auto& x : sk.bones)
            if (x.parent >= 1 && x.name != "spaet") x.parent += 1;
        sk.bones[0].parent = -1;
        sk.bones[1].parent = 4;
    }

    step("Animation erzeugen");
    const int frames = 8;
    const int nb = static_cast<int>(sk.bones.size());
    g2::AnimationFrames af;
    af.numBones = nb;
    af.matrices.resize(static_cast<std::size_t>(frames * nb));
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < nb; ++b) {
            g2::Mat3x4 m = g2::Mat3x4::identity();
            const float a = 0.05f * static_cast<float>(f + b);
            m.m[0][0] = std::cos(a);
            m.m[0][1] = -std::sin(a);
            m.m[1][0] = std::sin(a);
            m.m[1][1] = std::cos(a);
            m.m[0][3] = static_cast<float>(f) * 0.5f;
            af.at(f, b) = m;
        }

    const auto written = g2::writeMdxa(sk, af);
    const g2::MdxaFile gla = g2::readMdxa(written.data);
    check(gla.numFrames == frames, "GLA gebaut und gelesen");

    step("exportieren");
    g2::xsiexp::ExportOptions eo;
    eo.scale = sk.scale;
    const std::string text = g2::xsiexp::exportSequence(gla, {"seq", 0, frames}, eo);
    // Vorgabe ist jetzt v3.0 — benannte Templates wie in Ravens root.xsi.
    check(text.find("xsi 0300txt") == 0, "dotXSI-Kopf geschrieben");
    check(text.find("SI_Scene") != std::string::npos, "SI_Scene vorhanden");

    // SI_FCurve MUSS direktes Kind von SI_Model sein. Steckt es in einem
    // SI_Animation-Block, findet der Importeur es nicht - dann bleibt jeder
    // Bone auf Frame 0 stehen, und ausgerechnet Frame 0 stimmt.
    check(text.find("SI_Animation") == std::string::npos,
          "keine SI_Animation-Verschachtelung um die Kurven");

    step("zurueckrechnen");
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / uniqueTestDir("export");
    std::filesystem::create_directories(tmp);
    const std::filesystem::path xf = tmp / "seq.xsi";
    { std::ofstream o(xf, std::ios::binary); o << text; }

    const auto anim = g2::xsi::loadAnimation(g2::xsi::parseFile(xf.string()), xf.string());
    check(static_cast<int>(anim.nodes.size()) == nb, "alle Bones in der Datei");

    g2::xsi::EvalOptions vo;
    vo.scale = sk.scale;
    const auto res = g2::xsi::evaluate(sk, anim, vo);
    check(res.missingBones.empty(), "kein Bone beim Rueckimport verloren");

    double worst = 0.0;
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < nb; ++b) {
            const auto A = gla.boneMatrix(f, b);
            const auto B = res.frames.at(f, b);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    worst = std::max(worst, static_cast<double>(std::fabs(A.m[r][c] - B.m[r][c])));
        }
    std::cout << "  groesster Rundlauffehler: " << worst << " (1 Quantisierungsstufe = 0.015625)\n";
    check(worst < 0.05, "Rundlauf innerhalb zweier Quantisierungsstufen");

    step("$keepmotion beachten");
    {
        // Wurde gelesen, aber nie beachtet: bei einem Skript mit
        // $keepmotion wurde die Bewegung trotzdem herausgerechnet.
        const std::filesystem::path kr = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("keepmotion");
        std::filesystem::create_directories(kr / "models");

        // Eine Animation, in der sich der Motion-Bone bewegt.
        {
            std::ofstream x(kr / "models" / "m.xsi", std::ios::binary);
            x << "xsi 0350txt 0032\n\nSI_Model MDL-model_root {\n"
                 "  SI_Transform SRT-model_root {\n    1.0,\n    1.0,\n    1.0,\n"
                 "    0.0,\n    0.0,\n    0.0,\n    0.0,\n    0.0,\n    0.0,\n  }\n"
                 "  SI_Model MDL-Motion {\n"
                 "    SI_Transform SRT-Motion {\n      1.0,\n      1.0,\n      1.0,\n"
                 "      0.0,\n      0.0,\n      0.0,\n      0.0,\n      0.0,\n      0.0,\n    }\n"
                 "    SI_FCurve Motion-TRANSLATION-X {\n"
                 "      \"Motion\",\n      \"TRANSLATION-X\",\n      \"LINEAR\",\n"
                 "      1,\n      1,\n      3,\n      0, 0.0,\n      1, 50.0,\n      2, 100.0,\n"
                 "    }\n  }\n}\n";
        }

        g2::Skeleton ks;
        ks.name = "k";
        ks.scale = 0.64f;
        for (const char* n : {"model_root", "Motion"}) {
            g2::Bone b;
            b.name = n;
            b.parent = (std::string(n) == "model_root") ? -1 : 0;
            b.basePose = g2::Mat3x4::identity();
            for (int r = 0; r < 3; ++r) b.basePose.m[r][r] = 0.64f;
            ks.bones.push_back(b);
        }

        const auto runWith = [&](bool keep) {
            std::ofstream c(kr / "t.car");
            c << "$aseanimgrabinit\n";
            if (keep) c << "$keepmotion\n";
            c << "$aseanimgrab models/m.xsi -enum BOTH_STAND1\n$aseanimgrabfinalize\n";
            c.close();
            const auto sc2 = g2::car::parseFile((kr / "t.car").string());
            g2::car::BuildOptions bo2;
            bo2.baseDir = kr.string();
            const auto br2 = g2::car::build(sc2, ks, (kr / "t.car").string(), bo2);
            // Verschiebung des Wurzelbones ueber die Sequenz.
            const auto a = br2.frames.at(0, 0);
            const auto b = br2.frames.at(br2.frames.frameCount() - 1, 0);
            return std::fabs(b.m[0][3] - a.m[0][3]);
        };

        const float ohne = runWith(false);
        const float mit = runWith(true);
        std::cout << "  Wurzelversatz ohne $keepmotion: " << ohne << ", mit: " << mit << "\n";

        // Ohne $keepmotion legt Carcass eine Gegenrampe auf den Wurzelbone.
        check(ohne > 1.0f, "ohne $keepmotion wird die Bewegung herausgerechnet");
        // Mit $keepmotion bleibt der Wurzelbone stehen.
        check(mit < 0.01f, "mit $keepmotion bleibt sie in der Animation");

        std::filesystem::remove_all(kr);
    }

    step("Wurzelbewegung aus der GLA holen");
    {
        // Die .frames-Datei ist dafuer nicht noetig: Carcass rechnet die
        // Bewegung als lineare Rampe auf den Wurzelbone, und die steht
        // weiterhin in der GLA.
        //
        // Hier wird eine GLA mit bekannter Rampe gebaut und geprueft, dass
        // genau sie zurueckkommt.
        g2::Skeleton s3;
        s3.name = "t3";
        s3.scale = 0.64f;
        for (int i = 0; i < 2; ++i) {
            g2::Bone b;
            b.name = i == 0 ? "model_root" : "Motion";
            b.parent = i == 0 ? -1 : 0;
            b.basePose = g2::Mat3x4::identity();
            s3.bones.push_back(b);
        }
        const int fr3 = 11;   // 10 Schritte
        g2::AnimationFrames af3;
        af3.numBones = 2;
        af3.matrices.resize(static_cast<std::size_t>(fr3 * 2));
        for (int f = 0; f < fr3; ++f)
            for (int b = 0; b < 2; ++b) {
                g2::Mat3x4 m = g2::Mat3x4::identity();
                // Nur der Wurzelbone traegt die Rampe: -35 ueber 10 Schritte.
                if (b == 0) m.m[0][3] = -3.5f * static_cast<float>(f);
                af3.at(f, b) = m;
            }
        const auto w3 = g2::writeMdxa(s3, af3);
        const g2::MdxaFile g3 = g2::readMdxa(w3.data);

        const auto rm = g2::xsiexp::detectRootMotion(g3, {"s", 0, fr3});
        check(rm.has_value(), "Bewegung erkannt");
        if (rm) {
            // averagevec = -Rampe / Schritte = 35/10 = 3.5
            check(std::fabs((*rm)[0] - 3.5f) < 0.05f, "Betrag stimmt");
            check(std::fabs((*rm)[1]) < 0.05f, "keine Bewegung in Y");
            check(std::fabs((*rm)[2]) < 0.05f, "keine Bewegung in Z");
        }

        // Ohne Bewegung darf nichts gemeldet werden — sonst wuerde beim
        // Export etwas eingesetzt, das es nie gab.
        g2::AnimationFrames af4 = af3;
        for (int f = 0; f < fr3; ++f) af4.at(f, 0) = g2::Mat3x4::identity();
        const auto w4 = g2::writeMdxa(s3, af4);
        const g2::MdxaFile g4 = g2::readMdxa(w4.data);
        check(!g2::xsiexp::detectRootMotion(g4, {"s", 0, fr3}).has_value(),
              "ohne Bewegung wird nichts erfunden");

        // Unsinnige Bereiche duerfen nicht abstuerzen.
        check(!g2::xsiexp::detectRootMotion(g3, {"s", 0, 1}).has_value(), "ein Frame");
        check(!g2::xsiexp::detectRootMotion(g3, {"s", -3, 5}).has_value(), "negativer Start");
        check(!g2::xsiexp::detectRootMotion(g3, {"s", 0, 999}).has_value(), "zu langer Bereich");
    }

    step("Unterbereiche zusammenfassen");
    {
        // Der Kern des Rueckwegs: exportierte man jede Sequenz einzeln,
        // haette die neue GLA mehr Frames als die alte und die
        // animation.cfg passte nicht mehr.
        using g2::xsiexp::CfgSequence;
        const std::vector<CfgSequence> cfg = {
            {"MASTER_A", 0, 10, -1, 20},
            {"TEIL_A1", 0, 3, -1, 30},     // liegt in MASTER_A
            {"TEIL_A2", 5, 2, 0, 10},      // liegt ebenfalls darin
            {"MASTER_B", 10, 5, 0, 20},
            {"ALIAS_B", 10, 5, 0, 20},     // gleicher Bereich -> Unterbereich
            {"MASTER_C", 15, 4, -1, 20},
        };
        const auto g = g2::xsiexp::groupSequences(cfg);
        check(g.masters.size() == 3, "drei Master erkannt");
        check(g.partial == 0, "nichts teilweise ueberlappend");

        // Entscheidend: die Master duerfen sich nicht ueberschneiden und
        // muessen alles abdecken. Sonst stimmt die Framezahl nicht.
        int covered = 0;
        int last = -1;
        bool disjoint = true;
        for (const auto& m : g.masters) {
            if (m.self.start < last) disjoint = false;
            last = m.self.start + m.self.count;
            covered += m.self.count;
        }
        check(disjoint, "Master ueberschneiden sich nicht");
        check(covered == 19, "Master decken alle 19 Frames ab");

        std::size_t inside = 0;
        for (const auto& m : g.masters) inside += m.inside.size();
        check(inside == 3, "drei Unterbereiche zugeordnet");

        // Das Skript muss die Unterbereiche als -additional mit dem
        // richtigen Versatz tragen.
        const auto sc = g2::xsiexp::buildScript(g, "models/x/", std::nullopt);
        check(sc.grabs.size() == 3, "drei Grabs");
        check(sc.grabs[0].enumName && *sc.grabs[0].enumName == "MASTER_A", "Enum gesetzt");
        check(sc.grabs[0].frameSpeed && *sc.grabs[0].frameSpeed == 20, "Rate aus der cfg");
        check(sc.grabs[0].additional.size() == 2, "zwei Unterbereiche am ersten Grab");
        if (sc.grabs[0].additional.size() == 2) {
            check(sc.grabs[0].additional[1].targetOffset == 5, "Versatz relativ zum Master");
            check(sc.grabs[0].additional[1].frameSpeed == 10, "Rate des Unterbereichs");
            check(sc.grabs[0].additional[1].loopFrame == 0, "Loopframe des Unterbereichs");
        }

        // Die letzte Zeile sagt, WO die GLA entsteht und wie sie heisst.
        // Der Wert steht exakt im Kopf der Quell-GLA — wird er abgeleitet
        // statt uebernommen, landet die neue GLA unter falschem Namen und
        // das Modell findet sie nicht.
        const auto scMs = g2::xsiexp::buildScript(g, "models/players/j/", std::nullopt, 0.64f,
                                                  false, "models/players/_humanoid/_humanoid");
        check(scMs.convert && scMs.convert->makeSkel == "models/players/_humanoid/_humanoid",
              "Skelettpfad wird uebernommen, nicht abgeleitet");
        check(scMs.convert && scMs.convert->root == "models/players/j/root",
              "root zeigt dorthin, wo die Dateien liegen");

        // Ohne Angabe wird abgeleitet — und darf keinen fuehrenden
        // Schraegstrich bekommen, sonst ist es ein absoluter Pfad.
        const auto scNo = g2::xsiexp::buildScript(g, "", std::nullopt);
        check(scNo.convert && scNo.convert->makeSkel == "_humanoid",
              "ohne Praefix kein fuehrender Schraegstrich");

        // Und der Versatz muss im Skript landen, sonst steht das Modell
        // beim Neubauen um diesen Betrag daneben.
        const auto sc2 = g2::xsiexp::buildScript(g, "models/x/",
                                                 std::array<float, 3>{0.0f, 0.0f, 24.0f});
        check(sc2.convert.has_value(), "Konvertierungsanweisung vorhanden");
        check(sc2.convert && sc2.convert->origin.has_value(), "origin steht im Skript");

        // Ein Bereich, der ueber das Ende hinausragt, darf nicht still
        // verbogen werden.
        const auto g2p = g2::xsiexp::groupSequences({{"A", 0, 10, -1, 20}, {"B", 5, 10, -1, 20}});
        check(g2p.partial == 1, "teilweise ueberlappender Bereich wird gemeldet");
    }

    step("animation.cfg heisst wie die Engine sie sucht");
    {
        // Sie hiess frueher "<name>_animation.cfg". Das ueberschreibt nichts
        // und liegt harmlos daneben — aber die Engine sucht "animation.cfg".
        // Wer den Ausgabeordner ins Spiel kopiert, hat weiterhin die alte,
        // und weil sich beim Einfuegen von Animationen ALLE nachfolgenden
        // Zielframes verschieben, laeuft dann bei jedem Namen die Animation,
        // die dort zufaellig steht.
        //
        // Das sah wie ein kaputtes Werkzeug aus und war eine vergessene
        // Datei.
        const std::filesystem::path cd = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("cfgname");
        std::filesystem::create_directories(cd);

        std::vector<g2::car::Sequence> sq;
        sq.push_back({"BOTH_STAND1", 0, 2, -1, 20, "a.xsi", false, false});
        const std::string text = g2::car::writeAnimationCfg(sq, "2 frames");

        // Der Name muss genau so lauten.
        const std::filesystem::path erwartet = cd / "animation.cfg";
        { std::ofstream o(erwartet, std::ios::binary); o << text; }
        check(std::filesystem::exists(erwartet), "heisst animation.cfg");

        // Und wieder eingelesen muss dieselbe Sequenz herauskommen.
        std::ifstream in(erwartet);
        std::string line, gefunden;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '/') continue;
            std::istringstream is(line);
            std::string nm;
            int a = 0, b = 0, c = 0, d = 0;
            if (is >> nm >> a >> b >> c >> d) gefunden = nm;
        }
        check(gefunden == "BOTH_STAND1", "Sequenz wieder lesbar");

        std::filesystem::remove_all(cd);
    }

    step("Fehlende Dateien mit Zuordnung melden");
    {
        // "Eine Datei fehlt" hilft bei 1393 Grabs niemandem. Gebraucht wird,
        // WELCHE Sequenz betroffen ist und wo die Datei erwartet wurde.
        const std::filesystem::path mr = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("missing");
        std::filesystem::create_directories(mr / "models" / "x");
        // Eine Datei muss lesbar sein: fehlen ALLE, bricht build mit einer
        // anderen Meldung ab, und der Fall waere hier nicht geprueft.
        {
            std::ofstream x(mr / "models" / "x" / "da.xsi", std::ios::binary);
            x << "xsi 0350txt 0032\n\nSI_Model MDL-model_root {\n"
                 "  SI_Transform SRT-model_root {\n"
                 "    1.0,\n    1.0,\n    1.0,\n    0.0,\n    0.0,\n    0.0,\n"
                 "    0.0,\n    0.0,\n    0.0,\n  }\n"
                 "  SI_FCurve model_root-TRANSLATION-X {\n"
                 "    \"model_root\",\n    \"TRANSLATION-X\",\n    \"LINEAR\",\n"
                 "    1,\n    1,\n    2,\n    0, 0.0,\n    1, 0.0,\n  }\n}\n";
        }
        {
            std::ofstream c(mr / "t.car");
            c << "$aseanimgrabinit\n"
              << "$aseanimgrab models/x/weg.xsi -enum BOTH_ATTACK1\n"
              << "$aseanimgrab models/x/auchweg.xsi\n"
              << "$aseanimgrab models/x/da.xsi -enum ROOT\n"
              << "$aseanimgrabfinalize\n";
        }
        const auto scr = g2::car::parseFile((mr / "t.car").string());

        g2::car::BuildOptions bo;
        bo.baseDir = mr.string();
        bo.skipMissing = true;   // damit wir das Ergebnis bekommen statt einer Ausnahme

        g2::Skeleton sk2;
        sk2.name = "t";
        sk2.scale = 0.64f;
        g2::Bone rb;
        rb.name = "model_root";
        rb.basePose = g2::Mat3x4::identity();
        sk2.bones.push_back(rb);

        const auto br = g2::car::build(scr, sk2, (mr / "t.car").string(), bo);
        check(br.missing.size() == 2, "beide fehlenden erfasst");
        if (br.missing.size() == 2) {
            check(br.missing[0].sequence == "BOTH_ATTACK1", "Sequenzname aus -enum");
            // Ohne -enum wird der Name aus dem Dateinamen abgeleitet.
            check(br.missing[1].sequence == "AUCHWEG", "Sequenzname aus dem Dateinamen");
            check(br.missing[0].line == 2, "Zeile in der .car mitgeliefert");
            check(br.missing[1].line == 3, "auch fuer die zweite");
            check(br.missing[0].file == "models/x/weg.xsi", "Pfad wie in der .car");
        }
        // Die alte Liste muss weiterhin gefuellt sein.
        check(br.missingFiles.size() == 2, "Pfadliste bleibt erhalten");

        std::filesystem::remove_all(mr);
    }

    step("parallelFor: Threadnummer und Blockvergabe");
    {
        // 1) Jedes Element genau einmal.
        for (unsigned th : {1u, 2u, 3u, 8u, 64u}) {
            const std::size_t n = 1000;
            std::vector<std::atomic<int>> seen(n);
            for (auto& x : seen) x.store(0);
            g2::parallelFor(n, [&](std::size_t i) { seen[i].fetch_add(1); }, th);
            int falsch = 0;
            for (const auto& x : seen)
                if (x.load() != 1) ++falsch;
            check(falsch == 0, "jedes Element genau einmal bei " + std::to_string(th) +
                                   " Threads");
        }

        // 2) Die Threadnummer bleibt im gueltigen Bereich.
        //
        // Genau hier lag der Fehler: die Nummer stand frueher in einem
        // thread_local und ueberlebte den Aufruf. Beim zweiten Durchlauf mit
        // WENIGER Threads zeigte sie hinter das Ende des Zaehlerfeldes.
        for (unsigned th : {8u, 4u, 1u, 2u}) {
            std::atomic<int> zuGross{0};
            g2::parallelForWorker(
                500,
                [&](std::size_t, unsigned w) {
                    if (w >= th) zuGross.fetch_add(1);
                },
                th);
            check(zuGross.load() == 0,
                  "Threadnummer unter " + std::to_string(th) + " (nach vorherigem Lauf)");
        }

        // 3) Je Thread ein eigener Zaehler, ohne Sperre — die Summe muss
        //    stimmen.
        {
            const unsigned th = 4;
            std::vector<long> perWorker(th, 0);
            g2::parallelForWorker(
                10000, [&](std::size_t i, unsigned w) { perWorker[w] += static_cast<long>(i); },
                th);
            long sum = 0;
            for (const long v : perWorker) sum += v;
            check(sum == 49995000L, "Summe ueber getrennte Zaehler stimmt");
        }

        // 4) Eine Ausnahme kommt beim Aufrufer an und reisst nichts mit.
        {
            bool geworfen = false;
            try {
                g2::parallelFor(
                    100,
                    [&](std::size_t i) {
                        if (i == 50) throw std::runtime_error("Testfehler");
                    },
                    4);
            } catch (const std::exception& e) {
                geworfen = std::string(e.what()) == "Testfehler";
            }
            check(geworfen, "Ausnahme wird weitergereicht");
        }

        // 5) Randfaelle duerfen nicht abstuerzen.
        check((g2::parallelFor(0, [](std::size_t) {}, 8), true), "leerer Bereich");
        check((g2::parallelFor(1, [](std::size_t) {}, 8), true), "ein Element, acht Threads");
    }

    step("Auskunft ueber die Laufzeit");
    {
        // Die Frage, ob das Programm auf einem fremden Rechner startet,
        // soll sich ohne externe Werkzeuge beantworten lassen.
        //
        // Auf Linux ist die Antwort immer "ja"; die Pruefung stellt hier
        // sicher, dass die Funktion ueberhaupt auswertbar ist und zur
        // Uebersetzungszeit feststeht.
        static_assert(g2::staticRuntime() || !g2::staticRuntime(),
                      "staticRuntime muss zur Uebersetzungszeit feststehen");
        check(g2::staticRuntime() == g2::staticRuntime(), "Auskunft ist stabil");
    }

    step("Bonenamen ohne Ruecksicht auf Gross-/Kleinschreibung");
    {
        // Ravens Modelle schreiben den Bewegungsbone "Motion", eigene oft
        // "motion" — beim SBD-Humanoid ist genau das der Fall. Wird nur
        // genau verglichen, findet die Rampenberechnung ihren Bone nicht,
        // die Wurzelbewegung bleibt drin, und die Figur rutscht im Spiel
        // beim Laufen davon.
        g2::xsi::AnimFile a;
        g2::xsi::AnimNode n1;
        n1.name = "motion";
        a.nodes.push_back(n1);
        g2::xsi::AnimNode n2;
        n2.name = "LHand";
        a.nodes.push_back(n2);

        check(a.indexOf("motion") == 0, "genaue Schreibweise gefunden");
        check(a.indexOf("Motion") == 0, "andere Schreibweise gefunden");
        check(a.indexOf("lhand") == 1, "auch andersherum");
        check(a.indexOf("gibtsnicht") < 0, "unbekannter Name bleibt unbekannt");

        // Bei zwei Bones, die sich nur in der Schreibweise unterscheiden,
        // muss der exakte gewinnen.
        g2::xsi::AnimFile b;
        g2::xsi::AnimNode m1;
        m1.name = "motion";
        b.nodes.push_back(m1);
        g2::xsi::AnimNode m2;
        m2.name = "Motion";
        b.nodes.push_back(m2);
        check(b.indexOf("Motion") == 1, "genaue Uebereinstimmung hat Vorrang");
    }

    step("Rahmen um die Grabs");
    {
        // writeScript gibt die Grabs NUR nach einem $aseanimgrabinit aus.
        // Ohne addGrabFrame sieht die geschriebene Datei vollstaendig aus
        // und enthaelt keine einzige Sequenz — ein Fehler, den weder der
        // Uebersetzer noch ein Blick auf die Datei zeigt.
        g2::car::Script leer;
        g2::car::GrabDirective gd;
        gd.file = "models/x/a.xsi";
        leer.grabs.push_back(gd);

        const std::string ohne = g2::car::writeScript(leer);
        check(ohne.find("$aseanimgrab ") == std::string::npos,
              "ohne Rahmen geht der Grab verloren (belegt das Problem)");

        g2::car::addGrabFrame(leer);
        const std::string mit = g2::car::writeScript(leer);
        check(mit.find("$aseanimgrabinit") != std::string::npos, "init geschrieben");
        check(mit.find("$aseanimgrab models/x/a.xsi") != std::string::npos, "Grab geschrieben");
        check(mit.find("$aseanimgrabfinalize") != std::string::npos, "finalize geschrieben");

        // Zweimal aufrufen darf nichts verdoppeln.
        g2::car::addGrabFrame(leer);
        const std::string zwei = g2::car::writeScript(leer);
        check(zwei == mit, "zweiter Aufruf aendert nichts");

        // Und wieder einlesen muss den Grab zurueckgeben.
        const auto zurueck = g2::car::parse(mit, "test.car");
        check(zurueck.grabs.size() == 1, "wieder eingelesen: ein Grab");
    }

    step("Scherung: naechstgelegene Rotation");
    {
        // Eine Matrix mit ungleichen Achsenlaengen UND leichter Schiefe.
        // Genau so sehen die lokalen Transformationen aus, wenn Bindposen
        // mit verschiedenen Achsenlaengen hintereinandergeschaltet werden —
        // in JK2s _humanoid.gla haengen 46 der 72 Bones direkt am
        // Brustkorb, und dort kommt das haeufig vor.
        g2::Skeleton s2;
        s2.name = "t2";
        s2.scale = 0.64f;
        for (int i = 0; i < 3; ++i) {
            g2::Bone b;
            b.name = "s" + std::to_string(i);
            b.parent = i == 0 ? -1 : 0;   // flach: alles am ersten Bone
            b.basePose = g2::Mat3x4::identity();
            // Ungleiche Achsenlaengen, aber rechtwinklig — wie bei Raven.
            b.basePose.m[0][0] = 0.64f;
            b.basePose.m[1][1] = 0.696f;
            b.basePose.m[2][2] = 0.64f;
            b.basePose.m[0][3] = static_cast<float>(i) * 30.0f;   // langer Hebel
            s2.bones.push_back(b);
        }
        const int nb2 = static_cast<int>(s2.bones.size());
        g2::AnimationFrames af2;
        af2.numBones = nb2;
        af2.matrices.resize(static_cast<std::size_t>(4 * nb2));
        for (int f = 0; f < 4; ++f)
            for (int b = 0; b < nb2; ++b) {
                g2::Mat3x4 m = g2::Mat3x4::identity();
                const float a = 0.3f * static_cast<float>(f + b);
                m.m[0][0] = std::cos(a);
                m.m[0][2] = std::sin(a);
                m.m[2][0] = -std::sin(a);
                m.m[2][2] = std::cos(a);
                af2.at(f, b) = m;
            }
        const auto w2 = g2::writeMdxa(s2, af2);
        const g2::MdxaFile g2f = g2::readMdxa(w2.data);

        g2::xsiexp::ExportOptions eo2;
        eo2.scale = s2.scale;
        const std::string t2 = g2::xsiexp::exportSequence(g2f, {"s", 0, 4}, eo2);
        const std::filesystem::path tp = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("shear");
        std::filesystem::create_directories(tp);
        { std::ofstream o(tp / "s.xsi", std::ios::binary); o << t2; }

        const auto a2 = g2::xsi::loadAnimation(g2::xsi::parseFile((tp / "s.xsi").string()),
                                               (tp / "s.xsi").string());
        g2::xsi::EvalOptions v2;
        v2.scale = s2.scale;
        const auto r2 = g2::xsi::evaluate(s2, a2, v2);

        double worst2 = 0.0;
        for (int f = 0; f < 4; ++f)
            for (int b = 0; b < nb2; ++b) {
                const auto A = g2f.boneMatrix(f, b);
                const auto B2 = r2.frames.at(f, b);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        worst2 = std::max(worst2,
                                          static_cast<double>(std::fabs(A.m[r][c] - B2.m[r][c])));
            }
        std::cout << "  flaches Skelett mit ungleichen Achsen: " << worst2 << "\n";

        // Ohne Polarzerlegung lag der Fehler hier bei ueber 0,5 — mit ihr
        // bleibt er in derselben Groessenordnung wie die Quantisierung.
        check(worst2 < 0.2, "flaches Skelett bleibt brauchbar");
        std::filesystem::remove_all(tp);
    }

    step("Restbewegung erkennen");
    {
        // Eine Sequenz OHNE Bewegung darf nicht gemeldet werden.
        check(g2::xsiexp::residualMotion(gla, {"ganz", 0, frames}) < 0.05f,
              "unbewegte Sequenz meldet keine Restbewegung");

        // Ohne Motion-Bone im Skelett gibt es nichts zu messen.
        g2::Skeleton ohne = sk;
        for (auto& b : ohne.bones)
            if (b.name == "Motion") b.name = "kein_motion";
        g2::MdxaFile kopie = gla;
        kopie.skeleton = ohne;
        check(g2::xsiexp::residualMotion(kopie, {"x", 0, frames}) == 0.0f,
              "ohne Motion-Bone wird nichts gemeldet");

        // Unsinnige Bereiche duerfen nicht abstuerzen.
        check(g2::xsiexp::residualMotion(gla, {"x", 0, 1}) == 0.0f, "ein Frame: nichts zu messen");
        check(g2::xsiexp::residualMotion(gla, {"x", -5, 10}) == 0.0f, "negativer Start abgewiesen");
        check(g2::xsiexp::residualMotion(gla, {"x", 0, frames + 99}) == 0.0f,
              "Bereich ausserhalb abgewiesen");
    }

    step("Templatenamen mit Leerzeichen");
    {
        // Softimage schreibt Szenennamen mit Leerzeichen und Punkten.
        // Ein Parser, der genau ein Token erwartet, bricht dort ab — bei
        // Ravens Zwischensequenzen fielen so fuenf von 1400 Dateien aus,
        // mit der Meldung "'{' erwartet nach Template SI_Scene".
        const std::filesystem::path td = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("tmplname");
        std::filesystem::create_directories(td);
        {
            std::ofstream o(td / "n.xsi", std::ios::binary);
            o << "xsi 0350txt 0032\n\n"
              << "SI_Scene Mein Modell v2.1 mit Leerzeichen {\n"
              << "  \"FRAMES\",\n  0.000000,\n  10.000000,\n  20.000000,\n}\n"
              << "SI_Model MDL-model_root {\n"
              << "  SI_Transform SRT-model_root {\n"
              << "    1.0,\n1.0,\n1.0,\n0.0,\n0.0,\n0.0,\n0.0,\n0.0,\n0.0,\n  }\n}\n";
        }
        bool ok = false;
        std::size_t roots = 0;
        try {
            const auto doc = g2::xsi::parseFile((td / "n.xsi").string());
            roots = doc.roots.size();
            ok = true;
        } catch (const std::exception&) {
        }
        check(ok, "Datei mit mehrteiligem Namen wird gelesen");
        check(roots == 2, "beide Wurzeltemplates erkannt");

        // Eine Datei ohne oeffnende Klammer darf trotzdem abbrechen,
        // nicht endlos weiterlesen.
        {
            std::ofstream o(td / "kaputt.xsi", std::ios::binary);
            o << "xsi 0350txt 0032\n\nSI_Scene ";
            for (int i = 0; i < 200; ++i) o << "wort" << i << " ";
            o << "\n";
        }
        bool geworfen = false;
        try {
            (void)g2::xsi::parseFile((td / "kaputt.xsi").string());
        } catch (const std::exception&) {
            geworfen = true;
        }
        check(geworfen, "fehlende Klammer bricht weiterhin ab");

        std::filesystem::remove_all(td);
    }

    step("GLA-Name aus -makeskel");
    {
        // Der Name steht als Zeichenkette im Kopf der GLA und sagt der
        // Engine, welches Skelett das ist. Kam er von der Referenz, trug
        // JEDE gebaute Datei "models/players/_humanoid/_humanoid" — egal
        // wohin sie gehoerte.
        //
        // Im Spiel meldete sich ein eigener Humanoid dann als der
        // Standard-Humanoid, die Engine nahm dessen animation.cfg und
        // spielte an jeder Stelle die Animation ab, die dort zufaellig
        // stand. Das sah nach kaputten Animationen aus und war ein Name.
        g2::Skeleton umbenannt = sk;
        umbenannt.name = "models/players/_humanoid_bdroid/_humanoid";
        const auto w5 = g2::writeMdxa(umbenannt, af);
        const g2::MdxaFile g5 = g2::readMdxa(w5.data);
        check(g5.skeleton.name == "models/players/_humanoid_bdroid/_humanoid",
              "der gesetzte Name steht in der Datei");
        check(g5.skeleton.name != sk.name, "und nicht der der Vorlage");

        // Bones und Frames bleiben davon unberuehrt.
        check(g5.numFrames == frames, "Framezahl unveraendert");
        check(g5.skeleton.bones.size() == sk.bones.size(), "Bonezahl unveraendert");
    }

    step("dotXSI-Fassung 3.0 und 3.5");
    {
        // Der Unterschied ist nicht nur die Zahl im Kopf: v3.0 BENENNT
        // seine Templates, v3.5 laesst sie namenlos. Wir schrieben bisher
        // benannte Templates unter einem 3.5-Kopf — 3.0-Inhalt mit
        // 3.5-Etikett.
        //
        // Ravens JK2-root.xsi ist v3.0 (511 BASEPOSE, 514 SRT, benannte
        // Kurven), ihre JKA-Animationsdateien sind v3.5 mit namenlosen.
        g2::xsiexp::ExportOptions o30 = eo, o35 = eo;
        o30.version = g2::xsiexp::ExportOptions::Version::V30;
        o35.version = g2::xsiexp::ExportOptions::Version::V35;

        const std::string t30 = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, o30);
        const std::string t35 = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, o35);

        check(t30.find("xsi 0300txt") == 0, "3.0 schreibt 0300txt");
        check(t35.find("xsi 0350txt") == 0, "3.5 schreibt 0350txt");

        // Und der Inhalt muss dazu passen, nicht nur der Kopf.
        // Gegen den TATSAECHLICHEN ersten Bonenamen pruefen, nicht gegen
        // "model_root": das Testskelett heisst anders, und ein Test, der auf
        // einen fremden Namen prueft, misst nichts.
        const std::string erster = "SI_FCurve " + sk.bones[0].name + "-";
        check(t30.find(erster) != std::string::npos, "3.0 benennt die Kurven");
        check(t35.find(erster) == std::string::npos, "3.5 benennt sie nicht");
        check(t35.find("SI_FCurve {") != std::string::npos, "3.5 schreibt sie namenlos");

        // Beide muessen sich wieder einlesen lassen und dasselbe ergeben.
        const std::filesystem::path vd = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("xsiver");
        std::filesystem::create_directories(vd);
        { std::ofstream a(vd / "a.xsi", std::ios::binary); a << t30; }
        { std::ofstream b(vd / "b.xsi", std::ios::binary); b << t35; }

        const auto a30 = g2::xsi::loadAnimation(g2::xsi::parseFile((vd / "a.xsi").string()),
                                                (vd / "a.xsi").string());
        const auto a35 = g2::xsi::loadAnimation(g2::xsi::parseFile((vd / "b.xsi").string()),
                                                (vd / "b.xsi").string());
        check(a30.nodes.size() == a35.nodes.size(), "gleich viele Knoten");

        g2::xsi::EvalOptions ve;
        ve.scale = sk.scale;
        const auto r30 = g2::xsi::evaluate(sk, a30, ve);
        const auto r35 = g2::xsi::evaluate(sk, a35, ve);
        double d = 0.0;
        for (int f = 0; f < r30.frames.frameCount(); ++f)
            for (int b = 0; b < nb; ++b) {
                const auto A = r30.frames.at(f, b);
                const auto B2 = r35.frames.at(f, b);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        d = std::max(d, static_cast<double>(std::fabs(A.m[r][c] - B2.m[r][c])));
            }
        check(d < 1e-6, "beide Fassungen ergeben dieselbe Animation");

        std::filesystem::remove_all(vd);
    }

    step("Ein-Frame-Sequenzen");
    {
        // Carcass weist eine .xsi mit nur einem Frame ab:
        //   "XSI file-format doesn't support 1-frames files 100% legally"
        // Ravens eigene 58 Ein-Frame-Sequenzen sind denn auch alle
        // -additional-Unterbereiche laengerer Dateien.
        //
        // Deshalb wird der Frame verdoppelt: zwei identische Frames, die
        // Animation unveraendert, und Carcass kann die Datei lesen.
        const std::string one = g2::xsiexp::exportSequence(gla, {"einzel", 3, 1}, eo);
        check(one.find("0,\n  1,\n") != std::string::npos,
              "Framebereich 0..1 statt 0..0");

        // Beide Frames muessen denselben Inhalt haben.
        const std::filesystem::path od = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("onefr");
        std::filesystem::create_directories(od);
        { std::ofstream o(od / "o.xsi", std::ios::binary); o << one; }
        const auto oa = g2::xsi::loadAnimation(g2::xsi::parseFile((od / "o.xsi").string()),
                                               (od / "o.xsi").string());
        check(oa.lastFrame == 1, "zwei Frames in der Datei");

        g2::xsi::EvalOptions ov;
        ov.scale = sk.scale;
        const auto orr = g2::xsi::evaluate(sk, oa, ov);
        check(orr.frames.frameCount() == 2, "zwei Frames ausgewertet");

        double diff = 0.0;
        for (int b = 0; b < nb; ++b) {
            const auto A = orr.frames.at(0, b);
            const auto B2 = orr.frames.at(1, b);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    diff = std::max(diff, static_cast<double>(std::fabs(A.m[r][c] - B2.m[r][c])));
        }
        check(diff < 1e-5, "beide Frames identisch - die Animation aendert sich nicht");

        // Mehrframige Sequenzen bleiben unveraendert.
        const std::string many = g2::xsiexp::exportSequence(gla, {"mehr", 0, frames}, eo);
        check(many.find("0,\n  " + std::to_string(frames - 1) + ",\n") != std::string::npos,
              "mehrframige Sequenz unveraendert");

        std::filesystem::remove_all(od);
    }

    step("BASEPOSE-Block");
    {
        // Ravens root.xsi hat pro Bone ZWEI Transform-Bloecke: "SRT-" fuer
        // die Pose und "BASEPOSE-" fuer die Bindepose. Carcass liest den
        // zweiten. Wir schrieben ihn nicht — und deshalb meldete Carcass
        // fuer jeden Bone "Basepose ... differs".
        //
        // Der Inhalt ist die WELT-Bindepose in dotXSI-Koordinaten, durch die
        // Skelettskalierung geteilt. Gegengeprueft an Ravens eigener Datei:
        // fuer lfemurYZ steht dort
        //   1.0 1.0 1.0 / 90.174767 3.982727 -74.993988 / 5.643987 55.604065 0.322196
        // und genau das ergibt die Rechnung.
        const std::string txt3 = g2::xsiexp::exportSequence(gla, {"bp", 0, frames}, eo);
        check(txt3.find("SI_Transform BASEPOSE-") != std::string::npos,
              "BASEPOSE-Block wird geschrieben");

        // Fuer jeden Bone genau einer, wie bei Raven.
        std::size_t nBase = 0, nSrt = 0, pos = 0;
        while ((pos = txt3.find("SI_Transform BASEPOSE-", pos)) != std::string::npos) {
            ++nBase;
            ++pos;
        }
        pos = 0;
        while ((pos = txt3.find("SI_Transform SRT-", pos)) != std::string::npos) {
            ++nSrt;
            ++pos;
        }
        // Die WURZEL bekommt keinen BASEPOSE-Block.
        //
        // Ravens Dateien haben durchweg genau drei SRT mehr als BASEPOSE —
        // root.xsi 277/274, jede Animationsdatei 98/95. Es fehlen
        // model_root, mesh_root und skeleton_root. Von denen kennt die GLA
        // nur model_root, also ist die Differenz bei uns genau eins.
        check(nBase == nSrt - 1, "genau ein SRT mehr als BASEPOSE");
        check(nBase == sk.bones.size() - 1, "fuer alle ausser der Wurzel");
        check(txt3.find("BASEPOSE-" + sk.bones[0].name) == std::string::npos,
              "die Wurzel hat keinen BASEPOSE-Block");

        // Die drei Varianten muessen sich unterscheiden.
        //
        // Welche Carcass will, ist offen — deshalb sind alle drei
        // erzeugbar. Ein Schalter, der nichts aendert, waere schlimmer als
        // keiner: man probiert dreimal dasselbe und haelt das Ergebnis fuer
        // eine Antwort.
        {
            g2::xsiexp::ExportOptions ow = eo, ol = eo, on = eo;
            ow.basePose = g2::xsiexp::ExportOptions::BasePose::World;
            ol.basePose = g2::xsiexp::ExportOptions::BasePose::Local;
            on.basePose = g2::xsiexp::ExportOptions::BasePose::None;

            const std::string tw = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, ow);
            const std::string tl = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, ol);
            const std::string tn = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, on);

            check(tw != tl, "Welt- und lokale Bindepose ergeben verschiedene Dateien");
            check(tn.find("BASEPOSE-") == std::string::npos, "none schreibt keinen Block");
            check(tw.find("BASEPOSE-") != std::string::npos, "world schreibt einen Block");
            check(tl.find("BASEPOSE-") != std::string::npos, "local schreibt einen Block");
        }

        // Und er muss VOR dem zugehoerigen SRT stehen, wie in Ravens
        // Dateien. Nicht gegen den ERSTEN SRT pruefen: der gehoert zur
        // Wurzel, die keinen BASEPOSE hat.
        {
            const std::string& zweiter = sk.bones[1].name;
            const std::size_t b2 = txt3.find("SI_Transform BASEPOSE-" + zweiter);
            const std::size_t s2 = txt3.find("SI_Transform SRT-" + zweiter);
            check(b2 != std::string::npos && s2 != std::string::npos && b2 < s2,
                  "BASEPOSE steht vor dem zugehoerigen SRT");
        }
    }

    step("SI_Transform ist die Ruhelage");
    {
        // Carcass liest den SI_Transform-Block als Bindepose, verkettet ihn
        // ueber die Hierarchie und vergleicht das Ergebnis mit dem
        // Zielskelett. Stand dort die animierte Pose von Frame 0, meldete es
        // fuer jeden Bone "Basepose for bone ... differs".
        //
        // Der eigene Importeur nimmt die Werte nie — er wertet die FCurves
        // aus —, deshalb blieb das unbemerkt, bis jemand die Datei durch
        // Ravens Carcass schickte.
        const std::string txt2 = g2::xsiexp::exportSequence(gla, {"ruhe", 0, frames}, eo);
        const std::filesystem::path rp = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("rest");
        std::filesystem::create_directories(rp);
        { std::ofstream o(rp / "r.xsi", std::ios::binary); o << txt2; }

        const auto ra = g2::xsi::loadAnimation(g2::xsi::parseFile((rp / "r.xsi").string()),
                                               (rp / "r.xsi").string());

        // Die SRT-Kette muss die Bindepose ergeben, nicht Frame 0.
        //
        // Geprueft ueber den vorhandenen Weg: eine Kopie der Datei ohne
        // FCurves auswerten. Dann greift in localMatrix der SRT-Rueckfall,
        // und das Ergebnis ist genau die Ruhelage, die Carcass liest.
        //
        // Gegengeprueft an Ravens eigener Both_forcelandleft1.xsi: dort steht
        // fuer lower_lumbar Skalierung 1,0 und Translation 9,0. Genau das
        // ergibt die lokale Bindepose — ohne Division durch die
        // Skelettskalierung, denn die kuerzt sich beim Bilden von
        // Eltern^-1 mal Kind heraus. Mit Division stuenden dort 1,5625 und
        // 14,0625, und Carcass meldete "Basepose ... differs".
        g2::xsi::AnimFile ruhe = ra;
        for (auto& n : ruhe.nodes) n.channels.clear();

        double worstRest = 0.0;
        std::string worstBone;
        for (std::size_t b = 0; b < sk.bones.size(); ++b) {
            const int nd = ruhe.indexOf(sk.bones[b].name);
            if (nd < 0) continue;

            const int par = sk.bones[b].parent;
            const g2::Mat3x4 local =
                par < 0 ? sk.bones[b].basePose
                        : g2::mul(g2::affineInverse(sk.bones[static_cast<std::size_t>(par)].basePose),
                                  sk.bones[b].basePose);

            const g2::Mat3x4 m = ruhe.localMatrix(nd, 0);

            // Translation UND Skalierung pruefen.
            //
            // Nur die Translation zu vergleichen war zu schwach: die
            // Skelettskalierung an der WURZEL blieb dabei unbemerkt, weil
            // deren Translation null ist. Carcass meldete sie sehr wohl —
            // als "model_root differs by 0.230400", und weil die ganze Kette
            // darauf aufbaut, danach fuer jeden Bone.
            for (int r = 0; r < 3; ++r) {
                const double e = std::fabs(static_cast<double>(local.m[r][3]) - m.m[r][3]);
                if (e > worstRest) {
                    worstRest = e;
                    worstBone = sk.bones[b].name + " (Translation)";
                }
            }
            // Spaltenlaengen: die Skalierung, unabhaengig von der Rotation.
            const int par2 = sk.bones[b].parent;
            for (int c = 0; c < 3; ++c) {
                double lenLocal = 0.0, lenM = 0.0;
                for (int r = 0; r < 3; ++r) {
                    lenLocal += static_cast<double>(local.m[r][c]) * local.m[r][c];
                    lenM += static_cast<double>(m.m[r][c]) * m.m[r][c];
                }
                lenLocal = std::sqrt(lenLocal);
                lenM = std::sqrt(lenM);
                // Bei der Wurzel steckt die Skelettskalierung noch in der
                // Bindepose; die Datei traegt sie bewusst nicht.
                if (par2 < 0 && sk.scale > 0.0f) lenLocal /= sk.scale;
                const double e = std::fabs(lenLocal - lenM);
                if (e > worstRest) {
                    worstRest = e;
                    worstBone = sk.bones[b].name + " (Skalierung)";
                }
            }
        }
        std::cout << "  groesster Unterschied Ruhelage <-> Bindepose: " << worstRest
                  << (worstBone.empty() ? "" : " bei " + worstBone) << "\n";
        check(worstRest < 0.01, "SI_Transform traegt die Bindepose, nicht Frame 0");

        std::filesystem::remove_all(rp);
    }

    step("averagevec lesen");
    {
        const std::filesystem::path ff = tmp / "t.frames";
        {
            std::ofstream o(ff, std::ios::binary);
            o << "\r\nc:/pfad/zur/seq.xsi\r\n{\r\n\t\"startframe\"\t\"0\"\r\n"
              << "\t\"duration\"\t\"8\"\r\n\t\"fps\"\t\"20\"\r\n"
              << "\t\"averagevec\"\t\"1.500 0.000 -2.250\"\r\n}\r\n";
        }
        const auto av = g2::xsiexp::readAverageVec(ff.string(), "seq");
        check(av.has_value(), "averagevec gefunden");
        if (av) {
            check(std::fabs((*av)[0] - 1.5f) < 1e-4, "x stimmt");
            check(std::fabs((*av)[2] + 2.25f) < 1e-4, "z stimmt");
        }
        // Unbekannte Sequenz darf nichts liefern statt irgendetwas.
        check(!g2::xsiexp::readAverageVec(ff.string(), "gibtsnicht").has_value(),
              "unbekannte Sequenz liefert nichts");
    }

    std::filesystem::remove_all(tmp);
}

int main() {
    // Ausgabe ungepuffert. Bei einem Haenger oder Absturz ist sonst nicht zu
    // erkennen, wo es passiert ist: der zuletzt gedruckte Text steckt noch im
    // Puffer, und der Lauf sieht so aus, als waere er frueher stehen
    // geblieben. Ein Testlauf ist kurz genug, dass die Einbusse egal ist.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_trace = std::getenv("G2C_TRACE") != nullptr;
    std::thread watchdog(watchdogMain);

    // Jeden Abschnitt einzeln absichern.
    //
    // Bricht ein Test ab, soll das Werkzeug SAGEN, welcher es war. Vorher
    // endete die Ausgabe einfach mitten im Lauf, und aus "es hoert nach
    // Skriptpruefung auf" laesst sich nicht ableiten, ob der Test selbst
    // abgestuerzt ist oder der naechste beim Start.
    const auto run = [](const char* name, void (*fn)()) {
        g_currentTest.store(name);
        g_currentStep.store("(Anfang)");
        // Nach jedem Abschnitt leeren. Bei einem harten Absturz — etwa einer
        // Zugriffsverletzung, die kein catch abfaengt — geht der gepufferte
        // Text sonst verloren, und die Ausgabe endet scheinbar frueher als
        // der Lauf. Das lenkt die Suche auf die falsche Stelle.
        std::fflush(stdout);
        try {
            fn();
            std::fflush(stdout);
        } catch (const std::exception& e) {
            std::printf("\n!! Abbruch in %s: %s\n\n", name, e.what());
            ++g_failures;
        } catch (...) {
            std::printf("\n!! Abbruch in %s: unbekannte Ausnahme\n\n", name);
            ++g_failures;
        }
    };

    run("testQuantizationBias", testQuantizationBias);
    run("testOutOfRangeClamping", testOutOfRangeClamping);
    run("testQuatMatrixRoundTrip", testQuatMatrixRoundTrip);
    run("testFullBoneRoundTrip", testFullBoneRoundTrip);
    run("testMdxaRoundTrip", testMdxaRoundTrip);
    run("testAffineInverse", testAffineInverse);
    run("testDedupe", testDedupe);
    run("testSkeletonValidation", testSkeletonValidation);
    run("testMdxmWrite", testMdxmWrite);
    run("testMdxmMultiLod", testMdxmMultiLod);
    run("testMdxmLimits", testMdxmLimits);
    run("testXsiParser", testXsiParser);
    run("testCarParser", testCarParser);
    run("testCarBuild", testCarBuild);
    run("testEnumTable", testEnumTable);
    run("testCarValidate", testCarValidate);
    run("testAnimCache", testAnimCache);
    run("testLocaleIndependence", testLocaleIndependence);
    run("testAnimEval", testAnimEval);
    run("testXsiExport", testXsiExport);


    g_currentTest.store("(fertig)");
    g_currentStep.store("(fertig)");
    g_watchdogStop.store(true);
    watchdog.join();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " Pruefungen bestanden\n";
    if (g_failures) std::cout << g_failures << " FEHLGESCHLAGEN\n";
    return g_failures ? 1 : 0;
}
