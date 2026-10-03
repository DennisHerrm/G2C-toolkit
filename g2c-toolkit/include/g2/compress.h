// g2/compress.h - Quantization of the bone matrices into the 14-byte format.
//
// This is where Carcass v2.2 has two real bugs. Details in docs/BUGS.md;
// the short version:
//
//   B2  Carcass converts with _ftol, which truncates toward zero. Since a
//       constant +2.0 or +512.0 is added beforehand, the rounding error is
//       always one-sided -> directional drift instead of symmetric noise.
//       Correct rounding halves the error and is fully format-compatible.
//
//   B3  Out-of-range values return 0 in Carcass, which decodes to -2.0
//       (quaternion) or -512 units (translation). A single outlier thus
//       flings the bone to the absolute extreme instead of clamping it. On
//       top of that, it warns only ONCE per program run; after that
//       everything passes through silently.
//
// Not fixable without changing the engine: the format fixes the quaternion
// value range at -2..+2, although unit quaternions only need -1..+1. So one
// bit is permanently wasted. Changing that also means touching
// MC_UnCompressQuat in the engine and requires a new GLA version.

#pragma once

#include "g2/format.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace g2 {

// Is the C++ runtime statically linked in?
//
// This decides whether the program starts on someone else's machine at all.
// Without a built-in runtime, Windows reports "VCRUNTIME140.dll was not
// found" there, and the recipient can do nothing about it.
//
// With /MD (dynamic), MSVC defines both _MT and _DLL; with /MT (static),
// only _MT. The answer costs nothing and doesn't have to be queried with
// external tools.
constexpr bool staticRuntime() {
#if defined(_MSC_VER)
#if defined(_DLL)
    return false;
#else
    return true;
#endif
#else
    // On other systems the question doesn't arise in this form.
    return true;
#endif
}

// Write a file and really check the result.
//
// A bare "if (!f)" after opening is not enough. It only catches the case
// where the file cannot be created at all. If something goes wrong while
// WRITING - disk full, network drive gone, quota reached - the stream only
// reports it on the next access, and part of the data is already on disk.
// The result is half a GLA that looks like a whole one and shows up in the
// game as a broken model.
//
// Therefore: close after writing and check the state AFTERWARDS. Only
// close() flushes the buffer, so write errors often surface exactly there.
//
// Throws std::runtime_error with the path in the message.
void writeFileChecked(const std::string& path, const void* data, std::size_t size);
void writeFileChecked(const std::string& path, const std::string& text);

// 3x4 matrix: rotation in columns 0..2, translation in column 3.
struct Mat3x4 {
    float m[3][4]{};

    static Mat3x4 identity() {
        Mat3x4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
        return r;
    }
};

struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class Rounding {
    Nearest,  // correct: rounds to the nearest representable step
    Legacy,   // Carcass's _ftol truncation. Out-of-range values are still clamped,
              // not turned into Carcass's zero (which decodes to -2.0 / -512).
};

// Counts what went wrong during compression. Unlike Carcass, not a single
// violation gets lost.
struct CompressStats {
    std::uint64_t quatClamped = 0;   // quaternion component outside -2..2
    std::uint64_t xlatClamped = 0;   // translation outside -511..511
    std::uint64_t nonUnitQuat = 0;   // quaternion was not normalized
    float         maxXlatSeen = 0.0f;

    bool clean() const { return quatClamped == 0 && xlatClamped == 0; }
    std::string summary() const;
};

// --- Scalar quantization ---------------------------------------------------

// Quaternion component -> uint16.  raw = (f + 2.0) * 16383.0
std::uint16_t squashQuatComponent(float f, Rounding r, CompressStats& stats);

// Translation component -> uint16.  raw = (f + 512.0) * 64.0
std::uint16_t squashXlatComponent(float f, Rounding r, CompressStats& stats);

float unsquashQuatComponent(std::uint16_t raw);
float unsquashXlatComponent(std::uint16_t raw);

// --- Matrix <-> Quaternion -------------------------------------------------

// Converts the rotation part into a quaternion. Uses Shepperd's method, i.e.
// the term with the largest magnitude as the pivot, which is numerically much
// more stable for rotations near 180 degrees than the naive trace formula.
Quat matrixToQuat(const Mat3x4& mat);

// Builds the rotation part from the quaternion. Matches exactly the
// convention the engine uses in MC_UnCompressQuat.
void quatToMatrix(const Quat& q, Mat3x4& out);

Quat normalize(const Quat& q);
float dot(const Quat& a, const Quat& b);

// Angle between two rotations in degrees.
//
// Deliberately NOT via 2*acos(dot): acos has an infinite derivative for
// arguments near 1, so float rounding error alone produces a measurement
// floor of about 0.03 degrees - considerably more than the error you
// actually want to measure. Instead, the stable form
// 2*atan2(|qa-qb|, |qa+qb|) in double precision.
double angleBetweenDeg(const Quat& a, const Quat& b);

// --- Bone compression ------------------------------------------------------

struct CompressOptions {
    Rounding rounding = Rounding::Nearest;

    // If true, the quaternion's sign is chosen so that w >= 0. q and -q
    // describe the same rotation and decode to the same matrix, but produce
    // different 14-byte entries. Canonicalizing therefore improves the hit
    // rate of the pool deduplication without changing the result. Turn off
    // for a byte-exact comparison with Carcass.
    bool canonicalizeSign = true;

    // Error-minimizing quantization.
    //
    // MC_UnCompressQuat does NOT normalize the decoded quaternion. A
    // quantization error therefore not only changes the rotation but also
    // makes the resulting matrix slightly non-orthonormal - the bones get
    // minimally sheared and scaled. That is why the measured rotation error
    // is about eight times larger than the step size alone would suggest.
    //
    // Component-wise rounding is therefore not optimal. With this option
    // enabled, the 81 candidates within +/-1 step per component are tried,
    // and the one whose decoded matrix deviates least from the target
    // rotation is chosen. Costs about 2500 flops per bone and does not change
    // the file format.
    bool optimizeQuat = true;
};

fmt::CompQuatBone compressBone(const Mat3x4& mat, const CompressOptions& opt, CompressStats& stats);
Mat3x4            uncompressBone(const fmt::CompQuatBone& c);

// Maximum absolute position error produced by a compression.
// Useful for regression tests against reference models.
float boneRoundTripError(const Mat3x4& original, const CompressOptions& opt);

}  // namespace g2
