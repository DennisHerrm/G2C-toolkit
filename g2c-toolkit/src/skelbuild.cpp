#include "g2/skelbuild.h"

#include "g2/mdxa.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>

namespace g2 {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Identity of a bone across files and against a reference: its GLA name,
// case-insensitive. Carcass compared the raw names case-sensitively, so a
// second file spelling "LHAND" created a second bone and the fingers ended up
// under lradiusX without a word (SPEC B4); "face" in a reference never matched
// "face_always_" in the sources (B2).
std::string keyOf(const std::string& name) { return lower(xsi::glaBoneName(name)); }

struct Work {
    std::string name;        // first spelling seen (source name, may have _always_)
    std::string parentKey;   // "" = root
    Mat3x4      base = Mat3x4::identity();
    bool        hasBase = false;
    bool        fromRef = false;
    bool        always = false;
    bool        used = false;
};

}  // namespace

SkeletonBuildResult buildSkeleton(const std::vector<const xsi::AnimFile*>& anims,
                                  const std::vector<std::string>& weighted,
                                  const SkeletonBuildOptions& opt) {
    SkeletonBuildResult res;
    if (anims.empty()) throw std::runtime_error("Neues Skelett: keine Animationsdatei ($aseanimgrab)");

    float scale = opt.scale > 0.0f ? opt.scale : 1.0f;
    if (opt.reference && opt.reference->scale > 0.0f) scale = opt.reference->scale;


    std::vector<Work> bones;
    std::map<std::string, std::size_t> index;   // key -> position
    std::map<std::string, std::string> parentSeen;   // key -> first parent key, for the conflict warning
    std::set<std::string> parentConflicts, spellingClashes, baseDeviations;

    const auto addRef = [&] {
        const Skeleton& r = *opt.reference;
        for (const auto& b : r.bones) {
            const std::string k = keyOf(b.name);
            auto it = index.find(k);
            if (it == index.end()) {
                it = index.emplace(k, bones.size()).first;
                Work w;
                w.name = b.name;
                bones.push_back(w);
            }
            Work& w = bones[it->second];
            w.fromRef = true;
            w.parentKey = b.parent >= 0 ? keyOf(r.bones[static_cast<std::size_t>(b.parent)].name) : "";
            if (b.flags & fmt::kBoneFlagAlwaysXform) w.always = w.always || lower(b.name) != "motion";
        }
    };

    for (std::size_t fi = 0; fi <= anims.size(); ++fi) {
        if (opt.reference && fi == std::min(opt.refBeforeGrab, anims.size())) addRef();
        if (fi == anims.size()) break;
        const xsi::AnimFile& a = *anims[fi];
        std::set<std::string> pcj;
        for (std::size_t p = 0; p < opt.pcj.size(); ++p)
            if (p >= opt.pcjFrom.size() || opt.pcjFrom[p] <= fi) pcj.insert(lower(opt.pcj[p]));
        const std::string file = a.sourcePath.empty() ? "Datei " + std::to_string(fi + 1) : a.sourcePath;

        // Names within one file must be unique - which node would be meant?
        std::map<std::string, std::size_t> inFile;
        for (std::size_t n = 0; n < a.nodes.size(); ++n) {
            const std::string k = keyOf(a.nodes[n].name);
            if (!inFile.emplace(k, n).second)
                throw std::runtime_error("Bone \"" + a.nodes[n].name + "\" kommt in \"" + file +
                                         "\" mehrfach vor - welcher gemeint ist, ist nicht zu entscheiden");
        }

        // Rest pose for nodes without BASEPOSE: the SRT chain of this file
        // (Carcass took the LOCAL SRT as the absolute pose, B3).
        std::vector<Mat3x4> srtWorld(a.nodes.size());
        for (std::size_t n = 0; n < a.nodes.size(); ++n) {
            const Mat3x4 l = xsi::srtMatrix(a.nodes[n].srt);
            const int p = a.nodes[n].parent;
            srtWorld[n] = p < 0 ? l : mul(srtWorld[static_cast<std::size_t>(p)], l);
        }

        std::set<std::string> seen;
        for (std::size_t n = 0; n < a.nodes.size(); ++n) {
            const xsi::AnimNode& node = a.nodes[n];
            const std::string k = keyOf(node.name);
            auto it = index.find(k);
            const bool isNew = it == index.end();
            if (isNew) {
                it = index.emplace(k, bones.size()).first;
                Work w;
                w.name = node.name;
                bones.push_back(w);
            }
            Work& w = bones[it->second];
            if (!isNew && w.name != node.name && xsi::glaBoneName(w.name) != xsi::glaBoneName(node.name))
                spellingClashes.insert(w.name + " / " + node.name);
            if (xsi::isAlwaysName(node.name)) w.always = true;

            // Parent: the XSI parent, or with $pcj the nearest ancestor in the
            // PCJ list, falling back to the topmost node (Carcass 0x447e80).
            std::string parent;
            if (node.parent >= 0) {
                int cur = node.parent;
                if (!pcj.empty()) {
                    while (true) {
                        const auto& cn = a.nodes[static_cast<std::size_t>(cur)];
                        if (pcj.count(lower(cn.name))) break;
                        if (cn.parent < 0) break;
                        cur = cn.parent;
                    }
                }
                parent = keyOf(a.nodes[static_cast<std::size_t>(cur)].name);
            }
            // A reference line before the grabs dictates the hierarchy.
            const bool refRules = w.fromRef && opt.reference && opt.refBeforeGrab == 0;
            if (!refRules) {
                const auto ps = parentSeen.emplace(k, parent);
                if (!ps.second && ps.first->second != parent) parentConflicts.insert(xsi::glaBoneName(node.name));
                w.parentKey = parent;   // the last file wins, as in Carcass
            }

            const Mat3x4 pose = xsi::toGhoul2Space(node.hasBasePose ? xsi::srtMatrix(node.basePose) : srtWorld[n], scale);
            if (w.hasBase) {
                float worst = 0.0f;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(w.base.m[r][c] - pose.m[r][c]));
                if (worst > 0.01f) {
                    std::ostringstream os;
                    os << xsi::glaBoneName(node.name) << " (" << worst << " in " << file << ")";
                    baseDeviations.insert(os.str());
                }
            }
            w.base = pose;   // the last file wins
            w.hasBase = true;
            seen.insert(k);
        }
        if (fi > 0 && !bones.empty() && !seen.count(keyOf(bones[0].name)))
            throw std::runtime_error("\"" + file + "\" hat den Wurzel-Bone \"" + bones[0].name +
                                     "\" nicht - die Datei passt nicht zu diesem Skelett");
    }

    // Which bones the mesh uses.
    for (const auto& wname : weighted) {
        const auto it = index.find(keyOf(wname));
        if (it == index.end())
            throw std::runtime_error("Bone \"" + wname + "\" traegt Gewichte im Mesh, kommt aber in keiner "
                                     "Animationsdatei vor - ohne ihn laesst sich das Skelett nicht bauen");
        bones[it->second].used = true;
    }
    for (const auto& b : bones)
        if (!opt.mdr && lower(xsi::glaBoneName(b.name)) == "motion" && b.used)
            throw std::runtime_error("Der Bone \"" + b.name + "\" (Wurzelbewegung) wird vom Mesh benutzt - "
                                     "das geht nicht, er wird beim Bauen herausgerechnet");

    const auto keep = [&](const Work& b, std::size_t i) {
        if (i == 0 || b.fromRef || b.used || b.always) return true;
        return opt.keepMotion && lower(xsi::glaBoneName(b.name)) == "motion";
    };

    // $bonehiercap: cut off everything below the cap bones (Carcass 0x437660),
    // except "_always_" bones. Before the unused bones go, so the cut bones'
    // weights no longer keep them alive.
    if (!opt.caps.empty()) {
        std::map<std::string, std::string> parentOf;
        for (const auto& b : bones) parentOf[keyOf(b.name)] = b.parentKey;
        const auto below = [&](const std::string& k, const std::string& cap) {
            std::string p = parentOf[k];
            for (int guard = 0; !p.empty() && guard < 100000; ++guard) {
                if (p == cap) return true;
                p = parentOf[p];
            }
            return false;
        };
        std::set<std::string> capKeys;
        for (const auto& c : opt.caps) capKeys.insert(keyOf(c));
        for (const auto& ck : capKeys) {
            if (!index.count(ck)) {
                res.warnings.push_back("$bonehiercap: Bone \"" + ck + "\" gibt es nicht");
                continue;
            }
            std::vector<std::string> removed;
            for (std::size_t i = 0; i < bones.size(); ++i) {
                const Work& b = bones[i];
                if (below(keyOf(b.name), ck) && !b.always && keep(b, i))
                    removed.push_back(xsi::glaBoneName(b.name));
            }
            if (removed.empty()) continue;
            res.capped.push_back({xsi::glaBoneName(bones[index[ck]].name), removed});
        }
        std::set<std::string> gone;
        for (const auto& [cap, list] : res.capped)
            for (const auto& n : list) gone.insert(lower(n));
        for (std::size_t i = bones.size(); i-- > 1;) {
            if (!gone.count(keyOf(bones[i].name))) continue;
            const std::string k = keyOf(bones[i].name);
            for (auto& o : bones)
                if (o.parentKey == k) o.parentKey = bones[i].parentKey;
            bones.erase(bones.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // Remove what nothing needs; its children move up to its parent.
    for (std::size_t i = bones.size(); i-- > 1;) {
        if (keep(bones[i], i)) continue;
        const std::string gone = keyOf(bones[i].name);
        const std::string up = bones[i].parentKey;
        for (auto& o : bones)
            if (o.parentKey == gone) o.parentKey = up;
        bones.erase(bones.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // Into the GLA skeleton.
    Skeleton& sk = res.skeleton;
    sk.name = opt.name;
    sk.scale = scale;
    std::map<std::string, int> pos;
    for (std::size_t i = 0; i < bones.size(); ++i) pos[keyOf(bones[i].name)] = static_cast<int>(i);
    std::set<std::string> glaNames;
    for (const auto& b : bones) {
        Bone o;
        o.name = xsi::glaBoneName(b.name);
        if (!glaNames.insert(lower(o.name)).second)
            throw std::runtime_error("Zwei Bones heissen in der GLA \"" + o.name + "\" (nach dem Entfernen von "
                                     "\"_always_\")");
        if (b.always) o.flags |= fmt::kBoneFlagAlwaysXform;
        if (opt.keepMotion && lower(o.name) == "motion") o.flags |= fmt::kBoneFlagAlwaysXform;
        o.parent = -1;
        if (!b.parentKey.empty()) {
            const auto it = pos.find(b.parentKey);
            if (it == pos.end())
                throw std::runtime_error("Eltern-Bone von \"" + o.name + "\" nicht gefunden");
            o.parent = it->second;
        }
        if (!b.hasBase && !b.fromRef)
            res.warnings.push_back("Bone \"" + o.name + "\" ohne Bindepose - Einheitsmatrix");
        o.basePose = b.base;
        sk.bones.push_back(std::move(o));
    }

    // A reference bone the sources never had needs a base pose from it.
    if (opt.reference)
        for (std::size_t i = 0; i < bones.size(); ++i)
            if (!bones[i].hasBase)
                for (const auto& rb : opt.reference->bones)
                    if (keyOf(rb.name) == keyOf(bones[i].name)) sk.bones[i].basePose = rb.basePose;

    // Everything must hang below bone 0.
    {
        const auto kids = sk.buildChildLists();
        std::vector<int> stack{0};
        std::vector<bool> reached(sk.bones.size(), false);
        while (!stack.empty()) {
            const int x = stack.back();
            stack.pop_back();
            if (reached[static_cast<std::size_t>(x)]) continue;
            reached[static_cast<std::size_t>(x)] = true;
            for (int k : kids[static_cast<std::size_t>(x)]) stack.push_back(k);
        }
        std::string lost;
        for (std::size_t i = 0; i < reached.size(); ++i)
            if (!reached[i]) lost += (lost.empty() ? "" : ", ") + sk.bones[i].name;
        if (!lost.empty())
            throw std::runtime_error("Diese Bones haengen nicht unter \"" + sk.bones[0].name +
                                     "\" (zweite Wurzel in den Dateien?): " + lost);
    }

    // An always-transformed bone whose parent no surface uses: Carcass
    // aborted here (and named the GLA instead of the bone). A warning.
    for (std::size_t i = 0; i < sk.bones.size(); ++i) {
        const Bone& b = sk.bones[i];
        if (!(b.flags & fmt::kBoneFlagAlwaysXform) || b.parent < 0) continue;
        if (!bones[static_cast<std::size_t>(b.parent)].used && b.parent != 0)
            res.warnings.push_back("\"" + b.name + "\" wird immer transformiert, haengt aber an \"" +
                                   sk.bones[static_cast<std::size_t>(b.parent)].name +
                                   "\", das kein Mesh-Teil benutzt");
    }

    // The reference's order: a GLM made for it must keep working.
    if (opt.reference && !keepBoneOrder(sk, *opt.reference)) {
        std::string extra, missing;
        std::set<std::string> have, ref;
        for (const auto& b : sk.bones) have.insert(lower(b.name));
        for (const auto& b : opt.reference->bones) ref.insert(lower(xsi::glaBoneName(b.name)));
        for (const auto& n : have)
            if (!ref.count(n)) extra += (extra.empty() ? "" : ", ") + n;
        for (const auto& n : ref)
            if (!have.count(n)) missing += (missing.empty() ? "" : ", ") + n;
        throw std::runtime_error("Das Skelett passt nicht zur Referenz ($aseanimref_gla)" +
                                 (extra.empty() ? std::string() : " - zusaetzlich: " + extra) +
                                 (missing.empty() ? std::string() : " - fehlt: " + missing));
    }

    for (const auto& c : spellingClashes)
        res.warnings.push_back("Bone in verschiedener Schreibweise, als derselbe behandelt: " + c);
    for (const auto& c : parentConflicts)
        res.warnings.push_back("Bone \"" + c + "\" haengt in den Dateien an verschiedenen Eltern - "
                               "die letzte Datei gilt (wie bei Carcass)");
    if (!baseDeviations.empty()) {
        std::string list;
        std::size_t n = 0;
        for (const auto& d : baseDeviations) {
            if (n++ == 6) { list += ", ..."; break; }
            list += (list.empty() ? "" : ", ") + d;
        }
        res.warnings.push_back(std::to_string(baseDeviations.size()) +
                               " Bindepose(n) weichen zwischen den Dateien um mehr als 0.01 ab, die letzte "
                               "Datei gilt (Carcass brach hier ohne -ignorebasedeviations ab): " + list);
    }
    return res;
}

std::string writeBoneCap(const std::vector<std::pair<std::string, std::vector<std::string>>>& capped) {
    auto sorted = capped;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
    std::string out;
    for (const auto& [cap, list] : sorted) {
        out += cap + " ";
        for (const auto& n : list) out += n + " ";
        out += "\r\n";
    }
    return out;
}

std::vector<std::string> readBoneCapCaps(const std::string& text) {
    std::vector<std::string> caps;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string first;
        if (ls >> first) caps.push_back(first);
    }
    return caps;
}

bool keepBoneOrder(Skeleton& built, const Skeleton& existing) {
    if (built.bones.size() != existing.bones.size()) return false;
    std::map<std::string, int> at;
    for (std::size_t i = 0; i < built.bones.size(); ++i) at[lower(built.bones[i].name)] = static_cast<int>(i);
    std::vector<int> order;   // new index -> old (built) index
    for (const auto& b : existing.bones) {
        const auto it = at.find(lower(xsi::glaBoneName(b.name)));
        if (it == at.end()) return false;
        order.push_back(it->second);
    }
    std::vector<int> newOf(built.bones.size());
    for (std::size_t n = 0; n < order.size(); ++n) newOf[static_cast<std::size_t>(order[n])] = static_cast<int>(n);
    std::vector<Bone> out;
    for (const int o : order) {
        Bone b = built.bones[static_cast<std::size_t>(o)];
        if (b.parent >= 0) b.parent = newOf[static_cast<std::size_t>(b.parent)];
        out.push_back(std::move(b));
    }
    built.bones = std::move(out);
    return true;
}

}  // namespace g2
