#include "g2/sidefiles.h"

#include <cstdio>
#include <sstream>

namespace g2 {

std::string writeSkin(const Mesh& mesh) {
    std::ostringstream os;
    if (mesh.lods.empty()) return os.str();

    for (const Surface& s : mesh.lods.front().surfaces) {
        // Tags tragen keine Textur — sie werden nie gerendert.
        if (s.flags & fmt::kSurfFlagIsBolt) continue;
        // "[nomaterial]" ist Carcass' Platzhalter fuer "kein Shader" und
        // steht in Ravens .skin-Dateien nicht drin.
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
        // Drei Nachkommastellen, wie im Original.
        std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", static_cast<double>(e.averageVec[0]),
                      static_cast<double>(e.averageVec[1]), static_cast<double>(e.averageVec[2]));
        os << "\t\"averagevec\"\t\"" << buf << "\"\r\n";
        os << "}\r\n";
    }
    return os.str();
}

}  // namespace g2
