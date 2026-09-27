// g2/animenums.h - The enum table from anims.h.
//
// Assimilate loads exactly one such file and checks the .car's sequence
// names against it. Which file that is belongs in the settings: Raven's
// table fits Raven's data set, a mod extends it.
//
// Measured on Movie Duels: the OpenJK anims.h knows 1603 enums, the mod's
// animation.cfg lists 1683 sequences - 266 of them (BOTH_MD_*, BOTH_BOLT_*,
// BOTH_BLOCK_*) are the mod's own animations. Treating unknown names as
// errors blocks all work on a mod. They should be reported as warnings,
// not as an abort.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace g2::anim {

struct EnumTable {
    std::string              sourcePath;
    std::vector<std::string> names;   // in declaration order
    std::map<std::string, int> index;

    bool empty() const { return names.empty(); }
    std::size_t size() const { return names.size(); }

    bool contains(const std::string& name) const { return index.count(name) != 0; }

    // -1 if unknown.
    int indexOf(const std::string& name) const {
        const auto it = index.find(name);
        return it == index.end() ? -1 : it->second;
    }
};

// Reads the enums from a C header.
//
// The first `typedef enum` block is recognized; within it, every identifier
// made of uppercase letters, digits and underscores that is followed by a
// comma. Assignments (`FOO = 3,`) are allowed, comments are ignored.
//
// Deliberately lenient: the file is source code from another project and
// changes over time. A parser that fails on an unexpected macro would be
// worse here than one that skips a few lines.
EnumTable parseEnumHeader(const std::string& text, const std::string& sourcePath = {});
EnumTable parseEnumHeaderFile(const std::string& path);

}  // namespace g2::anim
