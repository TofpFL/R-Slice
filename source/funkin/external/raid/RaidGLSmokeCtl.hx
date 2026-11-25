package funkin.external.raid;

#if (windows && cpp)
/**
 * Haxe-side driver for the Phase 4 GL smoke test. Call [[maybeInstall]]
 * once from PlayState.create; it no-ops unless RAID_GL_SMOKE is set.
 */
@:build(funkin.util.macro.LinkerMacro.xml('project/Build.xml'))
@:include('raid_bridge.hpp')
class RaidGLSmokeCtl
{
	static var _glCount:Int = 200000;
	static var _glMode:Int = 0;
	static var _glFrames:Int = 0;
	static var _installed:Bool = false;

	public static function maybeInstall():Void
	{
		if (_installed) return;
		if (Sys.getEnv("RAID_GL_SMOKE") == null) return;
		_installed = true;
		var report:String = RaidGLSmoke.probe();
		if (report != null) trace(report);
		_glCount = Std.parseInt(Sys.getEnv("RAID_GL_SMOKE_N") ?? "200000");
		if (_glCount <= 0) _glCount = 200000;
		_glMode = Std.parseInt(Sys.getEnv("RAID_GL_SMOKE_MODE") ?? "0");
		if (_glMode < 0 || _glMode > 3) _glMode = 0;
		_glFrames = 0;
		flixel.FlxG.signals.postDraw.add(_onPostDraw);
		trace('RAID_GL_SMOKE active: count=' + _glCount + ' mode=' + _glMode);
	}

	static function _onPostDraw():Void
	{
		var ms = RaidGLSmoke.frame(_glCount, _glMode);
		if (ms < 0)
		{
			flixel.FlxG.signals.postDraw.remove(_onPostDraw);
			return;
		}
		_glFrames++;
		if (_glFrames % 120 == 0)
		{
			var s:String = RaidGLSmoke.stats();
			if (s != null) trace(s);
		}
	}
}
#end
