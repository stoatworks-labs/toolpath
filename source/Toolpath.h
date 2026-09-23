#pragma once

#include "PassBuffer.h"
#include "Path.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
	Toolpath -- CNC pocketing from a distance field, as an FFGL effect.

	**The one idea.** A router clears a pocket with a round tool. Its centre
	can only go where the whole disc fits, and what it removes is everywhere
	the disc has been: the pocket OPENED by a disc of the tool's radius. The
	pocketing path is the set of offset contours of the region -- level sets
	of its distance field at r, r + s, r + 2s ... for tool radius r and
	stepover s. So one distance field, computed by jump flooding from the
	region's boundary, gives the whole CAM job, and the features of a
	machined part fall out of the geometry rather than being drawn:

	  - inside corners keep a fillet of exactly the tool radius;
	  - a slot narrower than the tool is never entered;
	  - a stepover wider than the tool leaves scallops, ridges of stock whose
	    width is s - 2r beside a straight wall;
	  - the tool moves at a feed rate, so a big pocket takes longer.

	The pipeline and its passes are in Shaders.h; the CAM half (trace, order,
	where the tool is) in Path.h, with no GL in it. See AGENTS.md.
*/
class Toolpath : public CFFGLPlugin
{
public:
	Toolpath();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Region
		PT_DETECT_ON,
		PT_THRESHOLD,
		PT_INVERT,
		PT_SMOOTH,

		//Tool
		PT_TOOL_DIAMETER,
		PT_STEPOVER,
		PT_STRATEGY,
		PT_FEED,
		PT_RESTART,
		PT_GEOMETRY,

		//Render
		PT_MODE,
		PT_STOCK_R,
		PT_STOCK_G,
		PT_STOCK_B,
		PT_DEPTH,
		PT_LIGHT_ANGLE,
		PT_PATH_R,
		PT_PATH_G,
		PT_PATH_B,
		PT_SHOW_TOOL,

		//Field
		PT_FIELD_MODE,
		PT_DISTANCE,
		PT_WIDTH,
		PT_FALLOFF,

		//Output
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Negative-control hooks. Always 0 in the plugin; each bit breaks one
	/// piece of the model so `tptest --negative` can show a check fails.
	enum Perturb : int
	{
		kPerturbSquareTool     = 1 << 0,///< the stamp cuts a square (GLSL bit 1)
		kPerturbNoPrepass      = 1 << 1,///< skip the step-1 pass before the flood
		kPerturbNoFinish       = 1 << 2,///< skip the finishing steps after it
		kPerturbFirstLevelHalf = 1 << 3,///< the first pass at r / 2: the tool gouges the wall
		kPerturbStepoverRadius = 1 << 4,///< stepover read as radii, not diameters
		kPerturbFeedPerFrame   = 1 << 5,///< the tool advances Feed / 60 a frame, whatever dt is
		kPerturbResizeClears   = 1 << 6,///< Latch: a resize re-grabs the region, clearing the cut
		kPerturbRestartKeeps   = 1 << 7,///< Restart re-grabs but leaves the cut buffer
		kPerturbFullResField   = 1 << 8,///< the field at the full raster, not the working lattice
	};

	/// Job pixels to a side of one field texel: the region is decided, and
	/// the field flooded and resolved, on a lattice this many times coarser
	/// than the raster, at EVERY raster. Fixed rather than capped so that the
	/// checks at 320x180 and 1280x720 run the very code that ships at 4K;
	/// AGENTS.md has why 2 and what it costs.
	static constexpr int kFieldScale = 2;

	/// Most samples across the trace grid. The field is sampled down to at
	/// most this wide for the CPU tracer; at or under it, one sample per job
	/// pixel. AGENTS.md: why 1280.
	static constexpr int kTraceMaxWidth = 1280;

	/// Douglas-Peucker tolerance on a traced contour, job pixels.
	static constexpr double kSimplify = 0.2;

	/// The most levels one job may have. A 1% tool at a 0.1 stepover across a
	/// 4K frame needs ~1100.
	static constexpr int kMaxLevels = 4096;

	//--- test hooks (tptest) ---------------------------------------------------
	/// The harness declares its clock's unit rather than leaving the voting
	/// to infer one.
	void SetClockScaleForTest( double scale );
	void SetPerturbForTest( int bits );
	/// Time the field computation with glFinish on both sides (for --bench).
	void SetTimingForTest( bool on );
	double LastFieldMillisForTest() const
	{
		return fieldMillis;
	}
	double LastTraceMillisForTest() const
	{
		return traceMillis;
	}

	/// The signed distance field as it stands, in job pixels, on the working
	/// lattice (rows bottom-up): ceil( job / FieldScaleForTest() ) texels.
	bool ReadFieldForTest( std::vector< float >& out, int& width, int& height );
	/// Job pixels to a field texel side, as the last grab used it.
	int FieldScaleForTest() const
	{
		return fieldScale;
	}
	/// The cut buffer as it stands (job raster, rows bottom-up).
	bool ReadCutForTest( std::vector< float >& out, int& width, int& height );
	/// Where the tool is, in job pixels (GL convention), and its timeline.
	toolpath::path::Point ToolForTest() const;
	double TauForTest() const
	{
		return tau;
	}
	const toolpath::path::Path& PathForTest() const
	{
		return path;
	}
	int JobWidthForTest() const
	{
		return jobWidth;
	}
	int JobHeightForTest() const
	{
		return jobHeight;
	}

private:
	double nowSeconds();

	bool compileAll();
	/// Detect (a mean over each k x k block), blur, seed, flood, resolve on
	/// the working lattice, into the job field, in job pixels. Returns false
	/// if a buffer could not be allocated.
	bool computeField( const FFGLTextureStruct& input, int width, int height );
	/// Sample the job field onto the trace grid, read it back, trace and order.
	void buildPath( double radius, double stepover, bool insideOut );
	/// The job's 0..1 on the field's lattice: 1 unless the raster is odd, when
	/// the last texel overhangs it by a pixel.
	float fieldUVx() const
	{
		return field.Width() > 0 ? static_cast< float >( jobWidth ) / static_cast< float >( fieldScale * field.Width() ) : 1.0f;
	}
	float fieldUVy() const
	{
		return field.Height() > 0 ? static_cast< float >( jobHeight ) / static_cast< float >( fieldScale * field.Height() ) : 1.0f;
	}
	void stamp( toolpath::PassBuffer& target, const std::vector< toolpath::path::Span >& pieces, float scaleX,
	            float scaleY, float radius, int shape, float ringWidth, const float channel[ 4 ] );

	ffglex::FFGLShader detectShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader seedShader;
	ffglex::FFGLShader floodShader;
	ffglex::FFGLShader resolveShader;
	ffglex::FFGLShader sampleShader;
	ffglex::FFGLShader stampShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	GLuint stampVAO       = 0;
	GLuint cornerBuffer   = 0;
	GLuint pieceBuffer    = 0;

	toolpath::PassBuffer value[ 2 ];///< the detected channel, and the blur's scratch
	toolpath::PassBuffer seeds[ 2 ];///< the flood's ping-pong, RGBA16UI
	toolpath::PassBuffer field;     ///< signed distance in job pixels, working lattice
	toolpath::PassBuffer grid;      ///< the field on the trace grid
	toolpath::PassBuffer cut;       ///< the part: what the tool has removed, job raster
	toolpath::PassBuffer overlay;   ///< path lines and the tool, output raster

	std::vector< float > gridPixels;
	toolpath::path::Path path;
	bool pathValid     = false;
	double pathRadius  = -1.0;
	double pathStep    = -1.0;
	bool pathInsideOut = false;

	int jobWidth    = 0;
	int jobHeight   = 0;
	int fieldScale  = kFieldScale;///< job pixels a field texel, for the grab in hand
	bool captured   = false;
	int lastWidth   = 0;
	int lastHeight  = 0;
	int lastGeometry = -1;
	double tau       = 0.0;
	double liveStart = -1.0;
	bool restartPending = false;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb        = 0;
	bool timing        = false;
	double fieldMillis = 0.0;
	double traceMillis = 0.0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	std::string aboutText;
};
