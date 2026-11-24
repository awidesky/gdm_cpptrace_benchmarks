// "User" side of the benchmark: the GL call sites, called from under a deep stack.
//
// Every GL call goes through glad's debug function pointer (glXxx -> glad_debug_glXxx),
// exactly like user code linked against gdm. The call sites are non-inlined, non-tail
// functions in their own TU, and sit kUserDepth frames above runAtDepth(), so new's
// kInternalScanDepth + depth capture is always filled with real frames.
#include <glutil/gl.hpp>

#include "user_code.hpp"

namespace user {

int g_siteLine[kSiteCount];
#define SITE(id) g_siteLine[id] = __LINE__  // same line as the GL call -> expected line

static GLuint g_nextId = 100;

__attribute__((noinline)) void siteBindTextureError(void*) { glBindTexture(GL_TEXTURE_2D, 99999); SITE(kBindTextureError); asm volatile(""); }
__attribute__((noinline)) void siteCreateShader(void*)     { GLuint s = glCreateShader(GL_VERTEX_SHADER); SITE(kCreateShader); asm volatile("" :: "r"(s)); }
__attribute__((noinline)) void siteCreateProgram(void*)    { GLuint p = glCreateProgram(); SITE(kCreateProgram); asm volatile("" :: "r"(p)); }
__attribute__((noinline)) void siteCompileShader(void*)    { glCompileShader(g_nextId++); SITE(kCompileShader); asm volatile(""); }
__attribute__((noinline)) void siteGenBuffers(void*)       { GLuint b = g_nextId++; glGenBuffers(1, &b); SITE(kGenBuffers); asm volatile("" :: "r"(b)); }
__attribute__((noinline)) void siteBindBuffer(void*)       { glBindBuffer(GL_ARRAY_BUFFER, g_nextId++); SITE(kBindBuffer); asm volatile(""); }
__attribute__((noinline)) void siteBindVertexArray(void*)  { glBindVertexArray(g_nextId++); SITE(kBindVertexArray); asm volatile(""); }
__attribute__((noinline)) void siteBindTexture(void*)      { glBindTexture(GL_TEXTURE_2D, g_nextId++); SITE(kBindTexture); asm volatile(""); }
__attribute__((noinline)) void siteDrawArrays(void*)       { glDrawArrays(GL_TRIANGLES, 0, 3); SITE(kDrawArrays); asm volatile(""); }
__attribute__((noinline)) void unbindVertexArray()         { glBindVertexArray(0); asm volatile(""); }

// A deep, non-inlined, non-tail call chain standing in for the application's stack
// (main -> app -> render loop -> scene -> ...).
template <int N> __attribute__((noinline)) void level(SiteFn fn, void* arg) { level<N - 1>(fn, arg); asm volatile(""); }
template <> __attribute__((noinline)) void level<0>(SiteFn fn, void* arg) { fn(arg); asm volatile(""); }

void runAtDepth(SiteFn fn, void* arg) { level<kUserDepth>(fn, arg); }

} // namespace user
