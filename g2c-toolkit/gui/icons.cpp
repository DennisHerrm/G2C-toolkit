#include "gui/icons.h"

#include <string>

namespace g2::gui {
namespace {
bool g_have = false;
}

bool iconsAvailable() { return g_have; }
void setIconsAvailable(bool v) { g_have = v; }

// Symbol und Text zusammensetzen.
//
// Der Puffer ist statisch und wird reihum benutzt: ImGui liest die
// Zeichenkette sofort im selben Aufruf, ein laengeres Leben braucht sie
// nicht. Mehrere Puffer, damit zwei Aufrufe in einer Zeile sich nicht
// gegenseitig ueberschreiben.
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
