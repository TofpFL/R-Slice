// .raid document model implementation — see raid_doc.hpp.
#include "raid_doc.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include "miniz.h"

namespace gh::raid::doc {

namespace {

// ------------------------------------------------------------ byte I/O ----

struct Cur {
    const char* p = nullptr;
    const char* end = nullptr;
    bool ok = true;
    uint8_t u8() {
        if (p >= end) { ok = false; return 0; }
        return uint8_t(*p++);
    }
    uint16_t u16() { return uint16_t(u8() | (u8() << 8)); }
    uint32_t u32() { return uint32_t(u16()) | (uint32_t(u16()) << 16); }
    uint64_t u64() { return uint64_t(u32()) | (uint64_t(u32()) << 32); }
    int64_t i64() { return int64_t(u64()); }
    uint64_t var() {
        uint64_t v = 0;
        int s = 0;
        for (;;) {
            uint8_t c = u8();
            if (!ok) return 0;
            v |= uint64_t(c & 0x7F) << s;
            if (!(c & 0x80)) return v;
            s += 7;
            if (s > 63) { ok = false; return 0; }
        }
    }
    int64_t zig() {
        uint64_t v = var();
        return int64_t((v >> 1) ^ uint64_t(0) - (v & 1));
    }
    bool skip(size_t n) {
        if (size_t(end - p) < n) { ok = false; return false; }
        p += n;
        return true;
    }
};

struct Buf {
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

constexpr int kMaxDepth = 8;
// Local-time vectors are kept per pattern for group-window counting; cap
// the materialization so hostile defs cannot exhaust memory. Analytic
// stats (count/first/last) are still exact without the vector.
constexpr uint64_t kTimesCap = 8ull << 20;

// ------------------------------------------------------------- parsing ---

// Skips n varints (zigzag and varint have identical wire length).
bool skipVars(Cur& c, uint64_t n, std::string& err) {
    for (uint64_t i = 0; i < n; ++i) {
        (void)c.var();
        if (!c.ok) { err = "truncated literal"; return false; }
    }
    return true;
}

// Parses one literal op starting after its kind byte. Fills raw (the whole
// op including the kind byte), n, firstUs/lastUs (decoded with a zero
// context: absolute for kLitAbs, pattern-local for kLitRel) and stackSum.
bool parseLiteral(Cur& c, const char* opStart, uint8_t kind, Op& op,
                  std::string& err) {
    op.kind = kind;
    op.n = uint32_t(c.u32());
    if (!c.ok || op.n == 0 || op.n > (1u << 24)) { err = "bad literal count"; return false; }
    // Time column: first value is zigzag (abs) or varint offset (rel),
    // then n-1 unsigned deltas.
    int64_t t;
    if (kind == kLitAbs) t = c.zig(); else t = int64_t(c.var());
    if (!c.ok) { err = "truncated literal"; return false; }
    const int64_t first = t;
    for (uint32_t k = 1; k < op.n; ++k) {
        const uint64_t d = c.var();
        if (!c.ok) { err = "truncated literal"; return false; }
        t += int64_t(d);
    }
    op.firstUs = first;
    op.lastUs = t;
    // Property columns: length, pitch, lane, attr, type (5 x n), then the
    // stack column is decoded for stats; all are n varints each.
    if (!skipVars(c, uint64_t(op.n) * 5, err)) return false;
    uint64_t stackSum = 0;
    for (uint32_t k = 0; k < op.n; ++k) {
        const uint64_t s = c.var();
        if (!c.ok) { err = "truncated literal"; return false; }
        stackSum += s ? s : 1;
    }
    op.raw.assign(opStart, c.p);
    op.startUs = first;
    op.stackSum = stackSum;
    return true;
}

// Parses `count` ops from `p` (which points at the u32 opCount field).
// Trailing bytes after the last op are ignored (chunk payloads end with a
// u64 expanded-count trailer).
bool parseOps(const char* p, size_t len, std::vector<Op>& ops, std::string& err) {
    Cur c{p, p + len};
    const uint32_t count = uint32_t(c.u32());
    if (!c.ok || count > (1u << 22)) { err = "bad op count"; return false; }
    ops.clear();
    ops.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const char* opStart = c.p;
        const uint8_t kind = c.u8();
        if (!c.ok) { err = "truncated op"; return false; }
        Op op;
        if (kind == kLitAbs || kind == kLitRel) {
            if (!parseLiteral(c, opStart, kind, op, err)) return false;
        } else if (kind == kInst || kind == kBurst) {
            op.kind = kind;
            op.patternId = uint32_t(c.u32());
            op.repeatCount = uint32_t(c.u32());
            op.anchorUs = c.zig();
            op.pitchDelta = c.zig();
            op.laneDelta = c.zig();
            if (kind == kBurst) op.strideUs = c.zig();
            if (!c.ok) { err = "truncated pattern op"; return false; }
            op.startUs = op.anchorUs;
        } else if (kind == kGroup) {
            op.kind = kind;
            const uint32_t nEnt = uint32_t(c.u32());
            if (!c.ok || nEnt == 0 || nEnt > 4096) { err = "bad group count"; return false; }
            op.entries.reserve(nEnt);
            op.startUs = INT64_MAX;
            for (uint32_t e = 0; e < nEnt; ++e) {
                Op::GEnt ge;
                ge.patternId = uint32_t(c.u32());
                ge.repeatCount = uint32_t(c.u32());
                ge.anchorUs = c.zig();
                ge.pitchDelta = c.zig();
                ge.laneDelta = c.zig();
                ge.winLoUs = c.zig();
                ge.winHiUs = c.zig();
                if (!c.ok) { err = "truncated group op"; return false; }
                op.startUs = std::min(op.startUs, ge.anchorUs);
                op.entries.push_back(ge);
            }
        } else {
            err = "unknown op kind";
            return false;
        }
        ops.push_back(std::move(op));
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------ serialize ---

bool Doc::serializeOp(const Op& op, std::vector<char>& out) {
    out.clear();
    if (op.kind == kLitAbs || op.kind == kLitRel) {
        out = op.raw;
        return !out.empty();
    }
    Buf b;
    if (op.kind == kInst || op.kind == kBurst) {
        b.u8(op.kind);
        b.u32(op.patternId);
        b.u32(op.repeatCount);
        b.zig(op.anchorUs);
        b.zig(op.pitchDelta);
        b.zig(op.laneDelta);
        if (op.kind == kBurst) b.zig(op.strideUs);
    } else if (op.kind == kGroup) {
        b.u8(kGroup);
        b.u32(uint32_t(op.entries.size()));
        for (const Op::GEnt& ge : op.entries) {
            b.u32(ge.patternId);
            b.u32(ge.repeatCount);
            b.zig(ge.anchorUs);
            b.zig(ge.pitchDelta);
            b.zig(ge.laneDelta);
            b.zig(ge.winLoUs);
            b.zig(ge.winHiUs);
        }
    } else {
        return false;
    }
    out = std::move(b.b);
    return true;
}

size_t Doc::opSize(const Op& op) {
    if (op.kind == kLitAbs || op.kind == kLitRel) return op.raw.size();
    std::vector<char> tmp;
    return serializeOp(op, tmp) ? tmp.size() : 0;
}

// ------------------------------------------------------------ load --------

bool Doc::load(const std::string& path, std::string& err) {
    err.clear();
    raid::Reader rd;
    if (!rd.open(path, err)) return false;
    info = rd.info();
    metaJson = rd.metaJson();

    std::vector<char> blob;
    std::vector<raid::PatternRec> recs;
    if (!rd.patternBlob(blob, err) || !rd.patternRecs(recs, err)) return false;
    patterns.clear();
    patterns.resize(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        const raid::PatternRec& r = recs[i];
        if (r.off > blob.size() || r.len > blob.size() - r.off || r.len < 4) {
            err = "bad pattern record";
            return false;
        }
        PatternDoc& d = patterns[i];
        d.spanUs = r.spanUs;
        d.hasPitch = r.hasPitch;
        if (!parseOps(blob.data() + r.off, r.len, d.ops, err)) {
            err = "pattern " + std::to_string(i) + ": " + err;
            return false;
        }
    }

    chunks.clear();
    chunks.resize(info.chunkCount);
    uint64_t declaredTotal = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::vector<char> payload;
        if (!rd.chunkPayloadRaw(i, payload, err)) return false;
        if (payload.size() < 12) { err = "bad chunk payload"; return false; }
        // Trailer: last 8 bytes are the declared expanded count.
        Cur tc{payload.data() + payload.size() - 8, payload.data() + payload.size()};
        const uint64_t declared = tc.u64();
        if (!tc.ok) { err = "bad chunk trailer"; return false; }
        declaredTotal += declared;
        ChunkDoc& d = chunks[i];
        int64_t ft = 0, lt = 0;
        if (!rd.chunkTimeRange(i, ft, lt)) { err = "bad chunk index"; return false; }
        d.indexFirstUs = ft;
        d.indexLastUs = lt;
        if (!parseOps(payload.data(), payload.size() - 8, d.ops, err)) {
            err = "chunk " + std::to_string(i) + ": " + err;
            return false;
        }
        d.expanded = declared;
        d.firstTimeUs = ft;
        d.lastTimeUs = lt;
    }
    if (declaredTotal != info.logicalEvents) {
        err = "chunk expanded counts do not sum to logicalEvents";
        return false;
    }
    invalidateStats();
    dirty = false;
    return true;
}

void Doc::invalidateStats() {
    for (PatternDoc& p : patterns) p.statReady = false;
}

// ------------------------------------------------------------ stats -------

// Builds pattern pid's clip-free expansion stat under the identity
// transform (timeBase 0, no clip window). Exact unless the def's expansion
// depends on instantiation context (group ops inside) -> dynamic.
bool Doc::buildStat(size_t pid, int depth, std::string& err) {
    if (pid >= patterns.size()) { err = "bad pattern id"; return false; }
    PatternDoc& self = patterns[pid];
    if (self.statReady) return true;
    if (depth > kMaxDepth) { err = "pattern nesting too deep"; return false; }
    self.statReady = true;  // cycle guard: a recursive reference reads as done
    self.dynamic = false;
    self.hasTimes = true;
    self.times.clear();
    self.statCount = 0;
    self.statFirst = INT64_MAX;
    self.statLast = INT64_MIN;

    auto noteRange = [&](int64_t f, int64_t l) {
        self.statFirst = std::min(self.statFirst, f);
        self.statLast = std::max(self.statLast, l);
    };

    // emitOps appends each op's events in op order and Reader::range relies
    // on non-decreasing times, so op i+1's earliest event must not precede
    // op i's latest. Track the previous op's produced range to enforce it.
    int64_t prevLast = INT64_MIN;
    for (const Op& op : self.ops) {
        int64_t opFirst = INT64_MAX, opLast = INT64_MIN;
        bool rangeKnown = true, unknownRange = false;
        if (op.kind == kLitAbs || op.kind == kLitRel) {
            self.statCount += op.n;
            noteRange(op.firstUs, op.lastUs);
            opFirst = op.firstUs;
            opLast = op.lastUs;
            // Collect local times for group-window counting. Literal times
            // are stored as first + deltas; re-derive them from raw.
            if (self.hasTimes) {
                Cur c{op.raw.data(), op.raw.data() + op.raw.size()};
                (void)c.u8();  // kind
                const uint32_t n = uint32_t(c.u32());
                if (!c.ok || n != op.n) { self.hasTimes = false; }
                else {
                    int64_t t = op.kind == kLitAbs ? c.zig() : int64_t(c.var());
                    self.times.push_back(t);
                    for (uint32_t k = 1; k < n; ++k) {
                        t += int64_t(c.var());
                        self.times.push_back(t);
                    }
                    if (self.times.size() > kTimesCap) self.hasTimes = false;
                    // Skip the remaining property columns.
                    if (!skipVars(c, uint64_t(n) * 6, err)) return false;
                }
            }
        } else if (op.kind == kInst || op.kind == kBurst) {
            if (op.patternId >= patterns.size()) { err = "bad pattern id"; return false; }
            PatternDoc& child = patterns[op.patternId];
            if (!buildStat(op.patternId, depth + 1, err)) return false;
            if (child.dynamic) {
                self.dynamic = true;
                self.hasTimes = false;  // expansion is context-dependent
                rangeKnown = false;
                unknownRange = true;
            } else {
                const int64_t spacing =
                    op.kind == kBurst ? op.strideUs : int64_t(child.spanUs);
                const uint64_t reps = op.repeatCount;
                // Repeated copies must not overlap: copy r ends at
                // anchor+r*spacing+statLast, copy r+1 starts at
                // anchor+(r+1)*spacing+statFirst.
                if (reps > 1 && child.statCount &&
                    child.statLast - child.statFirst > spacing) {
                    err = "pattern " + std::to_string(pid) + " op " +
                          std::to_string(&op - self.ops.data()) +
                          ": repeat spacing smaller than pattern content";
                    return false;
                }
                self.statCount += reps * child.statCount;
                if (reps) {
                    // Rep r anchors at anchor + r*spacing (spacing >= 0).
                    noteRange(op.anchorUs + child.statFirst,
                              op.anchorUs + int64_t(reps - 1) * spacing +
                                  child.statLast);
                    opFirst = op.anchorUs + child.statFirst;
                    opLast = op.anchorUs + int64_t(reps - 1) * spacing +
                             child.statLast;
                } else {
                    rangeKnown = false;  // produces nothing
                }
                if (self.hasTimes && child.hasTimes && reps &&
                    child.times.size() &&
                    reps * child.times.size() <= kTimesCap) {
                    for (uint64_t r = 0; r < reps; ++r) {
                        const int64_t base = op.anchorUs + int64_t(r) * spacing;
                        for (int64_t t : child.times) self.times.push_back(base + t);
                    }
                    if (self.times.size() > kTimesCap) self.hasTimes = false;
                } else {
                    self.hasTimes = false;
                }
            }
        } else if (op.kind == kGroup) {
            // Clip windows are absolute: expansion depends on where this def
            // is instantiated, so no clip-free stat exists.
            self.dynamic = true;
            self.hasTimes = false;
            self.statCount = 0;
            self.statFirst = INT64_MAX;
            self.statLast = INT64_MIN;
            return true;
        }
        if (rangeKnown && opFirst != INT64_MAX) {
            if (opFirst < prevLast) {
                err = "pattern " + std::to_string(pid) + " op " +
                      std::to_string(&op - self.ops.data()) +
                      ": ops overlap; decoded events would be out of order";
                return false;
            }
            prevLast = opLast;
        } else if (unknownRange) {
            prevLast = INT64_MIN;  // can't verify ordering across it
        }
    }
    if (self.hasTimes) {
        if (self.times.size() > 1)
            std::sort(self.times.begin(), self.times.end());
        if (!self.times.empty()) {
            self.statFirst = self.times.front();
            self.statLast = self.times.back();
        } else {
            self.statFirst = INT64_MAX;
            self.statLast = INT64_MIN;
        }
    }
    return true;
}

// ------------------------------------------------------------- counting ---

// clip bound minus a base, saturated (clip bounds may be INT64_MIN/MAX).
int64_t Doc::clipMinus(int64_t clip, int64_t base) {
    if (clip == INT64_MIN || clip == INT64_MAX) return clip;
    if (base > 0 && clip < INT64_MIN + base) return INT64_MIN;
    if (base < 0 && clip > INT64_MAX + base) return INT64_MAX;
    return clip - base;
}

bool Doc::countOps(const std::vector<Op>& ops, int64_t timeBase, int64_t clipLo,
                   int64_t clipHi, int depth, CountOut& out, std::string& err) {
    if (depth > kMaxDepth) { err = "pattern nesting too deep"; return false; }
    const bool noClip = clipLo == INT64_MIN && clipHi == INT64_MAX;
    // Same ordering rule as buildStat: emitOps appends ops in order and
    // Reader::range assumes non-decreasing output times.
    int64_t prevLast = INT64_MIN;
    for (const Op& op : ops) {
        CountOut sub;
        bool rangeKnown = true, unknownRange = false;
        if (op.kind == kLitAbs || op.kind == kLitRel) {
            if (noClip) {
                sub.count = op.n;
                sub.first = op.kind == kLitRel ? timeBase + op.firstUs : op.firstUs;
                sub.last = op.kind == kLitRel ? timeBase + op.lastUs : op.lastUs;
            } else {
                // Finite clip: scan the time column.
                Cur c{op.raw.data(), op.raw.data() + op.raw.size()};
                (void)c.u8();
                const uint32_t n = uint32_t(c.u32());
                if (!c.ok || n != op.n) { err = "bad literal"; return false; }
                int64_t t = op.kind == kLitAbs ? c.zig() : int64_t(c.var()) + timeBase;
                if (!c.ok) { err = "truncated literal"; return false; }
                for (uint32_t k = 0; k < n; ++k) {
                    if (k) {
                        t += int64_t(c.var());
                        if (!c.ok) { err = "truncated literal"; return false; }
                    }
                    if (t >= clipLo && t < clipHi) {
                        ++sub.count;
                        sub.first = std::min(sub.first, t);
                        sub.last = std::max(sub.last, t);
                    }
                }
            }
            if (sub.count == 0) rangeKnown = false;
        } else if (op.kind == kInst || op.kind == kBurst) {
            if (op.patternId >= patterns.size()) { err = "bad pattern id"; return false; }
            PatternDoc& child = patterns[op.patternId];
            if (!child.statReady && !buildStat(op.patternId, depth + 1, err)) return false;
            const int64_t spacing = op.kind == kBurst ? op.strideUs : int64_t(child.spanUs);
            const uint64_t reps = op.repeatCount;
            if (child.dynamic) {
                unknownRange = true;
                rangeKnown = false;
            } else {
                if (reps > 1 && child.statCount &&
                    child.statLast - child.statFirst > spacing) {
                    err = "repeat spacing smaller than pattern content";
                    return false;
                }
                if (!noClip && child.hasTimes && reps && !child.times.empty() &&
                    reps <= (kTimesCap / child.times.size())) {
                    // Clip window shifts per rep: binary-search the child's
                    // sorted local times for each rep's absolute window.
                    for (uint64_t r = 0; r < reps; ++r) {
                        const int64_t base = timeBase + op.anchorUs + int64_t(r) * spacing;
                        const int64_t lo = clipMinus(clipLo, base), hi = clipMinus(clipHi, base);
                        const auto& tv = child.times;
                        auto lb = std::lower_bound(tv.begin(), tv.end(), lo);
                        auto ub = std::lower_bound(tv.begin(), tv.end(), hi);
                        const size_t kept = size_t(ub - lb);
                        sub.count += kept;
                        if (kept) {
                            sub.first = std::min(sub.first, base + *lb);
                            sub.last = std::max(sub.last, base + *(ub - 1));
                        }
                    }
                } else if (!child.dynamic && noClip) {
                    if (reps) {
                        sub.count = reps * child.statCount;
                        sub.first = timeBase + op.anchorUs + child.statFirst;
                        sub.last = timeBase + op.anchorUs + int64_t(reps - 1) * spacing +
                                  child.statLast;
                    } else {
                        rangeKnown = false;
                    }
                } else {
                    // Dynamic-ish clip or huge rep count: expand per rep
                    // (the decoder pays the same cost).
                    for (uint64_t r = 0; r < reps; ++r) {
                        const int64_t base = timeBase + op.anchorUs + int64_t(r) * spacing;
                        if (!countPatternOps(op.patternId, base, clipLo, clipHi, depth + 1,
                                             sub, err))
                            return false;
                    }
                    if (sub.count == 0) rangeKnown = false;
                }
            }
        } else if (op.kind == kGroup) {
            for (const Op::GEnt& ge : op.entries) {
                if (ge.patternId >= patterns.size()) { err = "bad pattern id"; return false; }
                PatternDoc& child = patterns[ge.patternId];
                if (!child.statReady && !buildStat(ge.patternId, depth + 1, err)) return false;
                const int64_t spacing = int64_t(child.spanUs);
                if (ge.repeatCount > 1 && !child.dynamic && child.statCount &&
                    child.statLast - child.statFirst > spacing) {
                    err = "group entry repeat spacing smaller than pattern content";
                    return false;
                }
                for (uint32_t r = 0; r < ge.repeatCount; ++r) {
                    const int64_t base = timeBase + ge.anchorUs + int64_t(r) * spacing;
                    if (!countPatternOps(ge.patternId, base, ge.winLoUs, ge.winHiUs,
                                         depth + 1, sub, err))
                        return false;
                }
            }
        }
        if (rangeKnown && sub.count) {
            if (sub.first < prevLast) {
                err = "ops overlap; decoded events would be out of order";
                return false;
            }
            prevLast = sub.last;
        } else if (unknownRange) {
            prevLast = INT64_MIN;
        }
        out.count += sub.count;
        out.first = std::min(out.first, sub.first);
        out.last = std::max(out.last, sub.last);
    }
    return true;
}

bool Doc::countPatternOps(size_t pid, int64_t timeBase, int64_t clipLo,
                          int64_t clipHi, int depth, CountOut& out, std::string& err) {
    if (pid >= patterns.size()) { err = "bad pattern id"; return false; }
    PatternDoc& child = patterns[pid];
    if (!child.statReady && !buildStat(pid, depth, err)) return false;
    if (!child.dynamic && clipLo == INT64_MIN && clipHi == INT64_MAX) {
        out.count += child.statCount;
        out.first = std::min(out.first, timeBase + child.statFirst);
        out.last = std::max(out.last, timeBase + child.statLast);
        return true;
    }
    if (!child.dynamic && child.hasTimes) {
        const auto& tv = child.times;
        const int64_t lo = clipMinus(clipLo, timeBase), hi = clipMinus(clipHi, timeBase);
        auto lb = std::lower_bound(tv.begin(), tv.end(), lo);
        auto ub = std::lower_bound(tv.begin(), tv.end(), hi);
        const size_t kept = size_t(ub - lb);
        out.count += kept;
        if (kept) {
            out.first = std::min(out.first, timeBase + *lb);
            out.last = std::max(out.last, timeBase + *(ub - 1));
        }
        return true;
    }
    return countOps(child.ops, timeBase, clipLo, clipHi, depth + 1, out, err);
}

// ------------------------------------------------------------ analyze -----

bool Doc::analyze(std::string& err) {
    err.clear();
    invalidateStats();
    // Validate every pattern def (referenced or not): buildStat enforces
    // internal op ordering and repeat-spacing rules.
    for (size_t i = 0; i < patterns.size(); ++i) {
        if (!buildStat(i, 0, err)) {
            err = "pattern " + std::to_string(i) + ": " + err;
            return false;
        }
    }
    uint64_t expandedTotal = 0, literalEvents = 0, uniqEv = 0, uniqNotes = 0;
    int64_t prevLast = INT64_MIN;
    for (ChunkDoc& ch : chunks) {
        CountOut o;
        if (!countOps(ch.ops, 0, INT64_MIN, INT64_MAX, 0, o, err)) return false;
        if (o.count == 0 || o.count > raid::kMaxChunkExpand) {
            err = "chunk expands to " + std::to_string(o.count) +
                  " events (must be 1.." + std::to_string(raid::kMaxChunkExpand) + ")";
            return false;
        }
        if (o.first == INT64_MAX || o.first < 0 || o.last < o.first || o.first < prevLast) {
            err = "chunk time ranges overlap or are unordered after edits";
            return false;
        }
        prevLast = o.last;
        ch.expanded = o.count;
        ch.firstTimeUs = o.first;
        ch.lastTimeUs = o.last;
        expandedTotal += o.count;
    }
    // Stats for the Info block: literal events in chunks, stored events and
    // stack-weighted notes in the pattern table (stackSum was decoded with
    // each literal op at parse time).
    for (const ChunkDoc& ch : chunks)
        for (const Op& op : ch.ops)
            if (op.kind == kLitAbs) literalEvents += op.n;
    for (const PatternDoc& p : patterns)
        for (const Op& op : p.ops)
            if (op.kind == kLitAbs || op.kind == kLitRel) {
                uniqEv += op.n;
                uniqNotes += op.stackSum ? op.stackSum : op.n;
            }
    info.logicalEvents = expandedTotal;
    info.literalEvents = literalEvents;
    info.uniqueEvents = uniqEv;
    info.uniqueNotes = uniqNotes;
    info.chunkCount = chunks.size();
    info.patternCount = patterns.size();
    return true;
}

// ------------------------------------------------------------ save --------

bool Doc::save(const std::string& outPath, raid::Info* stats, std::string& err) {
    if (!analyze(err)) return false;
    const std::string& dest = outPath.empty() ? path : outPath;

    // ---- pattern section -------------------------------------------------
    Buf pb;
    pb.u32(uint32_t(patterns.size()));
    for (const PatternDoc& p : patterns) {
        Buf stream;
        stream.u32(uint32_t(p.ops.size()));
        for (const Op& op : p.ops) {
            std::vector<char> ob;
            if (!serializeOp(op, ob)) { err = "cannot serialize pattern op"; return false; }
            stream.raw(ob.data(), ob.size());
        }
        pb.u64(p.spanUs);
        pb.u8(p.hasPitch ? 1 : 0);
        pb.u32(uint32_t(stream.b.size()));
        pb.raw(stream.b.data(), stream.b.size());
    }
    {
        Buf t;
        t.u32(uint32_t(mz_crc32(0, reinterpret_cast<const unsigned char*>(pb.b.data()),
                                pb.b.size())));
        pb.raw(t.b.data(), t.b.size());
    }
    std::vector<char> patSection = std::move(pb.b);

    // ---- chunks ----------------------------------------------------------
    std::vector<raid::DocChunkOut> outs;
    outs.reserve(chunks.size());
    uint64_t firstEvent = 0;
    for (const ChunkDoc& ch : chunks) {
        Buf payload;
        payload.u32(uint32_t(ch.ops.size()));
        for (const Op& op : ch.ops) {
            std::vector<char> ob;
            if (!serializeOp(op, ob)) { err = "cannot serialize chunk op"; return false; }
            payload.raw(ob.data(), ob.size());
        }
        payload.u64(ch.expanded);
        raid::DocChunkOut d;
        d.payload = std::move(payload.b);
        d.firstEvent = firstEvent;
        d.firstTimeUs = ch.firstTimeUs;
        d.lastTimeUs = ch.lastTimeUs;
        d.expanded = uint32_t(ch.expanded);
        d.opCount = uint32_t(ch.ops.size());
        firstEvent += ch.expanded;
        outs.push_back(std::move(d));
    }

    raid::EncodeOptions opt;
    if (!raid::writeRaidDoc(dest, patSection, outs, info.logicalEvents,
                            info.literalEvents, info.uniqueEvents,
                            info.uniqueNotes, metaJson, opt, stats, err))
        return false;
    dirty = false;
    return true;
}

}  // namespace gh::raid::doc
