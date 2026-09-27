#include "g2/compress.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace g2 {

// Write to a neighboring file first, then rename.
//
// Writing directly to the target file truncates it to zero as the very first
// step. If anything goes wrong after that - disk full, network drive gone,
// file locked by the game - the old version is lost and the new one is
// incomplete. Especially with _humanoid.gla, which is usually also the
// reference for the next build, that would mean losing several days of work.
//
// The rename replaces the target in a single step: afterwards either the old
// or the new file is there in full, never anything in between.
void writeFileChecked(const std::string& path, const void* data, std::size_t size) {
    namespace fs = std::filesystem;
    const fs::path target(path);
    fs::path tmp = target;
    tmp += ".g2c_tmp";

    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) throw std::runtime_error("Kann \"" + path + "\" nicht zum Schreiben oeffnen");

        if (size) {
            f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!f) {
                f.close();
                std::error_code ec;
                fs::remove(tmp, ec);
                throw std::runtime_error("Fehler beim Schreiben von \"" + path + "\"");
            }
        }

        // Only close() flushes the buffer. A write error often shows up
        // exactly here and not already at write().
        f.close();
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw std::runtime_error("Fehler beim Abschliessen von \"" + path + "\"");
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        throw std::runtime_error("Kann \"" + path + "\" nicht ersetzen: " + ec.message());
    }
}

void writeFileChecked(const std::string& path, const std::string& text) {
    writeFileChecked(path, text.data(), text.size());
}
namespace {

// Format constants. Do not touch, the engine decodes with exactly these.
constexpr float kQuatScale = 16383.0f;
constexpr float kQuatBias  = 2.0f;
constexpr float kXlatScale = 64.0f;
constexpr float kXlatBias  = 512.0f;

// Limits that follow from them.
constexpr float kQuatMax = 2.0f;
constexpr float kXlatMax = 511.0f;   // (511 + 512) * 64 = 65472, fits in uint16

constexpr std::uint16_t kU16Max = 65535;

inline std::uint16_t toU16(float scaled, Rounding r) {
    // Legacy: this is bit-exactly what _ftol does, namely truncate toward zero.
    // Nearest: std::lrintf with the default rounding mode, i.e. round to
    // nearest, ties to even. That halves the expected error and removes the
    // one-sided bias.
    long v = (r == Rounding::Legacy) ? static_cast<long>(scaled) : std::lrintf(scaled);
    if (v < 0) v = 0;
    if (v > kU16Max) v = kU16Max;
    return static_cast<std::uint16_t>(v);
}

inline void writeU16LE(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xff);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

inline std::uint16_t readU16LE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

std::string CompressStats::summary() const {
    std::ostringstream os;
    if (clean() && nonUnitQuat == 0) return "keine Auffaelligkeiten";
    if (quatClamped) os << quatClamped << " Quaternionkomponenten geklemmt; ";
    if (xlatClamped) os << xlatClamped << " Translationen geklemmt (max |t| = " << maxXlatSeen << "); ";
    if (nonUnitQuat) os << nonUnitQuat << " nicht normierte Quaternionen korrigiert; ";
    std::string s = os.str();
    if (s.size() >= 2) s.erase(s.size() - 2);
    return s;
}

std::uint16_t squashQuatComponent(float f, Rounding r, CompressStats& stats) {
    if (!(f >= -kQuatMax && f <= kQuatMax)) {   // also catches NaN
        ++stats.quatClamped;
        // Carcass returns 0 here, which decodes to -2.0. We clamp to the
        // nearest valid value instead.
        f = std::isnan(f) ? 0.0f : std::clamp(f, -kQuatMax, kQuatMax);
    }
    return toU16((f + kQuatBias) * kQuatScale, r);
}

std::uint16_t squashXlatComponent(float f, Rounding r, CompressStats& stats) {
    if (!(f >= -kXlatMax && f <= kXlatMax)) {
        ++stats.xlatClamped;
        stats.maxXlatSeen = std::max(stats.maxXlatSeen, std::isnan(f) ? 0.0f : std::fabs(f));
        f = std::isnan(f) ? 0.0f : std::clamp(f, -kXlatMax, kXlatMax);
    }
    return toU16((f + kXlatBias) * kXlatScale, r);
}

float unsquashQuatComponent(std::uint16_t raw) {
    return static_cast<float>(raw) / kQuatScale - kQuatBias;
}

float unsquashXlatComponent(std::uint16_t raw) {
    return static_cast<float>(raw) / kXlatScale - kXlatBias;
}

float dot(const Quat& a, const Quat& b) {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

Quat normalize(const Quat& q) {
    const float len2 = dot(q, q);
    if (len2 <= 1e-12f) return Quat{};
    const float inv = 1.0f / std::sqrt(len2);
    return Quat{q.w * inv, q.x * inv, q.y * inv, q.z * inv};
}

double angleBetweenDeg(const Quat& a, const Quat& b) {
    const Quat na = normalize(a);
    Quat nb = normalize(b);
    // q and -q are the same rotation; pick the closer sign.
    if (dot(na, nb) < 0.0f) { nb.w = -nb.w; nb.x = -nb.x; nb.y = -nb.y; nb.z = -nb.z; }

    const double dw = double(na.w) - nb.w, dx = double(na.x) - nb.x;
    const double dy = double(na.y) - nb.y, dz = double(na.z) - nb.z;
    const double sw = double(na.w) + nb.w, sx = double(na.x) + nb.x;
    const double sy = double(na.y) + nb.y, sz = double(na.z) + nb.z;

    const double diff = std::sqrt(dw * dw + dx * dx + dy * dy + dz * dz);
    const double sum  = std::sqrt(sw * sw + sx * sx + sy * sy + sz * sz);
    return 2.0 * std::atan2(diff, sum) * 180.0 / 3.14159265358979323846;
}

Quat matrixToQuat(const Mat3x4& mat) {
    const float(&m)[3][4] = mat.m;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    Quat q;

    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;   // s = 4w
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;   // s = 4x
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;   // s = 4y
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;   // s = 4z
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return normalize(q);
}

void quatToMatrix(const Quat& q, Mat3x4& out) {
    const float tx = 2.0f * q.x, ty = 2.0f * q.y, tz = 2.0f * q.z;
    const float twx = tx * q.w, twy = ty * q.w, twz = tz * q.w;
    const float txx = tx * q.x, txy = ty * q.x, txz = tz * q.x;
    const float tyy = ty * q.y, tyz = tz * q.y, tzz = tz * q.z;

    out.m[0][0] = 1.0f - (tyy + tzz);
    out.m[0][1] = txy - twz;
    out.m[0][2] = txz + twy;
    out.m[1][0] = txy + twz;
    out.m[1][1] = 1.0f - (txx + tzz);
    out.m[1][2] = tyz - twx;
    out.m[2][0] = txz - twy;
    out.m[2][1] = tyz + twx;
    out.m[2][2] = 1.0f - (txx + tyy);
}

fmt::CompQuatBone compressBone(const Mat3x4& mat, const CompressOptions& opt, CompressStats& stats) {
    Quat q = matrixToQuat(mat);

    // matrixToQuat already normalizes; here we only record whether the input
    // matrix was a clean rotation in the first place. Non-uniform scaling in
    // the skeleton is a common mistake in source assets, and Carcass does
    // report it (0x430790), but only per bone.
    {
        const float c0 = std::sqrt(mat.m[0][0] * mat.m[0][0] + mat.m[1][0] * mat.m[1][0] +
                                   mat.m[2][0] * mat.m[2][0]);
        if (std::fabs(c0 - 1.0f) > 1e-3f) ++stats.nonUnitQuat;
    }

    if (opt.canonicalizeSign && q.w < 0.0f) {
        q.w = -q.w; q.x = -q.x; q.y = -q.y; q.z = -q.z;
    }

    fmt::CompQuatBone out{};
    const Rounding r = opt.rounding;

    std::uint16_t raw[4] = {
        squashQuatComponent(q.w, r, stats),
        squashQuatComponent(q.x, r, stats),
        squashQuatComponent(q.y, r, stats),
        squashQuatComponent(q.z, r, stats),
    };

    if (opt.optimizeQuat && r == Rounding::Nearest) {
        // Target matrix from the exactly normalized quaternion.
        Mat3x4 target;
        quatToMatrix(q, target);

        const auto matrixError = [&target](const Quat& cand) {
            Mat3x4 m;
            quatToMatrix(cand, m);
            float e = 0.0f;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const float d = m.m[i][j] - target.m[i][j];
                    e += d * d;
                }
            return e;
        };

        std::uint16_t best[4] = {raw[0], raw[1], raw[2], raw[3]};
        float bestErr = matrixError(Quat{unsquashQuatComponent(raw[0]), unsquashQuatComponent(raw[1]),
                                         unsquashQuatComponent(raw[2]), unsquashQuatComponent(raw[3])});

        for (int dw = -1; dw <= 1; ++dw)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        const int d[4] = {dw, dx, dy, dz};
                        std::uint16_t cand[4];
                        for (int k = 0; k < 4; ++k) {
                            const int v = static_cast<int>(raw[k]) + d[k];
                            if (v < 0 || v > 65535) goto skip;
                            cand[k] = static_cast<std::uint16_t>(v);
                        }
                        {
                            const float e = matrixError(Quat{
                                unsquashQuatComponent(cand[0]), unsquashQuatComponent(cand[1]),
                                unsquashQuatComponent(cand[2]), unsquashQuatComponent(cand[3])});
                            if (e < bestErr) {
                                bestErr = e;
                                for (int k = 0; k < 4; ++k) best[k] = cand[k];
                            }
                        }
                    skip:;
                    }

        for (int k = 0; k < 4; ++k) raw[k] = best[k];
    }

    writeU16LE(out.comp + 0, raw[0]);
    writeU16LE(out.comp + 2, raw[1]);
    writeU16LE(out.comp + 4, raw[2]);
    writeU16LE(out.comp + 6, raw[3]);
    writeU16LE(out.comp + 8,  squashXlatComponent(mat.m[0][3], r, stats));
    writeU16LE(out.comp + 10, squashXlatComponent(mat.m[1][3], r, stats));
    writeU16LE(out.comp + 12, squashXlatComponent(mat.m[2][3], r, stats));
    return out;
}

Mat3x4 uncompressBone(const fmt::CompQuatBone& c) {
    Quat q;
    q.w = unsquashQuatComponent(readU16LE(c.comp + 0));
    q.x = unsquashQuatComponent(readU16LE(c.comp + 2));
    q.y = unsquashQuatComponent(readU16LE(c.comp + 4));
    q.z = unsquashQuatComponent(readU16LE(c.comp + 6));

    Mat3x4 out;
    quatToMatrix(q, out);
    out.m[0][3] = unsquashXlatComponent(readU16LE(c.comp + 8));
    out.m[1][3] = unsquashXlatComponent(readU16LE(c.comp + 10));
    out.m[2][3] = unsquashXlatComponent(readU16LE(c.comp + 12));
    return out;
}

float boneRoundTripError(const Mat3x4& original, const CompressOptions& opt) {
    CompressStats s;
    const Mat3x4 back = uncompressBone(compressBone(original, opt, s));
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            worst = std::max(worst, std::fabs(back.m[i][j] - original.m[i][j]));
    return worst;
}

}  // namespace g2
