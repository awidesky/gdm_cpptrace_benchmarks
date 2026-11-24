// A fake GL driver handed to the real glad loader (gladLoadGL). Only the behaviour the
// glutil debug callbacks depend on is modelled; everything else is a no-op.
#include <glutil/gl.hpp>

#include <cstring>

#include "gl_stub.hpp"

namespace stub {

Injection g_inject;
bool g_recordLabels = false;
std::vector<std::string> g_labels;

namespace {
GLenum g_pendingError = GL_NO_ERROR;
GLuint g_nextName = 1000;

void raise(GLenum e) { if (g_pendingError == GL_NO_ERROR) g_pendingError = e; }

GLenum GLAD_API_PTR getError() { GLenum e = g_pendingError; g_pendingError = GL_NO_ERROR; return e; }
const GLubyte* GLAD_API_PTR getString(GLenum n) {
    if (n == GL_VERSION) return reinterpret_cast<const GLubyte*>("4.6.0 stub");
    if (n == GL_SHADING_LANGUAGE_VERSION) return reinterpret_cast<const GLubyte*>("4.60");
    return reinterpret_cast<const GLubyte*>("stub");
}
const GLubyte* GLAD_API_PTR getStringi(GLenum, GLuint) { return reinterpret_cast<const GLubyte*>("GL_KHR_debug"); }
void GLAD_API_PTR getIntegerv(GLenum p, GLint* v) {
    if (v) *v = (p == GL_NUM_EXTENSIONS) ? 1 : 0;
    if (p == GL_CURRENT_PROGRAM && g_inject.getIntegervCurrentProgram) { g_inject.getIntegervCurrentProgram = false; raise(GL_INVALID_ENUM); }
}
void GLAD_API_PTR getShaderiv(GLuint, GLenum p, GLint* v) {
    if (v) *v = (p == GL_COMPILE_STATUS) ? GL_TRUE : 0;
    if (p == GL_COMPILE_STATUS && g_inject.getShaderivCompileStatus) { g_inject.getShaderivCompileStatus = false; raise(GL_INVALID_OPERATION); }
}
void GLAD_API_PTR getProgramiv(GLuint, GLenum, GLint* v) { if (v) *v = 0; }
GLuint GLAD_API_PTR createShader(GLenum) { return g_nextName++; }
GLuint GLAD_API_PTR createProgram() { return g_nextName++; }
GLboolean GLAD_API_PTR isTrue(GLuint) { return GL_TRUE; }
void GLAD_API_PTR objectLabel(GLenum, GLuint, GLsizei length, const GLchar* label) {
    if (g_recordLabels && label) g_labels.emplace_back(label, length < 0 ? std::strlen(label) : static_cast<std::size_t>(length));
    if (g_inject.objectLabel) { g_inject.objectLabel = false; raise(GL_INVALID_VALUE); }
}
void GLAD_API_PTR getObjectLabel(GLenum, GLuint, GLsizei, GLsizei* length, GLchar* label) {
    if (length) *length = 0;
    if (label) *label = '\0';
}
void GLAD_API_PTR noop() {}

struct Entry { const char* name; GLADapiproc fn; };
const Entry kEntries[] = {
    {"glGetError", reinterpret_cast<GLADapiproc>(getError)},
    {"glGetString", reinterpret_cast<GLADapiproc>(getString)},
    {"glGetStringi", reinterpret_cast<GLADapiproc>(getStringi)},
    {"glGetIntegerv", reinterpret_cast<GLADapiproc>(getIntegerv)},
    {"glGetShaderiv", reinterpret_cast<GLADapiproc>(getShaderiv)},
    {"glGetProgramiv", reinterpret_cast<GLADapiproc>(getProgramiv)},
    {"glCreateShader", reinterpret_cast<GLADapiproc>(createShader)},
    {"glCreateProgram", reinterpret_cast<GLADapiproc>(createProgram)},
    {"glIsShader", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glIsProgram", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glIsBuffer", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glIsTexture", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glIsVertexArray", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glIsFramebuffer", reinterpret_cast<GLADapiproc>(isTrue)},
    {"glObjectLabel", reinterpret_cast<GLADapiproc>(objectLabel)},
    {"glGetObjectLabel", reinterpret_cast<GLADapiproc>(getObjectLabel)},
};
} // namespace

void setNextError(GLenum e) { g_pendingError = e; }

GLADapiproc loader(const char* name) {
    for (const Entry& e : kEntries)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return reinterpret_cast<GLADapiproc>(noop);
}

} // namespace stub
