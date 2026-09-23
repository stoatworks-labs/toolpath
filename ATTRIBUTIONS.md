# Attributions

Toolpath is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Detect On and PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The four ways a picture becomes a region (Luma, Alpha, Chroma, Luma or Alpha) and the Luma-or-Alpha trap are tinsel's, as is the idea of PassBuffer, the off-screen buffer that reallocates only when the size changes.

### Harness shape, --pipe contract and verify — Stoatworks rebate

<https://github.com/stoatworks-labs/rebate>  
Licence: MIT  
Copyright: Stoatworks Labs

The offline harness's shape, the --pipe frame contract, tools/verify.sh, the negative-control pattern and the provisional About headers are rebate's; the host clock-unit voting is readout's by way of rebate.

### --offline, --allow-no-gl and the CI shape — Stoatworks slowscan

<https://github.com/stoatworks-labs/slowscan>  
Licence: MIT  
Copyright: Stoatworks Labs

The --offline selector, the --allow-no-gl loud skip and the two workflows follow slowscan's.

### A traced path at a physical rate — Stoatworks galvo

<https://github.com/stoatworks-labs/galvo>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of a traced path driven at a physical rate is galvo's; colour triples as consecutive red, green and blue parameters are bassalt's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### CNC pocketing and mathematical morphology

Morphological opening by a disc and contour-parallel (offset) pocketing are textbook mathematical morphology and CAM. Implemented from that description; no CAM package's code or toolpaths were used.

## Standards and published specifications

What the implementation is measured against.

- **Guodong Rong and Tiow-Seng Tan, "Jump Flooding in GPU with Applications to Voronoi Diagram and Distance Transform" (I3D 2006)** — Jump flooding and its 1+JFA and JFA+k variants, implemented from the paper.
- **Pedro Felzenszwalb and Daniel Huttenlocher, "Distance Transforms of Sampled Functions" (Theory of Computing 8, 2012)** — The exact Euclidean distance transform the harness checks the flood against.
- **William Lorensen and Harvey Cline, "Marching Cubes" (SIGGRAPH 1987)** — Marching squares, its two-dimensional case, with saddles resolved by the cell mean.
- **David Douglas and Thomas Peucker, polyline simplification (The Canadian Cartographer 10, 1973)** — Douglas-Peucker simplification of the traced passes.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
