// g2/sidefiles.h - The two companion files Carcass produces alongside the GLA
// and GLM.
//
// Both formats were derived from Raven's originals: `model_red.skin` from
// the Luke model and `_humanoid.frames` from a real Carcass run.

#pragma once

#include "g2/carscript.h"
#include "g2/model.h"

#include <array>
#include <string>
#include <vector>

namespace g2 {

// --- .skin ----------------------------------------------------------------
//
// One "surfacename,texturepath" per line, CRLF line endings:
//
//     hips,models/players/luke/boots_hips_red.tga
//     l_leg,models/players/luke/boots_hips_red.tga
//     l_leg_cap_hips_off,models/players/stormtrooper/caps.tga
//
// All surfaces that are NOT tags are listed. Checked against Raven's
// `model_red.skin`: 34 entries for 80 surfaces, 46 of which are tags - so
// exactly the remaining 34, minus `stupidtriangle_off`, which carries
// "[nomaterial]".
//
// The file is not a Ghoul2 format in the strict sense but a texture mapping
// that the game reads at runtime. That is why what matters here is the
// shader name, not the geometry.
std::string writeSkin(const Mesh& mesh);

// --- .frames --------------------------------------------------------------
//
// One block per collected animation file:
//
//     <blank line>
//     c:/path/to/source.xsi
//     {
//         "startframe"  "0"
//         "duration"    "2"
//         "fps"         "60"
//         "averagevec"  "0.000 0.000 0.000"
//     }
//
// Keys and values are each in quotes and separated by a tab, and the lines
// are indented with a tab.
//
// `fps` is the frame rate from the source file's SI_Scene, NOT the
// framespeed from animation.cfg: for `face_alert.xsi` this says 60, while
// the .car specifies -framespeed 1.
//
// `averagevec` is the root motion **per frame**, with the opposite sign of
// the ramp on the root bone. Cross-checked:
//
//   both_strafe_left1   averagevec 3.520   ramp -42.25/12 = -3.521
//   both_death17        averagevec 0.028   ramp -3.444/124 = -0.0278
//   both_sit2tostand5   averagevec -0.084  ramp +4.62/55  = +0.084
//   both_wall_flip_right averagevec 0      no motion
struct FrameEntry {
    std::string sourcePath;   // as referenced in the script, absolute
    int         startFrame = 0;
    int         duration = 0;
    int         fps = 0;
    float       averageVec[3] = {0.0f, 0.0f, 0.0f};
    std::vector<std::array<float, 3>> deltaVecs;   // -deltavecs; replaces averagevec
};

std::string writeFrames(const std::vector<FrameEntry>& entries);

// --- <name>_info.txt ------------------------------------------------------
//
// Carcass writes this next to the GLM on every run. Layout byte-exact as in
// Carcass v2.2 (0x4416a0): the GLM header, the shaders, what is rendered by
// default per LOD, then either the GLA header with the pooling statistics and
// the bone list, or - for a model script - which GLA it uses.
struct InfoInput {
    std::vector<std::uint8_t> glm;   // the GLM as written
    // Shader per LOD-0 surface BEFORE -makeskin blanked them in the GLM.
    // Empty = take them from the GLM.
    std::vector<std::string>  shaders;
    std::vector<std::uint8_t> gla;   // the GLA as written; empty if none
    std::string               usesGla;   // model script: "models/.../x.gla"
    // -losedupverts / -smooth: what the vertex merge removed.
    // Per LOD; a line only for a LOD where something was removed.
    std::vector<std::size_t>  deletedDupVerts;
    std::vector<std::size_t>  deletedDupWeights;
};
std::string writeInfoText(const InfoInput& in);

}  // namespace g2
