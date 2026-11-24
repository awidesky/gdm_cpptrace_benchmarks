// Compiler portability for the harness (GCC / Clang / MinGW, and MSVC).
//
// BENCH_NOINLINE      the function keeps its own physical frame
// BENCH_FORCEINLINE   the function is folded into its caller (no frame of its own)
// BENCH_AFTER_CALL()  an instruction after the preceding call, so that call is never turned
//                     into a tail call (which would drop the caller's frame from the stack)
// BENCH_KEEP(x)       the same, and keeps x alive (the result of the GL call is "used")
//
// With GCC / Clang these are exactly what the harness always used (asm volatile barriers);
// MSVC has no inline asm on x64, so a store to a volatile sink does the same job.
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
namespace bench_detail {
inline volatile int g_sink = 0;
}
#define BENCH_NOINLINE __declspec(noinline)
#define BENCH_FORCEINLINE __forceinline
#define BENCH_AFTER_CALL() (bench_detail::g_sink = 0)
#define BENCH_KEEP(x) (bench_detail::g_sink = static_cast<int>(x))
#else
#define BENCH_NOINLINE __attribute__((noinline))
#define BENCH_FORCEINLINE inline __attribute__((always_inline))
#define BENCH_AFTER_CALL() asm volatile("")
#define BENCH_KEEP(x) asm volatile("" ::"r"(x))
#endif
