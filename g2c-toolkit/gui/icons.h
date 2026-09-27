// gui/icons.h - Icons from Windows' built-in icon font.
//
// No bundled file needed: "Segoe MDL2 Assets" has shipped with Windows since
// Windows 10, "Segoe Fluent Icons" since Windows 11. The glyphs live in the
// Unicode Private Use Area (PUA) from E700 up and are merged via MergeMode
// into the same font atlas as the text font and CJK.
//
// The catch, which Microsoft itself points out: PUA characters are not
// standardized. If the font is missing, the icons are missing. That is why
// the window backend checks whether it could be loaded, and otherwise leaves
// the icons out instead of showing empty boxes.

#pragma once

namespace g2::gui {

// Range contributed by the icon font.
constexpr unsigned kIconRangeMin = 0xE700;
constexpr unsigned kIconRangeMax = 0xF8FF;

// The slice we actually use.
//
// The twenty icons below all lie between E70F and EA39. Requesting the whole
// range would mean building a lookup table for 4608 characters in order to
// draw twenty.
//
// Anyone adding an icon outside it must widen these bounds -
// tools/check_win32.py checks that.
// Colors for icons.
//
// ImGui draws text in a single color, and therefore the icons too. An icon
// on its own - in a status column or in front of a message - can be tinted,
// though, and that helps comprehension: green means done, red means error,
// without having to read the text.
//
// Deliberately muted tones. A UI in which everything glows is more tiring
// than a gray one - and color loses its meaning when it is everywhere.
struct IconColor {
    float r, g, b, a;
};

inline constexpr IconColor kIconGood{0.42f, 0.83f, 0.52f, 1.0f};    // green
inline constexpr IconColor kIconWarn{0.95f, 0.75f, 0.35f, 1.0f};    // yellow
inline constexpr IconColor kIconBad{0.92f, 0.45f, 0.45f, 1.0f};     // red
inline constexpr IconColor kIconInfo{0.48f, 0.68f, 0.95f, 1.0f};    // blue
inline constexpr IconColor kIconMuted{0.55f, 0.55f, 0.58f, 1.0f};   // gray
inline constexpr IconColor kIconAccent{0.72f, 0.62f, 0.95f, 1.0f};  // violet

constexpr unsigned kIconUsedMin = 0xE70F;
constexpr unsigned kIconUsedMax = 0xEA39;

constexpr const char* ICON_FOLDER      = "\xEE\xA2\xB7";   // U+E8B7  folder
constexpr const char* ICON_FOLDER_OPEN = "\xEE\xA3\x9A";   // U+E8DA  open folder
constexpr const char* ICON_OPEN_FILE   = "\xEE\xA3\xA5";   // U+E8E5  open file
constexpr const char* ICON_SAVE        = "\xEE\x9D\x8E";   // U+E74E  save
constexpr const char* ICON_SAVE_ALL    = "\xEE\x9E\x8C";   // U+E78C  save all
constexpr const char* ICON_BUILD       = "\xEE\x9D\xA8";   // U+E768  build (play)
constexpr const char* ICON_CHECK       = "\xEE\x9C\xBE";   // U+E73E  check mark
constexpr const char* ICON_VALIDATE    = "\xEE\xA7\x99";   // U+E9D9  validate
constexpr const char* ICON_ADD         = "\xEE\x9C\x90";   // U+E710  add
constexpr const char* ICON_CANCEL      = "\xEE\x9C\x91";   // U+E711  cancel
constexpr const char* ICON_SETTINGS    = "\xEE\x9C\x93";   // U+E713  settings
constexpr const char* ICON_GLOBE       = "\xEE\x9D\xB4";   // U+E774  language
constexpr const char* ICON_DELETE      = "\xEE\x9D\x8D";   // U+E74D  delete
constexpr const char* ICON_EDIT        = "\xEE\x9C\x8F";   // U+E70F  edit
constexpr const char* ICON_DOCUMENT    = "\xEE\xA2\xA5";   // U+E8A5  document
constexpr const char* ICON_REFRESH     = "\xEE\x9C\xAC";   // U+E72C  reload
constexpr const char* ICON_FILTER      = "\xEE\x9C\x9C";   // U+E71C  filter
constexpr const char* ICON_WARNING     = "\xEE\x9E\xBA";   // U+E7BA  warning
constexpr const char* ICON_ERROR       = "\xEE\xA8\xB9";   // U+EA39  error
constexpr const char* ICON_INFO        = "\xEE\xA5\x86";   // U+E946  info

// Icons can be switched off in case the font is missing.
bool iconsAvailable();
void setIconsAvailable(bool v);

// Icon + text, or just text if no icons are available.
const char* withIcon(const char* icon, const char* text);

}  // namespace g2::gui
