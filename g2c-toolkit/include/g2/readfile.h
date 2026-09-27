// g2/readfile.h - Reading a file in one piece.
//
// The common idiom
//     std::string s{std::istreambuf_iterator<char>(f), {}};
// reads character by character through the stream buffer and keeps
// reallocating. On a 1.7 MB dotXSI this costs about as much as all the
// parsing afterwards, on a 10 MB GLA correspondingly more. Determine the
// size once, reserve once, read once.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace g2 {

inline std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Kann \"" + path + "\" nicht oeffnen");
    const std::streamsize n = f.tellg();
    if (n < 0) throw std::runtime_error("Kann Groesse von \"" + path + "\" nicht ermitteln");
    std::string out(static_cast<std::size_t>(n), '\0');
    f.seekg(0);
    if (n > 0 && !f.read(out.data(), n))
        throw std::runtime_error("Lesefehler in \"" + path + "\"");
    return out;
}

// Reads directly into the byte buffer. The detour via std::string copied a
// 22 MB GLA a second time and briefly held both copies in memory.
inline std::vector<std::uint8_t> readWholeFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Kann \"" + path + "\" nicht oeffnen");
    const std::streamsize n = f.tellg();
    if (n < 0) throw std::runtime_error("Kann Groesse von \"" + path + "\" nicht ermitteln");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    f.seekg(0);
    if (n > 0 && !f.read(reinterpret_cast<char*>(out.data()), n))
        throw std::runtime_error("Lesefehler in \"" + path + "\"");
    return out;
}

// Reads an environment variable; empty if it is not set.
//
// Uses _dupenv_s instead of getenv: MSVC flags getenv as unsafe because the
// returned pointer can be invalidated by later changes to the environment.
// The copy does not have that problem.
inline std::string envValue(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0 || !v) return {};
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

}  // namespace g2
