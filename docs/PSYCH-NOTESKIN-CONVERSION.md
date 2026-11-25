# Task: Convert Psych Engine Note Skin to FNF V-Slice Note Style Mod

## Context & Goal

We need to convert an existing Friday Night Funkin' **Psych Engine** note skin (bundled `.png` and Sparrow v2 `.xml` spritesheets) into a valid **FNF V-Slice** Note Style mod package.

Unlike Psych Engine—which often bundles notes, hold pieces, and strumline receptors into a single spritesheet—V-Slice requires split spritesheets and a structured Note Style JSON configuration (`data/notestyles/<style-id>.json`).

## Inputs (Verify in Workspace)

Locate the Psych Engine source assets in the workspace (e.g., `./input/` or root directory):

- Note/Strum spritesheet: `NOTE_assets.png` and `NOTE_assets.xml` (or equivalent note skin files)
- *(Optional)* Splash spritesheet: `noteSplashes.png` and `noteSplashes.xml`
- Reference V-Slice template (if available in workspace or check FNF V-Slice modding docs for `funkin.json` schema).

## Requirements & Architecture

### 1. Asset Splitting & Repacking (Python Script)

Write and run a Python script (using `Pillow` and `xml.etree.ElementTree`) to parse the Psych Engine Sparrow v2 XML and crop/repack the subtextures into separate V-Slice spritesheet pairs (`.png` + Sparrow v2 `.xml`) inside `mods/<mod-name>/images/`:

- **`notes.png` / `notes.xml`**: Directional tap note frames (e.g., `purple0`, `blue0`, `green0`, `red0`).
- **`holdNotes.png` / `holdNotes.xml`**: Sustain hold body and tail/cap frames (e.g., `purple hold piece`, `pruple end hold`, `blue hold piece`, `blue hold end`, etc. — note the common Psych/base-game typo `pruple end hold` when matching XML `name` attributes).
- **`noteStrumline.png` / `noteStrumline.xml`**: Static receptor, press, and confirm animations (`arrowLEFT`, `left press`, `left confirm`, and corresponding `DOWN`, `UP`, `RIGHT` prefixes).
- **`noteSplashes.png` / `noteSplashes.xml`** *(if present)*: Hit splash frames.

Ensure all generated XML files preserve exact `frameX`, `frameY`, `frameWidth`, and `frameHeight` trimming attributes relative to the newly packed coordinates so animations do not jitter or misalign.

### 2. V-Slice Note Style JSON Configuration

Create `mods/<mod-name>/data/notestyles/<style-id>.json` following the official V-Slice Note Style schema:

- Set metadata: `"version": "1.1.0"`, `"name"`, `"author"`, and `"fallback": "funkin"` (so any missing assets like hold covers fall back cleanly to base game assets).
- Configure `assets` mappings (`note`, `holdNote`, `noteStrumline`, and `noteSplash` if applicable):
  - Map `assetPath` to the relative paths inside `images/` (without `.png` extension).
  - Map `data` animation prefixes (`left`, `down`, `up`, `right`, `static`, `press`, `confirm`, `hold`, `holdEnd`) to the exact XML `name` prefixes generated in Step 1.
  - Set `scale` (typically `0.7` for standard resolution Psych skins) and default `offsets` (`[0, 0]`).

### 3. Mod Metadata & Packaging

- Create a valid `_polymod_meta.json` in `mods/<mod-name>/` with `title`, `description`, `author`, `api_version`, and `mod_version` so V-Slice recognizes the folder as a loadable mod.
- *(Optional)* If a target song's `metadata.json` is present in the workspace, update `playData.noteStyle` to `<style-id>` for testing.

## Verification & Deliverables

Before finishing, run automated checks to verify:

1. Every `.png` referenced by `assetPath` in `<style-id>.json` exists at `mods/<mod-name>/images/<assetPath>.png` and has a matching `.xml`.
2. Every animation `prefix` defined in `<style-id>.json` matches at least one `SubTexture` `name` attribute in its corresponding `.xml` file.
3. All JSON files (`<style-id>.json` and `_polymod_meta.json`) parse cleanly without syntax errors.
4. Provide a summary of the generated directory tree, any unmapped XML frames skipped during conversion, and instructions on where to drop `mods/<mod-name>/` in the V-Slice game directory.

---

## R-Slice notes (current pipeline coverage)

Much of this is already automated — check before writing a one-off script:

- **Runtime importer** (`source/funkin/noteskins/NoteSkinImporter.hx`): dropping a Psych/JS noteskin folder (`.png` + Sparrow `.xml` **or** TexturePacker `.json`) into `<exe>/noteskins/` converts it automatically — no manual repacking needed.
- **Atlas handling**: `SkinAtlasParser` (Sparrow v2), `TexturePackerAtlas` (JSON → XML normalization, trim preserved), `NoteSkinConverter` (frame-prefix resolution), `NoteSkinBaker` (RGB channel-mask palette baking for Psych skins), `ReceptorFramePatcher` (receptor trim normalization), `SkinCompatibility` + `NoteSkinReport` (per-skin report at `assets/noteskins/reports/<id>.txt`).
- **Palette masks**: Psych `{color}0` notes and press/confirm frames are RGB channel masks; the importer bakes JS `arrowRGB` palettes into the PNG at import time (`NoteSkinBaker`), or the `palette` field + `RGBPaletteShader` can apply it at runtime (`paletteStaticOnly` flag controls which strumline animations get shaded — see `docs/NOTESKIN-STRUCTURE.md`).
- **Manual mod path** (this doc's Step 1–3) is still useful for: shipping a skin *inside a mod* rather than `noteskins/`, or skins needing hand-tuned `offsets`/frame renames the auto-resolver can't guess.
- **Known gaps**: hold notes still fall back to default strips on imported skins; the `pruple end hold` typo must be handled when matching hold-end frames.
