#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`CFFGLPluginManager::SetParamInfo` clamps a STANDARD default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards, so
	a parameter declared in pixels cannot declare a default in pixels. Every
	slider here is therefore a plain 0..1 float and the conversions live in
	this one file, which the plugin and the harness both use. Every mapping a
	check needs to hit exactly has an inverse.

	**Lengths are in frame heights, not pixels.** A tool 4% of the frame high
	is the same tool at 320x180 and at 3840x2160, so the look does not change
	with the composition's raster, and every harness check can run at two
	rasters and mean the same thing at both. The spec's "Feed in px per
	second" is honoured as what `--feed` measures -- pixels of the job raster
	per second of clip time -- with the control declared in heights per
	second. AGENTS.md records the decision.

	Options are mapped by INDEX. An option parameter's range reads back 0..1
	from the SDK whatever its element count, so nothing outside this file
	should reason from the range.
*/
namespace toolpath::controls
{

/// Smooth: a Gaussian blur of the detected channel before the threshold, 0
/// (none) to a sigma of 1% of the frame height, linear.
float SmoothSigmaHeights( float value );

/// Tool Diameter: 1% to 25% of the frame height, geometric.
float ToolDiameterHeights( float value );
float ToolDiameterParam( float heights );

/// Stepover: 0.1 to 2.0 tool diameters, linear. Over 1.0 the passes no
/// longer overlap and scallops of uncut stock are left between them.
float StepoverDiameters( float value );
float StepoverParam( float diameters );

/// Feed: 0.05 to 8 frame heights per second of clip time, geometric.
float FeedHeightsPerSecond( float value );
float FeedParam( float heightsPerSecond );

/// Light Angle: 0 to 360 degrees, anticlockwise from +x, as radians.
float LightAngleRadians( float value );

/// Distance: 0 to 25% of the frame height, linear. Field mode's offset.
float DistanceHeights( float value );
float DistanceParam( float heights );

/// Width: 0.2% to 10% of the frame height, geometric. Field mode's band.
float WidthHeights( float value );
float WidthParam( float heights );

/// Option counts, and names in their menu order.
constexpr int kDetectCount = 4;
const char* DetectName( int index );
constexpr int kStrategyCount = 2;
const char* StrategyName( int index );
constexpr int kGeometryCount = 2;
const char* GeometryName( int index );
constexpr int kModeCount = 4;
const char* ModeName( int index );
constexpr int kFieldModeCount = 3;
const char* FieldModeName( int index );

enum Detect
{
	kDetectLuma,
	kDetectAlpha,
	kDetectChroma,
	kDetectLumaOrAlpha
};
enum Strategy
{
	kOutsideIn,
	kInsideOut
};
enum Geometry
{
	kLatch,
	kLive
};
enum Mode
{
	kReveal,
	kEngrave,
	kPaths,
	kField
};
enum FieldMode
{
	kGlow,
	kBevel,
	kOutline
};

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

} // namespace toolpath::controls
