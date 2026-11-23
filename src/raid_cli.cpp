// raid_engine: standalone RAID Engine command line tool (no raylib).
//
//   raid_engine convert <chart|flp|fsc> <out.raid>   encode a chart to .raid
//   raid_engine info <file.raid>                     header + encoder stats
//   raid_engine verify <file.raid>                   decode every chunk, count events
//   raid_engine window <file.raid> <startMs> <endMs> events in a time window
//   raid_engine meta <file.raid>                     the chart metadata (JSON)
//   raid_engine patterns <file.raid> [startMs] [endMs] op-stream log: which
//                                                    pattern plays when
//   raid_engine flppatterns <file.flp>               per-pattern charted-note
//                                                    census (discrepancy detection)
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "flp_import.hpp"
#include "miniz.h"
#include "raid.hpp"
#include "raid_chart.hpp"
#include "raid_doc.hpp"
#include "session.hpp"
#include "rchr.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
// Peak resident set size in bytes — the honest "how much RAM did this
// actually use" number for memory benchmarks (0 when unsupported).
uint64_t peakRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
    return 0;
#elif defined(__linux__)
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) return uint64_t(ru.ru_maxrss) * 1024;
    return 0;
#else
    return 0;
#endif
}
uint64_t curRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)))
        return pmc.WorkingSetSize;
    return 0;
#elif defined(__linux__)
    return 0;  // Linux has no cheap current-RSS without /proc parsing
#else
    return 0;
#endif
}
}  // namespace

using namespace gh;

// Character name a .rchr pack provides: the stem of its first
// characters/<name>.json (or data/characters/<name>.json) entry.
static std::string packCharName(const std::string& rchrPath, std::string& err) {
    std::vector<rchr::Entry> ents;
    if (!rchr::list(rchrPath, ents, err)) return "";
    for (const rchr::Entry& e : ents) {
        std::string lp = e.path;
        for (char& ch : lp) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        static const char* kPrefixes[] = {"characters/", "data/characters/"};
        for (const char* pre : kPrefixes) {
            const size_t pl = std::strlen(pre);
            if (lp.rfind(pre, 0) == 0 && lp.size() > pl + 5 &&
                lp.compare(lp.size() - 5, 5, ".json") == 0) {
                return e.path.substr(pl, e.path.size() - pl - 5);
            }
        }
    }
    return "";
}

static bool readBinaryFile(const std::string& path, std::vector<char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize(size_t(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, size_t(n), f) == size_t(n);
    std::fclose(f);
    return ok;
}

struct PackPick {
    std::string file;   // .rchr path given on the command line
    std::string name;   // explicit --*-name override, "" = derive from pack
    bool embedBytes = false;  // --*-pack-embed: store bytes, not a reference
};

static int convert(const std::string& in, const std::string& out,
                   const PackPick& opp, const PackPick& player) {
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;

    // Resolve the packs up front: --*-pack records an external .rchr
    // reference in the trailer; --*-pack-embed reads the bytes for
    // embedding. Either way the character name is derived for the meta.
    std::vector<raid::EmbeddedAsset> packs;
    std::string oppName = opp.name, playerName = player.name;
    auto loadPack = [&](const PackPick& pk, raid::EmbeddedAsset& dst,
                        std::string& charName) -> bool {
        if (pk.file.empty()) return true;
        dst.name = pk.file;
        const size_t slash = dst.name.find_last_of("/\\");
        if (slash != std::string::npos) dst.name = dst.name.substr(slash + 1);
        if (pk.embedBytes) {
            if (!readBinaryFile(pk.file, dst.data)) {
                std::printf("pack load failed: %s\n", pk.file.c_str());
                return false;
            }
        } else {
            dst.reference = true;
            dst.refPath = pk.file;
            for (char& ch : dst.refPath) if (ch == '\\') ch = '/';
        }
        packs.push_back(dst);
        if (charName.empty()) {
            charName = packCharName(pk.file, err);
            if (charName.empty())
                std::printf("  note: %s has no characters/*.json — meta name unchanged\n",
                            pk.file.c_str());
        }
        return true;
    };
    raid::EmbeddedAsset oppPack, playerPack;
    if (!loadPack(opp, oppPack, oppName) || !loadPack(player, playerPack, playerName))
        return 1;

    // Whole .flp/.fsc projects encode directly: patterns are stored once as
    // pattern-table definitions and playlist clips place them on the
    // timeline â€” the file decodes as one contiguous chart. A `#N` suffix
    // still selects a single pattern through the Chart path.
    std::string flpFile;
    int flpPat = -1;
    bool ok;
    if (flpChartPath(in, flpFile, flpPat) && flpPat < 0) {
        FlpProject p;
        const auto tp0 = std::chrono::steady_clock::now();
        if (!readFlpFile(flpFile, p, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "[flp] parsed %zu patterns, %zu playlist items in %.1f s\n",
                     p.patterns.size(), p.playlist.size(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count());
        const auto tp1 = std::chrono::steady_clock::now();
        ok = raid::writeRaidFlp(out, p, opt, &info, err, oppName, playerName);
        std::fprintf(stderr, "[flp] encoded in %.1f s\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - tp1).count());
        if (!ok) std::printf("encode failed: %s\n", err.c_str());
    } else {
        Chart c;
        if (!loadChartFile(in, "", c, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        if (!oppName.empty()) c.opponent = oppName;
        if (!playerName.empty()) c.player = playerName;
        ok = raid::saveChart(out, c, opt, &info, err);
        if (!ok) std::printf("encode failed: %s\n", err.c_str());
    }
    if (!ok) return 1;
    if (!packs.empty() && !raid::embedAssets(out, packs, err)) {
        std::printf("pack embed failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s -> %s\n", in.c_str(), out.c_str());
    std::printf("  logical events : %llu\n", (unsigned long long)info.logicalEvents);
    std::printf("  literal events : %llu (%.1f%%)\n", (unsigned long long)info.literalEvents,
                info.logicalEvents ? 100.0 * double(info.literalEvents) / double(info.logicalEvents) : 0.0);
    std::printf("  unique notes   : %llu in %llu stored events (pattern table)\n",
                (unsigned long long)info.uniqueNotes, (unsigned long long)info.uniqueEvents);
    std::printf("  patterns       : %llu\n", (unsigned long long)info.patternCount);
    std::printf("  chunks         : %llu\n", (unsigned long long)info.chunkCount);
    std::printf("  payload bytes  : %llu raw -> %llu stored (%.2fx)\n",
                (unsigned long long)info.rawBytes, (unsigned long long)info.storedBytes,
                info.storedBytes ? double(info.rawBytes) / double(info.storedBytes) : 0.0);
    if (!oppName.empty()) std::printf("  opponent char  : %s\n", oppName.c_str());
    if (!playerName.empty()) std::printf("  player char    : %s\n", playerName.c_str());
    for (const raid::EmbeddedAsset& p : packs)
        std::printf("  %s: %s (%s)\n", p.reference ? "pack ref" : "embedded pack",
                    p.name.c_str(), p.reference ? p.refPath.c_str() : "");
    return 0;
}

static int info(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const raid::Info& i = rd.info();
    std::printf("%s\n", path.c_str());
    std::printf("  version        : %u\n", i.version);
    std::printf("  logical events : %llu\n", (unsigned long long)i.logicalEvents);
    std::printf("  chunks         : %llu\n", (unsigned long long)i.chunkCount);
    std::printf("  patterns       : %llu\n", (unsigned long long)i.patternCount);
    {
        uint64_t uev = 0, un = 0;
        if (rd.uniqueStats(uev, un, err))
            std::printf("  unique notes   : %llu in %llu stored events (pattern table)\n",
                        (unsigned long long)un, (unsigned long long)uev);
    }
    {
        std::vector<raid::EmbeddedAsset> packs;
        if (rd.embeddedAssets(packs, err)) {
            for (const raid::EmbeddedAsset& p : packs)
                std::printf("  %s  : %s (%s)\n", p.reference ? "pack ref" : "embedded pack",
                            p.name.c_str(), p.reference ? p.refPath.c_str() : "");
        }
    }
    std::printf("  duration       : %.0f ms\n", double(rd.durationUs()) / 1000.0);
    rd.close();
    return 0;
}

// Detects files whose stamped format version is lower than what their
// chunk ops require (v1 stamp hiding op-3/op-4). Prints a warning with the
// exact upgrade command. Silent when the label is correct.
static void warnIfMislabeled(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) return;  // verify() already reported open errors
    const uint32_t stored = rd.info().version;
    uint32_t need = raid::kMinVersion;
    const uint64_t nc = rd.info().chunkCount;
    for (uint64_t i = 0; i < nc; ++i) {
        std::vector<char> p;
        if (!rd.chunkPayloadRaw(size_t(i), p, err)) break;
        need = std::max(need, raid::chunkPayloadVersion(p));
    }
    rd.close();
    if (stored < need)
        std::printf("WARNING: File contains v%u operations but is stamped v%u.\n"
                    "Run: raid_engine upgrade \"%s\"\n",
                    need, stored, path.c_str());
}

static int verify(const std::string& path) {
    Chart c;
    std::string err;
    if (!raid::loadChart(path, c, err)) {
        std::printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %zu notes, title \"%s\", %.0f ms\n", path.c_str(), c.notes.size(),
                c.title.c_str(), c.lengthMs);
    warnIfMislabeled(path);
    return 0;
}

static int window(const std::string& path, const char* a, const char* b) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const int64_t lo = int64_t(std::atof(a) * 1000.0), hi = int64_t(std::atof(b) * 1000.0);
    std::vector<raid::Event> evs;
    if (!rd.range(lo, hi, evs, err)) {
        std::printf("range failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%lld events in [%.0f, %.0f) ms\n", (long long)evs.size(),
                double(lo) / 1000.0, double(hi) / 1000.0);
    for (const raid::Event& e : evs)
        std::printf("  t=%lld.%03d ms lane=%u side=%u kind=%u pitch=%d len=%lld us type=%u stack=%u\n",
                    (long long)(e.timeUs / 1000), int(e.timeUs % 1000), e.lane, e.side, e.kind,
                    e.pitch, (long long)e.lengthUs, e.type, e.stack);
    rd.close();
    return 0;
}

static int meta(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    std::fputs(rd.metaJson().c_str(), stdout);
    std::fputc('\n', stdout);
    rd.close();
    return 0;
}

static int patterns(const std::string& path, const char* a, const char* b) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const int64_t lo = a ? int64_t(std::atof(a) * 1000.0) : 0;
    const int64_t hi = b ? int64_t(std::atof(b) * 1000.0) : INT64_MAX;
    std::vector<raid::Reader::OpSegment> segs;
    if (!rd.opLog(lo, hi, segs, err)) {
        std::printf("op log failed: %s\n", err.c_str());
        return 1;
    }
    uint64_t lits = 0, pats = 0, ppChanges = 0;
    std::set<uint32_t> used;
    int64_t prevPat = -1;  // patternId of the previous pattern segment
    for (const raid::Reader::OpSegment& s : segs) {
        if (s.kind == 2) {
            ++pats;
            used.insert(s.count);
            if (prevPat >= 0 && int64_t(s.count) != prevPat) ++ppChanges;
            prevPat = int64_t(s.count);
            std::printf("  pat#%-6u x%-4u chunk %-3u [%10.1f - %10.1f ms]  pitch%+d lane%+d\n",
                        s.count, s.aux, s.chunk, double(s.startUs) / 1000.0,
                        double(s.endUs) / 1000.0, s.pitchDelta, s.laneDelta);
        } else {
            ++lits;
            prevPat = -1;  // a literal breaks pattern-to-pattern adjacency
            std::printf("  lit      chunk %-3u [%10.1f - %10.1f ms]  %u events\n",
                        s.chunk, double(s.startUs) / 1000.0,
                        double(s.endUs) / 1000.0, s.count);
        }
    }
    std::printf("  segments: %zu (%llu literal runs, %llu pattern instances)\n",
                segs.size(), (unsigned long long)lits, (unsigned long long)pats);
    std::printf("  pattern->pattern changes: %llu   distinct patterns: %zu\n",
                (unsigned long long)ppChanges, used.size());
    rd.close();
    return 0;
}

// flppatterns <file.flp>: per-pattern charted-note census for discrepancy
// detection. For every FL pattern it counts the notes flpToChart would
// emit, split into in-bounds (inside the pattern's playlist loop length)
// and artifacts (FL keeps notes past the loop point; playlist clips never
// play them, so raid conversion drops them too).
static int flppatterns(const std::string& path) {
    FlpProject p;
    std::string err;
    if (!readFlpFile(path, p, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    const double bpm = (p.bpm > 0 ? p.bpm : 120.0) * flpBpmScale(p);
    const double msPerTick = 60000.0 / (bpm * p.ppqn);
    // Loop length per pattern in ticks - same rule writeRaidFlp uses:
    // declared length wins unless it overhangs the content; the content
    // extent is the last note onset rounded up to a whole bar.
    const uint64_t barTicks = p.timeSigDen > 0
        ? uint64_t(p.timeSigNum) * uint64_t(p.ppqn) * 4 / uint64_t(p.timeSigDen)
        : 0;
    std::set<int> used;
    for (const FlpPlaylistItem& it : p.playlist)
        if (!it.muted && it.pattern > 0) used.insert(it.pattern);

    std::printf("flppatterns: %s\n", path.c_str());
    std::printf("  ppqn=%d bpm=%.2f patterns=%zu playlist=%zu\n",
                p.ppqn, bpm, p.patterns.size(), p.playlist.size());
    // raw = notes before finalize() merges same-lane sub-ms notes into
    // stacks; charted/inbounds/artifacts count merged events; rawin/rawart
    // are the stack-weighted split of the raw notes at the loop boundary.
    uint64_t tRaw = 0, tCharted = 0, tArtifacts = 0, tPool = 0;
    uint64_t tRawArt = 0, tRawPool = 0, tRawPoolUsed = 0, tPoolUsed = 0;
    int totPatterns = 0;
    for (const FlpPattern& fp : p.patterns) {
        Chart c;
        std::string cerr;
        if (!flpToChart(p, fp.number, c, cerr)) {
            std::printf("pat %4d \"%s\": no chart notes\n",
                        fp.number, fp.name.c_str());
            continue;
        }
        ++totPatterns;
        uint64_t lastStart = 0, maxEnd = 0;
        for (const FlpNote& n : fp.notes) {
            lastStart = std::max<uint64_t>(lastStart, uint64_t(n.time));
            maxEnd = std::max<uint64_t>(maxEnd, uint64_t(n.time) + uint64_t(n.duration));
        }
        uint64_t contentTicks = 0;
        if (barTicks)
            contentTicks = lastStart ? (lastStart + barTicks - 1) / barTicks * barTicks : barTicks;
        else
            contentTicks = maxEnd;
        uint64_t ticks = fp.lengthTicks;
        if (!ticks || (contentTicks && ticks > contentTicks)) ticks = contentTicks;
        const double periodMs = double(ticks) * msPerTick;
        uint64_t inBounds = 0, artifacts = 0, rawIn = 0, rawArt = 0;
        for (const Note& n : c.notes) {
            const bool out = periodMs > 0 && n.time >= periodMs;
            (out ? artifacts : inBounds)++;
            (out ? rawArt : rawIn) += n.stack ? n.stack : 1;
        }
        const bool isUsed = used.count(fp.number) != 0;
        std::printf("pat %4d \"%s\": raw=%llu charted=%llu inbounds=%llu artifacts=%llu rawin=%llu rawart=%llu len=%llut used=%d\n",
                    fp.number, fp.name.c_str(),
                    (unsigned long long)c.rawNoteCount,
                    (unsigned long long)c.notes.size(),
                    (unsigned long long)inBounds, (unsigned long long)artifacts,
                    (unsigned long long)rawIn, (unsigned long long)rawArt,
                    (unsigned long long)ticks, isUsed ? 1 : 0);
        tRaw += c.rawNoteCount;
        tCharted += c.notes.size();
        tArtifacts += artifacts;
        tPool += inBounds;
        tRawArt += rawArt;
        tRawPool += rawIn;
        if (isUsed) { tPoolUsed += inBounds; tRawPoolUsed += rawIn; }
    }
    std::printf("[flp-total] patterns=%d raw=%llu charted=%llu artifacts=%llu pool=%llu poolused=%llu rawart=%llu rawpool=%llu rawpoolused=%llu\n",
                totPatterns, (unsigned long long)tRaw,
                (unsigned long long)tCharted, (unsigned long long)tArtifacts,
                (unsigned long long)tPool, (unsigned long long)tPoolUsed,
                (unsigned long long)tRawArt, (unsigned long long)tRawPool,
                (unsigned long long)tRawPoolUsed);
    return 0;
}

// Replays the game's feed loop: 60 fps song clock, 4 s pump window,
// 16 s async prefetch, per-10 s event buckets. Catches stream failures
// and dropped lanes that `verify` (one sequential decode) cannot see.
static int streamtest(const std::string& path) {
    raid::ChartStream cs;
    Chart meta;
    std::string err;
    if (!cs.open(path, meta, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %.0f ms, keys %d, P%llu O%llu notes\n", path.c_str(), meta.lengthMs,
                meta.keys, (unsigned long long)meta.playerNotes,
                (unsigned long long)meta.opponentNotes);
    int64_t fedUs = std::numeric_limits<int64_t>::min() / 4, prefetchUs = fedUs;
    double songMs = -300.0;
    const double stepMs = 1000.0 / 60.0;
    uint64_t total = 0, badLane = 0, stackSum = 0;
    std::vector<uint64_t> bucket(48, 0);
    while (songMs < meta.lengthMs + 5000.0) {
        songMs += stepMs;
        const int64_t endUs = int64_t(std::max(0.0, songMs + 4000.0) * 1000.0);
        if (endUs > fedUs) {
            std::vector<raid::Event> evs;
            std::vector<uint64_t> idx;
            if (!cs.pump(fedUs, endUs, evs, idx, err)) {
                std::printf("PUMP FAIL @ %.0f ms: %s\n", songMs, err.c_str());
                return 1;
            }
            for (const raid::Event& e : evs) {
                ++total;
                if (e.lane >= meta.keys) ++badLane;
                stackSum += e.stack ? e.stack : 1;
                const size_t b = std::min<size_t>(47, size_t(std::max<int64_t>(0, e.timeUs / 10000000)));
                ++bucket[b];
            }
            fedUs = endUs;
        }
        if (endUs >= prefetchUs) {
            cs.prefetch(endUs, endUs + int64_t(16000.0 * 1000.0));
            prefetchUs = endUs + int64_t(8000.0 * 1000.0);
        }
    }
    cs.close();
    std::printf("fed %llu events, %llu stacked notes (%llu out-of-range lanes)\n",
                (unsigned long long)total, (unsigned long long)stackSum, (unsigned long long)badLane);
    std::printf("peak RSS %.1f MB, RSS at close %.1f MB\n",
                peakRssBytes() / 1048576.0, curRssBytes() / 1048576.0);
    for (size_t i = 0; i < bucket.size(); ++i)
        if (bucket[i]) std::printf("  %4zu0-%4zus: %llu\n", i, i, (unsigned long long)bucket[i]);
    return 0;
}
// slicebench: A/B the reference decoder (Reader::range, whole-chunk decode
// + LRU) against window-sliced decode (Reader::rangeWindow) through the
// same 60 Hz / 4 s-lookahead feed loop the game uses.
static int slicebench(const std::string& path) {
    raid::Reader probe;
    std::string err;
    if (!probe.open(path, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    const double lengthMs = double(probe.durationUs()) / 1000.0;
    const uint64_t logical = probe.info().logicalEvents;
    probe.close();

    for (int mode = 0; mode < 2; ++mode) {
        raid::Reader rd;
        if (!rd.open(path, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
        int64_t fedUs = std::numeric_limits<int64_t>::min() / 4;
        double songMs = -300.0;
        const double stepMs = 1000.0 / 60.0;
        uint64_t total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (songMs < lengthMs + 5000.0) {
            songMs += stepMs;
            const int64_t endUs = int64_t(std::max(0.0, songMs + 4000.0) * 1000.0);
            if (endUs > fedUs) {
                std::vector<raid::Event> evs;
                std::vector<uint64_t> idx;
                bool ok = mode == 0
                    ? rd.range(fedUs, endUs, evs, err, &idx)
                    : rd.rangeWindow(fedUs, endUs, evs, idx, err);
                if (!ok) { std::printf("decode failed: %s\n", err.c_str()); return 1; }
                total += evs.size();
                fedUs = endUs;
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%-12s %10.1f s  %12llu events  %8.2fM ev/s  peakRSS %.1f MB\n",
                    mode == 0 ? "range" : "rangeWindow", secs,
                    (unsigned long long)total, total / secs / 1e6,
                    peakRssBytes() / 1048576.0);
        rd.close();
    }
    std::printf("%s: %llu logical events, %.0f ms\n", path.c_str(),
                (unsigned long long)logical, lengthMs);
    return 0;
}

// repgen: pattern-instance stress chart. Emits `repeats` copies of an
// m-event motif spaced `periodMs` apart, so the encoder stores one leaf
// pattern plus an instance op with reps ~= repeats. chunkEvents controls
// how many expanded events fit per chunk — crank it toward kMaxChunkExpand
// (4194304) to keep giant instances inside a single chunk.
//   raid_engine repgen <out.raid> <repeats> <motifEvents> <periodMs>
//                   [chunkEvents] [stack] [keys]
static int repgen(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: raid_engine repgen <out.raid> <repeats> <motifEvents>"
                    " <periodMs> [chunkEvents] [stack] [keys]\n");
        return 1;
    }
    const std::string out = argv[2];
    const uint64_t repeats = std::strtoull(argv[3], nullptr, 10);
    const int m = std::atoi(argv[4]);
    const double periodMs = std::atof(argv[5]);
    const uint32_t chunkEvents = argc > 6 ? uint32_t(std::strtoul(argv[6], nullptr, 10)) : 4194304;
    const uint16_t stack = argc > 7 ? uint16_t(std::strtoul(argv[7], nullptr, 10)) : 1;
    const int keys = argc > 8 ? std::atoi(argv[8]) : 4;
    if (m < 2 || m > 64 || repeats == 0 || periodMs <= 0) {
        std::printf("bad params (motif 2..64, repeats>0, periodMs>0)\n");
        return 1;
    }
    const uint64_t total = repeats * uint64_t(m);
    if (total > 64000000ull) {
        std::printf("event count %llu too large (max 64M — RAM bound)\n",
                    (unsigned long long)total);
        return 1;
    }
    const int64_t periodUs = int64_t(periodMs * 1000.0);
    const int64_t localStep = periodUs / m;
    std::vector<raid::Event> evs;
    evs.reserve(size_t(total));
    for (uint64_t r = 0; r < repeats; ++r) {
        const int64_t base = int64_t(r) * periodUs;
        for (int m2 = 0; m2 < m; ++m2) {
            raid::Event e;
            e.timeUs = base + int64_t(m2) * localStep;
            e.lengthUs = 0;
            e.lane = uint8_t(m2 % keys);
            e.side = uint8_t((m2 / keys) % 2);
            e.kind = 0;
            e.stack = stack;
            evs.push_back(e);
        }
    }
    Chart meta;
    meta.title = "repstress";
    meta.difficulty = "Stress";
    meta.keys = keys;
    meta.tempo.push_back({0, 120});
    meta.playerNotes = meta.opponentNotes = 0;
    meta.lengthMs = double(evs.back().timeUs) / 1000.0;
    raid::EncodeOptions opt;
    opt.chunkEvents = chunkEvents;
    raid::Info info;
    std::string err;
    if (!raid::writeRaid(out, evs, raid::chartMetaJson(meta), opt, &info, err)) {
        std::printf("encode failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s: %llu events -> %llu logical, %llu chunks, %llu patterns\n",
                out.c_str(), (unsigned long long)total,
                (unsigned long long)info.logicalEvents,
                (unsigned long long)info.chunkCount,
                (unsigned long long)info.patternCount);
    return 0;
}

// ---- stressgen: hand-authored giant pattern instance -------------------
// Builds a .raid whose single chunk is one kOpPattern op referencing a
// small leaf literal, with an arbitrary repeat count — the shape real
// encoder output takes, but with reps far beyond what pattern matching
// would find in feasible input. Goes through writeRaidDoc so the file is
// indistinguishable from encoder output.
namespace {
struct SBuf {
    std::vector<char> b;
    void u8(uint32_t v) { b.push_back(char(v & 0xFF)); }
    void u16(uint32_t v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void var(uint64_t v) {
        while (v >= 0x80) { u8(uint32_t(v) | 0x80); v >>= 7; }
        u8(uint32_t(v));
    }
    void zig(int64_t v) { var((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void raw(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        b.insert(b.end(), c, c + n);
    }
};
uint32_t scrc(const void* p, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const unsigned char* s = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ s[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
} // namespace

//   raid_engine stressgen <out.raid> <repeats> <motifEvents> <periodMs>
//                         [stack] [keys]
// expanded = repeats*motifEvents must be <= 4194304 (kMaxChunkExpand).
static int stressgen(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: raid_engine stressgen <out.raid> <repeats>"
                    " <motifEvents> <periodMs> [stack] [keys]\n");
        return 1;
    }
    const std::string out = argv[2];
    const uint64_t reps = std::strtoull(argv[3], nullptr, 10);
    const int m = std::atoi(argv[4]);
    const double periodMs = std::atof(argv[5]);
    const uint32_t stack = argc > 6 ? uint32_t(std::strtoul(argv[6], nullptr, 10)) : 1;
    const int keys = argc > 7 ? std::atoi(argv[7]) : 4;
    if (m < 1 || m > 1024 || reps == 0 || periodMs <= 0) {
        std::printf("bad params\n");
        return 1;
    }
    const uint64_t expanded = reps * uint64_t(m);
    if (expanded > 4194304ull) {
        std::printf("expanded %llu exceeds chunk cap 4194304\n",
                    (unsigned long long)expanded);
        return 1;
    }
    const int64_t periodUs = int64_t(periodMs * 1000.0);
    const int64_t localStep = periodUs / m;

    // Pattern 0: one literal-rel op, m events at local times j*localStep.
    SBuf stream;
    stream.u32(1);                 // opCount
    stream.u8(1);                  // kOpLiteralRel
    stream.u32(uint32_t(m));
    stream.var(0);                 // first time delta from timeBase (t0=0)
    for (int j = 1; j < m; ++j) stream.var(uint64_t(localStep));
    for (int j = 0; j < m; ++j) stream.var(0);                    // lengthUs
    for (int j = 0; j < m; ++j) stream.zig(0);                    // pitch deltas (pitch0=60 below)
    for (int j = 0; j < m; ++j) stream.zig(int64_t(j % keys) - 0); // lane deltas vs lane0=0
    for (int j = 0; j < m; ++j) stream.var(uint64_t((j / keys) % 2)); // attr: side
    for (int j = 0; j < m; ++j) stream.var(0);                    // type
    for (int j = 0; j < m; ++j) stream.var(stack);                // stack

    // Pattern section: u32 count + [u64 spanUs, u8 hasPitch, u32 len, bytes] + u32 crc
    SBuf pat;
    pat.u32(1);
    pat.u64(uint64_t(periodUs));   // spanUs = repeat stride
    pat.u8(1);                     // hasPitch
    pat.u32(uint32_t(stream.b.size()));
    pat.raw(stream.b.data(), stream.b.size());
    uint32_t pcrc = scrc(pat.b.data(), pat.b.size());
    pat.u32(pcrc);

    // Chunk payload (sealed): u32 opCount + ops + u64 expanded.
    // op = kOpPattern{pid=0, reps, anchor=0, pitchDelta=60, laneDelta=0}
    SBuf pay;
    pay.u32(1);
    pay.u8(2);                     // kOpPattern
    pay.u32(0);                    // pattern id
    pay.u32(uint32_t(reps));
    pay.zig(0);                    // anchorUs
    pay.zig(60);                   // pitchDelta -> pitchBase 60, pitch0 60 => pitch 60
    pay.zig(0);                    // laneDelta
    pay.u64(expanded);

    raid::DocChunkOut chunk;
    chunk.payload.assign(pay.b.begin(), pay.b.end());
    chunk.firstEvent = 0;
    chunk.firstTimeUs = 0;
    chunk.lastTimeUs = int64_t(reps - 1) * periodUs + int64_t(m - 1) * localStep;
    chunk.expanded = uint32_t(expanded);
    chunk.opCount = 1;
    std::vector<raid::DocChunkOut> chunks{chunk};

    Chart meta;
    meta.title = "stress";
    meta.difficulty = "Stress";
    meta.keys = keys;
    meta.tempo.push_back({0, 120});
    meta.lengthMs = double(chunk.lastTimeUs) / 1000.0;
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    if (!raid::writeRaidDoc(out, pat.b, chunks, expanded, 0, m,
                            uint64_t(m) * stack, raid::chartMetaJson(meta),
                            opt, &info, err)) {
        std::printf("writeRaidDoc failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s: reps=%llu motif=%d period=%.1fms expanded=%llu stack=%u\n",
                out.c_str(), (unsigned long long)reps, m, periodMs,
                (unsigned long long)expanded, stack);
    return 0;
}

// eval: lazy windowed-decode benchmark. Times Reader::rangeWindow over a
// fixed [startMs,endMs) window and reports decode-work counters — the
// whole point is repeatsSkipped vs repeatsIterated. With "verify", also
// runs Reader::range (whole-chunk reference) and diffs events+indices.
//   raid_engine eval <file.raid> <startMs> <endMs> [iters] [verify]
static int evalCmd(const std::string& path, const std::string& startMs,
                   const std::string& endMs, int iters, bool verify) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    const int64_t sUs = int64_t(std::llround(std::atof(startMs.c_str()) * 1000.0));
    const int64_t eUs = int64_t(std::llround(std::atof(endMs.c_str()) * 1000.0));
    if (eUs <= sUs) { std::printf("empty window\n"); return 1; }

    const raid::EvalStats st0 = rd.evalStats();
    std::vector<raid::Event> evs;
    std::vector<uint64_t> idx;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t total = 0;
    for (int i = 0; i < iters; ++i) {
        evs.clear(); idx.clear();
        if (!rd.rangeWindow(sUs, eUs, evs, idx, err)) {
            std::printf("rangeWindow failed: %s\n", err.c_str());
            return 1;
        }
        total += evs.size();
    }
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    const raid::EvalStats st1 = rd.evalStats();

    const uint64_t dLeaf = st1.leafInstances.load() - st0.leafInstances.load();
    const uint64_t dFall = st1.leafFallbacks.load() - st0.leafFallbacks.load();
    const uint64_t dIter = st1.repeatsIterated.load() - st0.repeatsIterated.load();
    const uint64_t dSkip = st1.repeatsSkipped.load() - st0.repeatsSkipped.load();
    const uint64_t dOps = st1.opsDecoded.load() - st0.opsDecoded.load();
    std::printf("eval %s [%lld,%lld)ms x%d: %llu events, %.2f ms total, %.1f ev/ms\n",
                path.c_str(), (long long)sUs / 1000, (long long)eUs / 1000, iters,
                (unsigned long long)(total / iters), secs * 1000.0,
                total / std::max(1e-9, secs * 1000.0));
    std::printf("  leafInstances=%llu fallbacks=%llu opsDecoded=%llu\n",
                (unsigned long long)dLeaf, (unsigned long long)dFall,
                (unsigned long long)dOps);
    std::printf("  repeats: iterated=%llu skipped=%llu (%.1f%% bypassed)\n",
                (unsigned long long)dIter, (unsigned long long)dSkip,
                dIter + dSkip ? 100.0 * dSkip / (dIter + dSkip) : 0.0);

    if (verify) {
        std::vector<raid::Event> ref;
        std::vector<uint64_t> refIdx;
        if (!rd.range(sUs, eUs, ref, err, &refIdx)) {
            std::printf("range failed: %s\n", err.c_str());
            return 1;
        }
        bool same = ref.size() == evs.size() && refIdx == idx;
        if (same) {
            for (size_t i = 0; i < ref.size(); ++i) {
                const raid::Event& a = ref[i];
                const raid::Event& b = evs[i];
                if (a.timeUs != b.timeUs || a.lengthUs != b.lengthUs ||
                    a.pitch != b.pitch || a.lane != b.lane || a.side != b.side ||
                    a.kind != b.kind || a.flags != b.flags || a.type != b.type ||
                    a.stack != b.stack) { same = false; break; }
            }
        }
        std::printf("verify vs range(): %s (%llu vs %llu events)\n",
                    same ? "MATCH" : "MISMATCH",
                    (unsigned long long)ref.size(), (unsigned long long)evs.size());
        if (!same) return 2;
    }
    rd.close();
    return 0;
}

// densgen: synthetic density chart for playback scaling tests.
//   raid_engine densgen <out.raid> <seconds> <eventsPerSec> <chunkEvents>
//                       [sustainEvery] [overlapEvery] [keys]
// Notes are periodic (period 2*keys) so the encoder's pattern matcher
// collapses them into repeated pattern instances — the file stays small
// while decode expands the full physical count. overlapEvery>0 adds a
// simultaneous note on the mirrored lane every Nth step.
static int densgen(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: raid_engine densgen <out.raid> <seconds> <evPerSec> <chunkEvents> [sustainEvery] [overlapEvery] [keys]\n");
        return 1;
    }
    const std::string out = argv[2];
    const double seconds = std::atof(argv[3]);
    const double eps = std::atof(argv[4]);
    const uint32_t chunkEvents = uint32_t(std::strtoul(argv[5], nullptr, 10));
    const int sustainEvery = argc > 6 ? std::atoi(argv[6]) : 32;
    const int overlapEvery = argc > 7 ? std::atoi(argv[7]) : 0;
    const int keys = argc > 8 ? std::atoi(argv[8]) : 4;
    const uint64_t n = uint64_t(seconds * eps);
    if (n == 0 || n > 25000000ull) {
        std::printf("event count %llu out of range (max 25M — RAM bound)\n",
                    (unsigned long long)n);
        return 1;
    }
    // Build raw raid::Events, NOT a Chart: Chart::finalize merges same
    // side+lane notes within 1 ms into stacks, which would cap physical
    // density at ~2*keys notes/ms. The density test needs the true
    // physical event rate, so it bypasses the Chart path entirely.
    std::vector<raid::Event> evs;
    evs.reserve(size_t(n) + (overlapEvery ? n / overlapEvery : 0));
    const int64_t stepUs = std::max<int64_t>(1, int64_t(1000000.0 / eps));
    uint64_t playerN = 0, oppN = 0;
    for (uint64_t i = 0; i < n; ++i) {
        raid::Event e;
        e.timeUs = int64_t(i) * stepUs;
        e.lengthUs = (sustainEvery > 0 && i % uint64_t(sustainEvery) == 0)
                         ? uint32_t(500000) : 0;
        e.lane = uint8_t(i % uint64_t(keys));
        e.side = uint8_t((i / uint64_t(keys)) % 2);
        e.kind = 0;
        e.stack = uint16_t(i % 251 == 0 ? 32 : 1);
        evs.push_back(e);
        (e.side ? playerN : oppN) += e.stack;
        if (overlapEvery > 0 && i % uint64_t(overlapEvery) == 0) {
            raid::Event o = e;
            o.lane = uint8_t((e.lane + 1) % keys);
            o.side ^= 1;
            evs.push_back(o);
            (o.side ? playerN : oppN) += o.stack;
        }
    }
    // Meta-only Chart for the header JSON (keys/tempo/totals; notes empty).
    Chart meta;
    meta.title = "density";
    meta.difficulty = "Dense";
    meta.keys = keys;
    meta.tempo.push_back({0, 120});
    meta.playerNotes = playerN;
    meta.opponentNotes = oppN;
    meta.lengthMs = double(evs.back().timeUs + evs.back().lengthUs) / 1000.0;
    raid::EncodeOptions opt;
    opt.chunkEvents = chunkEvents;
    raid::Info info;
    std::string err;
    if (!raid::writeRaid(out, evs, raid::chartMetaJson(meta), opt, &info, err)) {
        std::printf("encode failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %llu logical events, %llu chunks, %llu patterns, %.1f MB file\n",
                out.c_str(), (unsigned long long)info.logicalEvents,
                (unsigned long long)info.chunkCount, (unsigned long long)info.patternCount,
                double(std::filesystem::file_size(out)) / 1048576.0);
    return 0;
}

// densbench: full gameplay-path benchmark — ChartStream + PlaybackFeed +
// Session at the game's 60 Hz cadence with judgement updates. Reports feed
// throughput, per-frame pump/update times, Session lane-note residency,
// chunk-cache hit rate and bytes decoded per playback second.
static int densbench(const std::string& path, bool realtime, uint64_t memBudgetMb = 0) {
    raid::ChartStream cs;
    Chart meta;
    std::string err;
    if (!cs.open(path, meta, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    Session session;
    SessionConfig cfg;
    cfg.botplay = true;   // auto-judge: exercises hit/miss path + lets GC run
    session.init(meta, cfg);
    session.setTotals(meta.playerNotes, meta.opponentNotes);
    raid::PlaybackFeed feed;
    feed.bind(cs);
    feed.setMaxConcurrentDecodes(std::max(2, std::min(8, int(std::thread::hardware_concurrency()))));
    if (memBudgetMb) feed.setMemoryBudgetBytes(memBudgetMb << 20);

    const double lengthMs = cs.lengthMs();
    const double stepMs = 1000.0 / 60.0;
    uint64_t peakLaneNotes = 0, laneNoteSum = 0, laneSamples = 0;
    double pumpSum = 0, updSum = 0, pumpMax = 0;
    std::vector<double> pumpTimes, updTimes, convTimes, gcTimes, frameTimes;
    pumpTimes.reserve(size_t(lengthMs / stepMs) + 16);
    updTimes.reserve(pumpTimes.capacity()); convTimes.reserve(pumpTimes.capacity());
    gcTimes.reserve(pumpTimes.capacity()); frameTimes.reserve(pumpTimes.capacity());
    const auto t0 = std::chrono::steady_clock::now();
    const auto frame0 = t0;
    for (double songMs = 0.0; songMs <= lengthMs; songMs += stepMs) {
        const auto f0 = std::chrono::steady_clock::now();
        if (!feed.advance(songMs, session, meta.keys, false, err)) {
            std::printf("feed failed @ %.0f ms: %s\n", songMs, err.c_str());
            return 1;
        }
        const auto f1 = std::chrono::steady_clock::now();
        session.update(songMs);
        const auto f2 = std::chrono::steady_clock::now();
        // The game drains GameEvents every frame; do the same so `events`
        // doesn't grow unboundedly and skew the residency numbers.
        session.events.clear();
        const double pumpMs = std::chrono::duration<double, std::milli>(f1 - f0).count();
        const double updMs = std::chrono::duration<double, std::milli>(f2 - f1).count();
        pumpSum += pumpMs; updSum += updMs;
        pumpMax = std::max(pumpMax, pumpMs);
        pumpTimes.push_back(pumpMs);
        updTimes.push_back(updMs);
        convTimes.push_back(feed.lastConvertMs());
        gcTimes.push_back(feed.lastGcMs());
        frameTimes.push_back(std::chrono::duration<double, std::milli>(f2 - f0).count());
        size_t lanes = 0;
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < meta.keys; ++l) lanes += session.lane(side, l).size();
        peakLaneNotes = std::max<uint64_t>(peakLaneNotes, lanes);
        laneNoteSum += lanes;
        ++laneSamples;
        if (realtime) {
            // Pace to a real 60 Hz frame: sleep until 16.667 ms after the
            // previous frame boundary so prefetch workers get real time —
            // this isolates intrinsic pump cost from decode-keepup lag.
            const auto frameStart = frame0 +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(songMs / 1000.0));
            const auto deadline = frameStart + std::chrono::microseconds(16667);
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) std::this_thread::sleep_until(deadline);
        }
    }
    uint64_t ch = 0, cm = 0, cd = 0, cbd = 0;
    cs.reader().cacheStats(ch, cm, cd, cbd);
    const uint64_t cachedEv = cs.cachedEvents(), cachedCap = cs.cachedEventCapacity();
    cs.close();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto pct = [](std::vector<double> v, double q) {
        std::sort(v.begin(), v.end());
        return v[size_t(v.size() * q)];
    };
    const double p50 = pct(pumpTimes, 0.5);
    const double p99 = pct(pumpTimes, 0.99);
    const double p999 = pct(pumpTimes, 0.999);

    const uint64_t queries = ch + cm;
    std::printf("%s: %.0f ms chart, %.1f s bench%s\n", path.c_str(), lengthMs, secs,
                realtime ? " (real-time paced)" : "");
    std::printf("  fed %llu physical events -> %llu lane notes (%llu stack-weighted notes); %.2fM physical NPS, %.2fM stacked NPS over %.1f s\n",
                (unsigned long long)feed.fedEvents(),
                (unsigned long long)feed.appendedEvents(),
                (unsigned long long)feed.fedStackedNotes(),
                feed.fedEvents() / secs / 1e6, feed.fedStackedNotes() / secs / 1e6, secs);
    std::printf("  pump ms: avg %.3f  p50 %.3f  p99 %.3f  p99.9 %.3f  max %.1f\n",
                pumpSum / pumpTimes.size(), p50, p99, p999, pumpMax);
    auto line = [](const char* name, const std::vector<double>& v) {
        double sum = 0, mx = 0;
        for (double x : v) { sum += x; mx = std::max(mx, x); }
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        std::printf("    %-9s avg %7.3f  p50 %7.3f  p99 %7.3f  p99.9 %7.3f  max %8.1f\n",
                    name, sum / double(v.size()), s[s.size() / 2],
                    s[size_t(s.size() * 0.99)], s[size_t(s.size() * 0.999)], mx);
    };
    line("convert", convTimes);
    line("gc", gcTimes);
    line("session", updTimes);
    line("frame", frameTimes);
    std::printf("  session.update ms avg %.3f\n", updSum / pumpTimes.size());
    std::printf("  lane notes resident: peak %llu, avg %.0f (%.1f%% of peak-cap %.0f MB est)\n",
                (unsigned long long)peakLaneNotes,
                laneSamples ? double(laneNoteSum) / double(laneSamples) : 0.0,
                100.0 * double(peakLaneNotes) / double(std::max<uint64_t>(1, cachedCap)),
                double(cachedCap) * 32.0 / 1048576.0);
    std::printf("  chunk cache: %llu hit / %llu miss (%.1f%% hit), %llu decodes, %.1f MB raw decoded (%.1f MB/s)\n",
                (unsigned long long)ch, (unsigned long long)cm,
                queries ? 100.0 * double(ch) / double(queries) : 0.0,
                (unsigned long long)cd, double(cbd) / 1048576.0, cbd / secs / 1048576.0);
    std::printf("  cache resident %llu/%llu events (%.1f MB / %.1f MB budget)\n",
                (unsigned long long)cachedEv, (unsigned long long)cachedCap,
                double(cachedEv) * 32.0 / 1048576.0,
                double(cachedCap) * 32.0 / 1048576.0);
    std::printf("  peak RSS %.1f MB, RSS at close %.1f MB\n",
                peakRssBytes() / 1048576.0, curRssBytes() / 1048576.0);
    return 0;
}

// bench: decode the whole chart through ChartStream::pump at 60 Hz pump
// cadence (same pattern as gameplay) and report logical events/sec.
static int bench(const std::string& path) {
    raid::ChartStream cs;
    Chart meta;
    std::string err;
    if (!cs.open(path, meta, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %.0f ms, keys %d, P%llu O%llu notes\n", path.c_str(), meta.lengthMs,
                meta.keys, (unsigned long long)meta.playerNotes,
                (unsigned long long)meta.opponentNotes);

    // Warm-up: decode chunk 0 so pattern-blob load cost is excluded.
    {
        std::vector<raid::Event> w;
        std::vector<uint64_t> wi;
        cs.pump(0, 1000000, w, wi, err);
    }

    int64_t fedUs = std::numeric_limits<int64_t>::min() / 4, prefetchUs = fedUs;
    double songMs = -300.0;
    const double stepMs = 1000.0 / 60.0;
    uint64_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (songMs < meta.lengthMs + 5000.0) {
        songMs += stepMs;
        const int64_t endUs = int64_t(std::max(0.0, songMs + 4000.0) * 1000.0);
        if (endUs > fedUs) {
            std::vector<raid::Event> evs;
            std::vector<uint64_t> idx;
            if (!cs.pump(fedUs, endUs, evs, idx, err)) {
                std::printf("PUMP FAIL @ %.0f ms: %s\n", songMs, err.c_str());
                return 1;
            }
            total += evs.size();
            fedUs = endUs;
        }
        if (endUs >= prefetchUs) {
            cs.prefetch(endUs, endUs + int64_t(16000.0 * 1000.0));
            prefetchUs = endUs + int64_t(8000.0 * 1000.0);
        }
    }
    cs.close();
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("decoded %llu events in %.3f s -> %.0f events/s (%.1fM NPS logical)\n",
                (unsigned long long)total, sec, total / sec, total / sec / 1e6);
    std::printf("peak RSS %.1f MB, RSS at close %.1f MB\n",
                peakRssBytes() / 1048576.0, curRssBytes() / 1048576.0);
    return 0;
}

// bench <file> <jobs>: decode the whole chart through Reader::range split
// across `jobs` threads on disjoint time slices — measures parallel decode
// scaling (inflate + pattern expansion). jobs<=1 keeps the ChartStream path.
static int benchJobs(const std::string& path, int jobs) {
    raid::Reader r;
    std::string err;
    if (!r.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const raid::Info info = r.info();
    std::printf("%s: %.1f s, keys %u, %llu logical events, jobs %d\n", path.c_str(),
                r.durationUs() / 1e6, info.keyCount,
                (unsigned long long)info.logicalEvents, jobs);
    r.setMaxConcurrentDecodes(jobs);
    // Warm-up: pattern-blob load + first-chunk decode cost excluded.
    {
        std::vector<raid::Event> w;
        std::vector<uint64_t> wi;
        r.range(0, 1000000, w, err, &wi);
    }
    const int64_t totalUs = r.durationUs() + 2000000;
    std::vector<std::thread> ths;
    std::vector<uint64_t> counts(size_t(jobs), 0);
    std::vector<std::string> rangeErrs = std::vector<std::string>(size_t(jobs));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < jobs; ++i) {
        const int64_t lo = totalUs * i / jobs;
        const int64_t hi = totalUs * (i + 1) / jobs;
        ths.emplace_back([&, i, lo, hi] {
            std::vector<raid::Event> evs;
            std::vector<uint64_t> idx;
            if (!r.range(lo, hi, evs, rangeErrs[size_t(i)], &idx)) return;
            counts[size_t(i)] = evs.size();
        });
    }
    for (auto& t : ths) t.join();
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t total = 0;
    for (uint64_t c : counts) total += c;
    for (const std::string& e : rangeErrs)
        if (!e.empty()) { std::printf("range failed: %s\n", e.c_str()); return 1; }
    std::printf("decoded %llu events in %.3f s -> %.0f events/s (%.1fM NPS logical)\n",
                (unsigned long long)total, sec, total / sec, total / sec / 1e6);
    std::printf("peak RSS %.1f MB, RSS at close %.1f MB\n",
                peakRssBytes() / 1048576.0, curRssBytes() / 1048576.0);
    return 0;
}

// ---- .rchr (RAID Character archives) ---------------------------------------

namespace fs = std::filesystem;

// Expands pack arguments (files and directories, recursively) into
// (arcname, disk path) pairs. Arcnames are paths relative to `root` when
// possible, else the file name.
static void collectRchrInputs(const std::vector<std::string>& args, const fs::path& root,
                              std::vector<std::pair<std::string, std::string>>& out, std::string& err) {
    for (const std::string& a : args) {
        std::error_code ec;
        const fs::path p(a);
        if (fs::is_regular_file(p, ec)) {
            fs::path rel = fs::relative(p, root, ec);
            if (ec) rel = p.filename();
            out.push_back({rel.generic_string(), fs::absolute(p, ec).string()});
        } else if (fs::is_directory(p, ec)) {
            for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::error_code ec2;
                fs::path rel = fs::relative(it->path(), root, ec2);
                if (ec2) rel = it->path().filename();
                out.push_back({rel.generic_string(), it->path().string()});
            }
            if (ec) { err = "cannot scan " + a; return; }
        } else {
            err = "not a file or directory: " + a;
            return;
        }
    }
}

static int rchrPack(int argc, char** argv) {
    // rchr pack <out.rchr> [--root dir] <file|dir> [...]
    if (argc < 4) return 2;
    const std::string out = argv[3];
    fs::path root = fs::current_path();
    std::vector<std::string> inputs;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) {
            root = fs::absolute(argv[++i]);
        } else {
            inputs.push_back(a);
        }
    }
    std::vector<std::pair<std::string, std::string>> files;
    std::string err;
    collectRchrInputs(inputs, root, files, err);
    if (!err.empty()) { std::printf("pack failed: %s\n", err.c_str()); return 1; }
    if (!rchr::pack(out, files, err)) { std::printf("pack failed: %s\n", err.c_str()); return 1; }
    uint64_t raw = 0, stored = 0;
    std::error_code ec;
    for (const auto& [arc, disk] : files) raw += fs::file_size(disk, ec);
    stored = fs::file_size(out, ec);
    std::printf("%s: %zu files, %llu -> %llu bytes\n", out.c_str(), files.size(),
                (unsigned long long)raw, (unsigned long long)stored);
    for (const auto& [arc, disk] : files) std::printf("  %s\n", arc.c_str());
    return 0;
}

static int rchrCmd(int argc, char** argv) {
    const std::string sub = argc >= 3 ? argv[2] : "";
    if (sub == "pack" && argc >= 4) return rchrPack(argc, argv);
    if (sub == "list" && argc >= 4) {
        std::vector<rchr::Entry> entries;
        std::string err;
        if (!rchr::list(argv[3], entries, err)) { std::printf("list failed: %s\n", err.c_str()); return 1; }
        uint64_t raw = 0, stored = 0;
        for (const rchr::Entry& e : entries) {
            raw += e.rawSize;
            stored += e.storedSize;
            std::printf("  %-48s %8llu -> %8llu  %s\n", e.path.c_str(),
                        (unsigned long long)e.rawSize, (unsigned long long)e.storedSize,
                        e.method ? "deflate" : "store");
        }
        std::printf("%s: %zu entries, %llu -> %llu bytes\n", argv[3], entries.size(),
                    (unsigned long long)raw, (unsigned long long)stored);
        return 0;
    }
    if (sub == "unpack" && argc >= 5) {
        std::string err;
        if (!rchr::extract(argv[3], argv[4], err)) { std::printf("unpack failed: %s\n", err.c_str()); return 1; }
        std::printf("unpacked %s -> %s\n", argv[3], argv[4]);
        return 0;
    }
    std::printf("raid_engine rchr pack <out.rchr> [--root dir] <file|dir> [...]\n"
                "raid_engine rchr list <file.rchr>\n"
                "raid_engine rchr unpack <file.rchr> <outdir>\n");
    return 2;
}

// ---- raidedit: .raid document editor ------------------------------------
// Edits the two structural layers of a .raid file: the pattern table
// (reusable op streams) and the chunk op log (when patterns spawn).
// Literal ops keep their original bytes; instance/group/burst ops are
// field-edited and re-encoded. `save` re-analyzes every chunk and rewrites
// the file through raid::writeRaidDoc.

namespace {

using raid::doc::Doc;
using raid::doc::Op;
using raid::doc::PatternDoc;
using raid::doc::kLitAbs;
using raid::doc::kLitRel;
using raid::doc::kInst;
using raid::doc::kGroup;
using raid::doc::kBurst;

const char* kOpNames[] = {"LIT-ABS", "LIT-REL", "INST", "GROUP", "BURST"};

std::string opSummary(const Op& o) {
    char b[256];
    if (o.kind == kLitAbs || o.kind == kLitRel) {
        std::snprintf(b, sizeof b, "%s n=%u t=[%.1f..%.1f ms]",
                      kOpNames[o.kind], o.n, double(o.firstUs) / 1000.0,
                      double(o.lastUs) / 1000.0);
        return b;
    }
    if (o.kind == kGroup) {
        std::snprintf(b, sizeof b, "GROUP entries=%zu", o.entries.size());
        return b;
    }
    std::snprintf(b, sizeof b, "%s pat=%u reps=%u anchor=%.1f ms pitch=%+lld lane=%+lld%s%lld",
                  kOpNames[o.kind], o.patternId, o.repeatCount,
                  double(o.anchorUs) / 1000.0, (long long)o.pitchDelta,
                  (long long)o.laneDelta, o.kind == kBurst ? " stride=" : "",
                  o.kind == kBurst ? (long long)o.strideUs : 0);
    return b;
}

// Spawn ops = everything that places pattern content on the timeline.
bool isSpawn(const Op& o) { return o.kind == kInst || o.kind == kBurst || o.kind == kGroup; }

// (chunk, op) locations of spawn ops in order -> spawn index.
std::vector<std::pair<size_t, size_t>> spawnMap(const Doc& d) {
    std::vector<std::pair<size_t, size_t>> m;
    for (size_t c = 0; c < d.chunks.size(); ++c)
        for (size_t o = 0; o < d.chunks[c].ops.size(); ++o)
            if (isSpawn(d.chunks[c].ops[o])) m.push_back({c, o});
    return m;
}

int64_t parseUs(const std::string& v, bool isTime, bool& ok) {
    ok = true;
    if (isTime) return int64_t(std::llround(std::atof(v.c_str()) * 1000.0));
    return std::stoll(v, nullptr, 0);
}

// Sets fields on an inst/burst op. Times are milliseconds, deltas/counts raw.
bool setInstFields(Op& o, const std::vector<std::pair<std::string, std::string>>& kv,
                   std::string& err) {
    if (o.kind != kInst && o.kind != kBurst) { err = "not an instance op"; return false; }
    try {
    for (const auto& kvp : kv) {
        const std::string& k = kvp.first;
        const std::string& v = kvp.second;
        bool ok = true;
        if (k == "pat" || k == "pattern" || k == "id") o.patternId = uint32_t(std::stoul(v, nullptr, 0));
        else if (k == "reps" || k == "repeat") o.repeatCount = uint32_t(std::stoul(v, nullptr, 0));
        else if (k == "anchor" || k == "t" || k == "time") o.anchorUs = parseUs(v, true, ok);
        else if (k == "pitch") o.pitchDelta = std::stoll(v, nullptr, 0);
        else if (k == "lane") o.laneDelta = std::stoll(v, nullptr, 0);
        else if (k == "stride") o.strideUs = std::stoll(v, nullptr, 0);
        else { err = "unknown field " + k; return false; }
        if (!ok) { err = "bad number " + v; return false; }
    }
    } catch (const std::exception&) { err = "bad numeric value"; return false; }
    return true;
}

bool setGroupFields(Op::GEnt& ge, const std::vector<std::pair<std::string, std::string>>& kv,
                    std::string& err) {
    try {
    for (const auto& kvp : kv) {
        const std::string& k = kvp.first;
        const std::string& v = kvp.second;
        bool ok = true;
        if (k == "pat" || k == "pattern" || k == "id") ge.patternId = uint32_t(std::stoul(v, nullptr, 0));
        else if (k == "reps" || k == "repeat") ge.repeatCount = uint32_t(std::stoul(v, nullptr, 0));
        else if (k == "anchor" || k == "t" || k == "time") ge.anchorUs = parseUs(v, true, ok);
        else if (k == "pitch") ge.pitchDelta = std::stoll(v, nullptr, 0);
        else if (k == "lane") ge.laneDelta = std::stoll(v, nullptr, 0);
        else if (k == "winlo" || k == "lo") ge.winLoUs = parseUs(v, true, ok);
        else if (k == "winhi" || k == "hi") ge.winHiUs = parseUs(v, true, ok);
        else { err = "unknown field " + k; return false; }
        if (!ok) { err = "bad number " + v; return false; }
    }
    } catch (const std::exception&) { err = "bad numeric value"; return false; }
    return true;
}

// Re-sorts a chunk's ops by placement start (stable) and refreshes startUs.
void resortChunk(Doc& d, size_t ci) {
    auto& ops = d.chunks[ci].ops;
    for (Op& o : ops) {
        if (o.kind == kInst || o.kind == kBurst) o.startUs = o.anchorUs;
        else if (o.kind == kGroup) {
            o.startUs = INT64_MAX;
            for (const Op::GEnt& ge : o.entries) o.startUs = std::min(o.startUs, ge.anchorUs);
        }
    }
    std::stable_sort(ops.begin(), ops.end(),
                     [](const Op& a, const Op& b) { return a.startUs < b.startUs; });
}

// Validates a chunk-ops mutation with a full analyze; restores the
// snapshot on failure so a bad edit can't corrupt the doc.
bool commitChunkOps(Doc& d, size_t ci, std::vector<Op>& bak, std::string& err) {
    resortChunk(d, ci);
    if (d.analyze(err)) { d.dirty = true; return true; }
    d.chunks[ci].ops = std::move(bak);
    resortChunk(d, ci);
    std::string ignored;
    d.analyze(ignored);  // rebuild stats for the restored state
    return false;
}

// Same for pattern-table mutations.
bool commitPattern(Doc& d, size_t pid, PatternDoc& bak, std::string& err) {
    if (d.analyze(err)) { d.dirty = true; return true; }
    d.patterns[pid] = std::move(bak);
    std::string ignored;
    d.analyze(ignored);
    return false;
}

// Tokenizes "k=v k=v" args.
bool kvPairs(const std::vector<std::string>& a, size_t from,
             std::vector<std::pair<std::string, std::string>>& kv, std::string& err) {
    for (size_t i = from; i < a.size(); ++i) {
        const size_t eq = a[i].find('=');
        if (eq == std::string::npos) { err = "expected key=value, got " + a[i]; return false; }
        kv.push_back({a[i].substr(0, eq), a[i].substr(eq + 1)});
    }
    return true;
}

void printPatterns(Doc& d) {
    std::printf("%4s  %-9s %-5s %-4s %-10s %s\n", "id", "span", "pitch", "ops", "events", "stat");
    for (size_t i = 0; i < d.patterns.size(); ++i) {
        const auto& p = d.patterns[i];
        uint64_t ev = 0;
        for (const Op& o : p.ops)
            if (o.kind == kLitAbs || o.kind == kLitRel) ev += o.n;
        std::printf("%4zu  %9llu %-5s %-4zu %-10llu %s\n", i,
                    (unsigned long long)p.spanUs, p.hasPitch ? "yes" : "no",
                    p.ops.size(), (unsigned long long)ev,
                    p.dynamic ? "dyn" : (p.statReady ? std::to_string(p.statCount).c_str() : "?"));
    }
}

void printChunkOps(Doc& d, size_t ci) {
    const auto& ops = d.chunks[ci].ops;
    std::printf("chunk %zu: %zu ops, expanded=%llu t=[%.1f..%.1f ms]\n", ci, ops.size(),
                (unsigned long long)d.chunks[ci].expanded,
                double(d.chunks[ci].firstTimeUs) / 1000.0,
                double(d.chunks[ci].lastTimeUs) / 1000.0);
    for (size_t i = 0; i < ops.size(); ++i)
        std::printf("  op[%3zu] %-7s %s\n", i, kOpNames[ops[i].kind], opSummary(ops[i]).c_str());
}

void printSpawn(Doc& d, size_t g) {
    const auto sm = spawnMap(d);
    if (g >= sm.size()) { std::printf("no spawn %zu (0..%zu)\n", g, sm.size() ? sm.size() - 1 : 0); return; }
    const auto& loc = sm[g];
    const Op& o = d.chunks[loc.first].ops[loc.second];
    std::printf("spawn %zu = chunk %zu op %zu: %s\n", g, loc.first, loc.second, opSummary(o).c_str());
    if (o.kind == kGroup)
        for (size_t e = 0; e < o.entries.size(); ++e) {
            const Op::GEnt& ge = o.entries[e];
            std::printf("  ent[%zu] pat=%u reps=%u anchor=%.1f ms pitch=%+lld lane=%+lld win=[%.1f..%.1f ms]\n",
                        e, ge.patternId, ge.repeatCount, double(ge.anchorUs) / 1000.0,
                        (long long)ge.pitchDelta, (long long)ge.laneDelta,
                        double(ge.winLoUs) / 1000.0, double(ge.winHiUs) / 1000.0);
        }
}

void printSpawns(Doc& d) {
    const auto sm = spawnMap(d);
    std::printf("%5s  %-5s %-4s %s\n", "idx", "chunk", "op", "summary");
    for (size_t g = 0; g < sm.size(); ++g) {
        const auto& loc = sm[g];
        const Op& o = d.chunks[loc.first].ops[loc.second];
        std::printf("%5zu  %-5zu %-4zu %s\n", g, loc.first, loc.second, opSummary(o).c_str());
    }
    std::printf("%zu spawn ops in %zu chunks\n", sm.size(), d.chunks.size());
}

// Executes one command line against the doc. Returns false on `quit`.
bool raidEditCmd(Doc& d, const std::vector<std::string>& a) {
    if (a.empty()) return true;
    const std::string& c = a[0];
    std::string err;
    auto need = [&](size_t n) { return a.size() >= n; };
    auto num = [&](const char* s, long long& v) {
        try { v = std::stoll(s, nullptr, 0); return true; } catch (...) { return false; }
    };

    if (c == "quit" || c == "exit") return false;
    if (c == "help" || c == "?") {
        std::printf("commands:\n"
                    "  info | patterns | pattern <id> | chunkops <c>\n"
                    "  pdel <patId> <opIdx>   delete an op from a pattern def\n"
                    "  pset <patId> <opIdx> k=v...   edit inst/burst fields in a def\n"
                    "      fields: pat= reps= anchor=(ms) pitch= lane= stride=(us)\n"
                    "  pspan <patId> <spanUs>   set the pattern repeat span\n"
                    "  spawns | spawn <idx>\n"
                    "  sset <idx> k=v...       edit a spawn op's fields\n"
                    "  gset <idx> <ent> k=v... edit one group entry (winlo/winhi in ms)\n"
                    "  sdel <idx> | delop <chunk> <opIdx>   delete ops\n"
                    "  add <ms> <patId> [reps] [pitch] [lane]              insert instance\n"
                    "  addb <ms> <patId> <reps> <strideUs> [pitch] [lane]   insert burst\n"
                    "  analyze | save [out.RAID]   (save defaults to in-place, .bak kept)\n");
        return true;
    }
    if (c == "info") {
        std::printf("%s: v%u, %llu logical events, %zu patterns, %zu chunks, %.0f ms\n",
                    d.path.c_str(), d.info.version,
                    (unsigned long long)d.info.logicalEvents, d.patterns.size(),
                    d.chunks.size(), double(d.info.lengthMs));
        return true;
    }
    if (c == "patterns") { printPatterns(d); return true; }
    if (c == "pattern" && need(2)) {
        long long id;
        if (!num(a[1].c_str(), id) || id < 0 || size_t(id) >= d.patterns.size()) { std::printf("bad pattern id\n"); return true; }
        const auto& p = d.patterns[size_t(id)];
        std::printf("pattern %lld: span=%llu us pitch=%s ops=%zu stat=%s\n", id,
                    (unsigned long long)p.spanUs, p.hasPitch ? "yes" : "no", p.ops.size(),
                    p.dynamic ? "dynamic" : (p.statReady ? "ok" : "?"));
        for (size_t i = 0; i < p.ops.size(); ++i)
            std::printf("  op[%3zu] %-7s %s\n", i, kOpNames[p.ops[i].kind], opSummary(p.ops[i]).c_str());
        return true;
    }
    if (c == "chunkops" && need(2)) {
        long long ci;
        if (!num(a[1].c_str(), ci) || ci < 0 || size_t(ci) >= d.chunks.size()) { std::printf("bad chunk\n"); return true; }
        printChunkOps(d, size_t(ci));
        return true;
    }
    if (c == "spawns") { printSpawns(d); return true; }
    if (c == "spawn" && need(2)) {
        long long g;
        if (!num(a[1].c_str(), g)) { std::printf("bad index\n"); return true; }
        printSpawn(d, size_t(g));
        return true;
    }
    if (c == "pdel" && need(3)) {
        long long id, oi;
        if (!num(a[1].c_str(), id) || !num(a[2].c_str(), oi)) { std::printf("bad args\n"); return true; }
        if (id < 0 || size_t(id) >= d.patterns.size()) { std::printf("bad pattern\n"); return true; }
        auto& ops = d.patterns[size_t(id)].ops;
        if (oi < 0 || size_t(oi) >= ops.size()) { std::printf("bad op\n"); return true; }
        PatternDoc bak = d.patterns[size_t(id)];
        ops.erase(ops.begin() + oi);
        if (!commitPattern(d, size_t(id), bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("deleted pattern %lld op %lld\n", id, oi);
        return true;
    }
    if (c == "pset" && need(4)) {
        long long id, oi;
        if (!num(a[1].c_str(), id) || !num(a[2].c_str(), oi)) { std::printf("bad args\n"); return true; }
        if (id < 0 || size_t(id) >= d.patterns.size()) { std::printf("bad pattern\n"); return true; }
        auto& ops = d.patterns[size_t(id)].ops;
        if (oi < 0 || size_t(oi) >= ops.size()) { std::printf("bad op\n"); return true; }
        PatternDoc bak = d.patterns[size_t(id)];
        std::vector<std::pair<std::string, std::string>> kv;
        if (!kvPairs(a, 3, kv, err) || !setInstFields(ops[size_t(oi)], kv, err)) { std::printf("%s\n", err.c_str()); return true; }
        if (!commitPattern(d, size_t(id), bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("pattern %lld op %lld: %s\n", id, oi, opSummary(ops[size_t(oi)]).c_str());
        return true;
    }
    if (c == "pspan" && need(3)) {
        long long id, span;
        if (!num(a[1].c_str(), id) || !num(a[2].c_str(), span)) { std::printf("bad args\n"); return true; }
        if (id < 0 || size_t(id) >= d.patterns.size()) { std::printf("bad pattern\n"); return true; }
        const uint64_t oldSpan = d.patterns[size_t(id)].spanUs;
        d.patterns[size_t(id)].spanUs = uint64_t(span);
        PatternDoc bak = d.patterns[size_t(id)];
        bak.spanUs = oldSpan;
        if (!commitPattern(d, size_t(id), bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("pattern %lld span = %lld us\n", id, span);
        return true;
    }
    if (c == "sset" && need(3)) {
        long long g;
        if (!num(a[1].c_str(), g)) { std::printf("bad index\n"); return true; }
        const auto sm = spawnMap(d);
        if (g < 0 || size_t(g) >= sm.size()) { std::printf("no spawn %lld\n", g); return true; }
        const auto& loc = sm[size_t(g)];
        const size_t ci = loc.first, oi = loc.second;
        auto bak = d.chunks[ci].ops;
        std::vector<std::pair<std::string, std::string>> kv;
        if (!kvPairs(a, 2, kv, err) || !setInstFields(d.chunks[ci].ops[oi], kv, err)) { std::printf("%s\n", err.c_str()); return true; }
        if (!commitChunkOps(d, ci, bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        printSpawn(d, size_t(g));
        return true;
    }
    if (c == "gset" && need(4)) {
        long long g, e;
        if (!num(a[1].c_str(), g) || !num(a[2].c_str(), e)) { std::printf("bad args\n"); return true; }
        const auto sm = spawnMap(d);
        if (g < 0 || size_t(g) >= sm.size()) { std::printf("no spawn %lld\n", g); return true; }
        const auto& loc = sm[size_t(g)];
        const size_t ci = loc.first, oi = loc.second;
        Op& o = d.chunks[ci].ops[oi];
        if (o.kind != kGroup || e < 0 || size_t(e) >= o.entries.size()) { std::printf("bad group entry\n"); return true; }
        auto bak = d.chunks[ci].ops;
        std::vector<std::pair<std::string, std::string>> kv;
        if (!kvPairs(a, 3, kv, err) || !setGroupFields(o.entries[size_t(e)], kv, err)) { std::printf("%s\n", err.c_str()); return true; }
        if (!commitChunkOps(d, ci, bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        printSpawn(d, size_t(g));
        return true;
    }
    if (c == "sdel" && need(2)) {
        long long g;
        if (!num(a[1].c_str(), g)) { std::printf("bad index\n"); return true; }
        const auto sm = spawnMap(d);
        if (g < 0 || size_t(g) >= sm.size()) { std::printf("no spawn %lld\n", g); return true; }
        const auto& loc = sm[size_t(g)];
        auto bak = d.chunks[loc.first].ops;
        d.chunks[loc.first].ops.erase(d.chunks[loc.first].ops.begin() + loc.second);
        if (!commitChunkOps(d, loc.first, bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("deleted spawn %lld\n", g);
        return true;
    }
    if (c == "delop" && need(3)) {
        long long ci, oi;
        if (!num(a[1].c_str(), ci) || !num(a[2].c_str(), oi)) { std::printf("bad args\n"); return true; }
        if (ci < 0 || size_t(ci) >= d.chunks.size()) { std::printf("bad chunk\n"); return true; }
        auto& ops = d.chunks[size_t(ci)].ops;
        if (oi < 0 || size_t(oi) >= ops.size()) { std::printf("bad op\n"); return true; }
        auto bak = ops;
        ops.erase(ops.begin() + oi);
        if (!commitChunkOps(d, size_t(ci), bak, err)) {
            std::printf("edit rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("deleted chunk %lld op %lld\n", ci, oi);
        return true;
    }
    if ((c == "add" && need(3)) || (c == "addb" && need(5))) {
        const double ms = std::atof(a[1].c_str());
        long long pid, reps = 1, pitch = 0, lane = 0, stride = 0;
        if (!num(a[2].c_str(), pid)) { std::printf("bad pattern id\n"); return true; }
        if (pid < 0 || size_t(pid) >= d.patterns.size()) { std::printf("bad pattern id\n"); return true; }
        if (c == "add") {
            if (a.size() > 3) num(a[3].c_str(), reps);
            if (a.size() > 4) num(a[4].c_str(), pitch);
            if (a.size() > 5) num(a[5].c_str(), lane);
        } else {
            if (!num(a[3].c_str(), reps) || !num(a[4].c_str(), stride)) { std::printf("bad args\n"); return true; }
            if (a.size() > 5) num(a[5].c_str(), pitch);
            if (a.size() > 6) num(a[6].c_str(), lane);
        }
        const int64_t t = int64_t(std::llround(ms * 1000.0));
        // Pick the chunk whose index range contains t (nearest otherwise).
        size_t best = 0;
        int64_t bestDist = INT64_MAX;
        for (size_t i = 0; i < d.chunks.size(); ++i) {
            const auto& ch = d.chunks[i];
            if (t >= ch.indexFirstUs && t <= ch.indexLastUs) { best = i; bestDist = -1; break; }
            const int64_t dist = t < ch.indexFirstUs ? ch.indexFirstUs - t : t - ch.indexLastUs;
            if (dist < bestDist) { bestDist = dist; best = i; }
        }
        Op o;
        o.kind = c == "add" ? kInst : kBurst;
        o.patternId = uint32_t(pid);
        o.repeatCount = uint32_t(reps);
        o.anchorUs = t;
        o.pitchDelta = pitch;
        o.laneDelta = lane;
        o.strideUs = stride;
        o.startUs = t;
        auto bak = d.chunks[best].ops;
        auto& ops = d.chunks[best].ops;
        auto it = std::upper_bound(ops.begin(), ops.end(), t,
                                   [](int64_t v, const Op& x) { return v < x.startUs; });
        ops.insert(it, o);
        if (!commitChunkOps(d, best, bak, err)) {
            std::printf("add rejected: %s\n", err.c_str());
            return true;
        }
        std::printf("added %s pat=%lld @%.1f ms -> chunk %zu\n",
                    o.kind == kInst ? "INST" : "BURST", pid, ms, best);
        return true;
    }
    if (c == "analyze") {
        if (d.analyze(err)) std::printf("ok: %llu events, %zu chunks\n",
                                      (unsigned long long)d.info.logicalEvents, d.chunks.size());
        else std::printf("analyze failed: %s\n", err.c_str());
        return true;
    }
    if (c == "save" && need(1)) {
        const std::string out = a.size() > 1 ? a[1] : "";
        if (out.empty() && !d.dirty) { std::printf("no changes to save\n"); return true; }
        if (out.empty()) {
            std::error_code ec;
            const std::string bak = d.path + ".bak";
            std::filesystem::copy_file(d.path, bak,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { std::printf("backup failed: %s\n", ec.message().c_str()); return true; }
        }
        raid::Info st;
        if (d.save(out, &st, err)) {
            std::printf("saved %s (%llu events, %llu stored bytes)\n",
                        out.empty() ? d.path.c_str() : out.c_str(),
                        (unsigned long long)st.logicalEvents,
                        (unsigned long long)st.storedBytes);
        } else std::printf("save failed: %s\n", err.c_str());
        return true;
    }
    std::printf("unknown command '%s' (try 'help')\n", c.c_str());
    return true;
}

int raidedit(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: raid_engine raidedit <file.RAID> [command args...]\n"
                    "       raid_engine raidedit <file.RAID>   (interactive)\n");
        return 2;
    }
    Doc d;
    std::string err;
    if (!d.load(argv[2], err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
    if (!d.analyze(err)) { std::printf("analyze failed: %s\n", err.c_str()); return 1; }
    std::printf("loaded %s: %llu events, %zu patterns, %zu chunks\n",
                argv[2], (unsigned long long)d.info.logicalEvents,
                d.patterns.size(), d.chunks.size());
    if (argc > 3) {
        // Each argv element is one command line, so edits chain:
        //   raidedit file.RAID "add 5000 25 1" "save out.RAID"
        for (int i = 3; i < argc; ++i) {
            std::istringstream ss(argv[i]);
            std::vector<std::string> a;
            for (std::string t; ss >> t;) a.push_back(t);
            if (!raidEditCmd(d, a)) break;
        }
        return 0;
    }
    // Interactive REPL.
    std::printf("raidedit> (help for commands, quit to exit)\n");
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::vector<std::string> a;
        for (std::string t; ss >> t;) a.push_back(t);
        if (!raidEditCmd(d, a)) break;
        if (d.dirty) std::printf("  (unsaved changes)\n");
        std::printf("raidedit> ");
    }
    if (d.dirty) std::printf("warning: unsaved changes\n");
    return 0;
}

}  // namespace

// Re-stamps a .raid file's format version to the minimum its chunk ops
// require. Files written between the op-3/op-4 encoder commits and the v2
// bump are stamped v1 but contain v2 ops; older builds reject them with
// "unknown op kind". The fix is header-only: bump the version u32 at
// offset 4 and rewrite the header CRC - payloads are untouched, so the
// upgrade is lossless and instant even on huge files. Files already at
// the right version (or pure v1 content) are left alone.
static int upgradeOne(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("%s: open failed: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    uint32_t need = raid::kMinVersion;
    const uint64_t nc = rd.info().chunkCount;
    for (uint64_t i = 0; i < nc; ++i) {
        std::vector<char> p;
        if (!rd.chunkPayloadRaw(size_t(i), p, err)) {
            std::printf("%s: chunk %llu: %s\n", path.c_str(),
                        (unsigned long long)i, err.c_str());
            rd.close();
            return 1;
        }
        need = std::max(need, raid::chunkPayloadVersion(p));
    }
    rd.close();

    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) {
        std::printf("%s: cannot open for update\n", path.c_str());
        return 1;
    }
    unsigned char h[raid::kHeaderSize];
    if (std::fread(h, 1, raid::kHeaderSize, f) != raid::kHeaderSize ||
        std::memcmp(h, raid::kMagic, 4) != 0) {
        std::printf("%s: not a raid file\n", path.c_str());
        std::fclose(f);
        return 1;
    }
    const uint32_t stored = uint32_t(h[4]) | (uint32_t(h[5]) << 8) |
                            (uint32_t(h[6]) << 16) | (uint32_t(h[7]) << 24);
    if (stored >= need) {
        std::printf("%s: already version %u (needs %u) - unchanged\n",
                    path.c_str(), stored, need);
        std::fclose(f);
        return 0;
    }
    h[4] = char(need); h[5] = char(need >> 8);
    h[6] = char(need >> 16); h[7] = char(need >> 24);
    h[108] = h[109] = h[110] = h[111] = 0;
    const mz_ulong crc = mz_crc32(0, h, raid::kHeaderSize);
    h[108] = char(crc); h[109] = char(crc >> 8);
    h[110] = char(crc >> 16); h[111] = char(crc >> 24);
    if (std::fseek(f, 0, SEEK_SET) != 0 ||
        std::fwrite(h, 1, raid::kHeaderSize, f) != raid::kHeaderSize) {
        std::printf("%s: write failed\n", path.c_str());
        std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("%s: version %u -> %u\n", path.c_str(), stored, need);
    return 0;
}

static int upgradeCmd(int argc, char** argv) {
    int fails = 0, done = 0;
    for (int i = 0; i < argc; ++i) {
        const std::string p = argv[i];
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& de : std::filesystem::recursive_directory_iterator(p, ec)) {
                if (de.is_regular_file(ec) &&
                    de.path().extension() == ".raid") {
                    fails += upgradeOne(de.path().string()) != 0;
                    ++done;
                }
            }
        } else {
            fails += upgradeOne(p) != 0;
            ++done;
        }
    }
    std::printf("%d file(s) checked, %d failed\n", done, fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("raid_engine - RAID Engine command line tool\n"
                    "  raid_engine convert <chart|flp|fsc> <out.raid>\n"
                    "      [--opponent-pack f.rchr] [--player-pack f.rchr]\n"
                    "      [--opponent-pack-embed f] [--player-pack-embed f]\n"
                    "      [--opponent-name n] [--player-name n]\n"
                    "  raid_engine info <file.raid>\n"
                    "  raid_engine verify <file.raid>\n"
                    "  raid_engine window <file.raid> <startMs> <endMs>\n"
                    "  raid_engine meta <file.raid>\n"
                    "  raid_engine patterns <file.raid> [startMs] [endMs]\n"
                    "  raid_engine streamtest <file.raid>\n"
                    "  raid_engine bench <file.raid> [jobs]\n"
                    "  raid_engine slicebench <file.raid>\n"
                    "  raid_engine eval <file.raid> <startMs> <endMs> [iters] [verify]\n"
                    "  raid_engine repgen <out.raid> <repeats> <motif> <periodMs> [chunkEv] [stack] [keys]\n"
                    "  raid_engine stressgen <out.raid> <repeats> <motif> <periodMs> [stack] [keys]\n"
                    "  raid_engine densgen <out.raid> <sec> <evPerSec> <chunkEvents> [susEvery] [overlapEvery] [keys]\n"
                    "  raid_engine densbench <file.raid>\n"
                    "  raid_engine rchr pack|list|unpack ...\n"
                    "  raid_engine upgrade <file|dir>...   re-stamp format version\n");
        return argc < 2 ? 2 : 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "convert" && argc >= 4) {
        // Optional pack flags: --opponent-pack/--player-pack record an
        // external .rchr reference in the .raid trailer (--*-pack-embed
        // stores the bytes instead); --*-name overrides the character name.
        PackPick opp, player;
        if ((argc - 4) % 2 != 0) {
            std::printf("convert flags must come in --flag value pairs\n");
            return 2;
        }
        for (int i = 4; i + 1 < argc; i += 2) {
            const std::string flag = argv[i];
            PackPick* dst = nullptr;
            if (flag == "--opponent-pack") dst = &opp;
            else if (flag == "--player-pack") dst = &player;
            else if (flag == "--opponent-pack-embed") { opp.embedBytes = true; dst = &opp; }
            else if (flag == "--player-pack-embed") { player.embedBytes = true; dst = &player; }
            else if (flag == "--opponent-name") { opp.name = argv[i + 1]; continue; }
            else if (flag == "--player-name") { player.name = argv[i + 1]; continue; }
            else { std::printf("unknown convert flag: %s\n", flag.c_str()); return 2; }
            dst->file = argv[i + 1];
        }
        return convert(argv[2], argv[3], opp, player);
    }
    if (cmd == "info" && argc >= 3) return info(argv[2]);
    if (cmd == "verify" && argc >= 3) return verify(argv[2]);
    if (cmd == "window" && argc >= 5) return window(argv[2], argv[3], argv[4]);
    if (cmd == "meta" && argc >= 3) return meta(argv[2]);
    if (cmd == "streamtest" && argc >= 3) return streamtest(argv[2]);
    if (cmd == "slicebench" && argc >= 3) return slicebench(argv[2]);
    if (cmd == "eval" && argc >= 5)
        return evalCmd(argv[2], argv[3], argv[4],
                       argc > 5 ? std::atoi(argv[5]) : 1,
                       argc > 6 && std::string(argv[6]) == "verify");
    if (cmd == "densgen" && argc >= 6) return densgen(argc, argv);
    if (cmd == "repgen" && argc >= 6) return repgen(argc, argv);
    if (cmd == "stressgen" && argc >= 6) return stressgen(argc, argv);
    if (cmd == "densbench" && argc >= 3)
        return densbench(argv[2], argc >= 4 && std::string(argv[3]) == "rt",
                        argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 0);
    if (cmd == "bench" && argc >= 3) {
        const int jobs = argc >= 4 ? std::max(1, std::atoi(argv[3])) : 0;
        return jobs >= 1 ? benchJobs(argv[2], jobs) : bench(argv[2]);
    }
    if (cmd == "rchr" && argc >= 3) return rchrCmd(argc, argv);
    if (cmd == "patterns" && argc >= 3)
        return patterns(argv[2], argc >= 4 ? argv[3] : nullptr,
                        argc >= 5 ? argv[4] : nullptr);
    if (cmd == "flppatterns" && argc >= 3) return flppatterns(argv[2]);
    if (cmd == "raidedit" && argc >= 3) return raidedit(argc, argv);
    if (cmd == "upgrade" && argc >= 3) return upgradeCmd(argc - 2, argv + 2);
    std::printf("unknown or incomplete command\n");
    return 2;
}
