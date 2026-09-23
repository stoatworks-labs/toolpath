# AGENTS.md — Toolpath

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

CNC pocketing from a distance field, as an FFGL 2.1 effect (`TP01`, shown as
`SW Toolpath`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/toolpath`.

Built 2026-09-23 in one session from the fleet's templates and
`specs/SPEC-toolpath.md` with `BRIEF.md` and `BRIEF-ADDENDUM.md`: tinsel for Detect
On and the fleet's trap list, galvo for the idea of a traced path driven at a
physical rate, rebate for the harness, `--pipe`, verify and the negative controls,
slowscan for `--offline`, `--allow-no-gl` and the CI shape, bassalt for colour
triples, readout (by way of rebate) for the host clock.

---

## The one idea

**A router clears a pocket with a round tool, and the path is the level sets of the
pocket's distance field.**

The tool's centre can only go where the whole disc fits, and what it removes is
everywhere the disc has been: the pocket opened by a disc of radius r. The passes
that do it are offset contours — the field's level sets at r, r + s, r + 2s, … — so
one distance field is the whole CAM job, and the recognisable features of a machined
part fall out of the geometry:

| the geometry | what comes out |
| --- | --- |
| the first pass is r in from every wall, and meets itself r in from both walls of a corner | **a fillet of exactly r** in every inside corner |
| inside a slot w wide the field never exceeds w/2 | **a slot under 2r is never entered** |
| each pass cuts a band 2r wide, the passes s apart | **scallops s − 2r wide** past s = 2r, none below |
| the tool moves at Feed along a path whose length is the pocket's | **a big pocket takes longer** |

The pipeline:

1. **Region.** Detect On (tinsel's luma, alpha, chroma, luma-or-alpha), an optional
   Gaussian (`Smooth`), `Threshold`, `Invert`. A pixel is inside or it is not.
2. **Field.** A jump flood carrying both seeds — each pixel's nearest outside pixel
   and nearest inside pixel — then a resolve: inside, the distance to the nearest
   outside pixel centre (or to the frame's edge, which is a wall) less half a pixel;
   outside, minus the same. So a straight wall sits half way between the last inside
   centre and the first outside one, and the field is exactly the distance to it.
3. **Toolpath.** The field sampled onto a trace grid (one sample a pixel up to 1280
   wide), read back, traced by marching squares at every level in one pass over the
   cells, corners put back, simplified to 0.2 px, and ordered one pocket at a time.
4. **Machine.** The tool advances along the path at Feed (Latch) or sweeps its prefix
   (Live), and every piece it covers is stamped into the cut buffer as a capsule of
   radius r, MAX-blended, its coverage a one-pixel ramp centred on r.
5. **Render.** Reveal (the clip through opaque stock where cut), Engrave (the clip,
   the cut sunk into it, lit, with a bevel from the field), Paths (the CAM preview),
   Field (glow, bevel or outline straight from the field, no machining).

### What falls out, and what does not

- **The fillet, the slot and the scallops are not drawn.** Nothing in the code knows
  about corners, slots or ridges; they are measured out of the picture.
- **Rapids cut nothing.** Between loops the tool lifts, crosses at 4× feed and does
  not stamp. Feed is the tool's speed while it cuts.
- **Not modelled:** depth (one pass, one depth), ramping and plunging, climb or
  conventional direction (loops run anticlockwise round what they enclose), a
  finishing pass along the medial axis (a pocket whose last pass falls short of its
  middle keeps an island there), tool deflection, chip load. The part is 2D.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses. Lengths in frame heights. |
| `source/Path.{h,cpp}` | The CAM half, no GL: `TraceLevels` (marching squares, all levels, one pass), `SharpenCorners`, `SimplifyLoop` (Douglas–Peucker), `Order` (the pocket forest), `PositionAt`, `CutSpans`. `TracePerturb` hooks. |
| `source/Shaders.{h,cpp}` | The ten shaders: detect, blur, seed, flood, resolve, sample, stamp (vertex and fragment), composite, and the shared vertex shader. |
| `source/PassBuffer.*` | A texture and a framebuffer, any format, no depth. Not an FFGLFBO (see the traps). |
| `source/Toolpath.{h,cpp}` | The plugin: parameters, the clock, the flood schedule, Latch and Live, the stamps, the composite, the test hooks. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/tptest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/check-shaders.sh` | glslc over the dumped shaders, for verify.sh and CI alike. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Per frame: **detect → [blur ×2] → seed → flood ×N → resolve** (only when a region is
grabbed: every frame in Live, once per job in Latch) → **sample + readback + trace +
order** (when the job or the tool changes) → **stamp** (the new pieces in Latch, the
whole prefix in Live) → **overlay** (lines and the tool) → **composite** to the host.

---

## Traps

Roughly in the order they will bite.

### ☠️ JFA has no error bound, and the one I first wrote down was false at 1080p

The spec asks for the field's error against an exact EDT with a bound "derived from
the known JFA failure cases". The first bound was one pixel, argued from the medial
axis: every competitor there is a pixel of the same digitised curve. It held at
320×180 and 1280×720 and **failed by hand at 1920×1080** (a disc 1.12 px out, a star
1.18). The error grew with the raster — a disc 0, 0.53, 1.12, 2.41 px at 180p,
720p, 1080p and 4K with the spec's 1+JFA and the "+2" finish — which the digitised
curve argument cannot explain.

What survives is narrower and true: JFA's failure on a digitised boundary is the thin
Voronoi wedge, which leaves a pixel holding the seed of a **neighbouring** cell;
neighbouring boundary seeds are 8-neighbours, at most √2 apart; so by the triangle
inequality that failure costs at most √2. It is only a bound if the flood's surviving
errors are all of that kind, and with the short finish they were not. So the finish
is now a second run of halving steps from 1/128 of the longest side (4, 2, 1 at
180p; 16 … 1 at 720p and 1080p; 32 … 1 at 4K), the bound is √2, and the worst
measured is 0.91 px (a disc at 4K). A check at two rasters would not have found
this: run the physics by hand at 1080p before believing a bound that grows nowhere.

### ☠️ Lengthening the flood made the known bad case benign

The first fixture for the negative control was nine one-pixel islands that the flood
without its prepass missed by 1.5 px. The longer finish repaired it on its own, so
"skip the prepass" stopped failing — `--negative` caught that at once. The fixture now
is four islands, found by searching sparse constellations against a model of the new
schedule: 5.7 px out at 320×180 and 23 px at 1280×720 without the prepass, exact
with it. A negative control's fixture is tied to the algorithm; change one, re-run
the other.

### ☠️ A bad case is a property of a lattice

The constellation is bad at 320×180 × 2^k, where the jump sequence scales with it,
and nowhere else in particular: embedded in 333×187, the flood without its prepass
got it exactly right. It is flooded at its own lattice — the largest 320×180 × 2^k
that fits the requested raster — so it is the known bad case at every raster the
check is asked for.

### ☠️ Marching squares cuts every corner of every pass

A pocket's offset contours have a corner wherever the pocket does, and marching
squares joins a cell's two crossings with a chord that cuts it off — by up to a
quarter of the cell's diagonal. Worse, the field has a ridge (its medial axis)
running into that corner, and a crossing on a cell edge the ridge also crosses is
interpolated across the kink and lands off the contour. At 320×180 with r = 9 the
fillet came out as two arcs on centres half a pixel apart. `SharpenCorners` puts the
corner back where the straight runs either side meet (runs straight to 5°, turning
by more than 45°: a circle never qualifies). A first attempt, cell-local, from the
crossings' estimated gradients, was 0.5 px off on an off-grid corner; the `--march`
corner check caught it and it was replaced.

### ☠️ FFGLFBO cannot hold the flood

`FFGLFBO::GenerateColorTexture()` allocates every texture with `GL_RGBA, GL_FLOAT`
as the upload format (SDK b1afaf9). For RGBA16UI that is GL_INVALID_OPERATION even
with no data: no texture, an incomplete framebuffer. It also gives every buffer a
24-bit depth renderbuffer — 33 MB each at 4K. `PassBuffer` here is its own texture
and framebuffer, picks the upload format from the internal format, and saves and
restores the bindings it disturbs by hand (the `ffglex::Scoped*` bindings clear to 0
on exit instead of restoring).

### ☠️ The flood costs 11 ms at 4K

Twenty passes of RGBA16UI at 3840×2160. Latch pays it once per job; Live pays it
every frame, and with the trace that is 16 ms — a whole 60 fps frame. It is
full-resolution on purpose (every check measures the plugin at the raster it
renders), and a stated-fraction field is the obvious lever if Live at 4K matters.

### ☠️ A digitised distance lit one texel apart is a sunburst

The Engrave and Field bevels first took the field's gradient by central differences
one texel apart. A distance to a staircase boundary swings with the staircase, and
lit, every curve grew radial hairlines. Over a quarter of the tool's radius (or of
the band) it is smooth.

### ☠️ A reader that leaves kills --pipe with SIGPIPE

The fleet's contract says a closed stdout exits 1. With a real pipe the next write
raises SIGPIPE and the process dies with 141 and no word on stderr. `--pipe` ignores
SIGPIPE, so the write fails with EPIPE and the loop says so; verify.sh checks both a
closed stdout and a reader that takes ten bytes and leaves.

### ☠️ A colour triple's names run past 16 characters

`Stock Colour_Green` is 18. The host truncates silently. `--names` caught it before
the first commit; the green and blue members are `Stock_Green`, `Path_Blue`.

### ☠️ The sweep found two controls that looked dead and were not

`Light Angle`'s ends are 0 and 360 degrees: the same light, the same picture. And
Latch and Live agree wherever the clip is still, which in 90 frames of Reveal is
everywhere the tool has been. Swept 0 against 180 degrees, and in the Paths preview.

### ☠️ Mutation-test only a committed tree

The mutation below was applied to a clean, committed tree and reverted with
`git checkout`; nothing uncommitted was lost with it.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
put back before the composite); every `ffglex::Scoped*` clears to 0 on exit, so every
allocation happens before this frame binds anything; `SetParamInfo` clamps a STANDARD
default into 0..1 before `SetParamRange` can widen it; the core is an **OBJECT**
library; `SetTextParameter` must return `FF_SUCCESS` for the About block; the harness
drives a synthetic clock; `nm | grep -q` fails under pipefail when grep succeeds; an
option's range reads back 0..1 whatever its element count; Resolume's clock overflows
a float, so nothing absolute crosses into GLSL (the tool's progress is Feed × dt in
double, Live's is Feed × (now − start) in double, and no shader sees a time at all);
a buffer holding state across frames must survive a resize (the latched field and cut
stay at the job raster and are never reallocated by one); `max( luma * a, a )` is 1
for every opaque pixel (Luma or Alpha is `a (0.35 + 0.65 luma)`).

---

## Defects found in my own checks

Every one of these made a check pass or fail for the wrong reason, and every one was
found by the check disagreeing with something else — a second raster, a negative
control, a CPU re-computation — not by reading it.

- **`--fillet` scanned the wrong half of the arc.** Rows from 1.5 px to r(1 − 1/√2)
  from the wall found four points. The steep half is from r(1 − 1/√2) to r.
- **`--fillet` fitted a free circle, which is ill-conditioned on a quarter arc.**
  Every boundary point lay within 0.05 px of the true circle and the fit said radius
  7.84 for 9: a bigger radius with the centre further out fits nearly as well, and
  the pixels' wiggle chose. A fillet is tangent to both walls, so its centre is R in
  from each and R is the one thing fitted. Printing the points' distances from the
  expected centre is what showed it.
- **`--fillet` had a 1.5 px wall exclusion and row/column scans** that starved the fit
  at 180p; it now reads 61 rays from the corner, 15° to 75°, bilinear.
- **`--scallop` read rows where the inner passes had already turned the corner.** A
  "ridge" 3 px too wide was the rectangle's inner loops, not a scallop. Rows are now
  at least span + 2r from the top and bottom walls.
- **`--feed` drew the pocket white,** so the revealed cut had green in it and the
  tool's centroid came out 44 px off. The pocket is red, found at threshold 0.1.
- **`--latch` pressed Restart after the resize,** when the new job's raster
  reallocated the cut anyway, so a Restart that kept the cut still passed. Its
  negative control caught it. Restart is now checked at the job's own raster.
- **The √2 story above:** a bound argued, not measured, and false at 1080p.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`, and at 640×480, 333×187 and 1920×1080 by hand
(`--distance` also once at 3840×2160).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--distance` square, frame | the plugin's field (R32F, read back) against Felzenszwalb–Huttenlocher's exact EDT of the same mask | **6 ULP** of the distance: GLSL 4.10 §4.7.1 gives sqrt as 1/inversesqrt, 2 + 2.5 ULP, plus one rounding of the half-pixel subtraction; the squared distances are exact integers | none: rectilinear nearest centres lie along rows and columns |
| `--distance` disc, ring, star, blobs | the same | **√2 px**: the flood's failure on a digitised boundary leaves a pixel the seed of a neighbouring cell, and neighbouring boundary seeds are 8-neighbours (triangle inequality). Holds only if the finish grows with the raster (above) | the finish is 1/128 of the longest side, so the schedule scales with the raster; measured 0 / 0.36 / 0.47 / 0.91 px at 180p / 720p / 1080p / 4K |
| `--distance` constellation | the same, the known bad case | **√2 px** with the prepass (measured exact); must exceed it without | flooded at 320×180 × 2^k, its own lattice, whatever raster is asked for |
| `--fillet` | a circle tangent to both walls fitted to 61 ray crossings of the cut coverage (bilinear) | **1 px** on the radius (the spec); each point within **0.5 px** of the fitted circle (a crossing read off a one-pixel ramp, clamped on one side, moves by less than the ramp's half-width). What can move it: the field is exact for a square, the corner is put back exactly, Douglas–Peucker does not touch a corner vertex | rays from 15° to 75° stay 0.13 r clear of both walls, so no tap reaches the black outside at r ≥ 8 px (r = 9 at 180p) |
| `--slot` narrow | cut coverage past mouth + one trace cell + r + 1 px, the whole slot | **exactly 0**: the sampled field never exceeds the true one, w/2 < r, so no pass is traced in the slot; the stamp's ramp ends at r + 0.5 | the trace cell (1 px up to 1280 wide) is in the margin |
| `--slot` wide | the slot's centre row, mouth to end | coverage **≥ 0.5** everywhere: w = 2r + 2 leaves one pixel for a sampled peak half a cell low and one for a row of centres half a pixel off the peak | w and r both scale with the height |
| `--scallop` s > 2r | uncut run widths between 0.5 crossings, rows beside the straight part of the left wall | **0.25 px**: two crossings, each read off a one-pixel ramp with one sample possibly clamped, worst case a(a − ½)/(a + ½) = 0.086 px each; the field, the trace and the passes are exact on a straight wall | rows chosen from r and the span at each raster |
| `--scallop` s ≤ 2r | the least coverage beside the wall | **≥ 0.5 − 2^-11**: at s = 2r bands meet at exactly 0.5, and the cut buffer is R16F | none |
| `--feed` | the tool's displacement over one second on a straight pass, at 60 and 30 fps | **1e-9 of Feed**: positions and time are double; one float ULP would be 1e-7 | none: Feed is in job heights |
| `--feed` picture | the tool's ring centroid in the frame against its position | **one texel**: a symmetric antialiased ring sampled at pixel centres | the ring width scales with the height |
| `--latch` restart | pixels cut one frame after Restart | **one frame's capsule**, 2(r + 1)·Feed·dt + π(r + 1)², a pixel of antialiasing all round | computed from the raster |
| `--latch` resize | every fully cut pixel's four successors at twice the raster | **≥ 0.5625 − 1/256**: bilinear at a quarter texel, the nearest texel weighs 0.75², less the filter's 8-bit weights | the check resizes to twice whatever raster it is given |
| `--march` circles (no GL) | traced points of a cone's levels against their circles | **cell²/(8ρ)**: linear interpolation of a distance along a cell edge, second derivative ≤ 1/ρ | a fixed grid; no GL |
| `--march` corners (no GL) | a rectangle's inset contour, every point and every corner | **one float ULP** of the largest sample (38.5: 3.8e-6): the grid is float | a fixed grid; no GL |
| `--exact` (no GL) | the reference EDT against brute force | **exact**, in doubles of integers | random small masks |

Deliberately NOT relied on: exact cancellation (`--distance` compares to 6 ULP, not
equality), a free circle fit on a short arc, the order in which a driver visits
fragments (the flood's ties keep the incumbent and its neighbours are visited in a
fixed order, so it is a pure function of the mask), implicit derivatives (the flood
and resolve use `texelFetch` and no derivatives).

What might still differ on another rasteriser: the bilinear filtering of the cut and
the field in the composite (8-bit sub-texel weights on some GPUs — the `--latch`
tolerance allows for it, and at the job's own raster every tap is at a texel centre);
the stamp's quads cover whole pixels by their centres, which every conforming
rasteriser agrees on; `sqrt` within its stated ULPs. The GL checks have never run on a
software rasteriser: CI would run them there if its runner could make a 4.1 context.

---

## Negative controls and the mutation

`tptest --negative` runs seven, and `--perturb BITS` runs any check verbosely against
one. Each perturbs the *plugin* — a `Perturb` bit the shipped plugin carries at zero
— never the harness's expectation. At 320×180:

| perturbation | what fails |
| --- | --- |
| no 1+JFA prepass (the spec's) | `--distance`: the constellation 5.69 px out, bound √2 (23.4 px at 720p) |
| a square tool (the spec's) | `--fillet`: the fitted radius 2.25 for r = 9, points 0.64 px off any circle |
| the first pass at r/2 | `--slot`: 2,224 pixels cut in a slot 2 px narrower than the tool |
| the stepover read in radii | `--scallop`: no ridges at 1.25, 1.5 or 2 diameters |
| Feed / 60 a frame, whatever dt | `--feed`: 45 px/s against 90 at 30 fps (right at 60) |
| a resize that re-grabs the job | `--latch`: 15,304 of 16,608 successors uncut, the job at the new raster |
| a Restart that keeps the cut | `--latch`: 4,468 pixels still cut after Restart, bound 434 |

And `--negative-offline`, with no GL: the reference EDT with a city-block distance
fails `--exact`; crossings put at the nearer sample, and corners left as chords, each
fail `--march`.

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the resolve pass,
`min( toStock, float( wall ) ) - 0.5` → `+ 0.5` (every inside distance one pixel too
long). Caught by `--distance` (square and frame 1.000 px out against 6 ULP) and
`--fillet` (radius 5.5 for 9) at 320×180 and 1280×720. **Not** caught by `--slot`
(the narrow slot's field rose to 8.5, still under r = 9), `--scallop` (it measures
widths, which a uniform offset does not change), `--feed` or `--latch`, correctly. The
curved shapes in `--distance` passed too: a uniform 1 px is under √2, which is why the
rectilinear shapes are held to the ULP. Reverted with `git checkout
source/Shaders.cpp`; the tree was clean before and after.

---

## Decisions taken without asking

- **Lengths in frame heights.** Tool Diameter 1–25% of the height, Feed 0.05–8
  heights a second: the look does not change with the composition's raster, and every
  check means the same thing at every raster. The spec's "Feed in px per second" is
  what `--feed` measures, in job pixels.
- **Stepover in tool diameters**, 0.1 to 2: CAM's own unit, and 1.0 is exactly where
  scallops begin.
- **The frame's edge is a wall.** An all-white clip is a pocket the size of the frame.
  The resolve pass adds it as an integer distance; the tracer's virtual ring of
  samples half a cell outside the grid carries the wall's own field.
- **The field's sign and offset.** Inside positive; half a pixel off the centre
  distance, so a straight wall is exactly where the field says.
- **The flood is 1 + JFA + a finish from 1/128 of the longest side** (above), at full
  resolution. The spec's 1+JFA is the prepass; the finish is mine.
- **One sample a pixel up to 1280 wide on the trace grid.** At 1280×720 and under,
  every check sees the plugin's own pixels; above, a sample covers 1.5 px (1080p) or
  3 px (4K) and Live's trace stays near 4.5 ms.
- **Simplify to 0.2 px**, and put corners back first.
- **Pocket by pocket.** Loops form a forest (a loop's parent is the loop one level out
  it lies nearest to); Outside In cuts a loop before its children, Inside Out after.
  The job starts at the loop nearest the frame's bottom-left corner.
- **Rapids at 4× feed, cutting nothing.** Feed is the cutting speed.
- **Latch** grabs the region on the first frame after InitGL, on Restart, and on
  switching to Latch — not on a clip retrigger, which an FFGL effect cannot see. A
  resize never re-grabs; the job's field and cut are sampled into the new raster.
  Changing Tool Diameter, Stepover or Strategy mid-job re-plans the path from the same
  field and carries on at the same place on the timeline.
- **Live** re-grabs every frame and cuts the current path's prefix, Feed × time since
  the sweep began, with a one-second hold on the finished part before it starts over.
- **Detect On defaults to Luma**, not tinsel's Luma or Alpha: `Threshold` then reads
  as a luma, which is what "the bright part is the pocket" means.
- **Reveal's stock is opaque** (alpha 1 where uncut); the other modes keep the clip's
  alpha, and Glow and Outline raise it where they draw.
- **Colour triples** (`Stock Colour`, `Path Colour`) as FF_TYPE_RED/GREEN/BLUE so a
  host can show a swatch, with ≤ 16-character member names.
- **Restart is an event** (FF_TYPE_EVENT): a press is remembered until the next frame.
- **No factory presets, no OpenFX, no browser demo** (not required for 0.1.0).
- **Test hooks live in the shipped plugin** (the `Perturb` bits, the `...ForTest`
  readers), always inert: the negative controls must perturb the plugin, not the
  harness's expectation.
- **`StoatworksAbout.h`, `StoatworksAboutLinks.h`, `StoatworksAboutParams.h` and
  `ATTRIBUTIONS.md` are provisional hand copies**, adapted from rebate's, with
  `guide=""`: register the project and re-run the syncs before the first release.
- **The commit trailer names the model that did the work** (`Claude Opus 5.5`), as the
  session's instructions asked, over the brief's.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 640×480, 333×187 and
1920×1080 by hand.

- **Distance.** Square and frame exact (max 1e-4 px at 1080p, inside 6 ULP). Disc,
  ring, star, blobs: max 0 / 0 / 0 / 0 px at 320×180; 0.36 / 0.06 / 0.04 / 0.14 at
  1280×720; 0.47 / 0.36 / 0.03 / 0.11 at 1920×1080; 0.91 / 0.49 / 0.14 / 0.20 at
  3840×2160 (once, by hand). The constellation exact at every raster; without the
  prepass 5.69 px (180p), 23.4 (720p), 47.1 (4K); plain JFA 9.27, 39.3, 79.3.
- **Fillet.** Radius 8.981 for 9 at 320×180, 36.002 for 36 at 1280×720, all four
  corners; every boundary point within 0.057 / 0.064 px of the fitted circle.
- **Slot.** 16 px against 2r = 18: 0 pixels cut over 139 px of slot; 20 px: its centre
  row cut throughout (least coverage 1.0). 70 and 74 against 72 at 720p, the same.
- **Scallop.** 1.25, 1.5 and 2 diameters: ridge widths within 0.086, 0.150, 0.008 px
  (180p) and 0.075, 0.150, 0.009 (720p) of s − 2r; 0.5 and 1.0 diameters: no ridge,
  least coverage 1.0 and 0.5996.
- **Feed.** 90.000000 px/s (180p) and 360.000000 (720p) for Feed = 0.5 heights a
  second, at 60 and 30 fps, error ≤ 6e-14; the ring's centroid within 0.011 / 0.048 px.
- **Latch.** 364 pixels cut a frame after Restart (bound 434) at 180p, 5,788 (bound
  6,077) at 720p; every fully cut pixel still cut after the resize (least 0.76 / 0.70).
- **Offline.** 29 names within 16 characters and unique; the reference EDT equal to
  brute force on 41,795 pixels; circles within 0.90 of the cell²/(8ρ) bound; corners
  within 2.7e-7 (bound 3.8e-6); both orders visit every loop once with rapids only
  between loops.
- **Negative controls.** All seven fail their check; all three offline ones too.
- **Mutation.** Caught by two checks (above).
- **No dead controls**, all 25, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, all ten, as the plugin assembles them.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue,
  and exits 1 on a closed stdout and on a reader that leaves.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.toolpath`, ad-hoc signs, and `oxbow` reports `SW Toolpath` /
  `TP01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost** at the defaults on the test card, best of three runs of 60 frames
  after a warm-up, `glFinish` both sides, on a shared GPU:

  | | Latch ms | Live ms | the field ms | trace + order ms (CPU) |
  | --- | --- | --- | --- | --- |
  | 1280×720 | 0.07 | 7.0 | 1.8 | 4.4 |
  | 1920×1080 | 0.10 | 7.4 | 2.3 | 4.6 |
  | 3840×2160 | 0.16 | 16.2 | 10.8 | 4.9 |

  The field is timed inside the plugin with `glFinish` on both sides; the trace
  includes the readback stall. A Latch job pays both once, when it is grabbed.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture is the synthetic card. A thresholded clip
  of real video is a mask full of specks; each speck is a pocket or an island, and
  the ordering's cost (loops squared, capped) and the look are unjudged there.
- **The √2 bound is an argument from the failure mechanism, checked to 4K.** JFA has
  no proven bound; at 8K the finish grows again, but that raster was never run.
- **Live at 4K costs a whole frame** (16 ms); not optimised.
- **The trace at 1080p and 4K** is on a grid 1.5 and 3 px a sample; every check ran at
  or under 1280 wide, where it is one sample a pixel, except by hand at 1920×1080.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
- **The Windows build is CI-only** and CI cannot run yet.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **The provisional About headers and ATTRIBUTIONS** are hand copies (above).
- **Nothing has been through a show.**

---

## Open questions

- **Should the field be computed at a stated fraction above 1080p?** Half resolution
  would take the 4K flood from 11 ms to about 3, at the price of every length being
  measured on a coarser lattice than the picture.
- **Should there be a finishing pass along the medial axis?** Real CAM adds one, and
  it would remove the centre islands; it is a skeleton, which the flood's seeds could
  give (where the nearest outside seeds of neighbours disagree).
- **Should rapids be drawn in Reveal?** They cut nothing, so they are invisible there;
  a tool that visibly lifts and flies would read better.
- **Should Latch re-grab on a clip retrigger?** An effect cannot see one; a sudden
  change in the region (a threshold on the mask's difference) could stand in.
- **Is Live's one-second hold right?** The sweep restarts whenever it finishes, and a
  changing clip changes the path's length under it.

---

## Siblings

- **tinsel** — Detect On, `PassBuffer`'s idea, `sweep.py`, and the fleet's trap list.
- **galvo** — edges traced into ordered paths and driven along them at a physical rate.
- **rebate** — the harness, `--pipe`, `verify.sh`, the negative-control pattern, the
  About hand copies and the "Would this hold" table.
- **slowscan** — `--offline`, `--allow-no-gl` and the CI that knows the runner has no GL.
- **bassalt** — colour triples as consecutive red, green, blue parameters.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
