#include "Path.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace toolpath::path
{
namespace
{
struct Segment
{
	uint64_t startKey;
	uint64_t endKey;
	Point start;
	int level;
};

/// A corner, for SharpenCorners: the straight runs either side turn by more
/// than 45 degrees, and each run is straight to 5 degrees over its three
/// points. A circle of radius rho turns by about cell / rho per point, so its
/// runs are not straight until rho is over ~11 cells, and by then three
/// points turn by far less than 45 degrees: no circle is ever sharpened.
constexpr double kCornerCosine   = 0.7071067811865476;//cos 45 degrees
constexpr double kStraightCosine = 0.9961946980917455;//cos 5 degrees

/// How far out, in points, the straight runs either side of a corner start;
/// the points between them are the ones a corner can have put wrong.
constexpr int kCornerReach = 2;

double distance( const Point& a, const Point& b )
{
	return std::hypot( b.x - a.x, b.y - a.y );
}

/// Distance from p to the segment ab.
double distanceToSegment( const Point& p, const Point& a, const Point& b )
{
	const double dx = b.x - a.x, dy = b.y - a.y;
	const double len2 = dx * dx + dy * dy;
	double t          = len2 > 0.0 ? ( ( p.x - a.x ) * dx + ( p.y - a.y ) * dy ) / len2 : 0.0;
	t                 = std::clamp( t, 0.0, 1.0 );
	return std::hypot( p.x - ( a.x + t * dx ), p.y - ( a.y + t * dy ) );
}
} // namespace

//---------------------------------------------------------------------------
std::vector< Loop > TraceLevels( const float* grid, int tw, int th, double cellW, double cellH, const Levels& levels,
                                 int perturb )
{
	std::vector< Loop > loops;
	if( grid == nullptr || tw <= 0 || th <= 0 || levels.count <= 0 || !( levels.step > 0.0 ) )
		return loops;

	//The virtual ring of samples round the grid: half a cell outside the
	//frame, which is the wall, so the field there is minus half a cell. Every
	//level is above it, so every contour closes; and for the linear field
	//beside a straight wall a crossing interpolated against it lands exactly
	//where the wall's own field says.
	const double ring = -0.5 * std::max( cellW, cellH );
	const int pw      = tw + 2;
	auto value        = [ & ]( int i, int j ) -> double {
        if( i < 0 || j < 0 || i >= tw || j >= th )
            return ring;
        return static_cast< double >( grid[ static_cast< size_t >( j ) * tw + i ] );
	};
	auto position = [ & ]( int i, int j ) {
		return Point{ ( i + 0.5 ) * cellW, ( j + 0.5 ) * cellH };
	};
	//Edge keys. Horizontal edge (i, j)-(i+1, j) and vertical edge (i, j)-(i, j+1),
	//in padded coordinates, per level.
	auto key = [ & ]( int level, int vertical, int i, int j ) -> uint64_t {
		return ( static_cast< uint64_t >( level ) << 34 ) | ( static_cast< uint64_t >( vertical ) << 33 )
		       | static_cast< uint64_t >( ( j + 1 ) * pw + ( i + 1 ) );
	};
	//The crossing on an edge, always interpolated from the edge's lower
	//corner, so both cells that share an edge would compute the same point.
	auto crossing = [ & ]( double level, int vertical, int i, int j ) {
		const int i1    = vertical ? i : i + 1;
		const int j1    = vertical ? j + 1 : j;
		const double a  = value( i, j );
		const double b  = value( i1, j1 );
		double t        = ( b != a ) ? ( level - a ) / ( b - a ) : 0.5;
		t               = std::clamp( t, 0.0, 1.0 );
		if( perturb & kTraceNearestCrossing )
			t = std::round( t );
		const Point p0 = position( i, j );
		const Point p1 = position( i1, j1 );
		return Point{ p0.x + t * ( p1.x - p0.x ), p0.y + t * ( p1.y - p0.y ) };
	};

	std::vector< Segment > segments;
	segments.reserve( static_cast< size_t >( tw + th ) * 8 );

	for( int j = -1; j < th; ++j )
	{
		for( int i = -1; i < tw; ++i )
		{
			//Corners anticlockwise from the bottom left.
			const double v[ 4 ] = { value( i, j ), value( i + 1, j ), value( i + 1, j + 1 ), value( i, j + 1 ) };
			const double lo     = std::min( { v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ] } );
			const double hi     = std::max( { v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ] } );
			if( hi < levels.first )
				continue;

			//Levels L with lo < L <= hi cross this cell; the bracket is
			//widened by one each way and every candidate tested directly, so
			//a level that lands exactly on a sample is neither lost nor
			//doubled by the division's rounding.
			int k0 = static_cast< int >( std::floor( ( lo - levels.first ) / levels.step ) );
			int k1 = static_cast< int >( std::floor( ( hi - levels.first ) / levels.step ) ) + 1;
			k0     = std::max( k0, 0 );
			k1     = std::min( k1, levels.count - 1 );

			for( int k = k0; k <= k1; ++k )
			{
				const double level = levels.first + k * levels.step;
				bool in[ 4 ];
				int inside = 0;
				for( int n = 0; n < 4; ++n )
				{
					in[ n ] = v[ n ] >= level;
					inside += in[ n ] ? 1 : 0;
				}
				if( inside == 0 || inside == 4 )
					continue;

				//Edge n joins corner n to corner n + 1, anticlockwise. Its
				//key and crossing are those of the canonical grid edge.
				struct EdgeRef
				{
					int vertical, i, j;
				};
				const EdgeRef edges[ 4 ] = {
					{ 0, i, j },    //bottom: (i, j)-(i+1, j)
					{ 1, i + 1, j },//right:  (i+1, j)-(i+1, j+1)
					{ 0, i, j + 1 },//top:    (i, j+1)-(i+1, j+1)
					{ 1, i, j },    //left:   (i, j)-(i, j+1)
				};


				//A segment runs from an edge where the inside is LEAVING
				//(corner n in, corner n+1 out) to an edge where it is
				//ENTERING (n out, n+1 in): then the inside is on its left.
				//With two of each (a saddle), the cell's mean decides which
				//corners the segments cut off: the outside ones if the
				//centre is inside, so the inside stays connected; the inside
				//ones otherwise.
				const bool centreInside = ( v[ 0 ] + v[ 1 ] + v[ 2 ] + v[ 3 ] ) * 0.25 >= level;
				for( int n = 0; n < 4; ++n )
				{
					if( !( in[ n ] && !in[ ( n + 1 ) % 4 ] ) )
						continue;
					int m;
					if( inside == 2 && in[ ( n + 2 ) % 4 ] )
						m = centreInside ? ( n + 1 ) % 4 : ( n + 3 ) % 4;//saddle
					else
					{
						//The one entering edge.
						m = -1;
						for( int e = 0; e < 4; ++e )
							if( !in[ e ] && in[ ( e + 1 ) % 4 ] )
								m = e;
					}
					if( m < 0 )
						continue;

					Segment s;
					s.level     = k;
					s.startKey  = key( k, edges[ n ].vertical, edges[ n ].i, edges[ n ].j );
					s.endKey    = key( k, edges[ m ].vertical, edges[ m ].i, edges[ m ].j );
					s.start     = crossing( level, edges[ n ].vertical, edges[ n ].i, edges[ n ].j );
					segments.push_back( s );
				}
			}
		}
	}

	//Chain: every crossing starts exactly one segment and ends exactly one.
	std::unordered_map< uint64_t, size_t > byStart;
	byStart.reserve( segments.size() * 2 );
	for( size_t n = 0; n < segments.size(); ++n )
		byStart.emplace( segments[ n ].startKey, n );

	std::vector< uint8_t > used( segments.size(), 0 );
	for( size_t n = 0; n < segments.size(); ++n )
	{
		if( used[ n ] )
			continue;
		Loop loop;
		loop.level = segments[ n ].level;
		size_t at  = n;
		while( !used[ at ] )
		{
			used[ at ] = 1;
			loop.points.push_back( segments[ at ].start );
			const auto next = byStart.find( segments[ at ].endKey );
			if( next == byStart.end() )
				break;
			at = next->second;
		}
		if( loop.points.size() >= 3 )
		{
			if( !( perturb & kTraceNoCorners ) )
				SharpenCorners( loop, std::max( cellW, cellH ) );
			loops.push_back( std::move( loop ) );
		}
	}

	return loops;
}

//---------------------------------------------------------------------------
// Corners. Marching squares joins a cell's two crossings with a chord, and
// where the contour has a corner inside the cell the chord cuts it off -- by
// up to a quarter of the cell's diagonal. Worse, the field of a pocket has a
// RIDGE running into every corner of an offset contour (its medial axis),
// and a crossing on a cell edge the ridge also crosses is interpolated
// across the kink and lands off the contour. A pocket's offset contours have
// a corner wherever the pocket does, which is exactly where the fillet is.
//
// So: where the contour runs straight, turns by more than 45 degrees, and
// runs straight again, the corner is where the two straight runs' lines
// meet, and the few points between them -- the ones the ridge can have put
// wrong -- are replaced by it. The lines are taken from points kCornerReach
// and more away, which lie on cell edges the ridge does not cross.
//---------------------------------------------------------------------------
void SharpenCorners( Loop& loop, double cell )
{
	const int n = static_cast< int >( loop.points.size() );
	if( n < 4 * ( kCornerReach + 2 ) )
		return;
	auto P = [ & ]( int i ) -> const Point& {
		return loop.points[ static_cast< size_t >( ( ( i % n ) + n ) % n ) ];
	};
	auto unit = []( const Point& a, const Point& b, Point& u ) {
		const double l = std::hypot( b.x - a.x, b.y - a.y );
		if( l <= 1e-12 )
			return false;
		u = Point{ ( b.x - a.x ) / l, ( b.y - a.y ) / l };
		return true;
	};
	auto dot = []( const Point& a, const Point& b ) {
		return a.x * b.x + a.y * b.y;
	};

	//Candidate corners: the tightest turn in each neighbourhood.
	std::vector< double > turn( static_cast< size_t >( n ), 0.0 );
	for( int i = 0; i < n; ++i )
	{
		Point u, v;
		if( unit( P( i - 1 ), P( i ), u ) && unit( P( i ), P( i + 1 ), v ) )
			turn[ static_cast< size_t >( i ) ] = 1.0 - dot( u, v );
	}

	struct Corner
	{
		int at;
		Point x;
	};
	std::vector< Corner > corners;
	for( int i = 0; i < n; ++i )
	{
		bool peak = true;
		for( int k = -kCornerReach; k <= kCornerReach && peak; ++k )
			if( k != 0 && turn[ static_cast< size_t >( ( ( i + k ) % n + n ) % n ) ] > turn[ static_cast< size_t >( i ) ] )
				peak = false;
		if( !peak || turn[ static_cast< size_t >( i ) ] <= 1e-12 )
			continue;

		//The straight runs: three points each, starting kCornerReach out.
		const Point& a0 = P( i - kCornerReach - 2 );
		const Point& a1 = P( i - kCornerReach - 1 );
		const Point& a2 = P( i - kCornerReach );
		const Point& b0 = P( i + kCornerReach );
		const Point& b1 = P( i + kCornerReach + 1 );
		const Point& b2 = P( i + kCornerReach + 2 );
		Point ua, ua2, ub, ub2;
		if( !unit( a0, a1, ua ) || !unit( a1, a2, ua2 ) || !unit( b0, b1, ub ) || !unit( b1, b2, ub2 ) )
			continue;
		if( dot( ua, ua2 ) < kStraightCosine || dot( ub, ub2 ) < kStraightCosine )
			continue;//not straight either side: a curve, not a corner
		if( dot( ua2, ub ) > kCornerCosine )
			continue;//straight, but not turning enough to be a corner

		//a2 + s ua2 = b0 + t ub.
		const double det = ua2.x * ( -ub.y ) - ua2.y * ( -ub.x );
		if( std::fabs( det ) < 1e-12 )
			continue;
		const double rx = b0.x - a2.x, ry = b0.y - a2.y;
		const double s  = ( rx * ( -ub.y ) - ry * ( -ub.x ) ) / det;
		const Point x   = { a2.x + s * ua2.x, a2.y + s * ua2.y };

		//It must be the corner these points were cutting: every point
		//replaced within reach of it.
		bool near = true;
		for( int k = -kCornerReach + 1; k <= kCornerReach - 1; ++k )
			if( std::hypot( P( i + k ).x - x.x, P( i + k ).y - x.y ) > ( kCornerReach + 0.5 ) * cell )
				near = false;
		if( near && ( corners.empty() || i - corners.back().at > 2 * kCornerReach ) )
			corners.push_back( { i, x } );
	}
	if( corners.empty() )
		return;

	//Rebuild: each corner's inner points (i - reach + 1 .. i + reach - 1)
	//become the one corner point.
	std::vector< uint8_t > drop( static_cast< size_t >( n ), 0 );
	std::vector< int > cornerAt( static_cast< size_t >( n ), -1 );
	for( size_t c = 0; c < corners.size(); ++c )
	{
		for( int k = -kCornerReach + 1; k <= kCornerReach - 1; ++k )
			drop[ static_cast< size_t >( ( ( corners[ c ].at + k ) % n + n ) % n ) ] = 1;
		cornerAt[ static_cast< size_t >( corners[ c ].at ) ] = static_cast< int >( c );
	}
	std::vector< Point > rebuilt;
	rebuilt.reserve( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
	{
		if( cornerAt[ static_cast< size_t >( i ) ] >= 0 )
			rebuilt.push_back( corners[ static_cast< size_t >( cornerAt[ static_cast< size_t >( i ) ] ) ].x );
		else if( !drop[ static_cast< size_t >( i ) ] )
			rebuilt.push_back( loop.points[ static_cast< size_t >( i ) ] );
	}
	if( rebuilt.size() >= 3 )
		loop.points = std::move( rebuilt );
}

//---------------------------------------------------------------------------
void SimplifyLoop( Loop& loop, double tolerance )
{
	const size_t n = loop.points.size();
	if( n < 4 || !( tolerance > 0.0 ) )
		return;

	//Split the ring at point 0 and the point furthest from it, then
	//Douglas-Peucker each half as an open chain.
	size_t far        = 0;
	double farDistance = -1.0;
	for( size_t i = 1; i < n; ++i )
	{
		const double d = distance( loop.points[ 0 ], loop.points[ i ] );
		if( d > farDistance )
		{
			farDistance = d;
			far         = i;
		}
	}

	std::vector< uint8_t > keep( n, 0 );
	keep[ 0 ]   = 1;
	keep[ far ] = 1;

	//Chains as (first, last) index pairs; the second half wraps round to 0,
	//expressed as indices n..n (point n is point 0 again).
	std::vector< std::pair< size_t, size_t > > stack = { { 0, far }, { far, n } };
	auto at = [ & ]( size_t i ) -> const Point& {
		return loop.points[ i % n ];
	};
	while( !stack.empty() )
	{
		const auto [ a, b ] = stack.back();
		stack.pop_back();
		if( b <= a + 1 )
			continue;
		double worst  = -1.0;
		size_t worstI = a;
		for( size_t i = a + 1; i < b; ++i )
		{
			const double d = distanceToSegment( at( i ), at( a ), at( b ) );
			if( d > worst )
			{
				worst  = d;
				worstI = i;
			}
		}
		if( worst > tolerance )
		{
			keep[ worstI % n ] = 1;
			stack.push_back( { a, worstI } );
			stack.push_back( { worstI, b } );
		}
	}

	std::vector< Point > kept;
	kept.reserve( n );
	for( size_t i = 0; i < n; ++i )
		if( keep[ i ] )
			kept.push_back( loop.points[ i ] );
	if( kept.size() >= 3 )
		loop.points = std::move( kept );
}

//---------------------------------------------------------------------------
// Ordering. The loops form a forest: a loop's parent is the loop one level
// out that it lies nearest to -- offset contours of one pocket nest s apart,
// so the nearest loop one level out is its own pocket's. The job is a walk of
// that forest, one pocket (root) at a time, nearest root first: Outside In
// cuts a loop and then its children (pre-order), Inside Out the children and
// then the loop (post-order). Siblings go nearest first. So a pocket is
// finished before the tool moves to the next, as a CAM package does it,
// rather than every pocket's first pass, then every pocket's second.
//---------------------------------------------------------------------------
Path Order( std::vector< Loop > loops, int levelCount, bool insideOut, Point start )
{
	Path path;
	const size_t n = loops.size();

	std::vector< std::vector< size_t > > byLevel( static_cast< size_t >( std::max( levelCount, 0 ) ) );
	for( size_t i = 0; i < n; ++i )
		if( loops[ i ].level >= 0 && loops[ i ].level < levelCount )
			byLevel[ static_cast< size_t >( loops[ i ].level ) ].push_back( i );

	//Parents. Measured from each loop's first point to every vertex of each
	//candidate one level out; with thousands of loops of noise that is too
	//much, and the candidates' first points stand in for their vertices.
	std::vector< std::vector< size_t > > children( n );
	std::vector< size_t > roots;
	for( size_t level = 0; level < byLevel.size(); ++level )
	{
		for( size_t i : byLevel[ level ] )
		{
			if( level == 0 || byLevel[ level - 1 ].empty() )
			{
				roots.push_back( i );
				continue;
			}
			const std::vector< size_t >& outer = byLevel[ level - 1 ];
			size_t vertices                    = 0;
			for( size_t o : outer )
				vertices += loops[ o ].points.size();
			const bool everyVertex = static_cast< double >( vertices ) * static_cast< double >( byLevel[ level ].size() ) < 4e7;

			const Point& p = loops[ i ].points.front();
			size_t best    = outer.front();
			double bestD2  = std::numeric_limits< double >::infinity();
			for( size_t o : outer )
			{
				const std::vector< Point >& pts = loops[ o ].points;
				const size_t count              = everyVertex ? pts.size() : 1;
				for( size_t v = 0; v < count; ++v )
				{
					const double dx = pts[ v ].x - p.x, dy = pts[ v ].y - p.y;
					const double d2 = dx * dx + dy * dy;
					if( d2 < bestD2 )
					{
						bestD2 = d2;
						best   = o;
					}
				}
			}
			children[ best ].push_back( i );
		}
	}

	Point cursor = start;
	auto append  = [ & ]( const Point& p, bool cut ) {
		if( path.points.empty() )
		{
			path.points.push_back( p );
			path.tau.push_back( 0.0 );
			return;
		}
		const double length = distance( path.points.back(), p );
		if( length <= 1e-9 )
			return;
		path.points.push_back( p );
		path.cut.push_back( cut ? 1 : 0 );
		path.tau.push_back( path.tau.back() + ( cut ? length : length / kRapidFactor ) );
		if( cut )
			path.cutLength += length;
	};

	//Cut one loop, entered at its vertex nearest the tool.
	auto cutLoop = [ & ]( size_t i ) {
		const std::vector< Point >& pts = loops[ i ].points;
		size_t entry                    = 0;
		double bestD2                   = std::numeric_limits< double >::infinity();
		for( size_t v = 0; v < pts.size(); ++v )
		{
			const double dx = pts[ v ].x - cursor.x, dy = pts[ v ].y - cursor.y;
			const double d2 = dx * dx + dy * dy;
			if( d2 < bestD2 )
			{
				bestD2 = d2;
				entry  = v;
			}
		}
		append( pts[ entry ], false );//the rapid in (nothing, for the first)
		for( size_t s = 1; s <= pts.size(); ++s )
			append( pts[ ( entry + s ) % pts.size() ], true );
		cursor = pts[ entry ];
		++path.loops;
	};

	//The nearest of a set of loops to the tool, by first point.
	auto takeNearest = [ & ]( std::vector< size_t >& set ) {
		size_t best   = 0;
		double bestD2 = std::numeric_limits< double >::infinity();
		for( size_t q = 0; q < set.size(); ++q )
		{
			const Point& p  = loops[ set[ q ] ].points.front();
			const double d2 = ( p.x - cursor.x ) * ( p.x - cursor.x ) + ( p.y - cursor.y ) * ( p.y - cursor.y );
			if( d2 < bestD2 )
			{
				bestD2 = d2;
				best   = q;
			}
		}
		const size_t chosen = set[ best ];
		set[ best ]         = set.back();
		set.pop_back();
		return chosen;
	};

	//Depth first, iteratively: a stack of (loop, children still to visit).
	struct Frame
	{
		size_t loop;
		std::vector< size_t > pending;
	};
	while( !roots.empty() )
	{
		std::vector< Frame > stack;
		const size_t root = takeNearest( roots );
		if( !insideOut )
			cutLoop( root );
		stack.push_back( { root, children[ root ] } );
		while( !stack.empty() )
		{
			Frame& top = stack.back();
			if( top.pending.empty() )
			{
				if( insideOut )
					cutLoop( top.loop );
				stack.pop_back();
				continue;
			}
			const size_t next = takeNearest( top.pending );
			if( !insideOut )
				cutLoop( next );
			stack.push_back( { next, children[ next ] } );
		}
	}

	return path;
}

//---------------------------------------------------------------------------
namespace
{
/// The segment containing timeline value tau: tau[i] <= tau < tau[i + 1].
size_t segmentAt( const Path& path, double tau )
{
	const auto it = std::upper_bound( path.tau.begin(), path.tau.end(), tau );
	const size_t i = static_cast< size_t >( std::max< std::ptrdiff_t >( 0, ( it - path.tau.begin() ) - 1 ) );
	return std::min( i, path.points.size() - 2 );
}

Point lerpOn( const Path& path, size_t i, double tau )
{
	const double t0 = path.tau[ i ], t1 = path.tau[ i + 1 ];
	const double u  = t1 > t0 ? std::clamp( ( tau - t0 ) / ( t1 - t0 ), 0.0, 1.0 ) : 0.0;
	const Point& a  = path.points[ i ];
	const Point& b  = path.points[ i + 1 ];
	return Point{ a.x + u * ( b.x - a.x ), a.y + u * ( b.y - a.y ) };
}
} // namespace

Point PositionAt( const Path& path, double tau )
{
	if( path.points.empty() )
		return Point{};
	if( path.points.size() == 1 || tau <= 0.0 )
		return path.points.front();
	if( tau >= path.Duration() )
		return path.points.back();
	return lerpOn( path, segmentAt( path, tau ), tau );
}

void CutSpans( const Path& path, double tau0, double tau1, std::vector< Span >& out )
{
	if( path.Empty() || !( tau1 > tau0 ) )
		return;
	tau0 = std::max( tau0, 0.0 );
	tau1 = std::min( tau1, path.Duration() );
	if( !( tau1 > tau0 ) )
		return;

	for( size_t i = segmentAt( path, tau0 ); i + 1 < path.points.size() && path.tau[ i ] < tau1; ++i )
	{
		if( !path.cut[ i ] )
			continue;
		const double a = std::max( tau0, path.tau[ i ] );
		const double b = std::min( tau1, path.tau[ i + 1 ] );
		if( !( b > a ) )
			continue;
		out.push_back( Span{ lerpOn( path, i, a ), lerpOn( path, i, b ) } );
	}
}

void AllSegments( const Path& path, std::vector< Span >& cuts, std::vector< Span >& rapids )
{
	for( size_t i = 0; i + 1 < path.points.size(); ++i )
		( path.cut[ i ] ? cuts : rapids ).push_back( Span{ path.points[ i ], path.points[ i + 1 ] } );
}

} // namespace toolpath::path
