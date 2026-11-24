#pragma once

#include <glutil/gl.hpp>

namespace stub {

// One-shot GL errors raised from inside a GL call that glutil itself makes.
struct Injection {
    bool objectLabel = false;               // glObjectLabel           (C6: via labelGLobject)
    bool getIntegervCurrentProgram = false; // glGetIntegerv(CURRENT_PROGRAM) (C7a: autoPostInspcector:526)
    bool getShaderivCompileStatus = false;  // glGetShaderiv(COMPILE_STATUS)  (C7b: autoLabelGLObjects:338)
};
extern Injection g_inject;

void setNextError(GLenum e);          // error returned by the next glGetError (user GL call fails)
GLADapiproc loader(const char* name); // for gladLoadGL

} // namespace stub
