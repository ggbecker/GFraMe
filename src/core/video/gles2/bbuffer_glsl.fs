/**
 * Fragment shader for rendering the backbuffer
 *
 * @file src/core/video/gles2/bbuffer_glsl.vs
 */

"#version 100\n"

/** The current texture */
"uniform sampler2D gSampler;\n"

/** Texture coordinate, retrieved from vertex shader */
"varying mediump vec2 texCoord;\n"

"void main() {\n"
    /* Simply retrieve the color from the texture coordinate */
"    gl_FragColor = texture2D(gSampler, texCoord.st);\n"
"}"
