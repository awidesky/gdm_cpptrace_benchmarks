#pragma once

namespace user {

enum Site {
    kBindTextureError, kCreateShader, kCreateProgram, kCompileShader, kGenBuffers,
    kBindBuffer, kBindVertexArray, kBindTexture, kDrawArrays, kSiteCount
};
extern int g_siteLine[kSiteCount];  // line of each GL call in user_code.cpp (set when it runs)

constexpr int kUserDepth = 32;      // user frames below each GL call site

using SiteFn = void (*)(void*);
void siteBindTextureError(void*);
void siteCreateShader(void*);
void siteCreateProgram(void*);
void siteCompileShader(void*);
void siteGenBuffers(void*);
void siteBindBuffer(void*);
void siteBindVertexArray(void*);
void siteBindTexture(void*);
void siteDrawArrays(void*);
void unbindVertexArray();

void runAtDepth(SiteFn fn, void* arg);

} // namespace user
