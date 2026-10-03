// Tests for the Carcass features added in October 2026 (GLM reading, model
// scripts, selectable outputs, ...). Included from tests.cpp so the helpers
// there (check, section, ...) are available.

namespace {

g2::Mesh carcassTestMesh(int lods) {
    g2::Mesh m;
    m.name = "models/players/x/model.glm";
    m.animName = "models/players/_humanoid/_humanoid";
    m.numBones = 53;
    for (int L = 0; L < lods; ++L) {
        g2::LOD lod;
        for (int s = 0; s < 3; ++s) {
            g2::Surface surf;
            surf.name = s == 2 ? "*tag" : (s ? "arm" : "body");
            surf.shader = s == 2 ? "[nomaterial]" : "models/players/x/body.tga";
            surf.parentIndex = s ? 0 : -1;
            surf.flags = s == 2 ? g2::fmt::kSurfFlagIsBolt : 0u;
            const int n = 6 - L;
            for (int v = 0; v < n; ++v) {
                g2::Vertex x;
                x.position[0] = static_cast<float>(v) + 0.25f * s;
                x.position[1] = static_cast<float>(L);
                x.normal[2] = 1.0f;
                x.uv[0] = 0.125f * v;
                x.uv[1] = 0.5f;
                // Two weights on two different bones; the stored values are
                // multiples of 1/1023 so they survive the 10-bit packing.
                x.weights.push_back({(v + s) % 7, 700.0f / 1023.0f});
                x.weights.push_back({(v + s) % 7 + 10, 323.0f / 1023.0f});
                surf.vertices.push_back(x);
            }
            for (int t = 0; t + 2 < n; ++t) surf.triangles.push_back({{t, t + 1, t + 2}});
            lod.surfaces.push_back(std::move(surf));
        }
        m.lods.push_back(std::move(lod));
    }
    return m;
}

}  // namespace

void testGlmRead() {
    section("GLM lesen (fuer -dump/info und die Vergleiche mit Carcass)");

    const g2::Mesh m = carcassTestMesh(3);
    const auto w = g2::writeMdxm(m);
    check(g2::isMdxm(w.data), "Ident 2LGM erkannt");

    const g2::MdxmFile f = g2::readMdxm(w.data);
    check(f.mesh.name == m.name && f.mesh.animName == m.animName && f.mesh.numBones == 53,
          "Kopf: Name, GLA-Name, Bones");
    check(f.mesh.lods.size() == 3, "drei LODs gelesen");
    bool same = f.mesh.lods.size() == m.lods.size();
    for (std::size_t l = 0; same && l < m.lods.size(); ++l) {
        for (std::size_t s = 0; same && s < m.lods[l].surfaces.size(); ++s) {
            const auto& a = m.lods[l].surfaces[s];
            const auto& b = f.mesh.lods[l].surfaces[s];
            same = a.name == b.name && a.shader == b.shader && a.flags == b.flags &&
                   a.parentIndex == b.parentIndex && a.vertices.size() == b.vertices.size() &&
                   a.triangles.size() == b.triangles.size();
            for (std::size_t v = 0; same && v < a.vertices.size(); ++v) {
                const auto& va = a.vertices[v];
                const auto& vb = b.vertices[v];
                for (int k = 0; k < 3; ++k)
                    same = same && va.position[k] == vb.position[k] && va.normal[k] == vb.normal[k];
                same = same && va.uv[0] == vb.uv[0] && va.uv[1] == vb.uv[1] &&
                       vb.weights.size() == 2;
                // Bone indices come back global (through the reference table),
                // the weights within the 10-bit resolution.
                for (std::size_t k = 0; same && k < 2; ++k)
                    same = vb.weights[k].boneIndex == va.weights[k].boneIndex &&
                           std::fabs(vb.weights[k].weight - va.weights[k].weight) < 1.0f / 1023.0f;
            }
            for (std::size_t t = 0; same && t < a.triangles.size(); ++t)
                for (int k = 0; k < 3; ++k)
                    same = same && a.triangles[t].indexes[k] == b.triangles[t].indexes[k];
        }
    }
    check(same, "alle Surfaces, Vertices, Gewichte und Dreiecke kommen unveraendert zurueck");

    // Writing what was read gives the identical file.
    const auto again = g2::writeMdxm(f.mesh);
    check(again.data == w.data, "Lesen + Schreiben ist bitgleich");

    // Broken files: every truncation and a few flipped offsets must end in an
    // exception, never in a crash or a read past the buffer.
    bool allThrew = true;
    for (std::size_t cut = 0; cut + 1 < w.data.size(); cut += 37) {
        std::vector<std::uint8_t> d(w.data.begin(), w.data.begin() + static_cast<std::ptrdiff_t>(cut));
        try {
            (void)g2::readMdxm(d);
            allThrew = false;
        } catch (const std::exception&) {
        }
    }
    check(allThrew, "abgeschnittene GLM: immer Fehlermeldung, nie Absturz");

    int survived = 0;
    std::mt19937 rng(7);
    for (int round = 0; round < 400; ++round) {
        std::vector<std::uint8_t> d = w.data;
        for (int k = 0; k < 4; ++k) d[164 + rng() % (d.size() - 164)] = static_cast<std::uint8_t>(rng());
        d[148 + (rng() % 4) * 4 + (rng() % 4)] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        try {
            (void)g2::readMdxm(d);
            ++survived;
        } catch (const std::exception&) {
        }
    }
    check(true, "400 zufaellig beschaedigte GLM ohne Absturz gelesen (" + std::to_string(survived) +
                    " davon noch gueltig)");

    std::vector<std::uint8_t> huge = w.data;
    const std::int32_t big = 0x7fffff00;
    std::memcpy(huge.data() + 152, &big, 4);   // numSurfaces
    bool threw = false;
    try {
        (void)g2::readMdxm(huge);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "riesige Surface-Anzahl: Fehler statt Speicheranforderung");
}

// --- helpers for the Carcass tests -------------------------------------------

namespace {

// A dotXSI animation: rig model_root > pelvis > (lfemur > ltibia, spine >
// face_always_ > jaw), Motion under pelvis. Every node has a BASEPOSE; pelvis
// rotates by degPerFrame about Z.
std::string ctAnim(int frames, float degPerFrame, const std::string& extraUnderSpine = "",
                   const std::string& pelvisName = "pelvis") {
    std::ostringstream f;
    f << "xsi 0350txt 0032\n";
    f << "SI_Scene s {\n  \"FRAMES\",\n  1.000000,\n  " << frames << ".000000,\n  30.000000,\n}\n";
    const auto node = [&](const std::string& name, float ty, const std::string& inner, bool anim) {
        std::ostringstream n;
        n << "SI_Model MDL-rig." << name << " {\n"
          << "  SI_Transform SRT-" << name << " { 1.0,1.0,1.0, 0.0,0.0,0.0, 0.0," << ty << ",0.0, }\n"
          << "  SI_Transform BASEPOSE-" << name << " { 1.0,1.0,1.0, 0.0,0.0,0.0, 0.0," << ty << ",0.0, }\n";
        if (anim) {
            n << "  SI_FCurve { \"rig." << name << "\", \"ROTATION-Z\", \"LINEAR\", 1, 1, " << frames << ",\n";
            for (int i = 1; i <= frames; ++i) n << "    " << i << "," << degPerFrame * static_cast<float>(i - 1) << ",\n";
            n << "  }\n";
        }
        n << inner << "}\n";
        return n.str();
    };
    const std::string jaw = node("jaw", 1.0f, "", false);
    const std::string face = node("face_always_", 1.0f, jaw, false);
    const std::string spine = node("spine", 3.0f, face + extraUnderSpine, false);
    const std::string ltibia = node("ltibia", -2.0f, "", false);
    const std::string lfemur = node("lfemur", -1.0f, ltibia, false);
    const std::string motion = node("Motion", 0.0f, "", false);
    const std::string pelvis = node(pelvisName, 4.0f, lfemur + spine + motion, true);
    f << node("model_root", 0.0f, pelvis, false);
    return f.str();
}

// A mesh file weighting the given bones (one triangle each).
std::string ctMesh(const std::vector<std::string>& bones) {
    std::ostringstream f;
    f << "xsi 0350txt 0032\nSI_Model MDL-rig.model_root {\n  SI_Model MDL-mesh_root {\n";
    for (std::size_t b = 0; b < bones.size(); ++b) {
        f << "    SI_Model MDL-surf" << b << " {\n      SI_Mesh MSH-surf" << b << " {\n"
          << "        SI_Shape SHP { 2, \"ORDERED\", 3, \"POSITION\", 0,0,0, 1,0,0, 0,1,0, 1, \"NORMAL\", 0,0,1, }\n"
          << "        SI_TriangleList t { 1, \"NORMAL\", \"m\", 0,1,2, 0,0,0, }\n      }\n";
        if (b == 0) continue;   // the others go below the first mesh
        f << "    }\n";
    }
    if (!bones.empty()) f << "    }\n";
    f << "  }\n}\nSI_EnvelopeList {\n";
    for (std::size_t b = 0; b < bones.size(); ++b)
        f << "  SI_Envelope { \"MDL-surf" << b << "\", \"MDL-rig." << bones[b] << "\", 3, 0,100.0, 1,100.0, 2,100.0, }\n";
    f << "}\n";
    return f.str();
}

const g2::Bone* ctBone(const g2::Skeleton& s, const std::string& name) {
    for (const auto& b : s.bones)
        if (b.name == name) return &b;
    return nullptr;
}

}  // namespace

void testCarcassScriptSyntax() {
    section("Carcass-Skript: Kommentare ; # /* */, $basedir, $CFGNameFromCAR");
    const std::string text =
        "; Kopf mit Semikolon\r\n"
        "# Kopf mit Raute\r\n"
        "$aseanimgrabinit\r\n"
        "$CFGNameFromCAR\r\n"
        "/* ein Block\r\n"
        "   ueber zwei Zeilen */\r\n"
        "$aseanimgrab models/a/one.xsi -loop 2 ; dahinter\r\n"
        "$basedir c:/anderswo/\r\n"
        "$aseanimgrab two\r\n"
        "$aseanimgrabfinalize\r\n"
        "$aseanimconvertmdx_noask models/a/root -makeskel models/a/a\r\n";
    const auto s = g2::car::parse(text, "syntax.car");
    check(s.grabs.size() == 2, "zwei Grabs trotz Kommentaren");
    check(s.grabs[0].loop && *s.grabs[0].loop == 2, "Wert vor einem ;-Kommentar gelesen");
    check(s.grabs[0].trailingComment.find("dahinter") != std::string::npos, ";-Kommentar am Zeilenende behalten");
    check(s.grabs[0].baseDir.empty() && s.grabs[1].baseDir == "c:/anderswo/",
          "$basedir gilt nur fuer die Grabs danach (wie Carcass)");
    bool unknown = false;
    for (const auto& st : s.statements) unknown = unknown || st.cmd == g2::car::Cmd::Unknown;
    check(!unknown, "$CFGNameFromCAR ist ein bekannter Befehl");
    const std::string back = g2::car::writeScript(s);
    if (back != text) {
        std::printf("  --- erwartet ---\n%s  --- geschrieben ---\n%s", text.c_str(), back.c_str());
    }
    check(back == text, "Speichern gibt die Datei byte-gleich zurueck (alle Kommentarformen)");
    const std::string noNl = "$aseanimgrab a.xsi\r\n$aseanimconvertmdx_noask root -makeskel x/x";
    check(g2::car::writeScript(g2::car::parse(noNl, "x.car")) == noNl,
          "REGRESSION: Datei ohne Zeilenumbruch am Ende bekommt beim Speichern keinen dazu");
}

void testSkeletonBuild() {
    section("Neues Skelett aus den Quellen (wie Carcass)");
    const auto a1 = g2::xsi::loadAnimation(g2::xsi::parse(ctAnim(3, 10.0f)), "a1.xsi");
    // The second file adds a bone "late_always_" under spine.
    const std::string late =
        "SI_Model MDL-rig.late_always_ {\n  SI_Transform BASEPOSE-late_always_ { 1.0,1.0,1.0, 0.0,0.0,0.0, 0.0,9.0,0.0, }\n}\n";
    const auto a2 = g2::xsi::loadAnimation(g2::xsi::parse(ctAnim(2, 5.0f, late)), "a2.xsi");

    g2::SkeletonBuildOptions so;
    so.name = "models/players/x/x";
    so.scale = 0.5f;
    so.keepMotion = true;
    const auto r = g2::buildSkeleton({&a1, &a2}, {"pelvis", "ltibia", "jaw"}, so);
    const auto& sk = r.skeleton;
    std::string order;
    for (const auto& b : sk.bones) order += b.name + " ";
    std::printf("  Bones: %s\n", order.c_str());
    check(sk.bones.size() == 7, "model_root, pelvis, ltibia, face, jaw, Motion, late (lfemur/spine ohne Gewicht fallen weg)");
    check(sk.bones[0].name == "model_root" && sk.bones[1].name == "pelvis", "Reihenfolge der ersten Datei");
    check(sk.bones.back().name == "late", "spaeter auftauchender Bone hinten angehaengt, _always_ abgeschnitten");
    const auto* tib = ctBone(sk, "ltibia");
    check(tib && sk.bones[static_cast<std::size_t>(tib->parent)].name == "pelvis",
          "Kind eines weggefallenen Bones haengt am Grosselternteil");
    const auto* face = ctBone(sk, "face");
    check(face && (face->flags & g2::fmt::kBoneFlagAlwaysXform), "face_always_ -> face mit ALWAYSXFORM");
    const auto* mot = ctBone(sk, "Motion");
    check(mot && (mot->flags & g2::fmt::kBoneFlagAlwaysXform), "Motion mit $keepmotion behalten und markiert");
    checkNear(sk.bones[1].basePose.m[2][3], 0.5 * 4.0, 1e-5, "Bindepose aus BASEPOSE, Y-up -> Z-up, skaliert");
    check(sk.scale == 0.5f && sk.name == "models/players/x/x", "Name und Scale im Kopf");

    // Without $keepmotion the Motion bone goes.
    so.keepMotion = false;
    const auto r2 = g2::buildSkeleton({&a1}, {"pelvis"}, so);
    check(!ctBone(r2.skeleton, "Motion"), "ohne $keepmotion kein Motion-Bone");

    // A weighted bone no animation has: an error, not a silent dropout.
    bool threw = false;
    try {
        (void)g2::buildSkeleton({&a1}, {"pelvis", "gibtsnicht"}, so);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "gewichteter Bone ohne Animation: Fehler");

    // REGRESSION (Carcass B4): a second file spelling PELVIS must not create
    // a second bone and move its children elsewhere.
    const auto a3 = g2::xsi::loadAnimation(g2::xsi::parse(ctAnim(2, 1.0f, "", "PELVIS")), "a3.xsi");
    const auto r3 = g2::buildSkeleton({&a1, &a3}, {"pelvis", "ltibia"}, so);
    const auto* tib3 = ctBone(r3.skeleton, "ltibia");
    check(tib3 && r3.skeleton.bones[static_cast<std::size_t>(tib3->parent)].name == "pelvis",
          "REGRESSION: andere Schreibweise in der zweiten Datei aendert die Hierarchie nicht (Carcass: ja)");

    // $pcj $flatten: everything under the topmost node.
    g2::SkeletonBuildOptions sf = so;
    sf.pcj = {"$flatten"};
    const auto rf = g2::buildSkeleton({&a1}, {"pelvis", "ltibia", "jaw"}, sf);
    bool flat = true;
    for (std::size_t b = 1; b < rf.skeleton.bones.size(); ++b) flat = flat && rf.skeleton.bones[b].parent == 0;
    check(flat, "$pcj $flatten haengt alle Bones an die Wurzel");
    // ... but only from the grab on that follows the $pcj line.
    sf.pcjFrom = {1};
    const auto rf2 = g2::buildSkeleton({&a1}, {"pelvis", "ltibia", "jaw"}, sf);
    check(ctBone(rf2.skeleton, "ltibia")->parent != 0, "$pcj nach den Grabs aendert nichts (wie Carcass)");

    // $bonehiercap: everything below spine goes, the _always_ bone stays.
    g2::SkeletonBuildOptions sc = so;
    sc.caps = {"pelvis"};
    const auto rc = g2::buildSkeleton({&a1}, {"pelvis", "ltibia", "jaw"}, sc);
    check(!ctBone(rc.skeleton, "ltibia") && !ctBone(rc.skeleton, "jaw") && ctBone(rc.skeleton, "face"),
          "$bonehiercap kappt alles darunter ausser _always_-Bones");
    check(rc.capped.size() == 1 && rc.capped[0].first == "pelvis", ".bonecap-Inhalt: ein Eintrag fuer pelvis");
    check(g2::writeBoneCap(rc.capped) == "pelvis ltibia jaw \r\n", ".bonecap im Format von Carcass");

    // keepBoneOrder: same bones, other order -> existing order.
    g2::Skeleton built = r2.skeleton;
    g2::Skeleton existing = built;
    std::reverse(existing.bones.begin() + 1, existing.bones.end());
    for (auto& b : existing.bones) b.parent = b.parent < 0 ? -1 : 0;
    check(g2::keepBoneOrder(built, existing) && built.bones[1].name == existing.bones[1].name,
          "vorhandene GLA mit denselben Bones: deren Reihenfolge bleibt (GLMs bleiben gueltig)");
    g2::Skeleton other = r2.skeleton;
    other.bones.pop_back();
    g2::Skeleton b2 = r2.skeleton;
    check(!g2::keepBoneOrder(b2, other), "andere Bone-Menge: keine Umsortierung");
}

void testFrameStepAndOutputs() {
    section("-framestep, Ausgaben waehlbar, Modell-Skript, Fehler der Convert-Zeile");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / uniqueTestDir("carcass") / "base";
    fs::create_directories(root / "models" / "players" / "t");
    {
        std::ofstream(root / "models" / "players" / "t" / "a.xsi") << ctAnim(10, 3.0f);
        std::ofstream(root / "models" / "players" / "t" / "b.xsi") << ctAnim(3, 3.0f);
        std::ofstream(root / "models" / "players" / "t" / "root.xsi") << ctMesh({"pelvis", "ltibia", "jaw"});
        std::ofstream(root / "models" / "players" / "t" / "t.car")
            << "$aseanimgrabinit\r\n$scale 1.0\r\n$keepmotion\r\n"
               "$aseanimgrab models/players/t/a.xsi -framespeed 30 -loop 4\r\n"
               "$aseanimgrab models/players/t/b.xsi\r\n"
               "$aseanimgrabfinalize\r\n"
               "$aseanimconvertmdx_noask models/players/t/root -makeskel models/players/t/t\r\n";
    }
    const std::string car = (root / "models" / "players" / "t" / "t.car").string();
    const auto script = g2::car::parseFile(car);

    // A new skeleton (no GLA there yet), only GLA + animation.cfg.
    g2::car::JobOptions jo;
    jo.build.baseDir = root.string();
    jo.outputs = g2::car::JobOutputs{true, true, false, false, false, false};
    const auto r = g2::car::runJob(script, car, jo);
    check(r.skeletonBuilt, "ohne vorhandene GLA wird das Skelett neu gebaut (wie Carcass)");
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(root / "models" / "players" / "t")) {
        const auto ext = e.path().extension().string();
        if (ext == ".gla" || ext == ".cfg" || ext == ".glm" || ext == ".frames" || ext == ".txt" || ext == ".skin") ++files;
    }
    check(r.written.size() == 2 && files == 2, "nur GLA und animation.cfg gewaehlt: genau zwei Dateien");

    // Everything, with -framestep 3.
    jo.outputs = g2::car::JobOutputs{};
    jo.build.frameStep = 3;
    const auto r3 = g2::car::runJob(script, car, jo);
    check(fs::exists(root / "models" / "players" / "t" / "t_skip.gla") &&
              fs::exists(root / "models" / "players" / "t" / "t_skip.glm") &&
              fs::exists(root / "models" / "players" / "t" / "t_skip_info.txt"),
          "-framestep: eigene _skip-Namen, auch bei Schritt 2-3 (Carcass: erst ab 3)");
    check(r3.anim.totalFrames() == 4 + 3, "a: 10 Frames -> 4 (0,3,6,9); b: 3 Frames bleiben (nicht laenger als n)");
    check(r3.anim.sequences[0].frameSpeed == 10 && r3.anim.sequences[0].loopFrame == 1,
          "Geschwindigkeit und Loop mit dem Schritt geteilt (Carcass: doppelt so schnell)");
    check(r3.anim.frameBlocks[0].fps == 10, ".frames: fps durch den Schritt geteilt");
    const auto info = g2::readWholeFile((root / "models" / "players" / "t" / "t_skip_info.txt").string());
    check(info.find("GLA Header info:") != std::string::npos && info.find("Complete list of all") != std::string::npos,
          "_info.txt mit GLM- und GLA-Teil");

    // A model script against that GLA: GLM only.
    {
        fs::create_directories(root / "models" / "players" / "m");
        fs::copy_file(root / "models" / "players" / "t" / "root.xsi", root / "models" / "players" / "m" / "root.xsi");
        std::ofstream(root / "models" / "players" / "m" / "model.car")
            << "$aseanimgrabinit\r\n$aseanimgrab_gla models/players/t/t.gla\r\n$aseanimgrabfinalize\r\n"
               "$aseanimconvertmdx_noask models/players/m/root -makeskin\r\n";
    }
    const std::string mcar = (root / "models" / "players" / "m" / "model.car").string();
    g2::car::JobOptions mo;
    mo.build.baseDir = root.string();
    const auto rm = g2::car::runJob(g2::car::parseFile(mcar), mcar, mo);
    check(rm.kind == g2::car::ScriptKind::Model && rm.meshBuilt, "Modell-Skript: GLM gebaut");
    check(fs::exists(root / "models" / "players" / "m" / "model.glm") &&
              fs::exists(root / "models" / "players" / "m" / "model_default.skin") &&
              !fs::exists(root / "models" / "players" / "m" / "model.gla"),
          "model.glm + model_default.skin, keine GLA");
    const auto glm = g2::readMdxm(g2::readWholeFileBytes((root / "models" / "players" / "m" / "model.glm").string()));
    check(glm.mesh.animName == "models/players/t/t" && glm.mesh.name == "model.glm",
          "GLM-Kopf: Name nach der .car, GLA aus $aseanimgrab_gla");
    bool blank = true;
    for (const auto& sf : glm.mesh.lods[0].surfaces) blank = blank && sf.shader.empty();
    check(blank, "-makeskin: Shader in der GLM leer, die Texturen stehen im Skin (wie Carcass)");

    // Convert-line errors and the mixed script.
    const auto fails = [&](const std::string& text, const char* what) {
        const std::string p = (root / "models" / "players" / "t" / "bad.car").string();
        std::ofstream(p) << text;
        bool threw = false;
        try {
            g2::car::JobOptions b;
            b.build.baseDir = root.string();
            (void)g2::car::runJob(g2::car::parseFile(p), p, b);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, what);
    };
    fails("$aseanimgrab models/players/t/a.xsi\r\n$aseanimconvertmdx_noask models/players/t/root -makeskel models/players/t/t -scale 2\r\n",
          "-scale auf der Convert-Zeile: Fehler mit Hinweis auf $scale");
    fails("$aseanimgrab models/players/t/a.xsi\r\n$aseanimconvertmdx_noask models/players/t/root -makeskel models/players/t/t -bogus\r\n",
          "unbekannte Angabe auf der Convert-Zeile: Fehler (Carcass: Fehler mit verstuemmeltem Namen)");
    fails("$aseanimgrab_gla models/players/t/t.gla\r\n$aseanimgrab models/players/t/a.xsi\r\n$aseanimconvertmdx_noask models/players/t/root\r\n",
          "$aseanimgrab_gla und $aseanimgrab gemischt: Fehler");

    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
}

void testGlmWeightsLikeCarcass() {
    section("GLM-Gewichte wie Carcass");
    g2::Mesh m = carcassTestMesh(1);
    auto& v = m.lods[0].surfaces[0].vertices[0];
    // 30/30/20: not renormalised, equal weights keep their order.
    v.weights = {{3, 0.3f}, {1, 0.3f}, {5, 0.2f}};
    const auto w = g2::writeMdxm(m);
    const auto f = g2::readMdxm(w.data);
    const auto& rv = f.mesh.lods[0].surfaces[0].vertices[0];
    check(rv.weights.size() == 3 && rv.weights[0].boneIndex == 3 && rv.weights[1].boneIndex == 1,
          "gleiche Gewichte: stabile Reihenfolge (Carcass std::list::sort)");
    check(std::fabs(rv.weights[0].weight - 307.0f / 1023.0f) < 1e-6f && std::fabs(rv.weights[1].weight - 307.0f / 1023.0f) < 1e-6f,
          "nicht renormiert: 0.3 -> 307");
    // A weight that rounds to 0 ends the list - but never the first one
    // (Carcass wrote 0xC0000000 then: four weights, all zero).
    v.weights = {{2, 0.0001f}};
    const auto w2 = g2::writeMdxm(m);
    const auto f2 = g2::readMdxm(w2.data);
    check(f2.mesh.lods[0].surfaces[0].vertices[0].weights.size() == 1,
          "REGRESSION: einziges Gewicht rundet auf 0 -> ein Gewicht, nicht vier (Carcass-Fehler)");
}

void testMeshLikeCarcass() {
    section("GLM aus dotXSI wie Carcass: Zusammenlegen nach Wert, LODs, Tags, Glaetten");
    // Two triangles sharing an edge, but with SEPARATE position indices for
    // the shared points: Carcass merges by value.
    const std::string src =
        "xsi 0350txt 0032\nSI_Model MDL-mesh_root {\n"
        "  SI_Model MDL-body {\n    SI_Mesh MSH-body {\n"
        "      SI_Shape SHP { 2, \"ORDERED\", 6, \"POSITION\", 0,0,0, 1,0,0, 0,1,0, 1,0,0, 0,1,0, 1,1,0,\n"
        "                     1, \"NORMAL\", 0,0,1, }\n"
        "      SI_TriangleList t { 2, \"NORMAL\", \"m\", 0,1,2, 3,5,4, 0,0,0, 0,0,0, }\n    }\n"
        "    SI_Model MDL-body_1 {\n      SI_Mesh MSH-b1 {\n"
        "        SI_Shape SHP { 2, \"ORDERED\", 3, \"POSITION\", 0,0,0, 1,0,0, 0,1,0, 1, \"NORMAL\", 0,0,1, }\n"
        "        SI_TriangleList t { 1, \"NORMAL\", \"m\", 0,1,2, 0,0,0, }\n      }\n    }\n"
        "    SI_Model MDL-bolt_hand {\n      SI_Mesh MSH-tag {\n"
        "        SI_Shape SHP { 2, \"ORDERED\", 3, \"POSITION\", 0,0,0, 4,0,0, 0,2,0, 1, \"NORMAL\", 0,0,1, }\n"
        "        SI_TriangleList t { 1, \"NORMAL\", \"m\", 0,1,2, 0,0,0, }\n      }\n    }\n"
        "    SI_Model MDL-bolt_hand_1 {\n      SI_Mesh MSH-tag1 {\n"
        "        SI_Shape SHP { 2, \"ORDERED\", 3, \"POSITION\", 0,0,0, 4,0,0, 0,2,0, 1, \"NORMAL\", 0,0,1, }\n"
        "        SI_TriangleList t { 1, \"NORMAL\", \"m\", 0,1,2, 0,0,0, }\n      }\n    }\n"
        "  }\n}\n"
        "SI_EnvelopeList {\n"
        "  SI_Envelope { \"MDL-body\", \"MDL-rig.pelvis\", 6, 0,100.0, 1,100.0, 2,100.0, 3,100.0, 4,100.0, 5,100.0, }\n"
        "  SI_Envelope { \"MDL-body_1\", \"MDL-rig.pelvis\", 3, 0,100.0, 1,100.0, 2,100.0, }\n"
        "  SI_Envelope { \"MDL-bolt_hand\", \"MDL-rig.pelvis\", 3, 0,100.0, 1,100.0, 2,100.0, }\n"
        "  SI_Envelope { \"MDL-bolt_hand_1\", \"MDL-rig.pelvis\", 3, 0,100.0, 1,100.0, 2,100.0, }\n"
        "}\n";
    g2::xsi::MeshImportOptions mo;
    mo.boneNames = {"model_root", "pelvis"};
    mo.modelName = "model.glm";
    const auto r = g2::xsi::importMesh(g2::xsi::parse(src), mo);
    check(r.mesh.lods.size() == 2, "\"_1\"-Meshes bilden LOD 1");
    const auto& body = r.mesh.lods[0].surfaces[0];
    check(body.vertices.size() == 4, "gleiche Punkte aus verschiedenen Positionsindizes zusammengelegt (4 statt 6)");
    const auto& tag = r.mesh.lods[0].surfaces[1];
    check(tag.name == "*hand" && (tag.flags & g2::fmt::kSurfFlagIsBolt), "bolt_hand -> *hand, Tag");
    // Corners grabbed 0,2,1: (0,0,0) (0,2,0) (4,0,0). The hypotenuse is the
    // edge from the second to the third, so (0,2,0) comes first - in Z-up
    // (x, -z, y) that is (0,0,2); then (4,0,0) for the medium edge.
    check(tag.vertices.size() == 3 && std::fabs(tag.vertices[0].position[2] - 2.0f) < 1e-6f &&
              std::fabs(tag.vertices[1].position[0] - 4.0f) < 1e-6f,
          "Tag-Vertices in der Reihenfolge Hypotenuse, mittlere, kurze Kante");
    check(r.mesh.lods[1].surfaces[0].name == "body", "LOD-1-Surface in LOD-0-Reihenfolge, Name ohne _1");

    // Equilateral tag: no direction - an error, as in Carcass.
    std::string eq = src;
    const std::string tri = "0,0,0, 4,0,0, 0,2,0,";
    eq.replace(eq.find(tri, eq.find("MDL-bolt_hand {")), tri.size(), "0,0,0, 2,0,0, 1,1.7320508,0,");
    bool threw = false;
    try {
        (void)g2::xsi::importMesh(g2::xsi::parse(eq), mo);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "gleichseitiger Tag: Fehler statt zufaelliger Ausrichtung");

    // -smooth: back-to-back sheets keep their own sides.
    const std::string sheet =
        "xsi 0350txt 0032\nSI_Model MDL-mesh_root {\n  SI_Model MDL-s {\n    SI_Mesh MSH-s {\n"
        "      SI_Shape SHP { 2, \"ORDERED\", 3, \"POSITION\", 0,0,0, 1,0,0, 0,1,0, 2, \"NORMAL\", 0,0,1, 0,0,-1, }\n"
        "      SI_TriangleList t { 2, \"NORMAL\", \"m\", 0,1,2, 0,2,1, 0,0,0, 1,1,1, }\n    }\n  }\n}\n"
        "SI_EnvelopeList {\n  SI_Envelope { \"MDL-s\", \"MDL-rig.pelvis\", 3, 0,100.0, 1,100.0, 2,100.0, }\n}\n";
    g2::xsi::MeshImportOptions sm = mo;
    sm.smooth = true;
    const auto rs = g2::xsi::importMesh(g2::xsi::parse(sheet), sm);
    bool sides = true;
    for (const auto& vx : rs.mesh.lods[0].surfaces[0].vertices)
        sides = sides && std::fabs(std::fabs(vx.normal[1]) + std::fabs(vx.normal[2]) - 1.0f) < 1e-4f;
    check(sides, "REGRESSION -smooth: doppelseitige Flaeche behaelt ihre Normalen (Carcass: Null-Normalen)");
    sm.carcassSmooth = true;
    const auto rc = g2::xsi::importMesh(g2::xsi::parse(sheet), sm);
    check(rc.mesh.lods[0].surfaces[0].vertices.size() == 3, "-carcass: Carcass' Glaetten, beide Seiten verschmolzen");
}

void testAseAndMdr() {
    section("ASE lesen, GLM und MDR daraus");
    const std::string ase =
        "*3DSMAX_ASCIIEXPORT 200\n"
        "*MATERIAL_LIST {\n *MATERIAL_COUNT 1\n *MATERIAL 0 {\n  *MAP_DIFFUSE {\n   *BITMAP \"C:\\Game\\base\\models\\x\\Skin Name.TGA\"\n  }\n }\n}\n"
        "*GEOMOBJECT {\n *NODE_NAME \"u_body part\"\n *MESH {\n  *MESH_NUMVERTEX 3\n  *MESH_NUMFACES 1\n"
        "  *MESH_VERTEX_LIST {\n   *MESH_VERTEX 0 0 0 0\n   *MESH_VERTEX 1 1 0 0\n   *MESH_VERTEX 2 0 1 0\n  }\n"
        "  *MESH_FACE_LIST {\n   *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 1 *MESH_MTLID 0\n  }\n"
        "  *MESH_NUMTVERTEX 3\n  *MESH_TVERTLIST {\n   *MESH_TVERT 0 0 0 0\n   *MESH_TVERT 1 1 0 0\n   *MESH_TVERT 2 0 1 0\n  }\n"
        "  *MESH_NUMTVFACES 1\n  *MESH_TFACELIST {\n   *MESH_TFACE 0 0 1 2\n  }\n }\n"
        " *MESH_WEIGHTS {\n  *MESH_NUMVERTEX 3\n  *MESH_NUMBONE 1\n  *MESH_BONE_LIST {\n   *MESH_BONE_NAME 0 \"pelvis\"\n  }\n"
        "  *MESH_BONE_VERTEX_LIST {\n"
        "   *MESH_BONE_VERTEX 0 0 0 0 0 1.0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0\n"
        "   *MESH_BONE_VERTEX 1 1 0 0 0 1.0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0\n"
        "   *MESH_BONE_VERTEX 2 0 1 0 0 1.0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0\n"
        "  }\n }\n *MATERIAL_REF 0\n}\n"
        "*GEOMOBJECT {\n *NODE_NAME \"Bip01 Spine\"\n *MESH {\n  *MESH_NUMVERTEX 0\n  *MESH_NUMFACES 0\n }\n}\n";
    const auto sc = g2::ase::parse(ase, "C:/Game/base");
    check(sc.objects.size() == 1, "Biped-Hilfsobjekt weggelassen (Carcass wollte es, schaffte es nie)");
    check(sc.objects[0].name == "u_body part", "Name in Anfuehrungszeichen ganz gelesen (Carcass: abgeschnitten)");
    check(sc.materials.size() == 1 && sc.materials[0] == "models/x/skin name.tga",
          "Textur unter base/ mit Leerzeichen gelesen, klein, relativ");
    check(sc.bones.size() == 1 && sc.bones[0] == "pelvis", "Bone-Liste");

    g2::xsi::MeshImportOptions mo;
    mo.boneNames = {"model_root", "pelvis"};
    const auto glm = g2::xsi::importMeshAse(sc, mo);
    check(glm.mesh.lods.size() == 1 && glm.mesh.lods[0].surfaces[0].vertices.size() == 3, "GLM aus ASE: 3 Vertices");
    const auto& nv = glm.mesh.lods[0].surfaces[0].vertices[0].normal;
    check(std::fabs(std::fabs(nv[2]) - 1.0f) < 1e-5f, "Normalen aus den Dreiecken berechnet");

    // An object without weights: an error (Carcass put every point at 0,0,0).
    std::string noW = ase;
    noW.erase(noW.find(" *MESH_WEIGHTS"), noW.find(" *MATERIAL_REF") - noW.find(" *MESH_WEIGHTS"));
    bool threw = false;
    try {
        (void)g2::ase::parse(noW, "C:/Game/base");
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "ASE-Objekt ohne *MESH_WEIGHTS: Fehler");

    // MDR: one bone, two frames, the bone moves by +1 in x.
    g2::Skeleton sk;
    sk.bones.push_back({"model_root", -1, g2::Mat3x4::identity(), 0});
    sk.bones.push_back({"pelvis", 0, g2::Mat3x4::identity(), 0});
    g2::ase::MdrInput mi;
    mi.scene = &sc;
    mi.skeleton = &sk;
    g2::Mat3x4 moved = g2::Mat3x4::identity();
    moved.m[0][3] = 1.0f;
    mi.frames = {{g2::Mat3x4::identity(), g2::Mat3x4::identity()}, {g2::Mat3x4::identity(), moved}};
    mi.name = "x.mdr";
    const auto mdr = g2::ase::buildMdr(mi);
    check(mdr.bones == 1 && mdr.frames == 2 && mdr.surfaces == 1, "MDR: 1 Bone (nur die benutzten), 2 Frames, 1 Surface");
    std::int32_t ofsFrames = 0;
    std::memcpy(&ofsFrames, mdr.data.data() + 80, 4);
    check(std::memcmp(mdr.data.data(), "RDM5", 4) == 0 && ofsFrames == 104,
          "RDM5, unkomprimierte Frames (Carcass' komprimierte liest kein Spiel)");
    float maxs1x = 0.0f;
    std::memcpy(&maxs1x, mdr.data.data() + 104 + (56 + 48) + 12, 4);
    check(std::fabs(maxs1x - 2.0f) < 1e-5f, "Frame-Bounds aus den bewegten Vertices");
    check(mdr.skin == "u_body part,models/x/skin name.tga\r\n", "Skin-Zeile");
}

// Opt-in: G2C_CAR_CORPUS=<text file with one .car path per line> (relative
// paths against G2C_CAR_ROOT). Every script must come back byte for byte from
// writeScript - the files are only read.
void testCarCorpus() {
    const std::string list = g2::envValue("G2C_CAR_CORPUS");
    if (list.empty()) return;
    section("Echte .car-Dateien: Lesen + Speichern byte-gleich");
    const std::string base = g2::envValue("G2C_CAR_ROOT");
    std::ifstream in(list);
    std::string line;
    std::size_t n = 0, same = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string path = base.empty() ? line : base + "/" + line;
        std::string text;
        try {
            text = g2::readWholeFile(path);
        } catch (const std::exception&) {
            continue;
        }
        ++n;
        g2::car::ParseOptions po;
        po.followIncludes = false;
        const auto sc = g2::car::parse(text, path, po);
        if (g2::car::writeScript(sc) == text) ++same;
        else {
            std::printf("  anders: %s\n", path.c_str());
            const std::string w = g2::car::writeScript(sc);
            std::size_t k = 0;
            while (k < w.size() && k < text.size() && w[k] == text[k]) ++k;
            const std::size_t from = k > 80 ? k - 80 : 0;
            std::printf("    Original:    [%s]\n    Geschrieben: [%s]\n", text.substr(from, 160).c_str(),
                        w.substr(from, 160).c_str());
        }
    }
    check(n > 0 && same == n, std::to_string(same) + " von " + std::to_string(n) + " .car byte-gleich zurueck");
}
