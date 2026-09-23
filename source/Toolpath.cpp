#include "Toolpath.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace toolpath;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Toolpath >,// Create method
	"TP01",                   // Plugin unique ID of maximum length 4.
	"SW Toolpath",            // Plugin name
	2,                        // API major version number
	1,                        // API minor version number
	0,                        // Plugin major version number
	1,                        // Plugin minor version number
	FF_EFFECT,                // Plugin type
	"CNC pocketing from a distance field.\n\nThe clip's bright region is a pocket to be cleared with a round tool. A jump-flooded distance field gives the offset passes; a tool follows them at a feed rate and cuts a disc wherever it goes.\n\nWhat falls out: inside corners keep a fillet of the tool's radius, slots narrower than the tool are never entered, a wide stepover leaves scallops, and a big pocket takes longer than a small one.",// Plugin description
	"Toolpath FFGL effect"    // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance the clock by.
constexpr double kMaxFrameDelta = 0.25;

/// Live: how long the finished part holds before the sweep starts again.
constexpr double kLiveHoldSeconds = 1.0;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void bindTexture( int unit, GLuint texture )
{
	glActiveTexture( GL_TEXTURE0 + static_cast< GLenum >( unit ) );
	glBindTexture( GL_TEXTURE_2D, texture );
}

double millisSince( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
}
} // namespace

//---------------------------------------------------------------------------
Toolpath::Toolpath()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The tool advances with the host's clock, so a re-render of the same
	//composition machines the same part the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. The bright part of the clip is the pocket; a tool 4% of the
	// frame high clears it outside in at a 45% stepover and 0.6 frame heights
	// a second, grabbed once (Latch) and revealed through aluminium-grey
	// stock.
	//---------------------------------------------------------------------
	params[ PT_DETECT_ON ] = static_cast< float >( controls::kDetectLuma );
	params[ PT_THRESHOLD ] = 0.5f;
	params[ PT_INVERT ]    = 0.0f;
	params[ PT_SMOOTH ]    = 0.2f;

	params[ PT_TOOL_DIAMETER ] = controls::ToolDiameterParam( 0.04f );
	params[ PT_STEPOVER ]      = controls::StepoverParam( 0.45f );
	params[ PT_STRATEGY ]      = static_cast< float >( controls::kOutsideIn );
	params[ PT_FEED ]          = controls::FeedParam( 0.6f );
	params[ PT_RESTART ]       = 0.0f;
	params[ PT_GEOMETRY ]      = static_cast< float >( controls::kLatch );

	params[ PT_MODE ]        = static_cast< float >( controls::kReveal );
	params[ PT_STOCK_R ]     = 0.62f;
	params[ PT_STOCK_G ]     = 0.64f;
	params[ PT_STOCK_B ]     = 0.67f;
	params[ PT_DEPTH ]       = 0.5f;
	params[ PT_LIGHT_ANGLE ] = 0.375f;//135 degrees: from the top left
	params[ PT_PATH_R ]      = 0.20f;
	params[ PT_PATH_G ]      = 0.85f;
	params[ PT_PATH_B ]      = 1.00f;
	params[ PT_SHOW_TOOL ]   = 1.0f;

	params[ PT_FIELD_MODE ] = static_cast< float >( controls::kGlow );
	params[ PT_DISTANCE ]   = 0.0f;
	params[ PT_WIDTH ]      = controls::WidthParam( 0.02f );
	params[ PT_FALLOFF ]    = 0.5f;

	params[ PT_MIX ] = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_DETECT_ON, "Detect On", controls::kDetectCount, controls::DetectName );
	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_SMOOTH, "Smooth", FF_TYPE_STANDARD );

	SetParamInfof( PT_TOOL_DIAMETER, "Tool Diameter", FF_TYPE_STANDARD );
	SetParamInfof( PT_STEPOVER, "Stepover", FF_TYPE_STANDARD );
	declareOptions( PT_STRATEGY, "Strategy", controls::kStrategyCount, controls::StrategyName );
	SetParamInfof( PT_FEED, "Feed", FF_TYPE_STANDARD );
	SetParamInfo( PT_RESTART, "Restart", FF_TYPE_EVENT, false );
	declareOptions( PT_GEOMETRY, "Geometry", controls::kGeometryCount, controls::GeometryName );

	declareOptions( PT_MODE, "Mode", controls::kModeCount, controls::ModeName );
	//Consecutive red/green/blue parameters are what a host needs to show a
	//swatch rather than three sliders.
	SetParamInfof( PT_STOCK_R, "Stock Colour", FF_TYPE_RED );
	SetParamInfof( PT_STOCK_G, "Stock_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_STOCK_B, "Stock_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_DEPTH, "Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_LIGHT_ANGLE, "Light Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_PATH_R, "Path Colour", FF_TYPE_RED );
	SetParamInfof( PT_PATH_G, "Path_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PATH_B, "Path_Blue", FF_TYPE_BLUE );
	SetParamInfo( PT_SHOW_TOOL, "Show Tool", FF_TYPE_BOOLEAN, true );

	declareOptions( PT_FIELD_MODE, "Field Mode", controls::kFieldModeCount, controls::FieldModeName );
	SetParamInfof( PT_DISTANCE, "Distance", FF_TYPE_STANDARD );
	SetParamInfof( PT_WIDTH, "Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_FALLOFF, "Falloff", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_DETECT_ON; i <= PT_SMOOTH; ++i )
		SetParamGroup( i, "Region" );
	for( FFUInt32 i = PT_TOOL_DIAMETER; i <= PT_GEOMETRY; ++i )
		SetParamGroup( i, "Tool" );
	for( FFUInt32 i = PT_MODE; i <= PT_SHOW_TOOL; ++i )
		SetParamGroup( i, "Render" );
	for( FFUInt32 i = PT_FIELD_MODE; i <= PT_FALLOFF; ++i )
		SetParamGroup( i, "Field" );
	SetParamGroup( PT_MIX, "Output" );

	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Toolpath effect" );

	diag::init();
}

//---------------------------------------------------------------------------
bool Toolpath::compileAll()
{
	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string vertex;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &detectShader, vertex, shaders::Detect(), "detect" },
		{ &blurShader, vertex, shaders::Blur(), "blur" },
		{ &seedShader, vertex, shaders::Seed(), "seed" },
		{ &floodShader, vertex, shaders::Flood(), "flood" },
		{ &resolveShader, vertex, shaders::Resolve(), "resolve" },
		{ &sampleShader, vertex, shaders::Sample(), "sample" },
		{ &stampShader, shaders::StampVertex(), shaders::StampFragment(), "stamp" },
		{ &compositeShader, vertex, shaders::Composite(), "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( stage.vertex, stage.fragment ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the effect simply
		//does nothing. These two lines are the only record of which pass.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Toolpath: shader failed to compile" );
		return false;
	}
	return true;
}

FFResult Toolpath::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileAll() || !quad.Initialise() )
	{
		diag::error( "InitGL failed" );
		DeInitGL();
		return FF_FAIL;
	}

	//The stamp's geometry: a unit quad (along 0..1, across -1..1) shared by
	//every instance, and a stream of pieces, one vec4 per instance.
	const float corners[ 8 ] = { 0.0f, -1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	glGenVertexArrays( 1, &stampVAO );
	glGenBuffers( 1, &cornerBuffer );
	glGenBuffers( 1, &pieceBuffer );
	glBindVertexArray( stampVAO );
	glBindBuffer( GL_ARRAY_BUFFER, cornerBuffer );
	glBufferData( GL_ARRAY_BUFFER, sizeof( corners ), corners, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
	glBindBuffer( GL_ARRAY_BUFFER, pieceBuffer );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, 0, nullptr );
	glVertexAttribDivisor( 1, 1 );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	captured       = false;
	pathValid      = false;
	restartPending = false;
	lastGeometry   = -1;
	lastWidth = lastHeight = 0;
	tau                    = 0.0;
	liveStart              = -1.0;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Toolpath::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock delta
//to a steady clock's names the unit outright.
double Toolpath::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;
	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
// Detect, blur, seed, flood, resolve: the field of the current frame's
// region, into `field`, which then IS the job raster.
//---------------------------------------------------------------------------
bool Toolpath::computeField( const FFGLTextureStruct& input, int width, int height )
{
	const bool allocated = value[ 0 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Nearest )
	                       && value[ 1 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Nearest )
	                       && seeds[ 0 ].Ensure( width, height, GL_RGBA16UI, PassBuffer::Sampling::Nearest )
	                       && seeds[ 1 ].Ensure( width, height, GL_RGBA16UI, PassBuffer::Sampling::Nearest )
	                       && field.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Linear );
	if( !allocated )
	{
		diag::error( "could not allocate the field buffers at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return false;
	}
	jobWidth  = width;
	jobHeight = height;

	const auto start = std::chrono::steady_clock::now();
	if( timing )
		glFinish();

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//1. detect
	value[ 0 ].BindForDrawing();
	glUseProgram( detectShader.GetGLID() );
	bindTexture( 0, input.Handle );
	detectShader.Set( "InputTexture", 0 );
	detectShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
	detectShader.Set( "DetectOn", controls::OptionIndex( params[ PT_DETECT_ON ], controls::kDetectCount ) );
	quad.Draw();

	//2. blur, x then y, back into value[ 0 ]
	const float sigma = controls::SmoothSigmaHeights( params[ PT_SMOOTH ] ) * static_cast< float >( height );
	if( sigma >= 0.3f )
	{
		const int taps = std::min( 48, static_cast< int >( std::ceil( 3.0f * sigma ) ) );
		glUseProgram( blurShader.GetGLID() );
		blurShader.Set( "Value", 0 );
		blurShader.Set( "Sigma", sigma );
		blurShader.Set( "Taps", taps );
		for( int axis = 0; axis < 2; ++axis )
		{
			value[ 1 - axis ].BindForDrawing();
			bindTexture( 0, value[ axis ].TextureID() );
			glUniform2i( blurShader.FindUniform( "Axis" ), axis == 0 ? 1 : 0, axis == 0 ? 0 : 1 );
			quad.Draw();
		}
	}

	//3. seed
	seeds[ 0 ].BindForDrawing();
	glUseProgram( seedShader.GetGLID() );
	bindTexture( 0, value[ 0 ].TextureID() );
	seedShader.Set( "Value", 0 );
	seedShader.Set( "Threshold", std::clamp( params[ PT_THRESHOLD ], 0.0f, 1.0f ) );
	seedShader.Set( "Invert", params[ PT_INVERT ] > 0.5f ? 1 : 0 );
	quad.Draw();

	//4. flood: 1 + JFA + 2. The steps run from the largest power of two
	//below the raster's longer side down to 1; the prepass and the two
	//finishing passes are the published variants (Rong & Tan 2006) that
	//repair the flood's known failures.
	std::vector< int > steps;
	if( !( perturb & kPerturbNoPrepass ) )
		steps.push_back( 1 );
	int longest = 1;
	while( longest < std::max( width, height ) )
		longest *= 2;
	for( int step = longest / 2; step >= 1; step /= 2 )
		steps.push_back( step );
	if( !( perturb & kPerturbNoFinish ) )
	{
		steps.push_back( 2 );
		steps.push_back( 1 );
	}

	glUseProgram( floodShader.GetGLID() );
	floodShader.Set( "Seeds", 0 );
	glUniform2i( floodShader.FindUniform( "Size" ), width, height );
	int current = 0;
	for( int step : steps )
	{
		seeds[ 1 - current ].BindForDrawing();
		bindTexture( 0, seeds[ current ].TextureID() );
		floodShader.Set( "Step", step );
		quad.Draw();
		current = 1 - current;
	}

	//5. resolve
	field.BindForDrawing();
	glUseProgram( resolveShader.GetGLID() );
	bindTexture( 0, seeds[ current ].TextureID() );
	resolveShader.Set( "Seeds", 0 );
	glUniform2i( resolveShader.FindUniform( "Size" ), width, height );
	quad.Draw();

	if( timing )
	{
		glFinish();
		fieldMillis = millisSince( start );
	}
	return true;
}

//---------------------------------------------------------------------------
// The CAM job, from the job field: sample it onto the trace grid, read it
// back, trace the offset levels, simplify, order.
//---------------------------------------------------------------------------
void Toolpath::buildPath( double radius, double stepover, bool insideOut )
{
	pathValid     = true;
	pathRadius    = radius;
	pathStep      = stepover;
	pathInsideOut = insideOut;
	path          = path::Path{};

	if( !field.IsValid() || jobWidth <= 0 || jobHeight <= 0 )
		return;

	const auto start = std::chrono::steady_clock::now();

	const int tw = std::min( jobWidth, kTraceMaxWidth );
	const int th = std::max( 1, static_cast< int >( std::lround( static_cast< double >( jobHeight ) * tw / jobWidth ) ) );
	gridPixels.resize( static_cast< size_t >( tw ) * th );

	GLint previousFBO = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );
	if( tw == jobWidth && th == jobHeight )
	{
		field.BindForDrawing();
	}
	else
	{
		if( !grid.Ensure( tw, th, GL_R32F, PassBuffer::Sampling::Nearest ) )
		{
			diag::error( "could not allocate the trace grid" );
			return;
		}
		grid.BindForDrawing();
		glUseProgram( sampleShader.GetGLID() );
		bindTexture( 0, field.TextureID() );
		sampleShader.Set( "Field", 0 );
		quad.Draw();
	}
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, tw, th, GL_RED, GL_FLOAT, gridPixels.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFBO ) );

	float highest = -1.0e30f;
	for( float v : gridPixels )
		highest = std::max( highest, v );

	path::Levels levels;
	levels.first = ( perturb & kPerturbFirstLevelHalf ) ? 0.5 * radius : radius;
	levels.step  = std::max( stepover, 0.25 );
	levels.count = highest >= levels.first
	                   ? std::min( kMaxLevels, static_cast< int >( std::floor( ( highest - levels.first ) / levels.step ) ) + 1 )
	                   : 0;

	const double cellW = static_cast< double >( jobWidth ) / tw;
	const double cellH = static_cast< double >( jobHeight ) / th;
	std::vector< path::Loop > loops = path::TraceLevels( gridPixels.data(), tw, th, cellW, cellH, levels );
	for( path::Loop& loop : loops )
		path::SimplifyLoop( loop, kSimplify );
	path = path::Order( std::move( loops ), levels.count, insideOut, path::Point{ 0.0, 0.0 } );

	traceMillis = millisSince( start );
}

//---------------------------------------------------------------------------
void Toolpath::stamp( PassBuffer& target, const std::vector< path::Span >& pieces, float scaleX, float scaleY,
                      float radius, int shape, float ringWidth, const float channel[ 4 ] )
{
	if( pieces.empty() )
		return;

	target.BindForDrawing();
	glUseProgram( stampShader.GetGLID() );
	stampShader.Set( "TargetSize", static_cast< float >( target.Width() ), static_cast< float >( target.Height() ) );
	stampShader.Set( "Scale", scaleX, scaleY );
	stampShader.Set( "Radius", radius );
	stampShader.Set( "Shape", shape );
	stampShader.Set( "RingWidth", ringWidth );
	stampShader.Set( "Channel", channel[ 0 ], channel[ 1 ], channel[ 2 ], channel[ 3 ] );
	stampShader.Set( "Perturb", perturb & kPerturbSquareTool ? 1 : 0 );

	std::vector< float > data;
	data.reserve( pieces.size() * 4 );
	for( const path::Span& s : pieces )
	{
		data.push_back( static_cast< float >( s.a.x ) );
		data.push_back( static_cast< float >( s.a.y ) );
		data.push_back( static_cast< float >( s.b.x ) );
		data.push_back( static_cast< float >( s.b.y ) );
	}

	glBindVertexArray( stampVAO );
	glBindBuffer( GL_ARRAY_BUFFER, pieceBuffer );
	glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( data.size() * sizeof( float ) ), data.data(), GL_STREAM_DRAW );

	//MAX, not ADD: a pixel the tool passes twice is no more cut than one it
	//passes once, and the 0.5 contour must stay exactly Radius from the path.
	glEnable( GL_BLEND );
	glBlendEquation( GL_MAX );
	glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, static_cast< GLsizei >( pieces.size() ) );
	glBlendEquation( GL_FUNC_ADD );
	glDisable( GL_BLEND );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindVertexArray( 0 );
}

//---------------------------------------------------------------------------
FFResult Toolpath::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//ScopedFBOBinding restores the framebuffer binding and only that; the
	//host's viewport is read now and put back before the composite.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. Only the tool reads it, and only as a difference: dt for
	// Latch, time since the sweep began for Live, both in double.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = 0.0;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, kMaxFrameDelta );
	lastNow = now;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int geometry  = controls::OptionIndex( params[ PT_GEOMETRY ], controls::kGeometryCount );
	const int mode      = controls::OptionIndex( params[ PT_MODE ], controls::kModeCount );
	const bool machine  = mode != controls::kField;
	const bool insideOut = controls::OptionIndex( params[ PT_STRATEGY ], controls::kStrategyCount ) == controls::kInsideOut;
	const bool showTool = params[ PT_SHOW_TOOL ] > 0.5f;

	const bool resized = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth          = width;
	lastHeight         = height;

	const bool restart = restartPending;
	restartPending     = false;

	//---------------------------------------------------------------------
	// The region. Live grabs it every frame; Latch at the first frame, on
	// Restart, and on switching to Latch. A resize does NOT re-grab a latched
	// job: the field and the cut stay at the raster they were made at and
	// are sampled into whatever the output is now, so the part survives (the
	// photofinish trap, avoided by construction).
	//---------------------------------------------------------------------
	bool grab = geometry == controls::kLive || !captured || restart || lastGeometry != geometry;
	if( geometry == controls::kLatch && resized && ( perturb & kPerturbResizeClears ) )
		grab = true;
	const bool newJob = geometry == controls::kLatch && grab;
	lastGeometry      = geometry;

	if( grab )
	{
		if( !computeField( input, width, height ) )
			return FF_FAIL;
		captured  = true;
		pathValid = false;
	}

	//The job's buffers, at the job raster, allocated before anything else is
	//bound for this frame's drawing. The overlay is at the output raster.
	const bool jobResized = cut.Width() != jobWidth || cut.Height() != jobHeight;
	if( !cut.Ensure( jobWidth, jobHeight, GL_R16F, PassBuffer::Sampling::Linear )
	    || !overlay.Ensure( width, height, GL_RGBA8, PassBuffer::Sampling::Linear ) )
	{
		diag::error( "could not allocate the cut buffer" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// The tool. Lengths in job pixels: the tool's radius and the stepover
	// from their frame-height controls, and the feed in job pixels per
	// second.
	//---------------------------------------------------------------------
	const double jobH     = static_cast< double >( jobHeight );
	const double radius   = 0.5 * controls::ToolDiameterHeights( params[ PT_TOOL_DIAMETER ] ) * jobH;
	const double stepover = controls::StepoverDiameters( params[ PT_STEPOVER ] )
	                        * ( ( perturb & kPerturbStepoverRadius ) ? radius : 2.0 * radius );
	const double feed     = controls::FeedHeightsPerSecond( params[ PT_FEED ] ) * jobH;

	std::vector< path::Span > pieces;
	if( machine )
	{
		if( !pathValid || std::fabs( radius - pathRadius ) > 1e-9 || std::fabs( stepover - pathStep ) > 1e-9
		    || insideOut != pathInsideOut )
			buildPath( radius, stepover, insideOut );

		if( geometry == controls::kLatch )
		{
			if( newJob )
			{
				if( !( restart && ( perturb & kPerturbRestartKeeps ) ) )
					cut.Clear();
				tau = 0.0;
			}
			else if( jobResized )
				cut.Clear();

			const double advance = feed * ( ( perturb & kPerturbFeedPerFrame ) ? 1.0 / 60.0 : dt );
			const double next    = std::min( tau + advance, path.Duration() );
			path::CutSpans( path, tau, next, pieces );
			tau = next;
		}
		else
		{
			//Live: the path is this frame's; the part is cut afresh, the tool
			//as far along it as Feed has taken it since the sweep began.
			if( liveStart < 0.0 || restart )
				liveStart = now;
			const double elapsed = std::max( now - liveStart, 0.0 );
			const double cycle   = path.Duration() + feed * kLiveHoldSeconds;
			tau                  = cycle > 0.0 ? std::min( std::fmod( feed * elapsed, cycle ), path.Duration() ) : 0.0;
			cut.Clear();
			path::CutSpans( path, 0.0, tau, pieces );
		}
	}
	if( geometry == controls::kLatch )
		liveStart = -1.0;

	const float one[ 4 ] = { 1.0f, 1.0f, 1.0f, 1.0f };
	stamp( cut, pieces, 1.0f, 1.0f, static_cast< float >( radius ), 0, 0.0f, one );

	//---------------------------------------------------------------------
	// The overlay: the CAM preview's lines, and the tool.
	//---------------------------------------------------------------------
	overlay.Clear();
	const float scaleX = static_cast< float >( width ) / static_cast< float >( jobWidth );
	const float scaleY = static_cast< float >( height ) / static_cast< float >( jobHeight );
	if( machine && mode == controls::kPaths )
	{
		std::vector< path::Span > cuts, rapids;
		path::AllSegments( path, cuts, rapids );
		const float line          = std::max( 0.6f, 0.0009f * static_cast< float >( height ) );
		const float red[ 4 ]      = { 1.0f, 0.0f, 0.0f, 0.0f };
		const float green[ 4 ]    = { 0.0f, 1.0f, 0.0f, 0.0f };
		stamp( overlay, cuts, scaleX, scaleY, line, 0, 0.0f, red );
		stamp( overlay, rapids, scaleX, scaleY, line, 0, 0.0f, green );
	}
	if( machine && showTool && !path.Empty() )
	{
		const path::Point at            = path::PositionAt( path, tau );
		const std::vector< path::Span > tool = { path::Span{ at, at } };
		const float blue[ 4 ]           = { 0.0f, 0.0f, 1.0f, 0.0f };
		stamp( overlay, tool, scaleX, scaleY, static_cast< float >( radius ) * scaleY, 1,
		       std::max( 1.5f, 0.002f * static_cast< float >( height ) ), blue );
	}

	//---------------------------------------------------------------------
	// The composite, straight to the host.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const float angle             = controls::LightAngleRadians( params[ PT_LIGHT_ANGLE ] );
	const float outH              = static_cast< float >( height );

	glUseProgram( compositeShader.GetGLID() );
	bindTexture( 0, input.Handle );
	bindTexture( 1, cut.TextureID() );
	bindTexture( 2, field.TextureID() );
	bindTexture( 3, overlay.TextureID() );
	compositeShader.Set( "InputTexture", 0 );
	compositeShader.Set( "Cut", 1 );
	compositeShader.Set( "Field", 2 );
	compositeShader.Set( "Overlay", 3 );
	compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
	compositeShader.Set( "JobSize", static_cast< float >( jobWidth ), static_cast< float >( jobHeight ) );
	compositeShader.Set( "OutSize", static_cast< float >( width ), outH );
	compositeShader.Set( "Mode", mode );
	compositeShader.Set( "FieldMode", controls::OptionIndex( params[ PT_FIELD_MODE ], controls::kFieldModeCount ) );
	compositeShader.Set( "StockColour", params[ PT_STOCK_R ], params[ PT_STOCK_G ], params[ PT_STOCK_B ] );
	compositeShader.Set( "PathColour", params[ PT_PATH_R ], params[ PT_PATH_G ], params[ PT_PATH_B ] );
	compositeShader.Set( "Depth", std::clamp( params[ PT_DEPTH ], 0.0f, 1.0f ) );
	compositeShader.Set( "LightDir", std::cos( angle ), std::sin( angle ) );
	compositeShader.Set( "ToolRadius", static_cast< float >( radius ) );
	compositeShader.Set( "Offset", controls::DistanceHeights( params[ PT_DISTANCE ] ) * outH );
	compositeShader.Set( "Band", controls::WidthHeights( params[ PT_WIDTH ] ) * outH );
	compositeShader.Set( "Falloff", std::clamp( params[ PT_FALLOFF ], 0.0f, 1.0f ) );
	compositeShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
	quad.Draw();

	for( int unit = 3; unit >= 0; --unit )
		bindTexture( unit, 0 );
	glUseProgram( 0 );

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Toolpath::DeInitGL()
{
	detectShader.FreeGLResources();
	blurShader.FreeGLResources();
	seedShader.FreeGLResources();
	floodShader.FreeGLResources();
	resolveShader.FreeGLResources();
	sampleShader.FreeGLResources();
	stampShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	for( PassBuffer* b : { &value[ 0 ], &value[ 1 ], &seeds[ 0 ], &seeds[ 1 ], &field, &grid, &cut, &overlay } )
		b->Destroy();
	if( stampVAO != 0 )
		glDeleteVertexArrays( 1, &stampVAO );
	if( cornerBuffer != 0 )
		glDeleteBuffers( 1, &cornerBuffer );
	if( pieceBuffer != 0 )
		glDeleteBuffers( 1, &pieceBuffer );
	stampVAO = cornerBuffer = pieceBuffer = 0;
	captured                              = false;
	pathValid                             = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Toolpath::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//An event arrives as 1.0 on press and 0.0 on release. The press is
	//remembered until the next frame; the value itself is not kept.
	if( index == PT_RESTART )
	{
		if( value >= 0.5f )
			restartPending = true;
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Toolpath::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Toolpath::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Toolpath::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Toolpath::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Toolpath::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Toolpath::SetTimingForTest( bool on )
{
	timing = on;
}

namespace
{
bool readTexture( GLuint texture, int width, int height, std::vector< float >& out )
{
	if( texture == 0 || width <= 0 || height <= 0 )
		return false;
	out.resize( static_cast< size_t >( width ) * height );
	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, out.data() );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );
	return true;
}
} // namespace

bool Toolpath::ReadFieldForTest( std::vector< float >& out, int& width, int& height )
{
	width  = field.Width();
	height = field.Height();
	return readTexture( field.TextureID(), width, height, out );
}

bool Toolpath::ReadCutForTest( std::vector< float >& out, int& width, int& height )
{
	width  = cut.Width();
	height = cut.Height();
	return readTexture( cut.TextureID(), width, height, out );
}

path::Point Toolpath::ToolForTest() const
{
	return path::PositionAt( path, tau );
}
