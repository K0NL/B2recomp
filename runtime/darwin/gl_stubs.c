/* [Recomps#1] Darwin only: stand-ins for the OpenGL 1.1 functions of runtime/linux/GL/gl.h. The OpenGL renderer never
 * runs on Linux (no context: runtime/linux/windows.h), so nvn_gl.c / nvn_c4.c only need the names to link; without
 * them game_native would need libGL. Each does nothing and answers 0 / "no error". */
#include <GL/gl.h>

void glAlphaFunc(GLenum func, GLclampf ref) { (void)func; (void)ref; }
void glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; }
void glClear(GLbitfield mask) { (void)mask; }
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
void glClearDepth(GLclampd depth) { (void)depth; }
void glClearStencil(GLint s) { (void)s; }
void glCullFace(GLenum mode) { (void)mode; }
void glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }
void glDepthFunc(GLenum func) { (void)func; }
void glDepthMask(GLboolean flag) { (void)flag; }
void glDisable(GLenum cap) { (void)cap; }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { (void)mode; (void)first; (void)count; }
void glEnable(GLenum cap) { (void)cap; }
void glFinish(void) {  }
void glFlush(void) {  }
void glFrontFace(GLenum mode) { (void)mode; }
void glGenTextures(GLsizei n, GLuint *textures) { (void)n; (void)textures; }
void glGetBooleanv(GLenum pname, GLboolean *params) { (void)pname; (void)params; }
GLenum glGetError(void) { return 0; }
void glGetFloatv(GLenum pname, GLfloat *params) { (void)pname; (void)params; }
void glGetIntegerv(GLenum pname, GLint *params) { (void)pname; (void)params; }
const GLubyte * glGetString(GLenum name) { (void)name; return 0; }
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) { (void)target; (void)level; (void)format; (void)type; (void)pixels; }
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) { (void)target; (void)level; (void)pname; (void)params; }
GLboolean glIsEnabled(GLenum cap) { (void)cap; return 0; }
void glLineWidth(GLfloat width) { (void)width; }
void glLogicOp(GLenum opcode) { (void)opcode; }
void glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }
void glPointSize(GLfloat size) { (void)size; }
void glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) { (void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height; }
void glStencilFunc(GLenum func, GLint ref, GLuint mask) { (void)func; (void)ref; (void)mask; }
void glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; }
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)type; (void)pixels; }
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height; }
