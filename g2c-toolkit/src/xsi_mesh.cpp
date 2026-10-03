#include "g2/xsi_mesh.h"

#include "g2/ase.h"

#include "g2/readfile.h"
#include "g2/xsi_anim.h"
#include "stripper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <stdexcept>

// The GLM is built exactly as Carcass v2.2 builds it from dotXSI. Every rule
// below was measured against 26 Carcass builds (re/mesh/SPEC.md), and the
// reference implementation reproduces them byte for byte. What Carcass does
// wrong on purpose-free grounds is NOT copied; those places say so.

namespace g2::xsi {
namespace {

float F(double x) { return static_cast<float>(x); }

// --- Carcass's 4x4 float matrix (x87, 53-bit intermediates, float stores) ---
//
// Mesh vertices are placed with the SRT chain of their models. Carcass does it
// in float with double intermediates, and treats a matrix within 1e-4 of the
// identity as exactly the identity. Reproduced operation by operation, so the
// positions come out bit-identical.
struct CM4 {
    float m[4][4];
    bool  ident = true;
    CM4() {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) m[i][j] = i == j ? 1.0f : 0.0f;
    }
    void checkIdent() {
        ident = false;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const double v = m[i][j];
                if (i == j) {
                    if (std::fabs(1.0 - v) > 9.999999747378752e-05) return;
                } else if (std::fabs(v) > static_cast<double>(F(0.0001))) {
                    return;
                }
            }
        ident = true;
    }
};

CM4 cmScale(float sx, float sy, float sz) {
    CM4 r;
    r.m[0][0] = sx;
    r.m[1][1] = sy;
    r.m[2][2] = sz;
    r.checkIdent();
    return r;
}

CM4 cmRot(int axis, float a) {
    const float s = F(std::sin(static_cast<double>(a)));
    const float c = F(std::cos(static_cast<double>(a)));
    CM4 r;
    if (axis == 0) {
        r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; r.m[1][1] = c;
    } else if (axis == 1) {
        r.m[0][2] = s; r.m[2][0] = -s; r.m[0][0] = c; r.m[2][2] = c;
    } else {
        r.m[0][1] = -s; r.m[0][0] = c; r.m[1][0] = s; r.m[1][1] = c;
    }
    r.checkIdent();
    return r;
}

CM4 cmMul(const CM4& a, const CM4& b) {
    if (a.ident) return b;
    if (b.ident) return a;
    CM4 r;
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 4; ++j)
            r.m[k][j] = F(((static_cast<double>(b.m[0][j]) * a.m[k][0] + static_cast<double>(b.m[1][j]) * a.m[k][1]) +
                           static_cast<double>(b.m[2][j]) * a.m[k][2]) +
                          static_cast<double>(b.m[3][j]) * a.m[k][3]);
    r.ident = false;
    return r;
}

std::array<float, 3> cmPoint(const CM4& m, const std::array<float, 3>& p) {
    if (m.ident) return p;
    std::array<float, 3> o{};
    for (int j = 0; j < 3; ++j)
        o[static_cast<std::size_t>(j)] =
            F(((static_cast<double>(m.m[0][j]) * p[0] + static_cast<double>(m.m[1][j]) * p[1]) +
               static_cast<double>(m.m[2][j]) * p[2]) +
              m.m[3][j]);
    return o;
}

std::array<float, 3> cmVec(const CM4& m, const std::array<float, 3>& n) {
    if (m.ident) return n;
    std::array<float, 3> o{};
    for (int i = 0; i < 3; ++i) {
        float t = F(static_cast<double>(m.m[0][i]) * n[0] + 0.0);
        t = F(static_cast<double>(m.m[1][i]) * n[1] + t);
        t = F(static_cast<double>(m.m[2][i]) * n[2] + t);
        o[static_cast<std::size_t>(i)] = t;
    }
    return o;
}

// Numerical Recipes LU decomposition in float, as Carcass (0x417310).
bool cmLud(float a[4][4], int indx[4], double& d) {
    d = 1.0;
    float vv[4];
    for (int i = 0; i < 4; ++i) {
        double big = 0.0;
        for (int j = 0; j < 4; ++j) big = std::max(big, static_cast<double>(std::fabs(a[i][j])));
        if (big < 1e-15) return false;
        vv[i] = F(1.0 / big);
    }
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < j; ++i) {
            float s = a[i][j];
            for (int k = 0; k < i; ++k) s = F(s - static_cast<double>(a[i][k]) * a[k][j]);
            a[i][j] = s;
        }
        double big = 0.0;
        int imax = 0;
        for (int i = j; i < 4; ++i) {
            float s = a[i][j];
            for (int k = 0; k < j; ++k) s = F(s - static_cast<double>(a[i][k]) * a[k][j]);
            a[i][j] = s;
            const float dum = F(static_cast<double>(vv[i]) * F(std::fabs(s)));
            if (!(dum < big)) {
                big = dum;
                imax = i;
            }
        }
        if (j != imax) {
            for (int k = 0; k < 4; ++k) std::swap(a[imax][k], a[j][k]);
            d = -d;
            vv[imax] = vv[j];
        }
        indx[j] = imax;
        if (std::fabs(a[j][j]) < 1e-15) return false;
        if (j != 3) {
            const float dum = F(1.0 / a[j][j]);
            for (int i = j + 1; i < 4; ++i) a[i][j] = F(static_cast<double>(a[i][j]) * dum);
        }
    }
    return true;
}

void cmLub(float a[4][4], const int indx[4], float b[4]) {
    int ii = -1;
    for (int i = 0; i < 4; ++i) {
        const int ip = indx[i];
        float s = b[ip];
        b[ip] = b[i];
        if (ii >= 0) {
            for (int j = ii; j < i; ++j) s = F(s - static_cast<double>(a[i][j]) * b[j]);
        } else if (s != 0.0f) {
            ii = i;
        }
        b[i] = s;
    }
    for (int i = 3; i >= 0; --i) {
        float s = b[i];
        for (int j = i + 1; j < 4; ++j) s = F(s - static_cast<double>(a[i][j]) * b[j]);
        b[i] = F(static_cast<double>(s) / a[i][i]);
    }
}

CM4 cmInverseTranspose(const CM4& src) {
    CM4 r;
    if (src.ident) return r;
    float a[4][4];
    std::memcpy(a, src.m, sizeof(a));
    int indx[4];
    double d = 0.0;
    if (!cmLud(a, indx, d)) throw std::runtime_error("Mesh-Transformation ist nicht umkehrbar (Skalierung 0?)");
    CM4 inv;
    for (int c = 0; c < 4; ++c) {
        float b[4] = {0, 0, 0, 0};
        b[c] = 1.0f;
        cmLub(a, indx, b);
        for (int i = 0; i < 4; ++i) inv.m[i][c] = b[i];
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = inv.m[j][i];
    r.ident = false;
    return r;
}

float cmDetSign(const CM4& m) {
    if (m.ident) return 1.0f;
    float a[4][4];
    std::memcpy(a, m.m, sizeof(a));
    int indx[4];
    double d = 0.0;
    if (!cmLud(a, indx, d)) return 0.0f;
    float t = F(d * a[0][0]);
    t = F(static_cast<double>(t) * a[1][1]);
    t = F(static_cast<double>(t) * a[2][2]);
    t = F(static_cast<double>(t) * a[3][3]);
    return t;
}

// Local matrix of an SI_Transform SRT- (sx sy sz rx ry rz tx ty tz), already
// converted to Z-up - Carcass 0x449505..0x44990c.
CM4 cmSrt(const std::array<float, 9>& v) {
    const float negPi = F(-3.14159265358979323846);
    const CM4 S = cmScale(v[0], v[2], v[1]);
    const float a1 = F((static_cast<double>(v[3]) * negPi) / 180.0);
    const float a2 = F((static_cast<double>(v[4]) * negPi) / 180.0);
    const float a3 = F((static_cast<double>(v[5]) * negPi) / 180.0);
    CM4 R = cmMul(cmRot(0, a1), cmMul(cmRot(1, a2), cmRot(2, a3)));
    float A[3], B[3];
    for (int k = 0; k < 3; ++k) {
        A[k] = R.m[1][k];
        B[k] = -R.m[2][k];
    }
    for (int k = 0; k < 3; ++k) {
        R.m[2][k] = A[k];
        R.m[1][k] = B[k];
    }
    R.ident = false;
    for (int r = 0; r < 3; ++r) {
        const float c1 = R.m[r][1], c2 = R.m[r][2];
        R.m[r][1] = -c2;
        R.m[r][2] = c1;
    }
    CM4 M = cmMul(S, R);
    M.m[3][0] = v[6];
    M.m[3][1] = -v[8];
    M.m[3][2] = v[7];
    M.ident = false;
    M.checkIdent();
    return M;
}

// --- Names ------------------------------------------------------------------

// The trailing run of [A-Za-z0-9_] of a template name, as Carcass reduces
// every node name (0x4461c0): "MDL-B:Foo.model_root" -> "model_root".
std::string tailName(const std::string& s) {
    std::size_t i = s.size();
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')) --i;
    return s.substr(i);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// LOD from the name: "<name>_1".."_9" -> LOD 1..9, else 0.
int lodOfName(const std::string& n) {
    if (n.size() > 2 && n[n.size() - 2] == '_' && n.back() >= '1' && n.back() <= '9') return n.back() - '0';
    return 0;
}

// "<name>_<digit>" -> "<name>" (Carcass strips it from every surface name).
std::string stripLodSuffix(std::string n) {
    if (n.size() > 2 && n[n.size() - 2] == '_' && std::isdigit(static_cast<unsigned char>(n.back()))) n.resize(n.size() - 2);
    return n;
}

// Bone identity as Carcass matches mesh deformers to skeleton bones:
// case-insensitive, by GLA name.
std::string looseKey(const std::string& name) { return lower(glaBoneName(name)); }

// --- SI_Shape ---------------------------------------------------------------

// One attribute block from SI_Shape: count, identifier, then the values.
struct ShapeArray {
    std::string        kind;   // POSITION, NORMAL, TEX_COORD_UV0, ...
    std::vector<float> values;
    int                stride = 3;

    std::size_t count() const { return values.size() / static_cast<std::size_t>(stride); }
};

std::vector<ShapeArray> parseShape(const Template& shape) {
    // Layout: <arrayCount>, "ORDERED", then per array:
    //         <n>, "<kind>", <n*stride values>
    std::vector<ShapeArray> out;
    std::size_t i = 0;
    const auto& v = shape.values;

    // Skip over the leading count and "ORDERED".
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
        // TEX_COORD_UV0 additionally carries the projection name
        // ("Texture_Projection") - skip whatever is not a number, otherwise
        // every value after it is read off by one.
        while (i < v.size() && !v[i].asNumber()) ++i;

        a.values.reserve(std::min(need, v.size() - std::min(i, v.size())));
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
        if (a.kind.rfind(prefix, 0) == 0) {
            // A second UV set ("TEX_COORD_UV1") is not the texture coordinates.
            if (std::strcmp(prefix, "TEX_COORD") == 0 && a.kind != "TEX_COORD_UV" && a.kind != "TEX_COORD_UV0")
                continue;
            return &a;
        }
    return nullptr;
}

// --- Envelopes --------------------------------------------------------------

struct EnvelopeEntry {
    std::string      mesh;
    std::string      bone;
    std::vector<std::pair<int, float>> weights;   // position index, weight 0..1
};

std::vector<EnvelopeEntry> parseEnvelopes(const Document& doc) {
    std::vector<EnvelopeEntry> out;
    const Template* list = doc.findDeep("SI_EnvelopeList");
    if (!list) return out;

    for (const Template* e : list->findAll("SI_Envelope")) {
        if (e->values.size() < 3) continue;
        EnvelopeEntry en;
        en.mesh = tailName(e->values[0].text());
        en.bone = tailName(e->values[1].text());
        const auto n = e->values[2].asInt();
        if (!n || *n <= 0) continue;

        en.weights.reserve(std::min(static_cast<std::size_t>(*n), e->values.size() / 2));
        for (std::size_t i = 3; i + 1 < e->values.size(); i += 2) {
            const auto idx = e->values[i].asInt();
            const auto w = e->values[i + 1].asNumber();
            if (!idx || !w) continue;
            // Percent, as Carcass reads it: float(float(text) / 100). Zero
            // weights are listed explicitly and dropped.
            const float wv = F(static_cast<double>(F(*w)) / 100.0);
            if (!(wv > 0.0f)) continue;
            en.weights.emplace_back(static_cast<int>(*idx), wv);
        }
        out.push_back(std::move(en));
    }
    return out;
}

// --- The node tree ----------------------------------------------------------

struct Node {
    std::string     name;       // tail name, case kept
    int             parent = -1;
    const Template* model = nullptr;
    const Template* mesh = nullptr;
    std::string     material;   // for meshes
    CM4             world;
};

// Depth-first, a node before its children (Carcass's surface order). The
// shader comes from the last XSI_CustomPSet "Shader" seen among the PRECEDING
// models of the same level (mesh or not, Carcass 0x449156); every level starts
// with "[NoMaterial]".
void collectNodes(const std::vector<Template>& level, int parent, std::vector<Node>& out) {
    std::string shader = "[NoMaterial]";
    for (const auto& c : level) {
        if (c.type != "SI_Model") continue;
        Node n;
        n.name = tailName(c.name);
        n.parent = parent;
        n.model = &c;
        CM4 local;
        bool haveSrt = false;
        for (const auto& ch : c.children) {
            if (ch.type == "XSI_CustomPSet") {
                for (std::size_t i = 0; i + 2 < ch.values.size(); ++i)
                    if (ch.values[i].text() == "Shader") {
                        std::string s = lower(ch.values[i + 2].text());
                        std::replace(s.begin(), s.end(), '\\', '/');
                        if (!s.empty() && s[0] == '/') s.erase(0, 1);
                        shader = s;
                    }
            } else if (ch.type == "SI_Transform" && !haveSrt && ch.name.rfind("SRT-", 0) == 0 &&
                       ch.values.size() >= 9) {
                std::array<float, 9> v{};
                for (std::size_t k = 0; k < 9; ++k) {
                    const auto d = ch.values[k].asNumber();
                    v[k] = d ? F(*d) : 0.0f;
                }
                for (int k = 0; k < 3; ++k)
                    if (std::fabs(v[static_cast<std::size_t>(k)]) < 1e-6f)
                        throw std::runtime_error("Modell \"" + n.name + "\" hat eine Skalierung von 0 (SRT)");
                local = cmSrt(v);
                haveSrt = true;
            } else if (ch.type == "SI_Mesh" && !n.mesh) {
                n.mesh = &ch;
                // A CustomPSet AFTER the SI_Mesh only reaches later siblings.
                n.material = shader;
            }
        }
        n.world = parent >= 0 ? cmMul(local, out[static_cast<std::size_t>(parent)].world) : local;
        const int self = static_cast<int>(out.size());
        out.push_back(std::move(n));
        collectNodes(c.children, self, out);
    }
}

// --- Vertices ---------------------------------------------------------------

struct GVert {
    std::array<float, 3> p{};
    std::array<float, 3> n{};
    float u = 0.0f, v = 0.0f;
    std::vector<VertexWeight> w;
};

const float T001 = F(0.001);
const float T01 = F(0.01);

// 0x445d90: dx rounded to float, dy/dz kept in double.
double sqDist(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const double dx = F(static_cast<double>(a[0]) - b[0]);
    const double dy = static_cast<double>(a[1]) - b[1];
    const double dz = static_cast<double>(a[2]) - b[2];
    return (dz * dz + dx * dx) + dy * dy;
}

bool weightsEqual(const std::vector<VertexWeight>& a, const std::vector<VertexWeight>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].boneIndex != b[i].boneIndex ||
            !(std::fabs(F(static_cast<double>(a[i].weight) - b[i].weight)) < T001))
            return false;
    return true;
}

// Carcass's merge rule while grabbing (0x426780): the same vertex if weights,
// position (squared distance < 0.001, unscaled), normal (< 0.01) and UV
// (< 0.001 each) match - by VALUE, whatever position index it came from.
bool grabEqual(const GVert& a, const GVert& b) {
    if (!weightsEqual(a.w, b.w)) return false;
    if (!(sqDist(a.p, b.p) < T001)) return false;
    if (!(sqDist(a.n, b.n) < T01)) return false;
    if (!(std::fabs(F(static_cast<double>(a.u) - b.u)) < T001)) return false;
    if (!(std::fabs(F(static_cast<double>(a.v) - b.v)) < T001)) return false;
    return true;
}

// -losedupverts (0x43f8d4): stricter, per component, on scaled positions.
bool dupEqual(const GVert& a, const GVert& b) {
    for (int q = 0; q < 3; ++q) {
        if (!(std::fabs(static_cast<double>(a.n[static_cast<std::size_t>(q)]) - b.n[static_cast<std::size_t>(q)]) < T001)) return false;
        if (!(std::fabs(static_cast<double>(a.p[static_cast<std::size_t>(q)]) - b.p[static_cast<std::size_t>(q)]) < T001)) return false;
    }
    if (!(std::fabs(F(static_cast<double>(a.u) - b.u)) < T001)) return false;
    if (!(std::fabs(F(static_cast<double>(a.v) - b.v)) < T001)) return false;
    return weightsEqual(a.w, b.w);
}

struct GSurf {
    std::string           rawName;
    int                   node = -1;
    int                   lod = 0;
    std::vector<GVert>    verts;
    std::vector<std::array<int, 3>> tris;
    std::string           material;     // as read (lower-cased later)
    std::string           parentName;   // raw name of the parent surface, "" = none
};

// -smooth (0x4397f1): for every vertex, the normals of all vertices of the
// same LOD at the same place (each component < 0.001 apart) are added and
// normalised. Carcass adds every normal that is not exactly perpendicular -
// opposite ones too, so a double-sided sheet ends up with zero normals. Here
// only normals of the same side (dot > 0) are added, and tags never take part:
// they are not rendered, and in Carcass they bent the body normals at the
// seams (SPEC 11.1).
void smoothLod(std::vector<GSurf*>& surfs, std::vector<bool> isTag, bool carcass) {
    if (carcass) isTag.assign(isTag.size(), false);
    const double thr = static_cast<double>(F(std::cos(1.5707963267948966)));
    std::vector<GVert*> all;
    std::vector<bool> tagOf;
    for (std::size_t s = 0; s < surfs.size(); ++s)
        for (auto& v : surfs[s]->verts) {
            all.push_back(&v);
            tagOf.push_back(isTag[s]);
        }
    std::vector<std::array<float, 3>> acc(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const GVert& v = *all[i];
        if (tagOf[i]) {
            acc[i] = v.n;
            continue;
        }
        std::array<float, 3> a{0.0f, 0.0f, 0.0f};
        for (std::size_t j = 0; j < all.size(); ++j) {
            if (tagOf[j]) continue;
            const GVert& w = *all[j];
            if (!(std::fabs(F(static_cast<double>(w.p[0]) - v.p[0])) < T001 &&
                  std::fabs(F(static_cast<double>(w.p[1]) - v.p[1])) < T001 &&
                  std::fabs(F(static_cast<double>(w.p[2]) - v.p[2])) < T001))
                continue;
            const double d = (static_cast<double>(w.n[2]) * v.n[2] + static_cast<double>(w.n[1]) * v.n[1]) +
                             static_cast<double>(w.n[0]) * v.n[0];
            if (carcass) {
                if (std::fabs(F(d)) < thr) continue;
            } else if (!(d > 0.0) && &w != &v) {
                continue;
            }
            for (int k = 0; k < 3; ++k) a[static_cast<std::size_t>(k)] = F(static_cast<double>(a[static_cast<std::size_t>(k)]) + w.n[static_cast<std::size_t>(k)]);
        }
        acc[i] = a;
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& a = acc[i];
        const float l = F(std::sqrt(static_cast<double>(F((static_cast<double>(a[0]) * a[0] + static_cast<double>(a[1]) * a[1]) + static_cast<double>(a[2]) * a[2]))));
        if (l > 1e-10f) {
            const double inv = 1.0 / l;
            all[i]->n = {F(a[0] * inv), F(a[1] * inv), F(a[2] * inv)};
        } else {
            all[i]->n = {0.0f, 0.0f, 0.0f};
        }
    }
}

// A tag's three vertices in the order the engine reads its axes from: the
// start vertices of the hypotenuse, the medium and the short edge (MakeTag,
// 0x41a2c0). Carcass reused the previous tag's choice when two edges were
// exactly equal and could pick one vertex twice; here the remaining one is
// taken.
void reorderTag(GSurf& s) {
    std::array<float, 3> L{};
    for (int i = 0; i < 3; ++i) {
        const auto& a = s.verts[static_cast<std::size_t>((i + 1) % 3)].p;
        const auto& b = s.verts[static_cast<std::size_t>(i)].p;
        const double e0 = static_cast<double>(a[0]) - b[0], e1 = static_cast<double>(a[1]) - b[1],
                     e2 = static_cast<double>(a[2]) - b[2];
        L[static_cast<std::size_t>(i)] = F(std::sqrt((e2 * e2 + e1 * e1) + e0 * e0));
    }
    const auto strictMax = [](const std::array<float, 3>& l) {
        if (l[0] > l[1] && l[0] > l[2]) return 0;
        if (l[1] > l[0] && l[1] > l[2]) return 1;
        if (l[2] > l[0] && l[2] > l[1]) return 2;
        return -1;
    };
    const int h = strictMax(L);
    if (h < 0)
        throw std::runtime_error("Tag \"" + s.rawName + "\": das Dreieck braucht eine eindeutig laengste Seite "
                                 "(gleichseitige Tags haben keine Richtung)");
    L[static_cast<std::size_t>(h)] = -1.0f;
    int m = strictMax(L);
    if (m < 0) m = h == 0 ? 1 : 0;
    L[static_cast<std::size_t>(m)] = -1.0f;
    const int sh = 3 - h - m;
    s.verts = {s.verts[static_cast<std::size_t>(h)], s.verts[static_cast<std::size_t>(m)],
               s.verts[static_cast<std::size_t>(sh)]};
}

}  // namespace

void finishMesh(std::vector<GSurf>& surfs, const MeshImportOptions& opt, MeshImportResult& res,
                bool computeNormals);

std::string surfaceNameToGlm(const std::string& xsiName) {
    std::string s = lower(stripLodSuffix(xsiName.substr(0, 63)));
    if (s.rfind("bolt_", 0) == 0) s = "*" + s.substr(5);
    return s;
}

std::uint32_t surfaceFlagsFromName(const std::string& glmName) {
    std::uint32_t f = 0;
    if (!glmName.empty() && glmName[0] == '*') f |= fmt::kSurfFlagIsBolt;
    if (glmName.size() >= 4 && lower(glmName.substr(glmName.size() - 4)) == "_off") f |= fmt::kSurfFlagOff;
    return f;
}

MeshImportResult importMesh(const Document& doc, const MeshImportOptions& opt) {
    MeshImportResult res;
    res.mesh.name = opt.modelName;
    res.mesh.animName = opt.animName;
    res.mesh.numBones = static_cast<int>(opt.boneNames.size());
    auto& warn = res.stats.warnings;

    // --- Skeleton lookup --------------------------------------------------
    std::map<std::string, int> boneIndex, boneIndexLoose;
    for (std::size_t i = 0; i < opt.boneNames.size(); ++i) {
        boneIndex[opt.boneNames[i]] = static_cast<int>(i);
        boneIndexLoose.emplace(looseKey(opt.boneNames[i]), static_cast<int>(i));
        const auto al = opt.aliases.find(opt.boneNames[i]);
        if (al != opt.aliases.end()) boneIndex[al->second] = static_cast<int>(i);
    }

    // --- Nodes ------------------------------------------------------------
    std::vector<Node> nodes;
    collectNodes(doc.roots, -1, nodes);

    // --- Weights per mesh node and position --------------------------------
    std::map<std::string, std::vector<const Node*>> meshByName;
    for (const auto& n : nodes)
        if (n.mesh) meshByName[n.name].push_back(&n);
    std::map<std::string, std::string> parentName;   // node -> parent node, for moving weights up
    for (const auto& n : nodes)
        if (n.parent >= 0) parentName[n.name] = nodes[static_cast<std::size_t>(n.parent)].name;

    std::map<const Node*, std::map<int, std::vector<VertexWeight>>> weights;
    std::set<std::string> outside, movedUp;
    for (const auto& e : parseEnvelopes(doc)) {
        int bone = -1;
        if (const auto bi = boneIndex.find(e.bone); bi != boneIndex.end()) bone = bi->second;
        // Case-insensitive and by GLA name ("face_always_" -> "face").
        if (bone < 0)
            if (const auto bl = boneIndexLoose.find(looseKey(e.bone)); bl != boneIndexLoose.end()) bone = bl->second;
        // Not in the skeleton (cut off by $bonehiercap): the nearest
        // ancestor that is in it takes the weight.
        if (bone < 0) {
            for (auto up = parentName.find(e.bone); up != parentName.end() && bone < 0;
                 up = parentName.find(up->second)) {
                if (const auto bi = boneIndex.find(up->second); bi != boneIndex.end()) bone = bi->second;
                else if (const auto bl = boneIndexLoose.find(looseKey(up->second)); bl != boneIndexLoose.end())
                    bone = bl->second;
            }
            if (bone >= 0 && !e.weights.empty()) movedUp.insert(e.bone);
        }
        if (bone < 0) {
            if (!e.weights.empty()) outside.insert(e.bone);
            continue;
        }
        const auto mi = meshByName.find(e.mesh);
        if (mi == meshByName.end()) continue;
        for (const Node* mn : mi->second)
            for (const auto& [idx, w] : e.weights) weights[mn][idx].push_back(VertexWeight{bone, w});
    }
    if (!movedUp.empty()) {
        std::string list;
        for (const auto& b : movedUp) list += (list.empty() ? "" : ", ") + b;
        res.stats.notes.push_back("Gewichte von " + list + " auf den naechsten Bone im Skelett darueber");
    }
    if (!outside.empty()) {
        std::string list;
        for (const auto& b : outside) list += (list.empty() ? "" : ", ") + b;
        warn.push_back("Gewichte auf Bones ausserhalb des Skeletts (" + list + ") fallen weg");
    }

    // --- Grab every mesh ---------------------------------------------------
    std::vector<GSurf> surfs;
    std::size_t vertsWithoutWeights = 0;
    for (std::size_t ni = 0; ni < nodes.size(); ++ni) {
        const Node& nd = nodes[ni];
        if (!nd.mesh) continue;
        GSurf s;
        s.rawName = nd.name;
        s.node = static_cast<int>(ni);
        s.lod = lodOfName(nd.name);
        if (s.lod > 0 && nd.name.find("tag_") != std::string::npos) continue;
        if (s.lod == 0 && nd.name.rfind("tag_", 0) == 0)
            throw std::runtime_error("Mesh \"" + nd.name + "\" hat einen alten Tag-Namen - Tags heissen "
                                     "\"bolt_...\"");
        if (nd.material.size() > 1 && nd.material[1] == ':')
            throw std::runtime_error("Material \"" + nd.material + "\" von \"" + nd.name +
                                     "\" ist ein voller Pfad - erlaubt ist nur ein Pfad unter base/");

        const Template* shape = nd.mesh->findDeep("SI_Shape");
        std::vector<const Template*> triLists = nd.mesh->findAll("SI_TriangleList");
        for (const Template* pl : nd.mesh->findAll("SI_PolygonList")) triLists.push_back(pl);
        if (!shape || triLists.empty()) {
            warn.push_back("Mesh \"" + nd.name + "\" ohne Shape oder Dreiecke - leere Surface");
            surfs.push_back(std::move(s));
            continue;
        }
        const auto arrays = parseShape(*shape);
        const ShapeArray* pos = findArray(arrays, "POSITION");
        const ShapeArray* nrm = findArray(arrays, "NORMAL");
        const ShapeArray* uv = findArray(arrays, "TEX_COORD");
        if (!pos) {
            warn.push_back("Mesh \"" + nd.name + "\" ohne POSITION - leere Surface");
            surfs.push_back(std::move(s));
            continue;
        }
        // (x, y, z) -> (x, -z, y)
        const auto conv = [](const ShapeArray& a, std::size_t i) {
            return std::array<float, 3>{a.values[3 * i], -a.values[3 * i + 2], a.values[3 * i + 1]};
        };
        std::vector<std::array<float, 3>> worldPos(pos->count());
        for (std::size_t i = 0; i < worldPos.size(); ++i) worldPos[i] = cmPoint(nd.world, conv(*pos, i));
        const CM4 N = cmInverseTranspose(nd.world);
        const bool mirrored = cmDetSign(nd.world) < 0.0f;
        const auto& wByPos = weights[&nd];

        for (const Template* tl : triLists) {
            if (tl->values.size() < 3) {
                warn.push_back("Dreiecksliste von \"" + nd.name + "\" ist leer");
                continue;
            }
            const auto tc = tl->values[0].asInt();
            if (!tc || *tc <= 0) continue;   // Carcass ignores a list with count 0
            const bool polygons = tl->type == "SI_PolygonList";
            const std::size_t nTri = static_cast<std::size_t>(*tc);
            std::size_t at = 3;
            if (polygons) {
                // Per-polygon vertex counts; every one must be 3.
                if (tl->values.size() < at + nTri) {
                    warn.push_back("Polygonliste von \"" + nd.name + "\" abgeschnitten");
                    continue;
                }
                for (std::size_t k = 0; k < nTri; ++k)
                    if (tl->values[at + k].asInt().value_or(0) != 3)
                        throw std::runtime_error("Mesh \"" + nd.name + "\" enthaelt Vierecke - nur Dreiecke");
                at += nTri;
            }
            const std::size_t n = nTri * 3;
            std::vector<std::string> kinds;
            {
                const std::string attribs = tl->values[1].text();
                std::size_t from = 0;
                while (from <= attribs.size()) {
                    const std::size_t bar = attribs.find('|', from);
                    const std::string k = attribs.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
                    if (!k.empty()) kinds.push_back(k);
                    if (bar == std::string::npos) break;
                    from = bar + 1;
                }
            }
            bool ok = true;
            const auto block = [&](std::vector<int>* dst) {
                if (!ok || n > tl->values.size() || at > tl->values.size() - n) {
                    ok = false;
                    return;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const auto v = tl->values[at + k].asInt();
                    if (!v) {
                        ok = false;
                        return;
                    }
                    if (dst) dst->push_back(static_cast<int>(*v));
                }
                at += n;
            };
            // The blocks in the order the attribute string names them.
            // Carcass ignored the string and read a COLOR block as the UVs
            // (SPEC 11.17); a second UV set is skipped here too.
            std::vector<int> p, nn, uu;
            bool gotN = false, gotU = false;
            block(&p);
            for (const auto& k : kinds) {
                if (k == "NORMAL" && !gotN) {
                    block(&nn);
                    gotN = true;
                } else if ((k == "TEX_COORD_UV" || k == "TEX_COORD_UV0") && !gotU) {
                    block(&uu);
                    gotU = true;
                } else {
                    block(nullptr);
                }
            }
            if (!ok) {
                warn.push_back("Dreiecksliste von \"" + nd.name + "\" abgeschnitten oder mit ungueltigem Index");
                continue;
            }

            for (std::size_t t = 0; t < nTri; ++t) {
                std::array<std::size_t, 3> c{3 * t, 3 * t + 1, 3 * t + 2};
                // Mirrored model: keep the winding right.
                if (mirrored) std::swap(c[0], c[1]);
                // Face normal for meshes without NORMAL (Carcass wrote zero
                // normals there, SPEC 11.10).
                std::array<float, 3> faceN{0.0f, 0.0f, 0.0f};
                const auto posOf = [&](std::size_t corner) -> const std::array<float, 3>* {
                    const int pi = p[corner];
                    if (pi < 0 || static_cast<std::size_t>(pi) >= worldPos.size()) return nullptr;
                    return &worldPos[static_cast<std::size_t>(pi)];
                };
                if (!gotN) {
                    const auto* a = posOf(c[0]);
                    const auto* b = posOf(c[1]);
                    const auto* d = posOf(c[2]);
                    if (a && b && d) {
                        const double ux = (*b)[0] - (*a)[0], uy = (*b)[1] - (*a)[1], uz = (*b)[2] - (*a)[2];
                        const double vx = (*d)[0] - (*a)[0], vy = (*d)[1] - (*a)[1], vz = (*d)[2] - (*a)[2];
                        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                        const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (l > 1e-12) faceN = {F(-nx / l), F(-ny / l), F(-nz / l)};
                    }
                }
                std::array<int, 3> tri{};
                const int order[3] = {0, 2, 1};   // Carcass grabs corners 0, 2, 1
                for (int k = 0; k < 3; ++k) {
                    const std::size_t corner = c[static_cast<std::size_t>(order[k])];
                    const int pi = p[corner];
                    if (pi < 0 || static_cast<std::size_t>(pi) >= worldPos.size())
                        throw std::runtime_error("Mesh \"" + nd.name + "\": Positionsindex " + std::to_string(pi) +
                                                 " ausserhalb der Punktliste");
                    GVert v;
                    v.p = worldPos[static_cast<std::size_t>(pi)];
                    if (gotN && nrm) {
                        const int ix = nn[corner];
                        if (ix < 0 || static_cast<std::size_t>(ix) >= nrm->count())
                            throw std::runtime_error("Mesh \"" + nd.name + "\": Normalenindex ausserhalb");
                        std::array<float, 3> nz = cmVec(N, conv(*nrm, static_cast<std::size_t>(ix)));
                        const float l = F(std::sqrt(static_cast<double>(F((static_cast<double>(nz[2]) * nz[2] + static_cast<double>(nz[1]) * nz[1]) + static_cast<double>(nz[0]) * nz[0]))));
                        if (l > 1e-10f) {
                            const double inv = 1.0 / l;
                            nz = {F(nz[0] * inv), F(nz[1] * inv), F(nz[2] * inv)};
                        } else {
                            nz = {0.0f, 0.0f, 0.0f};
                        }
                        v.n = nz;
                    } else {
                        v.n = faceN;
                    }
                    float uu0 = 0.0f, vv0 = 0.0f;
                    if (gotU && uv) {
                        const int ix = uu[corner];
                        if (ix < 0 || static_cast<std::size_t>(ix) >= uv->count())
                            throw std::runtime_error("Mesh \"" + nd.name + "\": UV-Index ausserhalb");
                        uu0 = uv->values[2 * static_cast<std::size_t>(ix)];
                        vv0 = uv->values[2 * static_cast<std::size_t>(ix) + 1];
                        if (vv0 < 0.0f) vv0 = F(static_cast<double>(vv0) + 1.0);
                    }
                    v.u = uu0;
                    v.v = F(1.0 - vv0);
                    // Weights of the position; one bone listed twice is summed.
                    if (const auto wi = wByPos.find(pi); wi != wByPos.end())
                        for (const auto& w : wi->second) {
                            bool merged = false;
                            for (auto& x : v.w)
                                if (x.boneIndex == w.boneIndex) {
                                    x.weight = F(static_cast<double>(x.weight) + w.weight);
                                    merged = true;
                                    break;
                                }
                            if (!merged) v.w.push_back(w);
                        }
                    if (v.w.empty()) {
                        // Carcass refuses such a vertex. Bound to bone 0 so the
                        // model at least stays well-defined, and reported.
                        ++vertsWithoutWeights;
                        v.w.push_back(VertexWeight{0, 1.0f});
                    }
                    int hit = -1;
                    for (std::size_t e = 0; e < s.verts.size(); ++e)
                        if (grabEqual(v, s.verts[e])) {
                            hit = static_cast<int>(e);
                            break;
                        }
                    if (hit < 0) {
                        hit = static_cast<int>(s.verts.size());
                        s.verts.push_back(std::move(v));
                    }
                    tri[static_cast<std::size_t>(k)] = hit;
                }
                s.tris.push_back(tri);
            }
        }
        surfs.push_back(std::move(s));
    }
    if (surfs.empty()) throw std::runtime_error("Keine Mesh-Modelle in der dotXSI gefunden");
    for (auto& sf : surfs) {
        const Node& nd = nodes[static_cast<std::size_t>(sf.node)];
        sf.material = nd.material;
        // Parent surface: the nearest ancestor model that is a mesh.
        int p = nd.parent;
        while (p >= 0 && !nodes[static_cast<std::size_t>(p)].mesh) p = nodes[static_cast<std::size_t>(p)].parent;
        sf.parentName = p >= 0 ? nodes[static_cast<std::size_t>(p)].name : std::string();
    }
    if (vertsWithoutWeights)
        warn.push_back(std::to_string(vertsWithoutWeights) + " Vertices ohne Gewicht - an Bone 0 gehaengt "
                       "(Carcass bricht hier ab)");
    finishMesh(surfs, opt, res, false);
    return res;
}

// Everything after reading: LODs, strips, scale, normals (ASE), smoothing,
// tags, duplicates, the GLM surfaces and the checks - one path for dotXSI and
// ASE.
void finishMesh(std::vector<GSurf>& surfs, const MeshImportOptions& opt, MeshImportResult& res,
                bool computeNormals) {
    auto& warn = res.stats.warnings;

    // --- LODs ---------------------------------------------------------------
    std::vector<std::vector<GSurf*>> lods;
    for (int L = 0; L < 10; ++L) {
        std::vector<GSurf*> ls;
        for (auto& s : surfs)
            if (s.lod == L) ls.push_back(&s);
        if (ls.empty()) break;   // the first empty LOD ends the list (Carcass)
        lods.push_back(std::move(ls));
    }
    for (std::size_t L = 1; L < 10; ++L) {
        bool more = false;
        for (const auto& s : surfs)
            if (s.lod == static_cast<int>(L) && L >= lods.size()) more = true;
        if (more) {
            warn.push_back("LOD " + std::to_string(L) + " folgt nach einer Luecke und wird nicht benutzt");
            break;
        }
    }
    for (std::size_t L = 1; L < lods.size(); ++L)
        if (lods[L].size() != lods[0].size())
            throw std::runtime_error("LOD " + std::to_string(L) + " hat " + std::to_string(lods[L].size()) +
                                     " Surfaces, LOD 0 hat " + std::to_string(lods[0].size()) +
                                     " - alle LODs muessen gleich viele haben");

    const auto parentMeshName = [](const GSurf& s) { return s.parentName; };

    // --- Finish every surface: strips, scale, smoothing, tags, duplicates ----
    const float scale = F(opt.scale);
    res.stats.deletedDupVertsPerLod.assign(lods.size(), 0);
    res.stats.deletedDupWeightsPerLod.assign(lods.size(), 0);
    for (std::size_t li = 0; li < lods.size(); ++li) {
        auto& ls = lods[li];
        for (GSurf* s : ls) {
            if (!s->tris.empty()) s->tris = Stripper(s->tris).order();
            for (auto& v : s->verts)
                for (auto& c : v.p) c = F(static_cast<double>(c) * scale);
            if (computeNormals) {
                // ASE has no usable normals: per vertex the sum of
                // (p0 - p1) x (p2 - p1) of its triangles, normalised
                // (Carcass 0x432810). Zero length gives (0,0,0) - Carcass
                // wrote NaN.
                std::vector<std::array<double, 3>> acc(s->verts.size(), {0.0, 0.0, 0.0});
                for (const auto& t : s->tris) {
                    const auto& p0 = s->verts[static_cast<std::size_t>(t[0])].p;
                    const auto& p1 = s->verts[static_cast<std::size_t>(t[1])].p;
                    const auto& p2 = s->verts[static_cast<std::size_t>(t[2])].p;
                    const double a[3] = {static_cast<double>(p0[0]) - p1[0], static_cast<double>(p0[1]) - p1[1],
                                         static_cast<double>(p0[2]) - p1[2]};
                    const double b[3] = {static_cast<double>(p2[0]) - p1[0], static_cast<double>(p2[1]) - p1[1],
                                         static_cast<double>(p2[2]) - p1[2]};
                    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                         a[0] * b[1] - a[1] * b[0]};
                    for (int k : t)
                        for (int q = 0; q < 3; ++q) acc[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)] += c[q];
                }
                for (std::size_t i = 0; i < s->verts.size(); ++i) {
                    const auto& a = acc[i];
                    const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                    s->verts[i].n = l > 0.0 ? std::array<float, 3>{F(a[0] / l), F(a[1] / l), F(a[2] / l)}
                                            : std::array<float, 3>{0.0f, 0.0f, 0.0f};
                }
            }
        }
        if (opt.smooth) {
            std::vector<bool> isTag;
            for (GSurf* s : ls) isTag.push_back(lower(s->rawName).rfind("bolt_", 0) == 0);
            smoothLod(ls, isTag, opt.carcassSmooth);
        }
        for (GSurf* s : ls) {
            if (lower(s->rawName).rfind("bolt_", 0) != 0) continue;
            if (s->verts.size() != 3)
                throw std::runtime_error("Tag \"" + s->rawName + "\" hat " + std::to_string(s->verts.size()) +
                                         " Vertices, ein Tag braucht genau 3");
            reorderTag(*s);
        }
        if (opt.loseDupVerts || opt.smooth) {
            for (GSurf* s : ls) {
                std::vector<int> rep(s->verts.size());
                std::vector<int> newIdx(s->verts.size(), -1);
                std::vector<GVert> kept;
                for (std::size_t i = 0; i < s->verts.size(); ++i) {
                    rep[i] = static_cast<int>(i);
                    for (std::size_t k = 0; k < i; ++k)
                        if (dupEqual(s->verts[i], s->verts[k])) {
                            // To k's representative (Carcass pointed at k even
                            // if k itself was deleted, SPEC 11.9).
                            rep[i] = rep[k];
                            break;
                        }
                    if (rep[i] == static_cast<int>(i)) {
                        newIdx[i] = static_cast<int>(kept.size());
                        kept.push_back(s->verts[i]);
                    } else {
                        ++res.stats.deletedDupVerts;
                        res.stats.deletedDupWeights += s->verts[i].w.size();
                        ++res.stats.deletedDupVertsPerLod[li];
                        res.stats.deletedDupWeightsPerLod[li] += s->verts[i].w.size();
                    }
                }
                for (auto& t : s->tris)
                    for (auto& k : t) k = newIdx[static_cast<std::size_t>(rep[static_cast<std::size_t>(k)])];
                s->verts = std::move(kept);
            }
        }
    }

    // --- Into the mesh ------------------------------------------------------
    // LOD 0 decides names, flags, shaders and hierarchy; LOD n surfaces are
    // matched to it by name and written in its order.
    const auto& L0 = lods[0];
    std::vector<int> parentIdx(L0.size(), -1);
    for (std::size_t i = 0; i < L0.size(); ++i) {
        const std::string pn = parentMeshName(*L0[i]);
        if (pn.empty()) continue;
        for (std::size_t j = 0; j < L0.size(); ++j)
            if (L0[j]->rawName == pn) {
                parentIdx[i] = static_cast<int>(j);
                break;
            }
    }
    bool allUvBlank = true;
    for (std::size_t L = 0; L < lods.size(); ++L) {
        std::vector<GSurf*> order(L0.size(), nullptr);
        if (L == 0) {
            order = L0;
        } else {
            for (GSurf* s : lods[L]) {
                const std::string want = lower(stripLodSuffix(s->rawName));
                std::size_t j = 0;
                for (; j < L0.size(); ++j)
                    if (lower(L0[j]->rawName) == want) break;
                if (j == L0.size())
                    throw std::runtime_error("LOD " + std::to_string(L) + ": zu \"" + s->rawName +
                                             "\" gibt es in LOD 0 keine Surface gleichen Namens");
                order[j] = s;
            }
            for (std::size_t j = 0; j < L0.size(); ++j)
                if (!order[j])
                    throw std::runtime_error("LOD 0 Surface \"" + L0[j]->rawName + "\" fehlt in LOD " +
                                             std::to_string(L));
        }
        LOD lod;
        for (std::size_t j = 0; j < L0.size(); ++j) {
            const GSurf& src = *order[j];
            Surface sf;
            sf.name = surfaceNameToGlm(L0[j]->rawName);
            sf.flags = surfaceFlagsFromName(sf.name);
            sf.parentIndex = parentIdx[j];
            std::string sh = lower(L0[j]->material.substr(0, 63));
            if (sh.empty() && !(sf.flags & fmt::kSurfFlagIsBolt)) {
                if (L == 0) warn.push_back("Surface \"" + sf.name + "\" ohne Shader - \"[NoMaterial]\"");
                sh = "[NoMaterial]";
            }
            sf.shader = sh;
            for (const auto& v : src.verts) {
                Vertex x;
                for (int k = 0; k < 3; ++k) {
                    x.position[k] = v.p[static_cast<std::size_t>(k)];
                    x.normal[k] = v.n[static_cast<std::size_t>(k)];
                }
                x.uv[0] = v.u;
                x.uv[1] = v.v;
                x.weights = v.w;
                if (!(std::fabs(v.u) <= 0.001f && std::fabs(v.v - 1.0f) <= 0.001f)) allUvBlank = false;
                sf.vertices.push_back(std::move(x));
            }
            for (const auto& t : src.tris) sf.triangles.push_back(Triangle{{t[0], t[1], t[2]}});
            if (L == 0) {
                // The game's limits per surface: above them it refuses to
                // load the whole model.
                if (sf.vertices.size() > 1000 || sf.triangles.size() > 2000)
                    warn.push_back("Surface \"" + sf.name + "\": " + std::to_string(sf.vertices.size()) +
                                   " Vertices, " + std::to_string(sf.triangles.size()) +
                                   " Dreiecke - Jedi Academy laedt hoechstens 1000 Vertices und 2000 "
                                   "Dreiecke pro Surface");
                else if (sf.vertices.size() > 500)
                    res.stats.notes.push_back("Surface \"" + sf.name + "\": " + std::to_string(sf.vertices.size()) +
                                              " Vertices - ueber 500 keine Stencil-Schatten");
                if (sf.triangles.empty()) warn.push_back("Surface \"" + sf.name + "\" hat keine Dreiecke");
                if (sf.flags & fmt::kSurfFlagIsBolt) ++res.stats.tags;
                if (sf.flags & fmt::kSurfFlagOff) ++res.stats.offSurfaces;
            }
            res.stats.vertices += L == 0 ? sf.vertices.size() : 0;
            res.stats.triangles += L == 0 ? sf.triangles.size() : 0;
            lod.surfaces.push_back(std::move(sf));
        }
        res.mesh.lods.push_back(std::move(lod));
    }
    if (allUvBlank) warn.push_back("Alle Texturkoordinaten des Modells sind leer");

    // Every surface must hang below surface 0.
    {
        std::vector<std::vector<int>> kids(L0.size());
        for (std::size_t i = 0; i < L0.size(); ++i)
            if (parentIdx[i] >= 0) kids[static_cast<std::size_t>(parentIdx[i])].push_back(static_cast<int>(i));
        std::vector<bool> seen(L0.size(), false);
        std::vector<int> st{0};
        while (!st.empty()) {
            const int x = st.back();
            st.pop_back();
            if (seen[static_cast<std::size_t>(x)]) continue;
            seen[static_cast<std::size_t>(x)] = true;
            for (int k : kids[static_cast<std::size_t>(x)]) st.push_back(k);
        }
        std::string lost;
        for (std::size_t i = 0; i < L0.size(); ++i)
            if (!seen[i]) lost += (lost.empty() ? "" : ", ") + L0[i]->rawName;
        if (!lost.empty())
            throw std::runtime_error("Diese Surfaces haengen nicht unter der ersten (\"" + L0[0]->rawName +
                                     "\"): " + lost + " - alle Meshes muessen unter dem ersten Mesh liegen");
    }

    res.stats.surfaces = L0.size();
    res.stats.lods = lods.size();
}

MeshImportResult importMeshAse(const ase::Scene& scene, const MeshImportOptions& opt) {
    MeshImportResult res;
    res.mesh.name = opt.modelName;
    res.mesh.animName = opt.animName;
    res.mesh.numBones = static_cast<int>(opt.boneNames.size());
    for (const auto& w : scene.warnings) res.stats.warnings.push_back(w);

    std::map<std::string, int> boneLoose;
    for (std::size_t i = 0; i < opt.boneNames.size(); ++i) boneLoose.emplace(looseKey(opt.boneNames[i]), static_cast<int>(i));
    std::vector<int> boneOf(scene.bones.size(), -1);
    for (std::size_t i = 0; i < scene.bones.size(); ++i)
        if (const auto it = boneLoose.find(looseKey(scene.bones[i])); it != boneLoose.end()) boneOf[i] = it->second;

    std::set<std::string> objectNames;
    for (const auto& o : scene.objects) objectNames.insert(o.name);

    std::vector<GSurf> surfs;
    for (const auto& o : scene.objects) {
        GSurf s;
        s.rawName = o.name;
        s.lod = lodOfName(o.name);
        if (s.lod > 0 && o.name.find("tag_") != std::string::npos) continue;
        if (s.lod == 0 && o.name.rfind("tag_", 0) == 0)
            throw std::runtime_error("Objekt \"" + o.rawName + "\" hat einen alten Tag-Namen - Tags heissen \"bolt_...\"");
        s.material = o.materialRef >= 0 && static_cast<std::size_t>(o.materialRef) < scene.materials.size()
                         ? scene.materials[static_cast<std::size_t>(o.materialRef)]
                         : std::string();
        if (!o.parent.empty() && o.parent != o.name && objectNames.count(o.parent)) s.parentName = o.parent;
        else if (!surfs.empty()) s.parentName = surfs.front().rawName;

        for (std::size_t fi = 0; fi < o.faces.size(); ++fi) {
            std::array<int, 3> tri{};
            const int order[3] = {0, 2, 1};   // corners a, c, b
            for (int k = 0; k < 3; ++k) {
                const int corner = order[k];
                const auto& bv = o.boneVerts[static_cast<std::size_t>(o.faces[fi][static_cast<std::size_t>(corner)])];
                GVert v;
                v.p = bv.p;
                if (fi < o.tfaces.size()) {
                    const auto& t = o.tverts[static_cast<std::size_t>(o.tfaces[fi][static_cast<std::size_t>(corner)])];
                    v.u = t[0];
                    v.v = t[1];
                } else {
                    v.v = 1.0f;
                }
                for (const auto& w : bv.w) {
                    const int b = boneOf[static_cast<std::size_t>(w.bone)];
                    if (b < 0)
                        throw std::runtime_error("Bone \"" + scene.bones[static_cast<std::size_t>(w.bone)] +
                                                 "\" aus dem ASE gibt es im Skelett nicht");
                    if (!(w.weight > 0.0f)) continue;
                    bool merged = false;
                    for (auto& x : v.w)
                        if (x.boneIndex == b) {
                            x.weight = F(static_cast<double>(x.weight) + w.weight);
                            merged = true;
                            break;
                        }
                    if (!merged) v.w.push_back(VertexWeight{b, w.weight});
                }
                if (v.w.empty()) v.w.push_back(VertexWeight{0, 1.0f});
                int hit = -1;
                for (std::size_t e = 0; e < s.verts.size(); ++e)
                    if (grabEqual(v, s.verts[e])) {
                        hit = static_cast<int>(e);
                        break;
                    }
                if (hit < 0) {
                    hit = static_cast<int>(s.verts.size());
                    s.verts.push_back(std::move(v));
                }
                tri[static_cast<std::size_t>(k)] = hit;
            }
            s.tris.push_back(tri);
        }
        surfs.push_back(std::move(s));
    }
    if (surfs.empty()) throw std::runtime_error("ASE: keine Objekte fuer die GLM");
    finishMesh(surfs, opt, res, true);
    return res;
}

std::vector<std::string> weightedDeformers(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& e : parseEnvelopes(doc)) {
        if (e.weights.empty()) continue;
        if (std::find(out.begin(), out.end(), e.bone) == out.end()) out.push_back(e.bone);
    }
    return out;
}

MeshImportResult importMeshFile(const std::string& path, const MeshImportOptions& opt) {
    return importMesh(parseFile(path), opt);
}

}  // namespace g2::xsi
