/**
	tptest -- render Toolpath offline, and read the machining back out of it.

	Whether the field is a true distance, whether an inside corner keeps a
	fillet of the tool's radius, whether a slot narrower than the tool is ever
	entered, how wide the scallops between wide passes are, and how fast the
	tool moves are facts with one right answer each. Every GL check renders a
	synthetic picture through the REAL plugin class in a headless GL context
	and measures -- the field against an exact Euclidean distance transform,
	the part out of the Reveal picture -- rather than eyeballing.

		tptest --out /tmp/frame.png     a picture, on the test card
		tptest --list                   every parameter, its kind and default
		tptest --distance               the flooded field against an exact EDT of its lattice
		tptest --fillet                 inside corners keep a fillet of radius r
		tptest --slot                   a slot under 2r is never entered; over, it is cut
		tptest --scallop                ridges of width s - 2r past s = 2r; none below
		tptest --feed                   the tool covers Feed px of path a second
		tptest --latch                  the part survives a resize; Restart clears it
		tptest --lattice                the field is on the working lattice, and nowhere else
		tptest --negative               every GL check above can FAIL
		tptest --offline                the checks that need no GL (what CI runs)
		tptest --bench                  the render cost, and the field's share
		tptest --dump-shaders DIR       the exact GLSL the plugin compiles
		tptest --pipe                   raw frames in, raw frames out

	Every check drives a synthetic 60 fps clock through `SetTime` (the feed
	check also runs at 30). Run each at two rasters at least -- the one you
	develop at and 320x180, which is what CI uses; AGENTS.md has one line per
	check on where each tolerance comes from.

	**Orientation.** The checks work in GL's orientation throughout: row 0 is
	the BOTTOM, pixel (i, j) has its centre at (i + 0.5, j + 0.5), the same
	convention as the plugin's field, path and cut. Their sources are
	uploaded and their pictures read back without a flip. Only `--out` and
	`--pipe`, which exchange pictures with the outside world, flip.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the fleet's format. `--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | tptest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Path.h"
#include "Shaders.h"
#include "Toolpath.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <csignal>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace toolpath;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. `Image` is RGBA float, GL orientation (row 0 at the bottom).
//---------------------------------------------------------------------------
struct Image
{
	int width  = 0;
	int height = 0;
	std::vector< float > rgba;

	Image() = default;
	Image( int w, int h, float grey = 0.0f ) : width( w ), height( h ), rgba( static_cast< size_t >( w ) * h * 4, grey )
	{
		for( size_t i = 3; i < rgba.size(); i += 4 )
			rgba[ i ] = 1.0f;
	}
	float* at( int x, int y )
	{
		return rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	}
	const float* at( int x, int y ) const
	{
		return rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	}
	void set( int x, int y, float r, float g, float b )
	{
		float* p = at( x, y );
		p[ 0 ]   = r;
		p[ 1 ]   = g;
		p[ 2 ]   = b;
		p[ 3 ]   = 1.0f;
	}
	/// Fill pixels [x0, x1) x [y0, y1) with a grey.
	void fill( int x0, int y0, int x1, int y1, float v )
	{
		for( int y = std::max( 0, y0 ); y < std::min( height, y1 ); ++y )
			for( int x = std::max( 0, x0 ); x < std::min( width, x1 ); ++x )
				set( x, y, v, v, v );
	}
};

/// The test card, in GL orientation: dark stock with bright pockets. A big
/// rectangle with a slot off it narrower than the default tool and one
/// wider, an L with an inside corner, a disc, a ring with an island, and a
/// small disc on a slow Lissajous so Live differs from Latch. One shape is
/// orange, not white, so Chroma has something to find.
Image buildCard( int width, int height, int frame )
{
	Image card( width, height, 0.06f );
	const double w = width, h = height;
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / w, v = ( y + 0.5 ) / h;
			const double px = u * w / h, py = v;//in frame heights
			bool in         = false;
			bool orange     = false;

			//The rectangle, and its two slots to the right.
			if( u > 0.04 && u < 0.34 && v > 0.52 && v < 0.94 )
				in = true;
			if( u >= 0.34 && u < 0.48 && std::fabs( v - 0.83 ) < 0.015 )
				in = true;//3% of the height: under the default 4% tool
			if( u >= 0.34 && u < 0.48 && std::fabs( v - 0.63 ) < 0.04 )
				in = true;//8%: over it

			//An L.
			if( u > 0.06 && u < 0.30 && v > 0.08 && v < 0.24 )
				in = true;
			if( u > 0.06 && u < 0.14 && v > 0.08 && v < 0.44 )
				in = true;

			//A disc, and a ring with an island.
			const double dx = px - 0.62 * w / h, dy = py - 0.70;
			if( dx * dx + dy * dy < 0.2 * 0.2 )
			{
				in     = true;
				orange = true;
			}
			const double rx = px - 0.86 * w / h, ry = py - 0.28;
			const double rr = std::sqrt( rx * rx + ry * ry );
			if( ( rr < 0.2 && rr > 0.11 ) || rr < 0.05 )
				in = true;

			//The mover.
			const double mx = px - ( 0.45 + 0.08 * std::sin( frame * 0.05 ) ) * w / h;
			const double my = py - ( 0.28 + 0.06 * std::sin( frame * 0.037 + 1.1 ) );
			if( mx * mx + my * my < 0.07 * 0.07 )
				in = true;

			if( in )
			{
				if( orange )
					card.set( x, y, 0.98f, 0.62f, 0.18f );
				else
					card.set( x, y, 0.92f, 0.92f, 0.90f );
			}
		}
	}
	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Toolpath::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_RED:
	case FF_TYPE_GREEN:
	case FF_TYPE_BLUE: return "colour";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Toolpath& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Toolpath::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Toolpath& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Toolpath& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Toolpath& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Toolpath plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// The physics checks read float so their tolerances can be derived.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		process.HostFBO                                 = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width                       = w;
		height                      = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the composition changes size: hand the SAME
	/// instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderAt( int frame )
	{
		//A synthetic clock, and it has to be: left to the wall clock the
		//harness renders a hundred frames in milliseconds and the tool never
		//moves. The unit is declared, not inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	void upload( const Image& image )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, image.rgba.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// 8-bit pixels, top row first (the outside world's orientation).
	void uploadTopFirst( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// The output, top row first, 8-bit.
	std::vector< unsigned char > readBackTopFirst()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// The output as floats, GL orientation.
	Image readBack()
	{
		Image out;
		out.width  = width;
		out.height = height;
		out.rgba.resize( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 4 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, out.rgba.data() );
		return out;
	}

	void press( const char* name )
	{
		const int index = indexOfParameter( plugin, name );
		if( index >= 0 )
		{
			plugin.SetFloatParameter( static_cast< unsigned int >( index ), 1.0f );
			plugin.SetFloatParameter( static_cast< unsigned int >( index ), 0.0f );
		}
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

/// One ULP of a float at magnitude m.
double ulp( double m )
{
	return std::ldexp( 1.0, std::ilogb( std::max( std::fabs( m ), 1e-30 ) ) - 23 );
}

//---------------------------------------------------------------------------
// The baseline every GL check starts from: the region is the white of a
// white-on-black source (Luma, threshold 0.5, no smoothing, not inverted);
// Reveal over BLACK stock with no wall shading and no tool drawn, so the
// output's red channel IS the cut coverage wherever the source is white.
// Each check then moves what it is about.
//---------------------------------------------------------------------------
void baseline( Toolpath& p )
{
	set( p, "Detect On", 0.0f );
	set( p, "Threshold", 0.5f );
	set( p, "Invert", 0.0f );
	set( p, "Smooth", 0.0f );
	set( p, "Tool Diameter", controls::ToolDiameterParam( 0.1f ) );
	set( p, "Stepover", controls::StepoverParam( 0.45f ) );
	set( p, "Strategy", 0.0f );
	set( p, "Feed", controls::FeedParam( 8.0f ) );
	set( p, "Geometry", 0.0f );
	set( p, "Mode", 0.0f );
	set( p, "Stock Colour", 0.0f );
	set( p, "Stock_Green", 0.0f );
	set( p, "Stock_Blue", 0.0f );
	set( p, "Depth", 0.0f );
	set( p, "Show Tool", 0.0f );
	set( p, "Mix", 1.0f );
}

using Settings = std::vector< std::pair< const char*, float > >;

/// Run a Latch job on `source` to completion (the tool at the end of its
/// path), or for `frames` frames if that is positive, and read the picture.
/// Returns false on a GL failure.
bool machine( int width, int height, const Settings& settings, const Image& source, int perturb, Image& out,
              int frames = 0, path::Path* pathOut = nullptr, double* radiusOut = nullptr )
{
	Session session;
	baseline( session.plugin );
	for( const auto& s : settings )
		if( !set( session.plugin, s.first, s.second ) )
			return false;
	session.plugin.SetPerturbForTest( perturb );
	if( !session.begin( width, height ) )
		return false;
	session.upload( source );

	const int limit = frames > 0 ? frames : 20000;
	for( int frame = 0; frame < limit; ++frame )
	{
		if( !session.renderAt( frame ) )
		{
			session.end();
			return false;
		}
		if( frames <= 0 && frame > 1 && session.plugin.TauForTest() >= session.plugin.PathForTest().Duration() )
			break;
	}
	out = session.readBack();
	if( pathOut )
		*pathOut = session.plugin.PathForTest();
	if( radiusOut )
		*radiusOut = 0.5 * controls::ToolDiameterHeights( session.plugin.GetFloatParameter( Toolpath::PT_TOOL_DIAMETER ) )
		             * height;
	session.end();
	return true;
}

/// The cut coverage out of a baseline picture: the red channel.
float coverage( const Image& picture, int x, int y )
{
	return picture.at( x, y )[ 0 ];
}

/// The coverage between pixel centres, bilinear, at (x, y) in pixels.
double sampleCoverage( const Image& picture, double x, double y )
{
	const double fx = x - 0.5, fy = y - 0.5;
	const int x0 = static_cast< int >( std::floor( fx ) ), y0 = static_cast< int >( std::floor( fy ) );
	const double u = fx - x0, v = fy - y0;
	auto at = [ & ]( int i, int j ) {
		i = std::clamp( i, 0, picture.width - 1 );
		j = std::clamp( j, 0, picture.height - 1 );
		return static_cast< double >( coverage( picture, i, j ) );
	};
	return ( 1 - v ) * ( ( 1 - u ) * at( x0, y0 ) + u * at( x0 + 1, y0 ) ) + v * ( ( 1 - u ) * at( x0, y0 + 1 ) + u * at( x0 + 1, y0 + 1 ) );
}

//---------------------------------------------------------------------------
// The exact Euclidean distance transform: Felzenszwalb & Huttenlocher's
// lower envelope of parabolas, one dimension at a time. Exact in integers --
// squared distances between pixel centres -- and a different algorithm from
// the flood entirely, which is the point. `--offline` checks it against
// brute force.
//---------------------------------------------------------------------------
constexpr double kInf = 1e20;

void edt1d( const std::vector< double >& f, std::vector< double >& d, int n )
{
	std::vector< int > v( static_cast< size_t >( n ) );
	std::vector< double > z( static_cast< size_t >( n ) + 1 );
	int k   = 0;
	v[ 0 ]  = 0;
	z[ 0 ]  = -kInf;
	z[ 1 ]  = kInf;
	int first = -1;
	for( int q = 0; q < n; ++q )
		if( f[ static_cast< size_t >( q ) ] < kInf )
		{
			first = q;
			break;
		}
	if( first < 0 )
	{
		for( int q = 0; q < n; ++q )
			d[ static_cast< size_t >( q ) ] = kInf;
		return;
	}
	v[ 0 ] = first;
	for( int q = first + 1; q < n; ++q )
	{
		if( !( f[ static_cast< size_t >( q ) ] < kInf ) )
			continue;
		double s;
		while( true )
		{
			const int p = v[ static_cast< size_t >( k ) ];
			s = ( ( f[ static_cast< size_t >( q ) ] + double( q ) * q ) - ( f[ static_cast< size_t >( p ) ] + double( p ) * p ) )
			    / ( 2.0 * q - 2.0 * p );
			if( s <= z[ static_cast< size_t >( k ) ] && k > 0 )
				--k;
			else
				break;
		}
		++k;
		v[ static_cast< size_t >( k ) ]     = q;
		z[ static_cast< size_t >( k ) ]     = s;
		z[ static_cast< size_t >( k ) + 1 ] = kInf;
	}
	k = 0;
	for( int q = 0; q < n; ++q )
	{
		while( z[ static_cast< size_t >( k ) + 1 ] < q )
			++k;
		const int p                     = v[ static_cast< size_t >( k ) ];
		d[ static_cast< size_t >( q ) ] = double( q - p ) * ( q - p ) + f[ static_cast< size_t >( p ) ];
	}
}

/// Squared distance from every pixel centre to the nearest pixel centre
/// where `seed` is true. kInf where there is none.
std::vector< double > squaredEdt( const std::vector< uint8_t >& seed, int width, int height, bool cityBlock = false )
{
	std::vector< double > grid( static_cast< size_t >( width ) * height );
	for( size_t i = 0; i < grid.size(); ++i )
		grid[ i ] = seed[ i ] ? 0.0 : kInf;

	if( cityBlock )
	{
		//The negative control for the reference: a distance that is not
		//Euclidean. Two-pass chamfer with unit steps.
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				double& g = grid[ static_cast< size_t >( y ) * width + x ];
				if( x > 0 )
					g = std::min( g, grid[ static_cast< size_t >( y ) * width + x - 1 ] + 1.0 );
				if( y > 0 )
					g = std::min( g, grid[ static_cast< size_t >( y - 1 ) * width + x ] + 1.0 );
			}
		for( int y = height - 1; y >= 0; --y )
			for( int x = width - 1; x >= 0; --x )
			{
				double& g = grid[ static_cast< size_t >( y ) * width + x ];
				if( x + 1 < width )
					g = std::min( g, grid[ static_cast< size_t >( y ) * width + x + 1 ] + 1.0 );
				if( y + 1 < height )
					g = std::min( g, grid[ static_cast< size_t >( y + 1 ) * width + x ] + 1.0 );
			}
		for( double& g : grid )
			g = g < kInf ? g * g : kInf;
		return grid;
	}

	std::vector< double > f( static_cast< size_t >( std::max( width, height ) ) ), d( f.size() );
	for( int x = 0; x < width; ++x )
	{
		for( int y = 0; y < height; ++y )
			f[ static_cast< size_t >( y ) ] = grid[ static_cast< size_t >( y ) * width + x ];
		edt1d( f, d, height );
		for( int y = 0; y < height; ++y )
			grid[ static_cast< size_t >( y ) * width + x ] = d[ static_cast< size_t >( y ) ];
	}
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
			f[ static_cast< size_t >( x ) ] = grid[ static_cast< size_t >( y ) * width + x ];
		edt1d( f, d, width );
		for( int x = 0; x < width; ++x )
			grid[ static_cast< size_t >( y ) * width + x ] = d[ static_cast< size_t >( x ) ] >= kInf * 0.5 ? kInf : d[ static_cast< size_t >( x ) ];
	}
	return grid;
}

/// The field the plugin claims to compute, from the mask, exactly: inside,
/// the distance to the nearest outside centre, less half a texel, or to the
/// frame's edge; outside, minus the distance to the nearest inside centre,
/// less half a texel. In JOB pixels: `inside` is on a lattice `scale` job
/// pixels a texel, and the frame is the job raster's, jobWidth x jobHeight
/// (by default the lattice's own). With scale 1 it is the plain EDT field.
std::vector< double > exactField( const std::vector< uint8_t >& inside, int width, int height, bool cityBlock = false,
                                  int scale = 1, int jobWidth = 0, int jobHeight = 0 )
{
	if( jobWidth <= 0 )
		jobWidth = width * scale;
	if( jobHeight <= 0 )
		jobHeight = height * scale;
	const double k = scale;
	std::vector< uint8_t > outside( inside.size() );
	for( size_t i = 0; i < inside.size(); ++i )
		outside[ i ] = inside[ i ] ? 0 : 1;
	const std::vector< double > toOutside = squaredEdt( outside, width, height, cityBlock );
	const std::vector< double > toInside  = squaredEdt( inside, width, height, cityBlock );

	std::vector< double > field( inside.size() );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const size_t i = static_cast< size_t >( y ) * width + x;
			if( inside[ i ] )
			{
				const double cx   = k * ( x + 0.5 ), cy = k * ( y + 0.5 );
				const double wall = std::min( std::min( cx, jobWidth - cx ), std::min( cy, jobHeight - cy ) );
				const double d    = toOutside[ i ] < kInf ? k * ( std::sqrt( toOutside[ i ] ) - 0.5 ) : kInf;
				field[ i ]        = std::min( d, wall );
			}
			else
				field[ i ] = toInside[ i ] < kInf ? k * ( 0.5 - std::sqrt( toInside[ i ] ) ) : -1.0e6;
		}
	return field;
}

//---------------------------------------------------------------------------
// The working lattice. The plugin decides the region, and floods and
// resolves the field, on texels Toolpath::kFieldScale job pixels a side; the
// checks derive what that does rather than looking away from it.
//---------------------------------------------------------------------------
constexpr int kLattice = Toolpath::kFieldScale;

int latticeSide( int pixels, int k = kLattice )
{
	return ( pixels + k - 1 ) / k;
}

/// The working lattice's mask from a job-raster mask, by the plugin's rule:
/// a texel is inside when its block's mean is over the threshold -- for a
/// clean 0/1 mask at 0.5, when MORE than half its k x k block is inside --
/// with the block clamped at the frame's edge, as the detect pass's
/// texelFetch is.
std::vector< uint8_t > reduceMask( const std::vector< uint8_t >& mask, int width, int height, int k = kLattice )
{
	const int fw = latticeSide( width, k ), fh = latticeSide( height, k );
	std::vector< uint8_t > out( static_cast< size_t >( fw ) * fh );
	for( int j = 0; j < fh; ++j )
		for( int i = 0; i < fw; ++i )
		{
			int count = 0;
			for( int b = 0; b < k; ++b )
				for( int a = 0; a < k; ++a )
				{
					const int x = std::min( k * i + a, width - 1 ), y = std::min( k * j + b, height - 1 );
					count += mask[ static_cast< size_t >( y ) * width + x ] ? 1 : 0;
				}
			out[ static_cast< size_t >( j ) * fw + i ] = 2 * count > k * k ? 1 : 0;
		}
	return out;
}

/// Where the working lattice puts a straight wall the job raster has at
/// pixel edge e (the region at >= e if `insideAbove`, else at < e): on a
/// lattice line, the one the majority rule picks. A wall half way across a
/// block of two goes to the side that is NOT inside -- the region shrinks
/// onto the lattice, by at most k/2 px, and never grows.
int latticeEdge( int e, bool insideAbove, int k = kLattice )
{
	const int b = e / k, rem = e - b * k;
	if( rem == 0 )
		return e;
	const int insideCount = insideAbove ? k - rem : rem;
	const bool blockInside = 2 * insideCount > k;
	return insideAbove ? ( blockInside ? b * k : ( b + 1 ) * k ) : ( blockInside ? ( b + 1 ) * k : b * k );
}

//---------------------------------------------------------------------------
// --distance
//
// The flooded field against the exact EDT of the same mask, texel for
// texel, on shapes whose answer the reference computes exactly. The mask the
// plugin floods is the WORKING LATTICE's -- the job-raster mask reduced by
// the plugin's majority rule (reduceMask), k = kFieldScale job pixels a
// texel -- and the field is in job pixels, k x the lattice's own distance.
// So every bound below is the full-raster argument made on the lattice, and
// expressed in job pixels:
//
//   square, frame   rectilinear. Every nearest centre is along a row or a
//                   column, the flood cannot miss it, and the field must be
//                   EXACT to the GPU's sqrt: 6 ULP of the distance (GLSL
//                   4.10 section 4.7.1: sqrt is inherited from 1/inversesqrt,
//                   2 + 2.5 ULP, and the half-texel subtraction rounds once;
//                   the scaling by k = 2 is exact, so it is 6 ULP of the
//                   texel distance, times k).
//   disc, ring,     curved. JFA has no error bound in general; its known
//   star, blobs     failure on a digitised boundary is the thin Voronoi wedge
//                   along the medial axis, where a texel is left holding the
//                   seed of a NEIGHBOURING cell. Neighbouring seeds on a
//                   digitised boundary are 8-neighbours, at most sqrt2 texels
//                   apart, so by the triangle inequality the distance is at
//                   most SQRT2 TEXELS -- sqrt2 k job pixels -- too long. That
//                   is the bound, and it is only true if the flood's
//                   surviving errors are of that kind: with only the 2, 1
//                   finish they were not (a 4K disc 2.4 px out, a star 3.3,
//                   when the flood was at the full raster), which is why the
//                   finish grows with the lattice.
//   constellation   the known bad case: four one-texel islands, sparse seeds,
//                   Rong & Tan's configuration, where the competitor can be
//                   any seed at all. Without the 1+ prepass the flood misses
//                   by 5.7 texels on a 320x180 lattice and 23 on 1280x720,
//                   finish and all; the prepass is what repairs it, to the
//                   same sqrt2 (in fact exactly). It is drawn and flooded at
//                   its own LATTICE, 320x180 x 2^j texels -- the largest that
//                   fits, or the smallest there is -- because the
//                   configuration and its failure are properties of that
//                   lattice: at 333x187 plain JFA gets it right. Each island
//                   is a k x k block of job pixels, so it survives the
//                   reduction as one texel; at a requested 320x180 the job
//                   raster is 640x360, the first whose lattice is 320x180.
//
// Before any of that, the field must BE on the working lattice:
// ceil( raster / k ) texels. A field at any other size fails outright.
//
// Reported with and without the corrections. The negative control skips the
// prepass and the constellation must break the bound.
//---------------------------------------------------------------------------
struct Shape
{
	const char* name;
	bool exact;//rectilinear: to the ULP
	std::function< bool( double x, double y ) > inside;//pixel centre, GL
};

/// The largest power of two j whose LATTICE of 320j x 180j texels fits the
/// raster (at least 1): the constellation is drawn on the job raster
/// kLattice times that and flooded on that lattice. The flood's jump
/// sequence scales by j exactly with the lattice, so the configuration and
/// its failure are the same on every such lattice -- and only there.
int constellationScale( int width, int height )
{
	int j = 1;
	while( 320 * j * 2 * kLattice <= width && 180 * j * 2 * kLattice <= height )
		j *= 2;
	return j;
}

/// The four islands, in texels of a 320x180 lattice, (x, y) with y up. Found
/// by searching random sparse constellations (a numpy model of this flood,
/// in the session that wrote it) for one that the flood without its 1+
/// prepass gets badly wrong at both 320x180 and 1280x720, and the full flood
/// gets exactly right. It is a fixture, not a tolerance: the plugin's own
/// shaders are what the check runs on it.
const int kConstellation[ 4 ][ 2 ] = {
	{ 133, 84 }, { 306, 123 }, { 285, 73 }, { 85, 164 },
};

std::vector< Shape > distanceShapes( int width, int height )
{
	const double w = width, h = height;
	const double cx = 0.5 * w, cy = 0.5 * h;
	const int k    = constellationScale( width, height );

	//Blobs: a fixed field of Gaussian bumps, thresholded. Placed by an
	//integer hash so the shape is the same at every raster, scaled.
	auto hash = []( uint32_t v ) {
		v = v * 747796405u + 2891336453u;
		v = ( ( v >> ( ( v >> 28u ) + 4u ) ) ^ v ) * 277803737u;
		return ( ( v >> 22u ) ^ v ) / 4294967296.0;
	};
	struct Bump
	{
		double x, y, s, a;
	};
	auto bumps = std::make_shared< std::vector< Bump > >();
	for( uint32_t i = 0; i < 24; ++i )
		bumps->push_back( { hash( 4 * i ) * w, hash( 4 * i + 1 ) * h, ( 0.04 + 0.08 * hash( 4 * i + 2 ) ) * h,
		                    hash( 4 * i + 3 ) < 0.3 ? -0.8 : 1.0 } );

	return {
		{ "square", true, [ = ]( double x, double y ) { return std::fabs( x - cx ) < 0.35 * h && std::fabs( y - cy ) < 0.35 * h; } },
		{ "frame", true, []( double, double ) { return true; } },
		{ "disc", false, [ = ]( double x, double y ) { return std::hypot( x - cx, y - cy ) < 0.35 * h; } },
		{ "ring", false, [ = ]( double x, double y ) {
			 const double r = std::hypot( x - cx, y - cy );
			 return r < 0.4 * h && r > 0.2 * h;
		 } },
		{ "star", false, [ = ]( double x, double y ) {
			 return std::hypot( x - cx, y - cy ) < h * ( 0.25 + 0.12 * std::cos( 5.0 * std::atan2( y - cy, x - cx ) ) );
		 } },
		{ "blobs", false, [ = ]( double x, double y ) {
			 double sum = 0.0;
			 for( const Bump& b : *bumps )
				 sum += b.a * std::exp( -0.5 * ( ( x - b.x ) * ( x - b.x ) + ( y - b.y ) * ( y - b.y ) ) / ( b.s * b.s ) );
			 return sum > 0.5;
		 } },
		{ "constellation", false, [ = ]( double x, double y ) {
			 //One texel of the lattice each: a kLattice-square block of pixels.
			 const int px = static_cast< int >( x ) / kLattice, py = static_cast< int >( y ) / kLattice;
			 for( const auto& p : kConstellation )
				 if( px == p[ 0 ] * k && py == p[ 1 ] * k )
					 return true;
			 return false;
		 } },
	};
}

struct FieldError
{
	double max    = 0.0;
	double rms    = 0.0;
	int overBound = 0;
	bool ok       = false;
	bool lattice  = true;///< the field was on the working lattice at all
	int fieldWidth = 0, fieldHeight = 0;
};

/// Render the field of the job-raster `mask` with the given perturbation and
/// compare it, texel for texel, with the exact field of the working
/// lattice's mask (`exact`, fw x fh, in job pixels).
bool fieldError( int width, int height, const std::vector< uint8_t >& mask, const std::vector< double >& exact, bool exactShape,
                 int perturb, FieldError& result )
{
	Image source( width, height, 0.0f );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			if( mask[ static_cast< size_t >( y ) * width + x ] )
				source.set( x, y, 1.0f, 1.0f, 1.0f );

	Session session;
	baseline( session.plugin );
	set( session.plugin, "Mode", static_cast< float >( controls::kField ) );
	session.plugin.SetPerturbForTest( perturb );
	if( !session.begin( width, height ) )
		return false;
	session.upload( source );
	if( !session.renderAt( 0 ) )
	{
		session.end();
		return false;
	}
	std::vector< float > field;
	int fw = 0, fh = 0;
	session.plugin.ReadFieldForTest( field, fw, fh );
	session.end();
	result             = FieldError{};
	result.fieldWidth  = fw;
	result.fieldHeight = fh;
	if( fw != latticeSide( width ) || fh != latticeSide( height ) )
	{
		//Not on the working lattice: nothing to compare texel for texel.
		result.lattice = false;
		result.max     = 1.0e9;
		return true;
	}

	double sum2 = 0.0;
	size_t n    = 0;
	const double k = kLattice;
	for( size_t i = 0; i < exact.size(); ++i )
	{
		if( exact[ i ] <= -1.0e5 )
			continue;//no region at all: nothing to be a distance to
		const double e = std::fabs( static_cast< double >( field[ i ] ) - exact[ i ] );
		//6 ULP of the texel distance before the half-texel came off, times
		//k (exact); or sqrt2 texels, in job pixels.
		const double bound = exactShape ? 6.0 * k * ulp( std::max( std::fabs( exact[ i ] ) / k + 0.5, 1.0 ) ) : std::sqrt( 2.0 ) * k;
		if( e > bound )
			++result.overBound;
		result.max = std::max( result.max, e );
		sum2 += e * e;
		++n;
	}
	result.rms = n ? std::sqrt( sum2 / n ) : 0.0;
	result.ok  = result.overBound == 0;
	return true;
}

int runDistance( int width, int height, int perturb, bool quiet = false )
{
	int failures = 0;
	const int fullWidth = width, fullHeight = height;
	for( const Shape& shape : distanceShapes( fullWidth, fullHeight ) )
	{
		width  = fullWidth;
		height = fullHeight;
		if( std::strcmp( shape.name, "constellation" ) == 0 )
		{
			const int j = constellationScale( fullWidth, fullHeight );
			width       = 320 * j * kLattice;
			height      = 180 * j * kLattice;
		}
		std::vector< uint8_t > mask( static_cast< size_t >( width ) * height );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
				mask[ static_cast< size_t >( y ) * width + x ] = shape.inside( x + 0.5, y + 0.5 ) ? 1 : 0;
		const int fw = latticeSide( width ), fh = latticeSide( height );
		const std::vector< double > exact = exactField( reduceMask( mask, width, height ), fw, fh, false, kLattice, width, height );

		FieldError full, noPrepass, plain;
		if( !fieldError( width, height, mask, exact, shape.exact, perturb, full ) )
		{
			std::printf( "distance %s: render failed  FAILED\n", shape.name );
			++failures;
			continue;
		}
		if( !full.ok )
			++failures;
		if( quiet )
			continue;
		if( !full.lattice )
		{
			std::printf( "distance %-13s the field is %dx%d, not the working lattice's %dx%d  %s\n", shape.name, full.fieldWidth,
			             full.fieldHeight, fw, fh, verdict( false ) );
			continue;
		}

		fieldError( width, height, mask, exact, shape.exact, perturb | Toolpath::kPerturbNoPrepass, noPrepass );
		fieldError( width, height, mask, exact, shape.exact,
		            perturb | Toolpath::kPerturbNoPrepass | Toolpath::kPerturbNoFinish, plain );
		char bound[ 48 ];
		if( shape.exact )
			std::snprintf( bound, sizeof( bound ), "6 ULP" );
		else
			std::snprintf( bound, sizeof( bound ), "sqrt2 texels = %.2f px", std::sqrt( 2.0 ) * kLattice );
		std::printf( "distance %-13s max %.4f px (%.4f texels) rms %.2e  | no prepass max %.4f px | plain JFA max %.4f px"
		             "  bound %s  %s\n",
		             shape.name, full.max, full.max / kLattice, full.rms, noPrepass.max, plain.max, bound, verdict( full.ok ) );
	}
	if( !quiet )
		std::printf( "distance: %s\n", failures == 0 ? "the flooded field is the exact EDT of the working lattice within its "
		                                               "bound on every shape"
		                                             : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// Where a row or column of coverage crosses 0.5, between two pixel centres.
//---------------------------------------------------------------------------
double crossing( double c0, double c1, double p0, double p1 )
{
	const double t = ( 0.5 - c0 ) / ( c1 - c0 );
	return p0 + t * ( p1 - p0 );
}

/// Kasa's algebraic circle fit: least squares on x^2 + y^2 + D x + E y + F = 0.
bool fitCircle( const std::vector< path::Point >& points, double& cx, double& cy, double& r )
{
	if( points.size() < 5 )
		return false;
	//Normal equations for (D, E, F), centred for conditioning.
	double mx = 0, my = 0;
	for( const auto& p : points )
	{
		mx += p.x;
		my += p.y;
	}
	mx /= points.size();
	my /= points.size();
	double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sz = 0, sxz = 0, syz = 0, n = 0;
	for( const auto& p : points )
	{
		const double x = p.x - mx, y = p.y - my, z = x * x + y * y;
		sxx += x * x;
		sxy += x * y;
		syy += y * y;
		sx += x;
		sy += y;
		sz += z;
		sxz += x * z;
		syz += y * z;
		n += 1;
	}
	//[ sxx sxy sx ] [D]   [ -sxz ]
	//[ sxy syy sy ] [E] = [ -syz ]
	//[ sx  sy  n  ] [F]   [ -sz  ]
	const double a[ 3 ][ 4 ] = { { sxx, sxy, sx, -sxz }, { sxy, syy, sy, -syz }, { sx, sy, n, -sz } };
	double m[ 3 ][ 4 ];
	std::memcpy( m, a, sizeof( m ) );
	for( int c = 0; c < 3; ++c )
	{
		int pivot = c;
		for( int rr = c + 1; rr < 3; ++rr )
			if( std::fabs( m[ rr ][ c ] ) > std::fabs( m[ pivot ][ c ] ) )
				pivot = rr;
		if( std::fabs( m[ pivot ][ c ] ) < 1e-12 )
			return false;
		for( int k = 0; k < 4; ++k )
			std::swap( m[ c ][ k ], m[ pivot ][ k ] );
		for( int rr = 0; rr < 3; ++rr )
		{
			if( rr == c )
				continue;
			const double f = m[ rr ][ c ] / m[ c ][ c ];
			for( int k = 0; k < 4; ++k )
				m[ rr ][ k ] -= f * m[ c ][ k ];
		}
	}
	const double D = m[ 0 ][ 3 ] / m[ 0 ][ 0 ], E = m[ 1 ][ 3 ] / m[ 1 ][ 1 ], F = m[ 2 ][ 3 ] / m[ 2 ][ 2 ];
	cx = -0.5 * D + mx;
	cy = -0.5 * E + my;
	r  = std::sqrt( std::max( 0.25 * ( D * D + E * E ) - F, 0.0 ) );
	return true;
}

//---------------------------------------------------------------------------
// --fillet
//
// A square pocket, machined to the end. In each inside corner the stock the
// round tool could not reach is bounded by a quarter circle of the tool's
// radius, centred r in from both walls. The boundary is read out of the
// Reveal picture -- the 0.5 crossing of the cut coverage along 61 rays from
// the corner, 15 to 75 degrees -- and a circle tangent to both walls fitted.
//
// The walls are the POCKET's: where the working lattice put the source's
// (latticeEdge) -- at 320x180 the square's edges are odd and each wall is a
// pixel inside the source's; at 1280x720 they are even and it is not moved.
// That the region lands there is --lattice's claim, not this one's.
//
// The spec's tolerance is one pixel on the radius. What the fit can be off
// by, derived: the field of a square is exact on the lattice (--distance)
// and linear along each wall, so its bilinear samples on the trace grid are
// exact there (at a quarter texel, weights 1/4 and 3/4, exact even in a
// filter's 8 bits); the traced corner is cut where the field's ridge crosses
// one texel, by at most a quarter of the TEXEL's diagonal (0.71 px at 2 px a
// texel -- the bilinear corner of min( x, y ) sits 0.59 px in); Douglas-
// Peucker moves the path 0.2 px at most; each crossing read off a one-pixel
// ramp is within 0.09 px. 1.00 px in the worst case (0.997), at the pixel.
//
// The negative control cuts with a square tool, which reaches into the
// corner and leaves no arc to find.
//---------------------------------------------------------------------------
int runFillet( int width, int height, int perturb, bool quiet = false )
{
	const int side = static_cast< int >( std::lround( 0.7 * height ) );
	const int sx0 = ( width - side ) / 2, sy0 = ( height - side ) / 2;
	const int sx1 = sx0 + side, sy1 = sy0 + side;
	Image source( width, height, 0.0f );
	source.fill( sx0, sy0, sx1, sy1, 1.0f );
	//The pocket's walls, on the working lattice.
	const int x0 = latticeEdge( sx0, true ), x1 = latticeEdge( sx1, false );
	const int y0 = latticeEdge( sy0, true ), y1 = latticeEdge( sy1, false );

	Image picture;
	double r = 0.0;
	if( !machine( width, height, {}, source, perturb, picture, 0, nullptr, &r ) )
	{
		std::printf( "fillet: render failed  FAILED\n" );
		return 1;
	}

	int failures = 0;
	//Each corner as (wall x, wall y, inward x, inward y).
	const struct
	{
		const char* name;
		double wx, wy;
		int sx, sy;
	} corners[] = {
		{ "bottom left ", double( x0 ), double( y0 ), 1, 1 },
		{ "bottom right", double( x1 ), double( y0 ), -1, 1 },
		{ "top left    ", double( x0 ), double( y1 ), 1, -1 },
		{ "top right   ", double( x1 ), double( y1 ), -1, -1 },
	};
	for( const auto& c : corners )
	{
		const double ex = c.wx + c.sx * r, ey = c.wy + c.sy * r;//the expected centre
		std::vector< path::Point > boundary;

		//Rays from the wall corner into the pocket, one a degree from 15 to
		//75: each crosses the fillet once, from uncut to cut, and the 0.5
		//crossing of the coverage (bilinear between pixel centres, stepped
		//at 1/64 px, the last step refined linearly) is a boundary point.
		//Past 15 degrees from a wall the crossing is at least 0.13 r from
		//it, so no tap reaches the black outside the pocket at r >= 8.
		for( int degree = 15; degree <= 75; ++degree )
		{
			const double a  = degree * 3.14159265358979 / 180.0;
			const double dx = c.sx * std::cos( a ), dy = c.sy * std::sin( a );
			double previous = -1.0;
			for( double t = 0.25; t < 2.0 * r; t += 1.0 / 64.0 )
			{
				const double value = sampleCoverage( picture, c.wx + t * dx, c.wy + t * dy );
				if( previous >= 0.0 && previous < 0.5 && value >= 0.5 )
				{
					const double tt = t - ( 1.0 / 64.0 ) * ( value - 0.5 ) / ( value - previous );
					boundary.push_back( { c.wx + tt * dx, c.wy + tt * dy } );
					break;
				}
				previous = value;
			}
		}

		//A fillet is a circle tangent to both walls: its centre is R in from
		//each, and R is the one thing to fit. (A free circle fitted to a
		//quarter arc read off pixels is ill-conditioned -- a bigger radius
		//and a centre further out fit nearly as well -- and here it traded a
		//0.05 px wiggle for a 1.2 px error in the radius.) Least squares in
		//R, by golden section: the residual is smooth and single-minimum in
		//R across [r/4, 3r].
		auto residual = [ & ]( double R ) {
			double sum = 0.0;
			for( const auto& q : boundary )
			{
				const double e = std::hypot( q.x - ( c.wx + c.sx * R ), q.y - ( c.wy + c.sy * R ) ) - R;
				sum += e * e;
			}
			return sum;
		};
		double lo = 0.25 * r, hi = 3.0 * r;
		const double golden = 0.5 * ( std::sqrt( 5.0 ) - 1.0 );
		for( int it = 0; it < 200; ++it )
		{
			const double a = hi - golden * ( hi - lo ), b = lo + golden * ( hi - lo );
			if( residual( a ) < residual( b ) )
				hi = b;
			else
				lo = a;
		}
		const double fr = 0.5 * ( lo + hi );
		double worst    = 0.0;
		for( const auto& q : boundary )
			worst = std::max( worst, std::fabs( std::hypot( q.x - ( c.wx + c.sx * fr ), q.y - ( c.wy + c.sy * fr ) ) - fr ) );

		//Every one of the 61 rays must find a crossing, or there is no
		//fillet there to fit; every point must lie on the fitted circle
		//within half a texel, or it is not a circle.
		const bool found = boundary.size() == 61;
		const bool ok    = found && std::fabs( fr - r ) <= 1.0 && worst <= 0.5;
		if( !ok )
			++failures;
		if( !quiet )
		{
			if( found )
				std::printf( "fillet %s: radius %.3f against r = %.3f (|error| %.3f, tolerance 1 px); every point within "
				             "%.3f px of that circle (tolerance 0.5)  %s\n",
				             c.name, fr, r, std::fabs( fr - r ), worst, verdict( ok ) );
			else
				std::printf( "fillet %s: no arc of uncut stock -- %zu of 61 rays crossed one  %s\n", c.name, boundary.size(),
				             verdict( ok ) );
		}
	}
	if( !quiet )
		std::printf( "fillet: %s\n", failures == 0 ? "every inside corner keeps a fillet of the tool's radius" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --slot
//
// A pocket with a slot off its right wall, at two widths either side of the
// tool's diameter, machined to the end. The field is on the working lattice,
// k = kFieldScale px a texel, and the slot's walls land on it by the
// majority rule: each moves INWARD by up to k/2 px, never out.
//
//   w < 2r   the working slot is no wider than w, the field inside it never
//            exceeds w/2 < r, and the sampled field never exceeds the true
//            one, so no pass is traced in it. A trace sample can read at
//            least r only if a texel it interpolates does, and every such
//            texel is left of the mouth, so every sample at or past
//            mouth + k/2 reads under r and a crossing is interpolated at
//            most one trace cell further. Past mouth + k/2 + cell + r + 1 px
//            the cut coverage must be EXACTLY zero (the stamp's ramp ends at
//            r + 0.5). w is 2r - 2, as it was: the lattice only narrows it.
//   w > 2r   the slot's own pass runs down its middle. Its centre row must
//            be cut (coverage >= 0.5) from the mouth to within a pixel of
//            the working end wall. The sampled peak can be low by k/2 for
//            the walls (together they move in by up to k, so the middle by
//            k/2) and by k/2 more for the texel centres nearest the middle
//            missing it, so w/2 - k must exceed r: w is the least integer
//            over 2r + 2k (at the full raster, k = 1 and no wall moving, the
//            same rule gave the 2r + 2 this check used to draw).
//
// The negative control starts the first pass at r/2, a tool gouging its
// walls, which enters the narrow slot.
//---------------------------------------------------------------------------
int runSlot( int width, int height, int perturb, bool quiet = false )
{
	int failures = 0;
	const double r0 = 0.5 * controls::ToolDiameterHeights( controls::ToolDiameterParam( 0.1f ) ) * height;
	const int narrow = static_cast< int >( std::ceil( 2.0 * r0 ) ) - 2;
	const int wide   = static_cast< int >( std::floor( 2.0 * r0 + 2.0 * kLattice ) ) + 1;
	const int mouth  = static_cast< int >( std::lround( 0.45 * width ) );
	const int end    = static_cast< int >( std::lround( 0.92 * width ) );
	const int cell   = ( width + Toolpath::kTraceMaxWidth - 1 ) / Toolpath::kTraceMaxWidth;
	const int endWall = latticeEdge( end, false );

	for( int w : { narrow, wide } )
	{
		Image source( width, height, 0.0f );
		source.fill( static_cast< int >( 0.05 * width ), static_cast< int >( 0.12 * height ), mouth,
		             static_cast< int >( 0.88 * height ), 1.0f );
		const int s0 = height / 2 - w / 2, s1 = s0 + w;//slot rows [s0, s1)
		source.fill( mouth, s0, end, s1, 1.0f );

		Image picture;
		double r = 0.0;
		if( !machine( width, height, {}, source, perturb, picture, 0, nullptr, &r ) )
		{
			std::printf( "slot: render failed  FAILED\n" );
			return failures + 1;
		}

		if( w < 2.0 * r )
		{
			const int from = static_cast< int >( std::ceil( mouth + 0.5 * kLattice + cell + r + 1.0 ) );
			int cutPixels  = 0;
			float worst    = 0.0f;
			for( int y = s0; y < s1; ++y )
				for( int x = from; x < end; ++x )
				{
					const float c = coverage( picture, x, y );
					if( c > 0.0f )
						++cutPixels;
					worst = std::max( worst, c );
				}
			const bool ok = cutPixels == 0 && from < end;
			if( !ok )
				++failures;
			if( !quiet )
				std::printf( "slot w = %d px < 2r = %.2f: from x = %d to the end (%d px of slot) %d pixels cut, most %.3f  %s\n", w,
				             2.0 * r, from, end - from, cutPixels, worst, verdict( ok ) );
		}
		else
		{
			//The working slot's middle row, and its working end wall.
			const int row  = ( latticeEdge( s0, true ) + latticeEdge( s1, false ) ) / 2;
			int uncut      = 0;
			float least    = 1.0f;
			for( int x = mouth; x < endWall - 1; ++x )
			{
				const float c = coverage( picture, x, row );
				if( c < 0.5f )
					++uncut;
				least = std::min( least, c );
			}
			const bool ok = uncut == 0;
			if( !ok )
				++failures;
			if( !quiet )
				std::printf( "slot w = %d px > 2r + 2k = %.2f: its centre row cut from the mouth to the end, least coverage %.3f, "
				             "%d pixels under 0.5  %s\n",
				             w, 2.0 * r + 2.0 * kLattice, least, uncut, verdict( ok ) );
		}
	}
	if( !quiet )
		std::printf( "slot: %s\n", failures == 0 ? "a slot under the tool is never entered, one over it is cut end to end"
		                                         : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --scallop
//
// A big rectangular pocket, machined to the end at several stepovers. Along
// rows through the middle of its left wall the passes are vertical lines at
// r + k s from the wall, and each cuts a band 2r wide; between bands, if
// s > 2r, a ridge of stock s - 2r wide. Read off the picture: the 0.5
// crossings of the coverage, the widths of the uncut runs between them.
//
//   s > 2r   every ridge within 0.3 h of the wall is s - 2r to within
//            0.25 px: two crossings, each read off a one-pixel ramp at
//            pixel centres, where one of the two samples may be clamped --
//            worst case 0.086 px each -- plus float. Nothing else moves a
//            straight pass: the field is exact on the lattice and linear
//            beside a straight wall, so its bilinear samples on the trace
//            grid are exact too (quarter-texel weights), marching squares is
//            exact on a linear field, and Douglas-Peucker has nothing to
//            remove. The working lattice moves the WALL (up to k/2 px in,
//            --lattice), and every pass with it: a width does not see that.
//   s <= 2r  no uncut run at all: every pixel from the (working) wall to 0.3 h has
//            coverage >= 0.5 - 2^-11 (half a half-float ULP at 0.5: the cut
//            buffer is R16F, and at s = 2r two bands meet at exactly 0.5).
//
// The negative control reads the stepover in radii, not diameters.
//---------------------------------------------------------------------------
int runScallop( int width, int height, int perturb, bool quiet = false )
{
	int failures = 0;
	const int sx0 = static_cast< int >( std::lround( 0.05 * width ) );
	const int sy0 = static_cast< int >( std::lround( 0.1 * height ) );
	Image source( width, height, 0.0f );
	source.fill( sx0, sy0, width - sx0, height - sy0, 1.0f );
	//The pocket's left, bottom and top walls, on the working lattice.
	const int x0 = latticeEdge( sx0, true );
	const int y0 = latticeEdge( sy0, true ), y1 = latticeEdge( height - sy0, false );

	const float diameter = 0.06f;
	for( float stepover : { 0.5f, 1.0f, 1.25f, 1.5f, 2.0f } )
	{
		Image picture;
		double r = 0.0;
		const Settings settings = { { "Tool Diameter", controls::ToolDiameterParam( diameter ) },
			                        { "Stepover", controls::StepoverParam( stepover ) } };
		if( !machine( width, height, settings, source, perturb, picture, 0, nullptr, &r ) )
		{
			std::printf( "scallop: render failed  FAILED\n" );
			return failures + 1;
		}
		const double s    = controls::StepoverDiameters( controls::StepoverParam( stepover ) ) * 2.0 * r;
		const double span = 0.3 * height;

		//Rows far enough from the top and bottom walls that every pass whose
		//band reaches into the span is still a straight vertical line there:
		//a pass at d runs down the left wall only between d from the top and
		//d from the bottom, and a band reaches r past its pass.
		std::vector< double > widths;
		float least      = 1.0f;
		int rows         = 0;
		const int margin = static_cast< int >( std::ceil( span + 2.0 * r ) );
		for( int y = y0 + margin; y < y1 - margin; y += std::max( 1, height / 180 ) )
		{
			++rows;
			double down = -1.0;
			for( int x = x0; x + 1 < width && x + 1 - x0 < span; ++x )
			{
				const double c0 = coverage( picture, x, y ), c1 = coverage( picture, x + 1, y );
				least = std::min( least, static_cast< float >( c0 ) );
				if( c0 >= 0.5 && c1 < 0.5 )
					down = crossing( c0, c1, x + 0.5, x + 1.5 );
				else if( c0 < 0.5 && c1 >= 0.5 && down >= 0.0 )
				{
					//A ridge between two bands, if the band after it starts
					//inside the span.
					const double up = crossing( c0, c1, x + 0.5, x + 1.5 );
					widths.push_back( up - down );
					down = -1.0;
				}
			}
		}

		if( s > 2.0 * r )
		{
			const double predicted = s - 2.0 * r;
			double worst           = 0.0;
			for( double w : widths )
				worst = std::max( worst, std::fabs( w - predicted ) );
			//How many ridges the span must hold: bands start at k s.
			const int expectedPerRow = static_cast< int >( std::floor( ( span - 2.0 * r ) / s ) );
			const bool ok = !widths.empty() && worst <= 0.25 && static_cast< int >( widths.size() ) >= rows * std::max( 1, expectedPerRow - 1 );
			if( !ok )
				++failures;
			if( !quiet )
				std::printf( "scallop s = %.2f x 2r (s = %.2f, 2r = %.2f): %zu ridges over %d rows, predicted width %.3f, worst "
				             "error %.3f (tolerance 0.25 px)  %s\n",
				             controls::StepoverDiameters( controls::StepoverParam( stepover ) ), s, 2.0 * r, widths.size(), rows,
				             predicted, worst, verdict( ok ) );
		}
		else
		{
			const bool ok = widths.empty() && least >= 0.5f - 1.0f / 2048.0f;
			if( !ok )
				++failures;
			if( !quiet )
				std::printf( "scallop s = %.2f x 2r (s = %.2f, 2r = %.2f): %zu ridges, least coverage %.5f (floor %.5f)  %s\n",
				             controls::StepoverDiameters( controls::StepoverParam( stepover ) ), s, 2.0 * r, widths.size(),
				             least, 0.5 - 1.0 / 2048.0, verdict( ok ) );
		}
	}
	if( !quiet )
		std::printf( "scallop: %s\n", failures == 0 ? "ridges of s - 2r past s = 2r, none at or below it" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --feed
//
// A rectangular pocket; the tool starts at the bottom-left of the first pass
// and runs right along its bottom edge, a straight line longer than one
// second of travel. Its position is read every frame for one second, at 60
// fps and again at 30: along a straight line the chord IS the path, so the
// distance covered in one second is the path length per second, and must
// equal Feed in job pixels per second. Positions and time are both in
// double, so the tolerance is 1e-9 of the feed.
//
// And the position is the picture's: with the tool drawn, the centroid of
// its ring in the frame is within one texel of where the hook says (a
// symmetric antialiased ring, sampled at pixel centres).
//
// The negative control advances Feed / 60 per FRAME, which is right at 60
// fps and half speed at 30.
//---------------------------------------------------------------------------
int runFeed( int width, int height, int perturb, bool quiet = false )
{
	int failures = 0;
	const int x0 = static_cast< int >( std::lround( 0.05 * width ) );
	const int y0 = static_cast< int >( std::lround( 0.1 * height ) );
	//A RED pocket (found with the threshold at 0.1: red's luma is 0.21), so
	//in the picture the cut is red, the stock black and the tool's ring,
	//drawn white, the only thing with any green in it.
	Image source( width, height, 0.0f );
	for( int y = y0; y < height - y0; ++y )
		for( int x = x0; x < width - x0; ++x )
			source.set( x, y, 1.0f, 0.0f, 0.0f );
	const float feedHeights = 0.5f;

	for( double fps : { 60.0, 30.0 } )
	{
		Session session;
		session.fps = fps;
		baseline( session.plugin );
		set( session.plugin, "Feed", controls::FeedParam( feedHeights ) );
		set( session.plugin, "Tool Diameter", controls::ToolDiameterParam( 0.06f ) );
		set( session.plugin, "Show Tool", 1.0f );
		set( session.plugin, "Threshold", 0.1f );
		session.plugin.SetPerturbForTest( perturb );
		if( !session.begin( width, height ) )
			return failures + 1;
		session.upload( source );

		const double feed = controls::FeedHeightsPerSecond( controls::FeedParam( feedHeights ) ) * height;
		std::vector< path::Point > at;
		double worstCentroid = 0.0;
		const int frames     = static_cast< int >( fps );
		for( int frame = 0; frame <= frames; ++frame )
		{
			if( !session.renderAt( frame ) )
			{
				session.end();
				return failures + 1;
			}
			at.push_back( session.plugin.ToolForTest() );

			//The ring in the picture: it is white, the cut red, the stock
			//black, so green is the ring and only the ring.
			if( frame == frames / 2 || frame == frames )
			{
				const Image picture = session.readBack();
				double sx = 0, sy = 0, sw = 0;
				for( int y = 0; y < height; ++y )
					for( int x = 0; x < width; ++x )
					{
						const double g = picture.at( x, y )[ 1 ];
						sx += g * ( x + 0.5 );
						sy += g * ( y + 0.5 );
						sw += g;
					}
				if( sw > 0 )
					worstCentroid = std::max( worstCentroid, std::hypot( sx / sw - at.back().x, sy / sw - at.back().y ) );
				else
					worstCentroid = 1e9;
			}
		}
		session.end();

		//The tool must have stayed on one straight line: every step in the
		//same direction.
		bool straight = true;
		for( size_t i = 2; i < at.size(); ++i )
		{
			const double ax = at[ i - 1 ].x - at[ i - 2 ].x, ay = at[ i - 1 ].y - at[ i - 2 ].y;
			const double bx = at[ i ].x - at[ i - 1 ].x, by = at[ i ].y - at[ i - 1 ].y;
			if( std::fabs( ax * by - ay * bx ) > 1e-6 * ( std::hypot( ax, ay ) * std::hypot( bx, by ) + 1e-12 ) )
				straight = false;
		}
		//Frame 0 has no dt; the second runs from frame 0 to frame `frames`.
		const double covered = std::hypot( at.back().x - at.front().x, at.back().y - at.front().y );
		const double rate    = covered / ( frames / fps );
		const bool ok        = straight && std::fabs( rate - feed ) <= 1e-9 * feed && worstCentroid <= 1.0;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "feed at %2.0f fps: %.6f px/s along a straight pass against Feed = %.6f px/s (error %.2e); the ring "
			             "in the picture within %.3f px of the tool  %s\n",
			             fps, rate, feed, std::fabs( rate - feed ), worstCentroid, verdict( ok ) );
	}
	if( !quiet )
		std::printf( "feed: %s\n", failures == 0 ? "the tool covers Feed px of path per second of clip time" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --latch
//
// Latch a job, machine it for 40 frames, press Restart and render one frame:
// the region is grabbed again and the cut starts over, so at most one
// frame's sweep is cut -- a capsule of radius r and length Feed x dt, plus a
// pixel of antialiasing all round. This happens at the job's own raster, so
// no buffer is reallocated and only the Restart can have cleared anything.
//
// Then machine 40 frames more and resize to twice the raster -- the same
// instance, as a host does -- and render one frame. Every pixel fully cut
// before must still read as cut at each of the four output pixels it now
// covers: bilinear from the job-raster cut at a quarter texel off its
// centre, the nearest texel weighs 0.75 x 0.75 = 0.5625, so a fully cut
// texel gives at least that (less 1/256 for the filter's 8-bit weights).
//
// Negative controls: a resize that re-grabs the job (the photofinish bug),
// and a Restart that leaves the cut buffer.
//---------------------------------------------------------------------------
int runLatch( int width, int height, int perturb, bool quiet = false )
{
	int failures = 0;
	auto sourceAt = [ & ]( int w, int h ) {
		Image s( w, h, 0.0f );
		const int mx = static_cast< int >( std::lround( 0.05 * w ) ), my = static_cast< int >( std::lround( 0.1 * h ) );
		s.fill( mx, my, w - mx, h - my, 1.0f );
		return s;
	};
	auto countCut = []( const Image& picture ) {
		int n = 0;
		for( int y = 0; y < picture.height; ++y )
			for( int x = 0; x < picture.width; ++x )
				if( coverage( picture, x, y ) >= 0.5f )
					++n;
		return n;
	};

	Session session;
	baseline( session.plugin );
	const float feedHeights = 2.0f;
	set( session.plugin, "Feed", controls::FeedParam( feedHeights ) );
	session.plugin.SetPerturbForTest( perturb );
	if( !session.begin( width, height ) )
		return 1;
	session.upload( sourceAt( width, height ) );
	int frame = 0;

	//Restart, at the job's own raster -- so nothing is reallocated, and a
	//cleared cut can only have been cleared by the Restart.
	for( ; frame < 40; ++frame )
		session.renderAt( frame );
	const int cutBefore = countCut( session.readBack() );
	session.press( "Restart" );
	session.renderAt( frame++ );
	const int cutAfter = countCut( session.readBack() );
	const double r     = 0.5 * controls::ToolDiameterHeights( controls::ToolDiameterParam( 0.1f ) ) * height;
	const double step  = controls::FeedHeightsPerSecond( controls::FeedParam( feedHeights ) ) * height / session.fps;
	const double bound = 2.0 * ( r + 1.0 ) * step + 3.14159265358979 * ( r + 1.0 ) * ( r + 1.0 );
	const bool cleared = cutAfter <= bound && cutBefore > 4 * bound;
	if( !cleared )
		++failures;
	if( !quiet )
		std::printf( "latch restart: %d pixels cut after 40 frames, %d one frame after Restart, at most %.0f (one frame's "
		             "capsule)  %s\n",
		             cutBefore, cutAfter, bound, verdict( cleared ) );

	//Resize: machine on, then hand the same instance twice the raster.
	for( const int until = frame + 40; frame < until; ++frame )
		session.renderAt( frame );
	const Image before = session.readBack();

	session.resize( 2 * width, 2 * height );
	session.upload( sourceAt( 2 * width, 2 * height ) );
	session.renderAt( frame++ );
	const Image after = session.readBack();

	int fullyCut = 0, lost = 0;
	float least  = 1.0f;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			if( coverage( before, x, y ) < 1.0f )
				continue;
			++fullyCut;
			for( int dy = 0; dy < 2; ++dy )
				for( int dx = 0; dx < 2; ++dx )
				{
					const float c = coverage( after, 2 * x + dx, 2 * y + dy );
					least         = std::min( least, c );
					if( c < 0.5625f - 1.0f / 256.0f )
						++lost;
				}
		}
	const bool survived = fullyCut > 0 && lost == 0 && session.plugin.JobWidthForTest() == width;
	if( !survived )
		++failures;
	if( !quiet )
		std::printf( "latch resize %dx%d -> %dx%d: %d fully cut pixels before, %d of their %d successors below 0.5586 "
		             "(least %.4f); the job still at %dx%d  %s\n",
		             width, height, 2 * width, 2 * height, fullyCut, lost, 4 * fullyCut, least,
		             session.plugin.JobWidthForTest(), session.plugin.JobHeightForTest(), verdict( survived ) );
	session.end();

	if( !quiet )
		std::printf( "latch: %s\n", failures == 0 ? "Restart clears the part, and the part survives a resize" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --lattice
//
// The field is computed on the working lattice -- kFieldScale job pixels a
// texel -- and nowhere else. Every other check re-derives its tolerance for
// that lattice; this one proves it is the lattice in use, out of the
// plugin's own field, on a fixture a full-raster field cannot pass:
//
//   a pocket whose four walls are all at odd pixels -- the lattice moves
//   each a pixel in; a single black pixel inside it (an outside speck, three
//   quarters of its block inside, so on the lattice it is not there); and a
//   white hairline one pixel wide beside it (half of every block it touches,
//   not more, so on the lattice it is not a pocket at all).
//
//   raster   the field is ceil( W / k ) x ceil( H / k ) texels, and the
//            plugin says k.
//   sign     at EVERY job pixel, the field in the texel that holds it is
//            positive exactly where the reduced mask (reduceMask, the
//            plugin's majority rule, derived independently here) is inside:
//            0 disagreements. And the fixture must be able to tell: the
//            job-raster mask itself must disagree with the reduced one at
//            some pixels, or a full-raster field would pass too.
//
// The negative control computes the field at the full raster.
//---------------------------------------------------------------------------
int runLattice( int width, int height, int perturb, bool quiet = false )
{
	const int k  = kLattice;
	auto odd     = []( double v ) { return static_cast< int >( std::lround( v ) ) | 1; };
	const int x0 = odd( 0.1 * width ), x1 = odd( 0.8 * width );
	const int y0 = odd( 0.15 * height ), y1 = odd( 0.85 * height );
	const int hair  = odd( 0.9 * width );
	const int speckX = odd( 0.45 * width ), speckY = odd( 0.5 * height );

	std::vector< uint8_t > mask( static_cast< size_t >( width ) * height, 0 );
	auto put = [ & ]( int x, int y, uint8_t v ) {
		if( x >= 0 && y >= 0 && x < width && y < height )
			mask[ static_cast< size_t >( y ) * width + x ] = v;
	};
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
			put( x, y, 1 );
	put( speckX, speckY, 0 );
	for( int y = y0; y < y1; ++y )
		put( hair, y, 1 );

	Image source( width, height, 0.0f );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			if( mask[ static_cast< size_t >( y ) * width + x ] )
				source.set( x, y, 1.0f, 1.0f, 1.0f );

	Session session;
	baseline( session.plugin );
	set( session.plugin, "Mode", static_cast< float >( controls::kField ) );
	session.plugin.SetPerturbForTest( perturb );
	if( !session.begin( width, height ) )
	{
		std::printf( "lattice: render failed  FAILED\n" );
		return 1;
	}
	session.upload( source );
	if( !session.renderAt( 0 ) )
	{
		session.end();
		std::printf( "lattice: render failed  FAILED\n" );
		return 1;
	}
	std::vector< float > field;
	int fw = 0, fh = 0;
	session.plugin.ReadFieldForTest( field, fw, fh );
	const int scale = session.plugin.FieldScaleForTest();
	session.end();

	int failures = 0;
	const int ww = latticeSide( width ), wh = latticeSide( height );
	const bool rasterOk = fw == ww && fh == wh && scale == k;
	if( !rasterOk )
		++failures;
	if( !quiet )
		std::printf( "lattice raster: the field is %dx%d texels at %d px a texel, against %dx%d at %d  %s\n", fw, fh, scale, ww,
		             wh, k, verdict( rasterOk ) );

	//Sign, pixel by pixel: the field's texel for each job pixel, by the
	//field's own size (so a full-raster field is read as what it is).
	const std::vector< uint8_t > reduced = reduceMask( mask, width, height );
	int disagree = 0, discriminating = 0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const bool want = reduced[ static_cast< size_t >( y / k ) * ww + x / k ] != 0;
			if( ( mask[ static_cast< size_t >( y ) * width + x ] != 0 ) != want )
				++discriminating;
			if( fw <= 0 || fh <= 0 )
			{
				++disagree;
				continue;
			}
			const int tx = static_cast< int >( static_cast< long long >( x ) * fw / width );
			const int ty = static_cast< int >( static_cast< long long >( y ) * fh / height );
			const bool got = field[ static_cast< size_t >( ty ) * fw + tx ] > 0.0f;
			if( got != want )
				++disagree;
		}
	const bool signOk = disagree == 0 && discriminating > 0;
	if( !signOk )
		++failures;
	if( !quiet )
		std::printf( "lattice sign: %d of %d job pixels disagree with the reduced mask (must be 0); the job-raster mask "
		             "differs from it at %d, so a full-raster field could not pass  %s\n",
		             disagree, width * height, discriminating, verdict( signOk ) );
	if( !quiet )
		std::printf( "lattice: %s\n", failures == 0 ? "the region is decided, and the field computed, on the working lattice"
		                                            : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the
// PLUGIN -- through a hook it carries at zero -- and asserts that the check
// catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	const Control controls[] = {
		{ "distance with no 1+JFA prepass                ", runDistance( width, height, Toolpath::kPerturbNoPrepass, true ) },
		{ "fillet with a square tool                     ", runFillet( width, height, Toolpath::kPerturbSquareTool, true ) },
		{ "slot with the first pass at r/2               ", runSlot( width, height, Toolpath::kPerturbFirstLevelHalf, true ) },
		{ "scallop with the stepover read in radii       ", runScallop( width, height, Toolpath::kPerturbStepoverRadius, true ) },
		{ "feed advancing Feed/60 a frame, whatever dt   ", runFeed( width, height, Toolpath::kPerturbFeedPerFrame, true ) },
		{ "latch with a resize that re-grabs the job     ", runLatch( width, height, Toolpath::kPerturbResizeClears, true ) },
		{ "latch with a Restart that keeps the cut       ", runLatch( width, height, Toolpath::kPerturbRestartKeeps, true ) },
		{ "lattice with the field at the full raster     ", runLattice( width, height, Toolpath::kPerturbFullResField, true ) },
	};
	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed plugin is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// The offline checks: no GL.
//---------------------------------------------------------------------------

/// --names: every name fits the host's 16 characters and is unique.
int runNames()
{
	Toolpath plugin;
	int failures = 0;
	std::map< std::string, int > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name.size() > 16 )
		{
			std::printf( "names: '%s' is %zu characters, over 16  FAILED\n", p.name.c_str(), p.name.size() );
			++failures;
		}
		if( seen[ p.name ]++ > 0 )
		{
			std::printf( "names: '%s' is not unique  FAILED\n", p.name.c_str() );
			++failures;
		}
	}
	const char* display = "SW Toolpath";
	if( std::strlen( display ) > 16 )
		++failures;
	std::printf( "names: %zu parameters%s; display name '%s'  %s\n", seen.size(),
	             failures == 0 ? ", all within 16 characters and unique" : "", display, verdict( failures == 0 ) );
	return failures;
}

/// --exact: the reference EDT against brute force, on random masks. The
/// negative control swaps in a city-block distance, which must disagree.
int runExact( bool cityBlock, bool quiet = false )
{
	int failures = 0;
	uint32_t state = 12345u;
	auto next      = [ & ]() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};
	int masks = 0, pixels = 0;
	for( int trial = 0; trial < 40; ++trial )
	{
		const int w = 8 + static_cast< int >( next() % 57 ), h = 6 + static_cast< int >( next() % 41 );
		const int density = 1 + static_cast< int >( next() % 60 );
		std::vector< uint8_t > mask( static_cast< size_t >( w ) * h );
		for( auto& m : mask )
			m = ( next() % 100 ) < static_cast< uint32_t >( density ) ? 0 : 1;
		const std::vector< double > fast = exactField( mask, w, h, cityBlock );
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				const size_t i  = static_cast< size_t >( y ) * w + x;
				double best     = kInf;
				for( int yy = 0; yy < h; ++yy )
					for( int xx = 0; xx < w; ++xx )
						if( mask[ static_cast< size_t >( yy ) * w + xx ] != mask[ i ] )
							best = std::min( best, double( xx - x ) * ( xx - x ) + double( yy - y ) * ( yy - y ) );
				double brute;
				if( mask[ i ] )
				{
					const double wall = std::min( std::min( x + 1, w - x ), std::min( y + 1, h - y ) );
					brute             = std::min( best < kInf ? std::sqrt( best ) : kInf, wall ) - 0.5;
				}
				else
					brute = best < kInf ? 0.5 - std::sqrt( best ) : -1.0e6;
				++pixels;
				if( fast[ i ] != brute )
					++failures;
			}
		++masks;
	}
	if( !quiet )
		std::printf( "exact: the reference EDT against brute force on %d random masks, %d pixels: %d disagree (must be 0, "
		             "exactly)  %s\n",
		             masks, pixels, failures, verdict( failures == 0 ) );
	return failures;
}

/// --march: the tracer on analytic fields.
///
/// A cone f = R - |p - c| sampled on a grid: its level L is a circle of
/// radius R - L. Linear interpolation of the cone along a cell edge is off
/// by at most cell^2 / (8 rho), rho the distance from the centre (the
/// cone's second derivative along a line is at most 1 / rho), so every
/// traced point must lie within that of its circle (plus 1e-9). Every level
/// gives exactly one loop, anticlockwise (positive area: higher field on the
/// left). And the ordered job: every loop once, rapids only between loops,
/// the timeline increasing, the cut spans summing to the cut length.
///
/// The negative controls put each crossing at the nearer sample instead of
/// interpolating, and cut every corner with its chord.
int runMarch( int tracePerturb, bool quiet = false )
{
	int failures = 0;
	const int tw = 97, th = 61;
	const double cell = 1.5;
	const double cx = 0.5 * tw * cell + 0.3, cy = 0.5 * th * cell - 0.2, R = 40.0;
	std::vector< float > grid( static_cast< size_t >( tw ) * th );
	for( int j = 0; j < th; ++j )
		for( int i = 0; i < tw; ++i )
			grid[ static_cast< size_t >( j ) * tw + i ] =
				static_cast< float >( R - std::hypot( ( i + 0.5 ) * cell - cx, ( j + 0.5 ) * cell - cy ) );

	path::Levels levels{ 3.0, 4.5, 8 };//circles of radius 37 down to 5.5
	const std::vector< path::Loop > loops = path::TraceLevels( grid.data(), tw, th, cell, cell, levels, tracePerturb );

	std::vector< int > perLevel( static_cast< size_t >( levels.count ), 0 );
	double worst = 0.0;
	bool orientation = true;
	for( const path::Loop& loop : loops )
	{
		++perLevel[ static_cast< size_t >( loop.level ) ];
		const double radius = R - ( levels.first + loop.level * levels.step );
		const double tol    = cell * cell / ( 8.0 * ( radius - cell ) ) + 1e-6;
		double area         = 0.0;
		for( size_t n = 0; n < loop.points.size(); ++n )
		{
			const path::Point& a = loop.points[ n ];
			const path::Point& b = loop.points[ ( n + 1 ) % loop.points.size() ];
			area += a.x * b.y - b.x * a.y;
			const double e = std::fabs( std::hypot( a.x - cx, a.y - cy ) - radius );
			worst          = std::max( worst, e / tol );
		}
		if( area <= 0.0 )
			orientation = false;
	}
	bool oneEach = true;
	for( int n : perLevel )
		if( n != 1 )
			oneEach = false;
	const bool circles = worst <= 1.0;
	if( !circles || !oneEach || !orientation )
		++failures;
	if( !quiet )
		std::printf( "march: %zu loops for %d levels, one each %s, anticlockwise %s; worst point %.3f of its bound "
		             "cell^2/(8 rho)  %s\n",
		             loops.size(), levels.count, oneEach ? "yes" : "NO", orientation ? "yes" : "NO", worst,
		             verdict( circles && oneEach && orientation ) );

	//Corners. The field of a rectangle, min( x - a, b - x, y - c, d - y ), is
	//linear along every cell edge the contour crosses, so its crossings are
	//exact, and its offset contour at L is the rectangle inset by L, with
	//four right-angled corners placed off the grid. Every traced point must
	//be on that rectangle, and each corner must be a traced point, both to
	//one float ULP of the largest sample (the grid is float; a crossing is
	//off by the rounding of the samples it is interpolated from, over a
	//slope of 1): the straight runs either side of a corner meet exactly at
	//it.
	{
		const double a = 3.3, b = 120.1, c = 2.7, d = 80.4, L = 7.35;
		std::vector< float > box( static_cast< size_t >( tw ) * th );
		for( int j = 0; j < th; ++j )
			for( int i = 0; i < tw; ++i )
			{
				const double x = ( i + 0.5 ) * cell, y = ( j + 0.5 ) * cell;
				box[ static_cast< size_t >( j ) * tw + i ] = static_cast< float >( std::min( { x - a, b - x, y - c, d - y } ) );
			}
		const std::vector< path::Loop > rect = path::TraceLevels( box.data(), tw, th, cell, cell, path::Levels{ L, 1.0, 1 },
		                                                          tracePerturb );
		double offOutline = 0.0, missedCorner = 0.0;
		if( rect.size() == 1 )
		{
			for( const path::Point& q : rect[ 0 ].points )
			{
				const double e = std::min( { std::fabs( q.x - ( a + L ) ), std::fabs( q.x - ( b - L ) ), std::fabs( q.y - ( c + L ) ),
				                             std::fabs( q.y - ( d - L ) ) } );
				offOutline = std::max( offOutline, e );
			}
			for( const path::Point& corner : { path::Point{ a + L, c + L }, path::Point{ b - L, c + L }, path::Point{ b - L, d - L },
			                                   path::Point{ a + L, d - L } } )
			{
				double nearest = 1e9;
				for( const path::Point& q : rect[ 0 ].points )
					nearest = std::min( nearest, std::hypot( q.x - corner.x, q.y - corner.y ) );
				missedCorner = std::max( missedCorner, nearest );
			}
		}
		float largest = 0.0f;
		for( float v : box )
			largest = std::max( largest, v );
		const double tolerance = ulp( largest );
		const bool ok          = rect.size() == 1 && offOutline <= tolerance && missedCorner <= tolerance;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "march corners: %zu loop, every point within %.1e of the inset rectangle, every corner within %.1e "
			             "of a traced point (both to 1 ULP of %.1f, %.1e)  %s\n",
			             rect.size(), offOutline, missedCorner, largest, tolerance, verdict( ok ) );
	}

	//The ordered job, both ways round.
	for( bool insideOut : { false, true } )
	{
		const path::Path job = path::Order( loops, levels.count, insideOut, path::Point{ 0.0, 0.0 } );
		int rapids = 0;
		bool monotone = true;
		for( size_t i = 0; i + 1 < job.points.size(); ++i )
		{
			if( !job.cut[ i ] )
				++rapids;
			if( !( job.tau[ i + 1 ] > job.tau[ i ] ) )
				monotone = false;
		}
		std::vector< path::Span > spans;
		path::CutSpans( job, 0.0, job.Duration(), spans );
		double sum = 0.0;
		for( const auto& s : spans )
			sum += std::hypot( s.b.x - s.a.x, s.b.y - s.a.y );
		//The first pass visited: the outermost for Outside In, the innermost
		//for Inside Out.
		const double firstRadius = std::hypot( job.points.front().x - cx, job.points.front().y - cy );
		const double wantRadius  = R - ( levels.first + ( insideOut ? levels.count - 1 : 0 ) * levels.step );
		const bool ok = job.loops == static_cast< int >( loops.size() ) && rapids == job.loops - 1 && monotone
		                && std::fabs( sum - job.cutLength ) <= 1e-9 * job.cutLength
		                && std::fabs( firstRadius - wantRadius ) < 1.0;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "march order %s: %d loops, %d rapids, timeline increasing %s, cut spans %.6f = cut length %.6f, "
			             "starts on the r = %.2f pass  %s\n",
			             insideOut ? "Inside Out" : "Outside In", job.loops, rapids, monotone ? "yes" : "NO", sum,
			             job.cutLength, firstRadius, verdict( ok ) );
	}
	return failures;
}

int runNegativeOffline()
{
	int failures = 0;
	const struct
	{
		const char* name;
		int seen;
	} controls[] = {
		{ "exact with a city-block distance              ", runExact( true, true ) },
		{ "march with crossings at the nearer sample     ", runMarch( path::kTraceNearestCrossing, true ) },
		{ "march with every corner cut by its chord      ", runMarch( path::kTraceNoCorners, true ) },
	};
	for( const auto& c : controls )
	{
		const bool ok = c.seen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative-offline: every perturbation is caught" : "negative-offline: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
struct BenchResult
{
	double ms       = -1.0;
	double fieldMs  = 0.0;
	double traceMs  = 0.0;
};

BenchResult benchAt( const std::vector< std::string >& settings, int width, int height, int frames, double fps, bool live )
{
	BenchResult result;
	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& s : settings )
	{
		std::string error;
		applySetting( session.plugin, s, error );
	}
	if( live )
		set( session.plugin, "Geometry", static_cast< float >( controls::kLive ) );
	if( !session.begin( width, height ) )
		return result;
	session.upload( buildCard( width, height, 0 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.renderAt( frame );
	glFinish();

	//The best of three runs: this machine's GPU is shared, and a run that
	//lost it for a few milliseconds measures the contention, not the plugin.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	result.ms = best;

	//The field alone, timed inside the plugin with glFinish on both sides.
	session.plugin.SetTimingForTest( true );
	double fieldBest = 1e9, traceBest = 1e9;
	set( session.plugin, "Geometry", static_cast< float >( controls::kLive ) );
	for( int frame = 0; frame < 10; ++frame )
	{
		session.renderAt( warmup + 3 * frames + frame );
		fieldBest = std::min( fieldBest, session.plugin.LastFieldMillisForTest() );
		traceBest = std::min( traceBest, session.plugin.LastTraceMillisForTest() );
	}
	result.fieldMs = fieldBest;
	result.traceMs = traceBest;
	session.end();
	return result;
}

int runBench( const std::vector< std::string >& settings, int frames, double fps, bool fourK )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	std::vector< Size > sizes = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 } };
	if( fourK )
		sizes.push_back( { "3840x2160 ", 3840, 2160 } );

	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution   Latch ms/frame   Live ms/frame   field (flood) ms      trace+order ms\n" );
	for( const Size& size : sizes )
	{
		const BenchResult latch = benchAt( settings, size.width, size.height, frames, fps, false );
		const BenchResult live  = benchAt( settings, size.width, size.height, frames, fps, true );
		std::printf( "%s    %8.3f         %8.3f          %8.3f             %8.3f\n", size.name, latch.ms, live.ms,
		             live.fieldMs, live.traceMs );
	}
	std::printf( "\nLatch pays for the field and the trace once per job, then only stamps and composites.\n"
	             "Live pays for both every frame, and the trace is CPU work after a readback stall.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },       { "detect.frag", shaders::Detect() },
		{ "blur.frag", shaders::Blur() },           { "seed.frag", shaders::Seed() },
		{ "flood.frag", shaders::Flood() },         { "resolve.frag", shaders::Resolve() },
		{ "sample.frag", shaders::Sample() },       { "stamp.vert", shaders::StampVertex() },
		{ "stamp.frag", shaders::StampFragment() }, { "composite.frag", shaders::Composite() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"tptest -- render and measure the Toolpath CNC-pocketing effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/toolpath.png)\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source S          card (default), white, or black\n"
		"  --set \"Name=V\"      set a parameter by its display name. Repeatable.\n"
		"  --press \"Name@F\"    press an event parameter before frame F. Repeatable.\n"
		"  --list              print every parameter, its kind, default and range, then exit\n"
		"  --distance          the flooded field against an exact EDT of the working lattice, with and without the corrections\n"
		"  --fillet            inside corners keep a fillet of the tool's radius\n"
		"  --slot              a slot under 2r is never entered; over 2r it is cut end to end\n"
		"  --scallop           ridges s - 2r wide past s = 2r; none at or below\n"
		"  --feed              the tool covers Feed px of path per second, at 60 and 30 fps\n"
		"  --latch             the part survives a resize; Restart clears it\n"
		"  --lattice           the field is on the working lattice, kFieldScale px a texel\n"
		"  --negative          every GL check above can fail\n"
		"  --perturb BITS      run the checks against a perturbed plugin (Toolpath.h), verbosely\n"
		"  --names             every parameter name fits 16 characters and is unique (no GL)\n"
		"  --exact             the reference EDT against brute force (no GL)\n"
		"  --march             the tracer and the ordering on analytic fields (no GL)\n"
		"  --negative-offline  the no-GL checks can fail\n"
		"  --offline           all of the no-GL checks: what CI runs on a runner with no GPU\n"
		"  --allow-no-gl       with GL checks: SKIP loudly, not FAIL, when no context can be made\n"
		"  --bench             time ProcessOpenGL at 720p and 1080p, Latch and Live, and the field\n"
		"  --bench-4k          --bench, and 4K too (slow on a shared machine: once, not in a loop)\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/toolpath.png";
	std::string scriptPath;
	std::string sourceName = "card";
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool bench4k   = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::pair< std::string, int > > presses;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--press" && hasNext )
		{
			const std::string spec = argv[ ++i ];
			const size_t at        = spec.rfind( '@' );
			if( at == std::string::npos )
			{
				std::fprintf( stderr, "--press wants Name@Frame, got '%s'\n", spec.c_str() );
				return 2;
			}
			presses.emplace_back( spec.substr( 0, at ), std::atoi( spec.substr( at + 1 ).c_str() ) );
		}
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--bench-4k" )
			wantBench = bench4k = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
		{
			//Defined HERE, as every check that needs no GL, so a new one
			//cannot be left out of CI by a workflow that lists them.
			for( const char* c : { "--names", "--exact", "--march", "--negative-offline" } )
				checks.push_back( c );
		}
		else if( argument == "--distance" || argument == "--fillet" || argument == "--slot" || argument == "--scallop"
		         || argument == "--feed" || argument == "--latch" || argument == "--lattice" || argument == "--negative"
		         || argument == "--names"
		         || argument == "--exact" || argument == "--march" || argument == "--negative-offline" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		Toolpath plugin;
		std::printf( "%3s  %-18s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-18s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}

	//The checks that need no context run before one is made, so --offline
	//works on a machine where making one fails.
	bool needGL = wantBench || wantPipe || checks.empty();
	for( const std::string& check : checks )
		if( check != "--names" && check != "--exact" && check != "--march" && check != "--negative-offline" )
			needGL = true;

	int failures = 0;
	for( const std::string& check : checks )
	{
		if( check == "--names" )
			failures += runNames();
		else if( check == "--exact" )
			failures += runExact( false );
		else if( check == "--march" )
			failures += runMarch( 0 );
		else if( check == "--negative-offline" )
			failures += runNegativeOffline();
		else
			continue;
		std::printf( "\n" );
	}
	if( !needGL )
	{
		std::printf( "offline: shaders NOT checked against a driver here -- that is tools/check-shaders.sh (glslc)\n"
		             "and the GL checks, which run on the dev Mac in tools/verify.sh\n" );
		return failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		//A GitHub macOS runner cannot make an accelerated 4.1 context. With
		//--allow-no-gl that is a loud SKIP of the GL checks rather than a
		//red build that says nothing about the plugin; without it, a failure.
		if( allowNoGL )
		{
			std::printf( "SKIPPED (--allow-no-gl): no OpenGL 4.1 context on this machine, so NONE of the GL checks ran.\n"
			             "They run on the dev Mac in tools/verify.sh.\n" );
			return failures == 0 ? 0 : 1;
		}
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	bool ranGL = false;
	for( const std::string& check : checks )
	{
		int result = -1;
		if( check == "--distance" )
			result = runDistance( width, height, perturb );
		else if( check == "--fillet" )
			result = runFillet( width, height, perturb );
		else if( check == "--slot" )
			result = runSlot( width, height, perturb );
		else if( check == "--scallop" )
			result = runScallop( width, height, perturb );
		else if( check == "--feed" )
			result = runFeed( width, height, perturb );
		else if( check == "--latch" )
			result = runLatch( width, height, perturb );
		else if( check == "--lattice" )
			result = runLattice( width, height, perturb );
		else if( check == "--negative" )
			result = runNegative( width, height );
		if( result < 0 )
			continue;
		ranGL = true;
		failures += result;
		std::printf( "\n" );
	}
	if( ranGL || ( !checks.empty() && !wantBench && !wantPipe ) )
		return finish( failures == 0 ? 0 : 1 );

	if( wantBench )
		return finish( runBench( settings, frames, fps, bench4k ) );

	Session session;
	session.fps         = fps;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	for( const auto& p : presses )
		if( indexOfParameter( session.plugin, p.first ) < 0 )
		{
			std::fprintf( stderr, "--press %s: no parameter by that name\n", p.first.c_str() );
			return finish( 2 );
		}

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		//A reader that goes away mid-stream would otherwise kill this process
		//with SIGPIPE on the next write -- exit 141, and no word on stderr.
		//Ignored, the write fails with EPIPE, and the loop below says so and
		//exits 1, which is the contract.
		std::signal( SIGPIPE, SIG_IGN );

		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				session.end();
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					session.end();
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame at the end of a pipe is the end of the stream,
			//not a frame to render.
			if( got < frame.size() )
				break;

			//Through the plugin's own setter, so a cue moves what an
			//operator's slider would. An event cue (Restart) is a press on
			//the frame its value rises through 0.5.
			for( const auto& track : automation )
			{
				const float v = valueAt( track.second, index );
				if( session.plugin.GetParamType( track.first ) == FF_TYPE_EVENT )
				{
					if( v >= 0.5f && ( index == 0 || valueAt( track.second, index - 1 ) < 0.5f ) )
					{
						session.plugin.SetFloatParameter( track.first, 1.0f );
						session.plugin.SetFloatParameter( track.first, 0.0f );
					}
				}
				else
					session.plugin.SetFloatParameter( track.first, v );
			}

			session.uploadTopFirst( frame );
			if( !session.renderAt( index ) )
			{
				status = 1;
				break;
			}
			const std::vector< unsigned char > out = session.readBackTopFirst();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame on stdout is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	for( int frame = 0; frame < frames; ++frame )
	{
		for( const auto& p : presses )
			if( p.second == frame )
				session.press( p.first.c_str() );
		Image source;
		if( sourceName == "white" )
			source = Image( width, height, 1.0f );
		else if( sourceName == "black" )
			source = Image( width, height, 0.0f );
		else
			source = buildCard( width, height, frame );
		session.upload( source );
		if( !session.renderAt( frame ) )
		{
			session.end();
			return finish( 1 );
		}
	}
	const std::vector< unsigned char > image = session.readBackTopFirst();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
