// raid_engine: standalone RAID Engine command line tool (no raylib).
//
//   raid_engine convert <chart|flp|fsc> <out.raid>   encode a chart to .raid
//   raid_engine info <file.raid>                     header + encoder stats
//   raid_engine verify <file.raid>                   decode every chunk, count events
//   raid_engine window <file.raid> <startMs> <endMs> events in a time window
//   raid_engine meta <file.raid>                     the chart metadata (JSON)
//   raid_engine patterns <file.raid> [startMs] [endMs] op-stream log: which
//                                                    pattern plays when
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "flp_import.hpp"
#include "raid.hpp"
#include "raid_chart.hpp"
#include "raid_doc.hpp"
#include "rchr.hpp"

using namespace gh;

static int convert(const std::string& in, const std::string& out) {
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;

    // Whole .flp/.fsc projects encode directly: patterns are stored once as
    // pattern-table definitions and playlist clips place them on the
    // timeline â€” the file decodes as one contiguous chart. A `#N` suffix
    // still selects a single pattern through the Chart path.
    std::string flpFile;
    int flpPat = -1;
    if (flpChartPath(in, flpFile, flpPat) && flpPat < 0) {
        FlpProject p;
        if (!readFlpFile(flpFile, p, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        if (!raid::writeRaidFlp(out, p, opt, &info, err)) {
            std::printf("encode failed: %s\n", err.c_str());
            return 1;
        }
    } else {
        Chart c;
        if (!loadChartFile(in, "", c, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        if (!raid::saveChart(out, c, opt, &info, err)) {
            std::printf("encode failed: %s\n", err.c_str());
            return 1;
        }
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
    std::printf("  duration       : %.0f ms\n", double(rd.durationUs()) / 1000.0);
    rd.close();
    return 0;
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
        std::printf("  t=%lld.%03d ms lane=%u side=%u kind=%u len=%lld us\n",
                    (long long)(e.timeUs / 1000), int(e.timeUs % 1000), e.lane, e.side, e.kind,
                    (long long)e.lengthUs);
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
    for (size_t i = 0; i < bucket.size(); ++i)
        if (bucket[i]) std::printf("  %4zu0-%4zus: %llu\n", i, i, (unsigned long long)bucket[i]);
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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("raid_engine - RAID Engine command line tool\n"
                    "  raid_engine convert <chart|flp|fsc> <out.raid>\n"
                    "  raid_engine info <file.raid>\n"
                    "  raid_engine verify <file.raid>\n"
                    "  raid_engine window <file.raid> <startMs> <endMs>\n"
                    "  raid_engine meta <file.raid>\n"
                    "  raid_engine patterns <file.raid> [startMs] [endMs]\n"
                    "  raid_engine streamtest <file.raid>\n"
                    "  raid_engine rchr pack|list|unpack ...\n");
        return argc < 2 ? 2 : 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "convert" && argc >= 4) return convert(argv[2], argv[3]);
    if (cmd == "info" && argc >= 3) return info(argv[2]);
    if (cmd == "verify" && argc >= 3) return verify(argv[2]);
    if (cmd == "window" && argc >= 5) return window(argv[2], argv[3], argv[4]);
    if (cmd == "meta" && argc >= 3) return meta(argv[2]);
    if (cmd == "streamtest" && argc >= 3) return streamtest(argv[2]);
    if (cmd == "rchr" && argc >= 3) return rchrCmd(argc, argv);
    if (cmd == "patterns" && argc >= 3)
        return patterns(argv[2], argc >= 4 ? argv[3] : nullptr,
                        argc >= 5 ? argv[4] : nullptr);
    if (cmd == "raidedit" && argc >= 3) return raidedit(argc, argv);
    std::printf("unknown or incomplete command\n");
    return 2;
}
