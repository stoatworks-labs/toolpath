#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace toolpath::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}

constexpr float kPi = 3.14159265358979f;
} // namespace

float SmoothSigmaHeights( float value )
{
	return 0.01f * clamp01( value );
}

float ToolDiameterHeights( float value )
{
	return 0.01f * std::pow( 25.0f, clamp01( value ) );
}

float ToolDiameterParam( float heights )
{
	return clamp01( std::log( std::max( heights, 0.01f ) / 0.01f ) / std::log( 25.0f ) );
}

float StepoverDiameters( float value )
{
	return 0.1f + 1.9f * clamp01( value );
}

float StepoverParam( float diameters )
{
	return clamp01( ( diameters - 0.1f ) / 1.9f );
}

float FeedHeightsPerSecond( float value )
{
	return 0.05f * std::pow( 160.0f, clamp01( value ) );
}

float FeedParam( float heightsPerSecond )
{
	return clamp01( std::log( std::max( heightsPerSecond, 0.05f ) / 0.05f ) / std::log( 160.0f ) );
}

float LightAngleRadians( float value )
{
	return 2.0f * kPi * clamp01( value );
}

float DistanceHeights( float value )
{
	return 0.25f * clamp01( value );
}

float DistanceParam( float heights )
{
	return clamp01( heights / 0.25f );
}

float WidthHeights( float value )
{
	return 0.002f * std::pow( 50.0f, clamp01( value ) );
}

float WidthParam( float heights )
{
	return clamp01( std::log( std::max( heights, 0.002f ) / 0.002f ) / std::log( 50.0f ) );
}

const char* DetectName( int index )
{
	static const char* const names[ kDetectCount ] = { "Luma", "Alpha", "Chroma", "Luma or Alpha" };
	return names[ std::clamp( index, 0, kDetectCount - 1 ) ];
}

const char* StrategyName( int index )
{
	static const char* const names[ kStrategyCount ] = { "Outside In", "Inside Out" };
	return names[ std::clamp( index, 0, kStrategyCount - 1 ) ];
}

const char* GeometryName( int index )
{
	static const char* const names[ kGeometryCount ] = { "Latch", "Live" };
	return names[ std::clamp( index, 0, kGeometryCount - 1 ) ];
}

const char* ModeName( int index )
{
	static const char* const names[ kModeCount ] = { "Reveal", "Engrave", "Paths", "Field" };
	return names[ std::clamp( index, 0, kModeCount - 1 ) ];
}

const char* FieldModeName( int index )
{
	static const char* const names[ kFieldModeCount ] = { "Glow", "Bevel", "Outline" };
	return names[ std::clamp( index, 0, kFieldModeCount - 1 ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace toolpath::controls
