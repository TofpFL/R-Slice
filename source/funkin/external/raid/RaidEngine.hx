package funkin.external.raid;

#if (windows && cpp)
/**
 * Haxe bindings for the R-Slice RAID playback engine C ABI
 * (R-Slice-Main/src/engine_abi.h), reached through the C++ shim in
 * project/src/raid_bridge.cpp which is compiled and linked by
 * project/Build.xml.
 *
 * Pull model: the caller supplies song time and drains due events.
 * Event fields are read via [[field]] so no native pointers or structs
 * cross into Haxe.
 */
@:build(funkin.util.macro.LinkerMacro.xml('project/Build.xml'))
@:include('raid_bridge.hpp')
@:unreflective
extern class RaidEngine
{
	/**
	 * Opens a .raid chart file. Returns null on failure (reason is
	 * printed to stderr by the native side).
	 */
	@:native("rs_engine_open")
	public static function open(path:cpp.ConstCharStar):cpp.Pointer<cpp.Void>;

	/** Closes an engine handle. */
	@:native("rs_engine_close")
	public static function close(engine:cpp.Pointer<cpp.Void>):Void;

	/**
	 * Polls events due around `nowMs` (song milliseconds).
	 * Returns the number of events written; read them with [[field]].
	 */
	@:native("rs_engine_poll")
	public static function poll(engine:cpp.Pointer<cpp.Void>, nowMs:Float):Int;

	/**
	 * Reads field `field` (0-9) of event `index` from the last poll:
	 * 0 timeMs, 1 lane, 2 side, 3 lengthMs, 4 kind, 5 noteKind,
	 * 6 noteFlags, 7 noteType, 8 stack, 9 id.
	 */
	@:native("rs_event_field")
	public static function field(index:Int, field:Int):Float;

	/**
	 * Flat buffer of the last poll's events: 6 doubles per event in the
	 * order [timeMs, lane, side, lengthMs, kind, stack]. Read as
	 * `flat[eventIndex * 6 + field]`. Valid until the next poll.
	 */
	@:native("rs_events_flat")
	public static function eventsFlat():cpp.Pointer<Float>;

	/** Rewinds the stream cursor to `tMs`. Buffered events are dropped. */
	@:native("rs_engine_seek")
	public static function seek(engine:cpp.Pointer<cpp.Void>, tMs:Float):Int;

	/** Engine kind: 0 = json, 1 = raid, -1 = invalid handle. */
	@:native("rs_engine_kind")
	public static function kind(engine:cpp.Pointer<cpp.Void>):Int;

	/** Total logical events the engine reports (0 when unknown). */
	@:native("rs_engine_logical_events")
	public static function logicalEvents(engine:cpp.Pointer<cpp.Void>):Float;

	/**
	 * Stack-weighted note total for one strumline side — 0 opponent,
	 * 1 player, 2 gf, -1 all sides (0 when unknown). The engine decodes
	 * once and caches every side.
	 */
	@:native("rs_engine_note_total")
	public static function noteTotal(engine:cpp.Pointer<cpp.Void>, side:Int):Float;

	/** 1 when the engine supports seek(), else 0. */
	@:native("rs_engine_seekable")
	public static function seekable(engine:cpp.Pointer<cpp.Void>):Int;

	/** Optional decode warm-up for [tMs, tMs+horizonMs); no-op on JSON. */
	@:native("rs_engine_prefetch")
	public static function prefetch(engine:cpp.Pointer<cpp.Void>, tMs:Float, horizonMs:Float):Void;

	/** Starting tempo in BPM from the chart metadata (0 when unknown). */
	@:native("rs_engine_bpm")
	public static function bpm(engine:cpp.Pointer<cpp.Void>):Float;

	/**
	 * Reads the .raid meta JSON section directly. Returns the JSON string,
	 * or null when the file is not a readable .raid chart. The returned
	 * string is valid only until the next call.
	 */
	@:native("rs_raid_meta_json")
	public static function metaJson(path:cpp.ConstCharStar):cpp.ConstCharStar;

	/**
	 * Chunk decode-cache stats as a static string
	 * ("hits=.. misses=.. decodes=.. bytes=.. cached=.. cap=.."),
	 * "json" for materialized charts, or null on a bad handle.
	 * Valid only until the next call.
	 */
	@:native("rs_engine_cache_stats")
	public static function cacheStats(engine:cpp.Pointer<cpp.Void>):cpp.ConstCharStar;

	/**
	 * Reads the .raid op log for [startMs, endMs): the pattern-call view
	 * of the chart. Returns the segment count written (<= 512); read
	 * fields with [[opField]]. Decodes op streams synchronously — call
	 * sparingly (once per second or so).
	 */
	@:native("rs_engine_oplog")
	public static function opLog(engine:cpp.Pointer<cpp.Void>, startMs:Float, endMs:Float):Int;

	/** Segment count from the last [[opLog]] call. */
	@:native("rs_engine_oplog_count")
	public static function opLogCount(engine:cpp.Pointer<cpp.Void>):Int;

	/**
	 * Reads field `field` (0-7) of op segment `index` from the last
	 * opLog: 0 startMs, 1 endMs, 2 chunk, 3 kind (0 literal, 2 pattern),
	 * 4 count (literal events / patternId), 5 aux (repeatCount),
	 * 6 pitchDelta, 7 laneDelta.
	 */
	@:native("rs_op_field")
	public static function opField(index:Int, field:Int):Float;

	/**
	 * Last pump/decode error as a string, or null when the feed is
	 * healthy / the handle is bad. Lets callers distinguish "no events
	 * due" from a stalled stream.
	 */
	@:native("rs_engine_last_error")
	public static function lastError(engine:cpp.Pointer<cpp.Void>):cpp.ConstCharStar;
}
#end
