// g2/animcache.h - Cache for parsed animation files.
//
// Carcass does the same thing with its CARPET file. The original's build log
// says "( Reading 249.63MB CARPET file )" - the ten seconds in which it
// "processes" 1289 files are really spent reading this cache. Without it,
// Carcass would have to re-parse every .xsi as well.
//
// Measured, about 96 % of build time goes to reading and parsing the source
// files (80 % file I/O, 12 % parser, 4 % collecting FCurves). Exactly that
// result is stored here.
//
// Deliberate differences from CARPET:
//
//   - **One file per source**, not one combined archive. A corrupt entry
//     costs one file instead of the whole cache, and parallel runs cannot
//     get in each other's way.
//   - **Key from path, size and modification time.** Once a .xsi is
//     touched, its entry drops out automatically. No manual clearing.
//   - **Version number in the header.** If the reading behavior changes,
//     old entries are discarded instead of silently returning wrong data.

#pragma once

#include "g2/xsi_anim.h"

#include <cstdint>
#include <optional>
#include <string>

namespace g2 {

// Bump this whenever WHAT is read from a .xsi changes. Otherwise the cache
// returns data based on the old understanding - a bug that disguises itself
// as "the fix doesn't work" and is hard to find.
// 4: SI_Scene range stored as well, for the mismatch warning.
inline constexpr std::uint32_t kAnimCacheVersion = 4;

struct AnimCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t bytesRead = 0;

    double hitPercent() const {
        const std::uint64_t total = hits + misses;
        return total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    }
};

class AnimCache {
public:
    // An empty directory disables the cache.
    explicit AnimCache(std::string directory) : dir_(std::move(directory)) {}

    bool enabled() const { return !dir_.empty(); }

    // Returns the entry if the source's size and modification time match.
    std::optional<xsi::AnimFile> load(const std::string& sourcePath);

    // Write errors are never fatal: a cache is an optimization, not state.
    // A full disk or missing permissions must not abort a build.
    void store(const std::string& sourcePath, const xsi::AnimFile& anim);

    // Loads from the cache, or parses and stores.
    xsi::AnimFile loadOrParse(const std::string& sourcePath);

    const AnimCacheStats& stats() const { return stats_; }

    // Deletes all entries. Returns the count.
    std::size_t clear();

    // Total size of the cache in bytes.
    std::uint64_t sizeOnDisk() const;

private:
    std::string    dir_;
    AnimCacheStats stats_;

    std::string entryPath(const std::string& sourcePath) const;
};

}  // namespace g2
