#pragma once

#include <cstdint>
#include <vector>

/**
	The CAM half: from a sampled distance field to an ordered toolpath, and
	where the tool is on it. No GL anywhere in here, which is what lets
	`tptest --offline` check the tracer on analytic fields with no context.

	**Coordinates** are pixels of the JOB raster (the raster the region was
	grabbed at), GL convention: x to the right, y UP, the centre of pixel
	(i, j) at (i + 0.5, j + 0.5). The harness converts to rows-from-the-top
	where it reads pictures.

	**Tracing** is marching squares on a grid of field samples. Sample (i, j)
	of a `tw` x `th` grid sits at ((i + 0.5) W / tw, (j + 0.5) H / th): the
	centre of the job pixel when the grid is the job raster, the centre of a
	cell of the reduced grid otherwise. The grid is surrounded by a virtual
	ring of samples below every level, so every contour closes. A cell is
	visited once and emits a segment for every level that crosses it -- the
	levels between its lowest and highest corner -- so the work is the
	number of cells plus the total contour length, not cells x levels.
	Saddles are resolved by the mean of the four corners. Where a contour
	runs straight into a corner and straight out of it -- an offset contour
	of a pocket's corner -- the corner is put back where the two straight
	runs meet, rather than cut off by the chord marching squares draws across
	its cell (SharpenCorners). Segments are
	oriented with the higher field on the LEFT, so a loop runs anticlockwise
	round the deeper part of the pocket it encloses (and clockwise round an
	island, which it keeps on its right).

	**Ordering** walks the loops as a forest -- each loop's parent is the
	loop one level out it lies nearest to -- one pocket at a time: Outside In
	cuts a loop before its children, Inside Out after them. A loop is entered
	at its vertex nearest the tool, run once round and closed. Between loops
	the tool makes a RAPID: lifted, straight, at `kRapidFactor` times the
	feed, cutting nothing.

	**Timeline.** The tool's progress is one number, tau, in pixels of CUTTING
	travel: a cut segment of length L takes L of tau, a rapid of length L
	takes L / kRapidFactor. So Feed in px/s is exactly the tool's speed along
	the path while it cuts, which is what `tptest --feed` measures.
*/
namespace toolpath::path
{

struct Point
{
	double x = 0.0;
	double y = 0.0;
};

/// A closed contour at one level, as traced: the first point is not repeated.
struct Loop
{
	std::vector< Point > points;
	int level = 0;
};

struct Levels
{
	double first = 0.0;///< the first level, the tool radius
	double step  = 1.0;///< the stepover
	int count    = 0;
	/// Job pixels a texel of the field the grid was SAMPLED from, when that is
	/// coarser than the grid (0: the samples are the field's own). A corner
	/// of the field's ridge bends the sampled contour over a whole texel, so
	/// SharpenCorners must look that far out for straight runs.
	double fieldTexel = 0.0;
};

/// Test hooks, always 0 in the plugin. `tptest --negative-offline` sets them
/// so the tracer's own checks can be shown to fail.
enum TracePerturb
{
	kTraceNearestCrossing = 1 << 0,///< put a crossing at the nearer sample, not interpolated
	kTraceNoCorners       = 1 << 1,///< join every cell's crossings with the chord, never a corner
};

/// Trace every level of `levels` through a `tw` x `th` grid of samples (row
/// 0 at the BOTTOM), each standing for a cell `cellW` x `cellH` job pixels.
/// Loops shorter than three points are dropped.
std::vector< Loop > TraceLevels( const float* grid, int tw, int th, double cellW, double cellH, const Levels& levels,
                                 int perturb = 0 );

/// Put back the corners marching squares cuts off: where a loop runs
/// straight, turns by more than 45 degrees and runs straight again, the few
/// points between are replaced by the meeting point of the two straight
/// runs. The runs are read from kCornerReach cells out -- or kCornerReach
/// field texels, if `fieldTexel` is coarser than `cell`. Called by
/// TraceLevels; public for the harness.
void SharpenCorners( Loop& loop, double cell, double fieldTexel = 0.0 );

/// Douglas-Peucker on a closed loop, in place: no point of the original is
/// further than `tolerance` from the simplified polygon.
void SimplifyLoop( Loop& loop, double tolerance );

/// The ordered job.
struct Path
{
	std::vector< Point > points;///< vertices, in order
	std::vector< uint8_t > cut; ///< per segment i (points[i] -> points[i+1]): 1 cut, 0 rapid
	std::vector< double > tau;  ///< timeline at each vertex, px of cutting travel
	double cutLength = 0.0;     ///< total length of the cut segments, px
	int loops        = 0;

	bool Empty() const
	{
		return points.size() < 2;
	}
	double Duration() const
	{
		return tau.empty() ? 0.0 : tau.back();
	}
};

constexpr double kRapidFactor = 4.0;

/// Order traced loops into a job, pocket by pocket. `insideOut` cuts each
/// loop after the loops inside it rather than before. `start` is where the
/// tool is before the first move.
Path Order( std::vector< Loop > loops, int levelCount, bool insideOut, Point start );

/// The tool's position at timeline `tau`, clamped to the job.
Point PositionAt( const Path& path, double tau );

/// A piece of cutting between two timeline values: straight, from a to b.
struct Span
{
	Point a;
	Point b;
};

/// Every piece of CUT segment the tool sweeps between tau0 and tau1
/// (tau0 < tau1), clipped at both ends. Rapids are left out. Appended to
/// `out`.
void CutSpans( const Path& path, double tau0, double tau1, std::vector< Span >& out );

/// Every segment of the job, cut and rapid, for drawing the preview.
void AllSegments( const Path& path, std::vector< Span >& cuts, std::vector< Span >& rapids );

} // namespace toolpath::path
