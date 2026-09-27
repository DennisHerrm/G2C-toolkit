#include "g2/animcache.h"

#include "g2/bytebuf.h"
#include "g2/readfile.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace g2 {
namespace fs = std::filesystem;
namespace {

constexpr char kMagic[4] = {'G', '2', 'A', 'C'};

// FNV-1a. Good enough for file names; nothing is being secured here, only
// distributed. The actual validity check uses the size and timestamp in the
// entry header, not the hash.
std::uint64_t hash64(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex16(std::uint64_t v) {
    static const char* d = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = d[v & 0xf];
        v >>= 4;
    }
    return out;
}

struct SourceStamp {
    std::uint64_t size = 0;
    std::int64_t  mtime = 0;
    bool          valid = false;
};

SourceStamp stampOf(const std::string& path) {
    SourceStamp s;
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (ec) return s;
    const auto tm = fs::last_write_time(path, ec);
    if (ec) return s;
    s.size = static_cast<std::uint64_t>(sz);
    s.mtime = static_cast<std::int64_t>(tm.time_since_epoch().count());
    s.valid = true;
    return s;
}

// --- Reading -------------------------------------------------------------

class Reader {
public:
    Reader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}

    bool ok() const { return ok_; }
    bool atEnd() const { return i_ == n_; }

    std::uint32_t u32() {
        if (i_ + 4 > n_) { ok_ = false; return 0; }
        const std::uint32_t v = static_cast<std::uint32_t>(p_[i_]) |
                                (static_cast<std::uint32_t>(p_[i_ + 1]) << 8) |
                                (static_cast<std::uint32_t>(p_[i_ + 2]) << 16) |
                                (static_cast<std::uint32_t>(p_[i_ + 3]) << 24);
        i_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }
    float f32() {
        const std::uint32_t b = u32();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    std::string str() {
        const std::uint32_t len = u32();
        if (!ok_ || i_ + len > n_ || len > (1u << 20)) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + i_), len);
        i_ += len;
        return s;
    }
    bool raw(void* dst, std::size_t n) {
        if (i_ + n > n_) { ok_ = false; return false; }
        std::memcpy(dst, p_ + i_, n);
        i_ += n;
        return true;
    }

private:
    const std::uint8_t* p_;
    std::size_t         n_;
    std::size_t         i_ = 0;
    bool                ok_ = true;
};

}  // namespace

std::string AnimCache::entryPath(const std::string& sourcePath) const {
    // The file name contains the hash of the source path; that is enough to
    // find it again. Whether the entry is still valid is decided by the header.
    return (fs::path(dir_) / (hex16(hash64(sourcePath)) + ".g2ac")).string();
}

std::optional<xsi::AnimFile> AnimCache::load(const std::string& sourcePath) {
    if (!enabled()) return std::nullopt;

    const SourceStamp want = stampOf(sourcePath);
    if (!want.valid) return std::nullopt;

    std::vector<std::uint8_t> data;
    try {
        data = readWholeFileBytes(entryPath(sourcePath));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (data.size() < 32) return std::nullopt;

    Reader r(data.data(), data.size());
    char magic[4];
    if (!r.raw(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return std::nullopt;
    if (r.u32() != kAnimCacheVersion) return std::nullopt;
    if (r.u64() != want.size) return std::nullopt;
    if (static_cast<std::int64_t>(r.u64()) != want.mtime) return std::nullopt;

    xsi::AnimFile a;
    a.sourcePath = sourcePath;
    a.firstFrame = r.i32();
    a.lastFrame = r.i32();
    a.frameRate = r.f32();
    a.hasScene = r.u32() != 0;
    a.sceneFirst = r.i32();
    a.sceneLast = r.i32();

    const std::uint32_t nodeCount = r.u32();
    if (!r.ok() || nodeCount > (1u << 20)) return std::nullopt;
    a.nodes.resize(nodeCount);

    for (std::size_t idx = 0; idx < a.nodes.size(); ++idx) {
        auto& n = a.nodes[idx];
        n.name = r.str();
        n.parent = r.i32();
        // worldMatrices assumes that parents come before their children.
        // A corrupted entry must not undermine that, otherwise we would read
        // out of bounds. Better to re-parse in that case.
        if (n.parent < -1 || n.parent >= static_cast<std::int32_t>(idx)) return std::nullopt;
        n.hasSrt = r.u32() != 0;
        for (auto& v : n.srt) v = r.f32();

        const std::uint32_t chanCount = r.u32();
        if (!r.ok() || chanCount > 64) return std::nullopt;
        for (std::uint32_t c = 0; c < chanCount; ++c) {
            const std::string name = r.str();
            const std::uint32_t keys = r.u32();
            if (!r.ok() || keys > (1u << 22)) return std::nullopt;
            auto& target = n.channels[name];
            for (std::uint32_t k = 0; k < keys; ++k) {
                const std::int32_t frame = r.i32();
                const float value = r.f32();
                target[frame] = value;
            }
        }
    }
    // Leftover bytes mean the entry is not the one that store()
    // wrote.
    if (!r.ok() || !r.atEnd()) return std::nullopt;

    stats_.bytesRead += data.size();
    return a;
}

void AnimCache::store(const std::string& sourcePath, const xsi::AnimFile& anim) {
    if (!enabled()) return;

    const SourceStamp st = stampOf(sourcePath);
    if (!st.valid) return;

    std::error_code ec;
    fs::create_directories(dir_, ec);

    ByteBuf b;
    b.raw(kMagic, 4);
    b.u32(kAnimCacheVersion);
    b.u32(static_cast<std::uint32_t>(st.size & 0xffffffffu));
    b.u32(static_cast<std::uint32_t>((st.size >> 32) & 0xffffffffu));
    b.u32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(st.mtime) & 0xffffffffu));
    b.u32(static_cast<std::uint32_t>((static_cast<std::uint64_t>(st.mtime) >> 32) & 0xffffffffu));
    b.i32(anim.firstFrame);
    b.i32(anim.lastFrame);
    b.f32(anim.frameRate);
    b.u32(anim.hasScene ? 1u : 0u);
    b.i32(anim.sceneFirst);
    b.i32(anim.sceneLast);
    b.u32(static_cast<std::uint32_t>(anim.nodes.size()));

    const auto putStr = [&](const std::string& s) {
        b.u32(static_cast<std::uint32_t>(s.size()));
        b.raw(s.data(), s.size());
    };

    for (const auto& n : anim.nodes) {
        putStr(n.name);
        b.i32(n.parent);
        b.u32(n.hasSrt ? 1u : 0u);
        for (float v : n.srt) b.f32(v);
        b.u32(static_cast<std::uint32_t>(n.channels.size()));
        for (const auto& [name, keys] : n.channels) {
            putStr(name);
            b.u32(static_cast<std::uint32_t>(keys.size()));
            for (const auto& [frame, value] : keys) {
                b.i32(frame);
                b.f32(value);
            }
        }
    }

    // Write to a side file first, then rename. If the run aborts halfway,
    // no half-written entry is left lying around for a later run to mistake
    // for a valid one.
    //
    // The side file carries the thread ID: if the same .xsi appears twice
    // in the script, two threads write the same entry concurrently, and a
    // shared name would make both write into the same file.
    const std::string finalPath = entryPath(sourcePath);
    const std::string tmpPath =
        finalPath + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
        ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(b.bytes().data()),
                static_cast<std::streamsize>(b.size()));
        if (!f) { f.close(); fs::remove(tmpPath, ec); return; }
    }
    fs::rename(tmpPath, finalPath, ec);
    if (ec) { fs::remove(tmpPath, ec); return; }

    stats_.bytesWritten += b.size();
}

xsi::AnimFile AnimCache::loadOrParse(const std::string& sourcePath) {
    if (auto cached = load(sourcePath)) {
        ++stats_.hits;
        return std::move(*cached);
    }
    ++stats_.misses;
    xsi::AnimFile a = xsi::loadAnimationFile(sourcePath);
    store(sourcePath, a);
    return a;
}

std::size_t AnimCache::clear() {
    if (!enabled()) return 0;
    std::error_code ec;
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (e.path().extension() == ".g2ac" || e.path().extension() == ".tmp") {
            if (fs::remove(e.path(), ec)) ++n;
        }
    }
    return n;
}

std::uint64_t AnimCache::sizeOnDisk() const {
    if (!enabled()) return 0;
    std::error_code ec;
    std::uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (e.path().extension() == ".g2ac") {
            const auto sz = fs::file_size(e.path(), ec);
            if (!ec) total += sz;
        }
    }
    return total;
}

}  // namespace g2
