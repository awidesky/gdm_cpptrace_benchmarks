// "User" side of the benchmark: the GL call sites, called from under a deep stack.
//
// Every GL call goes through glad's debug function pointer (glXxx -> glad_debug_glXxx),
// exactly like user code linked against gdm. The call sites are non-inlined, non-tail
// functions in their own TU, and sit kUserDepth frames above runAtDepth(), so new's
// kInternalScanDepth + depth capture is always filled with real frames.
#include <glutil/gl.hpp>

#include "portable.hpp"
#include "user_code.hpp"

namespace user {

int g_siteLine[kSiteCount];
#define SITE(id) g_siteLine[id] = __LINE__  // same line as the GL call -> expected line

static GLuint g_nextId = 100;

BENCH_NOINLINE void siteBindTextureError(void*) { glBindTexture(GL_TEXTURE_2D, 99999); SITE(kBindTextureError); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void siteCreateShader(void*)     { GLuint s = glCreateShader(GL_VERTEX_SHADER); SITE(kCreateShader); BENCH_KEEP(s); }
BENCH_NOINLINE void siteCreateProgram(void*)    { GLuint p = glCreateProgram(); SITE(kCreateProgram); BENCH_KEEP(p); }
BENCH_NOINLINE void siteCompileShader(void*)    { glCompileShader(g_nextId++); SITE(kCompileShader); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void siteGenBuffers(void*)       { GLuint b = g_nextId++; glGenBuffers(1, &b); SITE(kGenBuffers); BENCH_KEEP(b); }
BENCH_NOINLINE void siteBindBuffer(void*)       { glBindBuffer(GL_ARRAY_BUFFER, g_nextId++); SITE(kBindBuffer); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void siteBindVertexArray(void*)  { glBindVertexArray(g_nextId++); SITE(kBindVertexArray); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void siteBindTexture(void*)      { glBindTexture(GL_TEXTURE_2D, g_nextId++); SITE(kBindTexture); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void siteDrawArrays(void*)       { glDrawArrays(GL_TRIANGLES, 0, 3); SITE(kDrawArrays); BENCH_AFTER_CALL(); }
BENCH_NOINLINE void unbindVertexArray()         { glBindVertexArray(0); BENCH_AFTER_CALL(); }

// A deep, non-inlined, non-tail call chain standing in for the application's stack
// (main -> app -> render loop -> scene -> ...).
template <int N> BENCH_NOINLINE void level(SiteFn fn, void* arg) { level<N - 1>(fn, arg); BENCH_AFTER_CALL(); }
template <> BENCH_NOINLINE void level<0>(SiteFn fn, void* arg) { fn(arg); BENCH_AFTER_CALL(); }

void runAtDepth(SiteFn fn, void* arg) { level<kUserDepth>(fn, arg); }

} // namespace user
