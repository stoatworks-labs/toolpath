#pragma once

#include <string>

/**
	The passes.

	Passes 1 to 5 run on the WORKING LATTICE: Toolpath::kFieldScale (2) job
	pixels a texel, ceil( raster / 2 ) texels, at every raster.

	1. **detect** -- lattice size, R16F. The Detect On channel (tinsel's:
	   luma, alpha, chroma, or luma-or-alpha), read from the host's texture
	   with texelFetch and averaged over each texel's 2 x 2 block of pixels.
	2. **blur** -- lattice size, R16F, twice (x then y), only when Smooth is
	   up: a Gaussian on the channel before the threshold, sigma in texels.
	3. **seed** -- lattice size, RGBA16UI. The threshold (and Invert) decides
	   inside and outside -- for a clean mask, a texel is inside when more
	   than half its block is; each texel starts the flood knowing only
	   itself: (nearest outside x, y, nearest inside x, y), 65535 for "none
	   yet".
	4. **flood** -- the jump flood, ping-ponged: a step-1 pass first (the
	   "1+" of 1+JFA), then steps N/2, N/4 ... 1, then a finish from N/128
	   down to 1 again. Each pass looks at the eight texels `Step` away and
	   keeps, for each of the two seeds, whichever is nearest.
	5. **resolve** -- the signed distance field, R32F, in JOB pixels: inside
	   positive, the distance to the nearest outside texel centre less half a
	   texel, times 2; the job frame's edge is a wall; outside negative, the
	   same way round.
	6. **sample** -- the field at the trace grid's sample points, bilinear,
	   R32F, read back for the CPU tracer. Skipped when the grid is the
	   lattice itself.
	7. **stamp** -- instanced capsules. One quad per piece of path, aligned
	   with it, blended with MAX: the tool's disc swept along the piece, into
	   the cut buffer; or thin lines and the tool's ring into the overlay.
	8. **composite** -- to the host. Reveal, Engrave, Paths or Field, then Mix.

	Every shader is assembled at run time from these strings, so
	`tptest --dump-shaders DIR` writes out exactly what the plugin compiles,
	and that is what `tools/check-shaders.sh` hands to glslc.
*/
namespace toolpath::shaders
{

std::string Vertex();
std::string Detect();
std::string Blur();
std::string Seed();
std::string Flood();
std::string Resolve();
std::string Sample();
std::string StampVertex();
std::string StampFragment();
std::string Composite();

/// Value the flood stores for "no seed of this kind yet".
constexpr unsigned kNoSeed = 65535u;

} // namespace toolpath::shaders
