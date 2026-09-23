#include "PassBuffer.h"

namespace toolpath
{
namespace
{
/// The upload format and type that go with an internal format. For an
/// integer internal format both must be integer too, even with no data.
void uploadFormatFor( GLint internalFormat, GLenum& format, GLenum& type )
{
	switch( internalFormat )
	{
	case GL_RGBA16UI:
		format = GL_RGBA_INTEGER;
		type   = GL_UNSIGNED_SHORT;
		return;
	case GL_R32F:
	case GL_R16F:
		format = GL_RED;
		type   = GL_FLOAT;
		return;
	case GL_RG16F:
		format = GL_RG;
		type   = GL_FLOAT;
		return;
	case GL_RGBA8:
		format = GL_RGBA;
		type   = GL_UNSIGNED_BYTE;
		return;
	default:
		format = GL_RGBA;
		type   = GL_FLOAT;
		return;
	}
}

bool isUnsigned( GLint internalFormat )
{
	return internalFormat == GL_RGBA16UI;
}

/// The bindings a buffer operation disturbs, put back on scope exit.
struct SavedState
{
	GLint framebuffer = 0;
	GLint texture     = 0;
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };

	SavedState()
	{
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &framebuffer );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
		glGetIntegerv( GL_VIEWPORT, viewport );
	}
	~SavedState()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( framebuffer ) );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( texture ) );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	}
};
} // namespace

bool PassBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint internalFormat, Sampling requestedSampling )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( framebuffer != 0 && width == requestedWidth && height == requestedHeight && format == internalFormat
	    && sampling == requestedSampling )
		return true;

	Destroy();

	SavedState saved;

	width    = requestedWidth;
	height   = requestedHeight;
	format   = internalFormat;
	sampling = requestedSampling;

	GLenum uploadFormat = GL_RGBA, uploadType = GL_FLOAT;
	uploadFormatFor( internalFormat, uploadFormat, uploadType );

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, uploadFormat, uploadType, nullptr );
	//An integer texture cannot be filtered at all: anything but NEAREST makes
	//it incomplete, and an incomplete texture samples as zero.
	const GLint filter = ( sampling == Sampling::Linear && !isUnsigned( internalFormat ) ) ? GL_LINEAR : GL_NEAREST;
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glGenFramebuffers( 1, &framebuffer );
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	if( !complete )
	{
		Destroy();
		return false;
	}

	if( isUnsigned( internalFormat ) )
		ClearUnsigned( 0 );
	else
		Clear( 0.0f );
	return true;
}

void PassBuffer::Clear( float value )
{
	if( framebuffer == 0 )
		return;
	SavedState saved;
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
	glClearColor( value, value, value, value );
	glClear( GL_COLOR_BUFFER_BIT );
}

void PassBuffer::ClearUnsigned( GLuint value )
{
	if( framebuffer == 0 )
		return;
	SavedState saved;
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
	const GLuint values[ 4 ] = { value, value, value, value };
	glClearBufferuiv( GL_COLOR, 0, values );
}

void PassBuffer::BindForDrawing() const
{
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
}

void PassBuffer::Destroy()
{
	if( framebuffer != 0 )
	{
		glDeleteFramebuffers( 1, &framebuffer );
		framebuffer = 0;
	}
	if( texture != 0 )
	{
		glDeleteTextures( 1, &texture );
		texture = 0;
	}
	width = height = 0;
	format         = 0;
}

} // namespace toolpath
