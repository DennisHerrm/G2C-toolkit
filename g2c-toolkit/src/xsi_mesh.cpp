#include "g2/xsi_mesh.h"

#include "g2/readfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <unordered_map>

namespace g2::xsi {
namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

Mat3x4 eulerXYZ(float rx, float ry, float rz) {
    const double a = rx * kDeg2Rad, b = ry * kDeg2Rad, c = rz * kDeg2Rad;
    const double cx = std::cos(a), sx = std::sin(a);
    const double cy = std::cos(b), sy = std::sin(b);
    const double cz = std::cos(c), sz = std::sin(c);
    Mat3x4 m{};
    m.m[0][0] = static_cast<float>(cz * cy);
    m.m[0][1] = static_cast<float>(cz * sy * sx - sz * cx);
    m.m[0][2] = static_cast<float>(cz * sy * cx + sz * sx);
    m.m[1][0] = static_cast<float>(sz * cy);
    m.m[1][1] = static_cast<float>(sz * sy * sx + cz * cx);
    m.m[1][2] = static_cast<float>(sz * sy * cx - cz * sx);
    m.m[2][0] = static_cast<float>(-sy);
    m.m[2][1] = static_cast<float>(cy * sx);
    m.m[2][2] = static_cast<float>(cy * cx);
    return m;
}

// Y-hoch nach Z-hoch, dieselbe Konvention wie bei den Bones.
void toGlmSpace(const float in[3], float scale, float out[3]) {
    out[0] = scale * in[0];
    out[1] = scale * -in[2];
    out[2] = scale * in[1];
}

std::string shortName(const std::string& full) {
    const std::size_t dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

std::string stripPrefix(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.rfind(prefix, 0) == 0 ? s.substr(n) : s;
}

// Ein Attributblock aus SI_Shape: Anzahl, Bezeichner, dann die Werte.
struct ShapeArray {
    std::string        kind;   // POSITION, NORMAL, TEX_COORD_UV0, ...
    std::vector<float> values;
    int                stride = 3;

    std::size_t count() const { return values.size() / static_cast<std::size_t>(stride); }
};

std::vector<ShapeArray> parseShape(const Template& shape) {
    // Aufbau: <anzahlArrays>, "ORDERED", dann je Array:
    //         <n>, "<kind>", <n*stride Werte>
    std::vector<ShapeArray> out;
    std::size_t i = 0;
    const auto& v = shape.values;

    // Ueber die fuehrende Anzahl und "ORDERED" hinweggehen.
    while (i < v.size() && !v[i].asNumber()) ++i;
    if (i < v.size()) ++i;
    while (i < v.size() && v[i].text() == "ORDERED") ++i;

    while (i + 1 < v.size()) {
        const auto n = v[i].asInt();
        if (!n || *n < 0) { ++i; continue; }
        const std::string kind = v[i + 1].text();
        if (kind.empty() || v[i + 1].asNumber()) { ++i; continue; }

        ShapeArray a;
        a.kind = kind;
        a.stride = (kind.rfind("TEX_COORD", 0) == 0) ? 2 : 3;
        const std::size_t need = static_cast<std::size_t>(*n) * static_cast<std::size_t>(a.stride);
        i += 2;

        // Nach Anzahl und Bezeichner koennen weitere Zeichenketten folgen.
        // TEX_COORD_UV0 traegt zusaetzlich den Projektionsnamen:
        //
        //     609,
        //     "TEX_COORD_UV0",
        //     "Texture_Projection",     <-- zusaetzlich
        //     0.127449,0.841402,
        //
        // Wer den nicht ueberliest, liest ab hier um eine Stelle versetzt.
        // Jede Ecke bekommt dann eine falsche UV, und weil die
        // Vertexzusammenfassung UV-Gleichheit verlangt, faellt anschliessend
        // gar nichts mehr zusammen. Der Fehler tarnt sich weit hinten als
        // scheinbares Toleranzproblem: keine noch so grosse Normaltoleranz
        // aendert etwas, weil die UV-Bedingung davor schon alles blockiert.
        //
        // Deshalb allgemein: ueberspringen, was keine Zahl ist.
        while (i < v.size() && !v[i].asNumber()) ++i;

        a.values.reserve(need);
        for (std::size_t k = 0; k < need && i < v.size(); ++k, ++i) {
            const auto d = v[i].asNumber();
            a.values.push_back(d ? static_cast<float>(*d) : 0.0f);
        }
        out.push_back(std::move(a));
    }
    return out;
}

const ShapeArray* findArray(const std::vector<ShapeArray>& arrays, const char* prefix) {
    for (const auto& a : arrays)
        if (a.kind.rfind(prefix, 0) == 0) return &a;
    return nullptr;
}

// Ein Eintrag der Envelope-Liste: welches Mesh, welcher Bone, welche
// Vertices mit welchem Gewicht.
struct EnvelopeEntry {
    std::string      mesh;
    std::string      bone;
    std::vector<std::pair<int, float>> weights;   // Positionsindex, Prozent
};

std::vector<EnvelopeEntry> parseEnvelopes(const Document& doc) {
    std::vector<EnvelopeEntry> out;
    const Template* list = doc.findDeep("SI_EnvelopeList");
    if (!list) return out;

    for (const Template* e : list->findAll("SI_Envelope")) {
        if (e->values.size() < 3) continue;
        EnvelopeEntry en;
        en.mesh = shortName(stripPrefix(e->values[0].text(), "MDL-"));
        en.bone = shortName(stripPrefix(e->values[1].text(), "MDL-"));
        const auto n = e->values[2].asInt();
        if (!n || *n <= 0) continue;

        en.weights.reserve(static_cast<std::size_t>(*n));
        for (std::size_t i = 3; i + 1 < e->values.size(); i += 2) {
            const auto idx = e->values[i].asInt();
            const auto w = e->values[i + 1].asNumber();
            if (!idx || !w) continue;
            // Gewichte stehen in Prozent. Nullgewichte sind ausdruecklich
            // aufgefuehrt und gehoeren verworfen.
            if (*w <= 0.0) continue;
            en.weights.emplace_back(static_cast<int>(*idx), static_cast<float>(*w * 0.01));
        }
        out.push_back(std::move(en));
    }
    return out;
}

// Shader aus XSI_CustomPSet <mesh>.Game { "NODE", 1, "Shader","Text","<pfad>" }
std::map<std::string, std::string> parseShaders(const Template& t,
                                                std::map<std::string, std::string>& out) {
    if (t.type == "XSI_CustomPSet" && t.name.size() > 5) {
        std::string nm = t.name;
        const std::string suffix = ".Game";
        if (nm.size() > suffix.size() && nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) == 0) {
            nm = nm.substr(0, nm.size() - suffix.size());
            for (std::size_t i = 0; i + 2 < t.values.size(); ++i) {
                if (t.values[i].text() == "Shader" && t.values[i + 1].text() == "Text") {
                    // Manche XSI_CustomPSet-Eintraege schreiben Windows-Pfade
                    // mit Backslashes. In der GLM stehen durchgehend
                    // Schraegstriche — sonst findet die Engine die Textur
                    // nicht, und ModView meldet sie als fehlend.
                    std::string path = t.values[i + 2].text();
                    std::replace(path.begin(), path.end(), '\\', '/');
                    out[shortName(nm)] = path;
                }
            }
        }
    }
    for (const auto& c : t.children) parseShaders(c, out);
    return out;
}

// Schluessel fuer die Vertexzusammenfassung.
//
// NICHT die Attributindizes: in Ravens Dateien ist der Normalenindex je Ecke
// fortlaufend (0,1,2 / 3,4,5 / ...), sodass ueber Indizes nie etwas
// zusammenfaellt — bei "hips" kaemen 609 statt 132 Vertices heraus, also
// genau drei je Dreieck.
//
// Zusammengefasst wird nach WERT: gleiche Position, gleiche Normale, gleiche
// UV ergeben denselben Vertex. Die Werte werden dafuer auf ein feines Gitter
// gerundet, sonst verhindert Rechenrauschen im letzten Bit jede Uebereinstimmung.
struct VertexKey {
    int pos = 0;
    int nx = 0, ny = 0, nz = 0;
    int u = 0, v = 0;

    bool operator==(const VertexKey& o) const noexcept {
        return pos == o.pos && nx == o.nx && ny == o.ny && nz == o.nz && u == o.u && v == o.v;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const noexcept {
        std::size_t h = 1469598103934665603ull;
        for (int v : {k.pos, k.nx, k.ny, k.nz, k.u, k.v}) {
            h ^= static_cast<std::size_t>(static_cast<unsigned>(v));
            h *= 1099511628211ull;
        }
        return h;
    }
};

// Normalen auf rund 1/16384, UVs auf rund 1/65536 quantisieren. Fein genug,
// dass echte Kanten getrennt bleiben, grob genug gegen Rundungsrauschen.
inline int quantN(float f) { return static_cast<int>(std::lround(f * 16384.0f)); }
inline int quantUV(float f) { return static_cast<int>(std::lround(f * 65536.0f)); }

}  // namespace

std::string surfaceNameToGlm(const std::string& xsiName) {
    std::string s = xsiName;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.rfind("bolt_", 0) == 0) s = "*" + s.substr(5);
    return s;
}

std::uint32_t surfaceFlagsFromName(const std::string& glmName) {
    std::uint32_t f = 0;
    if (!glmName.empty() && glmName[0] == '*') f |= fmt::kSurfFlagIsBolt;
    const std::string suffix = "_off";
    if (glmName.size() > suffix.size() &&
        glmName.compare(glmName.size() - suffix.size(), suffix.size(), suffix) == 0)
        f |= fmt::kSurfFlagOff;
    return f;
}

MeshImportResult importMesh(const Document& doc, const MeshImportOptions& opt) {
    MeshImportResult res;
    res.mesh.name = opt.modelName;
    res.mesh.animName = opt.animName;
    res.mesh.numBones = static_cast<int>(opt.boneNames.size());

    // Bonename -> Index. Aliase zeigen von der Referenz in die dotXSI, hier
    // wird umgekehrt nachgeschlagen.
    std::map<std::string, int> boneIndex;
    for (std::size_t i = 0; i < opt.boneNames.size(); ++i) {
        boneIndex[opt.boneNames[i]] = static_cast<int>(i);
        const auto al = opt.aliases.find(opt.boneNames[i]);
        if (al != opt.aliases.end()) boneIndex[al->second] = static_cast<int>(i);
    }

    const auto envelopes = parseEnvelopes(doc);
    std::map<std::string, std::string> shaders;
    for (const auto& r : doc.roots) parseShaders(r, shaders);

    // Envelope nach Mesh gruppieren.
    std::map<std::string, std::vector<const EnvelopeEntry*>> envByMesh;
    for (const auto& e : envelopes) envByMesh[e.mesh].push_back(&e);

    // --- Modellbaum durchlaufen -------------------------------------------
    struct Found {
        std::string     xsiName;
        const Template* model = nullptr;
        int             parent = -1;   // Index in dieser Liste
    };
    std::vector<Found> found;

    std::function<void(const Template&, int)> walk = [&](const Template& t, int parentSurface) {
        int self = parentSurface;
        if (t.type == "SI_Model") {
            const std::string nm = shortName(stripPrefix(t.name, "MDL-"));
            if (t.find("SI_Mesh") != nullptr) {
                Found f;
                f.xsiName = nm;
                f.model = &t;
                f.parent = parentSurface;
                self = static_cast<int>(found.size());
                found.push_back(std::move(f));
            }
        }
        for (const auto& c : t.children) walk(c, self);
    };
    for (const auto& r : doc.roots) walk(r, -1);

    if (found.empty()) throw std::runtime_error("Keine Mesh-Modelle in der dotXSI gefunden");

    LOD lod;
    lod.surfaces.reserve(found.size());

    // Welche Surface aus welchem Fund wurde. Ein uebersprungenes Mesh — ohne
    // Shape, ohne Dreiecke — hat keine; f.parent zeigt aber in die Fundliste.
    // Ohne Umrechnung verrutschten danach alle Elternbezuege um eins, und die
    // Shadervererbung unten, die nach gleichem Elternteil sucht, griffe
    // daneben.
    std::vector<int> surfaceOf(found.size(), -1);

    for (std::size_t fi = 0; fi < found.size(); ++fi) {
        const Found& f = found[fi];
        const Template* mesh = f.model->find("SI_Mesh");
        const Template* shape = mesh ? mesh->findDeep("SI_Shape") : nullptr;
        const Template* tris = mesh ? mesh->findDeep("SI_TriangleList") : nullptr;
        if (!shape || !tris) {
            res.stats.warnings.push_back("Mesh \"" + f.xsiName + "\" ohne Shape oder TriangleList");
            continue;
        }

        Surface surf;
        surf.name = surfaceNameToGlm(f.xsiName);
        surf.flags = surfaceFlagsFromName(surf.name);
        surf.parentIndex = f.parent;

        const auto sh = shaders.find(f.xsiName);
        surf.shader = (sh == shaders.end()) ? "" : sh->second;

        const auto arrays = parseShape(*shape);
        if (const std::string dbg = envValue("G2C_DEBUG_SHAPE"); !dbg.empty())
            if (f.xsiName == dbg) {
                std::fprintf(stderr, "SI_Shape %s: %zu Arrays\n", f.xsiName.c_str(), arrays.size());
                for (const auto& a : arrays)
                    std::fprintf(stderr, "   %-16s count=%zu stride=%d erste=%g %g %g\n",
                                 a.kind.c_str(), a.count(), a.stride,
                                 a.values.size() > 0 ? a.values[0] : 0.0,
                                 a.values.size() > 1 ? a.values[1] : 0.0,
                                 a.values.size() > 2 ? a.values[2] : 0.0);
            }
        const ShapeArray* pos = findArray(arrays, "POSITION");
        const ShapeArray* nrm = findArray(arrays, "NORMAL");
        const ShapeArray* uv = findArray(arrays, "TEX_COORD");
        if (!pos) {
            res.stats.warnings.push_back("Mesh \"" + f.xsiName + "\" ohne POSITION");
            continue;
        }

        // Objekttransformation des Mesh-Modells.
        //
        // BASEPOSE, nicht SRT: BASEPOSE ist die ABSOLUTE Pose, SRT die lokale
        // relativ zum Elternmodell. Die Mesh-Modelle sind ineinander
        // verschachtelt (torso unter hips unter mesh_root), sodass der lokale
        // Wert ohne Akkumulation ueber die ganze Kette falsch ist.
        //
        // Mit SRT lagen 83 von 84 Surfaces um exakt denselben Betrag daneben —
        // ein konstanter Versatz, der genau auf eine fehlende Elternkette
        // hindeutet. Dieselbe Unterscheidung war schon bei den Bones
        // entscheidend.
        Mat3x4 xf = Mat3x4::identity();
        for (const Template* x : f.model->findAll("SI_Transform")) {
            if (x->name.rfind("BASEPOSE-", 0) != 0 || x->values.size() < 9) continue;
            float v[9]{1, 1, 1, 0, 0, 0, 0, 0, 0};
            for (int k = 0; k < 9; ++k)
                if (const auto d = x->values[static_cast<std::size_t>(k)].asNumber())
                    v[k] = static_cast<float>(*d);
            xf = eulerXYZ(v[3], v[4], v[5]);
            for (int r = 0; r < 3; ++r) {
                xf.m[r][0] *= v[0];
                xf.m[r][1] *= v[1];
                xf.m[r][2] *= v[2];
            }
            xf.m[0][3] = v[6];
            xf.m[1][3] = v[7];
            xf.m[2][3] = v[8];
            break;
        }

        // --- TriangleList lesen -------------------------------------------
        // Aufbau: <anzahl>, "<attribute>", "<material>", dann je ein
        // Indexblock mit anzahl*3 Werten — Positionen, Normalen, UVs.
        if (tris->values.size() < 3) {
            res.stats.warnings.push_back("TriangleList von \"" + f.xsiName + "\" ist leer");
            continue;
        }
        const auto triCount = tris->values[0].asInt();
        if (!triCount || *triCount <= 0) continue;
        const std::string attribs = tris->values[1].text();
        const bool hasUv = attribs.find("TEX_COORD") != std::string::npos;
        const bool hasNormals = attribs.find("NORMAL") != std::string::npos;

        const std::size_t perBlock = static_cast<std::size_t>(*triCount) * 3;
        std::size_t at = 3;
        const auto readBlock = [&](std::vector<int>& dst) {
            dst.clear();
            dst.reserve(perBlock);
            for (std::size_t k = 0; k < perBlock && at < tris->values.size(); ++k, ++at) {
                const auto v = tris->values[at].asInt();
                dst.push_back(v ? static_cast<int>(*v) : 0);
            }
        };

        std::vector<int> posIdx, nrmIdx, uvIdx;
        readBlock(posIdx);
        if (hasNormals) readBlock(nrmIdx);
        if (hasUv) readBlock(uvIdx);

        if (posIdx.size() != perBlock) {
            res.stats.warnings.push_back("TriangleList von \"" + f.xsiName + "\" abgeschnitten");
            continue;
        }

        // Gewichte dieses Meshes: Positionsindex -> Liste (Bone, Gewicht).
        std::map<int, std::vector<VertexWeight>> weightsByPos;
        const auto ev = envByMesh.find(f.xsiName);
        if (ev != envByMesh.end()) {
            for (const EnvelopeEntry* e : ev->second) {
                const auto bi = boneIndex.find(e->bone);
                if (bi == boneIndex.end()) continue;   // Bone nicht im Skelett
                for (const auto& [vi, w] : e->weights)
                    weightsByPos[vi].push_back(VertexWeight{bi->second, w});
            }
        }

        // --- Attributaufloesung -------------------------------------------
        //
        // dotXSI hat je Attribut ein eigenes Indexarray; GLM kennt nur einen
        // Index pro Vertex. Also jedes vorkommende Tripel einmal anlegen und
        // wiederverwenden. Bei "hips" stehen 132 Positionen 609 Normalen
        // gegenueber und es entstehen wieder genau 132 Vertices — dort faellt
        // jede Normale mit derselben Position zusammen.
        // Kandidaten nach Positionsindex gruppiert. Die Suche laeuft nur
        // innerhalb einer Position — mehr verlangt die Regel nicht, und aus
        // der linearen Suche ueber alle Vertices wird eine ueber wenige.
        struct Proto {
            float uv[2];
            float normal[3];
            int   index;
        };
        std::unordered_map<int, std::vector<Proto>> byPos;
        byPos.reserve(perBlock / 2);

        surf.vertices.reserve(perBlock / 2);
        surf.triangles.reserve(static_cast<std::size_t>(*triCount));

        for (std::size_t t3 = 0; t3 < perBlock; t3 += 3) {
            Triangle tri;
            for (int c = 0; c < 3; ++c) {
                const std::size_t k = t3 + static_cast<std::size_t>(c);
                const int pi = posIdx[k];
                const int ni = hasNormals ? nrmIdx[k] : -1;
                const int ui = hasUv ? uvIdx[k] : -1;

                Vertex v;
                if (pos && static_cast<std::size_t>(pi) < pos->count()) {
                    const float* p = &pos->values[static_cast<std::size_t>(pi) * 3];
                    const float local[3] = {p[0], p[1], p[2]};
                    float world[3];
                    for (int r = 0; r < 3; ++r)
                        world[r] = xf.m[r][0] * local[0] + xf.m[r][1] * local[1] +
                                   xf.m[r][2] * local[2] + xf.m[r][3];
                    toGlmSpace(world, opt.scale, v.position);
                }
                if (nrm && ni >= 0 && static_cast<std::size_t>(ni) < nrm->count()) {
                    const float* n = &nrm->values[static_cast<std::size_t>(ni) * 3];
                    // Normalen werden nur gedreht, nicht verschoben, und die
                    // Skalierung faellt bei der anschliessenden Normierung weg.
                    const float world[3] = {xf.m[0][0] * n[0] + xf.m[0][1] * n[1] + xf.m[0][2] * n[2],
                                            xf.m[1][0] * n[0] + xf.m[1][1] * n[1] + xf.m[1][2] * n[2],
                                            xf.m[2][0] * n[0] + xf.m[2][1] * n[1] + xf.m[2][2] * n[2]};
                    float g[3];
                    toGlmSpace(world, 1.0f, g);
                    const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    if (len > 1e-8f) { g[0] /= len; g[1] /= len; g[2] /= len; }
                    v.normal[0] = g[0];
                    v.normal[1] = g[1];
                    v.normal[2] = g[2];
                }
                if (uv && ui >= 0 && static_cast<std::size_t>(ui) < uv->count()) {
                    const float* u = &uv->values[static_cast<std::size_t>(ui) * 2];
                    v.uv[0] = u[0];
                    // dotXSI zaehlt V von unten, GLM von oben.
                    v.uv[1] = 1.0f - u[1];
                }

                // Zusammengefasst wird nach Position UND UV, nicht nach der
                // Normalen.
                //
                // Position allein reicht nicht: an UV-Naehten muss derselbe
                // Punkt zweimal vorkommen, sonst faellt die Texturierung in
                // sich zusammen. Bei "torso" sind es 143 Positionen, aber 159
                // Vertices in der Originaldatei.
                //
                // Die Normale gehoert dagegen NICHT in den Schluessel: in
                // Ravens Dateien hat jede Dreiecksecke ihren eigenen
                // Normalenindex mit leicht abweichendem Wert. Nimmt man sie
                // dazu, faellt gar nichts mehr zusammen und aus 132 Vertices
                // werden 583.
                auto& bucket = byPos[pi];
                int hit = -1;
                for (const Proto& p : bucket) {
                    if (std::fabs(p.uv[0] - v.uv[0]) > opt.uvTolerance ||
                        std::fabs(p.uv[1] - v.uv[1]) > opt.uvTolerance)
                        continue;
                    if (std::fabs(p.normal[0] - v.normal[0]) >= opt.normalTolerance ||
                        std::fabs(p.normal[1] - v.normal[1]) >= opt.normalTolerance ||
                        std::fabs(p.normal[2] - v.normal[2]) >= opt.normalTolerance)
                        continue;
                    hit = p.index;
                    break;   // erster Treffer gewinnt, nicht der beste
                }
                if (hit >= 0) {
                    tri.indexes[c] = hit;
                    continue;
                }

                const auto w = weightsByPos.find(pi);
                if (w != weightsByPos.end()) v.weights = w->second;

                const int newIndex = static_cast<int>(surf.vertices.size());
                bucket.push_back(Proto{{v.uv[0], v.uv[1]},
                                       {v.normal[0], v.normal[1], v.normal[2]},
                                       newIndex});
                surf.vertices.push_back(std::move(v));
                tri.indexes[c] = newIndex;
            }
            // Wickelrichtung umkehren.
            //
            // dotXSI und GLM zaehlen die Ecken gegenlaeufig. An Ravens
            // _humanoid.glm gemessen: dort zeigt die aus den Positionen
            // berechnete Flaechennormale bei ALLEN 2846 Dreiecken
            // entgegengesetzt zur gemittelten Vertexnormale — ohne
            // Umkehrung kommt bei allen 2846 das Gegenteil heraus.
            //
            // Sichtbar wird das als scheinbar umgedrehte Normalen: die
            // Flaechen werden von innen gerendert. Die Normalen selbst sind
            // dabei voellig in Ordnung.
            std::swap(tri.indexes[1], tri.indexes[2]);
            surf.triangles.push_back(tri);
        }

        if (pos && surf.vertices.size() > pos->count())
            res.stats.splitVertices += surf.vertices.size() - pos->count();

        res.stats.vertices += surf.vertices.size();
        res.stats.triangles += surf.triangles.size();
        if (surf.flags & fmt::kSurfFlagIsBolt) ++res.stats.tags;
        if (surf.flags & fmt::kSurfFlagOff) ++res.stats.offSurfaces;

        surfaceOf[fi] = static_cast<int>(lod.surfaces.size());
        lod.surfaces.push_back(std::move(surf));
    }

    // Elternbezuege von Fund- auf Surfaceindizes umrechnen. Fehlt der
    // unmittelbare Elternteil, gilt der naechste vorhandene darueber.
    for (std::size_t fi = 0; fi < found.size(); ++fi) {
        if (surfaceOf[fi] < 0) continue;
        int p = found[fi].parent;
        while (p >= 0 && surfaceOf[static_cast<std::size_t>(p)] < 0)
            p = found[static_cast<std::size_t>(p)].parent;
        lod.surfaces[static_cast<std::size_t>(surfaceOf[fi])].parentIndex =
            p < 0 ? -1 : surfaceOf[static_cast<std::size_t>(p)];
    }

    // Shaderzuweisung vervollstaendigen.
    //
    // Nur 37 der 84 Mesh-Modelle tragen ein XSI_CustomPSet mit Shadernamen.
    // Die uebrigen — vor allem die Tags — erben ihn vom **unmittelbar
    // vorangehenden Geschwister**: gleicher Elternteil, vorherige Position in
    // der Surfacereihenfolge. Gibt es keines, steht "[nomaterial]".
    //
    // NICHT vom Elternteil selbst. Der Unterschied ist gut sichtbar:
    // "*l_hand" haengt unter "l_hand" (hand.tga), traegt aber "torso.tga" —
    // den Shader seines Vorgaengers "l_hand_sleeve". Und "*l_arm_cap_l_hand"
    // haengt unter "l_arm" (torso.tga) und traegt "hand.tga" von seinem
    // Vorgaenger "l_hand". Mit Elternvererbung kommt genau das Vertauschte
    // heraus.
    //
    // An allen 46 Tags von Ravens _humanoid.glm geprueft: 44 folgen der
    // Regel, die beiden uebrigen ("*l_leg_calf", "*r_leg_calf") haben kein
    // vorangehendes Geschwister und tragen "[nomaterial]".
    //
    // Ein LEERER Shadername kommt in der Originaldatei nirgends vor. ModView
    // laedt Shader ueber genau diesen Namen und meldet sonst fehlende
    // Texturen.
    for (std::size_t i = 0; i < lod.surfaces.size(); ++i) {
        Surface& s = lod.surfaces[i];
        if (!s.shader.empty()) continue;
        for (std::size_t j = i; j-- > 0;) {
            if (lod.surfaces[j].parentIndex != s.parentIndex) continue;
            s.shader = lod.surfaces[j].shader;
            break;
        }
        if (s.shader.empty()) s.shader = "[nomaterial]";
    }

    res.stats.surfaces = lod.surfaces.size();
    res.mesh.lods.push_back(std::move(lod));
    return res;
}

MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt) {
    return importMesh(parseFile(path), opt);
}

}  // namespace g2::xsi
