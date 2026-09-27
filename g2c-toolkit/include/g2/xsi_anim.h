// g2/xsi_anim.h - Animations from dotXSI against an existing skeleton.
//
// "Reference GLA" variant: the skeleton is NOT derived from the dotXSI but
// taken from an existing GLA. Reason: the bone selection cannot be derived
// from the XSI files.
//
// In the available Raven assets, the cascade looks like this:
//
//     191 bones in the root.xsi rig
//     102 of them animated in the animation files
//      66 of them with SI_Envelope, i.e. they deform geometry
//      53 in the finished GLA
//
// Dropped between 66 and 53 were, among others, the IK effectors
// (eff, eff1, larm_eff, lhand_tag_eff), the third finger joint in each case
// (l_d1_j3, l_d2_j3, l_d4_j3), ltarsal and ltlip1 - Raven had to lower the
// bone count from Jedi Outcast to Jedi Academy because of the Xbox.
// This selection is not in any of the source files; it comes from a
// .bonecap file ($bonehiercap) or else from the reference GLA.
//
// For the most common case - adding animations to an existing _humanoid -
// this is correct anyway, because the skeleton MUST stay unchanged. Any
// deviation would break all existing models.

#pragma once

#include "g2/compress.h"
#include "g2/model.h"
#include "g2/xsi.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace g2::xsi {

// An animatable node from the dotXSI hierarchy.
struct AnimNode {
    std::string name;        // bone name, i.e. the part after the last dot
    int         parent = -1; // index into AnimFile::nodes, -1 = root

    // Keyframes per channel. Channel names as in the file:
    // ROTATION-X/Y/Z, TRANSLATION-X/Y/Z, SCALING-X/Y/Z
    std::map<std::string, std::map<int, float>> channels;

    // Static local transform from SI_Transform SRT-<name>:
    // scaling XYZ, rotation XYZ in degrees, translation XYZ.
    //
    // This is a bone's rest value. FCurves override it per channel; where no
    // FCurve exists, it still applies. In Raven's animation files every bone
    // has FCurves for all channels it uses, so the block is never needed
    // there. In root.xsi, on the other hand, only 126 of 276 models have
    // FCurves - the remaining 150 depend on SRT alone.
    std::array<float, 9> srt{{1, 1, 1, 0, 0, 0, 0, 0, 0}};
    bool                 hasSrt = false;

    bool animated() const { return !channels.empty() || hasSrt; }
};

struct AnimFile {
    std::string           sourcePath;
    std::vector<AnimNode> nodes;
    int                   firstFrame = 0;
    int                   lastFrame = 0;

    // From SI_Scene: { "FRAMES", start, end, framerate }
    //
    // This is the authoritative source for the frame range AND playback rate.
    // If a .car line gives no -framespeed, Carcass writes exactly this rate
    // into animation.cfg. Checked against three real files:
    // torso_handsignal2 has 0..72 at rate 20 and appears in Raven's
    // animation.cfg with frameCount 73 and frameSpeed 20.
    float frameRate = 0.0f;
    bool  hasScene = false;

    // Frame range according to SI_Scene. The build uses the key range
    // (firstFrame..lastFrame); if the two differ, the file was exported
    // incorrectly. Carcass then aborts with "Header # frames = N, but I read
    // in M!!", g2c builds with the keys and warns.
    int sceneFirst = 0;
    int sceneLast = 0;
    bool sceneRangeDiffers() const {
        return hasScene && (sceneFirst != firstFrame || sceneLast != lastFrame);
    }

    int frameCount() const { return lastFrame - firstFrame + 1; }

    const AnimNode* find(const std::string& bone) const;
    int             indexOf(const std::string& bone) const;

    // Local matrix of a node in a frame.
    //
    // Important: SI_FCurve values are LOCAL, i.e. relative to the parent
    // model - unlike SI_Transform BASEPOSE-*, which is absolute. Assuming
    // "absolute", nothing fits.
    Mat3x4 localMatrix(int node, int frame) const;

    // World poses of all nodes in a frame, via the XSI hierarchy.
    std::vector<Mat3x4> worldMatrices(int frame) const;
};

// Reads the hierarchy and FCurves from a parsed dotXSI document.
AnimFile loadAnimation(const Document& doc, const std::string& sourcePath = {});
AnimFile loadAnimationFile(const std::string& path);

// --- Evaluation against a reference skeleton -------------------------------

struct EvalOptions {
    // $scale from the .car. Must match the value the reference GLA was built
    // with, otherwise the base poses don't fit.
    float scale = 1.0f;

    // -origin from the .car. Applied as a negative translation on the root
    // bone; the real _humanoid.gla has (0,0,-24) at model_root, matching
    // "-origin 0 0 24".
    std::optional<std::array<float, 3>> origin;

    // Bones of the reference skeleton that are missing from the animation
    // file stay in their rest pose. If true, each missing bone is reported
    // once.
    bool warnMissingBones = true;

    // Renames: the key is the bone name in the reference GLA, the value the
    // one in the dotXSI. Needed because Raven renamed bones between builds -
    // in the available _humanoid.gla the bone is called "face", but in
    // root.xsi and the animation files it is "face_always_". Without the
    // mapping, the bone and all eight face bones below it stay in rest pose.
    std::map<std::string, std::string> aliases;

    // Root motion.
    //
    // Carcass puts a LINEAR RAMP on the root bone that, over the sequence,
    // cancels out exactly the total displacement of the Motion bone - as a
    // counter-movement, i.e. with the opposite sign of the normal position
    // conversion. The engine then adds the displacement itself.
    //
    // Important: it is a ramp, NOT the actual curve of the Motion bone.
    // Checked on BOTH_DEATH17 and BOTH_SIT2TOSTAND5 - Motion fluctuates
    // strongly there, yet the root bone moves in a dead-straight line.
    //
    //     ramp(f) = -scale * C * (W_motion(last) - W_motion(first)) * f/(n-1)
    //
    // Without this, the figure stays on the spot in death and get-up
    // animations instead of moving.
    bool        extractRootMotion = true;
    std::string motionBone = "Motion";
};

struct EvalResult {
    AnimationFrames          frames;

    // Total root displacement in GLA space over the sequence. Divided by the
    // number of steps, this gives the value Carcass writes as "averagevec"
    // into the .frames file - there with the opposite sign, though, because
    // the ramp is a counter-movement.
    float rootMotion[3] = {0.0f, 0.0f, 0.0f};
    std::vector<std::string> missingBones;   // in the skeleton, not in the XSI
    std::vector<std::string> extraBones;     // in the XSI, not in the skeleton
    int                      frameCount = 0;
};

// Converts an animation file into frames for the reference skeleton.
//
// The formula follows the engine. tr_ghoul2.cpp evaluates
//     W(bone) = W(parent) * A(bone)
// and the skinning matrix is X * B^-1. Rearranged:
//     A(b) = B(parent) * X(parent)^-1 * X(b) * B(b)^-1
//
// B is the basePoseMat from the reference GLA, X the converted world pose
// from the FCurves. Checked against BOTH_attack10.xsi and the real
// _humanoid.gla: 840 comparisons, max. rotation deviation 1.77e-4 with a
// quantization step size of 6.10e-5.
EvalResult evaluate(const Skeleton& reference, const AnimFile& anim, const EvalOptions& opt = {});

// Concatenates multiple animations, as $aseanimgrab does.
struct ConcatResult {
    AnimationFrames frames;
    struct Entry {
        std::string name;         // sequence name
        int         targetFrame;  // start frame in the combined animation
        int         frameCount;
        std::string sourceFile;
    };
    std::vector<Entry>       sequences;
    std::vector<std::string> warnings;
};

ConcatResult concatenate(const Skeleton& reference,
                         const std::vector<std::pair<std::string, AnimFile>>& named,
                         const EvalOptions& opt = {});

}  // namespace g2::xsi
