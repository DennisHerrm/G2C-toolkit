// gui/preview.h - Camera and projection for the skeleton preview.
//
// Deliberately without DirectX: drawing uses ImGui's draw list, i.e. lines
// in screen coordinates. There are two reasons for this.
//
// First, the code thus ends up in a file that can be compiled and tested
// here - unlike gui/main_win32.cpp, where several bugs have already slipped
// through unnoticed today.
//
// Second, a skeleton doesn't need a graphics API. ImGui draws a few hundred
// lines per frame effortlessly, and we save ourselves shaders, buffers and a
// second render path.

#pragma once

#include "g2/compress.h"
#include "g2/mdxa.h"

#include <array>
#include <vector>

namespace g2::gui {

// Orbit camera: target point in the center, angles and distance around it.
struct PreviewCamera {
    float yawDeg = 30.0f;
    float pitchDeg = 15.0f;
    float distance = 100.0f;
    std::array<float, 3> target{{0.0f, 0.0f, 0.0f}};

    // Vertical field of view in degrees.
    float fovDeg = 45.0f;
};

// Axis-aligned box around a point cloud.
struct Bounds {
    std::array<float, 3> min{{0, 0, 0}};
    std::array<float, 3> max{{0, 0, 0}};
    bool                 valid = false;

    std::array<float, 3> center() const;
    float                radius() const;
};

Bounds computeBounds(const std::vector<Mat3x4>& world);

// Positions the camera so that everything fits into view.
//
// Without this, the camera for a 53-bone model ends up either inside the
// head or a hundred units off - depending on how large the model is. The
// GLA's scaling is not uniform (0.6 to 0.64), and custom models can be
// considerably larger.
void frameAll(PreviewCamera& cam, const Bounds& b);

// Point in screen coordinates.
//
// Returns false if the point is behind the camera - then the line must not
// be drawn, otherwise it appears mirrored on the wrong side.
bool projectPoint(const std::array<float, 3>& p, const PreviewCamera& cam, float viewW,
                  float viewH, float& outX, float& outY, float& outDepth);

// A connection between two bones to be drawn.
struct BoneLine {
    int   from = 0;
    int   to = 0;
    float depth = 0.0f;   // mean depth, for sorting
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// All visible connections for a frame, sorted back to front. Root bones
// without a parent produce no line.
std::vector<BoneLine> buildBoneLines(const MdxaFile& gla, const std::vector<Mat3x4>& world,
                                     const PreviewCamera& cam, float viewW, float viewH);

// Next frame during playback.
//
// Runs at the sequence's rate, not at the UI's frame rate: an animation
// at 20 frames per second should run at 20 even at 144 Hz. Negative rates
// mean backwards - this occurs in Raven's animation.cfg, e.g. for
// BOTH_UNCROUCH1 at -20.
struct Playback {
    int   frame = 0;
    float accumulator = 0.0f;
    bool  playing = false;
    bool  loop = true;
};

// dt in seconds, fps from animation.cfg (may be negative).
void advancePlayback(Playback& pb, float dt, int fps, int frameCount);

}  // namespace g2::gui
