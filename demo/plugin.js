/**
 * Toolpath — browser demo.
 *
 * CNC pocketing from a distance field. The one idea, from `AGENTS.md`: a router
 * clears a pocket with a round tool, its centre can only go where the whole disc
 * fits, and the passes that do it are the level sets of the pocket's distance
 * field at r, r + s, r + 2s … So one field is the whole CAM job, and the fillet
 * in every inside corner, the slot too narrow to enter, the scallops left by a
 * wide stepover and the big pocket that takes longer are not drawn — they fall
 * out of the geometry.
 *
 * Like Galvo, this plugin is **not only a shader**. It is a GPU jump flood, a
 * synchronous readback of the field, a CPU tracer and path planner, and GPU
 * stamps and a composite. The CPU middle has to exist here or the page has no
 * toolpath to show, so it is ported — and the two halves are not equally
 * faithful:
 *
 *   The shaders are the plugin's. `VERSION` and the ten `…_BODY` strings below
 *   are `kVersion`, `kVertexBody` … `kCompositeBody` from `source/Shaders.cpp`,
 *   copied across unedited and assembled the way `assemble()` assembles them:
 *   the version line, then the body. `demo/tools/check_shaders.py` compares
 *   every piece character for character and `tools/verify.sh` runs it, because
 *   two copies of a shader is exactly the arrangement that drifts.
 *
 *   The CPU half is a port — of `Controls.cpp`, `Path.cpp` (TraceLevels,
 *   SharpenCorners, SimplifyLoop, Order, PositionAt, CutSpans, AllSegments) and
 *   the parts of `Toolpath.cpp` around them (computeField's flood schedule,
 *   buildPath, stamp, and ProcessOpenGL's Latch and Live), function for
 *   function. Nothing checks a port but a reader. `tptest --march`, `--fillet`,
 *   `--slot`, `--scallop` and `--feed` in the repository check the C++ and have
 *   no idea this page exists.
 *
 * ------------------------------------------------------ what this page reads back
 *
 * The field, at the plugin's own trace grid, every time the plugin would: the
 * job raster up to 1280 wide (960 × 540 on this page's default composition),
 * sampled bilinearly off the two-pixel working lattice by the plugin's own
 * `sample` pass, and read back with a synchronous `readPixels` that stalls the
 * pipeline exactly as the plugin's `glReadPixels` does. In Latch that is once a
 * job and again whenever Tool Diameter, Stepover or Strategy moves; in Live it
 * is every frame, and the stats line under the canvas says what it costs here.
 *
 * The plugin reads that R32F grid as `GL_RED`/`GL_FLOAT`. WebGL2 with
 * EXT_color_buffer_float guarantees only `RGBA`/`FLOAT` for a float
 * framebuffer, so the page asks the driver which it will take and, if it is
 * not RED, reads four floats a sample and keeps the first. The numbers are the
 * same 32-bit floats either way; only the bytes moved differ. (Galvo's page
 * had to drop to RGBA8 because it reads bytes; this one reads floats, which
 * WebGL2 can do.)
 *
 * ------------------------------------------------------- what is missing
 *
 * **No audio caveat.** Toolpath has no audio path and no audio-driven control,
 * so there is nothing here that would have been driven by Resolume's spectrum.
 *
 * **Restart is a toggle, not an event.** FFGL has FF_TYPE_EVENT and the kit's
 * parameter model does not, so the renderer takes the press and releases it
 * itself, exactly as the plugin's SetFloatParameter remembers a press until the
 * next frame. It is why the button blinks.
 *
 * **The clock is the page's.** The plugin votes on whether the host's clock is
 * in seconds or milliseconds (readout's unit voting); the page's clock is in
 * seconds by construction, so that is not ported. The 0.25 s cap on a frame's
 * advance is.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * ------------------------------------------------------- decided, not asked
 *
 * **The clip list starts on the colour bars.** Toolpath is for a clean region
 * with corners in it, and at the default Luma threshold of 0.5 the 75% bars are
 * a large rectangle (the four bright bars) and a small one (the pluge's white
 * patch): two pockets, eight inside corners, and every fillet visible. The
 * repository's own test card (slots, an L, a ring with an island) is not a kit
 * clip, and the kit is not edited here.
 *
 * **Region controls act on a grab, as in the plugin.** In Latch the region is
 * taken once, so moving Threshold, Smooth, Invert or Detect On changes nothing
 * until Restart — which is the plugin's behaviour, not the page's, and the hints
 * say so. Switching the clip is the same: an FFGL effect cannot see a clip
 * change. Live re-grabs every frame.
 *
 * **There is a line of statistics under the canvas** — passes, loops, cut
 * length, how far along the job the tool is, and the CPU cost of the trace. It
 * reports; it measures nothing.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core — so a pixel here
 * is not evidence about a pixel there.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture, GLError } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// Each is `assemble( body )`: the version line, then the body. The one backtick
// inside a comment (`span`, in the composite) is escaped, because a template
// literal has nowhere else to put it; check_shaders.py decodes that one escape
// before comparing and rejects any other backslash, so it cannot hide a
// difference.
//---------------------------------------------------------------------------

const VERSION = `#version 410 core
`;

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const DETECT_BODY = `
uniform sampler2D InputTexture;
uniform ivec2 InputSize;//the input's pixels (the content, not the hardware size)
uniform int Scale;      //input pixels to a texel side
uniform int DetectOn;   //0 luma, 1 alpha, 2 chroma, 3 luma or alpha

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
	ivec2 base = ivec2( gl_FragCoord.xy ) * Scale;
	ivec2 last = InputSize - ivec2( 1 );
	float sum  = 0.0;
	for( int j = 0; j < Scale; ++j )
		for( int i = 0; i < Scale; ++i )
			sum += channel( texelFetch( InputTexture, min( base + ivec2( i, j ), last ), 0 ) );
	fragColor = vec4( sum / float( Scale * Scale ) );
}
`;

const BLUR_BODY = `
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
`;

const SEED_BODY = `
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
`;

const FLOOD_BODY = `
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
`;

const RESOLVE_BODY = `
uniform usampler2D Seeds;
uniform float Scale;  //job pixels to a texel side
uniform vec2 JobSize; //job pixels

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
		vec2 c        = ( vec2( p ) + 0.5 ) * Scale;
		float wall    = min( min( c.x, JobSize.x - c.x ), min( c.y, JobSize.y - c.y ) );
		float toStock = s.x != NONE ? ( centreDistance( p, s.xy ) - 0.5 ) * Scale : 1.0e9;
		d = min( toStock, wall );
	}
	else
	{
		d = s.z != NONE ? ( 0.5 - centreDistance( p, s.zw ) ) * Scale : -1.0e6;
	}
	fragColor = vec4( d );
}
`;

const SAMPLE_BODY = `
uniform sampler2D Field;
uniform vec2 FieldUV;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = vec4( texture( Field, uv * FieldUV ).r );
}
`;

const STAMP_VERTEX_BODY = `
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
`;

const STAMP_FRAGMENT_BODY = `
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
`;

const COMPOSITE_BODY = `
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform sampler2D Cut;    //job raster, coverage in r
uniform sampler2D Field;  //working lattice, signed distance in job pixels
uniform vec2 FieldUV;     //job 0..1 to the lattice's 0..1
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

//The field in job pixels, at a point of the job.
float fieldJob( vec2 at )
{
	return texture( Field, at * FieldUV ).r;
}

//The field in OUTPUT pixels.
float fieldAt( vec2 at )
{
	return fieldJob( at ) * ( OutSize.y / JobSize.y );
}

//The field's gradient, a unit vector where the field is a distance, by
//central differences \`span\` job pixels apart. Not one texel: a distance to a
//DIGITISED boundary has a gradient that swings by the staircase of the
//pixels it was measured to, and lit one texel apart that shows as a
//sunburst of hairlines round every curve.
vec2 fieldGradient( vec2 at, float span )
{
	vec2 h   = max( span, 1.0 ) / JobSize;
	float dx = fieldJob( at + vec2( h.x, 0.0 ) ) - fieldJob( at - vec2( h.x, 0.0 ) );
	float dy = fieldJob( at + vec2( 0.0, h.y ) ) - fieldJob( at - vec2( 0.0, h.y ) );
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
			float fromWall = fieldJob( uv );
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
`;

/// `assemble()` in Shaders.cpp: the version line, then the body.
const assemble = (body) => `${VERSION}${body}`;

const VERTEX = assemble(VERTEX_BODY);
const DETECT = assemble(DETECT_BODY);
const BLUR = assemble(BLUR_BODY);
const SEED = assemble(SEED_BODY);
const FLOOD = assemble(FLOOD_BODY);
const RESOLVE = assemble(RESOLVE_BODY);
const SAMPLE = assemble(SAMPLE_BODY);
const STAMP_VERTEX = assemble(STAMP_VERTEX_BODY);
const STAMP_FRAGMENT = assemble(STAMP_FRAGMENT_BODY);
const COMPOSITE = assemble(COMPOSITE_BODY);

//===========================================================================
// A port of source/Controls.cpp.
//
// Every slider is a plain 0..1 float, because SetParamInfo clamps a STANDARD
// default into 0..1 before a range can be attached. These say what a slider
// position means. The plugin computes them in `float`, so they are rounded to
// float here with Math.fround where the C++ would round; this is a third copy,
// and only a reader is checking it.
//===========================================================================

const f32 = Math.fround;
const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const kPi = f32(3.14159265358979);

const SmoothSigmaHeights = (v) => f32(0.01 * clamp01(v));
const ToolDiameterHeights = (v) => f32(0.01 * f32(Math.pow(25.0, clamp01(v))));
const ToolDiameterParam = (h) => f32(clamp01(Math.log(Math.max(h, 0.01) / 0.01) / Math.log(25.0)));
const StepoverDiameters = (v) => f32(0.1 + 1.9 * clamp01(v));
const StepoverParam = (d) => f32(clamp01((d - 0.1) / 1.9));
const FeedHeightsPerSecond = (v) => f32(0.05 * f32(Math.pow(160.0, clamp01(v))));
const FeedParam = (h) => f32(clamp01(Math.log(Math.max(h, 0.05) / 0.05) / Math.log(160.0)));
const LightAngleRadians = (v) => f32(2.0 * kPi * clamp01(v));
const DistanceHeights = (v) => f32(0.25 * clamp01(v));
const WidthHeights = (v) => f32(0.002 * f32(Math.pow(50.0, clamp01(v))));
const WidthParam = (h) => f32(clamp01(Math.log(Math.max(h, 0.002) / 0.002) / Math.log(50.0)));

const DETECT_NAMES = ['Luma', 'Alpha', 'Chroma', 'Luma or Alpha'];
const STRATEGY_NAMES = ['Outside In', 'Inside Out'];
const GEOMETRY_NAMES = ['Latch', 'Live'];
const MODE_NAMES = ['Reveal', 'Engrave', 'Paths', 'Field'];
const FIELD_MODE_NAMES = ['Glow', 'Bevel', 'Outline'];

const kInsideOut = 1;
const kLatch = 0;
const kLive = 1;
const kPaths = 2;
const kField = 3;

/// `OptionIndex`: an option's value to its index, rounded and clamped.
const OptionIndex = (value, count) => clamp(Math.round(value), 0, count - 1);

//===========================================================================
// A port of source/Path.cpp — from a sampled distance field to an ordered
// toolpath, and where the tool is on it. No GL in it, in either language.
//
// Coordinates are JOB pixels, GL convention: x right, y UP, pixel (i, j)'s
// centre at (i + 0.5, j + 0.5).
//===========================================================================

/// SharpenCorners' thresholds: turns over 45 degrees, runs straight to 5.
const kCornerCosine = 0.7071067811865476;
const kStraightCosine = 0.9961946980917455;
const kCornerReach = 2;

/// Rapids at four times the feed, cutting nothing.
const kRapidFactor = 4.0;

const distance = (a, b) => Math.hypot(b.x - a.x, b.y - a.y);

function distanceToSegment(p, a, b) {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const len2 = dx * dx + dy * dy;
  let t = len2 > 0.0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
  t = clamp(t, 0.0, 1.0);
  return Math.hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

/**
 * `TraceLevels`: marching squares over a `tw` x `th` grid of samples (row 0 at
 * the BOTTOM), every level in one pass over the cells. Loops shorter than three
 * points are dropped.
 */
function TraceLevels(grid, tw, th, cellW, cellH, levels) {
  const loops = [];
  if (!grid || tw <= 0 || th <= 0 || levels.count <= 0 || !(levels.step > 0.0)) return loops;

  // The virtual ring half a cell outside the frame, which is the wall.
  const ring = -0.5 * Math.max(cellW, cellH);
  const pw = tw + 2;
  const value = (i, j) => (i < 0 || j < 0 || i >= tw || j >= th ? ring : grid[j * tw + i]);
  const position = (i, j) => ({ x: (i + 0.5) * cellW, y: (j + 0.5) * cellH });

  // Edge keys: (level << 34) | (vertical << 33) | padded index. A uint64 in
  // the C++; here a double, exact because every term is under 2^53 (4096
  // levels is 2^46).
  const key = (level, vertical, i, j) => level * 17179869184 + vertical * 8589934592 + ((j + 1) * pw + (i + 1));

  // The crossing on an edge, always interpolated from its lower corner.
  const crossing = (level, vertical, i, j) => {
    const i1 = vertical ? i : i + 1;
    const j1 = vertical ? j + 1 : j;
    const a = value(i, j);
    const b = value(i1, j1);
    let t = b !== a ? (level - a) / (b - a) : 0.5;
    t = clamp(t, 0.0, 1.0);
    const p0 = position(i, j);
    const p1 = position(i1, j1);
    return { x: p0.x + t * (p1.x - p0.x), y: p0.y + t * (p1.y - p0.y) };
  };

  const segStart = [];
  const segEnd = [];
  const segPoint = [];
  const segLevel = [];

  const v = [0, 0, 0, 0];
  const inn = [false, false, false, false];
  const edges = [
    { vertical: 0, i: 0, j: 0 },
    { vertical: 1, i: 0, j: 0 },
    { vertical: 0, i: 0, j: 0 },
    { vertical: 1, i: 0, j: 0 },
  ];

  for (let j = -1; j < th; j += 1) {
    for (let i = -1; i < tw; i += 1) {
      // Corners anticlockwise from the bottom left.
      v[0] = value(i, j);
      v[1] = value(i + 1, j);
      v[2] = value(i + 1, j + 1);
      v[3] = value(i, j + 1);
      const lo = Math.min(v[0], v[1], v[2], v[3]);
      const hi = Math.max(v[0], v[1], v[2], v[3]);
      if (hi < levels.first) continue;

      let k0 = Math.floor((lo - levels.first) / levels.step);
      let k1 = Math.floor((hi - levels.first) / levels.step) + 1;
      k0 = Math.max(k0, 0);
      k1 = Math.min(k1, levels.count - 1);

      for (let k = k0; k <= k1; k += 1) {
        const level = levels.first + k * levels.step;
        let inside = 0;
        for (let n = 0; n < 4; n += 1) {
          inn[n] = v[n] >= level;
          inside += inn[n] ? 1 : 0;
        }
        if (inside === 0 || inside === 4) continue;

        // bottom, right, top, left — the canonical grid edges.
        edges[0].i = i; edges[0].j = j;
        edges[1].i = i + 1; edges[1].j = j;
        edges[2].i = i; edges[2].j = j + 1;
        edges[3].i = i; edges[3].j = j;

        const centreInside = (v[0] + v[1] + v[2] + v[3]) * 0.25 >= level;
        for (let n = 0; n < 4; n += 1) {
          if (!(inn[n] && !inn[(n + 1) % 4])) continue;
          let m;
          if (inside === 2 && inn[(n + 2) % 4]) {
            m = centreInside ? (n + 1) % 4 : (n + 3) % 4; // saddle
          } else {
            m = -1;
            for (let e = 0; e < 4; e += 1) if (!inn[e] && inn[(e + 1) % 4]) m = e;
          }
          if (m < 0) continue;

          segLevel.push(k);
          segStart.push(key(k, edges[n].vertical, edges[n].i, edges[n].j));
          segEnd.push(key(k, edges[m].vertical, edges[m].i, edges[m].j));
          segPoint.push(crossing(level, edges[n].vertical, edges[n].i, edges[n].j));
        }
      }
    }
  }

  // Chain. `emplace` keeps the FIRST segment with a given start, so this does.
  const byStart = new Map();
  for (let n = 0; n < segStart.length; n += 1) {
    if (!byStart.has(segStart[n])) byStart.set(segStart[n], n);
  }

  const used = new Uint8Array(segStart.length);
  for (let n = 0; n < segStart.length; n += 1) {
    if (used[n]) continue;
    const loop = { points: [], level: segLevel[n] };
    let at = n;
    while (!used[at]) {
      used[at] = 1;
      loop.points.push(segPoint[at]);
      const next = byStart.get(segEnd[at]);
      if (next === undefined) break;
      at = next;
    }
    if (loop.points.length >= 3) {
      SharpenCorners(loop, Math.max(cellW, cellH), levels.fieldTexel);
      loops.push(loop);
    }
  }

  return loops;
}

/**
 * `SharpenCorners`: where a loop runs straight, turns by more than 45 degrees
 * and runs straight again, the points between are replaced by the meeting
 * point of the two straight runs. Reach is kCornerReach of whichever is
 * coarser, the cell or the field's texel.
 */
function SharpenCorners(loop, cell, fieldTexel = 0.0) {
  const reach = kCornerReach * Math.max(1, Math.ceil(fieldTexel / cell - 1e-9));
  const n = loop.points.length;
  if (n < 4 * (reach + 2)) return;
  const P = (i) => loop.points[((i % n) + n) % n];
  const unit = (a, b) => {
    const l = Math.hypot(b.x - a.x, b.y - a.y);
    if (l <= 1e-12) return null;
    return { x: (b.x - a.x) / l, y: (b.y - a.y) / l };
  };
  const dot = (a, b) => a.x * b.x + a.y * b.y;

  const turn = new Float64Array(n);
  for (let i = 0; i < n; i += 1) {
    const u = unit(P(i - 1), P(i));
    const w = u ? unit(P(i), P(i + 1)) : null;
    if (u && w) turn[i] = 1.0 - dot(u, w);
  }

  const corners = [];
  for (let i = 0; i < n; i += 1) {
    let peak = true;
    for (let k = -reach; k <= reach && peak; k += 1) {
      if (k !== 0 && turn[(((i + k) % n) + n) % n] > turn[i]) peak = false;
    }
    if (!peak || turn[i] <= 1e-12) continue;

    const a0 = P(i - reach - 2);
    const a1 = P(i - reach - 1);
    const a2 = P(i - reach);
    const b0 = P(i + reach);
    const b1 = P(i + reach + 1);
    const b2 = P(i + reach + 2);
    const ua = unit(a0, a1);
    const ua2 = ua ? unit(a1, a2) : null;
    const ub = ua2 ? unit(b0, b1) : null;
    const ub2 = ub ? unit(b1, b2) : null;
    if (!ua || !ua2 || !ub || !ub2) continue;
    if (dot(ua, ua2) < kStraightCosine || dot(ub, ub2) < kStraightCosine) continue; // a curve
    if (dot(ua2, ub) > kCornerCosine) continue; // not turning enough

    // a2 + s ua2 = b0 + t ub.
    const det = ua2.x * (-ub.y) - ua2.y * (-ub.x);
    if (Math.abs(det) < 1e-12) continue;
    const rx = b0.x - a2.x;
    const ry = b0.y - a2.y;
    const s = (rx * (-ub.y) - ry * (-ub.x)) / det;
    const x = { x: a2.x + s * ua2.x, y: a2.y + s * ua2.y };

    let near = true;
    for (let k = -reach + 1; k <= reach - 1; k += 1) {
      if (Math.hypot(P(i + k).x - x.x, P(i + k).y - x.y) > (reach + 0.5) * cell) near = false;
    }
    if (near && (corners.length === 0 || i - corners[corners.length - 1].at > 2 * reach)) {
      corners.push({ at: i, x });
    }
  }
  if (corners.length === 0) return;

  const drop = new Uint8Array(n);
  const cornerAt = new Int32Array(n).fill(-1);
  for (let c = 0; c < corners.length; c += 1) {
    for (let k = -reach + 1; k <= reach - 1; k += 1) drop[(((corners[c].at + k) % n) + n) % n] = 1;
    cornerAt[corners[c].at] = c;
  }
  const rebuilt = [];
  for (let i = 0; i < n; i += 1) {
    if (cornerAt[i] >= 0) rebuilt.push(corners[cornerAt[i]].x);
    else if (!drop[i]) rebuilt.push(loop.points[i]);
  }
  if (rebuilt.length >= 3) loop.points = rebuilt;
}

/** `SimplifyLoop`: Douglas-Peucker on a closed loop, in place. */
function SimplifyLoop(loop, tolerance) {
  const n = loop.points.length;
  if (n < 4 || !(tolerance > 0.0)) return;

  let far = 0;
  let farDistance = -1.0;
  for (let i = 1; i < n; i += 1) {
    const d = distance(loop.points[0], loop.points[i]);
    if (d > farDistance) {
      farDistance = d;
      far = i;
    }
  }

  const keep = new Uint8Array(n);
  keep[0] = 1;
  keep[far] = 1;

  const stack = [[0, far], [far, n]];
  const at = (i) => loop.points[i % n];
  while (stack.length) {
    const [a, b] = stack.pop();
    if (b <= a + 1) continue;
    let worst = -1.0;
    let worstI = a;
    for (let i = a + 1; i < b; i += 1) {
      const d = distanceToSegment(at(i), at(a), at(b));
      if (d > worst) {
        worst = d;
        worstI = i;
      }
    }
    if (worst > tolerance) {
      keep[worstI % n] = 1;
      stack.push([a, worstI]);
      stack.push([worstI, b]);
    }
  }

  const kept = [];
  for (let i = 0; i < n; i += 1) if (keep[i]) kept.push(loop.points[i]);
  if (kept.length >= 3) loop.points = kept;
}

const emptyPath = () => ({ points: [], cut: [], tau: [], cutLength: 0.0, loops: 0 });
const pathEmpty = (path) => path.points.length < 2;
const pathDuration = (path) => (path.tau.length ? path.tau[path.tau.length - 1] : 0.0);

/**
 * `Order`: the loops as a forest (a loop's parent is the loop one level out it
 * lies nearest to), walked one pocket at a time, nearest first. Outside In cuts
 * a loop before its children, Inside Out after them.
 */
function Order(loops, levelCount, insideOut, start) {
  const path = emptyPath();
  const n = loops.length;

  const byLevel = Array.from({ length: Math.max(levelCount, 0) }, () => []);
  for (let i = 0; i < n; i += 1) {
    if (loops[i].level >= 0 && loops[i].level < levelCount) byLevel[loops[i].level].push(i);
  }

  const children = Array.from({ length: n }, () => []);
  const roots = [];
  for (let level = 0; level < byLevel.length; level += 1) {
    for (const i of byLevel[level]) {
      if (level === 0 || byLevel[level - 1].length === 0) {
        roots.push(i);
        continue;
      }
      const outer = byLevel[level - 1];
      let vertices = 0;
      for (const o of outer) vertices += loops[o].points.length;
      const everyVertex = vertices * byLevel[level].length < 4e7;

      const p = loops[i].points[0];
      let best = outer[0];
      let bestD2 = Infinity;
      for (const o of outer) {
        const pts = loops[o].points;
        const count = everyVertex ? pts.length : 1;
        for (let q = 0; q < count; q += 1) {
          const dx = pts[q].x - p.x;
          const dy = pts[q].y - p.y;
          const d2 = dx * dx + dy * dy;
          if (d2 < bestD2) {
            bestD2 = d2;
            best = o;
          }
        }
      }
      children[best].push(i);
    }
  }

  let cursor = start;
  const append = (p, cut) => {
    if (path.points.length === 0) {
      path.points.push(p);
      path.tau.push(0.0);
      return;
    }
    const length = distance(path.points[path.points.length - 1], p);
    if (length <= 1e-9) return;
    path.points.push(p);
    path.cut.push(cut ? 1 : 0);
    path.tau.push(path.tau[path.tau.length - 1] + (cut ? length : length / kRapidFactor));
    if (cut) path.cutLength += length;
  };

  // Cut one loop, entered at its vertex nearest the tool.
  const cutLoop = (i) => {
    const pts = loops[i].points;
    let entry = 0;
    let bestD2 = Infinity;
    for (let q = 0; q < pts.length; q += 1) {
      const dx = pts[q].x - cursor.x;
      const dy = pts[q].y - cursor.y;
      const d2 = dx * dx + dy * dy;
      if (d2 < bestD2) {
        bestD2 = d2;
        entry = q;
      }
    }
    append(pts[entry], false);
    for (let s = 1; s <= pts.length; s += 1) append(pts[(entry + s) % pts.length], true);
    cursor = pts[entry];
    path.loops += 1;
  };

  // The nearest of a set to the tool, by first point; swap-removed, as there.
  const takeNearest = (set) => {
    let best = 0;
    let bestD2 = Infinity;
    for (let q = 0; q < set.length; q += 1) {
      const p = loops[set[q]].points[0];
      const d2 = (p.x - cursor.x) * (p.x - cursor.x) + (p.y - cursor.y) * (p.y - cursor.y);
      if (d2 < bestD2) {
        bestD2 = d2;
        best = q;
      }
    }
    const chosen = set[best];
    set[best] = set[set.length - 1];
    set.pop();
    return chosen;
  };

  while (roots.length) {
    const stack = [];
    const root = takeNearest(roots);
    if (!insideOut) cutLoop(root);
    stack.push({ loop: root, pending: children[root].slice() });
    while (stack.length) {
      const top = stack[stack.length - 1];
      if (top.pending.length === 0) {
        if (insideOut) cutLoop(top.loop);
        stack.pop();
        continue;
      }
      const next = takeNearest(top.pending);
      if (!insideOut) cutLoop(next);
      stack.push({ loop: next, pending: children[next].slice() });
    }
  }

  return path;
}

/// The segment containing tau: tau[i] <= tau < tau[i + 1] (std::upper_bound).
function segmentAt(path, tau) {
  let lo = 0;
  let hi = path.tau.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (path.tau[mid] > tau) hi = mid;
    else lo = mid + 1;
  }
  const i = Math.max(0, lo - 1);
  return Math.min(i, path.points.length - 2);
}

function lerpOn(path, i, tau) {
  const t0 = path.tau[i];
  const t1 = path.tau[i + 1];
  const u = t1 > t0 ? clamp((tau - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
  const a = path.points[i];
  const b = path.points[i + 1];
  return { x: a.x + u * (b.x - a.x), y: a.y + u * (b.y - a.y) };
}

/** `PositionAt`: the tool's position at timeline tau, clamped to the job. */
function PositionAt(path, tau) {
  if (path.points.length === 0) return { x: 0, y: 0 };
  if (path.points.length === 1 || tau <= 0.0) return path.points[0];
  if (tau >= pathDuration(path)) return path.points[path.points.length - 1];
  return lerpOn(path, segmentAt(path, tau), tau);
}

/** `CutSpans`: every piece of CUT segment swept between tau0 and tau1. */
function CutSpans(path, tau0, tau1, out) {
  if (pathEmpty(path) || !(tau1 > tau0)) return;
  tau0 = Math.max(tau0, 0.0);
  tau1 = Math.min(tau1, pathDuration(path));
  if (!(tau1 > tau0)) return;
  for (let i = segmentAt(path, tau0); i + 1 < path.points.length && path.tau[i] < tau1; i += 1) {
    if (!path.cut[i]) continue;
    const a = Math.max(tau0, path.tau[i]);
    const b = Math.min(tau1, path.tau[i + 1]);
    if (!(b > a)) continue;
    out.push({ a: lerpOn(path, i, a), b: lerpOn(path, i, b) });
  }
}

/** `AllSegments`: every segment of the job, cut and rapid, for the preview. */
function AllSegments(path, cuts, rapids) {
  for (let i = 0; i + 1 < path.points.length; i += 1) {
    (path.cut[i] ? cuts : rapids).push({ a: path.points[i], b: path.points[i + 1] });
  }
}

//===========================================================================
// A port of the rest of source/Toolpath.cpp: the constants, computeField's
// flood schedule, buildPath, stamp, and ProcessOpenGL.
//===========================================================================

/// Toolpath.h.
const kFieldScale = 2;
const kTraceMaxWidth = 1280;
const kSimplify = 0.2;
const kMaxLevels = 4096;

/// Toolpath.cpp.
const kMaxFrameDelta = 0.25;
const kLiveHoldSeconds = 1.0;

/// What the stats line under the canvas reports. Filled by the renderer.
const telemetry = {
  job: '',
  lattice: '',
  grid: '',
  floodPasses: 0,
  levels: 0,
  loops: 0,
  cutLength: 0,
  duration: 0,
  feed: 0,
  tau: 0,
  traceMillis: 0,
  machine: true,
  live: false,
  empty: true,
};

/**
 * computeField's pass list, verbatim in shape: a step-1 prepass (the "1+" of
 * 1+JFA), the halving sequence from the largest power of two under the longer
 * side, then a finish from 1/128 of it (at least 2, 1).
 */
function floodSteps(fw, fh) {
  const steps = [1];
  let longest = 1;
  while (longest < Math.max(fw, fh)) longest *= 2;
  for (let step = Math.floor(longest / 2); step >= 1; step = Math.floor(step / 2)) steps.push(step);
  for (let step = Math.max(2, Math.floor(longest / 128)); step >= 1; step = Math.floor(step / 2)) steps.push(step);
  return steps;
}

function createRenderer(gl, quad) {
  // The field is R32F and the sample and composite passes read it bilinearly.
  // Without this extension a float texture with LINEAR filtering is incomplete
  // and samples as zero — a field that is flat everywhere, and no toolpath at
  // all, which would look like a page with nothing over the threshold.
  if (!gl.getExtension('OES_texture_float_linear')) {
    throw new GLError('OES_texture_float_linear is missing. Toolpath samples its 32-bit distance field bilinearly, and without the extension every sample reads zero — a flat field and no toolpath, rather than an obvious failure.');
  }

  const detectShader = new Program(gl, VERTEX, DETECT, 'detect');
  const blurShader = new Program(gl, VERTEX, BLUR, 'blur');
  const seedShader = new Program(gl, VERTEX, SEED, 'seed');
  const floodShader = new Program(gl, VERTEX, FLOOD, 'flood');
  const resolveShader = new Program(gl, VERTEX, RESOLVE, 'resolve');
  const sampleShader = new Program(gl, VERTEX, SAMPLE, 'sample');
  // The stamp sources its own geometry: a unit quad at location 0 and one
  // piece of path per instance at location 1.
  const stampShader = new Program(gl, STAMP_VERTEX, STAMP_FRAGMENT, 'stamp', {
    attribs: { Corner: 0, Piece: 1 },
  });
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');

  // The ivec2 uniforms. The kit's set() has no integer-vector overload, and a
  // float upload to an ivec2 is refused, so these go straight to GL — the
  // plugin calls glUniform2i for the same three.
  const set2i = (program, name, x, y) => {
    const loc = program.location(name);
    if (loc !== null) gl.uniform2i(loc, x, y);
  };

  //-----------------------------------------------------------------------
  // InitGL: the stamp's geometry. A unit quad (along 0..1, across -1..1)
  // shared by every instance, and a stream of pieces, one vec4 per instance.
  // vertexAttribDivisor is VAO state, so it is set with this VAO bound.
  //-----------------------------------------------------------------------
  const corners = new Float32Array([0.0, -1.0, 1.0, -1.0, 0.0, 1.0, 1.0, 1.0]);
  const stampVAO = gl.createVertexArray();
  const cornerBuffer = gl.createBuffer();
  const pieceBuffer = gl.createBuffer();
  gl.bindVertexArray(stampVAO);
  gl.bindBuffer(gl.ARRAY_BUFFER, cornerBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, corners, gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, pieceBuffer);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
  gl.vertexAttribDivisor(1, 1);
  gl.bindVertexArray(null);
  gl.bindBuffer(gl.ARRAY_BUFFER, null);

  // PassBuffer's formats and sampling, as the plugin allocates them.
  const value = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
  const seeds = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
  const field = new PassBuffer(gl, { filter: 'linear' });
  const grid = new PassBuffer(gl, { filter: 'nearest' });
  const cut = new PassBuffer(gl, { filter: 'linear' });
  const overlay = new PassBuffer(gl, { filter: 'linear' });

  let gridPixels = new Float32Array(0);
  let gridRGBA = new Float32Array(0);
  let path = emptyPath();
  let pathValid = false;
  let pathRadius = -1.0;
  let pathStep = -1.0;
  let pathInsideOut = false;
  let levelCount = 0;

  let jobWidth = 0;
  let jobHeight = 0;
  let fieldScale = kFieldScale;
  let captured = false;
  let lastWidth = 0;
  let lastHeight = 0;
  let lastGeometry = -1;
  let tau = 0.0;
  let liveStart = -1.0;
  let restartPending = false;
  let lastNow = -1.0;

  /// The job's 0..1 on the field's lattice (fieldUVx / fieldUVy).
  const fieldUVx = () => (field.width > 0 ? jobWidth / (fieldScale * field.width) : 1.0);
  const fieldUVy = () => (field.height > 0 ? jobHeight / (fieldScale * field.height) : 1.0);

  //-----------------------------------------------------------------------
  // computeField: detect, blur, seed, flood, resolve, on the lattice.
  //-----------------------------------------------------------------------
  function computeField(input, width, height, p) {
    const k = kFieldScale;
    const fw = Math.floor((width + k - 1) / k);
    const fh = Math.floor((height + k - 1) / k);
    value[0].ensure(fw, fh, gl.R16F);
    value[1].ensure(fw, fh, gl.R16F);
    seeds[0].ensure(fw, fh, gl.RGBA16UI);
    seeds[1].ensure(fw, fh, gl.RGBA16UI);
    field.ensure(fw, fh, gl.R32F);
    jobWidth = width;
    jobHeight = height;
    fieldScale = k;

    gl.disable(gl.BLEND);

    // 1. detect, a mean over each k x k block
    value[0].bind();
    detectShader.use();
    bindTexture(gl, 0, input.texture);
    detectShader.setSampler('InputTexture', 0);
    set2i(detectShader, 'InputSize', width, height);
    detectShader.setInt('Scale', k);
    detectShader.setInt('DetectOn', OptionIndex(p('detectOn'), DETECT_NAMES.length));
    quad.draw();

    // 2. blur, x then y, back into value[0]; sigma in texels
    const sigma = f32(f32(SmoothSigmaHeights(p('smooth')) * height) / k);
    if (sigma >= f32(0.3)) {
      const taps = Math.min(48, Math.ceil(f32(3.0 * sigma)));
      blurShader.use();
      blurShader.setSampler('Value', 0);
      blurShader.set('Sigma', sigma);
      blurShader.setInt('Taps', taps);
      for (let axis = 0; axis < 2; axis += 1) {
        value[1 - axis].bind();
        bindTexture(gl, 0, value[axis].texture);
        set2i(blurShader, 'Axis', axis === 0 ? 1 : 0, axis === 0 ? 0 : 1);
        quad.draw();
      }
    }

    // 3. seed
    seeds[0].bind();
    seedShader.use();
    bindTexture(gl, 0, value[0].texture);
    seedShader.setSampler('Value', 0);
    seedShader.set('Threshold', clamp(p('threshold'), 0.0, 1.0));
    seedShader.setInt('Invert', p('invert') > 0.5 ? 1 : 0);
    quad.draw();

    // 4. flood, ping-ponged through the plugin's own schedule
    const steps = floodSteps(fw, fh);
    floodShader.use();
    floodShader.setSampler('Seeds', 0);
    set2i(floodShader, 'Size', fw, fh);
    let current = 0;
    for (const step of steps) {
      seeds[1 - current].bind();
      bindTexture(gl, 0, seeds[current].texture);
      floodShader.setInt('Step', step);
      quad.draw();
      current = 1 - current;
    }
    telemetry.floodPasses = steps.length;

    // 5. resolve
    field.bind();
    resolveShader.use();
    bindTexture(gl, 0, seeds[current].texture);
    resolveShader.setSampler('Seeds', 0);
    resolveShader.set('Scale', k);
    resolveShader.set('JobSize', width, height);
    quad.draw();

    telemetry.job = `${width} × ${height}`;
    telemetry.lattice = `${fw} × ${fh}`;
  }

  //-----------------------------------------------------------------------
  // buildPath: sample the job field onto the trace grid, read it back, trace
  // the offset levels, simplify, order.
  //-----------------------------------------------------------------------
  function buildPath(radius, stepover, insideOut) {
    pathValid = true;
    pathRadius = radius;
    pathStep = stepover;
    pathInsideOut = insideOut;
    path = emptyPath();
    levelCount = 0;

    if (!field.texture || jobWidth <= 0 || jobHeight <= 0) return;

    const start = performance.now();

    const tw = Math.min(jobWidth, kTraceMaxWidth);
    const th = Math.max(1, Math.round((jobHeight * tw) / jobWidth));
    if (gridPixels.length !== tw * th) gridPixels = new Float32Array(tw * th);

    let source;
    if (tw === field.width && th === field.height && jobWidth === fieldScale * tw && jobHeight === fieldScale * th) {
      source = field;
    } else {
      grid.ensure(tw, th, gl.R32F);
      grid.bind();
      gl.disable(gl.BLEND);
      sampleShader.use();
      bindTexture(gl, 0, field.texture);
      sampleShader.setSampler('Field', 0);
      sampleShader.set('FieldUV', fieldUVx(), fieldUVy());
      quad.draw();
      source = grid;
    }

    // The readback: the pipeline stall the plugin's notes count in its trace
    // time. The plugin reads GL_RED/GL_FLOAT; WebGL2 guarantees RGBA/FLOAT for
    // a float framebuffer and offers one more format of the driver's choosing,
    // so RED is used when the driver offers it and RGBA otherwise.
    gl.bindFramebuffer(gl.FRAMEBUFFER, source.fbo);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
    const format = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT);
    const type = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE);
    if (format === gl.RED && type === gl.FLOAT) {
      gl.readPixels(0, 0, tw, th, gl.RED, gl.FLOAT, gridPixels);
      telemetry.readFormat = 'RED/FLOAT';
    } else {
      if (gridRGBA.length !== tw * th * 4) gridRGBA = new Float32Array(tw * th * 4);
      gl.readPixels(0, 0, tw, th, gl.RGBA, gl.FLOAT, gridRGBA);
      for (let i = 0; i < tw * th; i += 1) gridPixels[i] = gridRGBA[i * 4];
      telemetry.readFormat = 'RGBA/FLOAT';
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    let highest = -1.0e30;
    for (let i = 0; i < gridPixels.length; i += 1) if (gridPixels[i] > highest) highest = gridPixels[i];

    const levels = {
      first: radius,
      step: Math.max(stepover, 0.25),
      fieldTexel: fieldScale,
      count: 0,
    };
    levels.count = highest >= levels.first
      ? Math.min(kMaxLevels, Math.floor((highest - levels.first) / levels.step) + 1)
      : 0;
    levelCount = levels.count;

    const cellW = jobWidth / tw;
    const cellH = jobHeight / th;
    const loops = TraceLevels(gridPixels, tw, th, cellW, cellH, levels);
    for (const loop of loops) SimplifyLoop(loop, kSimplify);
    path = Order(loops, levels.count, insideOut, { x: 0.0, y: 0.0 });

    telemetry.traceMillis = performance.now() - start;
    // The exact input the port traced, kept by reference for the cross-check
    // against Path.cpp (AGENTS.md, "The browser demo"). Nothing on the page reads it.
    telemetry.lastGrid = { pixels: gridPixels, tw, th, cellW, cellH, levels, insideOut };
    telemetry.grid = `${tw} × ${th}`;
    telemetry.levels = levels.count;
    telemetry.loops = path.loops;
    telemetry.cutLength = path.cutLength;
    telemetry.duration = pathDuration(path);
  }

  //-----------------------------------------------------------------------
  // stamp: instanced capsules, MAX-blended.
  //-----------------------------------------------------------------------
  function stamp(target, pieces, scaleX, scaleY, radius, shape, ringWidth, channel) {
    if (pieces.length === 0) return;

    target.bind();
    stampShader.use();
    stampShader.set('TargetSize', target.width, target.height);
    stampShader.set('Scale', scaleX, scaleY);
    stampShader.set('Radius', radius);
    stampShader.setInt('Shape', shape);
    stampShader.set('RingWidth', ringWidth);
    stampShader.set('Channel', channel[0], channel[1], channel[2], channel[3]);
    stampShader.setInt('Perturb', 0);

    const data = new Float32Array(pieces.length * 4);
    for (let i = 0; i < pieces.length; i += 1) {
      const s = pieces[i];
      data[i * 4] = s.a.x;
      data[i * 4 + 1] = s.a.y;
      data[i * 4 + 2] = s.b.x;
      data[i * 4 + 3] = s.b.y;
    }

    gl.bindVertexArray(stampVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, pieceBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW);

    // MAX, not ADD: a pixel the tool passes twice is no more cut than one it
    // passes once, and the 0.5 contour must stay exactly Radius from the path.
    gl.enable(gl.BLEND);
    gl.blendEquation(gl.MAX);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, pieces.length);
    gl.blendEquation(gl.FUNC_ADD);
    gl.disable(gl.BLEND);

    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.bindVertexArray(null);
  }

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // The clock. The page's is seconds already; only the difference is
      // read, clamped to a quarter second, as there.
      //------------------------------------------------------------------
      const now = time;
      let dt = 0.0;
      if (lastNow >= 0.0) dt = clamp(now - lastNow, 0.0, kMaxFrameDelta);
      lastNow = now;

      // Restart is FF_TYPE_EVENT in the plugin and a toggle here: take the
      // press and release it, as SetFloatParameter remembers one until the
      // next frame.
      if (p('restart') > 0.5) {
        restartPending = true;
        params.set('restart', 0);
      }

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const geometry = OptionIndex(p('geometry'), GEOMETRY_NAMES.length);
      const mode = OptionIndex(p('mode'), MODE_NAMES.length);
      const machine = mode !== kField;
      const insideOut = OptionIndex(p('strategy'), STRATEGY_NAMES.length) === kInsideOut;
      const showTool = p('showTool') > 0.5;

      const resized = lastWidth !== 0 && (lastWidth !== width || lastHeight !== height);
      lastWidth = width;
      lastHeight = height;

      const restart = restartPending;
      restartPending = false;

      //------------------------------------------------------------------
      // The region. Live grabs it every frame; Latch at the first frame, on
      // Restart, and on switching to Latch. A resize does not re-grab.
      //------------------------------------------------------------------
      const grab = geometry === kLive || !captured || restart || lastGeometry !== geometry;
      const newJob = geometry === kLatch && grab;
      lastGeometry = geometry;

      if (grab) {
        computeField(input, width, height, p);
        captured = true;
        pathValid = false;
      }

      // The job's buffers, at the job raster; the overlay at the output's.
      const jobResized = cut.width !== jobWidth || cut.height !== jobHeight;
      cut.ensure(jobWidth, jobHeight, gl.R16F);
      overlay.ensure(width, height, gl.RGBA8);

      //------------------------------------------------------------------
      // The tool, in job pixels.
      //------------------------------------------------------------------
      const jobH = jobHeight;
      const radius = 0.5 * ToolDiameterHeights(p('toolDiameter')) * jobH;
      const stepover = StepoverDiameters(p('stepover')) * (2.0 * radius);
      const feed = FeedHeightsPerSecond(p('feed')) * jobH;

      const pieces = [];
      if (machine) {
        if (!pathValid || Math.abs(radius - pathRadius) > 1e-9 || Math.abs(stepover - pathStep) > 1e-9
          || insideOut !== pathInsideOut) {
          buildPath(radius, stepover, insideOut);
        }

        if (geometry === kLatch) {
          if (newJob) {
            cut.clearTo(0, 0, 0, 0);
            tau = 0.0;
          } else if (jobResized) {
            cut.clearTo(0, 0, 0, 0);
          }
          const advance = feed * dt;
          const next = Math.min(tau + advance, pathDuration(path));
          CutSpans(path, tau, next, pieces);
          tau = next;
        } else {
          // Live: the path is this frame's; the part is cut afresh, the tool
          // as far along it as Feed has taken it since the sweep began.
          if (liveStart < 0.0 || restart) liveStart = now;
          const elapsed = Math.max(now - liveStart, 0.0);
          const cycle = pathDuration(path) + feed * kLiveHoldSeconds;
          tau = cycle > 0.0 ? Math.min((feed * elapsed) % cycle, pathDuration(path)) : 0.0;
          cut.clearTo(0, 0, 0, 0);
          CutSpans(path, 0.0, tau, pieces);
        }
      } else if (jobResized) {
        // The plugin's Ensure clears a buffer it allocates; the kit's does not.
        cut.clearTo(0, 0, 0, 0);
      }
      if (geometry === kLatch) liveStart = -1.0;

      const one = [1.0, 1.0, 1.0, 1.0];
      stamp(cut, pieces, 1.0, 1.0, radius, 0, 0.0, one);

      //------------------------------------------------------------------
      // The overlay: the CAM preview's lines, and the tool.
      //------------------------------------------------------------------
      overlay.clearTo(0, 0, 0, 0);
      const scaleX = width / jobWidth;
      const scaleY = height / jobHeight;
      if (machine && mode === kPaths) {
        const cuts = [];
        const rapids = [];
        AllSegments(path, cuts, rapids);
        const line = Math.max(0.6, 0.0009 * height);
        stamp(overlay, cuts, scaleX, scaleY, line, 0, 0.0, [1.0, 0.0, 0.0, 0.0]);
        stamp(overlay, rapids, scaleX, scaleY, line, 0, 0.0, [0.0, 1.0, 0.0, 0.0]);
      }
      if (machine && showTool && !pathEmpty(path)) {
        const at = PositionAt(path, tau);
        stamp(overlay, [{ a: at, b: at }], scaleX, scaleY, radius * scaleY, 1, Math.max(1.5, 0.002 * height), [0.0, 0.0, 1.0, 0.0]);
      }

      //------------------------------------------------------------------
      // The composite, straight to the canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      const angle = LightAngleRadians(p('lightAngle'));
      const outH = height;

      compositeShader.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, cut.texture);
      bindTexture(gl, 2, field.texture);
      bindTexture(gl, 3, overlay.texture);
      compositeShader.setSampler('InputTexture', 0);
      compositeShader.setSampler('Cut', 1);
      compositeShader.setSampler('Field', 2);
      compositeShader.setSampler('Overlay', 3);
      compositeShader.set('FieldUV', fieldUVx(), fieldUVy());
      // The host hands an FFGL plugin a texture the picture may not fill; the
      // page's clip always fills its own, so MaxUV is 1.
      compositeShader.set('MaxUV', 1, 1);
      compositeShader.set('JobSize', jobWidth, jobHeight);
      compositeShader.set('OutSize', width, outH);
      compositeShader.setInt('Mode', mode);
      compositeShader.setInt('FieldMode', OptionIndex(p('fieldMode'), FIELD_MODE_NAMES.length));
      compositeShader.set('StockColour', p('stockR'), p('stockG'), p('stockB'));
      compositeShader.set('PathColour', p('pathR'), p('pathG'), p('pathB'));
      compositeShader.set('Depth', clamp(p('depth'), 0.0, 1.0));
      compositeShader.set('LightDir', f32(Math.cos(angle)), f32(Math.sin(angle)));
      compositeShader.set('ToolRadius', radius);
      compositeShader.set('Offset', DistanceHeights(p('distance')) * outH);
      compositeShader.set('Band', WidthHeights(p('width')) * outH);
      compositeShader.set('Falloff', clamp(p('falloff'), 0.0, 1.0));
      compositeShader.set('MixAmount', clamp(p('mix'), 0.0, 1.0));
      quad.draw();

      for (let unit = 3; unit >= 0; unit -= 1) bindTexture(gl, unit, null);

      telemetry.machine = machine;
      telemetry.live = geometry === kLive;
      telemetry.tau = tau;
      telemetry.feed = feed;
      telemetry.empty = pathEmpty(path);
    },
  };
}

//===========================================================================
// The controls, read out of the plugin's constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: the About block. Restart is FF_TYPE_EVENT there and a boolean here
// (see the header).
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({
  id,
  name,
  type: 'standard',
  default: def,
  group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});

const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const percent = (heights) => `${(100 * heights).toFixed(2)}% of height`;

const demo = mountDemo({
  name: 'Toolpath',
  pluginId: 'TP01',
  tagline:
    'CNC pocketing from a distance field. The clip’s bright region is a pocket to be cleared with a round tool: a jump-flooded distance field gives the offset passes, a tool follows them at a feed rate, and it cuts a disc wherever it goes. Inside corners keep a fillet of the tool’s radius, a slot narrower than the tool is never entered, a wide stepover leaves scallops, and a big pocket takes longer — none of it drawn.',
  page: 'https://stoatworks-labs.com/software/toolpath/',
  repo: 'https://github.com/stoatworks-labs/toolpath',

  blurb:
    'It is Toolpath’s own GLSL — the jump flood, the stamps and the composite — ported to WebGL2, with its C++ tracer and path planner hand-ported to JavaScript (a port that only a reader checks), running on generated clips in this page.',

  // Reveal's stock is opaque, but Engrave, Paths and Field keep the clip's
  // alpha and Glow and Outline raise it where they draw.
  showBackdrop: true,

  // R16F and R32F render targets (the detected channel, the field, the cut),
  // and MAX blending into the R16F cut. Eight bits mid-chain would quantise
  // the distance field, and the field is the whole job.
  needFloat: true,

  params: [
    opt('detectOn', 'Detect On', DETECT_NAMES, 0, 'Region',
      'What makes a pixel part of the pocket. Luma: the bright part (un-premultiplied first). Alpha: the shape of a keyed clip. Chroma: distance from the pixel’s own grey. Luma or Alpha: alpha as a floor with luma on top. In Latch this acts on the next grab — press Restart.'),
    std('threshold', 'Threshold', 0.5, 'Region', {
      hint: 'A texel is inside when its 2×2 block’s mean is over this. In Latch this acts on the next grab — press Restart, as in the plugin.',
    }),
    bool('invert', 'Invert', 0, 'Region', 'Swap inside and outside: machine the dark part. Acts on the next grab in Latch.'),
    std('smooth', 'Smooth', 0.2, 'Region', {
      display: (v) => `σ ${percent(SmoothSigmaHeights(v))}`,
      hint: 'A Gaussian on the detected channel before the threshold, up to a sigma of 1% of the frame height. Under 0.3 lattice texels no blur runs at all: the 2×2 block mean is already that much smoothing.',
    }),

    std('toolDiameter', 'Tool Diameter', ToolDiameterParam(0.04), 'Tool', {
      display: (v) => percent(ToolDiameterHeights(v)),
      hint: '1% to 25% of the frame height. The first pass is a radius in from every wall, so every inside corner keeps a fillet of exactly that radius, and a slot narrower than the tool is never entered. Moving it re-plans the path from the same field and carries on where the tool was.',
    }),
    std('stepover', 'Stepover', StepoverParam(0.45), 'Tool', {
      display: (v) => `${StepoverDiameters(v).toFixed(2)} × diameter`,
      hint: 'How far apart the passes are, in tool diameters. Over 1.0 the passes no longer overlap and ridges of uncut stock — scallops — are left between them.',
    }),
    opt('strategy', 'Strategy', STRATEGY_NAMES, 0, 'Tool',
      'Outside In cuts each pocket’s outermost pass first and works inwards; Inside Out starts in the middle. Either way a pocket is finished before the tool moves to the next.'),
    std('feed', 'Feed', FeedParam(0.6), 'Tool', {
      display: (v) => `${FeedHeightsPerSecond(v).toFixed(2)} heights/s`,
      hint: 'The tool’s speed while it cuts, in frame heights per second of clip time. Rapids between passes run at four times this and cut nothing.',
    }),
    bool('restart', 'Restart', 0, 'Tool',
      'Grab the region again and start a fresh job. An event in the plugin (a button in Resolume); a toggle here that releases itself, which is why it blinks.'),
    opt('geometry', 'Geometry', GEOMETRY_NAMES, 0, 'Tool',
      'Latch grabs the region once and machines it; Live re-grabs every frame and re-cuts the current path’s prefix, holding the finished part for a second before starting over. Live re-traces every frame — the line under the canvas says what that costs here.'),

    opt('mode', 'Mode', MODE_NAMES, 0, 'Render',
      'Reveal: the clip through opaque stock where the tool has cut. Engrave: the cut sunk into the clip, lit. Paths: the CAM preview — cutting moves solid, rapids faint. Field: glow, bevel or outline straight from the distance field, no machining.'),
    colour('stockR', 'Stock Colour', 0.62, 'Render', 'The uncut material in Reveal.'),
    colour('stockG', 'Stock_Green', 0.64, 'Render'),
    colour('stockB', 'Stock_Blue', 0.67, 'Render'),
    std('depth', 'Depth', 0.5, 'Render', {
      hint: 'How strongly the cut’s walls, floor and bevels are lit.',
    }),
    std('lightAngle', 'Light Angle', 0.375, 'Render', {
      display: (v) => `${((LightAngleRadians(v) * 180) / Math.PI).toFixed(0)}°`,
      hint: 'Where the light comes from, anticlockwise from +x. The default, 135°, is the top left.',
    }),
    colour('pathR', 'Path Colour', 0.2, 'Render', 'The path lines in Paths, and the glow or outline in Field.'),
    colour('pathG', 'Path_Green', 0.85, 'Render'),
    colour('pathB', 'Path_Blue', 1.0, 'Render'),
    bool('showTool', 'Show Tool', 1, 'Render', 'A ring the size of the tool where it is now.'),

    opt('fieldMode', 'Field Mode', FIELD_MODE_NAMES, 0, 'Field',
      'Field mode only. Glow falls away outside the region, Bevel lights a slope inside it, Outline draws a line outside it.'),
    std('distance', 'Distance', 0.0, 'Field', {
      display: (v) => percent(DistanceHeights(v)),
      hint: 'Field mode: how far from the boundary the glow, bevel or outline starts, up to a quarter of the frame height.',
    }),
    std('width', 'Width', WidthParam(0.02), 'Field', {
      display: (v) => percent(WidthHeights(v)),
      hint: 'Field mode: the band’s width, 0.2% to 10% of the frame height.',
    }),
    std('falloff', 'Falloff', 0.5, 'Field', {
      hint: 'Field mode: Glow from Gaussian to exponential, Bevel from chamfer to round-over, Outline from hard to soft.',
    }),

    std('mix', 'Mix', 1.0, 'Output'),
  ],

  // Clean regions with corners first. See the header for why the bars lead.
  sources: ['bars', 'alpha', 'spot', 'ramp', 'scene', 'grid'],

  // The plugin ships no factory presets, so these are the page's own —
  // expressed entirely in the plugin's parameters and reachable with the
  // sliders.
  presets: {
    'CAM preview (Paths)': { mode: kPaths },
    'Engrave': { mode: 1 },
    'Wide stepover: scallops': { stepover: StepoverParam(1.6) },
    'Big tool: big fillets': { toolDiameter: ToolDiameterParam(0.12), stepover: StepoverParam(0.6) },
    'Inside Out, in Paths': { strategy: kInsideOut, mode: kPaths },
    'Fast feed': { feed: FeedParam(3.0) },
    'Live': { geometry: kLive },
    'Field: glow': { mode: kField, fieldMode: 0 },
    'Field: bevel': { mode: kField, fieldMode: 1, width: WidthParam(0.05), depth: 0.8 },
    'Field: outline, offset': { mode: kField, fieldMode: 2, distance: 0.12, width: WidthParam(0.006) },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Toolpath is not only a shader: between the jump flood and the stamps sit a readback of the distance field, a marching-squares tracer, a corner put-back, Douglas–Peucker, and a planner that walks the passes as a forest one pocket at a time — Path.cpp, and buildPath and ProcessOpenGL in Toolpath.cpp, with Controls.cpp’s conversions. All of it is ported here function for function, because without it there is no toolpath to show. Nothing checks a port but a reader; the repository’s tptest --march, --fillet, --slot, --scallop and --feed check the C++ and have never heard of this page.',
    'The GPU half is not a port. Detect, blur, seed, the jump flood (with the plugin’s own 1 + JFA + finish schedule), resolve, sample, the stamp and the composite are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the eleven pieces they are assembled from drifts.',
    'The field is read back at the plugin’s own trace grid — the job raster up to 1280 wide, sampled bilinearly off the two-pixel working lattice — with a synchronous readPixels that stalls the pipeline where the plugin’s glReadPixels does: once a job in Latch, every frame in Live. The plugin reads that 32-bit float grid as RED/FLOAT; WebGL2 guarantees only RGBA/FLOAT, so where the driver does not offer RED the page reads four floats a sample and keeps the first. Same numbers, four times the bytes.',
    'The render targets need EXT_color_buffer_float (the R16F detected channel and cut, the R32F field) and OES_texture_float_linear (the field is sampled bilinearly). Both are standard on desktop browsers; without either the page says so rather than rendering a plausible wrong picture.',
    'Restart is FF_TYPE_EVENT in the plugin — a button in Resolume. The kit has no event type, so it is a toggle the renderer releases itself, which is why it blinks. The plugin’s host-clock unit voting is not ported either: the page’s clock is in seconds already. The 0.25 s cap on one frame’s advance is.',
    'In Latch the region is grabbed once, so Threshold, Smooth, Invert and Detect On act on the next grab — press Restart — and switching the clip does nothing until then either, because an FFGL effect cannot see a clip change. That is the plugin’s behaviour, not a gap in the page. Live re-grabs every frame.',
    'There is no audio caveat on this page: Toolpath has no audio path and no audio-driven control.',
    'The plugin’s numerical proof — the flood against an exact EDT, the fillet measured at r, the slot under 2r never entered, scallops of s − 2r, the feed in pixels per second, at two rasters, each with a negative control — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The stats line. Reports what the job is; measures nothing. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.dataset.stats = '';
    stage.append(line);

    const number = (n) => Math.round(n).toLocaleString('en-GB');

    setInterval(() => {
      const t = telemetry;
      if (!t.job) return;
      const where = `Job ${t.job}, field on a ${t.lattice} lattice (${t.floodPasses} flood passes)`;
      if (!t.machine) {
        line.textContent = `${where}. Field mode: no machining, the composite reads the field directly.`;
        return;
      }
      if (t.empty) {
        line.textContent = `${where}, traced on ${t.grid || '—'}: no pass fits — nothing in the region is wider than the tool. Try a smaller Tool Diameter, or a clip with a bigger bright area.`;
        return;
      }
      const seconds = t.duration / Math.max(t.feed, 1e-6);
      const done = t.duration > 0 ? (100 * t.tau) / t.duration : 0;
      line.textContent =
        `${where}, traced on ${t.grid}: ${number(t.levels)} pass${t.levels === 1 ? '' : 'es'}, `
        + `${number(t.loops)} loop${t.loops === 1 ? '' : 's'}, ${number(t.cutLength)} px of cutting, `
        + `${seconds.toFixed(1)} s of job at this feed; the tool is ${done.toFixed(0)}% along. `
        + `Readback and trace: ${t.traceMillis.toFixed(1)} ms${t.live ? ' every frame (Live)' : ' once a job'}.`;
    }, 250);
  }
}

// For the headless check and the cross-check against the C++: the port and the
// last numbers, on the window. Nothing on the page reads these.
window.__toolpath = { telemetry, TraceLevels, SimplifyLoop, Order, CutSpans, PositionAt };
