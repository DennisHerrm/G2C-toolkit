// Minimal test runner without external dependencies.

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
#include "g2/readfile.h"
#include "g2/gladiff.h"
#include "g2/xsi_mesh.h"

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

// A separate folder per run. If a test run hangs, the process keeps its files
// open; a new run in the same folder would, on Windows, run into the old one's
// locks. With a process ID and timestamp in the name that can't happen.
std::string uniqueTestDir(const char* what) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("g2c_") + what + "_" +
           std::to_string(static_cast<unsigned long long>(now) & 0xffffffull);
}

// Fine-grained step marker. Visible only when G2C_TRACE is set - normally it
// gets in the way, but when hunting down a hang it is the only thing that
// helps.
// Where is the run right now? The watchdog reads this.
std::atomic<const char*> g_currentTest{"(Start)"};
std::atomic<const char*> g_currentStep{"(noch nichts)"};
std::atomic<bool>        g_watchdogStop{false};

bool g_trace = false;

void step(const char* what) {
    g_currentStep.store(what);
    if (g_trace) std::printf("    [%s]\n", what);
}

// Reports when a section hangs, and aborts afterwards.
//
// A hanging test is worse than a crashing one: it says nothing at all.
// Previously you had to guess which step had stalled - now the program says
// so on its own, without a switch and without a second run.
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

    // Carcass returns 0 here, which decodes to -2.0.
    const float backQuat = g2::unsquashQuatComponent(
        g2::squashQuatComponent(5.0f, g2::Rounding::Nearest, stats));
    checkNear(backQuat, 2.0, 1e-3, "Quaternion 5.0 wird auf +2.0 geklemmt, nicht auf -2.0");
    check(stats.quatClamped == 1, "Klemmung wird gezaehlt");

    // And here: a -512 unit translation jump in the original.
    const float backXlat = g2::unsquashXlatComponent(
        g2::squashXlatComponent(9000.0f, g2::Rounding::Nearest, stats));
    // The largest value the format can hold: raw 65535 = 511.984375.
    checkNear(backXlat, 511.984375, 1e-3, "Translation 9000 wird auf den Hoechstwert geklemmt, nicht auf -512");
    check(stats.xlatClamped == 1, "Translationsklemmung wird gezaehlt");

    // NaN must not slip through.
    const float backNan = g2::unsquashQuatComponent(
        g2::squashQuatComponent(std::nanf(""), g2::Rounding::Nearest, stats));
    check(std::isfinite(backNan), "NaN erzeugt einen endlichen Wert");

    std::printf("  Statistik: %s\n", stats.summary().c_str());
}

void testQuatMatrixRoundTrip() {
    section("Matrix <-> Quaternion");

    // Also near 180 degrees, where the naive trace formula breaks down.
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

    // The same data in Carcass mode for comparison.
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

    // Reproduction of the real case: $scale 0.64 baked into the base pose.
    g2::Mat3x4 m = rotationZ(0.6f);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m.m[r][c] *= 0.64f;
    m.m[0][3] = 3.6f; m.m[1][3] = -0.2f; m.m[2][3] = 35.6f;

    const g2::Mat3x4 inv = g2::affineInverse(m);

    // M * MInv must yield the identity matrix.
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

    // The transpose would be clearly wrong here - exactly the bug that the
    // real _humanoid.gla uncovered.
    double transposeErr = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            transposeErr = std::max(transposeErr, std::fabs(double(inv.m[r][c]) - m.m[c][r]));
    std::printf("  Abweichung Transponierte vs. echte Inverse: %.4f\n", transposeErr);
    check(transposeErr > 0.5, "Transponierte waere bei Scale 0.64 grob falsch");

    // And once through the real write path.
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

    // Static pose across all frames: everything must collapse into one entry.
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

    // Parse the header back
    const auto rd32 = [&](std::size_t o) {
        return static_cast<std::int32_t>(w.data[o] | (w.data[o + 1] << 8) | (w.data[o + 2] << 16) |
                                         (std::uint32_t(w.data[o + 3]) << 24));
    };
    check(static_cast<std::uint32_t>(rd32(0)) == g2::fmt::kMdxmIdent, "Ident ist \"2LGM\"");
    check(rd32(4) == g2::fmt::kMdxmVersion, "Version ist 6");
    // Header offsets: numBones=140, numLODs=144, ofsLODs=148,
    // numSurfaces=152, ofsSurfHierarchy=156, ofsEnd=160
    check(rd32(140) == 6, "numBones ist 6");
    check(rd32(144) == 1, "numLODs ist 1");
    check(rd32(152) == 1, "numSurfaces ist 1");
    check(static_cast<std::size_t>(rd32(160)) == w.data.size(), "ofsEnd entspricht der Dateigroesse");

    // Check the vertex weight packing
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

    // Offset base of the LOD surface table: verified against the real
    // _humanoid.glm. offsets[0] must be numSurfaces*4 because the first surface
    // sits directly after the table.
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

    // Structure read off Raven's model.glm: 4 LODs, 80 surfaces,
    // offsets[0] = numSurfaces*4, and each LOD block ends where the next
    // one begins.
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
        // offsets[0] must be numSurfaces*4: the first surface sits directly
        // after the offset table, and the values count from its start.
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

    // Referencing 40 different bones -> must fail, not silently truncate.
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

    // Invalid triangle index
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

    // The interesting case: POSITION is a value, SI_Mesh a template.
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

    // Error cases
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

    // Reference skeleton: root -> a -> b
    g2::Skeleton ref;
    ref.name = "ref";
    ref.scale = 1.0f;
    g2::Mat3x4 br = g2::Mat3x4::identity();
    g2::Mat3x4 ba = g2::Mat3x4::identity(); ba.m[2][3] = 10.0f;
    g2::Mat3x4 bb = g2::Mat3x4::identity(); bb.m[2][3] = 20.0f;
    ref.bones.push_back({"root", -1, br, 0});
    ref.bones.push_back({"a",     0, ba, 0});
    ref.bones.push_back({"b",     1, bb, 0});

    // Animation file: only "a" is animated, "b" is missing entirely.
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

    // Frame numbers with decimals. 3ds Max writes "1.000000", Raven writes
    // "1". Reading the number as an integer loses EVERY keyframe of Max
    // exports - the channels then exist but are empty, and all bones
    // silently stay in the rest pose.
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

    // SI_Transform SRT-<name> as the rest value. Bones without an FCurve must
    // keep their static pose, not fall back to identity. In root.xsi only
    // 126 of 276 models have FCurves - without this fallback more than half
    // of the skeleton ends up wrong.
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
        // The FCurve wins in its channel, SRT stays in the others.
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

    // The non-animated bone "b" must get the identity matrix - that is exactly
    // what Carcass writes too (in the real _humanoid.gla the non-animated
    // bone "face" has the identity matrix).
    for (int f = 0; f < 2; ++f) {
        const g2::Mat3x4& m = res.frames.at(f, 2);
        double worst = 0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, std::fabs(double(m.m[r][c]) - (r == c ? 1.0 : 0.0)));
        checkNear(worst, 0.0, 1e-5, "Frame " + std::to_string(f) + ": Bone \"b\" ist identisch");
    }

    // Frame 1 has rotation 0 -> "a" must be the identity matrix too.
    {
        const g2::Mat3x4& m = res.frames.at(0, 1);
        checkNear(m.m[0][0], 1.0, 1e-4, "Frame 0: Bone \"a\" unrotiert");
        checkNear(m.m[0][1], 0.0, 1e-4, "Frame 0: Bone \"a\" ohne Scherung");
    }
    // Frame 2 has 30 degrees around Z.
    {
        const g2::Mat3x4& m = res.frames.at(1, 1);
        checkNear(m.m[0][0], std::cos(30.0 * 3.14159265358979 / 180.0), 2e-3,
                  "Frame 1: Bone \"a\" um 30 Grad gedreht");
    }

    // Root motion: Carcass puts a LINEAR ramp over the total displacement of
    // the Motion bone onto the root bone, as a counter-motion. Deliberately
    // not the actual curve - the Motion bone swings wildly here, yet the ramp
    // must still run dead straight.
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

        // Motion runs from z=0 to z=10, so the total displacement is 10.
        // GLA-y = -scale * (-dz) = +scale*dz = 20, spread over 4 steps.
        const double expect[5] = {0.0, 5.0, 10.0, 15.0, 20.0};
        for (int f = 0; f < 5; ++f)
            checkNear(rr.frames.at(f, 0).m[1][3], expect[f], 1e-3,
                      "Wurzelrampe Frame " + std::to_string(f));

        // Can be switched off.
        g2::xsi::EvalOptions off = ro;
        off.extractRootMotion = false;
        const auto ro2 = g2::xsi::evaluate(ref, ra, off);
        checkNear(ro2.frames.at(4, 0).m[1][3], 0.0, 1e-4, "ohne Wurzelbewegung bleibt es bei 0");
    }

    // The origin offset ends up on the root bone.
    g2::xsi::EvalOptions o2 = opt;
    o2.origin = std::array<float, 3>{0.0f, 0.0f, 24.0f};
    const auto res2 = g2::xsi::evaluate(ref, anim, o2);
    checkNear(res2.frames.at(0, 0).m[2][3], -24.0, 1e-4, "-origin liegt negativ auf dem Wurzelbone");

    // Alias mapping
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

    // Missing arguments must produce a useful message.
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

    // Create two tiny animation files.
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
    writeAnim("scened.xsi", 6, 2.0f, 29.97f);  // with SI_Scene, NTSC rate

    {
        std::ofstream c(root / "test.car");
        c << "$aseanimgrabinit\n"
             "$scale 1.0\n"
             "$aseanimgrab models/anims/first.xsi -loop -1 -framespeed 20\n"
             "$aseanimgrab models/anims/second.xsi -loop 3 -framespeed 30 "
             "-additional 2 4 -1 15 EXTRA_SEQ\n"
             // Without -loop and without -framespeed: the defaults must apply.
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

    // Defaults without flags. Both read off Raven's animation.cfg:
    // without -loop Carcass writes 0 (not -1), and without -framespeed the
    // frame rate from the SI_Scene of the respective .xsi.
    const auto& sc = br.sequences[3];
    check(sc.name == "SCENED", "dritte Sequenz benannt");
    check(sc.loopFrame == 0, "ohne -loop ist der Loopframe 0, nicht -1");
    check(sc.frameSpeed == 29, "Framerate 29.97 wird abgeschnitten, nicht gerundet");
    check(sc.frameCount == 6, "Framezahl aus SI_Scene (1..6)");

    // -origin from the script must end up on the root bone.
    checkNear(br.frames.at(0, 0).m[2][3], -24.0, 1e-4, "-origin aus dem Skript angewandt");

    // Missing file: hard abort with a useful message.
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

    // Generate animation.cfg and check it back.
    const std::string cfg = g2::car::writeAnimationCfg(br.sequences, "test");
    check(cfg.find("EXTRA_SEQ") != std::string::npos, "animation.cfg enthaelt die Zusatzsequenz");
    check(cfg.find("\r\n") != std::string::npos, "animation.cfg nutzt CRLF");

    { std::error_code ec; fs::remove_all(root, ec); }
}

// Forces a decimal comma instead of a decimal point without needing a system
// locale. This reproduces the case the old Carcass failed on: it only worked
// with American regional settings.
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

    // The cache entry must contain everything the parser delivers -
    // otherwise it acts like silent data loss.
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

    // If the source changes, the entry must expire. Otherwise the cache
    // returns stale data and a fix appears not to work.
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

    // A disabled cache must not create anything.
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

    // Raven's anims.h is NOT valid C: many entries have no comma. A parser
    // that requires one silently misses them - in the real file that was
    // 102 of 1705 entries, and as a result perfectly valid sequences were
    // reported as unknown.
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

        // The crucial case: a path that cannot be opened MUST blow up instead
        // of silently doing nothing. That is exactly what used to happen in
        // three places.
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
             "$aseanimgrab anims/a.xsi -enum BOTH_STAND1\n"          // duplicate
             "$aseanimgrab anims/a.xsi -enum BOTH_UNBEKANNT\n"       // not in the table
             "$aseanimgrab anims/a.xsi -enum BOTH_WALK1 -additional 0 5 9 20 BOTH_RUN1\n"
             "$aseanimgrab anims/fehlt.xsi -enum BOTH_RUN2\n"        // missing
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

    // A clean script must not produce errors.
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

    // Write and read back in: the meaning must be preserved.
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

    // Scan the directory.
    step("scanDirectory");
    {
        const auto found = g2::car::scanDirectory(root.string());
        check(found.size() == 2, "beide .car gefunden");
        bool haveGrabs = true;
        for (const auto& f : found) if (f.grabs == 0) haveGrabs = false;
        check(haveGrabs, "Grabzahl je Datei ermittelt");

        // Also find them in subfolders.
        std::error_code ec2;
        fs::create_directories(root / "tief" / "tiefer", ec2);
        { std::ofstream c(root / "tief" / "tiefer" / "d.car"); c << "$aseanimgrabinit\n"; }
        const auto deep = g2::car::scanDirectory(root.string());
        check(deep.size() == 3, "auch in Unterordnern gefunden");

        // The depth limit takes effect.
        const auto shallow = g2::car::scanDirectory(root.string(), 0);
        check(shallow.size() == 2, "Tiefenbegrenzung 0 bleibt im Wurzelordner");

        // A directory link to an ancestor must NOT send the search into an
        // endless loop. That is exactly where the previous version using
        // recursive_directory_iterator got stuck on Windows.
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

    // Cross-check: the locale is really active.
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

        // And the same for the .car script.
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

// Way back: GLA -> dotXSI -> GLA.
//
// This is the workflow the user cares about: extract an animation from
// someone else's GLA, mix it with their own files and rebuild.
void testXsiExport() {
    std::cout << "== Export GLA -> dotXSI ==\n";

    // Build a small skeleton and an animation by hand, so the test runs
    // without the large original files.
    step("Skelett anlegen");
    g2::Skeleton sk;
    sk.name = "t";
    sk.scale = 0.64f;
    for (int i = 0; i < 4; ++i) {
        g2::Bone b;
        b.name = "b" + std::to_string(i);
        b.parent = i == 0 ? -1 : i - 1;
        b.basePose = g2::Mat3x4::identity();
        // Bind poses with offset AND scale: a pure identity would hide
        // errors in both.
        b.basePose.m[0][3] = static_cast<float>(i) * 3.0f;
        for (int r = 0; r < 3; ++r) b.basePose.m[r][r] = 0.64f;
        sk.bones.push_back(b);
    }

    // A bone with its parent bone AFTER it - exactly the case where index
    // order breaks down.
    {
        g2::Bone b;
        b.name = "spaet";
        b.parent = 4;   // points to the last one, which is still to come
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
    // The default is now v3.0 - named templates as in Raven's root.xsi.
    check(text.find("xsi 0300txt") == 0, "dotXSI-Kopf geschrieben");
    check(text.find("SI_Scene") != std::string::npos, "SI_Scene vorhanden");

    // SI_FCurve MUST be a direct child of SI_Model. If it sits inside an
    // SI_Animation block the importer doesn't find it - then every bone
    // stays on frame 0, and frame 0 of all frames is the one that's correct.
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

    step("$keepmotion wie bei Raven");
    {
        // $keepmotion only keeps the Motion bone. The root motion is taken out
        // in both cases - that's what Carcass' output says ("Keeping motion
        // bone", then "Compensating for motion bone"), and that's how Raven's
        // own _humanoid.gla is built, whose script contains $keepmotion.
        const std::filesystem::path kr = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("keepmotion");
        std::filesystem::create_directories(kr / "models");

        // An animation in which the Motion bone moves.
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
            // Displacement of the root bone over the sequence.
            const auto a = br2.frames.at(0, 0);
            const auto b = br2.frames.at(br2.frames.frameCount() - 1, 0);
            return std::fabs(b.m[0][3] - a.m[0][3]);
        };

        const float ohne = runWith(false);
        const float mit = runWith(true);
        std::cout << "  Wurzelversatz ohne $keepmotion: " << ohne << ", mit: " << mit << "\n";

        check(ohne > 1.0f, "ohne $keepmotion wird die Bewegung herausgerechnet");
        check(std::fabs(mit - ohne) < 1e-4f,
              "mit $keepmotion ebenso — die Gegenrampe liegt in beiden Faellen an");

        std::error_code ec;
        std::filesystem::remove_all(kr, ec);
    }

    step("Wurzelbewegung aus der GLA holen");
    {
        // The .frames file isn't needed for this: Carcass applies the motion
        // as a linear ramp on the root bone, and that is still in the GLA.
        //
        // Here a GLA with a known ramp is built and we check that exactly
        // that ramp comes back.
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
        const int fr3 = 11;   // 10 steps
        g2::AnimationFrames af3;
        af3.numBones = 2;
        af3.matrices.resize(static_cast<std::size_t>(fr3 * 2));
        for (int f = 0; f < fr3; ++f)
            for (int b = 0; b < 2; ++b) {
                g2::Mat3x4 m = g2::Mat3x4::identity();
                // Only the root bone carries the ramp: -35 over 10 steps.
                if (b == 0) m.m[0][3] = -3.5f * static_cast<float>(f);
                af3.at(f, b) = m;
            }
        const auto w3 = g2::writeMdxa(s3, af3);
        const g2::MdxaFile g3 = g2::readMdxa(w3.data);

        const auto rm = g2::xsiexp::detectRootMotion(g3, {"s", 0, fr3});
        check(rm.has_value(), "Bewegung erkannt");
        if (rm) {
            // averagevec = -ramp / steps = 35/10 = 3.5
            check(std::fabs((*rm)[0] - 3.5f) < 0.05f, "Betrag stimmt");
            check(std::fabs((*rm)[1]) < 0.05f, "keine Bewegung in Y");
            check(std::fabs((*rm)[2]) < 0.05f, "keine Bewegung in Z");
        }

        // Without motion nothing may be reported - otherwise the export
        // would insert something that never existed.
        g2::AnimationFrames af4 = af3;
        for (int f = 0; f < fr3; ++f) af4.at(f, 0) = g2::Mat3x4::identity();
        const auto w4 = g2::writeMdxa(s3, af4);
        const g2::MdxaFile g4 = g2::readMdxa(w4.data);
        check(!g2::xsiexp::detectRootMotion(g4, {"s", 0, fr3}).has_value(),
              "ohne Bewegung wird nichts erfunden");

        // Nonsensical ranges must not crash.
        check(!g2::xsiexp::detectRootMotion(g3, {"s", 0, 1}).has_value(), "ein Frame");
        check(!g2::xsiexp::detectRootMotion(g3, {"s", -3, 5}).has_value(), "negativer Start");
        check(!g2::xsiexp::detectRootMotion(g3, {"s", 0, 999}).has_value(), "zu langer Bereich");
    }

    step("Unterbereiche zusammenfassen");
    {
        // The core of the way back: if every sequence were exported on its
        // own, the new GLA would have more frames than the old one and the
        // animation.cfg would no longer fit.
        using g2::xsiexp::CfgSequence;
        const std::vector<CfgSequence> cfg = {
            {"MASTER_A", 0, 10, -1, 20},
            {"TEIL_A1", 0, 3, -1, 30},     // lies inside MASTER_A
            {"TEIL_A2", 5, 2, 0, 10},      // also lies inside it
            {"MASTER_B", 10, 5, 0, 20},
            {"ALIAS_B", 10, 5, 0, 20},     // same range -> sub-range
            {"MASTER_C", 15, 4, -1, 20},
        };
        const auto g = g2::xsiexp::groupSequences(cfg);
        check(g.masters.size() == 3, "drei Master erkannt");
        check(g.partial == 0, "nichts teilweise ueberlappend");

        // Crucial: the masters must not overlap and must cover everything.
        // Otherwise the frame count is wrong.
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

        // The script must carry the sub-ranges as -additional with the
        // correct offset.
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

        // The last line says WHERE the GLA is created and what it's called.
        // The value is stored verbatim in the source GLA's header - if it is
        // derived instead of taken over, the new GLA ends up under the wrong
        // name and the model doesn't find it.
        const auto scMs = g2::xsiexp::buildScript(g, "models/players/j/", std::nullopt, 0.64f,
                                                  false, "models/players/_humanoid/_humanoid");
        check(scMs.convert && scMs.convert->makeSkel == "models/players/_humanoid/_humanoid",
              "Skelettpfad wird uebernommen, nicht abgeleitet");
        check(scMs.convert && scMs.convert->root == "models/players/j/root",
              "root zeigt dorthin, wo die Dateien liegen");

        // Without it, the path is derived - and must not get a leading
        // slash, otherwise it's an absolute path.
        const auto scNo = g2::xsiexp::buildScript(g, "", std::nullopt);
        check(scNo.convert && scNo.convert->makeSkel == "_humanoid",
              "ohne Praefix kein fuehrender Schraegstrich");

        // And the offset must end up in the script, otherwise the model is
        // off by that amount after rebuilding.
        const auto sc2 = g2::xsiexp::buildScript(g, "models/x/",
                                                 std::array<float, 3>{0.0f, 0.0f, 24.0f});
        check(sc2.convert.has_value(), "Konvertierungsanweisung vorhanden");
        check(sc2.convert && sc2.convert->origin.has_value(), "origin steht im Skript");

        // A range that extends past the end must not be silently
        // bent into shape.
        const auto g2p = g2::xsiexp::groupSequences({{"A", 0, 10, -1, 20}, {"B", 5, 10, -1, 20}});
        check(g2p.partial == 1, "teilweise ueberlappender Bereich wird gemeldet");
    }

    step("Kommentare in der animation.cfg");
    {
        // Raven's animation.cfg structures 1683 sequences with separators and
        // headings. Without them the file is a wall of numbers - and they were
        // lost on rebuild, because the cfg is generated from scratch.
        const std::filesystem::path kd = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("kommentar");
        std::filesystem::create_directories(kd);
        {
            std::ofstream c(kd / "t.car");
            c << "$aseanimgrabinit\n"
              << "\n"
              << "//////////////////////////////\n"
              << "//  NEUE KATA-ANIMATIONEN\n"
              << "//////////////////////////////\n"
              << "$aseanimgrab a.xsi -enum BOTH_SMASHDOWN_DUAL\n"
              << "// einzelne Animationen je Charakter\n"
              << "$aseanimgrab b.xsi -enum BOTH_WALK1_ANI\n"
              << "$aseanimgrabfinalize\n";
        }
        const auto sc3 = g2::car::parseFile((kd / "t.car").string());
        check(sc3.grabs.size() == 2, "zwei Grabs");
        if (sc3.grabs.size() == 2) {
            check(sc3.grabs[0].commentsBefore.size() == 3,
                  "drei Kommentarzeilen vor dem ersten");
            check(sc3.grabs[1].commentsBefore.size() == 1,
                  "eine vor dem zweiten");
            // The blank line after $aseanimgrabinit must not come along.
            for (const auto& c : sc3.grabs[0].commentsBefore)
                check(!c.empty(), "keine leere Zeile vorangestellt");
        }

        // And they must end up in the written cfg.
        std::vector<g2::car::Sequence> sq;
        for (const auto& g : sc3.grabs) {
            g2::car::Sequence q;
            q.name = g.enumName ? *g.enumName : g.derivedName();
            q.frameCount = 2;
            q.commentsBefore = g.commentsBefore;
            sq.push_back(std::move(q));
        }
        const std::string cfg = g2::car::writeAnimationCfg(sq, "test");
        check(cfg.find("NEUE KATA-ANIMATIONEN") != std::string::npos,
              "Ueberschrift in der cfg");

        // Space around the block: a heading glued to the line above looks
        // like an addendum to the previous sequence rather than the start of
        // a new block.
        {
            const std::size_t k = cfg.find("//////////////////////////////");
            check(k != std::string::npos, "Trennlinie vorhanden");

            // The FIRST block deliberately has no blank line: it follows the
            // file header directly, where one would be superfluous.
            // So we check the second comment, which sits in the middle of the
            // list.

            const std::size_t e = cfg.find("einzelne Animationen je Charakter");
            check(e != std::string::npos, "zweiter Block vorhanden");
            if (e != std::string::npos) {
                // Before it: the start of the line, and the line above is empty.
                const std::size_t za = cfg.rfind("\r\n", e);
                if (za != std::string::npos && za >= 2)
                    check(cfg.compare(za - 2, 2, "\r\n") == 0, "Leerzeile VOR dem Block");

                // After it: end of line, then an empty line.
                const std::size_t nz = cfg.find("\r\n", e);
                check(nz != std::string::npos && cfg.compare(nz + 2, 2, "\r\n") == 0,
                      "Leerzeile NACH dem Block");
            }
        }
        check(cfg.find("einzelne Animationen je Charakter") != std::string::npos,
              "zweiter Kommentar in der cfg");
        // And BEFORE its sequence, not just anywhere.
        check(cfg.find("NEUE KATA") < cfg.find("BOTH_SMASHDOWN_DUAL"),
              "Kommentar steht vor seiner Sequenz");

        { std::error_code rmEc; std::filesystem::remove_all(kd, rmEc); }
    }

    step("Spalten in der animation.cfg richten sich aus");
    {
        // Carcass pads the name to a fixed 20 characters. That's enough for
        // "BOTH_STAND1", but not for the long names from character sets -
        // there the numbers slip out of their column.
        std::vector<g2::car::Sequence> sp;
        const auto mach = [&](const char* n, int t, int c, int l, int f) {
            g2::car::Sequence q;
            q.name = n;
            q.targetFrame = t;
            q.frameCount = c;
            q.loopFrame = l;
            q.frameSpeed = f;
            sp.push_back(std::move(q));
        };
        mach("ROOT", 1, 2, -1, 20);
        mach("BOTH_BOLT_BLOCK_TWO_HAND_BOTTOM_LEFT_ANAKIN", 36789, 20, 0, 30);
        mach("BOTH_WALK1", 300, 152, 0, 20);

        const std::string txt = g2::car::writeAnimationCfg(sp, "t");

        // Determine the position of the first number in each data line.
        std::vector<std::size_t> spalten;
        std::istringstream is(txt);
        std::string zeile;
        while (std::getline(is, zeile)) {
            if (zeile.empty() || zeile[0] == '/') continue;
            if (!zeile.empty() && zeile.back() == '\r') zeile.pop_back();
            // Position where the NUMBER FIELD begins - i.e. after the name
            // including its padding. Don't search for the first digit: the
            // numbers are right-aligned, so their starts lie at different
            // distances to the right.
            const std::size_t leer = zeile.find(' ');
            if (leer == std::string::npos) continue;
            spalten.push_back(zeile.find_first_not_of(' ', leer) - 0);
            // For the comparison, the END of the first number is what counts:
            // that's where the digits line up.
            const std::size_t start = zeile.find_first_not_of(' ', leer);
            const std::size_t ende = zeile.find(' ', start);
            spalten.back() = ende == std::string::npos ? zeile.size() : ende;
        }
        check(spalten.size() == 3, "drei Datenzeilen");

        bool gleich = true;
        for (std::size_t k = 1; k < spalten.size(); ++k)
            if (spalten[k] != spalten[0]) gleich = false;
        check(gleich, "erste Zahl steht in allen Zeilen an derselben Stelle");

        // And the engine must still be able to read it: the name stays the
        // first field, followed by four numbers.
        {
            std::istringstream p2(txt);
            int datenzeilen = 0;
            std::string z2;
            while (std::getline(p2, z2)) {
                if (z2.empty() || z2[0] == '/') continue;
                std::istringstream f(z2);
                std::string nm;
                int a = 0, b = 0, c = 0, d = 0;
                if (f >> nm >> a >> b >> c >> d) ++datenzeilen;
            }
            check(datenzeilen == 3, "alle drei Zeilen bleiben lesbar");
        }
    }

    step("animation.cfg heisst wie die Engine sie sucht");
    {
        // It used to be called "<name>_animation.cfg". That overwrites nothing
        // and sits harmlessly next to it - but the engine looks for
        // "animation.cfg". Anyone who copies the output folder into the game
        // still has the old one, and because inserting animations shifts ALL
        // subsequent target frames, each name then plays whatever animation
        // happens to be there.
        //
        // It looked like a broken tool and was actually a forgotten file.
        const std::filesystem::path cd = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("cfgname");
        std::filesystem::create_directories(cd);

        std::vector<g2::car::Sequence> sq;
        {
            g2::car::Sequence q;
            q.name = "BOTH_STAND1";
            q.frameCount = 2;
            q.frameSpeed = 20;
            q.sourceFile = "a.xsi";
            sq.push_back(std::move(q));
        }
        const std::string text = g2::car::writeAnimationCfg(sq, "2 frames");

        // The name must be exactly this.
        const std::filesystem::path erwartet = cd / "animation.cfg";
        { std::ofstream o(erwartet, std::ios::binary); o << text; }
        check(std::filesystem::exists(erwartet), "heisst animation.cfg");

        // And reading it back in must yield the same sequence.
        //
        // The stream lives in its own block: on Windows an open file locks
        // its folder, and remove_all would throw - that exception used to
        // abort the whole test along with all subsequent checks.
        std::string gefunden;
        {
            std::ifstream in(erwartet);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '/') continue;
                std::istringstream is(line);
                std::string nm;
                int a = 0, b = 0, c = 0, d = 0;
                if (is >> nm >> a >> b >> c >> d) gefunden = nm;
            }
        }
        check(gefunden == "BOTH_STAND1", "Sequenz wieder lesbar");

        std::error_code ec;
        std::filesystem::remove_all(cd, ec);
    }

    step("Groessengrenze der animation.cfg");
    {
        // The engine has a fixed buffer:
        //   UI_ParseAnimationFile: File ... too long (172308 > 159999)
        //
        // From a real case: aligning to the longest name padded each of the
        // 2463 lines to 46 characters - around 70000 bytes of spaces alone,
        // enough to push a previously fitting file over the limit.
        std::vector<g2::car::Sequence> viele;
        for (int i = 0; i < 2500; ++i) {
            g2::car::Sequence q;
            q.name = "BOTH_SEQUENCE_NUMBER_" + std::to_string(i);
            q.targetFrame = i * 20;
            q.frameCount = 20;
            q.loopFrame = -1;
            q.frameSpeed = 20;
            viele.push_back(std::move(q));
        }
        // A very long name must not bloat the whole file.
        viele[1000].name = "BOTH_BOLT_BLOCK_TWO_HAND_BOTTOM_LEFT_ANAKIN_EXTRA_LANG";

        const std::string gross = g2::car::writeAnimationCfg(viele, "test");
        std::cout << "  2500 Sequenzen ergeben " << gross.size() << " Byte\n";
        check(gross.size() <= 159999, "bleibt unter der Grenze der Engine");

        // With few sequences, on the other hand, alignment should apply.
        std::vector<g2::car::Sequence> wenige(viele.begin(), viele.begin() + 5);
        wenige[2].name = "BOTH_EIN_SEHR_LANGER_NAME_FUER_DEN_TEST";
        const std::string klein = g2::car::writeAnimationCfg(wenige, "test");
        const std::size_t p1 = klein.find("BOTH_SEQUENCE_NUMBER_0");
        check(p1 != std::string::npos, "erste Sequenz vorhanden");
        if (p1 != std::string::npos) {
            const std::size_t ze = klein.find("\r\n", p1);
            check(ze - p1 > 40, "bei wenigen Zeilen wird ausgerichtet");
        }
    }

    step("Doppelte Sequenznamen melden");
    {
        // The engine looks things up in animation.cfg by NAME. If one appears
        // twice, the last one wins - the first animation is unreachable even
        // though its frames take up space.
        //
        // In game it looks as if the animation does nothing, and nobody looks
        // for the cause in the cfg. From a real case: two sequences appeared
        // twice in a file 2463 entries long.
        const std::filesystem::path dd = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("doppelt");
        std::filesystem::create_directories(dd);
        {
            std::ofstream c(dd / "t.car");
            c << "$aseanimgrabinit\n"
              << "$aseanimgrab a.xsi -enum BOTH_RUN_DUAL\n"
              << "$aseanimgrab b.xsi -enum BOTH_WALK1\n"
              << "$aseanimgrab c.xsi -enum BOTH_RUN_DUAL\n"
              << "$aseanimgrabfinalize\n";
        }
        const auto sc = g2::car::parseFile((dd / "t.car").string());
        g2::car::ValidateOptions vo;
        vo.baseDir = dd.string();
        const auto vr = g2::car::validate(sc, (dd / "t.car").string(), vo);

        bool gemeldet = false;
        for (const auto& is : vr.issues)
            if (is.message.find("BOTH_RUN_DUAL") != std::string::npos &&
                is.message.find("2x") != std::string::npos)
                gemeldet = true;
        check(gemeldet, "Doppelung wird als Fehler gemeldet");

        // The unique name must NOT be reported.
        bool falschAlarm = false;
        for (const auto& is : vr.issues)
            if (is.message.find("BOTH_WALK1 steht") != std::string::npos) falschAlarm = true;
        check(!falschAlarm, "eindeutige Namen bleiben unbeanstandet");

        // -additional names count too: they end up in the cfg as well.
        {
            std::ofstream c(dd / "u.car");
            c << "$aseanimgrabinit\n"
              << "$aseanimgrab a.xsi -enum BOTH_STAND1 -additional 0 2 -1 20 BOTH_WALK1\n"
              << "$aseanimgrab b.xsi -enum BOTH_WALK1\n"
              << "$aseanimgrabfinalize\n";
        }
        const auto sc2 = g2::car::parseFile((dd / "u.car").string());
        const auto vr2 = g2::car::validate(sc2, (dd / "u.car").string(), vo);
        bool add2 = false;
        for (const auto& is : vr2.issues)
            if (is.message.find("BOTH_WALK1 steht") != std::string::npos) add2 = true;
        check(add2, "auch -additional-Namen werden geprueft");

        { std::error_code rmEc; std::filesystem::remove_all(dd, rmEc); }
    }

    step("Fehlende Dateien mit Zuordnung melden");
    {
        // "A file is missing" helps nobody with 1393 grabs. What's needed is
        // WHICH sequence is affected and where the file was expected.
        const std::filesystem::path mr = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("missing");
        std::filesystem::create_directories(mr / "models" / "x");
        // One file must be readable: if ALL are missing, build aborts with a
        // different message, and this case would not be tested here.
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
        bo.skipMissing = true;   // so we get the result instead of an exception

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
            // Without -enum the name is derived from the file name.
            check(br.missing[1].sequence == "AUCHWEG", "Sequenzname aus dem Dateinamen");
            check(br.missing[0].line == 2, "Zeile in der .car mitgeliefert");
            check(br.missing[1].line == 3, "auch fuer die zweite");
            check(br.missing[0].file == "models/x/weg.xsi", "Pfad wie in der .car");
        }
        // The old list must still be filled.
        check(br.missingFiles.size() == 2, "Pfadliste bleibt erhalten");

        { std::error_code rmEc; std::filesystem::remove_all(mr, rmEc); }
    }

    step("parallelFor: Threadnummer und Blockvergabe");
    {
        // 1) Every element exactly once.
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

        // 2) The thread number stays within the valid range.
        //
        // This is exactly where the bug was: the number used to live in a
        // thread_local and survived the call. On the second run with FEWER
        // threads it pointed past the end of the counter array.
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

        // 3) A separate counter per thread, without a lock - the sum must
        //    be correct.
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

        // 4) An exception reaches the caller and takes nothing down with it.
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

        // 5) Edge cases must not crash.
        check((g2::parallelFor(0, [](std::size_t) {}, 8), true), "leerer Bereich");
        check((g2::parallelFor(1, [](std::size_t) {}, 8), true), "ein Element, acht Threads");
    }

    step("Auskunft ueber die Laufzeit");
    {
        // Whether the program starts on someone else's machine should be
        // answerable without external tools.
        //
        // On Linux the answer is always "yes"; the check here makes sure the
        // function can be evaluated at all and is fixed at compile time.
        static_assert(g2::staticRuntime() || !g2::staticRuntime(),
                      "staticRuntime muss zur Uebersetzungszeit feststehen");
        check(g2::staticRuntime() == g2::staticRuntime(), "Auskunft ist stabil");
    }

    step("Bonenamen ohne Ruecksicht auf Gross-/Kleinschreibung");
    {
        // Raven's models spell the motion bone "Motion", custom ones often
        // "motion" - that is exactly the case with the SBD humanoid. With an
        // exact comparison only, the ramp calculation doesn't find its bone,
        // the root motion stays in, and the character slides away in game
        // while walking.
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

        // With two bones that differ only in case, the exact match must
        // win.
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
        // writeScript used to output the grabs ONLY after an $aseanimgrabinit.
        // Without addGrabFrame the file looked complete but contained not a
        // single sequence. Now writeScript adds the frame itself.
        g2::car::Script leer;
        g2::car::GrabDirective gd;
        gd.file = "models/x/a.xsi";
        leer.grabs.push_back(gd);

        const std::string ohne = g2::car::writeScript(leer);
        check(ohne.find("$aseanimgrab models/x/a.xsi") != std::string::npos &&
                  ohne.find("$aseanimgrabinit") != std::string::npos &&
                  ohne.find("$aseanimgrabfinalize") != std::string::npos,
              "ohne Rahmen bleibt der Grab erhalten, der Rahmen wird ergaenzt");

        g2::car::addGrabFrame(leer);
        const std::string mit = g2::car::writeScript(leer);
        check(mit.find("$aseanimgrabinit") != std::string::npos, "init geschrieben");
        check(mit.find("$aseanimgrab models/x/a.xsi") != std::string::npos, "Grab geschrieben");
        check(mit.find("$aseanimgrabfinalize") != std::string::npos, "finalize geschrieben");

        // Calling it twice must not duplicate anything.
        g2::car::addGrabFrame(leer);
        const std::string zwei = g2::car::writeScript(leer);
        check(zwei == mit, "zweiter Aufruf aendert nichts");

        // And reading it back in must return the grab.
        const auto zurueck = g2::car::parse(mit, "test.car");
        check(zurueck.grabs.size() == 1, "wieder eingelesen: ein Grab");
    }

    step("Scherung: naechstgelegene Rotation");
    {
        // A matrix with unequal axis lengths AND a slight skew.
        // That is exactly what the local transforms look like when bind poses
        // with different axis lengths are chained - in JK2's _humanoid.gla
        // 46 of the 72 bones hang directly off the rib cage, and it happens
        // a lot there.
        g2::Skeleton s2;
        s2.name = "t2";
        s2.scale = 0.64f;
        for (int i = 0; i < 3; ++i) {
            g2::Bone b;
            b.name = "s" + std::to_string(i);
            b.parent = i == 0 ? -1 : 0;   // flat: everything on the first bone
            b.basePose = g2::Mat3x4::identity();
            // Unequal axis lengths, but orthogonal - as with Raven.
            b.basePose.m[0][0] = 0.64f;
            b.basePose.m[1][1] = 0.696f;
            b.basePose.m[2][2] = 0.64f;
            b.basePose.m[0][3] = static_cast<float>(i) * 30.0f;   // long lever
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

        // Without polar decomposition the error here was above 0.5 - with it
        // it stays in the same order of magnitude as the quantization.
        check(worst2 < 0.2, "flaches Skelett bleibt brauchbar");
        { std::error_code rmEc; std::filesystem::remove_all(tp, rmEc); }
    }

    step("Restbewegung erkennen");
    {
        // A sequence WITHOUT motion must not be reported.
        check(g2::xsiexp::residualMotion(gla, {"ganz", 0, frames}) < 0.05f,
              "unbewegte Sequenz meldet keine Restbewegung");

        // Without a Motion bone in the skeleton there is nothing to measure.
        g2::Skeleton ohne = sk;
        for (auto& b : ohne.bones)
            if (b.name == "Motion") b.name = "kein_motion";
        g2::MdxaFile kopie = gla;
        kopie.skeleton = ohne;
        check(g2::xsiexp::residualMotion(kopie, {"x", 0, frames}) == 0.0f,
              "ohne Motion-Bone wird nichts gemeldet");

        // Nonsensical ranges must not crash.
        check(g2::xsiexp::residualMotion(gla, {"x", 0, 1}) == 0.0f, "ein Frame: nichts zu messen");
        check(g2::xsiexp::residualMotion(gla, {"x", -5, 10}) == 0.0f, "negativer Start abgewiesen");
        check(g2::xsiexp::residualMotion(gla, {"x", 0, frames + 99}) == 0.0f,
              "Bereich ausserhalb abgewiesen");
    }

    step("Templatenamen mit Leerzeichen");
    {
        // Softimage writes scene names with spaces and dots.
        // A parser that expects exactly one token aborts there - with Raven's
        // cutscenes five of 1400 files failed this way, with the message
        // "'{' erwartet nach Template SI_Scene".
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

        // A file without an opening brace must still abort, not keep
        // reading forever.
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

        { std::error_code rmEc; std::filesystem::remove_all(td, rmEc); }
    }

    step("GLA-Name aus -makeskel");
    {
        // The name is stored as a string in the GLA header and tells the
        // engine which skeleton this is. When it came from the reference,
        // EVERY built file carried "models/players/_humanoid/_humanoid" - no
        // matter where it belonged.
        //
        // In game a custom humanoid then identified itself as the standard
        // humanoid, the engine took its animation.cfg and at every slot
        // played whatever animation happened to be there. It looked like
        // broken animations and was just a name.
        g2::Skeleton umbenannt = sk;
        umbenannt.name = "models/players/_humanoid_bdroid/_humanoid";
        const auto w5 = g2::writeMdxa(umbenannt, af);
        const g2::MdxaFile g5 = g2::readMdxa(w5.data);
        check(g5.skeleton.name == "models/players/_humanoid_bdroid/_humanoid",
              "der gesetzte Name steht in der Datei");
        check(g5.skeleton.name != sk.name, "und nicht der der Vorlage");

        // Bones and frames are unaffected by this.
        check(g5.numFrames == frames, "Framezahl unveraendert");
        check(g5.skeleton.bones.size() == sk.bones.size(), "Bonezahl unveraendert");
    }

    step("dotXSI-Fassung 3.0 und 3.5");
    {
        // The difference isn't just the number in the header: v3.0 NAMES its
        // templates, v3.5 leaves them nameless. Until now we wrote named
        // templates under a 3.5 header - 3.0 content with a 3.5 label.
        //
        // Raven's JK2 root.xsi is v3.0 (511 BASEPOSE, 514 SRT, named
        // curves), their JKA animation files are v3.5 with nameless ones.
        g2::xsiexp::ExportOptions o30 = eo, o35 = eo;
        o30.version = g2::xsiexp::ExportOptions::Version::V30;
        o35.version = g2::xsiexp::ExportOptions::Version::V35;

        const std::string t30 = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, o30);
        const std::string t35 = g2::xsiexp::exportSequence(gla, {"v", 0, frames}, o35);

        check(t30.find("xsi 0300txt") == 0, "3.0 schreibt 0300txt");
        check(t35.find("xsi 0350txt") == 0, "3.5 schreibt 0350txt");

        // And the content must match too, not just the header.
        // Check against the ACTUAL first bone name, not against
        // "model_root": the test skeleton uses different names, and a test
        // that checks for a foreign name measures nothing.
        const std::string erster = "SI_FCurve " + sk.bones[0].name + "-";
        check(t30.find(erster) != std::string::npos, "3.0 benennt die Kurven");
        check(t35.find(erster) == std::string::npos, "3.5 benennt sie nicht");
        check(t35.find("SI_FCurve {") != std::string::npos, "3.5 schreibt sie namenlos");

        // Both must be readable again and yield the same result.
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

        { std::error_code rmEc; std::filesystem::remove_all(vd, rmEc); }
    }

    step("Ein-Frame-Sequenzen");
    {
        // Carcass rejects an .xsi with only one frame:
        //   "XSI file-format doesn't support 1-frames files 100% legally"
        // Accordingly, Raven's own 58 single-frame sequences are all
        // -additional sub-ranges of longer files.
        //
        // That's why the frame is doubled: two identical frames, the
        // animation unchanged, and Carcass can read the file.
        const std::string one = g2::xsiexp::exportSequence(gla, {"einzel", 3, 1}, eo);
        check(one.find("0,\n  1,\n") != std::string::npos,
              "Framebereich 0..1 statt 0..0");

        // Both frames must have the same content.
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

        // Multi-frame sequences stay unchanged.
        const std::string many = g2::xsiexp::exportSequence(gla, {"mehr", 0, frames}, eo);
        check(many.find("0,\n  " + std::to_string(frames - 1) + ",\n") != std::string::npos,
              "mehrframige Sequenz unveraendert");

        { std::error_code rmEc; std::filesystem::remove_all(od, rmEc); }
    }

    step("BASEPOSE-Block");
    {
        // Raven's root.xsi has TWO transform blocks per bone: "SRT-" for the
        // pose and "BASEPOSE-" for the bind pose. Carcass reads the second
        // one. We didn't write it - and that's why Carcass reported
        // "Basepose ... differs" for every bone.
        //
        // The content is the WORLD bind pose in dotXSI coordinates, divided by
        // the skeleton scale. Cross-checked against Raven's own file: for
        // lfemurYZ it contains
        //   1.0 1.0 1.0 / 90.174767 3.982727 -74.993988 / 5.643987 55.604065 0.322196
        // and that is exactly what the calculation yields.
        const std::string txt3 = g2::xsiexp::exportSequence(gla, {"bp", 0, frames}, eo);
        check(txt3.find("SI_Transform BASEPOSE-") != std::string::npos,
              "BASEPOSE-Block wird geschrieben");

        // Exactly one per bone, as with Raven.
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
        // The ROOT gets no BASEPOSE block.
        //
        // Raven's files consistently have exactly three more SRT than
        // BASEPOSE - root.xsi 277/274, every animation file 98/95. Missing are
        // model_root, mesh_root and skeleton_root. Of those the GLA only knows
        // model_root, so for us the difference is exactly one.
        check(nBase == nSrt - 1, "genau ein SRT mehr als BASEPOSE");
        check(nBase == sk.bones.size() - 1, "fuer alle ausser der Wurzel");
        check(txt3.find("BASEPOSE-" + sk.bones[0].name) == std::string::npos,
              "die Wurzel hat keinen BASEPOSE-Block");

        // The three variants must differ.
        //
        // Which one Carcass wants is an open question - that's why all three
        // can be generated. A switch that changes nothing would be worse than
        // none: you'd try the same thing three times and take the result for
        // an answer.
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

        // And it must come BEFORE its SRT, as in Raven's files. Don't check
        // against the FIRST SRT: that one belongs to the root, which has no
        // BASEPOSE.
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
        // Carcass reads the SI_Transform block as the bind pose, chains it
        // through the hierarchy and compares the result with the target
        // skeleton. When it held the animated pose of frame 0, it reported
        // "Basepose for bone ... differs" for every bone.
        //
        // Our own importer never uses these values - it evaluates the
        // FCurves - so this went unnoticed until someone ran the file through
        // Raven's Carcass.
        const std::string txt2 = g2::xsiexp::exportSequence(gla, {"ruhe", 0, frames}, eo);
        const std::filesystem::path rp = std::filesystem::temp_directory_path() /
                                         uniqueTestDir("rest");
        std::filesystem::create_directories(rp);
        { std::ofstream o(rp / "r.xsi", std::ios::binary); o << txt2; }

        const auto ra = g2::xsi::loadAnimation(g2::xsi::parseFile((rp / "r.xsi").string()),
                                               (rp / "r.xsi").string());

        // The SRT chain must yield the bind pose, not frame 0.
        //
        // Checked via the existing path: evaluate a copy of the file without
        // FCurves. Then the SRT fallback in localMatrix kicks in, and the
        // result is exactly the rest pose that Carcass reads.
        //
        // Cross-checked against Raven's own Both_forcelandleft1.xsi: there,
        // lower_lumbar has scale 1.0 and translation 9.0. That is exactly what
        // the local bind pose gives - without dividing by the skeleton scale,
        // because it cancels out when forming parent^-1 times child. With the
        // division it would read 1.5625 and 14.0625, and Carcass reported
        // "Basepose ... differs".
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

            // Check translation AND scale.
            //
            // Comparing only the translation was too weak: the skeleton scale
            // at the ROOT went unnoticed, because its translation is zero.
            // Carcass did report it - as "model_root differs by 0.230400", and
            // since the whole chain builds on it, for every bone after that.
            for (int r = 0; r < 3; ++r) {
                const double e = std::fabs(static_cast<double>(local.m[r][3]) - m.m[r][3]);
                if (e > worstRest) {
                    worstRest = e;
                    worstBone = sk.bones[b].name + " (Translation)";
                }
            }
            // Column lengths: the scale, independent of the rotation.
            const int par2 = sk.bones[b].parent;
            for (int c = 0; c < 3; ++c) {
                double lenLocal = 0.0, lenM = 0.0;
                for (int r = 0; r < 3; ++r) {
                    lenLocal += static_cast<double>(local.m[r][c]) * local.m[r][c];
                    lenM += static_cast<double>(m.m[r][c]) * m.m[r][c];
                }
                lenLocal = std::sqrt(lenLocal);
                lenM = std::sqrt(lenM);
                // For the root the skeleton scale is still in the bind pose;
                // the file deliberately doesn't carry it.
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

        { std::error_code rmEc; std::filesystem::remove_all(rp, rmEc); }
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
        // An unknown sequence must return nothing rather than something random.
        check(!g2::xsiexp::readAverageVec(ff.string(), "gibtsnicht").has_value(),
              "unbekannte Sequenz liefert nichts");
    }

    { std::error_code rmEc; std::filesystem::remove_all(tmp, rmEc); }
}

// Follow-up tests for the September 2026 review: every spot checked here was
// a real bug. Each comment states what used to happen.
void testRobustness() {
    namespace fs = std::filesystem;
    section("Speichern verliert nichts");
    {
        // Header comment, end-of-line comment on a non-grab line, unusual
        // flag order, unknown flag, path with spaces, -makeskin, comment at
        // the end of the file. Previously half of this was lost on the first
        // save.
        const std::string src =
            "// Kopf\r\n"
            "$scale 0.64  // Massstab\r\n"
            "$aseanimgrabinit\r\n"
            "$keepmotion\r\n"
            "$aseanimgrab models/a.xsi -loop -1 -qdskipstart -framespeed 30 -additional 0 2 -1 -10 X1 "
            "-qdskipstop -additional 3 1 -1 20 X2 -wunder 7\r\n"
            "$aseanimgrab \"models/my anims/b.xsi\"\r\n"
            "$aseanimgrabfinalize\r\n"
            "$aseanimconvertmdx_noask models/r -makeskin -makeskel models/players/x/x -origin 0 0 24\r\n"
            "// Ende\r\n";
        g2::car::Script sc = g2::car::parse(src);
        check(g2::car::writeScript(sc) == src, "unveraendert gespeichert: zeichengleich");

        // Now modify: the lines are regenerated and must still mean the
        // same thing.
        sc.grabs[0].loop = 5;
        sc.grabs[1].frameSpeed = 10;
        const std::string out = g2::car::writeScript(sc);
        const g2::car::Script back = g2::car::parse(out);
        check(back.grabs.size() == 2, "zwei Grabs");
        check(back.grabs[0].loop == 5, "Aenderung kommt an");
        check(back.grabs[0].additional.size() == 2 && back.grabs[0].additional[0].insideQdSkip &&
                  !back.grabs[0].additional[1].insideQdSkip,
              "qdskip-Klammer genau um den inneren Eintrag");
        check(back.grabs[0].extraArgs == std::vector<std::string>{"-wunder", "7"},
              "unbekanntes Flag bleibt erhalten");
        check(back.grabs[1].file == "models/my anims/b.xsi", "Pfad mit Leerzeichen bleibt ganz");
        check(back.convert && back.convert->makeSkin, "-makeskin bleibt erhalten");
        check(out.find("// Kopf") != std::string::npos && out.find("// Massstab") != std::string::npos &&
                  out.find("// Ende") != std::string::npos,
              "Kopf-, Zeilenend- und Schlusskommentar bleiben erhalten");
    }

    section("$include wird nicht ins Hauptskript kopiert");
    {
        const fs::path dir = fs::temp_directory_path() / uniqueTestDir("include");
        fs::create_directories(dir);
        { std::ofstream f(dir / "inc.car"); f << "$aseanimgrab i.xsi\n"; }
        {
            std::ofstream f(dir / "main.car");
            f << "$aseanimgrabinit\n$aseanimgrab a.xsi\n$include inc.car\n$aseanimgrab b.xsi\n"
                 "$aseanimgrabfinalize\n";
        }
        for (const bool follow : {true, false}) {
            g2::car::ParseOptions po;
            po.followIncludes = follow;
            const auto sc = g2::car::parseFile((dir / "main.car").string(), po);
            const std::string out = g2::car::writeScript(sc);
            std::size_t includes = 0;
            for (std::size_t p = out.find("$include"); p != std::string::npos;
                 p = out.find("$include", p + 1))
                ++includes;
            check(includes == 1 && out.find("i.xsi") == std::string::npos,
                  std::string("$include genau einmal, eingebundener Grab nicht kopiert (") +
                      (follow ? "eingebunden" : "nicht eingebunden") + ")");
            check(out.find("a.xsi") < out.find("$include") && out.find("$include") < out.find("b.xsi"),
                  "Reihenfolge a, $include, b bleibt");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    section("Beschaedigte GLA wird abgewiesen statt gelesen");
    {
        const g2::Skeleton skel = makeSkeleton(3);
        g2::AnimationFrames frames;
        frames.resize(2, 3);
        for (int f = 0; f < 2; ++f)
            for (int b = 0; b < 3; ++b) frames.at(f, b) = g2::Mat3x4::identity();
        const auto good = g2::writeMdxa(skel, frames).data;

        // Parent of the first bone set to 99: previously every evaluation then
        // read outside the skeleton.
        auto badParent = good;
        std::int32_t rel = 0;
        std::memcpy(&rel, badParent.data() + 100, 4);
        const std::int32_t p99 = 99;
        std::memcpy(badParent.data() + 100 + rel + 68, &p99, 4);
        bool threw = false;
        try { (void)g2::readMdxa(badParent); } catch (const std::exception&) { threw = true; }
        check(threw, "ungueltiger Parent-Index wird abgewiesen");

        // Negative offset: converted to size_t it became huge, small again
        // once the length was added, and the size check let it through.
        auto badOfs = good;
        const std::int32_t neg = -3;
        std::memcpy(badOfs.data() + 80, &neg, 4);
        threw = false;
        try { (void)g2::readMdxa(badOfs); } catch (const std::exception&) { threw = true; }
        check(threw, "negativer Frame-Offset wird abgewiesen");

        // Negative offset in the comparison.
        const auto a = g2::readMdxa(good);
        g2::DiffOptions dopt;
        dopt.frameOffsetB = -1;
        threw = false;
        try { (void)g2::diffMdxa(a, a, {}, dopt); } catch (const std::exception&) { threw = true; }
        check(threw, "negativer Frameversatz im Vergleich wird abgewiesen");
    }

    section("Unlesbare .xsi bricht den Bau ab");
    {
        // It used to be silently skipped; all following sequences then sat at
        // different target frames than in the animation.cfg.
        const fs::path dir = fs::temp_directory_path() / uniqueTestDir("unreadable");
        fs::create_directories(dir / "models");
        {
            std::ofstream x(dir / "models" / "ok.xsi", std::ios::binary);
            x << "xsi 0350txt 0032\n\nSI_Model MDL-model_root {\n"
                 "  SI_Transform SRT-model_root {\n    1.0,\n    1.0,\n    1.0,\n"
                 "    0.0,\n    0.0,\n    0.0,\n    0.0,\n    0.0,\n    0.0,\n  }\n"
                 "  SI_FCurve model_root-TRANSLATION-X {\n"
                 "    \"model_root\",\n    \"TRANSLATION-X\",\n    \"LINEAR\",\n"
                 "    1,\n    1,\n    2,\n    0, 0.0,\n    1, 1.0,\n  }\n}\n";
        }
        { std::ofstream x(dir / "models" / "kaputt.xsi", std::ios::binary); x << "kein xsi"; }
        {
            std::ofstream c(dir / "t.car");
            c << "$aseanimgrabinit\n$aseanimgrab models/kaputt.xsi\n$aseanimgrab models/ok.xsi\n"
                 "$aseanimgrabfinalize\n";
        }
        g2::Skeleton ks;
        ks.name = "k";
        ks.scale = 1.0f;
        g2::Bone b;
        b.name = "model_root";
        b.parent = -1;
        b.basePose = g2::Mat3x4::identity();
        ks.bones.push_back(b);

        const auto sc = g2::car::parseFile((dir / "t.car").string());
        g2::car::BuildOptions bo;
        bo.baseDir = dir.string();
        bool threw = false;
        try { (void)g2::car::build(sc, ks, (dir / "t.car").string(), bo); }
        catch (const std::exception&) { threw = true; }
        check(threw, "ohne -skipmissing: Abbruch mit Meldung");

        bo.skipMissing = true;
        threw = false;
        g2::car::BuildResult br;
        try { br = g2::car::build(sc, ks, (dir / "t.car").string(), bo); }
        catch (const std::exception&) { threw = true; }
        check(!threw && br.sequences.size() == 1, "mit -skipmissing: gebaut, Luecke gemeldet");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

// Assimilate's "Model" dialog: $scale, $keepmotion and $pcj are edited in
// place. Unchanged lines must come back byte for byte, changed ones keep their
// end-of-line comment, new ones go where Raven's scripts have them.
void testModelSettings() {
    using namespace g2::car;
    const std::string src =
        "$aseanimgrabinit\r\n"
        "$aseanimgrab models/a.xsi -loop -1\r\n"
        "$aseanimgrab models/b.xsi -framespeed 30\r\n"
        "$scale 0.64  // Massstab\r\n"
        "// Bewegung\r\n"
        "$keepmotion\r\n"
        "$pcj $flatten\r\n"
        "$pcj upper_lumbar  // Ruecken\r\n"
        "$pcj cranium\r\n"
        "$aseanimgrabfinalize\r\n"
        "$aseanimconvertmdx_noask models/root -makeskel models/x/_humanoid -origin 0 0 24\r\n";

    section("Modell-Einstellungen lesen");
    Script s = parse(src);
    ModelSettings m = modelSettingsOf(s);
    check(m.scale && *m.scale == 0.64, "Scale gelesen");
    check(m.keepMotion, "$keepmotion gelesen");
    check((m.pcj == std::vector<std::string>{"$flatten", "upper_lumbar", "cranium"}), "PCJ-Liste in Reihenfolge, $flatten dabei");

    section("Unveraendert anwenden aendert keine Zeile");
    applyModelSettings(s, m);
    check(writeScript(s) == src, "Datei byte-gleich");

    section("Scale aendern");
    {
        Script t = parse(src);
        ModelSettings n = modelSettingsOf(t);
        n.scale = 0.5;
        applyModelSettings(t, n);
        const std::string out = writeScript(t);
        check(out.find("$scale 0.5  // Massstab\r\n") != std::string::npos, "neuer Wert, Kommentar bleibt");
        check(out.find("0.64") == std::string::npos, "alter Wert weg");
        check(t.scale && *t.scale == 0.5, "Bauwert mitgezogen");
        check(modelSettingsOf(parse(out)) == n, "wieder gelesen gleich");
        std::string expect = src;
        expect.replace(expect.find("$scale 0.64"), 11, "$scale 0.5");
        check(out == expect, "nur diese Zeile geaendert");
    }

    section("$keepmotion entfernen, Kommentar darueber bleibt");
    {
        Script t = parse(src);
        ModelSettings n = modelSettingsOf(t);
        n.keepMotion = false;
        applyModelSettings(t, n);
        const std::string out = writeScript(t);
        check(out.find("$keepmotion") == std::string::npos, "Zeile weg");
        check(out.find("// Bewegung\r\n$pcj $flatten") != std::string::npos, "Kommentar wandert zur naechsten Zeile");
        check(!t.keepMotion, "Bauwert mitgezogen");
    }

    section("PCJ-Liste aendern");
    {
        Script t = parse(src);
        ModelSettings n = modelSettingsOf(t);
        n.pcj = {"$flatten", "cranium", "pelvis"};
        applyModelSettings(t, n);
        const std::string out = writeScript(t);
        check(out.find("$pcj $flatten\r\n$pcj cranium\r\n$pcj pelvis\r\n$aseanimgrabfinalize") != std::string::npos,
              "neue Reihenfolge an alter Stelle");
        check(out.find("upper_lumbar") == std::string::npos, "entfernter Eintrag weg");
        check((t.pcjBones == std::vector<std::string>{"cranium", "pelvis"}) && t.pcjFlatten, "Bauwerte mitgezogen");
        check(modelSettingsOf(parse(out)) == n, "wieder gelesen gleich");

        Script u = parse(src);
        ModelSettings k = modelSettingsOf(u);
        k.pcj = {"$flatten", "upper_lumbar"};
        applyModelSettings(u, k);
        check(writeScript(u).find("$pcj upper_lumbar  // Ruecken\r\n") != std::string::npos,
              "behaltener Eintrag behaelt seine Zeile samt Kommentar");
    }

    section("Neu anlegen: vor $aseanimgrabfinalize, Reihenfolge wie bei Raven");
    {
        const std::string bare =
            "$aseanimgrabinit\r\n"
            "$aseanimgrab models/a.xsi\r\n"
            "$aseanimgrabfinalize\r\n"
            "$aseanimconvertmdx_noask models/root -makeskel models/x/_humanoid\r\n";
        Script t = parse(bare);
        ModelSettings n;
        n.scale = 0.64;
        n.keepMotion = true;
        n.pcj = {"$flatten", "cranium"};
        applyModelSettings(t, n);
        const std::string out = writeScript(t);
        check(out == "$aseanimgrabinit\r\n$aseanimgrab models/a.xsi\r\n$scale 0.64\r\n$keepmotion\r\n"
                     "$pcj $flatten\r\n$pcj cranium\r\n$aseanimgrabfinalize\r\n"
                     "$aseanimconvertmdx_noask models/root -makeskel models/x/_humanoid\r\n",
              "an der richtigen Stelle angelegt");
        check(modelSettingsOf(parse(out)) == n, "wieder gelesen gleich");

        // And everything off again: back to the original file.
        Script v = parse(out);
        applyModelSettings(v, ModelSettings{});
        check(writeScript(v) == bare, "alles abgeschaltet = Ausgangsdatei");
    }

    section("Konvertierungszeile");
    {
        Script t = parse(src);
        t.convert->origin.reset();
        t.convert->makeSkin = true;
        t.convert->extraArgs.push_back("-smooth");
        const std::string out = writeScript(t);
        const ConvertDirective c = *parse(out).convert;
        check(!c.origin && c.makeSkin && c.makeSkel == "models/x/_humanoid", "-origin weg, -makeskin dazu");
        check(std::find(c.extraArgs.begin(), c.extraArgs.end(), "-smooth") != c.extraArgs.end(), "-smooth bleibt erhalten");
    }
}

// Regression tests for the findings of the code review of 2026-10-03.
// Each one failed (or crashed) with the code before the fix.
void testReviewBinary() {
    section("REGRESSION: Translation zwischen -512 und -511 bleibt erhalten");
    {
        g2::CompressStats st;
        const float back = g2::unsquashXlatComponent(g2::squashXlatComponent(-511.5f, g2::Rounding::Nearest, st));
        checkNear(back, -511.5, 1.0 / 64.0, "-511.5 kommt als -511.5 zurueck (vorher -511)");
        check(st.xlatClamped == 0, "und gilt nicht als geklemmt");
        const float lo = g2::unsquashXlatComponent(g2::squashXlatComponent(-512.0f, g2::Rounding::Nearest, st));
        checkNear(lo, -512.0, 1e-6, "-512 ist darstellbar");
    }

    section("REGRESSION: skalierte Bone-Matrix behaelt ihren Winkel");
    {
        const auto rx = [](float deg, float scale) {
            const float r = deg * 3.14159265f / 180.0f;
            g2::Mat3x4 m = g2::Mat3x4::identity();
            m.m[1][1] = std::cos(r) * scale; m.m[1][2] = -std::sin(r) * scale;
            m.m[2][1] = std::sin(r) * scale; m.m[2][2] = std::cos(r) * scale;
            m.m[0][0] = scale;
            return m;
        };
        const auto angleOf = [](const g2::Mat3x4& m) {
            return g2::angleBetweenDeg(g2::matrixToQuat(m), g2::Quat{});
        };
        g2::CompressOptions opt;
        for (float sc : {0.64f, 1.5625f}) {
            g2::CompressStats st;
            const g2::Mat3x4 back = g2::uncompressBone(g2::compressBone(rx(90.0f, sc), opt, st));
            // Same angle as the unscaled rotation (the helper measures in its own
            // convention, so compare against the clean case, not a number).
            checkNear(angleOf(back), angleOf(rx(90.0f, 1.0f)), 0.05,
                      "Rx(90) mit Skalierung " + std::to_string(sc) + " behaelt den Winkel (vorher 76/101 Grad)");
            check(st.nonUnitQuat == 1, "als skaliert gezaehlt");
        }
        {
            g2::CompressStats st;
            g2::Mat3x4 zero = g2::Mat3x4::identity();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) zero.m[r][c] = 0.0f;
            const g2::Mat3x4 back = g2::uncompressBone(g2::compressBone(zero, opt, st));
            checkNear(angleOf(back), 0.0, 0.05, "Nullskalierung wird keine Drehung (vorher 180 Grad um Z)");
        }
        {
            g2::CompressStats st;
            g2::Mat3x4 mirror = g2::Mat3x4::identity();
            mirror.m[2][2] = -1.0f;
            g2::compressBone(mirror, opt, st);
            check(st.nonUnitQuat == 1, "Spiegelung wird gemeldet (vorher nicht)");
        }
        {
            // A clean rotation must come out exactly as before the fix.
            g2::CompressStats st;
            const g2::Mat3x4 clean = rx(33.0f, 1.0f);
            const auto a = g2::compressBone(clean, opt, st);
            g2::Quat q = g2::matrixToQuat(clean);
            if (q.w < 0) { q.w = -q.w; q.x = -q.x; q.y = -q.y; q.z = -q.z; }
            const g2::Mat3x4 viaQuat = g2::uncompressBone(a);
            checkNear(angleOf(viaQuat), angleOf(clean), 0.05, "saubere Drehung unveraendert");
            check(st.nonUnitQuat == 0, "saubere Drehung nicht gezaehlt");
        }
    }

    section("REGRESSION: kaputte GLA-Koepfe");
    {
        const g2::Skeleton skel = makeSkeleton(3);
        g2::AnimationFrames frames;
        frames.resize(2, 3);
        for (auto& m : frames.matrices) m = g2::Mat3x4::identity();
        const auto w = g2::writeMdxa(skel, frames);

        // Two billion bones in a file of a few hundred bytes.
        std::vector<std::uint8_t> huge = w.data;
        const std::int32_t many = 0x7FFFFFFF;
        std::memcpy(huge.data() + 84, &many, 4);   // mdxaHeader.numBones
        bool clearError = false;
        try {
            g2::readMdxa(huge);
        } catch (const std::bad_alloc&) {
            clearError = false;
        } catch (const std::exception&) {
            clearError = true;
        }
        check(clearError, "riesige Bone-Anzahl: klare Fehlermeldung statt bad_alloc");

        // Parent cycle 1 -> 2 -> 1.
        std::vector<std::uint8_t> cyc = w.data;
        const std::size_t headerEnd = 100;
        std::int32_t rel1 = 0, rel2 = 0;
        std::memcpy(&rel1, cyc.data() + headerEnd + 4, 4);
        std::memcpy(&rel2, cyc.data() + headerEnd + 8, 4);
        const std::int32_t p1 = 2, p2 = 1;
        std::memcpy(cyc.data() + headerEnd + rel1 + 68, &p1, 4);
        std::memcpy(cyc.data() + headerEnd + rel2 + 68, &p2, 4);
        bool cycleCaught = false;
        try {
            g2::readMdxa(cyc);
        } catch (const std::exception& e) {
            cycleCaught = std::string(e.what()).find("Kreis") != std::string::npos;
        }
        check(cycleCaught, "Parent-Kreis wird erkannt (vorher still als Nullmatrizen)");

        // Diff with a negative target frame in the cfg.
        const g2::MdxaFile f = g2::readMdxa(w.data);
        std::vector<g2::car::Sequence> seqs(2);
        seqs[0].name = "NEG"; seqs[0].targetFrame = -5; seqs[0].frameCount = 10;
        seqs[1].name = "BIG"; seqs[1].targetFrame = 1; seqs[1].frameCount = 0x7FFFFFFF;
        bool survived = true;
        try {
            const auto d = g2::diffMdxa(f, f, seqs);
            // Same file on both sides: no sequence deviates (perSequence lists
            // only deviating ones). The point is that it gets here at all.
            survived = d.perSequence.empty();
        } catch (const std::exception& e) {
            std::printf("  Ausnahme: %s\n", e.what());
            survived = false;
        }
        check(survived, "Diff mit negativem Startframe und riesiger Anzahl: kein Absturz");
    }
}


void testReviewXsi() {
    const auto curve = [](const std::string& interp, int perKey, int keys, const std::string& body) {
        return "xsi 0300txt 0032\n"
               "SI_Scene s { \"FRAMES\", 0.000000, 20.000000, 20.000000, }\n"
               "SI_Model MDL-a {\n"
               "  SI_FCurve { \"a\", \"ROTATION-X\", \"" + interp + "\", 1, " + std::to_string(perKey) + ", " +
               std::to_string(keys) + ", " + body + " }\n}\n";
    };
    const auto keysOf = [](const g2::xsi::AnimFile& a) -> const std::map<int, float>* {
        if (a.nodes.empty()) return nullptr;
        const auto it = a.nodes[0].channels.find("ROTATION-X");
        return it == a.nodes[0].channels.end() ? nullptr : &it->second;
    };

    section("REGRESSION: Kurven mit mehreren Werten pro Key (CUBIC)");
    {
        // frame, value, then four tangent values per key.
        const auto a = g2::xsi::loadAnimation(g2::xsi::parse(curve("CUBIC", 5, 2, "0, 10, 0, 0, 0, 0, 20, 30, 0, 0, 0, 0,")));
        const auto* k = keysOf(a);
        check(k && k->size() == 2 && k->at(0) == 10.0f && k->at(20) == 30.0f,
              "zwei Keys 0->10 und 20->30 (vorher wurden Tangenten zu Keys)");
    }

    section("REGRESSION: CONSTANT ist gestuft");
    {
        const auto a = g2::xsi::loadAnimation(g2::xsi::parse(curve("CONSTANT", 1, 3, "0, 0, 10, 90, 20, -45,")));
        const auto* k = keysOf(a);
        check(k && k->count(5) && k->at(5) == 0.0f && k->at(15) == 90.0f,
              "zwischen den Keys haelt der Wert (vorher linear: 45 bei Frame 5)");
        const auto lin = g2::xsi::loadAnimation(g2::xsi::parse(curve("LINEAR", 1, 3, "0, 0, 10, 90, 20, -45,")));
        const auto* kl = keysOf(lin);
        check(kl && kl->size() == 3, "LINEAR bleibt wie es war");
    }

    section("REGRESSION: beschaedigte Kurven werden gemeldet");
    {
        bool threw = false;
        try {
            g2::xsi::loadAnimation(g2::xsi::parse(curve("LINEAR", 1, 5, "0, 0, 10, 90,")));
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("beschaedigt") != std::string::npos;
        }
        check(threw, "abgeschnittene Kurve: Fehler statt still stehender Bone");
        threw = false;
        try {
            g2::xsi::loadAnimation(g2::xsi::parse(curve("LINEAR", 1, 2, "0, 0, 10, \"-1.#IND00\",")));
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "NaN-Wert aus 3ds Max: Fehler statt still verworfenem Key");
        threw = false;
        try {
            g2::xsi::loadAnimation(g2::xsi::parse(
                "xsi 0300txt 0032\nSI_Scene s { \"FRAMES\", 0.0, 1000000000.0, 20.0, }\n"
                "SI_Model MDL-a { SI_FCurve { \"a\", \"ROTATION-X\", \"LINEAR\", 1, 1, 1, 0, 0, } }\n"));
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "SI_Scene mit einer Milliarde Frames: Fehler statt Terabyte-Anforderung");
    }

    section("REGRESSION: Keys kuerzer als SI_Scene werden gemeldet");
    {
        const auto a = g2::xsi::loadAnimation(g2::xsi::parse(curve("LINEAR", 1, 2, "0, 0, 12, 90,")));
        check(a.sceneRangeDiffers(), "Keys 0..12, SI_Scene 0..20: Warnung (vorher still eingefrorene Frames)");
        const auto full = g2::xsi::loadAnimation(g2::xsi::parse(curve("LINEAR", 1, 2, "0, 0, 20, 90,")));
        check(!full.sceneRangeDiffers(), "passende Keys: keine Warnung");
    }

    section("REGRESSION: Datei mit UTF-8-BOM");
    {
        bool ok = true;
        try {
            g2::xsi::parse("\xEF\xBB\xBF" + curve("LINEAR", 1, 2, "0, 0, 20, 90,"));
        } catch (const std::exception&) {
            ok = false;
        }
        check(ok, "BOM am Anfang wird ignoriert");
    }

    section("REGRESSION: Mesh mit mehreren Dreieckslisten, COLOR-Block, abgeschnittenem Block");
    {
        const std::string head =
            "xsi 0300txt 0032\n"
            "SI_Model MDL-mesh_root {\n"
            "  SI_Model MDL-quad {\n"
            "    SI_Mesh MSH-quad {\n"
            "      SI_Shape SHP-quad-ORG {\n"
            "        3, \"ORDERED\",\n"
            "        4, \"POSITION\", 0,0,0, 1,0,0, 1,1,0, 0,1,0,\n"
            "        1, \"NORMAL\", 0,0,1,\n"
            "        4, \"TEX_COORD_UV\", 0,0, 1,0, 1,1, 0,1,\n"
            "      }\n";
        const std::string tail = "    }\n  }\n}\n";
        g2::xsi::MeshImportOptions mo;
        mo.scale = 1.0f;

        const auto two = g2::xsi::importMesh(g2::xsi::parse(head +
            "      SI_TriangleList t1 { 1, \"NORMAL\", \"m1\", 0,1,2, 0,0,0, }\n"
            "      SI_TriangleList t2 { 1, \"NORMAL\", \"m2\", 0,2,3, 0,0,0, }\n" + tail), mo);
        check(!two.mesh.lods.empty() && two.mesh.lods[0].surfaces.size() == 1 &&
                  two.mesh.lods[0].surfaces[0].triangles.size() == 2,
              "beide Dreieckslisten eingelesen (vorher nur die erste)");

        const auto color = g2::xsi::importMesh(g2::xsi::parse(head +
            "      SI_TriangleList t1 { 1, \"NORMAL|COLOR|TEX_COORD_UV\", \"m\", 0,1,2, 0,0,0, 9,9,9, 1,2,3, }\n" + tail), mo);
        bool uvOk = false;
        if (!color.mesh.lods.empty() && !color.mesh.lods[0].surfaces.empty()) {
            const auto& sf = color.mesh.lods[0].surfaces[0];
            uvOk = sf.vertices.size() == 3 && sf.vertices[0].uv[0] == 1.0f && sf.vertices[0].uv[1] == 1.0f;
        }
        check(uvOk, "COLOR-Block uebersprungen, UVs aus dem richtigen Block (vorher Farbindizes als UV)");

        bool noCrash = true;
        g2::xsi::MeshImportResult cut;
        try {
            cut = g2::xsi::importMesh(g2::xsi::parse(head +
                "      SI_TriangleList t1 { 1, \"NORMAL|TEX_COORD_UV\", \"m\", 0,1,2, 0,0,0, }\n"
                "      SI_TriangleList t2 { 1, \"NORMAL\", \"m\", 0,2,3, 0,0,0, }\n" + tail), mo);
        } catch (const std::exception&) {
            noCrash = false;
        }
        bool warned = false;
        for (const auto& w : cut.stats.warnings) warned = warned || w.find("abgeschnitten") != std::string::npos;
        check(noCrash && warned && !cut.mesh.lods.empty() && cut.mesh.lods[0].surfaces[0].triangles.size() == 1,
              "fehlender UV-Block: Warnung, die heile Liste bleibt (vorher Lesen hinter dem Ende)");
    }

    section("REGRESSION: Surface ueber der Grenze des Spiels");
    {
        // 1002 separate vertices in one surface: Jedi Academy refuses the model.
        std::string src = "xsi 0300txt 0032\nSI_Model MDL-mesh_root {\n  SI_Model MDL-big {\n    SI_Mesh MSH-big {\n"
                          "      SI_Shape SHP-big-ORG {\n        2, \"ORDERED\",\n        1002, \"POSITION\",\n";
        for (int k = 0; k < 1002; ++k) src += "        " + std::to_string(k) + ",0,0,\n";
        src += "        1, \"NORMAL\", 0,0,1,\n      }\n      SI_TriangleList t { 334, \"NORMAL\", \"m\",\n";
        for (int k = 0; k < 1002; ++k) src += std::to_string(k) + ",";
        for (int k = 0; k < 1002; ++k) src += "0,";
        src += " }\n    }\n  }\n}\n";
        g2::xsi::MeshImportOptions mo;
        mo.scale = 1.0f;
        const auto r = g2::xsi::importMesh(g2::xsi::parse(src), mo);
        bool warned = false;
        for (const auto& w : r.stats.warnings) warned = warned || w.find("hoechstens 1000") != std::string::npos;
        check(warned, "1002 Vertices in einer Surface: Warnung (vorher still geschrieben, Spiel laedt nicht)");
    }
}

void testReviewScript() {
    using namespace g2::car;
    const auto same = [](const std::string& src) { return writeScript(parse(src)) == src; };

    section("REGRESSION: alles hinter $exit bleibt beim Speichern erhalten");
    {
        const std::string src = "$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$exit\r\n"
                                "$aseanimgrab geparkt.xsi -loop 5\r\n// Notiz\r\n$aseanimgrabfinalize\r\n";
        check(same(src), "Datei bytegleich (vorher fehlten die Zeilen nach $exit)");
        check(parse(src).grabs.size() == 1, "Carcass-Verhalten: nach $exit wird nichts gebaut");
    }

    section("REGRESSION: Leerzeilen, Einrueckung, LF-Dateien");
    {
        check(same("$aseanimgrabinit\r\n$keepmotion\r\n\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n"),
              "Leerzeile ohne Kommentar bleibt");
        check(same("$aseanimgrabinit\r\n    // eingerueckt\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n"),
              "eingerueckter Kommentar bleibt eingerueckt");
        check(same("$aseanimgrabinit\n$aseanimgrab a.xsi\n// Ende\n$aseanimgrabfinalize\n"),
              "reine LF-Datei bleibt LF");
        check(same("$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n\r\n\r\n"),
              "Leerzeilen am Dateiende bleiben");
        // ... and blank layout lines do not become comment rows.
        const auto sc = parse("$aseanimgrabinit\r\n\r\n// Kopf\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n");
        check(sc.grabs.size() == 1 && sc.grabs[0].commentsBefore.size() == 1,
              "Leerzeile davor ist Layout, kein Kommentar der Animation");
    }

    section("REGRESSION: Kommentar ueber $aseanimgrabfinalize bleibt dort");
    {
        check(same("$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$scale 0.64\r\n$pcj cranium\r\n"
                   "// Ende\r\n$aseanimgrabfinalize\r\n"),
              "nach $scale/$pcj: Kommentar springt nicht hinter die letzte Animation");
    }

    section("REGRESSION: mehrere Konvertierungszeilen, Zeilen zwischen Grabs");
    {
        check(same("$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n"
                   "$aseanimconvertmdx_noask models/a/root -makeskel models/a/a\r\n"
                   "$aseanimconvertmdx_noask models/b/root -makeskel models/b/b\r\n"),
              "beide Konvertierungszeilen bleiben (vorher nur die letzte, an Stelle der ersten)");
        check(same("$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$foo x\r\n$aseanimgrab b.xsi\r\n"
                   "$aseanimgrabfinalize\r\n"),
              "unbekannte Zeile zwischen zwei Grabs bleibt dazwischen");
        Script t = parse("$aseanimgrabinit\r\n$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n"
                         "$aseanimconvertmdx_noask models/a/root -makeskel models/a/a\r\n"
                         "$aseanimconvertmdx_noask models/b/root -makeskel models/b/b\r\n");
        t.convert->makeSkin = true;
        const std::string out = writeScript(t);
        check(out.find("models/a/root -makeskel models/a/a\r\n") != std::string::npos &&
                  out.find("models/b/root -makeskin -makeskel models/b/b") != std::string::npos,
              "Aenderung landet in der letzten, die erste bleibt wie sie war");
    }

    section("REGRESSION: // mitten in einem Pfad ist kein Kommentar");
    {
        const auto sc = parse("$aseanimgrabinit\r\n$aseanimgrab models/p//x.xsi -loop 0\r\n$aseanimgrabfinalize\r\n");
        check(sc.grabs.size() == 1 && sc.grabs[0].trailingComment.empty() && sc.grabs[0].loop == 0,
              "kein Zeilenkommentar erkannt, -loop gelesen");
        const auto sc2 = parse("$aseanimgrabinit\r\n$aseanimgrab a.xsi -loop 0 // echt\r\n$aseanimgrabfinalize\r\n");
        check(sc2.grabs[0].trailingComment == "// echt", "echter Zeilenkommentar weiter erkannt");
    }

    section("REGRESSION: Zahlen ausserhalb des Bereichs");
    {
        for (const char* bad : {"-loop nan", "-loop 1e10", "-framespeed inf", "-additional 0 1e12 -1 20 X"}) {
            bool threw = false;
            try {
                parse(std::string("$aseanimgrabinit\r\n$aseanimgrab a.xsi ") + bad + "\r\n$aseanimgrabfinalize\r\n");
            } catch (const std::exception&) {
                threw = true;
            }
            check(threw, std::string("abgelehnt: ") + bad + " (vorher INT_MIN in animation.cfg)");
        }
    }

    section("REGRESSION: Modell-Dialog und $include");
    {
        namespace fs = std::filesystem;
        const fs::path d = fs::temp_directory_path() / uniqueTestDir("modelinclude");
        fs::create_directories(d);
        {
            std::ofstream inc(d / "pcj.car", std::ios::binary);
            inc << "$pcj $flatten\r\n$pcj upper_lumbar\r\n$keepmotion\r\n";
        }
        const std::string mainSrc = "$aseanimgrabinit\r\n$include pcj.car\r\n$aseanimgrab a.xsi\r\n"
                                    "// Ruecken\r\n$pcj cranium\r\n$aseanimgrabfinalize\r\n";
        {
            std::ofstream m(d / "main.car", std::ios::binary);
            m << mainSrc;
        }
        Script t = parseFile((d / "main.car").string());
        ModelSettings own = modelSettingsOf(t);
        check(own.pcj == std::vector<std::string>{"cranium"} && !own.keepMotion,
              "eigene Zeilen: nur cranium, kein $keepmotion (das steht im include)");
        check(t.pcjFlatten && t.keepMotion && t.pcjBones.size() == 2, "Bau sieht alles, include eingeschlossen");
        applyModelSettings(t, own);
        check(writeScript(t) == mainSrc, "unveraendert angewendet: Datei bytegleich (vorher $flatten verdoppelt)");

        // Remove the own entry: its comment must not get lost.
        own.pcj.clear();
        applyModelSettings(t, own);
        const std::string out = writeScript(t);
        check(out.find("$pcj cranium") == std::string::npos && out.find("// Ruecken") != std::string::npos,
              "eigener Eintrag weg, Kommentar darueber bleibt (vorher verschluckt)");
        check(t.pcjFlatten && t.pcjBones.size() == 1 && t.keepMotion, "Bauwerte = was in den Dateien steht");
        std::error_code ec;
        fs::remove_all(d, ec);
    }

    section("REGRESSION: Kommentar ueber geloeschtem $keepmotion vor dem ersten Grab");
    {
        Script t = parse("$aseanimgrabinit\r\n$scale 0.64\r\n// Wurzel behalten\r\n$keepmotion\r\n"
                         "$aseanimgrab a.xsi\r\n$aseanimgrabfinalize\r\n");
        ModelSettings m = modelSettingsOf(t);
        m.keepMotion = false;
        applyModelSettings(t, m);
        check(writeScript(t).find("// Wurzel behalten\r\n$aseanimgrab a.xsi") != std::string::npos,
              "Kommentar wandert zur Animation statt zu verschwinden");
    }

    section("REGRESSION: doppelte Namen nur einmal gemeldet");
    {
        const auto sc = parse("$aseanimgrabinit\r\n$aseanimgrab a.xsi -enum BOTH_A1\r\n"
                              "$aseanimgrab b.xsi -enum BOTH_A1\r\n$aseanimgrabfinalize\r\n");
        ValidateOptions vo;
        const auto r = validate(sc, "x.car", vo);
        std::size_t dupe = 0;
        for (const auto& is : r.issues)
            if (is.sequence == "BOTH_A1") ++dupe;
        check(dupe == 1, "eine Meldung fuer BOTH_A1 (vorher zwei, Fehlerzahl doppelt): " + std::to_string(dupe));
    }

    section("REGRESSION: -additional ausserhalb der Datei wird beim Bauen gemeldet");
    {
        namespace fs = std::filesystem;
        const fs::path d = fs::temp_directory_path() / uniqueTestDir("addrange");
        fs::create_directories(d / "models");
        {
            std::ofstream f(d / "models" / "w.xsi");
            f << "xsi 0350txt 0032\nSI_Scene s { \"FRAMES\", 1.0, 5.0, 20.0, }\n"
                 "SI_Model MDL-rig.root {\n  SI_Model MDL-rig.a {\n"
                 "    SI_FCurve { \"rig.a\", \"ROTATION-Z\", \"LINEAR\", 1, 1, 5, 1,0, 2,1, 3,2, 4,3, 5,4, }\n  }\n}\n";
        }
        {
            std::ofstream c(d / "t.car");
            c << "$aseanimgrabinit\n$aseanimgrab models/w.xsi -additional 3 4 -1 20 ZU_LANG\n$aseanimgrabfinalize\n";
        }
        g2::Skeleton ref;
        ref.name = "x";
        ref.scale = 1.0f;
        ref.bones.push_back({"root", -1, g2::Mat3x4::identity(), 0});
        ref.bones.push_back({"a", 0, g2::Mat3x4::identity(), 0});
        BuildOptions bo;
        bo.baseDir = d.string();
        const auto br = build(parseFile((d / "t.car").string()), ref, (d / "t.car").string(), bo);
        bool warned = false;
        for (const auto& w : br.warnings) warned = warned || w.find("ZU_LANG") != std::string::npos;
        check(warned, "Teil 3..6 einer 5-Frame-Datei: Warnung (vorher still)");
        std::error_code ec;
        fs::remove_all(d, ec);
    }
}

int main() {
    // Unbuffered output. Otherwise, on a hang or crash you can't tell where it
    // happened: the last printed text is still stuck in the buffer, and the
    // run looks as if it stopped earlier. A test run is short enough that the
    // performance cost doesn't matter.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_trace = !g2::envValue("G2C_TRACE").empty();
    std::thread watchdog(watchdogMain);

    // Guard each section individually.
    //
    // If a test aborts, the tool should SAY which one it was. Previously the
    // output simply ended in the middle of the run, and from "it stops after
    // Skriptpruefung" you can't tell whether that test itself crashed or the
    // next one did on startup.
    const auto run = [](const char* name, void (*fn)()) {
        g_currentTest.store(name);
        g_currentStep.store("(Anfang)");
        // Flush after every section. On a hard crash - such as an access
        // violation that no catch intercepts - the buffered text is otherwise
        // lost, and the output seems to end earlier than the run did. That
        // points the search at the wrong place.
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
    run("testRobustness", testRobustness);
    run("testModelSettings", testModelSettings);
    run("testReviewBinary", testReviewBinary);
    run("testReviewXsi", testReviewXsi);
    run("testReviewScript", testReviewScript);


    g_currentTest.store("(fertig)");
    g_currentStep.store("(fertig)");
    g_watchdogStop.store(true);
    watchdog.join();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " Pruefungen bestanden\n";
    if (g_failures) std::cout << g_failures << " FEHLGESCHLAGEN\n";
    return g_failures ? 1 : 0;
}
