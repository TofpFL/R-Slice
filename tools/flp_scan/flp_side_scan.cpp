// Diagnostic: per-channel side breakdown of an FLP, playlist-aware.
// Usage: flp_side_scan <file.flp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "flp_import.hpp"

using namespace gh;

static bool sideName(const std::string& nm, int& side) {
    std::string low;
    for (char c : nm) low += char(std::tolower((unsigned char)c));
    static const char* kBase[] = {"opponent", "player", "opp"};
    static const int kSide[] = {0, 1, 0};
    for (size_t b = 0; b < 3; ++b) {
        const std::string base = kBase[b];
        if (low == base) { side = kSide[b]; return true; }
        if (low.size() <= base.size() || low.compare(0, base.size(), base) != 0) continue;
        std::string rest = low.substr(base.size());
        const size_t a = rest.find_first_not_of(' '), z = rest.find_last_not_of(' ');
        if (a == std::string::npos) continue;
        rest = rest.substr(a, z - a + 1);
        if (rest.empty() ||
            !std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit((unsigned char)c); }))
            continue;
        const int idx = std::atoi(rest.c_str());
        if (idx >= 1 && idx <= 50) { side = kSide[b]; return true; }
    }
    return false;
}

static bool camName(const std::string& nm) {
    std::string low;
    for (char c : nm) low += char(std::tolower((unsigned char)c));
    const size_t a = low.find_first_not_of(' '), z = low.find_last_not_of(' ');
    if (a == std::string::npos) return false;
    low = low.substr(a, z - a + 1);
    return low == "cam events" || low == "camevents" || low == "cam" || low == "camera";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: flp_side_scan <file.flp>\n"); return 2; }
    FlpProject p;
    std::string err;
    if (!readFlpFile(argv[1], p, err)) { std::fprintf(stderr, "read failed: %s\n", err.c_str()); return 1; }

    std::printf("title=%s bpm=%.2f ppqn=%d sig=%d/%d chartChannel=%d patterns=%zu playlist=%zu\n",
                p.title.c_str(), p.bpm, p.ppqn, p.timeSigNum, p.timeSigDen, p.chartChannel,
                p.patterns.size(), p.playlist.size());

    // channel id -> resolved side (-1 unnamed, -2 cam, -3 other)
    std::map<uint32_t, int> chSide;
    std::map<uint32_t, std::string> chLabel;
    for (const auto& [ch, names] : p.channelNames) {
        std::string all;
        for (const std::string& nm : names) {
            if (!all.empty()) all += " | ";
            all += nm;
            int s = -1;
            if (sideName(nm, s)) chSide[ch] = s;
            if (camName(nm)) chSide[ch] = -2;
        }
        chLabel[ch] = all;
    }
    std::printf("\n== channels ==\n");
    for (const auto& [ch, label] : chLabel) {
        int s = chSide.count(ch) ? chSide[ch] : -3;
        const char* tag = s == 0 ? "OPP" : s == 1 ? "PLAYER" : s == -2 ? "CAM" : s == -3 ? "other" : "?";
        std::printf("  ch %-6u %-4s %s\n", ch, tag, label.c_str());
    }
    if (chSide.empty()) std::printf("  (no channel names parsed)\n");

    // per-pattern note counts by resolved side (named side, else chartChannel -> pattern vote skipped here)
    std::printf("\n== patterns ==\n");
    for (const FlpPattern& pat : p.patterns) {
        if (pat.notes.empty()) continue;
        uint64_t opp = 0, player = 0, chart = 0, cam = 0, other = 0;
        std::map<uint32_t, uint64_t> byCh;
        for (const FlpNote& n : pat.notes) {
            ++byCh[n.channel];
            auto it = chSide.find(n.channel);
            const int s = it != chSide.end() ? it->second : -3;
            if (s == 0) ++opp;
            else if (s == 1) ++player;
            else if (s == -2) ++cam;
            else if (n.channel == uint32_t(p.chartChannel)) ++chart;
            else ++other;
        }
        std::printf("  pat %-4d notes=%-10zu opp=%-10llu player=%-10llu chartCh=%-10llu cam=%-8llu other=%-10llu  ch:",
                    pat.number, pat.notes.size(),
                    (unsigned long long)opp, (unsigned long long)player,
                    (unsigned long long)chart, (unsigned long long)cam, (unsigned long long)other);
        for (const auto& [ch, c] : byCh) std::printf(" %u:%llu", ch, (unsigned long long)c);
        std::printf("\n");
    }

    // Cam Events timeline: pitch 53/54/57/58 with times (ticks -> beats approx via ppqn)
    std::printf("\n== cam events ==\n");
    int camCount = 0;
    for (const FlpPattern& pat : p.patterns) {
        for (const FlpNote& n : pat.notes) {
            auto it = chSide.find(n.channel);
            if (it == chSide.end() || it->second != -2) continue;
            if (n.pitch < 50 || n.pitch > 60) continue;
            if (++camCount <= 400)
                std::printf("  pat %-4d t=%-8u pitch=%-3u (%.1f beats)\n", pat.number, n.time, n.pitch,
                            double(n.time) / p.ppqn);
        }
    }
    std::printf("  (%d cam markers total)\n", camCount);

    // playlist: per pattern, clip count and time span
    std::printf("\n== playlist per pattern ==\n");
    std::map<int, std::pair<uint64_t, uint64_t>> use;  // count, span ticks
    for (const FlpPlaylistItem& it : p.playlist) {
        if (it.muted || it.pattern <= 0) continue;
        auto& u = use[it.pattern];
        ++u.first;
        u.second = std::max<uint64_t>(u.second, uint64_t(it.startTick) + it.lengthTicks);
    }
    for (const auto& [pat, u] : use)
        std::printf("  pat %-4d clips=%-5llu span=%.1f beats\n", pat,
                    (unsigned long long)u.first, double(u.second) / p.ppqn);

    // overlap test: do opp-side and player-side notes share timestamps?
    std::printf("\n== side overlap per pattern ==\n");
    for (const FlpPattern& pat : p.patterns) {
        std::set<uint64_t> oppT, playerT;
        for (const FlpNote& n : pat.notes) {
            auto it = chSide.find(n.channel);
            const int s = it != chSide.end() ? it->second : (n.channel == uint32_t(p.chartChannel) ? 9 : -3);
            const uint64_t slot = uint64_t(n.time) / 24;
            if (s == 0) oppT.insert(slot);
            else if (s == 1) playerT.insert(slot);
        }
        if (oppT.empty() || playerT.empty()) continue;
        size_t both = 0;
        for (uint64_t t : oppT) if (playerT.count(t)) ++both;
        std::printf("  pat %-4d oppSlots=%-9zu playerSlots=%-9zu shared=%-9zu (%.1f%% of opp slots)\n",
                    pat.number, oppT.size(), playerT.size(), both,
                    100.0 * both / oppT.size());
    }

    // content comparison: "cmp A B" as argv[2], argv[3]
    if (argc > 3 && std::string(argv[2]) == "cmp") {
        const int na = std::atoi(argv[3]), nb = argc > 4 ? std::atoi(argv[4]) : -1;
        auto notesOf = [&](int num) {
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> v;
            for (const FlpPattern& pat : p.patterns)
                if (pat.number == num)
                    for (const FlpNote& n : pat.notes)
                        v.emplace_back(n.time, n.pitch, n.duration, uint32_t(n.velocity), uint32_t(n.flags), uint32_t(n.channel));
            std::sort(v.begin(), v.end());
            return v;
        };
        int other = -1;
        for (int i = 4; i < argc; ++i) other = std::atoi(argv[i]);
        auto va = notesOf(na), vb = notesOf(other > 0 ? other : na + 1);
        if (other <= 0) vb = notesOf(nb);
        size_t same = 0, i = 0, j = 0, velDiff = 0, flagDiff = 0;
        while (i < va.size() && j < vb.size()) {
            // compare ignoring channel (last field)
            auto ka = std::make_tuple(std::get<0>(va[i]), std::get<1>(va[i]), std::get<2>(va[i]),
                                      std::get<3>(va[i]), std::get<4>(va[i]));
            auto kb = std::make_tuple(std::get<0>(vb[j]), std::get<1>(vb[j]), std::get<2>(vb[j]),
                                      std::get<3>(vb[j]), std::get<4>(vb[j]));
            if (ka == kb) { ++same; ++i; ++j; }
            else {
                if (std::get<0>(va[i]) == std::get<0>(vb[j]) && std::get<1>(va[i]) == std::get<1>(vb[j]) &&
                    std::get<2>(va[i]) == std::get<2>(vb[j])) {
                    if (std::get<3>(va[i]) != std::get<3>(vb[j])) ++velDiff;
                    if (std::get<4>(va[i]) != std::get<4>(vb[j])) ++flagDiff;
                }
                if (va[i] < vb[j]) ++i; else ++j;
            }
        }
        std::printf("cmp pat %d vs %d: %zu vs %zu notes, %zu fully identical (t,p,d,vel,flags)\n",
                    na, other > 0 ? other : nb, va.size(), vb.size(), same);
        std::printf("  same-time-pitch-dur but vel differs: %zu, flags differ: %zu\n", velDiff, flagDiff);
        return 0;
    }

    // pattern length audit: declared lengthTicks vs last note end
    std::printf("\n== pattern length audit ==\n");
    const uint64_t barTicks = p.timeSigDen > 0
        ? uint64_t(p.timeSigNum) * uint64_t(p.ppqn) * 4 / uint64_t(p.timeSigDen) : 0;
    for (const FlpPattern& pat : p.patterns) {
        if (pat.notes.empty()) continue;
        uint64_t maxEnd = 0;
        for (const FlpNote& n : pat.notes) maxEnd = std::max<uint64_t>(maxEnd, uint64_t(n.time) + uint64_t(n.duration));
        const uint64_t rounded = barTicks ? (maxEnd + barTicks - 1) / barTicks * barTicks : maxEnd;
        std::printf("  pat %-4d lenTicks=%-8u (%5.1f beats)  contentEnd=%-8llu (%5.1f beats)  rounded=%llu  overhang=%s\n",
                    pat.number, pat.lengthTicks, double(pat.lengthTicks) / p.ppqn,
                    (unsigned long long)maxEnd, double(maxEnd) / p.ppqn,
                    (unsigned long long)rounded,
                    pat.lengthTicks > rounded ? "YES" : "no");
    }

    // clip dump: every clip tagged with the sides its pattern contains
    {
        std::map<int, std::string> patSides;
        for (const FlpPattern& pat : p.patterns) {
            bool o = false, pl = false;
            for (const FlpNote& n : pat.notes) {
                auto it = chSide.find(n.channel);
                const int s = it != chSide.end() ? it->second : -3;
                if (s == 0) o = true;
                else if (s == 1) pl = true;
            }
            patSides[pat.number] = std::string(o ? "O" : "-") + (pl ? "P" : "-");
        }
        std::vector<const FlpPlaylistItem*> clips;
        for (const FlpPlaylistItem& it : p.playlist)
            if (!it.muted && it.pattern > 0) clips.push_back(&it);
        std::sort(clips.begin(), clips.end(), [](const FlpPlaylistItem* a, const FlpPlaylistItem* b) {
            return a->startTick < b->startTick;
        });
        std::printf("\n== all clips (sides=pattern content) ==\n");
        for (const FlpPlaylistItem* it : clips)
            std::printf("  pat %-4d %-2s trk=%-3d %8.2f-%8.2f beats  off=%lld len=%u fl=%04x\n", it->pattern,
                        patSides[it->pattern].c_str(), it->track,
                        double(it->startTick) / p.ppqn,
                        double(it->startTick + it->lengthTicks) / p.ppqn,
                        (long long)it->offsetTicks, it->lengthTicks, unsigned(it->flags));
    }
    return 0;
}
