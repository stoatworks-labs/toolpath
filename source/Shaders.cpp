#include "Shaders.h"

namespace toolpath::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every full-screen pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// 1. detect. tinsel's Detect On, unchanged in meaning.
//---------------------------------------------------------------------------
const char* const kDetectBody = R"(
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform int DetectOn;//0 luma, 1 alpha, 2 chroma, 3 luma or alpha

in vec2 uv;
out vec4 fragColor;

float channel( vec4 c )
{
	if( DetectOn == 1 )
		return c.a;

	if( DetectOn == 2 )
	{
		//Chroma: distance from the pixel's own grey, so two colours of equal
		//brightness are still two regions.
		float y = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		return length( c.rgb - vec3( y ) ) + y * 0.25;
	}

	//Un-premultiply before taking luma, or a soft alpha edge reads as a
	//brightness ramp.
	vec3 straight = c.a > 0.0031 ? c.rgb / c.a : c.rgb;
	float luma    = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );

	//Luma or Alpha: the alpha is a floor so a dark mark on transparency is
	//still a region, and luma adds the detail on top. NOT max( luma * a, a ),
	//which is 1.0 for every opaque pixel (tinsel's trap).
	if( DetectOn == 3 )
		return c.a * ( 0.35 + 0.65 * luma );

	return luma;
}

void main()
{
	fragColor = vec4( channel( texture( InputTexture, uv * MaxUV ) ) );
}
)";

//---------------------------------------------------------------------------
// 2. blur. One axis of a Gaussian, texel for texel, clamped at the frame.
//---------------------------------------------------------------------------
const char* const kBlurBody = R"(
uniform sampler2D Value;
uniform ivec2 Axis;  //(1, 0) or (0, 1)
uniform float Sigma; //pixels
uniform int Taps;    //each side

out vec4 fragColor;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 last = textureSize( Value, 0 ) - ivec2( 1 );
	float sum = 0.0, weights = 0.0;
	for( int k = -Taps; k <= Taps; ++k )
	{
		float w = exp( -0.5 * float( k * k ) / ( Sigma * Sigma ) );
		sum += w * texelFetch( Value, clamp( p + Axis * k, ivec2( 0 ), last ), 0 ).r;
		weights += w;
	}
	fragColor = vec4( sum / weights );
}
)";

//---------------------------------------------------------------------------
// 3. seed. Inside or outside, and each pixel its own first seed.
//---------------------------------------------------------------------------
const char* const kSeedBody = R"(
uniform sampler2D Value;
uniform float Threshold;
uniform int Invert;

out uvec4 fragSeeds;

const uint NONE = 65535u;

void main()
{
	ivec2 p     = ivec2( gl_FragCoord.xy );
	bool inside = ( texelFetch( Value, p, 0 ).r > Threshold ) != ( Invert != 0 );
	//(nearest outside, nearest inside). Each pixel is its own nearest of
	//its own kind and knows nothing yet of the other.
	fragSeeds = inside ? uvec4( NONE, NONE, uvec2( p ) ) : uvec4( uvec2( p ), NONE, NONE );
}
)";

//---------------------------------------------------------------------------
// 4. flood. One jump-flooding pass at Step. Both seeds travel together: an
// inside pixel's nearest outside seed reaches it through pixels of either
// kind, so every pixel carries both.
//
// Distances are squared integers: exact, and no two candidates are ever
// compared through a rounding. Ties keep the incumbent (strict <), and the
// eight neighbours are visited in a fixed order, so the flood is a pure
// function of the mask on any driver.
//---------------------------------------------------------------------------
const char* const kFloodBody = R"(
uniform usampler2D Seeds;
uniform int Step;
uniform ivec2 Size;

out uvec4 fragSeeds;

const uint NONE = 65535u;
const int FAR   = 0x7fffffff;

int distance2( ivec2 p, uvec2 s )
{
	ivec2 d = ivec2( s ) - p;
	return d.x * d.x + d.y * d.y;
}

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	uvec4 best = texelFetch( Seeds, p, 0 );
	int toOut  = best.x == NONE ? FAR : distance2( p, best.xy );
	int toIn   = best.z == NONE ? FAR : distance2( p, best.zw );

	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			if( dx == 0 && dy == 0 )
				continue;
			ivec2 q = p + ivec2( dx, dy ) * Step;
			if( q.x < 0 || q.y < 0 || q.x >= Size.x || q.y >= Size.y )
				continue;
			uvec4 c = texelFetch( Seeds, q, 0 );
			if( c.x != NONE )
			{
				int d = distance2( p, c.xy );
				if( d < toOut )
				{
					toOut   = d;
					best.xy = c.xy;
				}
			}
			if( c.z != NONE )
			{
				int d = distance2( p, c.zw );
				if( d < toIn )
				{
					toIn    = d;
					best.zw = c.zw;
				}
			}
		}
	}
	fragSeeds = best;
}
)";

//---------------------------------------------------------------------------
// 5. resolve. The signed distance field, in pixels.
//
// Inside: the distance from this pixel's centre to the nearest outside pixel
// centre, less half a pixel, so a straight wall sits half way between the
// last inside centre and the first outside one and d is exactly the distance
// to it. The frame's edge is a wall too: a virtual outside pixel beyond each
// edge, whose distance is an integer and needs no flood. Outside: minus the
// same to the nearest inside centre.
//---------------------------------------------------------------------------
const char* const kResolveBody = R"(
uniform usampler2D Seeds;
uniform ivec2 Size;

out vec4 fragColor;

const uint NONE = 65535u;

float centreDistance( ivec2 p, uvec2 s )
{
	ivec2 d = ivec2( s ) - p;
	return sqrt( float( d.x * d.x + d.y * d.y ) );
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	uvec4 s = texelFetch( Seeds, p, 0 );
	bool inside = s.z != NONE && ivec2( s.zw ) == p;

	float d;
	if( inside )
	{
		int wall      = min( min( p.x + 1, Size.x - p.x ), min( p.y + 1, Size.y - p.y ) );
		float toStock = s.x != NONE ? centreDistance( p, s.xy ) : 1.0e9;
		d = min( toStock, float( wall ) ) - 0.5;
	}
	else
	{
		d = s.z != NONE ? 0.5 - centreDistance( p, s.zw ) : -1.0e6;
	}
	fragColor = vec4( d );
}
)";

//---------------------------------------------------------------------------
// 6. sample. The field at the trace grid's points, bilinear.
//---------------------------------------------------------------------------
const char* const kSampleBody = R"(
uniform sampler2D Field;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = vec4( texture( Field, uv ).r );
}
)";

//---------------------------------------------------------------------------
// 7. stamp. One instance per piece of path, a quad aligned with it and
// Radius + 1.5 px bigger all round, so the antialiased edge is inside it.
//---------------------------------------------------------------------------
const char* const kStampVertexBody = R"(
layout( location = 0 ) in vec2 Corner;//(0 or 1 along, -1 or 1 across)
layout( location = 1 ) in vec4 Piece; //(a.x, a.y, b.x, b.y) in job pixels

uniform vec2 TargetSize;//pixels of the buffer drawn into
uniform vec2 Scale;     //job pixels to target pixels
uniform float Radius;   //target pixels

flat out vec2 pieceA;
flat out vec2 pieceB;

void main()
{
	vec2 a     = Piece.xy * Scale;
	vec2 b     = Piece.zw * Scale;
	vec2 along = b - a;
	float len  = length( along );
	vec2 dir   = len > 1.0e-6 ? along / len : vec2( 1.0, 0.0 );
	vec2 side  = vec2( -dir.y, dir.x );
	float r    = Radius + 1.5;

	vec2 p      = a + dir * ( Corner.x * ( len + 2.0 * r ) - r ) + side * ( Corner.y * r );
	pieceA      = a;
	pieceB      = b;
	gl_Position = vec4( p / TargetSize * 2.0 - 1.0, 0.0, 1.0 );
}
)";

//The tool is a disc, so what it removes along a piece is every pixel within
//Radius of the piece: a capsule. Coverage ramps over one pixel centred on
//Radius, so the 0.5 contour of the cut is exactly Radius from the path.
//
//Perturb bit 1 is the negative control for --fillet: a square tool, whose
//swept shape is the Chebyshev distance and which cuts inside corners sharp.
const char* const kStampFragmentBody = R"(
uniform float Radius;   //target pixels
uniform int Shape;      //0 swept disc, 1 ring of RingWidth
uniform float RingWidth;
uniform vec4 Channel;   //what one unit of coverage writes
uniform int Perturb;

flat in vec2 pieceA;
flat in vec2 pieceB;
out vec4 fragColor;

void main()
{
	vec2 p      = gl_FragCoord.xy;
	vec2 along  = pieceB - pieceA;
	float len2  = dot( along, along );
	float t     = len2 > 0.0 ? clamp( dot( p - pieceA, along ) / len2, 0.0, 1.0 ) : 0.0;
	vec2 offset = p - ( pieceA + t * along );
	float d     = ( Perturb & 1 ) != 0 ? max( abs( offset.x ), abs( offset.y ) ) : length( offset );

	float cover = Shape == 1 ? clamp( 0.5 * RingWidth - abs( d - Radius ) + 0.5, 0.0, 1.0 )
	                         : clamp( Radius - d + 0.5, 0.0, 1.0 );
	fragColor = Channel * cover;
}
)";

//---------------------------------------------------------------------------
// 8. composite.
//---------------------------------------------------------------------------
const char* const kCompositeBody = R"(
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform sampler2D Cut;    //job raster, coverage in r
uniform sampler2D Field;  //job raster, signed distance in job pixels
uniform sampler2D Overlay;//output raster: r cut path, g rapids, b the tool
uniform vec2 JobSize;
uniform vec2 OutSize;

uniform int Mode;         //0 Reveal, 1 Engrave, 2 Paths, 3 Field
uniform int FieldMode;    //0 Glow, 1 Bevel, 2 Outline
uniform vec3 StockColour;
uniform vec3 PathColour;
uniform float Depth;
uniform vec2 LightDir;
uniform float ToolRadius; //job pixels
uniform float Offset;     //Field: Distance, output pixels
uniform float Band;       //Field: Width, output pixels
uniform float Falloff;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

float cutAt( vec2 at )
{
	return texture( Cut, at ).r;
}

//The field in OUTPUT pixels.
float fieldAt( vec2 at )
{
	return texture( Field, at ).r * ( OutSize.y / JobSize.y );
}

//The field's gradient, a unit vector where the field is a distance, by
//central differences `span` job pixels apart. Not one texel: a distance to a
//DIGITISED boundary has a gradient that swings by the staircase of the
//pixels it was measured to, and lit one texel apart that shows as a
//sunburst of hairlines round every curve.
vec2 fieldGradient( vec2 at, float span )
{
	vec2 h   = max( span, 1.0 ) / JobSize;
	float dx = texture( Field, at + vec2( h.x, 0.0 ) ).r - texture( Field, at - vec2( h.x, 0.0 ) ).r;
	float dy = texture( Field, at + vec2( 0.0, h.y ) ).r - texture( Field, at - vec2( 0.0, h.y ) ).r;
	vec2 g   = vec2( dx, dy );
	float m  = length( g );
	return m > 1.0e-4 ? g / m : vec2( 0.0 );
}

//Which way the cut's wall faces, and how steeply: the cut coverage's
//gradient over a third of the tool's radius.
vec2 wallGradient( vec2 at )
{
	vec2 h   = max( 1.0, 0.35 * ToolRadius ) / JobSize;
	float dx = cutAt( at + vec2( h.x, 0.0 ) ) - cutAt( at - vec2( h.x, 0.0 ) );
	float dy = cutAt( at + vec2( 0.0, h.y ) ) - cutAt( at - vec2( 0.0, h.y ) );
	return vec2( dx, dy ) * 0.5;
}

void main()
{
	vec4 clip = texture( InputTexture, uv * MaxUV );
	vec3 rgb;
	float alpha = clip.a;

	if( Mode == 3 )
	{
		float d = fieldAt( uv );
		if( FieldMode == 0 )
		{
			//Glow: light falling away from the region, starting Offset out.
			float t    = max( -d - Offset, 0.0 ) / max( Band, 1.0e-3 );
			float glow = d > -Offset ? 1.0 : mix( exp( -t * t ), exp( -t ), Falloff );
			if( d > 0.0 )
				glow = 0.0;
			rgb   = clip.rgb + PathColour * glow;
			alpha = max( clip.a, glow );
		}
		else if( FieldMode == 1 )
		{
			//Bevel: inside the region, a slope Band wide starting Offset in,
			//lit from LightDir. Falloff bends the profile from a chamfer
			//towards a round-over.
			float u      = clamp( ( d - Offset ) / max( Band, 1.0e-3 ), 0.0, 1.0 );
			float k      = 1.0 + 3.0 * Falloff;
			float slope  = ( d > Offset && d < Offset + Band ) ? k * pow( 1.0 - u, k - 1.0 ) : 0.0;
			float shade  = Depth * 0.5 * slope * dot( fieldGradient( uv, 0.25 * Band * JobSize.y / OutSize.y ), LightDir );
			rgb          = clip.rgb * ( 1.0 + shade ) + vec3( max( shade, 0.0 ) * 0.25 );
		}
		else
		{
			//Outline: a line Band wide, Offset outside the boundary.
			float soft = max( Falloff * 0.5 * Band, 0.5 );
			float a    = clamp( ( 0.5 * Band - abs( -d - Offset ) ) / soft + 0.5, 0.0, 1.0 );
			rgb        = mix( clip.rgb, PathColour, a );
			alpha      = max( clip.a, a );
		}
	}
	else
	{
		float cut  = cutAt( uv );
		vec4 over  = texture( Overlay, uv );
		float wall = dot( wallGradient( uv ), LightDir );

		if( Mode == 0 )
		{
			//Reveal: opaque stock, and the clip where the tool has been.
			rgb   = mix( StockColour, clip.rgb, cut ) + vec3( 1.2 * Depth * wall * cut );
			alpha = mix( 1.0, clip.a, cut );
		}
		else if( Mode == 1 )
		{
			//Engrave: the clip, the cut sunk into it. The floor is shaded by
			//depth; the wall of the cut catches the light or not; and inside
			//the tool's radius of the pocket's own wall the floor rises in a
			//bevel, lit by the distance field's gradient.
			float fromWall = texture( Field, uv ).r;
			float bevel    = cut * ( 1.0 - clamp( fromWall / max( ToolRadius * 2.0, 1.0 ), 0.0, 1.0 ) );
			float lit      = dot( fieldGradient( uv, 0.25 * ToolRadius ), LightDir );
			rgb = clip.rgb * ( 1.0 - 0.45 * Depth * cut ) + vec3( 1.2 * Depth * wall * cut ) + clip.rgb * ( 0.6 * Depth * bevel * lit );
		}
		else
		{
			//Paths: the CAM preview. The clip dimmed, what is cut tinted,
			//the cutting moves solid and the rapids faint.
			rgb = clip.rgb * 0.35 + PathColour * ( 0.18 * cut );
			rgb = mix( rgb, PathColour, over.r );
			rgb = mix( rgb, PathColour * 0.55, 0.7 * over.g );
		}

		rgb = mix( rgb, vec3( 1.0 ), over.b );//the tool
	}

	vec4 effect = vec4( rgb, alpha );
	fragColor   = MixAmount >= 1.0 ? effect : mix( clip, effect, MixAmount );
}
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}
std::string Detect()
{
	return assemble( kDetectBody );
}
std::string Blur()
{
	return assemble( kBlurBody );
}
std::string Seed()
{
	return assemble( kSeedBody );
}
std::string Flood()
{
	return assemble( kFloodBody );
}
std::string Resolve()
{
	return assemble( kResolveBody );
}
std::string Sample()
{
	return assemble( kSampleBody );
}
std::string StampVertex()
{
	return assemble( kStampVertexBody );
}
std::string StampFragment()
{
	return assemble( kStampFragmentBody );
}
std::string Composite()
{
	return assemble( kCompositeBody );
}

} // namespace toolpath::shaders
