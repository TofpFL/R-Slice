package funkin.external.raid;

#if (windows && cpp)
/**
 * Phase 4 GL smoke test bindings — instanced draw calls inside Lime's
 * existing OpenGL context. Native side: project/src/raid_gl_smoke.cpp.
 *
 * Enable at runtime with env vars (no rebuild needed):
 *   RAID_GL_SMOKE=1        master switch
 *   RAID_GL_SMOKE_N=200000 instance count (default 200000)
 *   RAID_GL_SMOKE_MODE=0   0 static, 1 dynamic subData, 2 orphan,
 *                          3 indirect draw
 */
@:build(funkin.util.macro.LinkerMacro.xml('project/Build.xml'))
@:include('raid_bridge.hpp')
@:unreflective
extern class RaidGLSmoke
{
	/** Capabilities report; valid until next call. */
	@:native("rs_gl_probe")
	public static function probe():cpp.ConstCharStar;

	/** One instanced-draw frame; returns ms or -1 on failure. */
	@:native("rs_gl_smoke_frame")
	public static function frame(count:Int, mode:Int):Float;

	/** Rolling stats; valid until next call. */
	@:native("rs_gl_smoke_stats")
	public static function stats():cpp.ConstCharStar;

	/**
	 * Vendor-neutral GL capability bitmask for fallback decisions.
	 * Bits: 0 VAO, 1 INSTANCED, 2 DIVISOR, 3 INDIRECT, 4 MULTI_INDIRECT,
	 * 5 COMPUTE, 6 SSBO, 7 BUFFER_STORAGE, 8 MAP_RANGE, 9 TEX_STORAGE,
	 * 10 FENCE_SYNC, 11 COMPUTE_OK, 12 PERSISTENT_MAP, 13 CORE_PROFILE,
	 * 14 GLSL_3_30.
	 */
	@:native("rs_gl_caps")
	public static function caps():Int;

	/**
	 * Hardware limit query for renderer sizing. `which`:
	 * 0=MAX_TEXTURE_SIZE, 1=MAX_VERTEX_ATTRIBS,
	 * 2=MAX_SHADER_STORAGE_BLOCK_BINDINGS,
	 * 3=MAX_COMPUTE_WORK_GROUP_INVOCATIONS, 4=MAX_UNIFORM_BLOCK_SIZE.
	 * Returns -1 when unsupported.
	 */
	@:native("rs_gl_limit")
	public static function limit(which:Int):Float;

	// ---- Phase 5D note renderer (instanced GPU positioning) ----

	/** Create a note renderer slot; -1 on failure. */
	@:native("rs_notes_create")
	public static function notesCreate():Int;

	@:native("rs_notes_destroy")
	public static function notesDestroy(h:Int):Void;

	@:native("rs_notes_reset")
	public static function notesReset(h:Int):Void;

	/**
	 * Upload straight-alpha RGBA pixels as the note atlas texture.
	 * `pixPtr` is a `lime.utils.DataPointer` (raw address as Float).
	 */
	@:native("rs_notes_setTexture")
	public static function notesSetTexture(h:Int, pixPtr:Float, w:Int, hh:Int):Int;

	/** Per-direction constants, 16 floats (see raid_gl_smoke.hpp layout). */
	@:native("rs_notes_setDir")
	public static function notesSetDir(h:Int, dir:Int,
		v0:Float, v1:Float, v2:Float, v3:Float,
		v4:Float, v5:Float, v6:Float, v7:Float,
		v8:Float, v9:Float, v10:Float, v11:Float,
		v12:Float, v13:Float, v14:Float, v15:Float):Void;

	@:native("rs_notes_add")
	public static function notesAdd(h:Int, time:Float, dir:Int):Void;

	@:native("rs_notes_kill")
	public static function notesKill(h:Int, index:Int):Void;

	@:native("rs_notes_removePrefix")
	public static function notesRemovePrefix(h:Int, n:Int):Void;

	/**
	 * Draw the instanced batch. Returns 1 when submitted, 0 when the
	 * renderer isn't ready — caller should use the CPU path instead.
	 */
	@:native("rs_notes_draw")
	public static function notesDraw(h:Int,
		now:Float, pxPerMs:Float, dirSign:Float,
		strumX:Float, strumY:Float, scrollX:Float, scrollY:Float,
		r00:Float, r01:Float, r02:Float,
		r10:Float, r11:Float, r12:Float,
		cullX0:Float, cullY0:Float, cullX1:Float, cullY1:Float):Int;
}
#end
