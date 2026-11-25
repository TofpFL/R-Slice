package funkin.external.raid;

/**
 * CPU profiler for the RAID pipeline. Enable with `RAID_PROFILE=1`.
 *
 * Sections accumulate CPU milliseconds per frame; `RaidChartStream.pump`
 * reports per-report-window averages plus frame-time percentiles
 * (p50/p95/p99/max) appended to the RAID-STREAM log line as `prof{...}`.
 * Work counters (records scanned, quads emitted, events polled) are
 * reported the same way. All hooks are no-ops when disabled.
 */
class RaidProf
{
	public static var enabled:Bool = Sys.getEnv("RAID_PROFILE") != null;

	/** Section name -> accumulated ms since last report. */
	static var acc:Map<String, Float> = new Map<String, Float>();
	/** Section name -> start stamp in seconds (haxe.Timer.stamp). */
	static var t0s:Map<String, Float> = new Map<String, Float>();
	/** Work counters, reset each report window. */
	static var counters:Map<String, Int> = new Map<String, Int>();

	public static inline function stamp():Float
	{
		return enabled ? haxe.Timer.stamp() : 0.0;
	}

	public static function begin(name:String):Void
	{
		if (enabled) t0s.set(name, haxe.Timer.stamp());
	}

	public static function end(name:String):Void
	{
		var t0:Null<Float> = t0s.get(name);
		if (t0 == null) return;
		t0s.remove(name);
		var cur:Null<Float> = acc.get(name);
		acc.set(name, (cur == null ? 0.0 : cur) + (haxe.Timer.stamp() - t0) * 1000.0);
	}

	public static function count(name:String, n:Int = 1):Void
	{
		if (!enabled) return;
		var cur:Null<Int> = counters.get(name);
		counters.set(name, (cur == null ? 0 : cur) + n);
	}

	/**
	 * Called once per RAID-STREAM report tick. Returns the `prof{...}`
	 * fragment and resets accumulators, or "" when disabled.
	 */
	public static function reportAndReset():String
	{
		if (!enabled) return "";
		var sb:StringBuf = new StringBuf();
		sb.add(" prof{");
		var names:Array<String> = [for (k in acc.keys()) k];
		names.sort(Reflect.compare);
		for (n in names)
		{
			var v:Null<Float> = acc.get(n);
			if (v != null && v > 0.005)
			{
				sb.add(' $n=');
				sb.add(Math.round(v * 10) / 10);
				sb.add("ms");
			}
		}
		var cnames:Array<String> = [for (k in counters.keys()) k];
		cnames.sort(Reflect.compare);
		for (n in cnames)
		{
			var v:Null<Int> = counters.get(n);
			if (v != null)
			{
				sb.add(' $n=');
				sb.add(v);
			}
		}
		sb.add(" }");
		acc.clear();
		counters.clear();
		return sb.toString();
	}
}
