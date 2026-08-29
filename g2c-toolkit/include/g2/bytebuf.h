// g2/bytebuf.h — Kleiner Little-Endian-Schreibpuffer.
//
// Ghoul2-Dateien sind durchgehend little-endian. Explizites Byteweise-Schreiben
// statt memcpy von Structs macht den Code portabel und unabhaengig von
// Padding-Annahmen des Compilers.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace g2 {

class ByteBuf {
public:
    std::size_t size() const { return data_.size(); }
    const std::vector<std::uint8_t>& bytes() const { return data_; }

    void u8(std::uint8_t v) { data_.push_back(v); }

    void u16(std::uint16_t v) {
        data_.push_back(static_cast<std::uint8_t>(v & 0xff));
        data_.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    void i32(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        data_.push_back(static_cast<std::uint8_t>(u & 0xff));
        data_.push_back(static_cast<std::uint8_t>((u >> 8) & 0xff));
        data_.push_back(static_cast<std::uint8_t>((u >> 16) & 0xff));
        data_.push_back(static_cast<std::uint8_t>((u >> 24) & 0xff));
    }

    void u32(std::uint32_t v) { i32(static_cast<std::int32_t>(v)); }

    void f32(float v) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        u32(bits);
    }

    void u24(std::uint32_t v) {
        data_.push_back(static_cast<std::uint8_t>(v & 0xff));
        data_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        data_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    }

    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        data_.insert(data_.end(), b, b + n);
    }

    // Fester Stringpuffer, null-terminiert und mit Nullen aufgefuellt.
    // Zu lange Namen sind ein harter Fehler statt einer stillen Kuerzung —
    // Carcass warnt hier zwar (Cmd_XSIConvertMDX bei 0x41d350), schreibt aber
    // trotzdem weiter.
    void fixedString(const std::string& s, std::size_t width, const char* what) {
        if (s.size() >= width) {
            throw std::runtime_error(std::string(what) + ": Name \"" + s + "\" ist " +
                                     std::to_string(s.size()) + " Zeichen lang, erlaubt sind " +
                                     std::to_string(width - 1));
        }
        data_.insert(data_.end(), s.begin(), s.end());
        data_.insert(data_.end(), width - s.size(), 0);
    }

    void pad(std::size_t n) { data_.insert(data_.end(), n, 0); }

    // Auf ein Vielfaches von n auffuellen. Zwischen Frame-Indizes und
    // Bone-Pool braucht die GLA ein 4-Byte-Alignment.
    void alignTo(std::size_t n) {
        while (data_.size() % n) data_.push_back(0);
    }

    // Platzhalter fuer spaeter bekannte Offsets.
    std::size_t reserveI32() {
        const std::size_t at = data_.size();
        i32(0);
        return at;
    }

    void patchI32(std::size_t at, std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        data_[at + 0] = static_cast<std::uint8_t>(u & 0xff);
        data_[at + 1] = static_cast<std::uint8_t>((u >> 8) & 0xff);
        data_[at + 2] = static_cast<std::uint8_t>((u >> 16) & 0xff);
        data_[at + 3] = static_cast<std::uint8_t>((u >> 24) & 0xff);
    }

private:
    std::vector<std::uint8_t> data_;
};

}  // namespace g2
