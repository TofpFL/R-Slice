# Noteskin System — Source Reference Index

Audit of reference engines for the noteskin rework (Phases 0–7).
All paths relative to `F:\vslice-lime-build`.

## Transpile / build feasibility (Phase 0 requirement)

| Engine | Format | Build path | Decision |
|---|---|---|---|
| JS Engine (`JS ENGINE SOURCE REF/`) | Haxe, `project.hxp` | Same lime/hxcpp toolchain as V-Slice in principle | **Not built.** Source analysis is the reference need; a full engine build costs ~10+ min and adds no information the `.hx` sources don't already give. Documented per Phase 0 fallback clause. |
| Codename Engine (`CodenameEngine/`) | Haxe, `project.xml` (legacy) | Same toolchain family | **Not built.** Same rationale — we consume its UI source as a design/architecture reference, not as a runnable artifact. |
| V-Slice (`source/`) | Haxe → hxcpp → C++20 via `haxelib run lime build windows` | `export/release/windows/bin/Funkin.exe` | Builds clean (~7 min incremental). |

If a runnable reference build is later required, both engines are Haxe/OpenFL
projects buildable with the local Haxe 2025.05 + haxelib repo; the blocker is
only time/dependency setup, not capability.

---

## JS Engine (`JS ENGINE SOURCE REF/source/`)

### Noteskin model
- `Note.hx` — falling notes + hold notes. Single Sparrow atlas per skin
  (`noteskins/NOTE_assets[-skinname]`). Frames addressed by prefix:
  - scroll anim: `{color}0` where color ∈ `purple,blue,green,red`
    (`colArray`, `Note.hx:120`)
  - hold body: `{color} hold piece`, hold end: `{color} hold end`
  - `defaultNoteSkin = 'noteskins/NOTE_assets'` (`Note.hx:121`),
    per-song override `PlayState.SONG.arrowSkin`, per-user postfix
    `getNoteSkinPostfix()` → `-<noteskin>` variant atlas (`Note.hx:410`)
  - scaling: `setGraphicSize(width * 0.7)`; `swagWidth = 160 * 0.7 = 112`
    is the receptor/note lane spacing unit (`Note.hx:118,487,504`)
- `StrumNote.hx` — receptors. Animations: `static` (prefix `arrow{DIR}`),
  `pressed` (`{dir} press`), `confirm` (`{dir} confirm`). RGB shader enabled
  iff current anim != `static` (`StrumNote.hx:174`).
- `ClientPrefs.hx:112-123` — `arrowRGB` / `arrowRGBPixel` palettes:
  per direction `[main, highlight(white), dark]`.
- `shaders/RGBPalette.hx` — `RGBPaletteShader` GLSL:
  `mapped = min(color.r*r + color.g*g + color.b*b, 1)`, `mix(color.rgb, mapped, mult)`.
  `RGBPalettePool.getDefault(noteData, pixel)` caches per-lane palettes.
  `RGBShaderReference` auto-clones palette on per-sprite override.
- `NoteSplash.hx`, `play/objects/SustainSplash.hx` — splash/sustain effects.
- `Paths.hx` — `getSparrowAtlas()`, `image()`, asset library resolution.
- `options/NotesSubState.hx`, `VisualsUISubState.hx` — noteskin picker UI.

### Channel-mask convention (verified against `NOTE_assets-hrk` atlas)
- `{color}0` falling-note frames and `{dir} press`/`{dir} confirm` receptor
  frames store lane color in **R=main, G=highlight, B=dark** channels.
- `arrow{DIR}` static receptor frames are **pre-colored** art (not masks).
- Palette baking alternative: `source/funkin/noteskins/NoteSkinBaker.hx`
  (V-Slice) bakes the same palettes into the PNG at import time.

## Codename Engine (`CodenameEngine/source/funkin/`)

### Editor UI kit — `editors/ui/` (~45 files)
Retained-mode, sprite-based UI. Base class `UISprite` (FlxSprite +
`members` group, `__rect` hitbox, `cursor` states ARROW/CLICK, `hovered`).
`UIState` is the manager (substate stack, `isOverlapping`, context menus,
editor sounds via `Flags.*_EDITOR_*_SOUND`).

Key components for V-Slice adaptation:
- `UIWindow.hx` / `UISubstateWindow.hx` — titled panel windows, collapsible
- `UISliceSprite.hx` — nine-slice panel primitive (all windows derive from it)
- `UIButton.hx`, `UIButtonList.hx`, `UIRadioButton.hx`, `UICheckbox.hx`
- `UIDropDown.hx`, `UISlider.hx`, `UINumericStepper.hx`, `UITextBox.hx`,
  `UIAutoCompleteTextBox.hx`
- `UIColorwheel.hx`, `UICompactColorwheel.hx`, `UIIconColorPicker.hx`
- `UIScrollBar.hx`, `UIFileExplorer.hx`, `UIImageExplorer.hx`, `UIImageList.hx`
- `UIContextMenu.hx`, `UITopMenu.hx`, `UIBackdrop.hx`, `UIText.hx`,
  `UIAudioPlayer.hx`, `UIScaleMode.hx`, `UIWarningSubstate.hx`
- `UIUtil.hx` — `follow()`, `processShortcuts()`, `toUIString()`,
  `confirmUISelections()` (commit-on-close pattern for text/number inputs)
- `IUIFocusable.hx`, `UIState.hx` — focus/hover model

### Editor architecture
- `EditorPicker.hx`, `EditorTreeMenu.hx`, `import.hx` — editor launcher
- `charter/Charter.hx` + `Charter*Screen.hx` — screen-per-section pattern
  (MetaData / Strumline / Event / Song screens), `CharterPreviewStrumLine.hx`
  is the live-preview reference for Phase 5.
- `character/CharacterEditor.hx`, `stage/StageEditor.hx`,
  `alphabet/AlphabetEditor.hx` — editor entry points.
- `UndoList.hx` — undo system.
- Assets: `assets/editors/ui/` nine-slice textures (`normal-popup` etc.).

## V-Slice current implementation (`source/funkin/`)

- `noteskins/NoteSkinImporter.hx` — runtime scanner (`<exe>/noteskins/`),
  Psych/JS Sparrow atlas → generated `notestyles/<id>.json` + atlas copy.
- `noteskins/NoteSkinConverter.hx` — frame-prefix mapping, `buildAssets()`.
- `noteskins/SkinAtlasParser.hx` — Sparrow XML → `AtlasFrame` list.
- `noteskins/NoteSkinBaker.hx` — palette-mask detection + channel bake.
- `noteskins/ReceptorFramePatcher.hx` — rewrites receptor frame
  sourceSize/offsets to native logical size (fixes left shift, Phase 1).
- `data/notestyle/NoteStyleData.hx` — JSON schema (typedefs); `palette`
  field added for RGB-mask styles.
- `play/notes/notestyle/NoteStyle.hx` — asset resolution, fallback chain,
  `buildNoteSprite`, `applyStrumline*`, `getNotePalette/getStrumlinePalette`.
- `play/notes/NoteSprite.hx` — falling notes; `hsvShader` (hit desat),
  `paletteShader` (RGB palette), `playNoteAnimation` per direction.
- `play/notes/StrumlineNote.hx` — receptors; anims `static/press/confirm/
  confirm-hold`; palette shader on non-`static` anims (JS parity).
- `play/notes/Strumline.hx` — lane layout: `NOTE_SPACING=112`,
  `INITIAL_OFFSET=-0.275*104`, `getXPos()` per direction.
- `graphics/shaders/RGBPaletteShader.hx` + `assets/preload/shaders/
  rgbpalette.frag` — JS `RGBPaletteShader` port.
- Built-in styles: `assets/preload/data/notestyles/{funkin,pixel,hrk}.json`;
  HRK atlas `assets/shared/images/NOTE_assets-hrk.{png,xml}`.

## Known limitations carried into later phases
- Hold notes still use `NOTE_hold_assets` strip (no per-skin hold compositing).
- `noteSplash` falls back to `shared:noteSplashes` when skin lacks splashes.
- Imported skins bake palette into PNG (no runtime shader path for imports).
- No TexturePacker JSON atlas support yet.
