// RAID Engine: the .raid binary chart format and its streaming decoder.
//
// Format layout (all integers little-endian, version 1):
//
//   [0..127]   fixed header: magic "RAID", version, flags, keyCount,
//              logicalEventCount (u64: the expanded event count, which may
//              exceed what fits in memory), chunkCount, patternCount,
//              offsets/sizes of the chunk index, meta and pattern sections,
//              and a CRC32 of the header.
//   [index]    chunkCount fixed-size entries (see kIndexEntrySize). Each
//              entry locates one independently compressed chunk and records
//              the expanded event range it covers, so playback can binary
//              search by time without decoding anything.
//   [meta]     deflate-compressed JSON chart metadata (title, tempo,
//              sections, note types, encoder stats). No events. The
//              decompressed payload ends with a CRC32 of the JSON bytes.
//   [patterns] deflate-compressed pattern table: reusable event sequences
//              with pitch/lane transforms (see raid.cpp for the op encoding).
//              The decompressed payload ends with a CRC32.
//   [chunks]   independently compressed event payloads. Each chunk is a
//              sequence of ops: literal columnar event runs (delta + varint
//              encoded, properties split into separate streams) and pattern
//              instantiations (pattern id, repeat count, time/pitch/lane
//              origins). Chunks decode independently and expand in place,
//              so a trillions-of-events chart never has to be materialized.
//              Each chunk index entry carries the CRC32 of the decompressed
//              payload.
//
// Forward compatibility: readers must accept version == kVersion and ignore
// unknown flag bits; writers set only defined flag bits.
#pragma once

#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chart.hpp"

namespace gh { struct FlpProject; }

namespace gh::raid {

// v2: chunk ops extended with op-3 (pattern groups) and op-4 (bursts).
// v1 files only contain ops 0-2, which decode identically, so the reader
// accepts both; writers always emit kVersion so older builds reject new
// files up front instead of failing mid-decode with "unknown op kind".
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMinVersion = 1;
constexpr char kMagic[4] = {'R', 'A', 'I', 'D'};
constexpr size_t kHeaderSize = 128;
constexpr size_t kIndexEntrySize = 56;
// Safety bound on how many events one chunk may expand to. The encoder never
// exceeds EncodeOptions::chunkEvents; this only stops corrupt/hostile files
// from allocating unbounded memory. Encoders must keep chunkEvents and
// maxPattern at or below this or the file cannot be decoded.
constexpr uint64_t kMaxChunkExpand = 1ull << 22;

// One decoded note event. Times are microseconds (Chart stores ms doubles;
// the bridge converts with llround(ms * 1000), lossless for ms-precision
// charts). pitch is a MIDI note number or -1 when the source has none.
struct Event {
    int64_t timeUs = 0;
    uint32_t lengthUs = 0;
    int32_t pitch = -1;
    uint8_t lane = 0;
    uint8_t side = 0;    // 0 opponent, 1 player (Chart::Note::side)
    uint8_t kind = 0;    // 0 normal, 1 mine (NoteKind)
    uint8_t flags = 0;   // NoteFlag bits
    uint16_t type = 0;   // index into Chart::noteTypes
    uint16_t stack = 1;  // merged notes this event counts for
};

struct Info {
    uint32_t version = 0;
    uint32_t flags = 0;
    uint32_t keyCount = 4;
    uint64_t logicalEvents = 0;   // expanded events in the whole file
    uint64_t chunkCount = 0;
    uint64_t patternCount = 0;
    uint64_t literalEvents = 0;   // events stored as literals (after dedup)
    uint64_t uniqueEvents = 0;    // events stored in pattern defs (each once)
    uint64_t uniqueNotes = 0;     // stack-weighted notes in pattern defs
    uint64_t storedBytes = 0;     // chunk payload bytes after deflate
    uint64_t rawBytes = 0;        // chunk payload bytes before deflate
    std::string title, difficulty, opponent, player, gf, stage;
    double lengthMs = 0;
};

// An asset pack recorded in a .raid trailer (see the "embedded packs"
// section at the bottom of this file for the layout).
struct EmbeddedAsset {
    std::string name;           // logical pack name (usually the .rchr name)
    std::vector<char> data;     // raw file bytes, e.g. a complete .rchr
    bool reference = false;     // true: external .rchr reference, not bytes
    std::string refPath;        // reference target when reference == true
};

struct EncodeOptions {
    uint32_t chunkEvents = 8192;    // expanded events per chunk (decode granularity)
                                     // must be <= kMaxChunkExpand
    uint32_t minPattern = 8;        // shortest run worth storing as a pattern (>= 2)
    uint32_t maxPattern = 512;      // longest run the matcher tries (<= kMaxChunkExpand)
    int compressLevel = 6;          // miniz level 1..10
    bool patterns = true;           // false: store every event literally
    uint32_t laneBound = 0;         // reserved for lane-range validation (0 = 256)
    uint64_t maxDetectEvents = 0;   // 0 = no limit; pattern matching is skipped
                                     // above this many input events (memory)
};

// ---------------------------------------------------------------- writer --

// Encodes already time-sorted events. `metaJson` is stored (deflated) in the
// meta section; pass the chart-metadata JSON from raid_chart.cpp or any JSON
// object. Fails honestly: returns false with err set when the input cannot be
// represented (unsorted times, corrupt state...).
bool writeRaid(const std::string& path, const std::vector<Event>& events,
               std::string_view metaJson, const EncodeOptions& opt,
               Info* statsOut, std::string& err);

// Encodes an FL Studio project directly: every pattern's chart notes become
// pattern-table definitions (stored once, split into blocks that fit the
// decoder's per-chunk expansion bound) and each playlist clip becomes a
// pattern-instance op on the timeline — the file decodes as one contiguous
// chart, exactly like a materialized JSON chart, without ever building it.
// Projects without a playlist arrangement lay their chart patterns end to
// end. `song.flp#N` single-pattern mode stays on the Chart path instead.
// `opponentName`/`playerName` optionally stamp the meta character names
// (e.g. from embedded .rchr packs); empty keeps the defaults.
bool writeRaidFlp(const std::string& path, const FlpProject& project,
                  const EncodeOptions& opt, Info* statsOut, std::string& err,
                  std::string_view opponentName = {},
                  std::string_view playerName = {});

// ---------------------------------------------------------------- reader --

// A pattern-table record. `off`/`len` locate the pattern's op stream (a u32
// opCount followed by ops) inside Reader::patternBlob_.
struct PatternRec {
    uint64_t spanUs = 0;        // repeat period in microseconds
    bool hasPitch = false;      // false: every decoded pitch is -1
    uint32_t off = 0, len = 0;
};

// Random-access reader over a .raid file. Only the header and chunk index
// are resident after open(); meta, patterns and chunks are read on demand.
// Decoded chunks are kept in a small LRU so sequential playback decodes each
// chunk once.
//
// Thread safety: range(), chunkEvents(), metaJson() and chunkEventsAsync()
// may be called concurrently from multiple threads, and the Reader (or the
// process) may be destroyed while async decodes are in flight — workers hold
// their own reference to the file state. File reads are serialized internally;
// decompression and pattern expansion run in parallel. chunkEventsAsync
// decodes at most setMaxConcurrentDecodes() chunks at once and every decode
// is bounded by setMemoryBudgetBytes().
class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();
    bool isOpen() const;
    const Info& info() const;
    const std::string& path() const;

    // The meta JSON (without its crc trailer), or "" when absent/corrupt.
    std::string metaJson();

    // Embedded asset packs from the file's trailer (empty when absent).
    // The trailer CRC is verified; a corrupt trailer is an error, not
    // silently ignored data.
    bool embeddedAssets(std::vector<EmbeddedAsset>& out, std::string& err);

    // Index of the last chunk whose firstTimeUs <= timeUs (-1 before the
    // first chunk).
    int64_t chunkAtOrBefore(int64_t timeUs) const;

    // Decodes chunk i, appending its expanded events to out (LRU cached).
    // Verifies payload size and CRC32 and the expanded-event count.
    bool chunkEvents(size_t i, std::vector<Event>& out, std::string& err);

    // All expanded events with timeUs in [startUs, endUs), in time order.
    // Decodes only the chunks that can contain the range; memory stays
    // proportional to the window, never to the chart's logical size.
    // When `indices` is non-null it receives each event's global expanded
    // index (the value LaneNote::id / script note ids are based on), so
    // streaming consumers keep stable ids without materializing the chart.
    bool range(int64_t startUs, int64_t endUs, std::vector<Event>& out,
               std::string& err, std::vector<uint64_t>* indices = nullptr);

    // Window-sliced variant of range(): identical events and global indices,
    // but cache misses decode only the requested window instead of the
    // whole chunk (repeat arithmetic + window clipping inside the op
    // interpreter). Slices are not inserted into the chunk LRU, so repeated
    // windows over the same chunk re-slice; use for benchmarking and for
    // access patterns where whole-chunk decode is wasteful. range() remains
    // the reference implementation.
    bool rangeWindow(int64_t startUs, int64_t endUs, std::vector<Event>& out,
                     std::vector<uint64_t>& indices, std::string& err);

    // One stretch of a chunk's op stream, in playback order. Only LEAF
    // ops produce segments: literal runs always, and pattern instances
    // whose definitions contain no further pattern ops — patterns that
    // merely wrap other patterns are traversed but not logged. Pattern
    // identity only exists here — expanded events lose it — so this is
    // the API for "which pattern is playing when" logging and analysis.
    struct OpSegment {
        int64_t startUs = 0, endUs = 0;  // covered time range
        uint32_t chunk = 0;              // chunk index
        uint8_t kind = 0;                // 0 = literal run, 2 = pattern instance
        uint32_t count = 0;              // literal: event count; pattern: patternId
        uint32_t aux = 0;                // pattern: repeatCount (0 for literals)
        int32_t pitchDelta = 0, laneDelta = 0;  // pattern transforms
    };

    // Walks the op streams of chunks overlapping [startUs, endUs) and appends
    // their segments in order — literals AND pattern instances, so callers
    // can see exactly where one pattern ends and another begins. Decodes the
    // chunk payloads but does not materialize events; memory stays bounded
    // by one chunk payload at a time.
    bool opLog(int64_t startUs, int64_t endUs, std::vector<OpSegment>& out,
               std::string& err);

    // First/last event time of chunk i (for prefetch planning).
    bool chunkTimeRange(size_t i, int64_t& firstUs, int64_t& lastUs) const;
    // Time of the last event in the file (0 when empty).
    int64_t durationUs() const;

    // Decodes chunk i on a worker thread. Safe to call concurrently with
    // range()/chunkEvents() and to destroy the Reader while the future is
    // outstanding (the worker keeps the file state alive until it finishes).
    std::future<bool> chunkEventsAsync(size_t i, std::vector<Event>* out);
    // Prefetch variant: decodes chunk i into the LRU cache only, with no
    // event copy out. Cheaper and half the memory of chunkEventsAsync for
    // callers that only need the cache warm.
    std::future<bool> warmChunkAsync(size_t i);
    // Batch variant: warms chunks [first, last) inside ONE worker thread.
    // Spawning a std::async per chunk costs ~0.1 ms of thread creation
    // each — a dense prefetch window is hundreds of chunks, which used to
    // monopolize a frame. Batches keep worker parallelism without the
    // thread-storm.
    std::future<bool> warmChunksAsync(size_t first, size_t last);

    // Bounds for concurrent/async decoding. At most `n` chunk decodes run at
    // once across all threads (default 2); `bytes` caps the events a single
    // chunk decode may materialize — a chunk whose expanded count exceeds
    // bytes/sizeof(Event) is rejected with an error instead of allocating
    // beyond the budget (default 128 MiB, still bounded by kMaxChunkExpand).
    // Call before opening or while idle; changing it does not affect decodes
    // already running.
    void setMaxConcurrentDecodes(int n);
    void setMemoryBudgetBytes(uint64_t bytes);
    uint64_t memoryBudgetBytes() const;
    // Resident decoded events in the chunk LRU cache and its capacity
    // (memory-budget derived). For playback memory statistics.
    uint64_t cachedEvents() const;
    uint64_t cachedEventCapacity() const;
    // Chunk-cache instrumentation for density benchmarking: cumulative
    // chunk queries that hit/missed the LRU, actual chunk decodes, and
    // raw payload bytes read for decode. Hit rate exposes when chunk size
    // or access pattern works against the cache.
    void cacheStats(uint64_t& hits, uint64_t& misses,
                    uint64_t& decodes, uint64_t& bytesDecoded) const;

    // Unique notes stored in the pattern table: events physically present in
    // pattern defs (each stored once, never expanded), stack-weighted for
    // `notes`. This is the ghost-note-free count — FL pattern defs only hold
    // events inside playlist clip windows, so notes that live in a pattern
    // but never play are not counted. Decodes the (deflated) pattern section
    // only; chunk payloads are not touched.
    bool uniqueStats(uint64_t& events, uint64_t& notes, std::string& err);

    // ---- editor access -------------------------------------------------
    // Raw structural access for the .raid document editor (raid_doc.cpp).
    // patternBlob() returns the decompressed pattern section INCLUDING its
    // trailing u32 crc; patternRecs() the per-pattern table records.
    // chunkPayloadRaw() returns chunk i's decompressed payload:
    // u32 opCount + ops + u64 expandedCount trailer (crc verified).
    bool patternBlob(std::vector<char>& out, std::string& err);
    bool patternRecs(std::vector<PatternRec>& out, std::string& err);
    bool chunkPayloadRaw(size_t i, std::vector<char>& out, std::string& err);

    uint64_t logicalEvents() const { return info().logicalEvents; }

private:
    struct ChunkEnt {
        uint64_t firstEvent = 0;
        int64_t firstTimeUs = 0, lastTimeUs = 0;
        uint64_t offset = 0;
        uint32_t storedSize = 0, rawSize = 0;
        uint32_t expanded = 0, opCount = 0;
        uint32_t crc = 0, flags = 0;   // flags bit0: payload is deflated
    };
    struct Cached {
        size_t index = 0;
        uint64_t stamp = 0;
        std::vector<Event> events;
    };

    struct Impl;
    std::shared_ptr<Impl> impl_;
    int decTotal_ = 2;
    uint64_t memBudget_ = 128ull << 20;
};

// Incremental op-stream logger. Call tick(horizonUs) as playback advances;
// it walks only the chunks covering newly-played time and appends each
// literal run / pattern instance to `path` in playback order, marking every
// pattern->pattern change. Pattern identity exists only in the op stream
// (expanded events lose it), so this is the tool for "which pattern is
// playing when". Memory stays bounded: it never materializes events, and
// already-logged chunks are skipped by a per-chunk ordinal.
class PatternLogger {
public:
    PatternLogger() = default;
    ~PatternLogger() { close(); }
    PatternLogger(const PatternLogger&) = delete;
    PatternLogger& operator=(const PatternLogger&) = delete;

    // Opens `path` for writing (truncates any previous log). `rd` must
    // outlive the logger.
    bool open(Reader& rd, const std::string& path, std::string& err);
    // Flushes the tail and closes the file.
    void close();

    // Logs every op segment starting before horizonUs that has not been
    // logged yet. horizonUs must be non-decreasing (song time in us);
    // close() flushes the remainder.
    bool tick(int64_t horizonUs, std::string& err);

    uint64_t patternInstances() const { return instances_; }
    uint64_t patternChanges() const { return changes_; }
    bool isOpen() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }

private:
    Reader* rd_ = nullptr;
    FILE* f_ = nullptr;
    std::string path_;
    int64_t winStartUs_ = 0;
    int64_t prevHorizonUs_ = -1;
    std::unordered_map<uint32_t, uint32_t> written_;  // chunk -> segments logged
    int64_t prevPatId_ = -1;  // last pattern id; -1 right after a literal
    uint64_t instances_ = 0, changes_ = 0;
};

// One chunk as prepared by an editor: `payload` is the complete sealed
// chunk payload (u32 opCount + ops + u64 expandedCount). firstEvent is the
// chunk's global expanded-event index; firstTimeUs/lastTimeUs its event
// time range (index entries require non-decreasing, non-overlapping ranges).
struct DocChunkOut {
    std::vector<char> payload;
    uint64_t firstEvent = 0;
    int64_t firstTimeUs = 0, lastTimeUs = 0;
    uint32_t expanded = 0, opCount = 0;
};

// Writes a .raid file from editor-prepared sections: `patSection` is the
// raw (uncompressed) pattern table (u32 count + records + u32 crc), chunks
// carry their sealed payloads. Same layout/validation as writeRaid.
bool writeRaidDoc(const std::string& path, const std::vector<char>& patSection,
                  std::vector<DocChunkOut>& chunks, uint64_t logicalEvents,
                  uint64_t literalEvents, uint64_t uniqueEvents,
                  uint64_t uniqueNotes, std::string_view metaJson,
                  const EncodeOptions& opt, Info* statsOut, std::string& err);

// Minimum format version a sealed chunk payload requires (1 or kVersion).
// See raid.cpp payloadHasV2Ops.
uint32_t chunkPayloadVersion(const std::vector<char>& payload);

// Quick magic check without parsing the index.
bool isRaidFile(const std::string& path);

// ------------------------------------------------------- embedded packs --
//
// A .raid file may carry embedded asset packs — typically .rchr character
// archives — in an optional trailer appended after the last chunk:
//
//   [assets]  v1 (footer reserved = 0): u32 packCount, then per pack:
//             u32 nameLen + name bytes + u64 dataSize + raw data bytes
//   [assets]  v2 (footer reserved = 1): u32 packCount, then per pack:
//             u8 type (0 = embedded, 1 = reference) + u32 nameLen + name +
//             u64 size + payload. Embedded payload = raw file bytes;
//             reference payload = UTF-8 path to an external .rchr.
//   [footer]  24 bytes at EOF: magic "RAIDASST" (8), u64 assetsOffset,
//             u32 crc32 of the assets blob, u32 reserved (= blob version)
//
// The trailer sits past every section the header/index describe, so readers
// that predate it simply never look there — no version bump is needed and
// old builds still play the chart (with default sprites). New readers find
// the footer by reading the last 24 bytes. v2 records are not understood by
// pre-v2 readers: they fail the trailer parse and skip the packs, which
// degrades to default sprites rather than breaking the chart.

// Appends the assets trailer to an existing .raid file, replacing any
// trailer already present. `assets` may be empty to strip a trailer.
// Fails when `path` is not a raid file.
bool embedAssets(const std::string& path,
                 const std::vector<EmbeddedAsset>& assets, std::string& err);

}  // namespace gh::raid
