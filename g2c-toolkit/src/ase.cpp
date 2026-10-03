#include "g2/ase.h"

#include "g2/bytebuf.h"
#include "g2/readfile.h"
#include "stripper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

namespace g2::ase {
namespace {

float F(double x) { return static_cast<float>(x); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// --- Tokens -------------------------------------------------------------------
//
// A token is a run of bytes above blank (Carcass 0x44d070); some keywords take
// the rest of the line. Quoted names are read as a whole here - Carcass cut
// them at the first blank and dropped the first character of unquoted ones.
class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}

    bool next(std::string& tok) {
        while (i_ < s_.size() && static_cast<unsigned char>(s_[i_]) <= 0x20) ++i_;
        if (i_ >= s_.size()) return false;
        const std::size_t a = i_;
        while (i_ < s_.size() && static_cast<unsigned char>(s_[i_]) > 0x20) ++i_;
        tok.assign(s_, a, i_ - a);
        return true;
    }
    std::string restOfLine() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) ++i_;
        const std::size_t a = i_;
        while (i_ < s_.size() && s_[i_] != '\r' && s_[i_] != '\n') ++i_;
        std::string r(s_, a, i_ - a);
        while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
        return r;
    }
    // A quoted string ("a b c"), or the next token.
    std::string name() {
        const std::string r = restOfLine();
        if (!r.empty() && r[0] == '"') {
            const std::size_t e = r.find('"', 1);
            return r.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        }
        const std::size_t sp = r.find_first_of(" \t");
        return r.substr(0, sp);
    }
    std::string need(const char* what) {
        std::string t;
        if (!next(t)) throw std::runtime_error(std::string("ASE: Datei endet in ") + what);
        return t;
    }
    double number(const char* what) {
        const std::string t = need(what);
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) throw std::runtime_error("ASE: Zahl erwartet in " + std::string(what) + ", gelesen \"" + t + "\"");
        return v;
    }
    long integer(const char* what) {
        std::string t = need(what);
        while (!t.empty() && t.back() == ':') t.pop_back();   // "0:" in *MESH_FACE
        char* end = nullptr;
        const long v = std::strtol(t.c_str(), &end, 10);
        if (end == t.c_str()) throw std::runtime_error("ASE: ganze Zahl erwartet in " + std::string(what) + ", gelesen \"" + t + "\"");
        return v;
    }
    void expectOpen(const char* what) {
        const std::string t = need(what);
        if (t != "{") throw std::runtime_error(std::string("ASE: \"{\" erwartet nach ") + what);
    }
    void skipBlock() {
        int depth = 0;
        std::string t;
        while (next(t)) {
            if (t == "{") ++depth;
            else if (t == "}") {
                if (--depth <= 0) return;
            }
        }
    }

private:
    const std::string& s_;
    std::size_t        i_ = 0;
};

std::string bitmapName(std::string raw, const std::string& baseDir, std::vector<std::string>& warnings) {
    std::replace(raw.begin(), raw.end(), '\\', '/');
    const std::string low = lower(raw);
    std::string base = lower(baseDir);
    std::replace(base.begin(), base.end(), '\\', '/');
    if (base.size() > 1 && base[1] == ':') base.erase(0, 2);
    if (!base.empty() && base.back() != '/') base += '/';
    if (!base.empty() && base != "/") {
        const std::size_t at = low.find(base);
        if (at != std::string::npos) return low.substr(at + base.size());
    }
    const std::size_t b = low.rfind("/base/");
    if (b != std::string::npos) return low.substr(b + 6);
    warnings.push_back("Textur \"" + raw + "\" liegt nicht unter base/ - als Name unbrauchbar");
    return "(not converted: '" + low + "')";
}

// Carcass's tag-name rule: names starting with "tag" are cut at the last
// "_" while there are two or more, then at the first blank.
std::string objectName(const std::string& raw) {
    std::string n = raw;
    if (n.rfind("tag", 0) == 0) {
        while (std::count(n.begin(), n.end(), '_') >= 2) n.resize(n.rfind('_'));
        const std::size_t sp = n.find(' ');
        if (sp != std::string::npos) n.resize(sp);
    }
    return lower(n);
}

}  // namespace

Scene parse(const std::string& text, const std::string& baseDir) {
    Scene sc;
    Lexer lx(text);
    std::map<std::string, int> globalBone;   // lower name -> index
    std::string tok;

    while (lx.next(tok)) {
        if (tok == "*3DSMAX_ASCIIEXPORT" || tok == "*COMMENT") {
            (void)lx.restOfLine();
        } else if (tok == "*SCENE") {
            lx.skipBlock();
        } else if (tok == "*MATERIAL_LIST") {
            lx.expectOpen("*MATERIAL_LIST");
            int depth = 1;
            while (depth > 0 && lx.next(tok)) {
                if (tok == "{") ++depth;
                else if (tok == "}") --depth;
                else if (tok == "*MATERIAL_COUNT") (void)lx.integer("*MATERIAL_COUNT");
                else if (tok == "*MATERIAL") {
                    (void)lx.integer("*MATERIAL");
                    lx.expectOpen("*MATERIAL");
                    std::string bitmap;
                    bool inDiffuse = false;
                    int d = 1, diffuseDepth = -1;
                    while (d > 0 && lx.next(tok)) {
                        if (tok == "{") ++d;
                        else if (tok == "}") {
                            if (d == diffuseDepth) inDiffuse = false;
                            --d;
                        } else if (tok == "*MAP_DIFFUSE") {
                            inDiffuse = true;
                            diffuseDepth = d + 1;
                        } else if (tok == "*BITMAP") {
                            const std::string b = lx.name();
                            if (inDiffuse && bitmap.empty()) bitmap = b;
                        } else if (tok.size() > 1 && tok[0] == '*') {
                            // values of other keys are skipped token by token
                        }
                    }
                    sc.materials.push_back(bitmap.empty() ? std::string() : bitmapName(bitmap, baseDir, sc.warnings));
                }
            }
        } else if (tok == "*GEOMOBJECT") {
            lx.expectOpen("*GEOMOBJECT");
            Object o;
            int numVertex = -1, numFaces = -1;
            std::vector<int> localBones;
            int depth = 1;
            while (depth > 0 && lx.next(tok)) {
                if (tok == "{") ++depth;
                else if (tok == "}") --depth;
                else if (tok == "*NODE_NAME" && depth == 1) o.rawName = lx.name();
                else if (tok == "*NODE_PARENT") o.parent = lower(lx.name());
                else if (tok == "*NODE_TM" || tok == "*TM_ANIMATION" || tok == "*MESH_ANIMATION") lx.skipBlock();
                else if (tok == "*MATERIAL_REF") o.materialRef = static_cast<int>(lx.integer("*MATERIAL_REF"));
                else if (tok.rfind("*PROP_", 0) == 0) (void)lx.restOfLine();
                else if (tok == "*MESH") {
                    lx.expectOpen("*MESH");
                    int md = 1;
                    while (md > 0 && lx.next(tok)) {
                        if (tok == "{") ++md;
                        else if (tok == "}") --md;
                        else if (tok == "*MESH_NUMVERTEX") numVertex = static_cast<int>(lx.integer("*MESH_NUMVERTEX"));
                        else if (tok == "*MESH_NUMFACES") numFaces = static_cast<int>(lx.integer("*MESH_NUMFACES"));
                        else if (tok == "*MESH_NUMTVFACES") {
                            const long n = lx.integer("*MESH_NUMTVFACES");
                            if (n != numFaces)
                                throw std::runtime_error("ASE \"" + o.rawName + "\": MESH_NUMTVFACES != MESH_NUMFACES");
                        } else if (tok == "*MESH_VERTEX") {
                            (void)lx.integer("*MESH_VERTEX");
                            const float x = F(lx.number("*MESH_VERTEX")), y = F(lx.number("*MESH_VERTEX")),
                                        z = F(lx.number("*MESH_VERTEX"));
                            o.meshVerts.push_back({x, y, z});
                        } else if (tok == "*MESH_FACE") {
                            (void)lx.integer("*MESH_FACE");
                            std::array<int, 3> f{};
                            for (int k = 0; k < 3; ++k) {
                                (void)lx.need("*MESH_FACE");   // "A:" "B:" "C:"
                                f[static_cast<std::size_t>(k)] = static_cast<int>(lx.integer("*MESH_FACE"));
                            }
                            (void)lx.restOfLine();
                            o.faces.push_back(f);
                        } else if (tok == "*MESH_TVERT") {
                            (void)lx.integer("*MESH_TVERT");
                            const float u = F(lx.number("*MESH_TVERT")), v = F(lx.number("*MESH_TVERT"));
                            (void)lx.number("*MESH_TVERT");
                            o.tverts.push_back({u, F(1.0 - v)});
                        } else if (tok == "*MESH_TFACE") {
                            (void)lx.integer("*MESH_TFACE");
                            std::array<int, 3> f{};
                            for (int k = 0; k < 3; ++k) f[static_cast<std::size_t>(k)] = static_cast<int>(lx.integer("*MESH_TFACE"));
                            o.tfaces.push_back(f);
                        } else if (tok == "*MESH_NORMALS") {
                            lx.skipBlock();
                        } else if (tok == "*TIMEVALUE" || tok == "*MESH_NUMTVERTEX") {
                            (void)lx.need(tok.c_str());
                        }
                    }
                } else if (tok == "*MESH_WEIGHTS") {
                    lx.expectOpen("*MESH_WEIGHTS");
                    o.hasWeights = true;
                    int wd = 1;
                    while (wd > 0 && lx.next(tok)) {
                        if (tok == "{") ++wd;
                        else if (tok == "}") --wd;
                        else if (tok == "*MESH_NUMVERTEX") {
                            const long n = lx.integer("*MESH_WEIGHTS *MESH_NUMVERTEX");
                            if (n != numVertex)
                                throw std::runtime_error("ASE \"" + o.rawName + "\": MESH_WEIGHTS nennt " + std::to_string(n) +
                                                         " Vertices, das Mesh hat " + std::to_string(numVertex));
                        } else if (tok == "*MESH_NUMBONE") {
                            (void)lx.integer("*MESH_NUMBONE");
                        } else if (tok == "*MESH_BONE_NAME") {
                            (void)lx.integer("*MESH_BONE_NAME");
                            const std::string bn = lx.name();
                            const std::string key = lower(bn);
                            auto it = globalBone.find(key);
                            if (it == globalBone.end()) {
                                it = globalBone.emplace(key, static_cast<int>(sc.bones.size())).first;
                                sc.bones.push_back(bn);
                            }
                            localBones.push_back(it->second);
                        } else if (tok == "*MESH_BONE_VERTEX") {
                            (void)lx.integer("*MESH_BONE_VERTEX");
                            Object::BoneVertex bv;
                            for (int k = 0; k < 3; ++k) bv.p[static_cast<std::size_t>(k)] = F(lx.number("*MESH_BONE_VERTEX"));
                            bool stop = false;
                            for (int k = 0; k < 8; ++k) {
                                const long b = lx.integer("*MESH_BONE_VERTEX");
                                const float w = F(lx.number("*MESH_BONE_VERTEX"));
                                if (b < 0) stop = true;
                                if (stop) continue;
                                if (b >= static_cast<long>(localBones.size()))
                                    throw std::runtime_error("ASE \"" + o.rawName + "\": Bone-Index " + std::to_string(b) +
                                                             " ausserhalb der *MESH_BONE_LIST (" +
                                                             std::to_string(localBones.size()) + " Eintraege)");
                                bv.w.push_back(BoneWeight{localBones[static_cast<std::size_t>(b)], w});
                            }
                            o.boneVerts.push_back(std::move(bv));
                        }
                    }
                    if (numVertex >= 0 && static_cast<int>(o.boneVerts.size()) > numVertex)
                        throw std::runtime_error("ASE \"" + o.rawName + "\": mehr *MESH_BONE_VERTEX als Vertices");
                } else if (tok.size() > 1 && tok[0] == '*') {
                    // unknown keyword: its value is skipped token by token
                }
            }
            o.name = objectName(o.rawName);
            if (o.faces.empty()) sc.warnings.push_back("ASE-Objekt \"" + o.rawName + "\" hat keine Dreiecke");
            // "ignore_" objects and Biped helpers ("Bip01 ...") are not part
            // of the model. Carcass meant to drop the "Bip" ones too, but
            // compared after lower-casing and never did.
            if (o.name.find("ignore_") != std::string::npos || o.rawName.find("Bip") != std::string::npos) continue;
            if (sc.objects.size() >= 256) throw std::runtime_error("ASE: mehr als 256 Objekte");
            sc.objects.push_back(std::move(o));
        } else if (tok != "{" && tok != "}") {
            sc.warnings.push_back("ASE: unbekanntes Element \"" + tok + "\"");
        }
    }
    if (sc.objects.empty()) throw std::runtime_error("ASE: keine Objekte (*GEOMOBJECT)");
    for (const auto& o : sc.objects) {
        if (!o.hasWeights)
            throw std::runtime_error("ASE-Objekt \"" + o.rawName + "\" hat keine *MESH_WEIGHTS - ohne Gewichte "
                                     "hat es keine Positionen (Carcass setzte alle Punkte in den Ursprung)");
        for (const auto& f : o.faces)
            for (int k : f)
                if (k < 0 || static_cast<std::size_t>(k) >= o.boneVerts.size())
                    throw std::runtime_error("ASE \"" + o.rawName + "\": Dreieck zeigt auf Vertex " + std::to_string(k));
        for (const auto& f : o.tfaces)
            for (int k : f)
                if (k < 0 || static_cast<std::size_t>(k) >= o.tverts.size())
                    throw std::runtime_error("ASE \"" + o.rawName + "\": Dreieck zeigt auf UV " + std::to_string(k));
        if (o.tfaces.size() != o.faces.size() && !o.tfaces.empty())
            throw std::runtime_error("ASE \"" + o.rawName + "\": Anzahl UV-Dreiecke passt nicht");
        // Carcass silently used the *MESH_BONE_VERTEX copy; say when it differs.
        bool differs = false;
        for (std::size_t i = 0; i < o.meshVerts.size() && i < o.boneVerts.size(); ++i)
            for (int k = 0; k < 3; ++k)
                if (std::fabs(o.meshVerts[i][static_cast<std::size_t>(k)] - o.boneVerts[i].p[static_cast<std::size_t>(k)]) > 0.001f)
                    differs = true;
        if (differs)
            sc.warnings.push_back("ASE \"" + o.rawName + "\": *MESH_VERTEX und *MESH_BONE_VERTEX weichen ab - "
                                  "benutzt werden die Positionen aus *MESH_BONE_VERTEX");
    }
    return sc;
}

Scene parseFile(const std::string& path, const std::string& baseDir) {
    return parse(readWholeFile(path), baseDir);
}

std::string findAseFile(const std::string& token, const std::string& baseDir, const std::string& carDir) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    const std::string low = lower(token);
    if (low.find(".ask") != std::string::npos || low.find(".ase") != std::string::npos) names.push_back(token);
    for (const char* e : {".ASK", ".ask", ".ase", ".ASE"}) names.push_back(token + e);
    for (const auto& n : names) {
        for (const fs::path& cand : {fs::path(baseDir) / n, fs::path(n), fs::path(carDir) / n}) {
            std::error_code ec;
            if (fs::is_regular_file(cand, ec)) return cand.lexically_normal().make_preferred().string();
        }
    }
    return {};
}

std::vector<std::string> referencedBones(const Scene& scene) {
    std::vector<bool> used(scene.bones.size(), false);
    for (const auto& o : scene.objects)
        for (const auto& f : o.faces)
            for (int k : f)
                for (const auto& w : o.boneVerts[static_cast<std::size_t>(k)].w) used[static_cast<std::size_t>(w.bone)] = true;
    std::vector<std::string> out;
    for (std::size_t i = 0; i < used.size(); ++i)
        if (used[i]) out.push_back(scene.bones[i]);
    return out;
}

// --- MDR -------------------------------------------------------------------------

namespace {

// Mode 0 takes LOD 0 only; "_2".."_9" and "_0" are LOD copies (Carcass's
// collector with flag 0 - "_1" counts as LOD 0 there).
bool isLodCopy(const std::string& n) {
    return n.size() > 2 && n[n.size() - 2] == '_' && (n.back() == '0' || (n.back() >= '2' && n.back() <= '9'));
}

// Surface name: only the LOD suffix "_<digit>" comes off. Carcass cut any
// "_x" ("u_x" -> "u"), which gave duplicate names (SPEC B4).
std::string surfName(std::string n) {
    n = n.substr(0, 63);
    if (n.size() > 2 && n[n.size() - 2] == '_' && std::isdigit(static_cast<unsigned char>(n.back()))) n.resize(n.size() - 2);
    return n;
}

struct MVert {
    std::array<float, 3> p{};
    float u = 0.0f, v = 0.0f;
    std::vector<std::pair<int, float>> w;   // written bone index, weight
    std::array<float, 3> n{};
};

}  // namespace

MdrResult buildMdr(const MdrInput& in) {
    MdrResult res;
    const Scene& sc = *in.scene;
    const Skeleton& sk = *in.skeleton;
    const std::size_t nFrames = in.frames.size();
    if (nFrames == 0) throw std::runtime_error("MDR: keine Frames - es fehlt ein $aseanimgrab");

    // Objects of LOD 0.
    std::vector<const Object*> objs;
    for (const auto& o : sc.objects) {
        if (isLodCopy(o.name)) continue;
        if (o.name.rfind("tag_", 0) == 0)
            throw std::runtime_error("Objekt \"" + o.rawName + "\" hat einen alten Tag-Namen - Tags heissen \"bolt_...\"");
        objs.push_back(&o);
    }
    if (objs.empty()) throw std::runtime_error("MDR: keine Objekte in LOD 0");

    // ASE bone -> skeleton bone.
    std::map<std::string, int> skIdx;
    for (std::size_t b = 0; b < sk.bones.size(); ++b) skIdx.emplace(lower(sk.bones[b].name), static_cast<int>(b));
    std::vector<int> aseToSk(sc.bones.size(), -1);
    for (std::size_t i = 0; i < sc.bones.size(); ++i) {
        std::string k = lower(sc.bones[i]);
        auto it = skIdx.find(k);
        if (it == skIdx.end()) {
            // "face_always_" in the ASE, "face" in the skeleton
            const std::size_t a = k.find("_always_");
            if (a != std::string::npos) k.erase(a, 8);
            if (!k.empty() && k.back() == '_') k.pop_back();
            it = skIdx.find(k);
        }
        if (it != skIdx.end()) aseToSk[i] = it->second;
    }

    // Bones written: every skeleton bone a weight of these objects names
    // (weight 0 too), in skeleton order.
    std::vector<bool> usedSk(sk.bones.size(), false);
    for (const Object* o : objs)
        for (const auto& f : o->faces)
            for (int k : f)
                for (const auto& w : o->boneVerts[static_cast<std::size_t>(k)].w) {
                    const int s = aseToSk[static_cast<std::size_t>(w.bone)];
                    if (s < 0)
                        throw std::runtime_error("Bone \"" + sc.bones[static_cast<std::size_t>(w.bone)] +
                                                 "\" gibt es im Skelett der Animationen nicht");
                    usedSk[static_cast<std::size_t>(s)] = true;
                }
    std::vector<int> written, outIdx(sk.bones.size(), -1);
    for (std::size_t b = 0; b < sk.bones.size(); ++b)
        if (usedSk[b]) {
            outIdx[b] = static_cast<int>(written.size());
            written.push_back(static_cast<int>(b));
        }

    // Bone matrices: scale divided out, -origin added, floored to 1e-7.
    std::vector<float> boneScale(written.size(), 1.0f);
    std::vector<std::vector<Mat3x4>> mats(nFrames, std::vector<Mat3x4>(written.size()));
    for (std::size_t wi = 0; wi < written.size(); ++wi) {
        const int b = written[wi];
        double mn = 1e30, mx = -1e30;
        for (std::size_t f = 0; f < nFrames; ++f) {
            const Mat3x4& m = in.frames[f][static_cast<std::size_t>(b)];
            double sq[3];
            for (int c = 0; c < 3; ++c)
                sq[c] = static_cast<double>(m.m[0][c]) * m.m[0][c] + static_cast<double>(m.m[1][c]) * m.m[1][c] +
                        static_cast<double>(m.m[2][c]) * m.m[2][c];
            if (std::fabs(sq[0] - sq[1]) > 0.001 || std::fabs(sq[0] - sq[2]) > 0.001 || std::fabs(sq[1] - sq[2]) > 0.001)
                throw std::runtime_error("Bone \"" + sk.bones[static_cast<std::size_t>(b)].name +
                                         "\" ist in Frame " + std::to_string(f) + " ungleichmaessig skaliert");
            for (double v : sq) {
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
        if (mx - mn > 0.001)
            throw std::runtime_error("Bone \"" + sk.bones[static_cast<std::size_t>(b)].name +
                                     "\" aendert seine Skalierung ueber die Frames");
        boneScale[wi] = F(std::sqrt((mn + mx) / 2.0));
        for (std::size_t f = 0; f < nFrames; ++f) {
            Mat3x4 m = in.frames[f][static_cast<std::size_t>(b)];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) m.m[r][c] = F(m.m[r][c] / static_cast<double>(boneScale[wi]));
            for (int r = 0; r < 3; ++r) m.m[r][3] = F(static_cast<double>(m.m[r][3]) + in.originAdjust[static_cast<std::size_t>(r)]);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) m.m[r][c] = F(std::floor(static_cast<double>(m.m[r][c]) * 1e7) / 1e7);
            mats[f][wi] = m;
        }
    }

    // Surfaces.
    struct MSurf {
        std::string name, shader;
        std::vector<MVert> verts;
        std::vector<std::array<int, 3>> tris;
    };
    std::vector<MSurf> surfs;
    const float T = F(0.001);
    for (const Object* o : objs) {
        MSurf s;
        s.name = surfName(o->name);
        s.shader = o->materialRef >= 0 && static_cast<std::size_t>(o->materialRef) < sc.materials.size() &&
                           !sc.materials[static_cast<std::size_t>(o->materialRef)].empty()
                       ? lower(sc.materials[static_cast<std::size_t>(o->materialRef)]).substr(0, 63)
                       : "NOMATERIAL";
        for (std::size_t fi = 0; fi < o->faces.size(); ++fi) {
            const auto& f = o->faces[fi];
            std::array<int, 3> tri{};
            const int order[3] = {0, 2, 1};   // corners a, c, b
            for (int k = 0; k < 3; ++k) {
                const int c = order[k];
                const auto& bv = o->boneVerts[static_cast<std::size_t>(f[static_cast<std::size_t>(c)])];
                MVert v;
                v.p = bv.p;
                if (fi < o->tfaces.size()) {
                    const auto& t = o->tverts[static_cast<std::size_t>(o->tfaces[fi][static_cast<std::size_t>(c)])];
                    v.u = t[0];
                    v.v = t[1];
                }
                for (const auto& w : bv.w) {
                    const int ob = outIdx[static_cast<std::size_t>(aseToSk[static_cast<std::size_t>(w.bone)])];
                    bool merged = false;
                    for (auto& x : v.w)
                        if (x.first == ob) {
                            x.second = F(static_cast<double>(x.second) + w.weight);
                            merged = true;
                            break;
                        }
                    if (!merged) v.w.emplace_back(ob, w.weight);
                }
                int hit = -1;
                for (std::size_t e = 0; e < s.verts.size() && hit < 0; ++e) {
                    const MVert& x = s.verts[e];
                    if (x.w != v.w) continue;
                    const double dx = x.p[0] - v.p[0], dy = x.p[1] - v.p[1], dz = x.p[2] - v.p[2];
                    if (!(std::sqrt(dx * dx + dy * dy + dz * dz) < T)) continue;
                    if (!(std::fabs(x.u - v.u) < T) || !(std::fabs(x.v - v.v) < T)) continue;
                    hit = static_cast<int>(e);
                }
                if (hit < 0) {
                    hit = static_cast<int>(s.verts.size());
                    s.verts.push_back(std::move(v));
                }
                tri[static_cast<std::size_t>(k)] = hit;
            }
            s.tris.push_back(tri);
        }
        if (!s.tris.empty()) s.tris = Stripper(s.tris).order();

        // Surface scale: the middle of the scales of the bones it uses.
        float smin = 1e30f, smax = -1e30f;
        for (const auto& v : s.verts)
            for (const auto& w : v.w) {
                smin = std::min(smin, boneScale[static_cast<std::size_t>(w.first)]);
                smax = std::max(smax, boneScale[static_cast<std::size_t>(w.first)]);
            }
        if (smax > 0.0f) {
            if (smax - smin > 0.001f) res.warnings.push_back("Surface \"" + s.name + "\": Bones mit verschiedener Skalierung");
            const float ss = F((static_cast<double>(smin) + smax) / 2.0);
            for (auto& v : s.verts)
                for (auto& c : v.p) c = F(static_cast<double>(c) * ss);
        }
        // Normals: sum of (p0 - p1) x (p2 - p1), normalised.
        std::vector<std::array<double, 3>> acc(s.verts.size(), {0.0, 0.0, 0.0});
        for (const auto& t : s.tris) {
            const auto& p0 = s.verts[static_cast<std::size_t>(t[0])].p;
            const auto& p1 = s.verts[static_cast<std::size_t>(t[1])].p;
            const auto& p2 = s.verts[static_cast<std::size_t>(t[2])].p;
            const double a[3] = {static_cast<double>(p0[0]) - p1[0], static_cast<double>(p0[1]) - p1[1], static_cast<double>(p0[2]) - p1[2]};
            const double b[3] = {static_cast<double>(p2[0]) - p1[0], static_cast<double>(p2[1]) - p1[1], static_cast<double>(p2[2]) - p1[2]};
            const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            for (int k : t)
                for (int q = 0; q < 3; ++q) acc[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)] += c[q];
        }
        for (std::size_t i = 0; i < s.verts.size(); ++i) {
            const auto& a = acc[i];
            const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            // Carcass wrote NaN for a zero length (SPEC B11).
            s.verts[i].n = l > 0.0 ? std::array<float, 3>{F(a[0] / l), F(a[1] / l), F(a[2] / l)}
                                   : std::array<float, 3>{0.0f, 0.0f, 0.0f};
            double sum = 0.0;
            for (const auto& w : s.verts[i].w) sum += w.second;
            if (std::fabs(sum - 1.0) > 0.01 && !s.verts[i].w.empty())
                res.warnings.push_back("Surface \"" + s.name + "\": Gewichte eines Vertex ergeben " + std::to_string(sum) +
                                       " statt 1");
        }
        res.vertices += s.verts.size();
        res.triangles += s.tris.size();
        res.skin += s.name + "," + s.shader + "\r\n";
        surfs.push_back(std::move(s));
    }

    // --- Write ------------------------------------------------------------------
    ByteBuf buf;
    buf.raw("RDM5", 4);
    buf.i32(2);
    buf.fixedString(in.name.substr(0, 63), 64, "MDR-Name");
    buf.i32(static_cast<std::int32_t>(nFrames));
    buf.i32(static_cast<std::int32_t>(written.size()));
    buf.i32(104);   // ofsFrames, positive = uncompressed
    buf.i32(1);     // numLODs
    const std::size_t pOfsLODs = buf.reserveI32();
    buf.i32(0);     // numTags
    const std::size_t pOfsTags = buf.reserveI32();
    const std::size_t pOfsEnd = buf.reserveI32();

    for (std::size_t f = 0; f < nFrames; ++f) {
        // Bounds: every vertex through its weighted bones (weights as they are).
        double mins[3] = {9999, 9999, 9999}, maxs[3] = {-9999, -9999, -9999};
        std::vector<std::array<double, 3>> pts;
        for (const auto& s : surfs)
            for (const auto& v : s.verts) {
                double p[3] = {0, 0, 0};
                for (const auto& w : v.w) {
                    const Mat3x4& m = mats[f][static_cast<std::size_t>(w.first)];
                    for (int r = 0; r < 3; ++r)
                        p[r] += w.second * (static_cast<double>(m.m[r][0]) * v.p[0] + static_cast<double>(m.m[r][1]) * v.p[1] +
                                            static_cast<double>(m.m[r][2]) * v.p[2] + m.m[r][3]);
                }
                for (int r = 0; r < 3; ++r) {
                    mins[r] = std::min(mins[r], p[r]);
                    maxs[r] = std::max(maxs[r], p[r]);
                }
                pts.push_back({p[0], p[1], p[2]});
            }
        double origin[3];
        for (int r = 0; r < 3; ++r) origin[r] = (mins[r] + maxs[r]) * 0.5;
        double radius = 0.0;
        for (const auto& p : pts)
            radius = std::max(radius, std::sqrt((p[0] - origin[0]) * (p[0] - origin[0]) + (p[1] - origin[1]) * (p[1] - origin[1]) +
                                                (p[2] - origin[2]) * (p[2] - origin[2])));
        for (double v : mins) buf.f32(F(v));
        for (double v : maxs) buf.f32(F(v));
        for (double v : origin) buf.f32(F(v));
        buf.f32(F(radius));
        buf.pad(16);   // frame name
        for (std::size_t wi = 0; wi < written.size(); ++wi)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) buf.f32(mats[f][wi].m[r][c]);
    }

    buf.patchI32(pOfsLODs, static_cast<std::int32_t>(buf.size()));
    const std::size_t lodStart = buf.size();
    buf.i32(static_cast<std::int32_t>(surfs.size()));
    buf.i32(12);
    const std::size_t pLodEnd = buf.reserveI32();
    for (const auto& s : surfs) {
        const std::size_t s0 = buf.size();
        buf.i32(0);
        buf.fixedString(s.name, 64, "Surface-Name");
        buf.fixedString(s.shader, 64, "Shader");
        buf.i32(0);   // shaderIndex
        buf.i32(-static_cast<std::int32_t>(s0));
        buf.i32(static_cast<std::int32_t>(s.verts.size()));
        buf.i32(168);
        buf.i32(static_cast<std::int32_t>(s.tris.size()));
        const std::size_t pTri = buf.reserveI32();
        buf.i32(0);   // numBoneReferences
        const std::size_t pRefs = buf.reserveI32();
        const std::size_t pEnd = buf.reserveI32();
        for (const auto& v : s.verts) {
            for (float x : v.n) buf.f32(x);
            buf.f32(v.u);
            buf.f32(v.v);
            buf.i32(static_cast<std::int32_t>(v.w.size()));
            for (const auto& w : v.w) {
                buf.i32(w.first);
                buf.f32(w.second);
                for (float x : v.p) buf.f32(x);
            }
        }
        buf.patchI32(pTri, static_cast<std::int32_t>(buf.size() - s0));
        for (const auto& t : s.tris)
            for (int k : t) buf.i32(k);
        buf.patchI32(pRefs, static_cast<std::int32_t>(buf.size() - s0));
        buf.patchI32(pEnd, static_cast<std::int32_t>(buf.size() - s0));
    }
    buf.patchI32(pLodEnd, static_cast<std::int32_t>(buf.size() - lodStart));
    buf.patchI32(pOfsTags, static_cast<std::int32_t>(buf.size()));
    buf.patchI32(pOfsEnd, static_cast<std::int32_t>(buf.size()));

    res.data = buf.bytes();
    res.bones = written.size();
    res.frames = nFrames;
    res.surfaces = surfs.size();
    return res;
}

}  // namespace g2::ase
