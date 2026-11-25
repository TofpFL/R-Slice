# R-Slice Noteskin System — Structure & Schema

How custom noteskins are dropped in, converted, and rendered at runtime.
No recompilation is required for any of this.

## 1. Drop-in folder layout

Place skins next to the executable:

```
<game>/noteskins/
  <Anything>.png + <Anything>.xml      // flat Sparrow atlas = one skin
  <Anything>.png + <Anything>.json     // flat TexturePacker JSON atlas = one skin
  <SkinFolder>/
    *.png + *.xml|*.json               // each pair = one skin
    skin.json                          // optional per-skin overrides
    list.txt                           // optional, currently ignored
```

- A **Sparrow `.xml`** atlas is used as-is.
- A **TexturePacker `.json`** atlas (both `{"frames": {...}}` hash and
  `{"frames": [...]}` array formats) is converted to Sparrow XML internally;
  `spriteSourceSize`/`sourceSize` become `frameX/frameY/frameWidth/frameHeight`,
  `rotated` is preserved.
- `skin.json` is **only** an overrides file — it is never treated as atlas
  metadata. A same-named `<atlas>.json` is treated as TexturePacker metadata
  when it contains a `"frames"` collection.

### skin.json options

```json
{
  "name": "Display Name",
  "author": "Someone",
  "scale": 0.7,
  "bakePalette": false
}
```

| key            | meaning                                                        |
| -------------- | -------------------------------------------------------------- |
| `name`         | Selector display name (default: prettified atlas filename)     |
| `author`       | Author string shown in metadata                                |
| `scale`        | Sprite scale for note/receptor slots (default `0.7`)           |
| `bakePalette`  | `true`/`false` forces/skips RGB-mask palette baking; omit = auto |

## 2. What the importer produces

All output goes to `assets/noteskins/` — **source files are never modified**.

```
assets/noteskins/
  images/<id>.png / <id>.xml          // copied (PNG possibly palette-baked) atlas
  data/notestyles/<id>.json           // generated NoteStyleData
  reports/<id>.txt                    // conversion report (overwritten per import)
```

The skin registers into `NoteStyleRegistry` at runtime and appears in
Options → Note Skin alongside built-ins (`funkin`, `pixel`, `hrk`).

## 3. Frame naming conventions probed

The converter (`NoteSkinConverter`) probes these prefixes per direction
(`left/down/up/right`; Psych color names `purple/blue/green/red`):

| slot        | candidates (first match wins)                                        |
| ----------- | -------------------------------------------------------------------- |
| note        | `{color}0`, `note{DIR}`, `arrow{DIR}`, `{dir}0`                        |
| static      | `arrow{DIR}`, `{dir} static`, `static{DIR}`, `{color}0`                |
| press       | `{dir} press`, `{dir}press`, `press{DIR}`, `{dir} pressed`, `{color} press` |
| confirm     | `{dir} confirm`, `{dir}confirm`, `confirm{DIR}`, `{dir} confirmed`, `{color} confirm` |
| splash      | `note splash`, `notesplash`, `splash`                                  |
| holds       | `{color} hold piece`, `{color} hold end` (detected for palette baking; strip compositing still falls back) |

Direction ↔ color map (JS Engine `arrowRGB` order): left=purple, down=blue,
up=green, right=red.

## 4. Palette-mask (RGB) atlases

Psych/JS skins often ship channel-mask art: texel **R → main color,
G → white highlight, B → dark shade** (JS `RGBPaletteShader`).

- `NoteSkinBaker` auto-detects the mask signature and bakes JS's default
  `arrowRGB` lane palettes into the PNG at import time.
- Built-in styles can instead use the runtime shader: a `palette` field on any
  `NoteStyleAssetData` — `[[r,g,b],[r,g,b],[r,g,b]] × 4 directions` (0-255),
  consumed by `RGBPaletteShader`.
- Static receptor frames (`arrow*`) are intentionally not baked — they are
  raw art in both JS Engine and V-Slice.

## 5. Generated NoteStyleData schema

Document root: `{version, name, author, fallback, assets}`.
`fallback: "funkin"` means every slot the skin doesn't provide inherits the
base style.

Asset slots (`assets.*`), each `{assetPath, scale?, offsets?, alpha?, animated?, palette?, data}`:

| slot             | data shape                                                        | notes |
| ---------------- | ----------------------------------------------------------------- | ----- |
| `note`           | `{left,down,up,right}` each `{prefix}`                            | falling arrows |
| `noteStrumline`  | 16 fields `{dir}Static/{dir}Press/{dir}Confirm/{dir}ConfirmHold` | receptors; all required |
| `noteSplash`     | `{enabled, framerateDefault, leftSplashes[], ...}`                | splash anims |
| `holdNote`       | `{}`                                                              | single 8-segment strip texture |
| `holdNoteCover`  | per-direction `{start,hold,end}`                                  | hold caps |
| `countdown*`     | `{audioPath}`                                                     | 3/2/1/go |
| `judgement*`     | `{}`                                                              | sick/good/bad/shit |
| `comboNumber0-9` | `{}`                                                              | combo digits |

`UnnamedAnimationData` fields: `prefix` (required for Sparrow atlases),
`offsets`, `looped`, `flipX`, `flipY`, `assetPath` (multi-atlas only).

## 6. Pipeline stages (source/funkin/noteskins/)

| class                  | role                                                        |
| ---------------------- | ----------------------------------------------------------- |
| `NoteSkinImporter`     | discovery (`noteskins/` scan), orchestration, asset registration, reports |
| `TexturePackerAtlas`   | TexturePacker JSON → Sparrow XML normalization             |
| `SkinAtlasParser`      | Sparrow XML → frame list (`AtlasFrame`)                    |
| `NoteSkinConverter`    | prefix probing → `NoteStyleData` JSON                      |
| `NoteSkinBaker`        | RGB-mask detection + palette baking into PNG               |
| `ReceptorFramePatcher` | receptor frames → native logical trim metadata             |
| `SkinCompatibility`    | kind classification (`psych`/`generic`/`unknown`) + issue list |
| `NoteSkinReport`       | per-skin conversion report (`assets/noteskins/reports/`)   |

## 7. Known limitations

- Hold notes still fall back to the `funkin` 8-segment strip; `{color} hold
  piece/end` frames are detected (and palette-baked) but not yet composited.
- The "make adjustments?" compatibility prompt is data-ready
  (`NoteSkinImporter.reports`) but the modal UI lands with the editor work.
- Only single-atlas skins are supported (one PNG per skin).
