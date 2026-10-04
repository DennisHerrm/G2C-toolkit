#include "gui/preview.h"

#include <algorithm>
#include <cmath>

namespace g2::gui {
namespace {

constexpr double kDeg2Rad = 0.017453292519943295;

std::array<float, 3> sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}};
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}};
}

std::array<float, 3> normalize(std::array<float, 3> v) {
    const float len = std::sqrt(dot(v, v));
    if (len > 1e-9f)
        for (auto& x : v) x /= len;
    return v;
}

// Camera position from angles and distance.
//
// Z is up. That matches the GLA's convention - in the game the figure
// stands along Z, not Y. Assuming Y is up here shows the model lying down.
std::array<float, 3> eyePosition(const PreviewCamera& cam) {
    const double yaw = cam.yawDeg * kDeg2Rad;
    const double pitch = cam.pitchDeg * kDeg2Rad;
    const double cp = std::cos(pitch);
    return {{cam.target[0] + static_cast<float>(cam.distance * cp * std::cos(yaw)),
             cam.target[1] + static_cast<float>(cam.distance * cp * std::sin(yaw)),
             cam.target[2] + static_cast<float>(cam.distance * std::sin(pitch))}};
}

}  // namespace

std::array<float, 3> Bounds::center() const {
    return {{(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f, (min[2] + max[2]) * 0.5f}};
}

float Bounds::radius() const {
    if (!valid) return 1.0f;
    float r = 0.0f;
    for (int i = 0; i < 3; ++i) r = std::max(r, (max[i] - min[i]) * 0.5f);
    return r > 1e-6f ? r : 1.0f;
}

Bounds computeBounds(const std::vector<Mat3x4>& world) {
    Bounds b;
    for (const auto& m : world) {
        const std::array<float, 3> p{{m.m[0][3], m.m[1][3], m.m[2][3]}};
        if (!b.valid) {
            b.min = p;
            b.max = p;
            b.valid = true;
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            b.min[static_cast<std::size_t>(i)] =
                std::min(b.min[static_cast<std::size_t>(i)], p[static_cast<std::size_t>(i)]);
            b.max[static_cast<std::size_t>(i)] =
                std::max(b.max[static_cast<std::size_t>(i)], p[static_cast<std::size_t>(i)]);
        }
    }
    return b;
}

void frameAll(PreviewCamera& cam, const Bounds& b) {
    if (!b.valid) return;
    cam.target = b.center();

    // Choose the distance so that the sphere around the model fits into the
    // field of view, with some margin at the edge.
    const float halfFov = static_cast<float>(cam.fovDeg * 0.5 * kDeg2Rad);
    const float s = std::sin(halfFov);
    cam.distance = s > 1e-6f ? (b.radius() * 1.6f) / s : b.radius() * 4.0f;
}

bool projectPoint(const std::array<float, 3>& p, const PreviewCamera& cam, float viewW,
                  float viewH, float& outX, float& outY, float& outDepth) {
    if (viewW <= 1.0f || viewH <= 1.0f) return false;

    const std::array<float, 3> eye = eyePosition(cam);
    const std::array<float, 3> fwd = normalize(sub(cam.target, eye));
    const std::array<float, 3> worldUp{{0.0f, 0.0f, 1.0f}};

    std::array<float, 3> right = cross(fwd, worldUp);
    if (dot(right, right) < 1e-12f) {
        // Looking straight down or up: then the right axis is undefined.
        // Make an arbitrary but fixed choice instead of dividing by zero.
        right = {{1.0f, 0.0f, 0.0f}};
    }
    right = normalize(right);
    const std::array<float, 3> up = normalize(cross(right, fwd));

    const std::array<float, 3> rel = sub(p, eye);
    const float z = dot(rel, fwd);
    if (z <= 1e-4f) return false;   // behind the camera

    const float halfFov = static_cast<float>(cam.fovDeg * 0.5 * kDeg2Rad);
    const float f = 1.0f / std::tan(halfFov);
    const float aspect = viewW / viewH;

    const float ndcX = (dot(rel, right) * (f / aspect)) / z;
    const float ndcY = (dot(rel, up) * f) / z;

    outX = (ndcX * 0.5f + 0.5f) * viewW;
    // Screen coordinates run downward, the up axis runs upward.
    outY = (0.5f - ndcY * 0.5f) * viewH;
    outDepth = z;
    return true;
}

std::vector<BoneLine> buildBoneLines(const MdxaFile& gla, const std::vector<Mat3x4>& world,
                                     const PreviewCamera& cam, float viewW, float viewH) {
    std::vector<BoneLine> out;
    const auto& bones = gla.skeleton.bones;
    if (world.size() != bones.size()) return out;

    for (std::size_t b = 0; b < bones.size(); ++b) {
        const int p = bones[b].parent;
        if (p < 0) continue;   // root has no connection

        const std::array<float, 3> a{{world[static_cast<std::size_t>(p)].m[0][3],
                                      world[static_cast<std::size_t>(p)].m[1][3],
                                      world[static_cast<std::size_t>(p)].m[2][3]}};
        const std::array<float, 3> c{{world[b].m[0][3], world[b].m[1][3], world[b].m[2][3]}};

        BoneLine l;
        float d0 = 0.0f, d1 = 0.0f;
        if (!projectPoint(a, cam, viewW, viewH, l.x0, l.y0, d0)) continue;
        if (!projectPoint(c, cam, viewW, viewH, l.x1, l.y1, d1)) continue;

        l.from = p;
        l.to = static_cast<int>(b);
        l.depth = (d0 + d1) * 0.5f;
        out.push_back(l);
    }

    // Back to front: whatever is closer to the camera is drawn last and
    // thus ends up on top.
    std::sort(out.begin(), out.end(),
              [](const BoneLine& a, const BoneLine& b) { return a.depth > b.depth; });
    return out;
}

void advancePlayback(Playback& pb, float dt, double fps, int frameCount) {
    if (!pb.playing || frameCount <= 1) return;

    // Negative rates mean backwards. Raven's animation.cfg has e.g.
    // BOTH_UNCROUCH1 at -20; that is BOTH_CROUCH1 played backwards.
    const double rate = fps != 0 ? fps : 20;
    const float step = static_cast<float>(std::fabs(rate));
    pb.accumulator += dt * step;

    while (pb.accumulator >= 1.0f) {
        pb.accumulator -= 1.0f;
        pb.frame += rate > 0 ? 1 : -1;

        if (pb.frame >= frameCount) {
            if (!pb.loop) {
                pb.frame = frameCount - 1;
                pb.playing = false;
                break;
            }
            pb.frame = 0;
        } else if (pb.frame < 0) {
            if (!pb.loop) {
                pb.frame = 0;
                pb.playing = false;
                break;
            }
            pb.frame = frameCount - 1;
        }
    }
}

}  // namespace g2::gui
