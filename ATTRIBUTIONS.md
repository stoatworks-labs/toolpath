# Attributions

Toolpath is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Toolpath is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Within the fleet

Not third-party, but owed a line. Detect On — the four ways a picture becomes a
region, and the Luma or Alpha trap — is **tinsel**'s (`github.com/stoatworks-labs/tinsel`,
MIT, Stoatworks Labs), as is the idea of `PassBuffer`. The harness shape, the
`--pipe` contract, `verify.sh`, the negative-control pattern and the provisional About
headers are **rebate**'s; the host clock-unit voting is **readout**'s by way of
rebate; `--offline`, `--allow-no-gl` and the CI shape are **slowscan**'s; the idea of
a traced path driven at a physical rate is **galvo**'s; colour triples as consecutive
red, green and blue parameters are **bassalt**'s.

## Science, not code

The algorithms are implemented here from their published descriptions, not copied
from anyone's source:

- **Jump flooding** and its 1+JFA and JFA+k variants — Guodong Rong and Tiow-Seng
  Tan, *Jump Flooding in GPU with Applications to Voronoi Diagram and Distance
  Transform*, I3D 2006.
- **The exact Euclidean distance transform** the harness checks the flood against —
  Pedro Felzenszwalb and Daniel Huttenlocher, *Distance Transforms of Sampled
  Functions*, Theory of Computing 8 (2012).
- **Marching squares**, the two-dimensional case of William Lorensen and Harvey
  Cline's marching cubes (SIGGRAPH 1987), with saddles resolved by the cell mean.
- **Douglas–Peucker** polyline simplification — David Douglas and Thomas Peucker,
  *The Canadian Cartographer* 10 (1973).
- **Morphological opening** by a disc, and contour-parallel (offset) pocketing, are
  textbook mathematical morphology and CAM; no CAM package's code or toolpaths were
  used.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
