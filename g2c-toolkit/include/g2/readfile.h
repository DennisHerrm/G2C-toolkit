// g2/readfile.h — Datei am Stueck einlesen.
//
// Das verbreitete Idiom
//     std::string s{std::istreambuf_iterator<char>(f), {}};
// liest zeichenweise ueber den Streampuffer und reallokiert dabei laufend.
// Auf einer 1,7-MB-dotXSI kostet das etwa so viel wie das gesamte Parsen
// danach, auf einer 10-MB-GLA entsprechend mehr. Einmal Groesse ermitteln,
// einmal reservieren, einmal lesen.

#pragma once

#include <cstdint>
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

inline std::vector<std::uint8_t> readWholeFileBytes(const std::string& path) {
    const std::string s = readWholeFile(path);
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace g2
