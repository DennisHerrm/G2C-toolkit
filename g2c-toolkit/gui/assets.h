// gui/assets.h - Game files for the preview: loose files, .pk3 archives,
// shaders, skins and images.
//
// The preview textures a model the way the game does: the skin says which
// shader a surface gets, the shader says which image it draws, and the image
// lives either as a loose file under a base folder or inside a .pk3. None of
// this needs a window - it is plain file work and is tested as such; only
// putting the pixels on the graphics card is the window's job.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace g2::gui {

// The files of one or more game folders, as one tree.
//
// Folders are searched in the order they were added - the first one wins.
// Within a folder its loose files come first (that is where a modder's work
// in progress lives), then its .pk3 archives, the alphabetically last first
// (as in the engine, where a later pk3 overrides an earlier one).
class AssetFs {
public:
    void addFolder(const std::string& folder);
    // A single archive, e.g. for tests.
    void addPk3(const std::string& path);

    // Game path such as "models/players/kyle/model_default.skin". Case and
    // slash direction do not matter.
    std::optional<std::vector<std::uint8_t>> read(const std::string& gamePath) const;
    bool exists(const std::string& gamePath) const;

    // Game paths under a folder with an extension, e.g. ("shaders", ".shader").
    std::vector<std::string> list(const std::string& folder, const std::string& ext) const;

    std::size_t archiveCount() const { return archives_.size(); }
    bool empty() const { return folders_.empty() && archives_.empty(); }

private:
    struct Entry {
        std::size_t   archive = 0;
        std::uint32_t localOffset = 0;
        std::uint32_t packedSize = 0;
        std::uint32_t size = 0;
        std::uint16_t method = 0;
    };
    struct Source {
        bool        isArchive = false;
        std::size_t index = 0;   // into folders_ or archives_
    };
    std::vector<std::string> folders_;
    std::vector<std::string> archives_;
    // Search order over both.
    std::vector<Source> order_;
    // Per archive: lower-case game path -> entry.
    std::vector<std::map<std::string, Entry>> entries_;

    std::optional<std::vector<std::uint8_t>> readEntry(const Entry& e) const;
};

// Normalizes a game path: lower case, forward slashes, no leading slash.
std::string gamePathKey(const std::string& p);

// Shader name -> the first image it draws ("map", "clampmap", the first frame
// of "animmap"; lightmaps and generated images skipped). Keys are lower case
// and without extension.
std::map<std::string, std::string> parseShaders(const std::string& text);
std::map<std::string, std::string> loadShaderIndex(const AssetFs& fs);

// A .skin file: lower-case surface name -> shader. "*off" hides the surface.
std::map<std::string, std::string> parseSkin(const std::string& text);

// The image file for a shader: via the shader index if it has one, otherwise
// the shader name itself is the image path. Tries the extensions the engine
// tries. Empty if nothing exists.
std::string resolveImage(const AssetFs& fs, const std::map<std::string, std::string>& shaders,
                         const std::string& shader);

// Decodes TGA, JPG or PNG to RGBA.
struct Image {
    int                       width = 0;
    int                       height = 0;
    std::vector<std::uint8_t> rgba;
};
std::optional<Image> decodeImage(const std::vector<std::uint8_t>& bytes);

// Game data folders of installed copies of Jedi Academy (Steam): every
// "GameData*" folder under "Jedi Academy" in every Steam library.
std::vector<std::string> findGameDataFolders();

// The folders of one GameData directory in search order: the mod folders
// (e.g. "MD") before "base".
std::vector<std::string> gameDataSearchFolders(const std::string& gameData);

// The asset root a file lies under: the folder that contains its "models"
// directory, or empty.
std::string baseOfAssetPath(const std::string& file);

}  // namespace g2::gui
