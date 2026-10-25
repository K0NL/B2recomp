/* [Recomps#1] Darwin only: the OpenGL 1.1 subset nvn_gl.c / nvn_c4.c use from <GL/gl.h> (types, constants, prototypes;
 * values and signatures as in Mesa's gl.h, MIT licence), so the Linux build needs no OpenGL development files. The
 * OpenGL renderer is Windows only (no WGL on Linux: runtime/linux/windows.h answers "no context"); the functions are
 * stand-ins in runtime/linux/gl_stubs.c. Made by a script from the names the two files use; add a name here if a new
 * one is used. */
#ifndef RECOMPS_DARWIN_GL_GL_H
#define RECOMPS_DARWIN_GL_GL_H
#define __gl_h_
#define GLAPI extern
#define GLAPIENTRY
#define APIENTRY
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
#define GL_FALSE 0
#define GL_ALPHA                         0x1906
#define GL_ALPHA_TEST                    0x0BC0
#define GL_ALPHA_TEST_FUNC               0x0BC1
#define GL_ALPHA_TEST_REF                0x0BC2
#define GL_ALWAYS                        0x0207
#define GL_BACK                          0x0405
#define GL_BLEND                         0x0BE2
#define GL_BLUE                          0x1905
#define GL_CCW                           0x0901
#define GL_CLAMP                         0x2900
#define GL_CLEAR                         0x1500
#define GL_COLOR                         0x1800
#define GL_COLOR_BUFFER_BIT              0x00004000
#define GL_COLOR_LOGIC_OP                0x0BF2
#define GL_COLOR_WRITEMASK               0x0C23
#define GL_CULL_FACE                     0x0B44
#define GL_CW                            0x0900
#define GL_DECR                          0x1E03
#define GL_DEPTH_BUFFER_BIT              0x00000100
#define GL_DEPTH_FUNC                    0x0B74
#define GL_DEPTH_TEST                    0x0B71
#define GL_FRONT                         0x0404
#define GL_FRONT_AND_BACK                0x0408
#define GL_GREEN                         0x1904
#define GL_INCR                          0x1E02
#define GL_INVERT                        0x150A
#define GL_KEEP                          0x1E00
#define GL_LESS                          0x0201
#define GL_LINEAR                        0x2601
#define GL_LINEAR_MIPMAP_LINEAR          0x2703
#define GL_LINEAR_MIPMAP_NEAREST         0x2701
#define GL_LINES                         0x0001
#define GL_LINE_LOOP                     0x0002
#define GL_LINE_STRIP                    0x0003
#define GL_NEAREST                       0x2600
#define GL_NEAREST_MIPMAP_LINEAR         0x2702
#define GL_NEAREST_MIPMAP_NEAREST        0x2700
#define GL_NEVER                         0x0200
#define GL_NONE                          0
#define GL_ONE                           1
#define GL_PACK_ALIGNMENT                0x0D05
#define GL_POINT                         0x1B00
#define GL_POINTS                        0x0000
#define GL_POLYGON                       0x0009
#define GL_POLYGON_OFFSET_FILL           0x8037
#define GL_POLYGON_OFFSET_LINE           0x2A02
#define GL_POLYGON_OFFSET_POINT          0x2A01
#define GL_QUADS                         0x0007
#define GL_QUAD_STRIP                    0x0008
#define GL_RED                           0x1903
#define GL_RENDERER                      0x1F01
#define GL_REPEAT                        0x2901
#define GL_REPLACE                       0x1E01
#define GL_RGB                           0x1907
#define GL_RGBA                          0x1908
#define GL_SCISSOR_BOX                   0x0C10
#define GL_SCISSOR_TEST                  0x0C11
#define GL_STENCIL_BUFFER_BIT            0x00000400
#define GL_STENCIL_TEST                  0x0B90
#define GL_TEXTURE_1D                    0x0DE0
#define GL_TEXTURE_2D                    0x0DE1
#define GL_TEXTURE_BORDER_COLOR          0x1004
#define GL_TEXTURE_HEIGHT                0x1001
#define GL_TEXTURE_MAG_FILTER            0x2800
#define GL_TEXTURE_MIN_FILTER            0x2801
#define GL_TEXTURE_WIDTH                 0x1000
#define GL_TEXTURE_WRAP_S                0x2802
#define GL_TEXTURE_WRAP_T                0x2803
#define GL_TRIANGLES                     0x0004
#define GL_TRIANGLE_FAN                  0x0006
#define GL_TRIANGLE_STRIP                0x0005
#define GL_TRUE                          1
#define GL_UNPACK_ALIGNMENT              0x0CF5
#define GL_UNSIGNED_BYTE                 0x1401
#define GL_UNSIGNED_INT                  0x1405
#define GL_UNSIGNED_SHORT                0x1403
#define GL_VENDOR                        0x1F00
#define GL_VERSION                       0x1F02
#define GL_VIEWPORT                      0x0BA2
#define GL_ZERO                          0
GLAPI void GLAPIENTRY glAlphaFunc(GLenum func, GLclampf ref);
GLAPI void GLAPIENTRY glBindTexture(GLenum target, GLuint texture);
GLAPI void GLAPIENTRY glClear(GLbitfield mask);
GLAPI void GLAPIENTRY glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
GLAPI void GLAPIENTRY glClearDepth(GLclampd depth);
GLAPI void GLAPIENTRY glClearStencil(GLint s);
GLAPI void GLAPIENTRY glCullFace(GLenum mode);
GLAPI void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint *textures);
GLAPI void GLAPIENTRY glDepthFunc(GLenum func);
GLAPI void GLAPIENTRY glDepthMask(GLboolean flag);
GLAPI void GLAPIENTRY glDisable(GLenum cap);
GLAPI void GLAPIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
GLAPI void GLAPIENTRY glEnable(GLenum cap);
GLAPI void GLAPIENTRY glFinish(void);
GLAPI void GLAPIENTRY glFlush(void);
GLAPI void GLAPIENTRY glFrontFace(GLenum mode);
GLAPI void GLAPIENTRY glGenTextures(GLsizei n, GLuint *textures);
GLAPI void GLAPIENTRY glGetBooleanv(GLenum pname, GLboolean *params);
GLAPI GLenum GLAPIENTRY glGetError(void);
GLAPI void GLAPIENTRY glGetFloatv(GLenum pname, GLfloat *params);
GLAPI void GLAPIENTRY glGetIntegerv(GLenum pname, GLint *params);
GLAPI const GLubyte * GLAPIENTRY glGetString(GLenum name);
GLAPI void GLAPIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels);
GLAPI void GLAPIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
GLAPI GLboolean GLAPIENTRY glIsEnabled(GLenum cap);
GLAPI void GLAPIENTRY glLineWidth(GLfloat width);
GLAPI void GLAPIENTRY glLogicOp(GLenum opcode);
GLAPI void GLAPIENTRY glPixelStorei(GLenum pname, GLint param);
GLAPI void GLAPIENTRY glPointSize(GLfloat size);
GLAPI void GLAPIENTRY glPolygonMode(GLenum face, GLenum mode);
GLAPI void GLAPIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
GLAPI void GLAPIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void GLAPIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask);
GLAPI void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
GLAPI void GLAPIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
GLAPI void GLAPIENTRY glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
GLAPI void GLAPIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
GLAPI void GLAPIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
#ifdef __cplusplus
}
#endif
#endif
