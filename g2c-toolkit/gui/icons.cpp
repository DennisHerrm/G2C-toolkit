#include "gui/icons.h"

#include <string>

namespace g2::gui {
namespace {
bool g_have = false;
}

bool iconsAvailable() { return g_have; }
void setIconsAvailable(bool v) { g_have = v; }

// Combines icon and text.
//
// The buffer is static and used in rotation: ImGui reads the string right
// away within the same call, it doesn't need to live any longer. Several
// buffers, so that two calls on one line don't overwrite each other.
const char* withIcon(const char* icon, const char* text) {
    static std::string bufs[8];
    static int next = 0;
    std::string& b = bufs[next];
    next = (next + 1) % 8;

    if (!g_have || !icon) {
        b = text ? text : "";
    } else {
        b = std::string(icon) + "  " + (text ? text : "");
    }
    return b.c_str();
}

}  // namespace g2::gui
