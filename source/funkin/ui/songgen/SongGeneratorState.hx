package funkin.ui.songgen;

import funkin.util.FileUtil;
import openfl.net.FileFilter;

#if sys
import haxe.Json;
import openfl.display.BitmapData;
import openfl.display.PNGEncoderOptions;
import openfl.events.KeyboardEvent;
import openfl.Lib;
import openfl.geom.Matrix;
import openfl.text.TextField;
import openfl.text.TextFormat;
import openfl.ui.Keyboard;
import sys.FileSystem;
import sys.io.File;
#end

import flixel.FlxG;
import flixel.FlxState;
import flixel.text.FlxText;
import flixel.text.FlxText.FlxTextAlign;
import funkin.data.song.SongData;
import funkin.ui.MusicBeatSubState;
import funkin.ui.debug.DebugMenuSubState;

enum GenMode
{
  HUB;
  FORM;
  PICKSONG;
}

enum FieldKind
{
  TEXT;
  CHOICE;
  FILE;
  ACTION;
}

class GenField
{
  public var label:String;
  public var kind:FieldKind;
  public var getText:Void->String;
  public var setText:Null<String->Void> = null;
  public var onLeft:Null<Void->Void> = null;
  public var onRight:Null<Void->Void> = null;
  public var onAccept:Null<Void->Void> = null;

  public function new(label:String, kind:FieldKind, getText:Void->String, ?setText:String->Void, ?onLeft:Void->Void, ?onRight:Void->Void,
      ?onAccept:Void->Void)
  {
    this.label = label;
    this.kind = kind;
    this.getText = getText;
    this.setText = setText;
    this.onLeft = onLeft;
    this.onRight = onRight;
    this.onAccept = onAccept;
  }
}

/**
 * Character picker state: cycles registered character IDs, with a final
 * "custom" slot that accepts a `.rchr` pack (or preserves an unknown ID
 * loaded from an existing song).
 */
class CharSel
{
  public var idx:Int = 0;
  public var onCustom:Bool = false;
  public var customPath:Null<String> = null;

  public function new(?idx:Int)
  {
    if (idx != null) this.idx = idx;
  }
}

/**
 * Freeplay Song Generator.
 *
 * Lets the user scaffold a complete Freeplay-ready song package at runtime:
 * song metadata + chart JSON, a level entry (capsule text), optional album
 * art/title, audio files (any supported format), `.raid` charts, and `.rchr`
 * character packs. Also supports editing an existing song's data files.
 *
 * Reachable from the main menu with TAB, or from the debug menu.
 */
class SongGeneratorState extends MusicBeatSubState
{
  static final ROW_H:Float = 36;
  static final ROW_TOP:Float = 110;
  static final MAX_ROWS:Int = 16;

  static final DIFF_PRESETS:Array<Array<String>> = [
    ['easy'],
    ['normal'],
    ['hard'],
    ['easy', 'normal'],
    ['normal', 'hard'],
    ['easy', 'normal', 'hard']
  ];

  static final FALLBACK_CHARS:Array<String> = ['bf', 'gf', 'dad', 'pico', 'monster', 'bf-car', 'gf-car'];
  static final FALLBACK_STAGES:Array<String> = ['mainStage'];
  static final FALLBACK_NOTESTYLES:Array<String> = ['funkin', 'hrk', 'pixel'];
  static final HUB_ITEMS:Array<String> = ['NEW SONG', 'EDIT SONG', 'BACK'];

  var mode:GenMode = HUB;
  var returnFactory:Void->FlxState;

  var cursor:Int = 0;
  var scroll:Int = 0;
  var fields:Array<GenField> = [];
  var rows:Array<FlxText> = [];
  var titleText:FlxText;
  var statusText:FlxText;
  var hintText:FlxText;

  var editing:Null<GenField> = null;
  var editBuffer:String = '';

  // ---- form data ----
  var songId:String = 'mysong';
  var songName:String = '';
  var artist:String = '';
  var charter:String = '';
  var bpmStr:String = '120';
  var ratingStr:String = '5';
  var diffPreset:Int = 1;
  var playerSel:CharSel = new CharSel(0);
  var oppSel:CharSel = new CharSel(1);
  var gfSel:CharSel = new CharSel(1);
  var stageIdx:Int = 0;
  var noteskinIdx:Int = 0;
  var instSrc:Null<String> = null;
  var pvSrc:Null<String> = null;
  var ovSrc:Null<String> = null;
  var raidSrc:Null<String> = null;
  var albumArtSrc:Null<String> = null;
  var capsuleText:String = '';
  var albumName:String = '';
  var albumIdStr:String = '';
  var levelNameStr:String = '';
  var levelBg:String = '#FF6B6B';
  var statusMsg:String = '';

  var srcAssets:Null<String> = null;

  var charIds:Array<String> = [];
  var stageIds:Array<String> = [];
  var noteskinIds:Array<String> = [];
  var songChoices:Array<String> = [];

  public function new(?returnFactory:Void->FlxState)
  {
    super();
    this.returnFactory = returnFactory ?? (() -> new DebugMenuSubState());
  }

  override function create():Void
  {
    trace('SONGGEN: state created');
    super.create();

    bgColor = 0xFF14141C;

    #if sys
    Lib.current.stage.addEventListener(KeyboardEvent.KEY_DOWN, onKeyDown);
    #end

    detectSourceAssets();
    loadIdLists();
    scanSongFolders();

    titleText = new FlxText(40, 24, 0, 'FREEPLAY SONG GENERATOR', 24);
    add(titleText);

    hintText = new FlxText(FlxG.width - 480, 30, 440, 'UP/DOWN select - LEFT/RIGHT adjust - ENTER edit/accept - ESC back', 10);
    hintText.alignment = FlxTextAlign.RIGHT;
    hintText.color = 0xFF666666;
    add(hintText);

    statusText = new FlxText(40, FlxG.height - 46, FlxG.width - 80, '', 12);
    statusText.color = 0xFF66FF66;
    add(statusText);

    for (i in 0...MAX_ROWS)
    {
      var t = new FlxText(60, ROW_TOP + i * ROW_H, FlxG.width - 120, '', 15);
      add(t);
      rows.push(t);
    }

    mode = HUB;
    cursor = 0;
    refreshRows();
  }

  override function destroy()
  {
    #if sys
    Lib.current.stage.removeEventListener(KeyboardEvent.KEY_DOWN, onKeyDown);
    #end
    super.destroy();
  }

  // ------------------------------------------------------------------
  // Data source listing
  // ------------------------------------------------------------------

  #if sys
  function listDataIds(subdir:String, fallback:Array<String>):Array<String>
  {
    var full = FileUtil.gameDirectory + '/assets/data/' + subdir;
    if (FileSystem.exists(full) && FileSystem.isDirectory(full))
    {
      var out:Array<String> = [];
      for (f in FileSystem.readDirectory(full))
      {
        if (f.endsWith('.json')) out.push(f.substr(0, f.length - 5));
      }
      if (out.length > 0)
      {
        out.sort(Reflect.compare);
        return out;
      }
    }
    return fallback;
  }

  function loadIdLists():Void
  {
    charIds = listDataIds('characters', FALLBACK_CHARS);
    stageIds = listDataIds('stages', FALLBACK_STAGES);
    noteskinIds = listDataIds('notestyles', FALLBACK_NOTESTYLES);
    if (stageIds.indexOf('mainStage') < 0) stageIds.unshift('mainStage');
    if (noteskinIds.indexOf('funkin') < 0) noteskinIds.unshift('funkin');
  }

  function scanSongFolders():Void
  {
    songChoices = [];
    var songsDir = FileUtil.gameDirectory + '/assets/data/songs';
    if (!FileSystem.exists(songsDir) || !FileSystem.isDirectory(songsDir)) return;

    for (entry in FileSystem.readDirectory(songsDir))
    {
      var full = songsDir + '/' + entry;
      if (FileSystem.isDirectory(full))
      {
        if (FileSystem.exists(full + '/' + entry + '-metadata.json')) songChoices.push(entry);
      }
      else if (entry.endsWith('-metadata.json'))
      {
        songChoices.push(entry.substr(0, entry.length - '-metadata.json'.length));
      }
    }
    songChoices.sort(Reflect.compare);
  }
  #else
  function loadIdLists():Void
  {
    charIds = FALLBACK_CHARS.copy();
    stageIds = FALLBACK_STAGES.copy();
    noteskinIds = FALLBACK_NOTESTYLES.copy();
  }

  function scanSongFolders():Void
  {
    songChoices = [];
  }
  #end

  // ------------------------------------------------------------------
  // Navigation / input
  // ------------------------------------------------------------------

  function itemCount():Int
  {
    return switch (mode)
    {
      case HUB: HUB_ITEMS.length;
      case PICKSONG: songChoices.length;
      case FORM: fields.length;
    };
  }

  function moveCursor(dir:Int):Void
  {
    var count = itemCount();
    if (count <= 0) return;
    cursor = (cursor + dir + count) % count;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + MAX_ROWS) scroll = cursor - MAX_ROWS + 1;
    refreshRows();
  }

  function activate():Void
  {
    trace('SONGGEN: activate mode=${mode} cursor=${cursor}');
    if (editing != null) return;
    switch (mode)
    {
      case HUB:
        switch (cursor)
        {
          case 0: resetForm(); mode = FORM; cursor = 0; scroll = 0; statusMsg = ''; refreshRows();
          case 1: scanSongFolders(); mode = PICKSONG; cursor = 0; scroll = 0; statusMsg = ''; refreshRows();
          case 2: exit();
          default:
        }
      case PICKSONG:
        if (songChoices.length > 0) loadSongForEdit(songChoices[cursor]);
      case FORM:
        var f = fields[cursor];
        if (f.onAccept != null)
        {
          f.onAccept();
        }
        else if (f.setText != null)
        {
          editing = f;
          editBuffer = f.getText();
          refreshRows();
        }
    }
  }

  function adjust(dir:Int):Void
  {
    if (editing != null || mode != FORM) return;
    var f = fields[cursor];
    var cb = dir < 0 ? f.onLeft : f.onRight;
    if (cb != null) cb();
    refreshRows();
  }

  function back():Void
  {
    if (editing != null)
    {
      editing = null;
      refreshRows();
      return;
    }
    switch (mode)
    {
      case FORM, PICKSONG:
        mode = HUB; cursor = 0; scroll = 0; statusMsg = ''; refreshRows();
      case HUB:
        exit();
    }
  }

  function exit():Void
  {
    FlxG.switchState(returnFactory);
  }

  override function update(elapsed:Float):Void
  {
    super.update(elapsed);

    if (editing != null) return; // text entry handled by the key listener

    if (FlxG.keys.justPressed.UP) moveCursor(-1);
    if (FlxG.keys.justPressed.DOWN) moveCursor(1);
    if (FlxG.keys.justPressed.LEFT) adjust(-1);
    if (FlxG.keys.justPressed.RIGHT) adjust(1);
    if (FlxG.keys.justPressed.ENTER || FlxG.keys.justPressed.SPACE) activate();
    if (FlxG.keys.justPressed.ESCAPE || FlxG.keys.justPressed.BACKSPACE) back();
  }

  #if sys
  function onKeyDown(e:KeyboardEvent):Void
  {
    if (editing == null) return;
    var kc:Int = e.keyCode;
    if (kc == Keyboard.ENTER || kc == Keyboard.TAB)
    {
      commitEdit();
    }
    else if (kc == Keyboard.ESCAPE)
    {
      editing = null;
      refreshRows();
    }
    else if (kc == Keyboard.BACKSPACE)
    {
      if (editBuffer.length > 0) editBuffer = editBuffer.substr(0, editBuffer.length - 1);
      refreshRows();
    }
    else if (e.charCode >= 32 && e.charCode < 127)
    {
      editBuffer += String.fromCharCode(e.charCode);
      refreshRows();
    }
  }
  #end

  function commitEdit():Void
  {
    if (editing != null && editing.setText != null) editing.setText(editBuffer);
    editing = null;
    refreshRows();
  }

  // ------------------------------------------------------------------
  // Rendering
  // ------------------------------------------------------------------

  function refreshRows():Void
  {
    var count = itemCount();
    if (count <= 0)
    {
      cursor = 0;
      scroll = 0;
    }
    else
    {
      if (cursor >= count) cursor = count - 1;
      if (cursor < 0) cursor = 0;
    }

    titleText.text = switch (mode)
    {
      case HUB: 'FREEPLAY SONG GENERATOR';
      case FORM: 'EDITING SONG: ${songId}';
      case PICKSONG: 'EDIT EXISTING SONG - pick one';
    };

    for (i in 0...rows.length)
    {
      var t = rows[i];
      var idx = scroll + i;
      if (idx >= count)
      {
        t.visible = false;
        continue;
      }
      t.visible = true;
      t.color = idx == cursor ? 0xFFFFFFFF : 0xFF999999;
      t.text = rowLabel(idx) + rowValue(idx);
    }

    statusText.text = statusMsg;
  }

  function rowLabel(idx:Int):String
  {
    var prefix = idx == cursor ? '> ' : '  ';
    var label = switch (mode)
    {
      case HUB: HUB_ITEMS[idx];
      case PICKSONG: songChoices[idx];
      case FORM: fields[idx].label;
    };
    return prefix + label;
  }

  function rowValue(idx:Int):String
  {
    return switch (mode)
    {
      case HUB, PICKSONG: '';
      case FORM:
        var f = fields[idx];
        var val = editing == f ? '[${editBuffer}_]' : f.getText();
        var pad = 44 - f.label.length;
        return pad > 0 ? padStr(pad) + val : ' ' + val;
    };
  }

  // ------------------------------------------------------------------
  // Form construction
  // ------------------------------------------------------------------

  function resetForm():Void
  {
    songId = 'mysong';
    songName = '';
    artist = '';
    charter = '';
    bpmStr = '120';
    ratingStr = '5';
    diffPreset = 1;
    playerSel = new CharSel(0);
    oppSel = new CharSel(charIds.length > 1 ? 1 : 0);
    gfSel = new CharSel(charIds.length > 1 ? 1 : 0);
    stageIdx = 0;
    noteskinIdx = 0;
    instSrc = null;
    pvSrc = null;
    ovSrc = null;
    raidSrc = null;
    albumArtSrc = null;
    capsuleText = '';
    albumName = '';
    albumIdStr = '';
    levelNameStr = '';
    levelBg = '#FF6B6B';
    statusMsg = '';
    buildFields();
  }

  function currentDiffs():Array<String>
  {
    return DIFF_PRESETS[diffPreset];
  }

  #if sys
  function browseFile(title:String, filters:Array<FileFilter>, onPick:String->Void):Void
  {
    FileUtil.browseForFile(title, filters, function(f) onPick(f.fullPath));
  }
  #end

  function buildFields():Void
  {
    fields = [];

    fields.push(new GenField('SONG ID', TEXT, () -> songId, function(v) songId = sanitizeId(v)));
    fields.push(new GenField('SONG NAME', TEXT, () -> songName, function(v) songName = v));
    fields.push(new GenField('ARTIST', TEXT, () -> artist, function(v) artist = v));
    fields.push(new GenField('CHARTER', TEXT, () -> charter, function(v) charter = v));
    fields.push(new GenField('BPM', TEXT, () -> bpmStr, function(v) bpmStr = v));
    fields.push(new GenField('RATING (0-15)', TEXT, () -> ratingStr, function(v) ratingStr = v));

    fields.push(new GenField('DIFFICULTIES', CHOICE, () -> currentDiffs().join(', '), null,
      function() { diffPreset = (diffPreset + DIFF_PRESETS.length - 1) % DIFF_PRESETS.length; },
      function() { diffPreset = (diffPreset + 1) % DIFF_PRESETS.length; }));

    fields.push(makeCharField('PLAYER', playerSel));
    fields.push(makeCharField('OPPONENT', oppSel));
    fields.push(makeCharField('GIRLFRIEND', gfSel));

    fields.push(new GenField('STAGE', CHOICE, () -> stageIds[stageIdx], null,
      function() { stageIdx = (stageIdx + stageIds.length - 1) % stageIds.length; },
      function() { stageIdx = (stageIdx + 1) % stageIds.length; }));

    fields.push(new GenField('NOTESKIN', CHOICE, () -> noteskinIds[noteskinIdx], null,
      function() { noteskinIdx = (noteskinIdx + noteskinIds.length - 1) % noteskinIds.length; },
      function() { noteskinIdx = (noteskinIdx + 1) % noteskinIds.length; }));

    fields.push(makeFileField('INSTRUMENTAL', function():Null<String> { return instSrc; }, function(v) instSrc = v, 'Select Inst audio',
      FileUtil.FILE_FILTERS_AUDIO));
    fields.push(makeFileField('PLAYER VOCALS', function():Null<String> { return pvSrc; }, function(v) pvSrc = v, 'Select player vocals',
      FileUtil.FILE_FILTERS_AUDIO));
    fields.push(makeFileField('OPPONENT VOCALS', function():Null<String> { return ovSrc; }, function(v) ovSrc = v, 'Select opponent vocals',
      FileUtil.FILE_FILTERS_AUDIO));
    fields.push(makeFileField('RAID CHART (.raid)', function():Null<String> { return raidSrc; }, function(v) raidSrc = v, 'Select .raid chart',
      [FileUtil.FILE_FILTER_RAID]));
    fields.push(makeFileField('ALBUM ART (.png)', function():Null<String> { return albumArtSrc; }, function(v) albumArtSrc = v, 'Select album art',
      [FileUtil.FILE_FILTER_PNG]));

    fields.push(new GenField('CAPSULE TEXT', TEXT, () -> capsuleText, function(v) capsuleText = v));
    fields.push(new GenField('ALBUM NAME', TEXT, () -> albumName, function(v) albumName = v));
    fields.push(new GenField('ALBUM ID', TEXT, () -> albumIdStr, function(v) albumIdStr = sanitizeId(v)));
    fields.push(new GenField('LEVEL NAME', TEXT, () -> levelNameStr, function(v) levelNameStr = v));
    fields.push(new GenField('BACKGROUND', TEXT, () -> levelBg, function(v) levelBg = v));

    fields.push(new GenField('>>> GENERATE / SAVE <<<', ACTION, () -> '', null, null, null, doGenerate));
    fields.push(new GenField('BACK TO MENU', ACTION, () -> '', null, null, null, function() back()));
  }

  function makeCharField(label:String, sel:CharSel):GenField
  {
    var display = function():String
    {
      if (sel.onCustom)
      {
        return sel.customPath != null ? 'RCHR: ' + baseName(sel.customPath) : 'CUSTOM (.rchr) - press ENTER';
      }
      return sel.idx >= 0 && sel.idx < charIds.length ? charIds[sel.idx] : '?';
    };

    var cycle = function(dir:Int):Void
    {
      var len = charIds.length + 1; // last slot = custom .rchr
      var pos = sel.onCustom ? len - 1 : sel.idx;
      pos = (pos + dir + len) % len;
      sel.onCustom = pos == len - 1;
      if (!sel.onCustom) sel.idx = pos;
    };

    var onAccept = function():Void
    {
      if (!sel.onCustom) return;
      #if sys
      FileUtil.browseForFile('Select .rchr character pack', [FileUtil.FILE_FILTER_RCHR], function(f)
      {
        sel.customPath = f.fullPath;
        refreshRows();
      });
      #end
    };

    return new GenField(label, CHOICE, display, null, function() cycle(-1), function() cycle(1), onAccept);
  }

  function makeFileField(label:String, getSrc:Void->Null<String>, setSrc:Null<String>->Void, title:String,
      filters:Array<FileFilter>):GenField
  {
    var display = function():String
    {
      var s = getSrc();
      return s == null ? '[none - press ENTER]' : baseName(s);
    };
    var onAccept = function():Void
    {
      #if sys
      browseFile(title, filters, function(path)
      {
        setSrc(path);
        refreshRows();
      });
      #end
    };
    return new GenField(label, FILE, display, null, null, null, onAccept);
  }

  // ------------------------------------------------------------------
  // Helpers
  // ------------------------------------------------------------------

  function padStr(n:Int):String
  {
    var s = '';
    for (i in 0...n) s += ' ';
    return s;
  }

  function sanitizeId(v:String):String
  {
    return FileUtil.INVALID_CHARS.replace(v.toLowerCase(), '-');
  }

  function baseName(p:String):String
  {
    var f = p.split('/').pop();
    #if sys
    f = f.split('\\').pop();
    #end
    var i = f.lastIndexOf('.');
    return i > 0 ? f.substr(0, i) : f;
  }

  function extOf(p:String):String
  {
    var f = p.split('/').pop();
    var i = f.lastIndexOf('.');
    return i > 0 ? f.substr(i) : '';
  }

  function selCharId(sel:CharSel, fallback:String):String
  {
    if (sel.onCustom && sel.customPath != null) return baseName(sel.customPath);
    return sel.idx >= 0 && sel.idx < charIds.length ? charIds[sel.idx] : fallback;
  }

  function effPlayer():String return selCharId(playerSel, 'bf');
  function effOpponent():String return selCharId(oppSel, 'gf');
  function effGirlfriend():String return selCharId(gfSel, 'gf');

  // ------------------------------------------------------------------
  // Edit existing song
  // ------------------------------------------------------------------

  #if sys
  function loadSongForEdit(id:String):Void
  {
    var dataDir = FileUtil.gameDirectory + '/assets/data';
    var songDir = dataDir + '/songs/' + id;
    var metaPath = songDir + '/' + id + '-metadata.json';
    if (!FileSystem.exists(metaPath))
    {
      statusMsg = 'ERROR: no metadata for ' + id;
      refreshRows();
      return;
    }

    try
    {
      // reset fields that the metadata may not provide
      instSrc = null;
      pvSrc = null;
      ovSrc = null;
      albumArtSrc = null;
      playerSel = new CharSel(0);
      oppSel = new CharSel(charIds.length > 1 ? 1 : 0);
      gfSel = new CharSel(charIds.length > 1 ? 1 : 0);
      ratingStr = '5';

      var meta:Dynamic = Json.parse(File.getContent(metaPath));
      songId = id;
      songName = meta.songName ?? id;
      artist = meta.artist ?? '';
      charter = meta.charter ?? '';

      var tc = meta.timeChanges;
      if (tc != null && tc.length > 0 && tc[0].bpm != null) bpmStr = '' + tc[0].bpm;

      var pd:Dynamic = meta.playData;
      if (pd != null)
      {
        var diffs:Array<String> = [];
        if (pd.difficulties != null) for (d in (cast pd.difficulties : Array<String>)) diffs.push(d);
        diffPreset = presetForDiffs(diffs);

        var chars:Dynamic = pd.characters;
        if (chars != null)
        {
          applyChar(chars.player, playerSel, 'bf');
          applyChar(chars.opponent, oppSel, 'gf');
          applyChar(chars.girlfriend, gfSel, 'gf');
        }
        applyChoice(pd.stage, stageIds, function(v) stageIdx = v);
        applyChoice(pd.noteStyle, noteskinIds, function(v) noteskinIdx = v);
        albumIdStr = pd.album ?? '';
      }

      raidSrc = null;
      var chartPath = songDir + '/' + id + '-chart.json';
      if (FileSystem.exists(chartPath))
      {
        var chart:Dynamic = Json.parse(File.getContent(chartPath));
        if (chart.raidChart != null)
        {
          var raidPath = FileUtil.gameDirectory + '/assets/songs/' + id + '/' + chart.raidChart;
          if (FileSystem.exists(raidPath)) raidSrc = raidPath;
        }
      }

      capsuleText = '';
      levelNameStr = songName;
      levelBg = '#FF6B6B';
      var levelPath = dataDir + '/levels/' + id + '.json';
      if (FileSystem.exists(levelPath))
      {
        var lvl:Dynamic = Json.parse(File.getContent(levelPath));
        if (lvl.name != null) levelNameStr = lvl.name;
        if (lvl.background != null) levelBg = lvl.background;
        if (lvl.capsule != null && lvl.capsule.name != null) capsuleText = lvl.capsule.name;
      }

      albumName = '';
      var albumPath = dataDir + '/ui/freeplay/albums/' + (albumIdStr != '' ? albumIdStr : id) + '.json';
      if (albumIdStr != '' && FileSystem.exists(albumPath))
      {
        var alb:Dynamic = Json.parse(File.getContent(albumPath));
        if (alb.name != null) albumName = alb.name;
      }

      statusMsg = 'Loaded ' + id + ' - adjust fields and GENERATE to save.';
      mode = FORM;
      cursor = 0;
      scroll = 0;
      buildFields();
    }
    catch (e:Dynamic)
    {
      statusMsg = 'ERROR reading ' + id + ': ' + e;
    }
    refreshRows();
  }

  function presetForDiffs(diffs:Array<String>):Int
  {
    for (i in 0...DIFF_PRESETS.length)
    {
      if (DIFF_PRESETS[i].length == diffs.length)
      {
        var ok = true;
        for (d in diffs) if (DIFF_PRESETS[i].indexOf(d) < 0) ok = false;
        if (ok) return i;
      }
    }
    return 1;
  }

  function applyChar(v:Dynamic, sel:CharSel, fallback:String):Void
  {
    if (v == null) return;
    var id:String = v;
    var i = charIds.indexOf(id);
    if (i >= 0)
    {
      sel.onCustom = false;
      sel.customPath = null;
      sel.idx = i;
    }
    else
    {
      // Unknown/mod character id - preserve it via the custom slot.
      sel.onCustom = true;
      sel.customPath = id;
      var fi = charIds.indexOf(fallback);
      if (fi >= 0) sel.idx = fi;
    }
  }

  function applyChoice(v:Dynamic, list:Array<String>, setIdx:Int->Void):Void
  {
    if (v == null) return;
    var i = list.indexOf(v);
    if (i >= 0) setIdx(i);
  }
  #else
  function loadSongForEdit(id:String):Void {}
  #end

  // ------------------------------------------------------------------
  // Generation
  // ------------------------------------------------------------------

  #if sys
  function ensureDir(path:String):Void
  {
    var parts = path.split('/');
    var cur = path.charAt(0) == '/' ? '/' : '';
    for (p in parts)
    {
      if (p == '') continue;
      cur = cur == '/' ? '/' + p : (cur == '' ? p : cur + '/' + p);
      try
      {
        if (!FileSystem.exists(cur)) FileSystem.createDirectory(cur);
      }
      catch (e:Dynamic) {}
    }
  }

  function ensureParent(path:String):Void
  {
    var i = path.lastIndexOf('/');
    if (i > 0) ensureDir(path.substr(0, i));
  }

  function copyTo(dst:String, src:String):Bool
  {
    try
    {
      if (!FileSystem.exists(src)) return false;
      ensureParent(dst);
      File.saveBytes(dst, File.getBytes(src));
      return true;
    }
    catch (e:Dynamic)
    {
      statusMsg = 'WARN: could not copy ' + src;
      return false;
    }
  }

  function writeText(path:String, content:String):Bool
  {
    try
    {
      ensureParent(path);
      File.saveContent(path, content);
      return true;
    }
    catch (e:Dynamic)
    {
      statusMsg = 'ERROR writing ' + path + ': ' + e;
      return false;
    }
  }

  function detectSourceAssets():Void
  {
    // Dev builds keep a source assets/ tree next to export/. Mirroring writes
    // there keeps generated songs alive across `lime build` (which wipes
    // export/*/bin/assets).
    var candidate = FileUtil.gameDirectory + '/../../../../assets';
    if (FileSystem.exists(candidate + '/preload/data')) srcAssets = candidate;
  }

  function dataTargets(rel:String):Array<String>
  {
    var out = [FileUtil.gameDirectory + '/assets/data/' + rel];
    if (srcAssets != null) out.push(srcAssets + '/preload/data/' + rel);
    return out;
  }

  function imageTargets(rel:String):Array<String>
  {
    var out = [FileUtil.gameDirectory + '/assets/images/' + rel];
    if (srcAssets != null) out.push(srcAssets + '/preload/images/' + rel);
    return out;
  }

  function songTargets(rel:String):Array<String>
  {
    var out = [FileUtil.gameDirectory + '/assets/songs/' + rel];
    if (srcAssets != null) out.push(srcAssets + '/songs/' + rel);
    return out;
  }

  function copyToAll(targets:Array<String>, name:String, src:String):Void
  {
    for (base in targets)
    {
      var dst = base + '/' + name;
      var dirEnd = dst.lastIndexOf('/');
      if (dirEnd > 0) ensureDir(dst.substr(0, dirEnd));
      copyTo(dst, src);
    }
  }

  function writeTextAll(targets:Array<String>, content:String):Void
  {
    for (path in targets) writeText(path, content);
  }

  function isPackPath(p:String):Bool
  {
    return p.indexOf('/') >= 0 || p.indexOf('\\') >= 0 || p.endsWith('.rchr');
  }

  function doGenerate():Void
  {
    trace('SONGGEN: doGenerate id=${songId}');
    statusMsg = '';
    songId = sanitizeId(songId).split(' ').join('-');
    if (songId == '')
    {
      statusMsg = 'ERROR: song id is empty';
      refreshRows();
      return;
    }

    var songDataTargets = dataTargets('songs/' + songId);
    var mediaTargets = songTargets(songId);
    for (t in songDataTargets) ensureDir(t);
    for (t in mediaTargets) ensureDir(t);

    var diffs = currentDiffs();
    var pChar = effPlayer();
    var oChar = effOpponent();
    var gfChar = effGirlfriend();

    // ---- media files ----
    var raidName:Null<String> = null;
    if (instSrc != null) copyToAll(mediaTargets, 'Inst' + extOf(instSrc), instSrc);
    if (pvSrc != null) copyToAll(mediaTargets, 'Voices-' + pChar + extOf(pvSrc), pvSrc);
    if (ovSrc != null) copyToAll(mediaTargets, 'Voices-' + oChar + extOf(ovSrc), ovSrc);
    if (raidSrc != null)
    {
      raidName = songId + '.raid';
      copyToAll(mediaTargets, raidName, raidSrc);
    }
    else if (FileSystem.exists(mediaTargets[0] + '/' + songId + '.raid'))
    {
      raidName = songId + '.raid'; // keep existing raid chart when editing
    }

    // .rchr character packs (only real file paths get copied; bare ids are preserved as-is)
    var customPaths = [playerSel.customPath, oppSel.customPath, gfSel.customPath];
    for (cp in customPaths)
    {
      if (cp != null && isPackPath(cp))
      {
        copyToAll(mediaTargets, 'chars/' + baseName(cp) + '.rchr', cp);
      }
    }

    // ---- album ----
    var wantAlbum = albumArtSrc != null;
    var albumId = albumIdStr != '' ? sanitizeId(albumIdStr) : songId;
    if (wantAlbum)
    {
      var imgTargets = imageTargets('freeplay/albumRoll');
      for (it in imgTargets) ensureDir(it);
      copyToAll(imgTargets, albumId + '.png', albumArtSrc);
      var titleText = albumName != '' ? albumName : (songName != '' ? songName : songId);
      renderTextPngAll(imgTargets, '/' + albumId + '-text.png', titleText);

      var albumObj:Dynamic = {
        version: '1.0.0',
        name: albumName != '' ? albumName : (songName != '' ? songName : songId),
        artists: [artist != '' ? artist : 'Unknown'],
        albumArtAsset: 'freeplay/albumRoll/' + albumId,
        albumTitleAsset: 'freeplay/albumRoll/' + albumId + '-text'
      };
      if (albumName != '') Reflect.setField(albumObj, 'albumOSTName', albumName);
      writeTextAll(dataTargets('ui/freeplay/albums/' + albumId + '.json'), Json.stringify(albumObj, null, '  '));
    }
    else if (albumName != '')
    {
      statusMsg = 'NOTE: album name set but no album art picked - album skipped';
    }

    // ---- metadata ----
    var dispName = songName != '' ? songName : songId;
    var dispArtist = artist != '' ? artist : 'Unknown';
    var meta = new SongMetadata(dispName, dispArtist, charter != '' ? charter : null);
    var bpmVal:Float = Std.parseFloat(bpmStr) ?? 100;
    if (bpmVal <= 0) bpmVal = 100;
    meta.timeChanges = [new SongTimeChange(0, bpmVal)];
    meta.playData.difficulties = diffs;
    meta.playData.characters = new SongCharacterData(pChar, gfChar, oChar);
    meta.playData.stage = stageIds[stageIdx];
    meta.playData.noteStyle = noteskinIds[noteskinIdx];
    var ratingVal:Int = Std.parseInt(ratingStr) ?? 5;
    ratingVal = Std.int(Math.max(0, Math.min(15, ratingVal)));
    var ratings = new Map<String, Int>();
    for (d in diffs) ratings.set(d, ratingVal);
    meta.playData.ratings = ratings;
    meta.playData.album = wantAlbum ? albumId : null;

    writeTextAll(dataTargets('songs/' + songId + '/' + songId + '-metadata.json'), meta.serialize());

    // ---- chart ----
    var scrollObj:Dynamic = {};
    var notesObj:Dynamic = {};
    for (d in diffs)
    {
      Reflect.setField(scrollObj, d, 1.0);
      Reflect.setField(notesObj, d, []);
    }
    var chartObj:Dynamic = {
      version: '2.0.0',
      scrollSpeed: scrollObj,
      events: [],
      notes: notesObj,
      generatedBy: 'R-Slice Song Generator'
    };
    if (raidName != null) Reflect.setField(chartObj, 'raidChart', raidName);
    writeTextAll(dataTargets('songs/' + songId + '/' + songId + '-chart.json'), Json.stringify(chartObj, null, '  '));

    // ---- level / freeplay capsule ----
    var lvlName = levelNameStr != '' ? levelNameStr : dispName;
    var levelObj:Dynamic = {
      version: '1.0.0',
      name: lvlName,
      titleAsset: 'storymenu/titles/tutorial',
      capsule: { name: capsuleText },
      songs: [songId],
      props: [],
      background: levelBg,
      visible: true
    };
    writeTextAll(dataTargets('levels/' + songId + '.json'), Json.stringify(levelObj, null, '  '));

    // Make the new files resolvable through the runtime asset layer immediately.
    funkin.util.assets.RuntimeAssetLibrary.refresh();

    trace('SONGGEN: generate complete');
    statusMsg = 'GENERATED "${songId}" - restart the game to see it in Freeplay.';
    refreshRows();
  }

  function renderTextPngAll(targets:Array<String>, suffix:String, text:String):Void
  {
    try
    {
      var tf = new TextField();
      var fmt = new TextFormat('_sans', 28, 0xFFFFFF);
      fmt.bold = true;
      tf.defaultTextFormat = fmt;
      tf.text = text;
      var w:Int = Std.int(Math.ceil(tf.width)) + 16;
      var h:Int = Std.int(Math.ceil(tf.height)) + 16;
      var bmp = new BitmapData(w, h, true, 0x00000000);
      bmp.draw(tf, new Matrix(1, 0, 0, 1, 8, 8));
      var bytes = bmp.encode(bmp.rect, new PNGEncoderOptions());
      var xmlSuffix:String = suffix.substr(0, suffix.length - 4) + '.xml';
      var xml:String = sparrowAtlasXml(baseName(suffix), w, h);
      for (base in targets)
      {
        File.saveBytes(base + suffix, bytes);
        File.saveContent(base + xmlSuffix, xml);
      }
    }
    catch (e:Dynamic)
    {
      statusMsg = 'WARN: album title render failed: ' + e;
    }
  }

  /**
   * Sparrow TextureAtlas XML matching the album-title format used by
   * `AlbumRoll` (`idle0001..8` looped, `switch0001..4` one-shot). All frames
   * point at the whole image so the rendered text displays statically.
   */
  function sparrowAtlasXml(pngName:String, w:Int, h:Int):String
  {
    var s:String = '<?xml version="1.0" encoding="UTF-8"?>\n'
      + '<TextureAtlas imagePath="$pngName" width="$w" height="$h">\n';
    for (i in 1...9)
    {
      s += '  <SubTexture name="idle${frameNum(i)}" x="0" y="0" width="$w" height="$h" frameX="0" frameY="0" frameWidth="$w" frameHeight="$h"/>\n';
    }
    for (i in 1...5)
    {
      s += '  <SubTexture name="switch${frameNum(i)}" x="0" y="0" width="$w" height="$h" frameX="0" frameY="0" frameWidth="$w" frameHeight="$h"/>\n';
    }
    return s + '</TextureAtlas>\n';
  }

  static function frameNum(i:Int):String
  {
    return i < 10 ? '000' + i : '00' + i;
  }
  #else
  function doGenerate():Void
  {
    statusMsg = 'File generation is only available in desktop builds.';
    refreshRows();
  }
  #end
}
