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

// Direkt in den Byte-Puffer lesen. Der Umweg ueber std::string kopierte eine
// 22-MB-GLA ein zweites Mal und hielt kurzzeitig beide Fassungen im Speicher.
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

// Umgebungsvariable lesen; leer, wenn sie fehlt.
//
// Ueber _dupenv_s statt getenv: MSVC markiert getenv als unsicher, weil der
// zurueckgegebene Zeiger von spaeteren Aenderungen der Umgebung ueberholt
// werden kann. Die Kopie hat das Problem nicht.
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
