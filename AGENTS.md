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
2. **Field**, on the **working lattice**: `kFieldScale` = 2 job pixels a texel, at
   every raster. Detect averages each 2×2 block, so after the threshold a texel is
   inside when more than half its block is. A jump flood carrying both seeds — each
   texel's nearest outside texel and nearest inside texel — then a resolve: inside,
   the distance to the nearest outside texel centre less half a texel, times 2 into
   job pixels (or the distance to the job frame's edge, which is a wall); outside,
   minus the same. So a straight wall sits half way between the last inside centre
   and the first outside one, and the field is exactly the distance to it.
3. **Toolpath.** The field sampled bilinearly onto a trace grid (one sample a job
   pixel up to 1280 wide), read back, traced by marching squares at every level in
   one pass over the cells, corners put back (looking a texel out, not a cell),
   simplified to 0.2 px, and ordered one pocket at a time.
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

Per frame: **detect → [blur ×2] → seed → flood ×N → resolve**, all on the lattice
(only when a region is grabbed: every frame in Live, once per job in Latch) →
**sample + readback + trace +
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
(All of this was at the full raster. The field is now on a lattice of two pixels,
the same argument holds texel for texel, and the bound is √2 texels, 2.83 px; the
finish is 1/128 of the lattice's side, and the worst measured is 0.53 texels.)

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
got it exactly right. It is flooded at its own lattice — the largest 320×180 × 2^j
of FIELD TEXELS that fits the requested raster — so it is the known bad case at
every raster the check is asked for. Since the field moved onto a lattice of two
pixels, each island is drawn as a 2×2 block (one texel), and the job raster is
twice the lattice: at a requested 320×180 the constellation renders at 640×360,
the smallest raster whose lattice is 320×180.

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

### ☠️ The flood cost 11 ms at 4K, so it moved onto a lattice of two pixels

Twenty passes of RGBA16UI at 3840×2160: 10.8 ms of Live's 16.2 at 4K, a whole
60 fps frame. The field is now computed on a lattice of `kFieldScale` = 2 job
pixels a texel, **at every raster** — 2.9 ms of 9.0 at 4K. Not a cap: a cap at
1080 or 720 lines would leave 320×180 and 1280×720 at full resolution, and every
check in `verify.sh` measuring code 4K never runs. With a fixed 2, both verify
rasters run exactly what ships at 4K, and every tolerance was re-derived for it
(the table below) rather than widened until it passed. At 720p and 1080p it buys
nothing measurable: the field there costs ~20 passes' overhead, not their pixels.

What the lattice costs, stated: the region is a threshold of 2×2 means, so a wall
half way across a block moves a pixel INTO the pocket (never out), and a feature
narrower than the lattice — a one-pixel line, a lone pixel — is not seen at all;
the field's bound on curves is √2 texels, 2.83 px. `--lattice` measures the first
two out of the plugin's own field; its negative control floods at the full raster
and fails.

### ☠️ A field sampled from a coarser lattice bends every corner over a texel

The first run on the lattice put the fillet at 9.350 for r = 9 at 320×180 (0.35 px
out; it had been 8.981), and `--feed` at 89.64 px/s for 90. One cause: the trace
grid samples the field bilinearly, and where the field's ridge runs into an offset
contour's corner the samples bend over a whole texel — two trace cells. The corner
putback (`SharpenCorners`) read its straight runs from two points out, which were
still on the bend, so it saw no corner and the chord stayed; Douglas–Peucker then
merged the chord's two ends (0.18 px each way) into one vertex. The tool's corner
fell short, so the fillet read large, and the first pass started on a chamfer, so
the tool's first second along it was 0.36 px short. `Levels::fieldTexel` now sets
the reach in trace cells to cover a field texel; the fillet reads 8.981 again, the
feed is exact. `--feed` catching a tracer defect is not something it was designed
to do: keep the checks independent and they cross-examine each other.

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
- **`--slot`'s wide slot on the lattice passed by float noise** (237e218): drawn as
  the least integer over 2r + 2k, it came out 22 px at 320×180 because the Tool
  Diameter control's round trip leaves r at 8.99999, and the slot sat in the derived
  worst case, its sampled peak 9.000 against r. Found by reading the check's own
  print ("22 px > 2r + 2k = 22.00") against the rule it claimed. Now 2r + 2k + 1,
  half a pixel of peak over the worst case, as the full-raster fixture had.
- **The √2 story above:** a bound argued, not measured, and false at 1080p.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`, and at 640×480, 333×187 and 1920×1080 by hand
(`--distance` also once at 3840×2160). k is `kFieldScale`, 2: every check runs on
the field's lattice, and the checks that read the pocket's walls read them where
the lattice put them (`latticeEdge`, the majority rule derived in the harness).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--distance` square, frame | the plugin's field (R32F, read back, one value a texel) against Felzenszwalb–Huttenlocher's exact EDT of the mask REDUCED to the lattice by the plugin's rule, in job pixels | **6 ULP** of the texel distance, times k: GLSL 4.10 §4.7.1 gives sqrt as 1/inversesqrt, 2 + 2.5 ULP, plus one rounding of the half-texel subtraction; the squared distances are exact integers, and the scaling by 2 is exact; the frame's wall is a multiple of a half, exact | none: rectilinear nearest centres lie along rows and columns of the lattice |
| `--distance` disc, ring, star, blobs | the same | **√2 texels = √2 k = 2.83 px**: the flood's failure on a digitised boundary leaves a texel the seed of a neighbouring cell, and neighbouring boundary seeds are 8-neighbours on the lattice (triangle inequality). Holds only if the finish grows with the lattice (above) | the finish is 1/128 of the lattice's longest side; measured 0.14 / 0.29 / 0.30 / 0.53 texels (0.28 / 0.58 / 0.59 / 1.05 px) at 180p / 720p / 1080p / 4K |
| `--distance` constellation | the same, the known bad case | **√2 texels** with the prepass (measured exact); must exceed it without | four 2×2 blocks, flooded on 320×180 × 2^j texels, its own lattice, whatever raster is asked for (640×360 job pixels at a requested 320×180) |
| `--lattice` | the field's size, and its sign at every job pixel against the harness's own 2×2 majority of the mask, on odd walls, an outside speck and a one-pixel hairline | **exact**: ceil(W/2) × ceil(H/2) texels, 0 pixels of the wrong sign; and the fixture must discriminate (the job-raster mask disagrees with the reduced one at > 0 pixels: 823 at 180p, 3,301 at 720p), or a full-raster field could pass | the fixture is placed from the raster and forced odd at every size |
| `--fillet` | a circle tangent to both WORKING walls (in a pixel at 180p, where the square's edges are odd; unmoved at 720p) fitted to 61 ray crossings of the cut coverage (bilinear) | **1 px** on the radius (the spec), now at the pixel in the worst case: the corner's cut across a field TEXEL, a quarter of its diagonal (0.71 px; the bilinear corner of min(x, y) sits 0.59 px in) + Douglas–Peucker 0.2 + a crossing 0.09 = 0.997 — and in fact the corner is put back exactly, since `SharpenCorners` reaches a texel out. Each point within **0.5 px** of the fitted circle (a crossing read off a one-pixel ramp, clamped on one side, moves by less than the ramp's half-width) | rays from 15° to 75° stay 0.13 r clear of both walls, so no tap reaches the black outside at r ≥ 8 px (r = 9 at 180p) |
| `--slot` narrow | cut coverage past mouth + k/2 + one trace cell + r + 1 px, the whole slot | **exactly 0**: the lattice only narrows a slot, so w/2 < r still; the sampled field never exceeds the true one; a trace sample reads r or more only if a texel it interpolates does, and all of those are left of the mouth, so no sample from mouth + k/2 on does, and a crossing lands at most a cell further; the stamp's ramp ends at r + 0.5 | the trace cell (1 px up to 1280 wide) and the texel are in the margin |
| `--slot` wide | the working slot's centre row, mouth to within a pixel of its working end wall | coverage **≥ 0.5** everywhere: the sampled peak is low by up to k/2 for the walls (each moves in by up to k/2) and k/2 for the texel centres nearest the middle, so it is at least w/2 − k, and w keeps half a pixel of peak over that: w/2 − k ≥ r + ½, w = 2r + 2k + 1 (23 px at 180p, 77 at 720p). At k = 1 with only the texel's half pixel to lose, the same rule gives the 2r + 2 this check used to draw. Not "the least integer over 2r + 2k", which 237e218 used: 22 at 180p, over only by the control's float round trip (r = 8.99999), in the worst case exactly (9.000 against 8.99999); 9124b4f | w and r both scale with the height |
| `--scallop` s > 2r | uncut run widths between 0.5 crossings, rows beside the straight part of the working left wall | **0.25 px**: two crossings, each read off a one-pixel ramp with one sample possibly clamped, worst case a(a − ½)/(a + ½) = 0.086 px each; the field, the trace and the passes are exact on a straight wall (the field is linear there, and bilinear at quarter texels is exact, even in 8-bit weights); the lattice moves the wall and every pass with it, which a width does not see | rows chosen from r, the span and the working walls at each raster |
| `--scallop` s ≤ 2r | the least coverage from the working wall | **≥ 0.5 − 2^-11**: at s = 2r bands meet at exactly 0.5, and the cut buffer is R16F | none |
| `--feed` | the tool's displacement over one second on a straight pass, at 60 and 30 fps | **1e-9 of Feed**: positions and time are double; one float ULP would be 1e-7. Blind to the lattice, except that a pass must start on a corner, not a chamfer (the trap above) | none: Feed is in job heights |
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
the bilinear sampling of the lattice's field onto the trace grid (at and under 1280
wide every sample is at a quarter texel, weights 1/4 and 3/4, exact in 8 bits; above
it, arbitrary weights, which no check at those rasters is tight enough to see);
the stamp's quads cover whole pixels by their centres, which every conforming
rasteriser agrees on; `sqrt` within its stated ULPs. The GL checks have never run on a
software rasteriser: CI would run them there if its runner could make a 4.1 context.

---

## Negative controls and the mutation

`tptest --negative` runs eight, and `--perturb BITS` runs any check verbosely against
one. Each perturbs the *plugin* — a `Perturb` bit the shipped plugin carries at zero
— never the harness's expectation. At 320×180:

| perturbation | what fails |
| --- | --- |
| no 1+JFA prepass (the spec's) | `--distance`: the constellation 5.69 texels (11.4 px) out, bound √2 texels (11.6 texels, 23.2 px, at 720p) |
| a square tool (the spec's) | `--fillet`: the fitted radius 2.25 for r = 9, points 0.64 px off any circle |
| the first pass at r/2 | `--slot`: 2,208 pixels cut in a slot 2 px narrower than the tool |
| the stepover read in radii | `--scallop`: no ridges at 1.25, 1.5 or 2 diameters |
| Feed / 60 a frame, whatever dt | `--feed`: 45 px/s against 90 at 30 fps (right at 60) |
| a resize that re-grabs the job | `--latch`: 15,304 of 16,608 successors uncut, the job at the new raster |
| a Restart that keeps the cut | `--latch`: 4,468 pixels still cut after Restart, bound 434 |
| the field at the full raster (`kPerturbFullResField`) | `--lattice`, on both counts: the field 320×180 against 160×90, and 823 pixels of the wrong sign — the odd walls, the speck and the hairline (1280×720 against 640×360 and 3,301 at 720p) |

And `--negative-offline`, with no GL: the reference EDT with a city-block distance
fails `--exact`; crossings put at the nearer sample, and corners left as chords, each
fail `--march`.

### The mutation

One character of the shipped GLSL, on a clean committed tree (237e218): in the
resolve pass, `( centreDistance( p, s.xy ) - 0.5 ) * Scale` → `+ 0.5` (every inside
distance to stock one texel, 2 px, too long). Caught by `--distance` (the square
2.000 px out against 6 ULP) and `--fillet` (radius 2.25 for 9 at 320×180, 29.0 for
36 at 1280×720) at both rasters, and by `--slot` at 320×180 (the narrow slot's field
rose from 7 to 9 = r, and 2,206 pixels were cut). **Not** caught by `--slot` at
1280×720 (35 against r = 36), `--scallop` (widths do not see a uniform offset),
`--feed`, `--latch` or `--lattice` (the sign is unchanged), correctly; nor by the
frame, whose field is the wall term alone, which this line no longer touches. The
curved shapes passed too: a uniform texel is under √2 texels, which is why the
rectilinear shapes are held to the ULP. Reverted with `git checkout
source/Shaders.cpp`; the tree was clean before and after.

The first mutation, before the lattice, changed `min( toStock, float( wall ) ) -
0.5`, a line that no longer exists; it was caught by `--distance` (square and frame)
and `--fillet`.

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
- **The flood is 1 + JFA + a finish from 1/128 of the longest side** (above), on the
  working lattice. The spec's 1+JFA is the prepass; the finish is mine.
- **The field on a lattice of two job pixels a texel, at every raster**
  (`kFieldScale`, 2026-09-23, asked for so Live fits a 4K frame). Fixed, not capped at
  a height, so the verify rasters run the 4K code (the trap above). Two, not four:
  at two the 4K field is 2.9 ms and the CPU trace (4.7 ms) is already the larger
  share of Live, so four would save under 2 ms at 4K and double every lattice term in
  every tolerance; and at 320×180 a tool of r = 9 px is still 4.5 texels. The
  region is the threshold of each 2×2 block's MEAN (texelFetch, so the host's
  filtering never enters), which for a clean mask is "more than half the block", a
  tie going outside, so the pocket only ever shrinks onto the lattice. Smooth's
  sigma is converted to texels, and under 0.3 texels (0.6 px) no blur runs: the
  block mean is already that much smoothing. An odd raster's last texel overhangs
  the frame by a pixel; the frame's wall is measured on the job raster and the
  sample and composite passes map the job's 0..1 onto the lattice's (`FieldUV`), so
  nothing moves at 333×187. The cut, the trace grid, the tool and every length stay
  in job pixels.
- **`SharpenCorners` reaches a field texel out**, not only a trace cell, when the
  grid is sampled from a coarser field (`Levels::fieldTexel`).
- **One sample a pixel up to 1280 wide on the trace grid**, sampling the lattice's
  field bilinearly. At 1280×720 and under every sample is at a quarter texel;
  above, a sample covers 1.5 px (1080p) or 3 px (4K) and Live's trace stays near
  4.6 ms.
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

- **Distance**, on the lattice of two pixels, against the exact EDT of the reduced
  mask. Square and frame exact (max 1e-4 px at 1080p and 4K, inside 6 ULP). Disc,
  ring, star, blobs, max in px: 0.28 / 0 / 0.08 / 0 at 320×180; 0.58 / 0.06 / 0 /
  0.15 at 1280×720; 0.59 / 0.44 / 0.03 / 0.20 at 1920×1080; 1.05 / 0.68 / 0.17 / 0.22
  at 3840×2160 (once, by hand) — the worst 0.53 texels against √2. The constellation
  exact at every raster; without the prepass 5.69 texels (11.4 px, lattice 320×180),
  11.60 (23.2 px, 640×360), 23.4 (46.9 px, 1280×720 at 4K); plain JFA 18.5, 38.6,
  78.6 px. At the full raster, before the lattice, the curved maxima were 0 / 0.36 /
  0.47 / 0.91 px: in texels the lattice's are no worse, in pixels up to 1.05.
- **Lattice.** The field 160×90 at 320×180 and 640×360 at 1280×720 (and ceil(W/2) ×
  ceil(H/2) at 333×187), 0 pixels of the wrong sign, on fixtures where the
  full-raster mask disagrees at 823 and 3,301 pixels.
- **Fillet.** Radius 8.981 for 9 at 320×180 (walls a pixel in, on the lattice),
  36.002 for 36 at 1280×720, all four corners; every boundary point within 0.057 /
  0.064 px of the fitted circle — the same figures as at the full raster.
- **Slot.** 16 px against 2r = 18: 0 pixels cut over 138 px of slot; 23 px (2r + 2k + 1):
  its centre row cut throughout (least coverage 1.0). 70 and 77 against 72 at 720p,
  the same; 24, 53 and 113 px at 333×187, 640×480 and 1920×1080.
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
- **Negative controls.** All eight fail their check; all three offline ones too.
- **Mutation.** Caught by two checks at both rasters and a third at 320×180 (above).
- **No dead controls**, all 25, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, all ten, as the plugin assembles them.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue,
  and exits 1 on a closed stdout and on a reader that leaves.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.toolpath`, ad-hoc signs, and `oxbow` reports `SW Toolpath` /
  `TP01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost** at the defaults on the test card, best of three runs of 60 frames
  after a warm-up, `glFinish` both sides, on a shared GPU — one run of
  `build-universal/tptest --bench-4k --frames 60` after `verify.sh` passed, the
  field on its lattice:

  | | Latch ms | Live ms | the field ms | trace + order ms (CPU) |
  | --- | --- | --- | --- | --- |
  | 1280×720 | 0.08 | 6.5 | 1.8 | 4.6 |
  | 1920×1080 | 0.12 | 7.3 | 2.6 | 4.5 |
  | 3840×2160 | 0.16 | **9.0** | **2.9** | 4.7 |

  Before, with the field at the full raster (b58aa2f, the same machine, earlier the
  same day): Live 7.0 / 7.4 / **16.2**, the field 1.8 / 2.3 / **10.8**. So at 4K the
  field is 3.7× cheaper and Live 1.8×; at 720p and 1080p nothing measurable changed
  (the 1080p field read 2.6 against 2.3, within what a shared GPU moves between
  runs). The field is timed inside the plugin with `glFinish` on both sides; the
  trace includes the readback stall. A Latch job pays both once, when it is
  grabbed.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture is the synthetic card. A thresholded clip
  of real video is a mask full of specks; each speck is a pocket or an island, and
  the ordering's cost (loops squared, capped) and the look are unjudged there.
- **The √2-texel bound is an argument from the failure mechanism, checked to 4K**
  (a lattice of 1920×1080). JFA has no proven bound; at 8K the finish grows again,
  but that raster was never run.
- **Live at 4K costs 9.0 ms**, a little over half a 60 fps frame, of which the CPU
  trace is 4.7. Whether Resolume, compositing its own layers, leaves that much of a
  frame is unmeasured.
- **The lattice's look is unjudged.** Two-pixel steps in the region, and a one-pixel
  line in the clip that never becomes a pocket, are measured, not seen on footage.
- **The trace at 1080p and 4K** is on a grid 1.5 and 3 px a sample; every check ran at
  or under 1280 wide, where it is one sample a pixel, except by hand at 1920×1080.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
- **The Windows build is CI-only** and CI cannot run yet.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **The provisional About headers and ATTRIBUTIONS** are hand copies (above).
- **Nothing has been through a show.**

---

## Open questions

- **Should the trace follow the lattice?** The field now costs 2.9 ms at 4K and the
  CPU trace 4.7; the trace grid samples the lattice's field onto one sample a job
  pixel up to 1280 wide, which at 720p is four samples a texel. Tracing on the
  lattice itself would quarter the CPU work at and under 1280 wide, at the price of
  re-deriving `--slot`'s and `--fillet`'s cell terms again. (The field's own
  fraction was the question here; it is answered above: two, at every raster.)
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
