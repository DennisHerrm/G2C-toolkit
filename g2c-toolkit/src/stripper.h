// src/stripper.h - Carcass's triangle order (internal, shared by GLM and MDR).

#pragma once

#include <array>
#include <map>
#include <stdexcept>
#include <vector>

namespace g2 {

// --- Triangle strips: q3data's OrderMesh, as Carcass runs it (0x445030) ---------
//
// It only reorders triangles and rotates their corners; vertices keep their
// order. The trial calls rotate corners in place, and that is part of the
// result.
class Stripper {
public:
    explicit Stripper(std::vector<std::array<int, 3>> mesh) : mesh_(std::move(mesh)), used_(mesh_.size(), 0) {
        int maxV = 0;
        for (const auto& t : mesh_)
            for (int k : t) maxV = std::max(maxV, k);
        std::vector<std::vector<int>> byVert(static_cast<std::size_t>(maxV) + 1);
        for (std::size_t i = 0; i < mesh_.size(); ++i)
            for (int k : mesh_[i]) {
                auto& l = byVert[static_cast<std::size_t>(k)];
                if (l.empty() || l.back() != static_cast<int>(i)) l.push_back(static_cast<int>(i));
            }
        nb_.resize(mesh_.size());
        for (std::size_t i = 0; i < mesh_.size(); ++i) {
            std::map<int, int> cnt;
            for (int k : mesh_[i])
                for (int t : byVert[static_cast<std::size_t>(k)]) ++cnt[t];
            for (const auto& [t, c] : cnt)
                if (t != static_cast<int>(i) && c >= 2) nb_[i].push_back(t);
        }
    }

    std::vector<std::array<int, 3>> order() {
        std::vector<std::array<int, 3>> out;
        while (out.size() < mesh_.size()) {
            auto st = buildOptimized();
            if (st.empty()) throw std::runtime_error("Dreiecksreihenfolge: kein Fortschritt");
            out.insert(out.end(), st.begin(), st.end());
        }
        return out;
    }

private:
    int findNext(int tri, int orient, bool odd) {
        const auto& m = mesh_[static_cast<std::size_t>(tri)];
        const int cur[3] = {m[static_cast<std::size_t>(orient % 3)], m[static_cast<std::size_t>((1 + orient) % 3)],
                            m[static_cast<std::size_t>((2 + orient) % 3)]};
        const int ra = odd ? cur[1] : cur[2];
        const int rb = odd ? cur[2] : cur[0];
        for (int t : nb_[static_cast<std::size_t>(tri)]) {
            if (t == tri || used_[static_cast<std::size_t>(t)]) continue;
            auto& mt = mesh_[static_cast<std::size_t>(t)];
            for (int side = 0; side < 3; ++side)
                if (ra == mt[static_cast<std::size_t>((side + 1) % 3)] && rb == mt[static_cast<std::size_t>(side)]) {
                    const std::array<int, 3> o = mt;
                    if (side == 1) mt = {o[1], o[2], o[0]};
                    else if (side == 2) mt = {o[2], o[0], o[1]};
                    return t;
                }
        }
        return -1;
    }

    std::vector<std::array<int, 3>> stripLength(int tri, int orient, int fill) {
        const auto& m = mesh_[static_cast<std::size_t>(tri)];
        std::vector<std::array<int, 3>> strip{{m[static_cast<std::size_t>(orient % 3)],
                                               m[static_cast<std::size_t>((orient + 1) % 3)],
                                               m[static_cast<std::size_t>((orient + 2) % 3)]}};
        used_[static_cast<std::size_t>(tri)] = fill;
        bool odd = true;
        int nxt = findNext(tri, orient, odd);
        while (nxt != -1) {
            used_[static_cast<std::size_t>(nxt)] = fill;
            odd = !odd;
            strip.push_back(mesh_[static_cast<std::size_t>(nxt)]);
            nxt = findNext(nxt, 0, odd);
        }
        return strip;
    }

    std::vector<std::array<int, 3>> buildOptimized() {
        std::vector<int> seeds[4];
        const int n = static_cast<int>(mesh_.size());
        for (int i = 0; i < 4; ++i)
            for (int t = 0; t < n; ++t) {
                if (used_[static_cast<std::size_t>(t)]) continue;
                int matched = 0;
                for (int o = 0; o < 3; ++o)
                    if (findNext(t, o, true) != -1) ++matched;
                if (matched == i) {
                    seeds[i].push_back(t);
                    if (seeds[i].size() == 16) break;
                }
            }
        int bestT = -1, bestO = -1;
        std::size_t bestL = 0;
        for (int i = 0; i < 4; ++i) {
            for (int t : seeds[i])
                for (int o = 0; o < 3; ++o) {
                    const std::size_t len = stripLength(t, o, 2).size();
                    if (len > bestL) {
                        bestT = t;
                        bestL = len;
                        bestO = o;
                    }
                    for (auto& u : used_)
                        if (u == 2) u = 0;
                }
            if (bestT != -1) break;
        }
        if (bestT == -1) return {};
        return stripLength(bestT, bestO, 1);
    }

    std::vector<std::array<int, 3>> mesh_;
    std::vector<int>                used_;
    std::vector<std::vector<int>>   nb_;
};

}  // namespace g2
