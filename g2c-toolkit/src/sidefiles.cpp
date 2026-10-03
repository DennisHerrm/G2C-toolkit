#include "g2/sidefiles.h"

#include "g2/mdxa.h"
#include "g2/mdxm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>

namespace g2 {

std::string writeSkin(const Mesh& mesh) {
    std::ostringstream os;
    if (mesh.lods.empty()) return os.str();

    for (const Surface& s : mesh.lods.front().surfaces) {
        // Tags carry no texture - they are never rendered.
        if (s.flags & fmt::kSurfFlagIsBolt) continue;
        // "[nomaterial]" is Carcass' placeholder for "no shader" and does
        // not appear in Raven's .skin files.
        if (s.shader.empty() || s.shader == "[nomaterial]") continue;
        os << s.name << "," << s.shader << "\r\n";
    }
    return os.str();
}

std::string writeFrames(const std::vector<FrameEntry>& entries) {
    std::ostringstream os;
    char buf[64];

    for (const FrameEntry& e : entries) {
        os << "\r\n" << e.sourcePath << "\r\n{\r\n";
        os << "\t\"startframe\"\t\"" << e.startFrame << "\"\r\n";
        os << "\t\"duration\"\t\"" << e.duration << "\"\r\n";
        os << "\t\"fps\"\t\"" << e.fps << "\"\r\n";
        if (!e.deltaVecs.empty()) {
            // Carcass's layout, including the line with just a tab.
            os << "\t\r\n\tdeltavecs\r\n\t{\r\n";
            for (std::size_t i = 0; i < e.deltaVecs.size(); ++i) {
                std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", static_cast<double>(e.deltaVecs[i][0]),
                              static_cast<double>(e.deltaVecs[i][1]), static_cast<double>(e.deltaVecs[i][2]));
                os << "\t\t\"delta" << i << "\"\t\"" << buf << "\"\r\n";
            }
            os << "\t}\r\n";
        } else {
            // Three decimal places, as in the original.
            std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", static_cast<double>(e.averageVec[0]),
                          static_cast<double>(e.averageVec[1]), static_cast<double>(e.averageVec[2]));
            os << "\t\"averagevec\"\t\"" << buf << "\"\r\n";
        }
        os << "}\r\n";
    }
    return os.str();
}

namespace {

std::int32_t le32(const std::vector<std::uint8_t>& d, std::size_t o) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(d[o]) |
                                     (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                                     (static_cast<std::uint32_t>(d[o + 2]) << 16) |
                                     (static_cast<std::uint32_t>(d[o + 3]) << 24));
}

// "%-20s%d" as Carcass prints its header lines: the label padded to column 21.
void field(std::ostringstream& os, const char* label, long long v, const char* tail = "") {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%-21s%lld%s\r\n", label, v, tail);
    os << buf;
}
void fieldS(std::ostringstream& os, const char* label, const std::string& v) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-21s%s\r\n", label, v.c_str());
    os << buf;
}

}  // namespace

std::string writeInfoText(const InfoInput& in) {
    const MdxmFile f = readMdxm(in.glm);
    const Mesh& m = f.mesh;
    std::ostringstream os;
    char buf[256];

    MdxaFile gla;
    const bool haveGla = !in.gla.empty();
    if (haveGla) gla = readMdxa(in.gla);
    // The "_always_" bones of the source. In the GLA they carry the
    // ALWAYSXFORM flag and lose the suffix ("face_always_" -> "face"); the
    // Motion bone carries the flag too but was never an _always_ bone.
    // Carcass lists them by their source name, so the suffix comes back here.
    const auto isAlways = [](const Bone& b) {
        if (!(b.flags & fmt::kBoneFlagAlwaysXform)) return false;
        std::string n = b.name;
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return n != "motion";
    };
    const auto shownName = [&](const Bone& b) { return isAlways(b) ? b.name + "_always_" : b.name; };
    int always = 0;
    if (haveGla)
        for (const auto& b : gla.skeleton.bones)
            if (isAlways(b)) ++always;

    os << "Information file for model '" << m.name << "'\r\n\r\n\r\n";
    os << "GLM Header info:\r\n\r\n";
    field(os, "->ident:", le32(in.glm, 0));
    field(os, "->version:", le32(in.glm, 4));
    fieldS(os, "->name:", m.name);
    fieldS(os, "->animName:", m.animName);
    std::snprintf(buf, sizeof(buf), "->animIndex:         ( only used by game, but defaulted to %d )\r\n",
                  static_cast<int>(f.animIndex));
    os << buf;
    if (haveGla && always > 0) {
        std::snprintf(buf, sizeof(buf), "  ( includes %d 'ALWAYS' bone(s) )", always);
        field(os, "->numBones:", m.numBones, buf);
    } else {
        field(os, "->numBones:", m.numBones);
    }
    field(os, "->numLODs:", le32(in.glm, 144));
    field(os, "->ofsLODs:", le32(in.glm, 148));
    field(os, "->numSurfaces:", le32(in.glm, 152));
    field(os, "->ofsSurfHierarchy:", le32(in.glm, 156));
    field(os, "->ofsEnd:", le32(in.glm, 160), "  ( also gives overall filesize )");
    os << "\r\n\r\n";

    // Shaders: every distinct one of the NON-tag surfaces, sorted by bytes.
    std::set<std::string> shaders;
    const std::size_t ns = m.lods.empty() ? 0 : m.lods[0].surfaces.size();
    for (std::size_t i = 0; i < ns; ++i) {
        if (m.lods[0].surfaces[i].flags & fmt::kSurfFlagIsBolt) continue;
        shaders.insert(i < in.shaders.size() ? in.shaders[i] : m.lods[0].surfaces[i].shader);
    }
    os << "Shader Info:\r\n\r\n" << shaders.size() << " unique shaders used, list follows:\r\n";
    for (const auto& sh : shaders) os << sh << "\r\n";
    os << "\r\n";

    std::size_t off = 0, tags = 0;
    for (std::size_t i = 0; i < ns; ++i) {
        const auto fl = m.lods[0].surfaces[i].flags;
        if (fl & fmt::kSurfFlagOff) ++off;
        else if (fl & fmt::kSurfFlagIsBolt) ++tags;
    }
    // Carcass counts tags among all surfaces (a tag is never OFF in practice).
    tags = 0;
    for (std::size_t i = 0; i < ns; ++i)
        if (m.lods[0].surfaces[i].flags & fmt::kSurfFlagIsBolt) ++tags;
    os << "Rendering Info:\r\n\r\n";
    os << off << " surfaces out of " << ns << " default to OFF\r\n";
    os << tags << " surfaces out of " << ns << " are tags only\r\n";
    os << "...therefore default rendered surface total = " << (ns - off - tags) << "\r\n\r\n";

    for (std::size_t l = 0; l < m.lods.size(); ++l) {
        std::size_t verts = 0, tris = 0, rverts = 0, rtris = 0;
        std::set<int> refs, rrefs;
        for (std::size_t si = 0; si < m.lods[l].surfaces.size(); ++si) {
            const Surface& s = m.lods[l].surfaces[si];
            verts += s.vertices.size();
            tris += s.triangles.size();
            const bool rendered = !(s.flags & (fmt::kSurfFlagOff | fmt::kSurfFlagIsBolt));
            const auto& table = f.boneRefs[l][si];
            for (const auto r : table) refs.insert(r);
            if (rendered) {
                rverts += s.vertices.size();
                rtris += s.triangles.size();
            }
            // Carcass counts the bone references of the tags in (not of _off).
            if (!(s.flags & fmt::kSurfFlagOff))
                for (const auto r : table) rrefs.insert(r);
        }
        os << "LOD " << l << "/" << m.lods.size() << ":\r\n{\r\n";
        std::snprintf(buf, sizeof(buf),
                      "Total Verts:                 %zu\r\nTotal Tris :                 %zu\r\n"
                      "Unique Bone Refs:            %zu\r\n\r\n"
                      "Default rendered Verts:      %zu\r\nDefault rendered Tris:       %zu\r\n"
                      "Default rendered Bone Refs:  %zu\r\n",
                      verts, tris, refs.size(), rverts, rtris, rrefs.size());
        os << buf;
        if (l < in.deletedDupVerts.size() && in.deletedDupVerts[l] > 0) {
            std::snprintf(buf, sizeof(buf),
                          "Deleted duplicate verts:     %zu  ( with %zu total bone-weights )\r\n",
                          in.deletedDupVerts[l],
                          l < in.deletedDupWeights.size() ? in.deletedDupWeights[l] : std::size_t{0});
            os << buf;
        }
        os << "}\r\n\r\n";
    }
    os << "\r\n\r\n";

    if (haveGla) {
        const std::vector<std::uint8_t>& g = in.gla;
        os << "GLA Header info:\r\n\r\n";
        field(os, "->ident:", le32(g, 0));
        field(os, "->version:", le32(g, 4));
        fieldS(os, "->name:", gla.skeleton.name);
        field(os, "->numFrames:", le32(g, 76));
        field(os, "->ofsFrames:", le32(g, 80));
        field(os, "->numBones:", le32(g, 84));
        field(os, "->ofsCompBonePool:", le32(g, 88));
        field(os, "->ofsSkel:", le32(g, 92));
        field(os, "->ofsEnd:", le32(g, 96), "  ( also gives overall filesize )");
        os << "\r\n\r\n";

        const long long nb = static_cast<long long>(gla.skeleton.bones.size());
        const long long nf = gla.numFrames;
        const long long total = nb * nf;
        const long long unique = static_cast<long long>(gla.bonePool.size());
        const long long unpooled = total * 14;
        const long long pooled = unique * 14;
        // Carcass prints the mesh's bone-reference count here and calls it
        // sizeof(int) - meaningless for the GLA. These are the real numbers:
        // one 3-byte pool index per bone and frame.
        const long long indexBytes = total * 3;
        std::snprintf(buf, sizeof(buf),
                      "Total  animation matrices:                     %lld   ( %lld bones * %lld frames )\r\n"
                      "Unique animation matrices:                     %lld\r\n"
                      "Total bytes for unpooled animation matrices:   %lld\r\n"
                      "Total bytes for   pooled animation matrices:   %lld\r\n",
                      total, nb, nf, unique, unpooled, pooled);
        os << buf;
        std::snprintf(buf, sizeof(buf),
                      "Index bytes for pooling:                       %lld   ( %lld total bone references * 3 bytes )\r\n"
                      "Bytes saved via pooling:                       %lld\r\n\r\n\r\n",
                      indexBytes, total, unpooled - pooled - indexBytes);
        os << buf;

        os << "Complete list of all " << nb << " bones:\r\n\r\n";
        const auto kids = gla.skeleton.buildChildLists();
        for (std::size_t b = 0; b < gla.skeleton.bones.size(); ++b) {
            const Bone& bone = gla.skeleton.bones[b];
            std::snprintf(buf, sizeof(buf), "Bone %3zu:   \"%s\":\r\n", b, shownName(bone).c_str());
            os << buf;
            const std::string parent =
                bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < gla.skeleton.bones.size()
                    ? shownName(gla.skeleton.bones[static_cast<std::size_t>(bone.parent)])
                    : std::string();
            os << "            Parent: \"" << parent << "\"  (index " << bone.parent << ")\r\n";
            os << "            #Kids:  " << kids[b].size() << "\r\n";
            for (std::size_t k = 0; k < kids[b].size(); ++k)
                os << "            Child " << k << ": (index " << kids[b][k] << "), name \""
                   << shownName(gla.skeleton.bones[static_cast<std::size_t>(kids[b][k])]) << "\"\r\n";
            os << "\r\n";
        }
    } else if (!in.usesGla.empty()) {
        os << "( Skeleton file not written out by this model, uses file '" << in.usesGla << "' )\r\n";
    }
    return os.str();
}

}  // namespace g2
