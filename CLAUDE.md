# toolpath

CNC pocketing from a distance field, as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the flood, the tracer, the ordering, the tool's clock,
or any check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/tptest --out /tmp/f.png --size 1920x1080 --frames 240`
- Set anything by name: `--set "Mode=2" --set "Stepover=0.63" --set "Geometry=1"`
  (0..1 for sliders, the element index for options)
- Press an event before a frame: `--press "Restart@45"`
- List parameters, kinds, defaults and ranges: `./build/tptest --list`
- Other sources: `--source white`, `--source black` (default: the test card)
- The exact GLSL the plugin compiles: `./build/tptest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues, linearly interpolated between
  cues (an event such as Restart is pressed on the frame its cue rises through 0.5);
  a cue naming no parameter is refused with exit 2, a partial frame at the end of
  stdin ends the stream cleanly, a failed render or a closed stdout (or a reader that
  leaves) exits 1:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/tptest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the no-GL checks +
  every GL check at 320x180 AND 1280x720 + --pipe + the sweep + a bench + the
  bundle, ~20 s)
- The shaders alone: `tools/check-shaders.sh build/tptest` (needs `brew install shaderc`)
- The field is an exact EDT of its lattice (6 ULP straight, √2 texels curved):
  `./build/tptest --distance`
- The field IS on the working lattice (size, and sign at every pixel):
  `./build/tptest --lattice`
- Inside corners keep a fillet of radius r: `./build/tptest --fillet`
- A slot under 2r never entered, over 2r cut: `./build/tptest --slot`
- Ridges s − 2r past s = 2r, none below: `./build/tptest --scallop`
- Feed px of path a second, at 60 and 30 fps: `./build/tptest --feed`
- Restart clears the part; a resize keeps it: `./build/tptest --latch`
- The checks can fail: `./build/tptest --negative`; one perturbation verbosely:
  `./build/tptest --perturb BITS --fillet` (bits in `Toolpath.h`)
- Everything with no GL (what CI runs): `./build/tptest --offline` (`--names`,
  `--exact`, `--march`, `--negative-offline`)
- On a machine with no GL: add `--allow-no-gl` to the GL checks for a loud SKIP
- Every check takes `--size WxH`; CI runs them at 320x180
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/tptest --bench` (720p, 1080p); `--bench-4k` (all three) once, by hand
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Toolpath.bundle`

## Notes
- **One field is the whole job.** Detect → threshold → flood (1+JFA, then a finish
  that grows with the lattice) → a signed field, inside positive, the frame's edge a
  wall. `Path.cpp` traces its levels r, r + s … on the CPU, puts corners back, and
  orders them pocket by pocket. The GPU stamps the tool's disc along the path into
  the cut buffer. A wrong pass is almost always a `Path.cpp` fix.
- **The field is on a lattice of `kFieldScale` = 2 job pixels a texel, at every
  raster** (detect averages each 2×2 block; a texel is inside when more than half
  its block is). It holds JOB pixels. The cut, the trace grid and the tool stay at
  the job raster. Every check's tolerance is derived for the lattice; `--lattice`
  proves it is the one in use. Change `kFieldScale` and re-derive them all
  (AGENTS.md).
- **Lengths are frame heights**, converted in `Controls.cpp`; the tool, the stepover
  and the feed are in JOB pixels (the raster the region was grabbed at).
- **Nothing absolute crosses from the host clock.** Latch advances Feed × dt, Live
  sweeps Feed × (now − start), both in double; dt is clamped to 0.25 s.
- **A latched job keeps its raster.** The field and the cut stay at the size they
  were grabbed at and are sampled into the output; a resize never re-grabs.
  `--latch` checks it.
- **The flood carries both seeds** (nearest outside, nearest inside) in RGBA16UI with
  squared integer distances, so it is a pure function of the mask on any driver.
  RGBA16UI is why `PassBuffer` is not an `FFGLFBO`.
- **`Perturb` bits are test hooks**, always 0 in the plugin; they exist so
  `--negative` can prove the checks fail. `path::TracePerturb` likewise for the tracer.
- **Parameter names must be unique and ≤ 16 characters** — `--names` checks; a colour
  triple's green and blue are `Stock_Green`, not `Stock Colour_Green`.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses.
  Options are mapped by index.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `toolpath_core` is an OBJECT library, not STATIC — the plugin registers itself from
  a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `TP01`, display name `SW Toolpath`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. On Windows, v0.1.0's CI build met Arena 7.27.1 in the
  fleet gate on win-lab (software rendering); see the README's status.
- Never seen on footage, only on the synthetic card.
- No OpenFX port, no browser demo, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by stoatworks-backend's
  sync-about.py and sync-attributions.py; edit them there.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/toolpath/toolpath.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\toolpath\logs\toolpath.YYYY-MM-DD.log   (Windows)
