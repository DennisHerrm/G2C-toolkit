// g2/skelbuild.h - A new skeleton from the source files, as Carcass builds it.
//
// Carcass does NOT take the skeleton from root.xsi. It collects the bones from
// the ANIMATION files ($aseanimgrab, in .car order) and uses the mesh source
// only to decide which of them survive: those that carry a vertex weight.
// Measured against 24 Carcass builds (re/skel/SPEC.md):
//
//   order    first appearance over the grabbed files (pre-order per file)
//   parent   from the last file that has the bone; with $pcj entries the
//            nearest ancestor in the PCJ list (Raven's re-parenting)
//   kept     bone 0, weighted bones, "_always_" bones, Motion with
//            $keepmotion, bones of an $aseanimref_gla reference
//   dropped  everything else; its children move up to its parent
//   name     "_always_" cut out (flag ALWAYSXFORM), one trailing "_" removed
//   base     BASEPOSE- of the last file, in GLA space
//
// Carcass's bugs are not copied: bone names are matched case-insensitively
// (Carcass silently moved fingers to another bone), a missing BASEPOSE is
// composed from the SRT chain instead of taking a LOCAL transform as the
// absolute pose, a reference is matched by its stripped name and its order
// is kept, and differing base poses or parents are reported instead of
// aborting or happening silently.

#pragma once

#include "g2/model.h"
#include "g2/xsi_anim.h"

#include <set>
#include <string>
#include <vector>

namespace g2 {

struct SkeletonBuildOptions {
    std::string name;            // GLA name (-makeskel, verbatim)
    float       scale = 1.0f;    // $scale; a reference's header scale wins
    bool        keepMotion = false;
    // $pcj entries, any case. "$flatten" alone hangs every bone under the
    // topmost node (-flatten on the command line adds it).
    std::vector<std::string> pcj;
    // For each pcj entry: from which grabbed file on it counts (the number of
    // $aseanimgrab lines before the $pcj line). Carcass applies the list
    // while it reads each file, so $pcj lines AFTER the grabs - Raven's own
    // layout - change nothing. Empty = all from the start.
    std::vector<std::size_t> pcjFrom;

    // $aseanimref_gla: its bones are kept even without weights, and the
    // result has its bone order. refBeforeGrab = how many $aseanimgrab
    // lines came before the reference line (0 = before all of them): before
    // the grabs it also dictates the parents, as in Carcass.
    const Skeleton* reference = nullptr;
    std::size_t     refBeforeGrab = 0;

    // $bonehiercap <bone>: everything below that bone is cut off, except
    // "_always_" bones (their children go too). The mesh weights of the cut
    // bones end up on the nearest remaining ancestor (see xsi_mesh).
    std::vector<std::string> caps;

    // For an MDR: the Motion bone is an ordinary bone there (weights on it
    // are allowed).
    bool mdr = false;
};

struct SkeletonBuildResult {
    Skeleton                 skeleton;
    std::vector<std::string> warnings;
    // Per cap that removed something: the cap bone and the removed bones in
    // skeleton order - the content of <name>.bonecap.
    std::vector<std::pair<std::string, std::vector<std::string>>> capped;
};

// <name>.bonecap: one line per cap, "<cap> <removed> <removed> ... " with
// every name followed by one blank, caps sorted case-insensitively, CRLF -
// byte-exact as Carcass writes it.
std::string writeBoneCap(const std::vector<std::pair<std::string, std::vector<std::string>>>& capped);
// Reads a .bonecap: every first word of a line is a cap.
std::vector<std::string> readBoneCapCaps(const std::string& text);

// anims: the grabbed files in .car order. weighted: the envelope deformers
// with a weight > 0 in the mesh source (xsi::weightedDeformers). Throws with
// a clear message where Carcass aborts for a real reason (a weighted bone no
// animation has, a node twice in one file, a file without the root bone,
// Motion used by the mesh, bones that bone 0 cannot reach, two bones with the
// same GLA name).
SkeletonBuildResult buildSkeleton(const std::vector<const xsi::AnimFile*>& anims,
                                  const std::vector<std::string>& weighted,
                                  const SkeletonBuildOptions& opt);

// Puts the bones of `built` into the order of `existing` if both have
// exactly the same bones (by name, case-insensitive). A GLM stores bone
// INDICES, so a rebuilt skeleton in a different order would break every
// model made for the old one. Returns false (and leaves `built` alone) if
// the bone sets differ.
bool keepBoneOrder(Skeleton& built, const Skeleton& existing);

}  // namespace g2
