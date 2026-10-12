#include "gui/assets.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

// stb_image: TGA, JPG and PNG, and its inflate for the .pk3 archives. Only
// the decoders that are needed, no stdio - everything comes from memory.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4456 4457 4701 4702 4996)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace g2::gui {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::optional<std::vector<std::uint8_t>> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Strips the extension of a game path ("textures/a/b.tga" -> "textures/a/b").
std::string withoutExt(const std::string& p) {
    const std::size_t slash = p.find_last_of('/');
    const std::size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return p;
    return p.substr(0, dot);
}

}  // namespace

std::string gamePathKey(const std::string& p) {
    std::string k = lower(p);
    std::replace(k.begin(), k.end(), '\\', '/');
    while (!k.empty() && k.front() == '/') k.erase(k.begin());
    return k;
}

// --- AssetFs ------------------------------------------------------------------

void AssetFs::addFolder(const std::string& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return;
    folders_.push_back(folder);
    order_.push_back({false, folders_.size() - 1});
    std::vector<std::string> pk3;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower(it->path().extension().string()) == ".pk3") pk3.push_back(it->path().string());
    }
    // The engine loads pk3s in name order and a later one overrides: so the
    // last name is searched first.
    std::sort(pk3.begin(), pk3.end(), [](const std::string& a, const std::string& b) { return lower(a) > lower(b); });
    for (const auto& p : pk3) addPk3(p);
}

void AssetFs::addPk3(const std::string& path) {
    // Only the end of the file and the central directory are read: game
    // archives are hundreds of megabytes, their table of contents is small.
    std::ifstream f(fs::path(path), std::ios::binary | std::ios::ate);
    if (!f) return;
    const auto fileSize = static_cast<std::uint64_t>(f.tellg());
    if (fileSize < 22) return;

    // End of central directory: the last record with its signature, within
    // the 64 KiB a trailing comment may take.
    const std::uint64_t tailLen = std::min<std::uint64_t>(fileSize, 22 + 65535);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tailLen));
    f.seekg(static_cast<std::streamoff>(fileSize - tailLen));
    if (!f.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tailLen))) return;
    std::size_t eocd = std::string::npos;
    for (std::size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (u32(&tail[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return;
    const std::uint16_t count = u16(&tail[eocd + 10]);
    const std::uint32_t dirSize = u32(&tail[eocd + 12]);
    const std::uint32_t dirOffset = u32(&tail[eocd + 16]);
    if (static_cast<std::uint64_t>(dirOffset) + dirSize > fileSize) return;
    std::vector<std::uint8_t> d(dirSize);
    f.seekg(dirOffset);
    if (dirSize && !f.read(reinterpret_cast<char*>(d.data()), dirSize)) return;
    std::size_t pos = 0;

    std::map<std::string, Entry> entries;
    for (std::uint16_t n = 0; n < count; ++n) {
        if (pos + 46 > d.size() || u32(&d[pos]) != 0x02014b50) break;
        Entry e;
        e.archive = archives_.size();
        e.method = u16(&d[pos + 10]);
        e.packedSize = u32(&d[pos + 20]);
        e.size = u32(&d[pos + 24]);
        const std::uint16_t nameLen = u16(&d[pos + 28]);
        const std::uint16_t extraLen = u16(&d[pos + 30]);
        const std::uint16_t commentLen = u16(&d[pos + 32]);
        e.localOffset = u32(&d[pos + 42]);
        if (pos + 46 + nameLen > d.size()) break;
        const std::string name(reinterpret_cast<const char*>(&d[pos + 46]), nameLen);
        if (!name.empty() && name.back() != '/') entries.emplace(gamePathKey(name), e);
        pos += 46u + nameLen + extraLen + commentLen;
    }
    archives_.push_back(path);
    entries_.push_back(std::move(entries));
    order_.push_back({true, archives_.size() - 1});
}

std::optional<std::vector<std::uint8_t>> AssetFs::readEntry(const Entry& e) const {
    std::ifstream f(fs::path(archives_[e.archive]), std::ios::binary);
    if (!f) return std::nullopt;
    std::uint8_t local[30];
    f.seekg(e.localOffset);
    if (!f.read(reinterpret_cast<char*>(local), 30) || u32(local) != 0x04034b50) return std::nullopt;
    f.seekg(static_cast<std::streamoff>(e.localOffset) + 30 + u16(&local[26]) + u16(&local[28]));
    std::vector<std::uint8_t> packed(e.packedSize);
    if (e.packedSize && !f.read(reinterpret_cast<char*>(packed.data()), e.packedSize)) return std::nullopt;
    if (e.method == 0) return packed;
    if (e.method != 8) return std::nullopt;   // only "stored" and "deflate" occur in pk3s
    std::vector<std::uint8_t> out(e.size);
    const int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()),
                                                   reinterpret_cast<const char*>(packed.data()),
                                                   static_cast<int>(packed.size()));
    if (n < 0 || static_cast<std::uint32_t>(n) != e.size) return std::nullopt;
    return out;
}

std::optional<std::vector<std::uint8_t>> AssetFs::read(const std::string& gamePath) const {
    const std::string key = gamePathKey(gamePath);
    if (key.empty()) return std::nullopt;
    for (const auto& src : order_) {
        if (!src.isArchive) {
            // Loose file. Windows ignores case, so the key works as a path.
            const fs::path p = fs::path(folders_[src.index]) / fs::path(key);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return readFile(p);
            continue;
        }
        const auto& m = entries_[src.index];
        const auto it = m.find(key);
        if (it != m.end()) return readEntry(it->second);
    }
    return std::nullopt;
}

bool AssetFs::exists(const std::string& gamePath) const {
    const std::string key = gamePathKey(gamePath);
    if (key.empty()) return false;
    for (const auto& src : order_) {
        if (!src.isArchive) {
            std::error_code ec;
            if (fs::is_regular_file(fs::path(folders_[src.index]) / fs::path(key), ec)) return true;
        } else if (entries_[src.index].count(key)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> AssetFs::list(const std::string& folder, const std::string& ext) const {
    const std::string pre = gamePathKey(folder) + "/";
    const std::string e = lower(ext);
    std::vector<std::string> out;
    const auto take = [&](const std::string& key) {
        if (key.compare(0, pre.size(), pre) == 0 && key.size() > e.size() &&
            key.compare(key.size() - e.size(), e.size(), e) == 0 &&
            std::find(out.begin(), out.end(), key) == out.end())
            out.push_back(key);
    };
    for (const auto& src : order_) {
        if (src.isArchive) {
            for (const auto& [key, entry] : entries_[src.index]) take(key);
        } else {
            std::error_code ec;
            const fs::path dir = fs::path(folders_[src.index]) / fs::path(pre);
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) take(pre + lower(it->path().filename().string()));
        }
    }
    return out;
}

// --- Shaders and skins -------------------------------------------------------------

std::map<std::string, std::string> parseShaders(const std::string& text) {
    // Tokens: words and braces; // and /* */ comments dropped.
    std::vector<std::string> tok;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t e = text.find("*/", i + 2);
            i = e == std::string::npos ? text.size() : e + 2;
        } else if (c == '{' || c == '}') {
            tok.emplace_back(1, c);
            ++i;
        } else {
            std::size_t j = i;
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != '{' &&
                   text[j] != '}')
                ++j;
            tok.push_back(text.substr(i, j - i));
            i = j;
        }
    }

    std::map<std::string, std::string> out;
    const auto usable = [](const std::string& img) { return !img.empty() && img[0] != '$' && img[0] != '*'; };
    for (std::size_t i = 0; i < tok.size();) {
        if (tok[i] == "{" || tok[i] == "}") {
            ++i;
            continue;
        }
        const std::string name = withoutExt(gamePathKey(tok[i]));
        if (i + 1 >= tok.size() || tok[i + 1] != "{") {
            ++i;
            continue;
        }
        // Walk the body, depth-aware.
        std::size_t j = i + 2;
        int depth = 1;
        std::string image, editorImage;
        while (j < tok.size() && depth > 0) {
            const std::string t = lower(tok[j]);
            if (t == "{") ++depth;
            else if (t == "}") --depth;
            else if (depth == 2 && image.empty() && (t == "map" || t == "clampmap") && j + 1 < tok.size()) {
                if (usable(tok[j + 1])) image = tok[j + 1];
            } else if (depth == 2 && image.empty() && t == "animmap" && j + 2 < tok.size()) {
                if (usable(tok[j + 2])) image = tok[j + 2];   // after the frequency: the first frame
            } else if (depth == 1 && t == "qer_editorimage" && j + 1 < tok.size()) {
                editorImage = tok[j + 1];
            }
            ++j;
        }
        if (image.empty()) image = editorImage;
        if (!image.empty() && !out.count(name)) out[name] = gamePathKey(image);
        i = j;
    }
    return out;
}

std::map<std::string, std::string> loadShaderIndex(const AssetFs& fs) {
    std::map<std::string, std::string> out;
    // Searched in priority order: the first definition of a name wins.
    for (const auto& path : fs.list("shaders", ".shader")) {
        const auto data = fs.read(path);
        if (!data) continue;
        for (auto& [name, img] : parseShaders(std::string(data->begin(), data->end())))
            out.emplace(name, img);
    }
    return out;
}

std::map<std::string, std::string> parseSkin(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        const auto trim = [](std::string s) {
            const std::size_t a = s.find_first_not_of(" \t\"");
            const std::size_t b = s.find_last_not_of(" \t\"");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string surf = lower(trim(line.substr(0, comma)));
        const std::string shader = trim(line.substr(comma + 1));
        if (!surf.empty() && surf.rfind("//", 0) != 0) out[surf] = shader;
    }
    return out;
}

std::string resolveImage(const AssetFs& fs, const std::map<std::string, std::string>& shaders,
                         const std::string& shader) {
    if (shader.empty()) return {};
    const std::string key = withoutExt(gamePathKey(shader));
    std::vector<std::string> candidates;
    if (const auto it = shaders.find(key); it != shaders.end()) candidates.push_back(it->second);
    candidates.push_back(gamePathKey(shader));
    for (const auto& c : candidates) {
        if (fs.exists(c)) return c;
        // The engine tries the other formats when the named one is missing.
        const std::string stem = withoutExt(c);
        for (const char* ext : {".tga", ".jpg", ".png"})
            if (fs.exists(stem + ext)) return stem + ext;
    }
    return {};
}

std::optional<Image> decodeImage(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return std::nullopt;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!px) return std::nullopt;
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(px);
    return img;
}

// --- Finding the game -----------------------------------------------------------------

std::vector<std::string> findGameDataFolders() {
    std::vector<fs::path> libraries;
    for (const char* steam : {"C:/Program Files (x86)/Steam", "C:/Program Files/Steam"}) {
        std::error_code ec;
        if (!fs::is_directory(steam, ec)) continue;
        libraries.emplace_back(steam);
        // Further libraries: "path" lines in libraryfolders.vdf.
        std::ifstream vdf(fs::path(steam) / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            const std::size_t k = line.find("\"path\"");
            if (k == std::string::npos) continue;
            const std::size_t a = line.find('"', k + 6);
            const std::size_t b = a == std::string::npos ? a : line.find('"', a + 1);
            if (b == std::string::npos) continue;
            std::string p = line.substr(a + 1, b - a - 1);
            for (std::size_t i = 0; (i = p.find("\\\\", i)) != std::string::npos;) p.erase(i, 1);
            libraries.emplace_back(p);
        }
    }
    std::vector<std::string> out;
    for (const auto& lib : libraries) {
        const fs::path ja = lib / "steamapps" / "common" / "Jedi Academy";
        std::error_code ec;
        for (fs::directory_iterator it(ja, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const std::string name = it->path().filename().string();
            if (lower(name).rfind("gamedata", 0) != 0) continue;
            if (!fs::is_directory(it->path() / "base", ec)) continue;
            const std::string s = it->path().string();
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> gameDataSearchFolders(const std::string& gameData) {
    std::vector<std::string> mods;
    std::error_code ec;
    for (fs::directory_iterator it(gameData, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (lower(it->path().filename().string()) == "base") continue;
        // A mod folder is one with archives in it.
        bool hasPk3 = false;
        std::error_code ec2;
        for (fs::directory_iterator f(it->path(), ec2), fe; !ec2 && f != fe; f.increment(ec2))
            if (lower(f->path().extension().string()) == ".pk3") {
                hasPk3 = true;
                break;
            }
        if (hasPk3) mods.push_back(it->path().string());
    }
    std::sort(mods.begin(), mods.end());
    mods.push_back((fs::path(gameData) / "base").string());
    return mods;
}

std::string baseOfAssetPath(const std::string& file) {
    fs::path dir = fs::path(file).parent_path();
    for (int i = 0; i < 16 && !dir.empty(); ++i) {
        if (lower(dir.filename().string()) == "models") return dir.parent_path().string();
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return {};
}

}  // namespace g2::gui
