#pragma once

#include <FFGLSDK.h>

namespace toolpath
{
/**
    An off-screen buffer for one stage of the chain: a colour texture and a
    framebuffer, and nothing else.

    Not an `ffglex::FFGLFBO`, unlike tinsel's and rebate's PassBuffer, for two
    reasons this plugin hits and they did not.

    **The jump flood stores integer pixel coordinates.** Its buffers are
    RGBA16UI, and `FFGLFBO::GenerateColorTexture()` allocates every texture
    with `GL_RGBA, GL_FLOAT` as the upload format (SDK b1afaf9). For an
    integer internal format that is GL_INVALID_OPERATION even with no data,
    so the texture never exists and the framebuffer is incomplete. Here the
    upload format follows the internal format.

    **Every FFGLFBO carries a 24-bit depth renderbuffer**, which nothing in a
    2D effect reads. At 3840x2160 that is 33 MB per buffer, and the flood
    ping-pongs two. None here.

    What it keeps from the fleet's PassBuffer: it reallocates only when the
    size or format changes; a new buffer is cleared, because undefined texture
    memory in a buffer that feeds back into itself is noise that never washes
    out; and `Destroy()` really deletes the colour texture, which
    `FFGLFBO::Release()` leaks (it tests `depthBufferID` twice where it meant
    `colorTextureID`).

    Nothing here uses an `ffglex::Scoped*` binding for its own work, because
    those clear to 0 on exit instead of restoring: allocating a buffer inside
    one would silently unbind whatever the caller had bound. It saves and
    restores the texture binding, the framebuffer and the viewport by hand.
*/
class PassBuffer
{
public:
	enum class Sampling
	{
		Nearest,///< data read texel for texel with texelFetch. Never filtered.
		Linear  ///< read between texels. Bilinear, no mip chain.
	};

	/// Allocate at this size and internal format, reusing the existing buffer
	/// if it already matches. Returns false if the framebuffer is incomplete.
	bool Ensure( GLsizei width, GLsizei height, GLint internalFormat, Sampling sampling );

	/// Clear to zero (or to `value` in every channel, for float formats).
	void Clear( float value = 0.0f );

	/// Clear an unsigned integer buffer to `value` in every channel.
	void ClearUnsigned( GLuint value );

	void Destroy();

	GLuint TextureID() const
	{
		return texture;
	}
	GLuint FramebufferID() const
	{
		return framebuffer;
	}
	GLsizei Width() const
	{
		return width;
	}
	GLsizei Height() const
	{
		return height;
	}
	bool IsValid() const
	{
		return framebuffer != 0;
	}

	/// Bind this buffer's framebuffer and set the viewport to cover it.
	void BindForDrawing() const;

private:
	GLuint texture     = 0;
	GLuint framebuffer = 0;
	GLsizei width      = 0;
	GLsizei height     = 0;
	GLint format       = 0;
	Sampling sampling  = Sampling::Nearest;
};

} // namespace toolpath
