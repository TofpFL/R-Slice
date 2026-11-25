// .raid document model for the editor (raid_engine raidedit).
//
// A .raid file is two editable layers:
//   - the pattern table: reusable op streams (literal event runs plus
//     nested pattern references), each with a repeat span and pitch flag;
//   - the chunk op log: the timeline's literal runs and pattern
//     instance/group/burst ops - "when the chart places patterns".
//
// raid_doc parses both layers into Doc structures. Literal ops keep their
// original encoded bytes so untouched content round-trips bit-exactly;
// instance/group/burst ops are held as fields and re-encoded on save.
// Save re-analyzes every chunk (expanded count + time range) and rewrites
// header/index/payloads through raid::writeRaidDoc.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "raid.hpp"

namespace gh::raid::doc {

// Op kinds mirror the on-disk codec (raid.cpp).
enum OpKind : uint8_t {
    kLitAbs = 0,    // literal run, absolute times (chunk level)
    kLitRel = 1,    // literal run, pattern-local times (inside defs)
    kInst = 2,      // pattern instance: pid, reps, anchor, pitch/lane delta
    kGroup = 3,     // overlapping instances with absolute clip windows
    kBurst = 4,     // instance repeated on an explicit stride
};

struct Op {
    uint8_t kind = kLitAbs;
    // Literal ops: the complete original op encoding (kind byte included),
    // plus the decoded time range for display/sorting. n is the stored
    // event count.
    std::vector<char> raw;
    uint32_t n = 0;
    int64_t firstUs = 0, lastUs = 0;
    uint64_t stackSum = 0;  // literal ops: sum of stack columns (note count)
    // Instance/burst ops (kInst, kBurst):
    uint32_t patternId = 0, repeatCount = 0;
    int64_t anchorUs = 0, pitchDelta = 0, laneDelta = 0, strideUs = 0;  // burst only
    // Group ops (kGroup):
    struct GEnt {
        uint32_t patternId = 0, repeatCount = 1;
        int64_t anchorUs = 0, pitchDelta = 0, laneDelta = 0;
        int64_t winLoUs = 0, winHiUs = 0;
    };
    std::vector<GEnt> entries;
    // Computed placement start (literal: first event time; inst/burst:
    // anchor; group: earliest entry anchor) - used for display order and
    // sorted insertion. Not serialized.
    int64_t startUs = 0;
};

struct PatternDoc {
    uint64_t spanUs = 0;    // repeat period (editable)
    bool hasPitch = false;  // preserved from the file
    std::vector<Op> ops;
    // Analysis cache (rebuilt on demand): expanded events per play and
    // the local time range, or dynamic=true when the def's expansion
    // depends on the instantiation context (group ops inside).
    bool statReady = false, dynamic = false, hasTimes = false;
    uint64_t statCount = 0;
    int64_t statFirst = 0, statLast = 0;
    std::vector<int64_t> times;  // sorted local event times (when hasTimes)
};

struct ChunkDoc {
    std::vector<Op> ops;
    // Original index time range (for placement decisions before save).
    int64_t indexFirstUs = 0, indexLastUs = 0;
    // Analysis results (filled by analyze()):
    uint64_t expanded = 0;
    int64_t firstTimeUs = 0, lastTimeUs = 0;
};

struct Doc {
    raid::Info info;
    std::string metaJson;
    std::string path;  // source file (save() target when outPath is empty)
    std::vector<PatternDoc> patterns;
    std::vector<ChunkDoc> chunks;
    bool dirty = false;

    // Loads every editable layer. `path` must be a .raid file.
    bool load(const std::string& path, std::string& err);
    // Re-analyzes all chunks and writes `path` (or `outPath` when non-empty).
    bool save(const std::string& outPath, raid::Info* stats, std::string& err);
    // Re-analyzes all chunks without writing (validates the doc).
    bool analyze(std::string& err);

    // Serializes one op (literal ops return their stored raw bytes).
    static bool serializeOp(const Op& op, std::vector<char>& out);
    // Serialized size of an op.
    static size_t opSize(const Op& op);

    // Pattern-table stat: expanded events + local time range per play.
    // Computed lazily; call invalidateStats() after pattern edits.
    void invalidateStats();

private:
    static int64_t clipMinus(int64_t clip, int64_t base);
    bool buildStat(size_t pid, int depth, std::string& err);
    // Counts expanded events of `ops` under `ctx` (timeBase/pitchBase/
    // laneBase/clip window), tracking the min/max produced event times.
    // Mirrors raid.cpp emitOps semantics without materializing events.
    struct CountOut { uint64_t count = 0; int64_t first = INT64_MAX, last = INT64_MIN; };
    bool countOps(const std::vector<Op>& ops, int64_t timeBase, int64_t clipLo,
                  int64_t clipHi, int depth, CountOut& out, std::string& err);
    bool countPatternOps(size_t pid, int64_t timeBase, int64_t clipLo,
                         int64_t clipHi, int depth, CountOut& out, std::string& err);
};

}  // namespace gh::raid::doc
