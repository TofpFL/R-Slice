# V-Slice Freeplay (Song Select) — Layout & Positioning Reference

Extracted from `H-Slice-master/source/mikolka/vslice/freeplay/` (the V-Slice
variant, NOT `source/states/FreeplayState.hx` which is the Psych-style one).
All coordinates are in the engine's 1280×720 logical space unless noted.

## Coordinate-space caveat (IMPORTANT)

`FreeplayState.CUTOUT_WIDTH = MobileScaleMode.gameCutoutSize.x / 1.5`.
`gameCutoutSize` is **0 on a standard 16:9 desktop** (it only measures the
side cutout bars in ultrawide mode). On desktop every term of the form
`CUTOUT_WIDTH * MULTI` evaluates to **0**, so all positions below marked
`[CW]` collapse to their constant. We should treat `[CW]` terms as tunable
anchor offsets, not literal pixels — verify visually and bake numbers.

Constants: `DJ_POS_MULTI = 0.44`, `SONGS_POS_MULTI = 0.75`,
`realScaled = 0.8` (capsule render scale inside `CustomAnimControl`).

## Song capsules (`SongMenuItem`, 612×132 frame, ~490×106 rendered)

Group position per item `i` relative to selection `s` (fractional while scrolling):

```
targetPos.x = intendedX(i - s) + CUTOUT_WIDTH * 0.75      // [CW]=0 → see below
targetPos.y = intendedY(i - s) + edgeCullOffset
intendedY(d) = d * (capsuleHeight*0.8 + 10) + 120        // ≈ d*115.6 + 120
intendedX(d) = 270 + 60 * fastSin(d)
```

Edge culling (`changeSelectionFractal`): items with `d < 0` get
`y -= 100`; `d > 4` get `y -= 10`; `d < 0` also `y += 50` base offset —
keeps offscreen capsules parked without rendering cost. Only ~±5 drawn.

Sub-element offsets inside each capsule (group-local, unscaled):

| Element | Position | Notes |
|---|---|---|
| capsule sprite | 0,0 | anim "selected"/"unselected" frames; offset.x = -5 when unselected |
| songText (CapsuleText) | `capsule.width*0.26` ≈ 159, y=45 | font "5by7" ~40px*0.8; clipWidth 290/245/210 depending on rank+fav |
| pixelIcon (PixelatedIcon) | 60, 14 | 32×32 char icon |
| bpmText | 144, 87 | scaled 0.9 |
| smallNumbers ×3 | x≈185+11i (+shift), y=88.5 | BPM digits |
| difficultyText | 414, 87 | scaled 0.9 |
| bigNumbers ×2 | 466+i*30, y=32 | difficulty rating digits |
| newText ("NEW" badge) | 454, 9 | atlas anim, frame offset by song ID |
| ranking (FreeplayRank) | 420, 41 | rankbadges atlas; +blurred copy w/ gaussian blur |
| favIcon / favIconBlurred | 380/405, 40 | 50×50, additive blend; x shifts +25 when ranked |
| sparkle | near ranking | random offset in (-20..3, -29..4), additive |
| txtWeek (album/week tag) | 298, 91 | AtlasText CAPSULE_TEXT font |

Selected state: grayscale shader 0→0.8, text alpha 1→0.6, blurred ghost
text visible only when unselected, ranking alpha 1→0.7, fav alpha 1→0.6.
Long titles: `initMove()` marquee scroll; `checkClip()` shrinks clipWidth.

Intro animation (`CustomAnimControl`): squash-stretch frame sequence
`xFrames=[1.7,1.8,0.85,0.85,0.97,0.97,1]` at 24fps with x-pos lerp curve
`[0.9,0.4,0.16,0.16,0.22,0.22,0.245]` (JANK — hand-tuned). SLIDE_LEFT starts
x=-800, SLIDE_RIGHT starts x=width. `forcePosition()` snaps to target.

## Chrome elements (FreeplayState::create)

| Element | Position | Notes |
|---|---|---|
| grpDifficulties | tween → `(CW*0.44 + 90, 80)` = **(90, 80)** on desktop | starts (-300,80); tabs spaced by own width, `x=-(w/2)+106` each |
| diffSelLeft arrow | `(CW*0.44 + 20, 70)` = **(20, 70)** | 'shine' anim atlas |
| diffSelRight arrow | `(CW*0.44 + 325, 70)` = **(325, 70)** | flipped |
| letterSort bar | `(CW*0.75 + 400, 75)` = **(400, 75)** | letters at i*80+50; arrows -20,15 and 380,15; swipe bounds (-20,-20,420×95) |
| DJ (FreeplayDJ) | `(CW*0.44 + 640, 366)` = **(640, 366)** | skipped in LOW_QUALITY |
| backingImage (angled BG) | x = `pinkBack.width*0.74`, y=0 | height = screen H, angle-mask shader, fades black→white 0.6s expoOut |
| backingCard pinkBack | slides x: -width → 0, 0.6s quartOut | 524×760 png scaled to CUTOUT_WIDTH width |
| orangeBackShit | (84, 440), w=pinkBack.w, h=75, 0xFFFEDA00 | hidden until confirm |
| alsoOrangeLOL | (0, 440), 100×75, 0xFFFFD400 | same |
| confirmGlow | `(CW*0.44)-30, 240` | glow behind DJ on confirm |
| confirmTextGlow | `(CW*0.44)-8, 115` | "confirm" glowing text |
| backing-text-yeah | `(CW*0.44)+640, 370` | atlas sprite near DJ |
| fp (FreeplayScore) | `(width - notch - 353, 60)`, 7 digits | sprite digits, ~45px spacing |
| albumRoll.newAlbumArt | `(width - 640 - notch, 360)` | atlas anim idle/intro |
| albumRoll.difficultyStars | `(width - 1140 - notch, 39)` | stars + flames |
| albumRoll.albumTitle | `(width - 355 - notch, 500)` | sparrow text, zIndex 1000 |
| topLeftCornerText "FREEPLAY" | `(max(notch,8), 8)`, 48px vcr | black bg box behind, stroke shader |
| ostName "OFFICIAL OST" | `(8-notch, 8)` right-aligned, 48px | hidden by default |
| charSelectHint | `(-40, 18)` → y+100 over 0.8s quartOut | "Press [X] to change characters", alpha sine 0.3–0.9 |
| fnfHighscoreSpr | `(width - notch - 420, 70)` | periodic 'highscore' anim every 12–50s |
| clearBoxSprite | `(width - notch - 115, 65)` | clear-box icon |
| txtCompletion "69" | `(width - notch - 95, 87)` | AtlasFont.FREEPLAY_CLEAR |
| overhangStuff | full-width black bar h=164, y=-164 → -100 | top mask during intro |
| missingTextBG/Text | fullscreen dim + centered msg | shown on load errors |
| touch ScrollableObject | `(-0.02, CW*0.75+150, 100, width-400, height)` | inertia scroll over song column; tap = confirm |

## Cameras & transitions

- `funnyCam` — main menu camera; `rankCamera` — rank reveal (only when
  returning from results).
- Entry (not from char select): backingImage black→white fade 0.6s
  expoOut w/ angleMaskShader.extraColor tracking; overhang bar slides up;
  everything else slides in via `exitMovers` (x=width offscreen-right,
  speed≈0.3) except char-select return which slides from top (y=-270..-300,
  speed 0.8).
- `busy` flag gates input until DJ intro completes (`dj.onIntroDone` or 0.5s).
- Song preview: `FADE_IN_DELAY` after selection change → plays Inst audio;
  Random capsule plays `freeplayRandom` track; `tweenCurSongColor` tints
  backingImage toward the song's color.

## Rank reveal (returning from PlayState results)

`rankAnimStart` → rankCamera zoom 1.85→1.8 sineIn, funnyCam 1.15→1.1;
curCapsule moves to rankCamera, scale pops 0→1 in 0.1s; capsule angle
tween; sparks + rankVignette (additive, alpha 0→) on rankCamera;
rank badge sounds per rank (`ranks/rankinperfect`, `rankinbad`,
`rankinnormal`, `loss`, `good`, `great`, `perfect`, `excellent`);
then cameras settle (rankCam 1.5 backIn, funnyCam elasticOut→1) and
capsule flies back to list position (quartIn, 1.3s).

## Input map

UP/DOWN select (wraps, scrollMenu sfx), wheel ±1/±8 scroll,
LEFT/RIGHT difficulty (regen song list filtered by diff),
ENTER: inst-variant list if `instVariants` else play,
RANDOM capsule: picks random alive song then confirms it,
F favorite toggle (fav/unfav sfx + heart anim + 5px bounce),
CTRL gameplay-changers substate, FREEPLAY_CHAR char-select,
RESET clears score, HOME/END jump first/last, BACK exits
(stops preview first if playing).

## Asset → element map (all under assets/freeplay/)

- `freeplayCapsule/capsule/freeplayCapsule*.png/.xml` — capsule anim frames
  ("mp3 capsule w backing" = selected, "NOT SELECTED" = unselected; 8 frames each @24fps)
- `freeplayCapsule/{bignumbers,smallnumbers}.{png,xml}` — digit strips (Sparrow atlas)
- `freeplayCapsule/{bpmtext,difficultytext,new,weektypes}` — capsule labels
- `freeplaySelector/` — difficulty arrow sprites (shine anim)
- `freeplayDifficulties/freeplay{easy,normal,hard,erect,nightmare}.png` — diff tabs
- `freeplayStars/` — difficulty-stars anim (0–15 rating, 100-frame loops/star)
- `albumRoll/` — album arts + title texts; `freeplayAlbum/` spritemap for roll anim
- `backingCards/pico/` + `pinkBack.png`, `cardGlow.png`, `confirmGlow*.png`,
  `glowingText.png`, `backing-text-yeah/` — backing card layer
- `freeplay-boyfriend/`, `freeplay-pico/` — DJ character atlases
- `icons/*pixel.*` — per-character pixel icons (PixelatedIcon)
- `rankbadges.png/.xml`, `rankVignette.png`, `sparks*.png/.xml`, `sparkle.*` — rank FX
- `freeplayBGdad*.png` — angled menu background
- `highscore.*`, `clearBox.png`, `instBox/`, `sortedLetters/`, `letterStuff.*`,
  `miniArrow.png`, `seperator.png`, `favHeart.*`, `freeplayFlame.*`,
  `beatdark/beatglow`, `transitionGradient.png`, `freeplay-clear` font — misc chrome
- `registry/{albums,styles}/*.json` — album/style metadata
- `freeplayRandom/freeplayRandom.ogg` — Random capsule music
- `assets/fonts/digital_numbers.*` — FreeplayScore digit font
- `assets/fonts/VCR_OSD_MONO_1.001.ttf` — "vcr.ttf" equivalent (VCR OSD Mono)
- `assets/sounds/{fav,unfav,remote_click,channel_switch,tv_on}.ogg`,
  `assets/sounds/ranks/*.ogg` — UI + rank stingers

## Native font identification (verified against H-Slice source)

| Element | Asset | Type | Source |
|---|---|---|---|
| Song title (capsule) | `assets/fonts/5by7.ttf` | TTF (FNF pixel font) | `CapsuleText.initText` -> `text.font = "5by7"` |
| Title glow | none (GlowFilter + blurred sprite copy, color = style capsule sel col, default `0xFF00ccff`) | effect | `CapsuleText.applyStyle` |
| Week/album tag | `assets/freeplay/fonts/capsule-text.png/.xml` | bitmap atlas (upper-only, spaceWidth=5) | `AtlasFont.CAPSULE_TEXT` |
| BPM label | `freeplayCapsule/bpmtext.png` | sprite | `SongMenuItem` (144,87), size*0.9 |
| BPM digits | `freeplayCapsule/smallnumbers.png/.xml` | sprite atlas, 3 digits centered (185+i*11, 88.5), size*0.9 | `CapsuleNumber` |
| Difficulty label | `freeplayCapsule/difficultytext.png` | sprite | (414,87), size*0.9 |
| Difficulty rating | `freeplayCapsule/bignumbers.png/.xml` | sprite atlas, 2 digits centered (466+i*30, 32), size*0.9 | `CapsuleNumber(big=true)` |
| Rank badge | `rankbadges.png/.xml` | sprite atlas, additive | `FreeplayRank` (420,41), scale 0.9 |
| NEW badge | `freeplayCapsule/new.png/.xml` | sprite, (454,9), size*0.9 | `newText` |
| Favorite heart | `favHeart` sprites | sprite, (380,40), 50x50 | `favIcon` |
| Surrounding freeplay UI | `assets/fonts/vcr.ttf` | TTF (VCR) | `OldPaths.font("vcr.ttf")` |
| Score readout | `assets/fonts/digital_numbers.png/.xml` | bitmap atlas | `FreeplayScore` |

Notes: `DS-DIGI.TTF` exists in H-Slice fonts but is NOT referenced by the
vslice freeplay code — not a capsule font. `CapsuleNumber` centers each
digit sprite on its (x,y); offsets -4/-1 apply to digits 1/3.
