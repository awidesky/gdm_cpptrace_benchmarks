# gdm 스택 프레임 스킵 사용처 전수조사 및 벤치마크 재설계 플랜

- 대상: gdm `external/glutil` (old = `358d42c^`, new = `HEAD` = `358d42c` + `05402ab`, 워킹트리의 `kInternalScanDepth = 6` 미커밋 변경 포함)
- 검증 환경: macOS (Darwin 24.6), arm64, Apple clang 17.0.0, cpptrace 1.0.4 (homebrew, gdm 로컬 빌드도 동일하게 `/opt/homebrew` 사용)
- 검증 방법: **실제 glutil 소스 전체 + 실제 glad DEBUG `gl.c` + GL 드라이버 stub**으로 프로브 바이너리를 만들어,
  각 호출 지점에서 실제 스택을 덤프함. `debug_stacktrace.cpp`만 덤프용으로 교체하고 나머지는 원본 그대로 사용.
  빌드는 gdm의 Debug(`-O0 -g`)와 RelWithDebInfo(`-O2 -g -DNDEBUG`) 플래그와 같음.
  프로브 소스 위치: `/private/tmp/claude-501/-Users-eugenehong-git-gdm-benchmarks/9402c3b5-f4e4-4b0d-bad9-bff82444340b/scratchpad/probe/src/`

---

## 1. 공통 메커니즘

### 1.1 old (358d42c^)
```cpp
// debug_stacktrace.hpp 기본값: printStackTrace/stringStackTrace skip=3, getCalledGLfunctionName skip=4
.print(out, cpptrace::stacktrace::current(skip, depth));          // printStackTrace / stringStackTrace
cpptrace::stacktrace trace = cpptrace::stacktrace::current(skip, 1); // getCalledGLfunctionName
```
- cpptrace의 `skip`과 `max_depth`는 **raw(물리) 프레임 단위**로 적용됨. 스킵된 프레임은 **resolve(심볼, DWARF, demangle)되지 않음**.
  - 프로브로 확인함: O2에서 `autoPostInspcector`처럼 인라인된 프레임은 skip 개수에 포함되지 않음.
- 그래서 skip 값은 "호출 지점부터 사용자 GL 호출까지의 **물리** 프레임 수"와 정확히 같아야 함.
  이 값은 인라인 여부(최적화 레벨)와 호출 경로에 따라 달라짐.

### 1.2 new (358d42c + 05402ab)
```cpp
cpptrace::stacktrace trace = cpptrace::stacktrace::current(0, captureDepth(skip, depth)); // = kInternalScanDepth + skip + depth
keepCallerFrames(trace, skip, depth);  // 맨 위부터 isInternalFrame()이 연속으로 true인 프레임을 제거, extraSkip만큼 더 제거, depth로 자름
```
- `isInternalFrame` prefix: `glutil::debug::callbacks::`, `glutil::debug::printStackTrace`, `glutil::debug::stringStackTrace`,
  `glutil::debug::getCalledGLfunctionName`, `_glad_debug_impl_`
- 맨 위 프레임이 internal이 아니면(심볼 없음) false를 반환함. printStackTrace는 raw trace를 출력하고, getCalledGLfunctionName은 `"unknown source location"`을 반환함.

### 1.3 비용 구조의 차이
캡처할 스택이 충분히 깊다는 전제에서, 내부 프레임 수를 N이라고 할 때:

|  | unwind하는 raw 프레임 | resolve하는 프레임 | 출력/스니펫 |
|---|---|---|---|
| old | skip + depth | **depth** | depth |
| new | kInternalScanDepth + depth | **kInternalScanDepth + depth** (N개 internal + (6−N)개 더 아래쪽 사용자 프레임) | depth |

- new는 **모든 경우에 old보다 정확히 `kInternalScanDepth`(=6)개 프레임을 더 resolve**함. N과는 상관없음.
  추가 unwind는 `6 − N`개.
- 따라서 오버헤드를 결정하는 "스킵되는 프레임의 특성"은 함수 본문의 복잡도(switch, va_list, try-catch)가 **아님**.
  결정 요인은 **추가로 resolve되는 6개 프레임이 어느 CU/바이너리에 있는지, DWARF 크기, 인라인 체인 길이, demangle할 이름의 길이**임.
- 덧붙여 `keepCallerFrames`의 prefix 비교(프레임당 최대 5회 `string_view` 비교)가 있음. resolve 비용에 비하면 무시할 만한 수준으로 예상됨.

---

## 2. 호출 지점 전수 목록

`git grep`으로 old와 new 모두에서 `printStackTrace|stringStackTrace|getCalledGLfunctionName|stacktrace::current`를 찾음.

| # | 파일:라인 | 함수 | API | old skip | new skip | 경로 ID |
|---|---|---|---|---|---|---|
| 1 | `src/debug_callback.cpp:32` | `checkGLErrorPostCallback` | printStackTrace | 3 (기본값) | 0 | C1, C6, C7 |
| 2 | `src/debug_callback.cpp:327` | `autoLabelGLObjects` / CreateShader | getCalledGLfunctionName | 4 (기본값) | 0 | C4 |
| 3 | `src/debug_callback.cpp:332` | `autoLabelGLObjects` / CreateProgram | 〃 | 4 | 0 | C4 |
| 4 | `src/debug_callback.cpp:345` | `autoLabelGLObjects` / CompileShader | 〃 | 4 | 0 | C4 |
| 5 | `src/debug_callback.cpp:407` | `autoLabelGLObjects` / GenBuffers·GenVertexArrays·GenTextures | 〃 | 4 | 0 | C4 |
| 6 | `src/debug_callback.cpp:426` | `autoLabelGLObjects` / BindBuffer | 〃 | 4 | 0 | C4 |
| 7 | `src/debug_callback.cpp:437` | `autoLabelGLObjects` / BindVertexArray | 〃 | 4 | 0 | C4 |
| 8 | `src/debug_callback.cpp:450` | `autoLabelGLObjects` / BindTexture | 〃 | 4 | 0 | C4 |
| 9 | `src/debug_callback.cpp:522` | `autoPostInspcector` / DrawArrays·DrawElements, VAO 없음 | 〃 | 4 | 0 | C5 |
| 10 | `src/debug_callback.cpp:529` | `autoPostInspcector` / DrawArrays·DrawElements, program 없음 | 〃 | 4 | 0 | C5 |
| 11 | `src/debug_callback.cpp:661` | `fullPostCallback` 에러 경로 (hashError) | getCalledGLfunctionName | **3 (명시)** | 0 | C2 |
| 12 | `src/debug_callback.cpp:671` | `fullPostCallback` 에러 경로 | printStackTrace | 3 (기본값) | 0 | C3 |

제외한 항목:
- `stringStackTrace`: 코드베이스 안에 호출하는 곳이 없음.
- `example/stacktrace.cpp:24`의 `printStackTrace()`: `#if !GDM_DEBUG` 블록 안에 있음. 이 빌드에서 printStackTrace는 아무것도 하지 않는 inline stub(`debug_stacktrace.hpp:93`)이라 cpptrace를 쓰지 않음.

---

## 3. 경로별 상세 (프로브 실측)

표기 규칙: `[i]`는 resolve된 프레임 인덱스, `pK`는 물리 프레임 번호, `inline`은 인라인 프레임(물리 프레임 아님).
"정답 skip"은 사용자 GL 호출 지점에 도달하는 old skip 값.

### 왜 skip이 필요한가 (공통)
- **printStackTrace**: GL 에러가 났을 때 출력하는 스택이 glutil/glad 내부가 아니라 **사용자의 GL 호출 지점에서 시작**해야 함.
- **getCalledGLfunctionName**: 객체 라벨(`Shader#7(main.cpp:30 -> glCreateShader(...))`), 경고 메시지, 에러 해시 키에
  **사용자의 GL 호출 코드 한 줄**(file:line과 소스 스니펫)이 필요함. 맨 위 1프레임만 쓰기 때문에 skip이 틀리면 결과가 곧바로 틀림.

### C1. 사용자 GL 호출 에러 → `checkGLErrorPostCallback` → printStackTrace (사이트 1)
- 진입 조건: callback 모드가 `errorCheck`일 때(사용자가 `setGladDebugCallbacks(errorCheck)`를 호출한 경우).
  full 모드 안에서 들어오는 경우는 C6/C7 참고.
```
O0 / O2 동일
[0] p0 glutil::debug::printStackTrace(...)                        debug_stacktrace.cpp
[1] p1 glutil::debug::callbacks::checkGLErrorPostCallback(...)    debug_callback.cpp:32
[2] p2 glad_debug_impl_glBindTexture                              gl.c:266
[3] p3 userBindTexture()        ← 정답
```
- 내부 프레임 3개(O0, O2 모두). **old skip=3이 정답**: `current(3,1)`이 `userBindTexture()`에 도달함. skip=4는 하나 더 넘어가서 `renderFrame`에 도달함.
- new: prefix `glad_debug_impl_`이면 정답. **현재 prefix `_glad_debug_impl_`이면 glad 래퍼에서 멈춤(버그, §5-B1)**.

### C2. `fullPostCallback` 에러 경로 → getCalledGLfunctionName (사이트 11)
```
[0] p0 glutil::debug::getCalledGLfunctionName(int)
[1] p1 glutil::debug::callbacks::fullPostCallback(...)            debug_callback.cpp:661
[2] p2 glad_debug_impl_glBindTexture
[3] p3 userBindTexture()        ← 정답
```
- 내부 프레임 3개(O0, O2). old에서 명시한 skip=3이 정답.

### C3. `fullPostCallback` 에러 경로 → printStackTrace (사이트 12)
- 프레임 구성은 C2와 같음(맨 위만 printStackTrace, 라인 671). 내부 프레임 3개, old skip=3이 정답.
- 주의: `logErrorOccurrence`(`hashing.hpp:74`)가 **같은 에러가 1초 안에 반복되면 억제**함(`report.count == 0`이면 return).
  그래서 루프로 반복하는 벤치마크에서는 671이 호출되지 않음. 661은 매번 호출됨.

### C4. `autoLabelGLObjects` → getCalledGLfunctionName (사이트 2~8, 7곳)
- 진입 조건: full 모드에서 에러가 없는 경로. `fullPostCallback:689`에서 `autoLabelGLObjects`를 호출함.
```
O0 / O2 동일 (O2에서도 autoLabelGLObjects는 인라인되지 않음: 크고 별도 심볼 존재)
[0] p0 glutil::debug::getCalledGLfunctionName(int)
[1] p1 glutil::debug::callbacks::autoLabelGLObjects(...)          debug_callback.cpp:327
[2] p2 glutil::debug::callbacks::fullPostCallback(...)            debug_callback.cpp:689
[3] p3 glad_debug_impl_glCreateShader
[4] p4 userCreateShader()       ← 정답
```
- 내부 프레임 4개. old skip=4가 정답. 7곳 모두 프레임 구조가 같고 라인/GL 함수만 다름.

### C5. `autoPostInspcector` → getCalledGLfunctionName (사이트 9, 10)
```
O0                                                     O2
[0] p0 getCalledGLfunctionName                         [0] p0 getCalledGLfunctionName
[1] p1 callbacks::autoPostInspcector  :522             [1] inline callbacks::autoPostInspcector :522   ← fullPostCallback에 인라인됨
[2] p2 callbacks::fullPostCallback    :690             [2] p1 callbacks::fullPostCallback       :690
[3] p3 glad_debug_impl_glDrawArrays                    [3] p2 glad_debug_impl_glDrawArrays
[4] p4 userDraw()   ← 정답                             [4] p3 userDraw()   ← 정답
```
- 내부 프레임: O0에서 물리 4개, O2에서 물리 3개 + inline 1개.
- **old skip=4는 O2(RelWithDebInfo)에서 틀림**: `renderFrame`, 즉 사용자 호출자의 호출자에 도달함. O2에서 정답 skip은 3.
  "release 계열에서 old가 동작하지 않는다"는 현상의 실제 원인이 이것임.
- new는 인라인 프레임도 `glutil::debug::callbacks::autoPostInspcector` 이름으로 resolve되기 때문에 O0, O2 모두 정답.

### C6. (중첩) glutil 헬퍼 안의 GL 호출 에러 → `checkGLErrorPostCallback` → printStackTrace (사이트 1)
- 진입 조건: `fullPostCallback`은 에러가 없는 경로에서 `setGladDebugCallbacks(errorCheck)`로 바꾼 다음
  track/autoLabel/autoPost를 실행함(`debug_callback.cpp:680`). 이 안에서 **callbacks 네임스페이스 밖의 glutil 함수**가
  GL을 호출하다 에러가 나는 경우임. 예: `labelGLobject`→`glObjectLabel`, `getGLobjectLabel`→`glGetObjectLabel`,
  `Inspector::*`, errorSnapshot의 `Snapshot` 등.
```
O0 / O2 동일
[0] p0 printStackTrace
[1] p1 callbacks::checkGLErrorPostCallback      :32
[2] p2 glad_debug_impl_glObjectLabel
[3] p3 glutil::debug::labelGLobject(...)        debug.cpp:139   ← old/new 모두 여기서 멈춤 (glutil 자신의 GL 호출 지점)
[4] p4 callbacks::autoLabelGLObjects            :325
[5] p5 callbacks::fullPostCallback              :689
[6] p6 glad_debug_impl_glCreateShader
[7] p7 userCreateShader()
```
- 내부 프레임 3개. old skip=3과 new의 결과가 같음(`labelGLobject`). new 주석에 적힌 의도("glutil's own code that calls GL ... reported at their own location")와도 일치함.

### C7. (중첩) callbacks 네임스페이스 안의 직접 GL 호출 에러 → `checkGLErrorPostCallback` → printStackTrace (사이트 1)
- 해당 GL 호출: `autoPostInspcector:526 glGetIntegerv`, `autoLabelGLObjects:338/343/368 glGetShaderiv`, `:354/384 glGetProgramiv`, `:361 glGetAttachedShaders`
```
O0 (autoPostInspcector:526)                           O2
[0] p0 printStackTrace                                [0] p0 printStackTrace
[1] p1 callbacks::checkGLErrorPostCallback :32        [1] p1 callbacks::checkGLErrorPostCallback :32
[2] p2 glad_debug_impl_glGetIntegerv                  [2] p2 glad_debug_impl_glGetIntegerv
[3] p3 callbacks::autoPostInspcector :526             [3] inline callbacks::autoPostInspcector :526
[4] p4 callbacks::fullPostCallback   :690             [4] p3 callbacks::fullPostCallback :690
[5] p5 glad_debug_impl_glDrawArrays                   [5] p4 glad_debug_impl_glDrawArrays
[6] p6 userDraw()  ← new의 결과                       [6] p5 userDraw()  ← new의 결과
```
- 내부 프레임 6개(O2에서는 물리 5개 + inline 1개). **코드베이스 전체에서 최대값**.
- old skip=3은 `autoPostInspcector`(glutil 내부)에 멈춤. new와 같은 결과를 내려면 skip이 O0에서 6, O2에서 5여야 함.
  하나의 사이트(32행)를 C1(3), C6(3), C7(5/6)이 함께 쓰므로 **old 방식으로는 원리적으로 세 경로를 동시에 맞출 수 없음**.
- `autoLabelGLObjects` 안에서 직접 GL을 호출하는 경우는 직접 프로브하지 않았음. `autoLabelGLObjects`가 O2에서도 인라인되지 않으므로 O0, O2 모두 내부 프레임 6개, 정답 skip 6으로 추정함.

---

## 4. 스킵되는 프레임 종류별 특성

| 프레임 | 소속 TU / 바이너리 | 링키지·호출 방식 | O0 | O2 | resolve 비용 관련 특성 |
|---|---|---|---|---|---|
| `glutil::debug::printStackTrace` | `debug_stacktrace.cpp` → libglutil.a(정적, 실행 파일에 링크) | 외부 함수, 다른 TU에서 직접 호출 | 물리 | 물리(LTO 없음) | C++ mangled, 시그니처가 김(`basic_ostream`, `basic_string`, `ColorMode`) → demangle 비용 큼 |
| `glutil::debug::getCalledGLfunctionName` | 〃 | 〃 | 물리 | 물리 | 이름이 짧음(`(int)`) |
| `callbacks::checkGLErrorPostCallback` | `debug_callback.cpp` (큰 TU, include 많음) | `static`, **variadic**, glad가 **함수 포인터**(`_post_call_gl_callback`)로 호출 → 인라인 불가 | 물리 | 물리 + `.cold.1` 분리 | 콜백 CU의 DWARF |
| `callbacks::fullPostCallback` | 〃 | 〃 | 물리 | 물리 + `.cold.1~21` 분리, `trackGLFunctions`/`autoPostInspcector`/`errorSnapshot`이 안으로 인라인됨 | 인라인 서브루틴이 많은 함수 → inline 체인 탐색이 필요 |
| `callbacks::autoLabelGLObjects` | 〃 | `static`, `va_list` 인자, 호출 지점 1곳, 큰 함수 | 물리 | **물리**(인라인 안 됨) | |
| `callbacks::autoPostInspcector` | 〃 | `static`, `va_list` 인자, 호출 지점 1곳 | 물리 | **inline 프레임** | old skip 계산을 깨뜨리는 원인 |
| `glad_debug_impl_glXxx` | glad DEBUG `gl.c` (C, 7,634줄, 래퍼 657개) → libgdm_glad_debug.a | `static`. 사용자는 매크로 `glXxx` → `glad_debug_glXxx` **함수 포인터**로 호출 → 인라인 불가. pre/post 콜백도 함수 포인터 + variadic | 물리 | **물리**(arm64에서는 variadic 인자가 스택으로 가기 때문에 post 콜백 호출이 tail call이 되지 않음, 역어셈블로 확인) | C 심볼이라 demangle 없음. **이름: DWARF가 있으면 `glad_debug_impl_X`, 심볼 테이블만 있으면 `_glad_debug_impl_X`(Mach-O 앞 `_`)** |
| `glutil::debug::labelGLobject` 등 (debug.cpp) | debug.cpp | 외부 함수 | 물리 | 물리 | internal 목록에 없음 → C6에서 경계 역할 |
| 사용자 프레임 | 사용자 TU | | | | new가 아래쪽으로 `6 − N`개를 더 resolve함 (C1~C3: 3개, C4: 2개, C5: 2개, C7: 0개) |

추가로 확인한 사항:
- O2에서 함수 본문을 hot/cold로 분리함(`.cold.N`). DWARF로 resolve하면 원래 함수 이름이 나오므로(프로브 C3 O2에서 `fullPostCallback :671`) prefix 매칭에는 문제가 없음.
- 플랫폼 의존성: x86-64 SysV에서는 variadic 인자가 레지스터로 전달되므로 glad 래퍼의 post 콜백 호출이 tail call이 될 수 있음. 그러면 래퍼 프레임이 사라짐. Windows/MSVC도 다를 수 있음. **이 문서는 macOS arm64에서만 검증했음.**

---

## 5. 코드베이스 버그 / 테스트 시 수정 사항

### B1. [new] glad prefix `_glad_debug_impl_`가 DWARF 환경에서 매칭되지 않음 (05402ab의 회귀)
- 실측 결과: 일반적인 Debug/RelWithDebInfo 빌드에서는 DWARF가 있으므로(.o debug map 또는 dSYM) cpptrace가 `glad_debug_impl_glBindTexture`를 보고함.
  `.o`를 치워서 심볼 테이블로만 resolve되게 한 경우에만 `_glad_debug_impl_glBindTexture`가 나옴.
- 결과적으로 현재 HEAD는 **모든 경로에서 glad 래퍼를 사용자 프레임으로 판정함**:
  getCalledGLfunctionName은 `gl.c:NNN`을 반환하고, printStackTrace는 glad 래퍼부터 출력함.
- 수정: 두 형태를 모두 허용함(`glad_debug_impl_`와 `_glad_debug_impl_`). 예를 들어 비교 전에 맨 앞의 `_` 하나를 무시하거나, prefix를 두 개 모두 등록함.
- **적용 완료 (커밋 안 함)**: `../gdm/external/glutil/src/debug_stacktrace.cpp`와 `test/glutil_callstack_replica.hpp`의 prefix 목록에 두 형태를 모두 등록함.
  수정한 실제 `debug_stacktrace.cpp`를 프로브에 링크해서 검증함. O0과 O2 모두 C1~C7이 의도한 프레임(사용자 호출 지점, C6은 `labelGLobject`)부터 출력되거나 반환됨.

### B2. [old] `autoPostInspcector`(사이트 9, 10) skip=4가 O2에서 하나 더 넘어감
- 테스트용 old 값: Debug 4, RelWithDebInfo 3.

### B3. [old] 사이트 1(checkGLErrorPostCallback)은 C7 경로에서 skip=3이 틀림
- 테스트용 old 값: C7 경로에 한해 Debug 6, RelWithDebInfo 5(autoPostInspcector:526). C1과 C6에는 3이 맞음.

### B4. [new] `kInternalScanDepth` 주석이 틀림
- 워킹트리 주석은 "Maximum internal frames in any call path: 4 (getCalledGLfunctionName from errorSnapshot)"임.
  그런데 errorSnapshot에서 getCalledGLfunctionName을 호출하는 경로는 없고, 실제 최대값은 **6(C7)**임.
  값 6은 정확히 하한이라 여유가 0임. 주석의 "2 frames of safety margin"은 사실이 아님.
- 기능 영향: internal이 6이면 `captureDepth = 6 + 0 + 15 = 21` → 15프레임이 그대로 나오므로 현재는 동작함.

### B5. (확인 요청) "checkGLErrorPostCallback의 old skip=3이 glad 래퍼를 잡는다"
- 이 환경의 O0과 O2에서는 재현되지 않음. C1에서 skip=3은 사용자 호출 지점에 정확히 도달하고, skip=4는 하나 더 넘어감.
- 관찰했던 상황이 B1(new + prefix 불일치), C7(old skip=3 → callbacks 프레임), 또는 다른 플랫폼일 가능성이 있음.
- 벤치마크에서는 이 값을 믿음에 의존해 하드코딩하지 않음. **검증 단계에서 각 (경로, 빌드)마다 정답 skip을 실측으로 결정함(§6-4)**.

---

## 6. 벤치마크 재설계 플랜

### 목표 (재확인)
**같은 실제 스택에서, old(정답 skip을 하드코딩한 경우)와 동일한 결과를 내는 new의 추가 오버헤드**를 경로와 빌드별로 측정함.
케이스는 §2와 §3의 경로만 다루고 그 밖의 케이스는 추가하지 않음.

### 6-1. 하네스 구조: lookalike를 버리고 실제 코드를 빌드
- `CMakeLists.txt`에 `GDM_SOURCE_DIR`(기본값 `../gdm`) 옵션을 둠. 다음 파일을 **원본 그대로** 컴파일함:
  `external/glutil/src/*.cpp`(단 `debug_stacktrace.cpp`는 제외), `external/glad-gl4.6core_DEBUG/src/gl.c`.
  gdm의 compile definition을 그대로 씀(`GDM_DEBUG=1 GDM_HAS_GLAD GDM_HAS_GLFW ...`). glfw는 링크만 함(호출하지 않음).
- `bench_stacktrace.cpp`가 `debug_stacktrace.cpp`를 대체함. 같은 심볼(`glutil::debug::printStackTrace` 등)에서
  런타임 스위치 `g_method ∈ {Old, New}`로 분기함:
  - Old 분기: `358d42c^` 본문을 그대로 옮김. skip은 인자 대신 "현재 경로의 정답 skip"(하네스가 설정)을 씀.
  - New 분기: HEAD 본문을 그대로 옮기고 B1만 수정함. `kInternalScanDepth`는 gdm 값을 씀.
  - 같은 함수, 같은 심볼, 같은 스택이므로 분기 외에는 차이가 없음. old와 new를 한 프로세스에서 interleave할 수 있음.
- GL stub 로더: `gladLoadGL(loader)`. `glGetError`(값 주입 가능), `glGetString(GL_VERSION)`="4.6", `glGetIntegerv`, `glGetStringi`,
  `glCreateShader/Program`, `glIs*`=TRUE, 나머지는 no-op. 실측 프로브(scratchpad)에서 이미 동작을 확인함.
- 사용자 코드: 별도 TU(`-g`)에 noinline이면서 tail call이 아닌 함수 체인을 둠. **GL 호출 지점 아래로 25프레임 이상** 쌓음.
  그래야 new의 `kInternalScanDepth + depth`(최대 21)가 모두 실제 프레임으로 채워짐. 지금 하네스처럼 스택이 얕으면 new의 추가 비용이 측정에서 빠짐.

### 6-2. 케이스 (12개 사이트 → 7개 경로)
| 경로 | 트리거 (하네스에서) | 사이트 | API |
|---|---|---|---|
| C1 | errorCheck 모드 + 사용자 `glBindTexture` 에러 | 1 | print |
| C2 | full 모드 + 사용자 `glBindTexture` 에러 | 11 | getCalled |
| C3 | 〃 | 12 | print |
| C4 | full 모드 + `glCreateShader`/`glCreateProgram`/`glCompileShader`/`glGen*`/`glBindBuffer`/`glBindVertexArray`/`glBindTexture` | 2~8 (7곳 모두) | getCalled |
| C5 | full 모드 + VAO와 program 없이 `glDrawArrays` | 9, 10 | getCalled |
| C6 | full 모드 + `glObjectLabel` stub이 에러 주입 | 1 | print |
| C7 | full 모드 + `glGetIntegerv(GL_CURRENT_PROGRAM)` 에러 주입 (+ autoLabel 안의 `glGetShaderiv` 에러 주입) | 1 | print |

### 6-3. 빌드 행렬
- Debug(`-O0 -g`)와 RelWithDebInfo(`-O2 -g -DNDEBUG`). gdm에서 `GDM_DEBUG=1`이 되는 두 구성임.
- 여기에 최적화 레벨 스윕을 추가함: `-O1 -g`, `-O3 -g`, `-Os -g` (모두 `GDM_DEBUG=1`). 인라인 결정이 레벨마다 다르므로 경로별 N과 old 정답 skip이 레벨마다 달라질 수 있음.
- old 하드코딩 값: (경로 × 최적화 레벨) 테이블을 하네스에 하드코딩함. 하네스는 케이스를 트리거하기 직전에
  `g_oldSkipPrint`와 `g_oldSkipGetCalled` 전역 값을 설정함. old 분기는 인자 대신 이 값을 씀.
  전역 int 하나를 읽는 비용은 측정에 영향을 주지 않음. `debug_callback.cpp`는 수정하지 않음.
  테이블 값은 `--verify`가 실측한 값과 일치해야 하며, 일치하지 않으면 실패로 처리함.
- C7의 목표 프레임은 new의 결과(사용자 호출 지점)로 정함. callbacks 네임스페이스는 전부 internal이라는 new의 설계를 따른 것임.
- cpptrace는 gdm과 같은 방식으로 찾음(`gdm_use_or_fetch_package`, 현재 homebrew 1.0.4). 어떤 cpptrace를 썼는지 결과에 기록함.
- dSYM은 만들지 않음(CMake 기본 흐름과 같은 .o debug map 방식).

### 6-4. 검증 단계 (`--verify`, 타이밍보다 먼저 실행, 실패하면 exit≠0이고 ctest도 실패)
1. 각 경로에서 `current(0, 40)` 전체 프레임을 덤프함(물리/inline 구분).
2. 기대 프레임(사용자 TU의 file:line, C6은 `debug.cpp:139`)을 기준으로 **old 정답 skip을 실측으로 계산**함.
   그 값을 (경로, 빌드) 테이블에 고정하고, 다음 실행부터는 테이블 값을 검증함.
3. 같은 값으로 Old 분기 결과와 New 분기 결과를 비교함:
   printStackTrace는 출력할 프레임 목록이 일치해야 하고, getCalledGLfunctionName은 반환 문자열이 일치해야 함.
   일치하지 않으면 그 케이스는 타이밍하지 않고 실패로 처리함.
4. `keepCallerFrames`가 true를 반환하는지, internal 개수 N도 기록함.

### 6-5. 측정 방법
- **측정 구간을 API 함수 내부에 계측**함(같은 실제 스택에서 호출되므로 스택 특성은 그대로 유지됨):
  - T_trace: 함수 진입부터 "출력할 프레임이 준비될 때까지". Old는 `current(skip, depth)`, New는 `current(0, captureDepth)` + `keepCallerFrames`.
  - T_total: 함수 진입부터 반환까지(포맷, 스니펫, 문자열 생성 포함).
  - 보조 지표: 사용자 GL 호출 한 번의 end-to-end 시간(Logger, 라벨링 등 공통 비용 포함).
- 출력 싱크: `Logger::errLogger().setOutput(&nullStream)`과 outLogger 모두 null 스트림으로 바꿈. 스트림 생성을 측정 구간 밖으로 뺌.
- C3 억제 회피: 하네스 전용 패치로 `logErrorOccurrence` 억제를 끔(`-DGLUTIL_BENCH_NO_ERROR_SUPPRESS` 또는 `hashing.hpp` 대체 include).
  이 패치는 결과 문서에 명시함.
- Warm: 워밍업 후 (Old, New)를 번갈아 B개 배치 × M회 실행함. median, p10, p90, Δ=New−Old, 비율을 보고함.
- Cold: 케이스와 방법마다 새 프로세스를 띄워 첫 호출만 측정함(cpptrace의 DWARF와 심볼 캐시가 비어 있는 상태). 프로세스 30회 실행의 median을 보고함.
  (getCalledGLfunctionName은 앱 시작 시 객체를 생성하면서 주로 호출되므로 cold와 semi-warm 비용이 실사용에 가까움.)
- 결과 표: (경로 × 빌드) 행마다 N, old skip, T_trace(old/new/Δ), T_total(old/new/Δ), cold(old/new/Δ).

### 6-6. 정리할 기존 파일
- 삭제 또는 대체: `test/glutil_callstack_replica.hpp`, `test/cpptrace_realistic_benchmark.cpp`, `test/cpptrace_skip_benchmark.cpp`,
  `benchmark_utils.ii`(4MB), 루트 `CMakeLists.txt`의 중복 블록.
- `BENCHMARK.md`는 다시 측정한 수치로만 새로 작성함. 환경 표(CPU, OS, 컴파일러, cpptrace, gdm 커밋, `kInternalScanDepth`)를 포함함.
- 프로브 코드는 `tools/stack_probe`로 옮겨 `--verify`의 기반으로 씀.

### 6-7. 결정 사항
1. B5 → C1의 old skip은 **3**. 코드가 의도한 프레임(사용자 GL 호출 지점)에 도달하는 것을 확인함.
2. 중첩 경로 C6과 C7을 **포함**함.
3. `kInternalScanDepth` → §7 논의를 보고 결정함.
4. 플랫폼: 이번 검증은 macOS arm64에서만 함. x86-64 Linux/Windows는 glad 래퍼의 tail call 여부가 달라질 수 있음.

---

## 7. `kInternalScanDepth` 개념 정리

### 7.1 왜 `current(0, depth)`가 아니라 `current(0, K + skip + depth)`인가
- cpptrace의 `max_depth`는 "**맨 위부터** 몇 개의 물리 프레임을 캡처할지"를 정함. 내부 프레임을 빼고 나서 depth개를 남기는 기능은 없음.
- `current(0, depth)`로 잡으면 맨 위 N개가 내부 프레임이므로, 필터링 후 사용자 프레임은 `depth − N`개만 남음.
  - getCalledGLfunctionName(depth=1): `current(0,1)`은 `getCalledGLfunctionName` 자기 자신만 잡음 → 필터링하면 0개 → 항상 `"unknown source location"`.
  - printStackTrace(depth=15): 15 − 3 = 12프레임만 출력됨.
- 따라서 `N + skip + depth`개를 잡아야 함. 문제는 **N을 캡처 전에 알 수 없다**는 점임. N은 심볼 이름을 봐야 알 수 있고, 이름은 resolve가 끝나야 나옴.
  그래서 N의 **상한**을 상수 K로 잡고 `K + skip + depth`개를 캡처함. 이 상수가 `kInternalScanDepth`임.
- 다른 선택지는 "전체 스택을 캡처"(`current(0)`)하는 것임. 실제 앱에서는 사용자 호출 아래로 이벤트 루프, 프레임워크, main, dyld까지 수십 프레임이 있어서 전부 resolve하면 훨씬 비쌈.

### 7.2 K가 보장해야 하는 것
- `max_depth`는 **물리** 프레임 기준임. 따라서 조건은 `P ≤ K`임(P = 내부 **물리** 프레임 수).
  inline 내부 프레임(O2의 autoPostInspcector)은 캡처 예산을 쓰지 않음.
- `P > K`일 때 일어나는 일:
  - printStackTrace: 시작 프레임은 여전히 정확하지만 출력이 `P − K`개 짧아짐(점진적 열화, 경고 없음).
    `P ≥ K + depth`이면 사용자 프레임이 하나도 없는 빈 trace가 출력됨.
  - getCalledGLfunctionName: `P ≥ K + 1`, 즉 `P > K`이면 캡처한 프레임이 전부 내부 프레임 → `"unknown source location"`(**즉시 실패**).
- 현재 코드의 P 최대값(실측):

| API | 경로 | P (O0 / O2) | K=6일 때 여유 |
|---|---|---|---|
| getCalledGLfunctionName | C4, C5 | 4 / 4(C5는 3+inline) | 2 |
| printStackTrace | C7 | **6** / 5 | **0** |

- **K=6으로 충분한가**: 현재 코드에서는 **충분함**. 최적화는 물리 프레임을 줄이기만 하고(인라인, tail call) 늘리지 않음.
  hot/cold 분할(`.cold.N`)은 call이 아니라 branch라서 프레임을 만들지 않음. 그래서 P의 최대값은 호출 그래프의 구조로 결정되고(C7 = 6), 플랫폼이나 최적화 레벨이 바뀌어도 이보다 커지지 않음.
  여유가 0인 쪽은 printStackTrace의 C7뿐이고, 이 경로는 넘쳐도 출력이 짧아지는 것에 그침.
- 위험 요소: 앞으로 코드가 바뀌어 중첩이 한 단계 깊어지면(예: 콜백 헬퍼 한 단계 추가) 아무 신호 없이 출력이 짧아지거나 unknown이 나옴.
  이 위험은 K의 여유분보다 **테스트로 막는 편이 나음**: 각 경로에서 `P ≤ K`를 확인하는 assert를 두면 됨(벤치마크 `--verify`가 이 역할을 함).
- 워킹트리 주석("최대 4, errorSnapshot, 여유 2")은 틀렸으므로 고쳐야 함(B4).

### 7.3 비용: K 1개당 얼마인가 (실측, O2 -g, warm, 40단계 깊이의 사용자 체인)

| 캡처 프레임 n | unwind만 (`generate_raw_trace`) | `current(0,n)` (unwind + resolve) |
|---|---|---|
| 1 | 108 ns | 33.4 µs |
| 7 | 179 ns | 56.2 µs |
| 9 | 205 ns | 63.1 µs |
| 15 | 279 ns | 85.6 µs |
| 21 | 355 ns | 107.8 µs |
| 23 | 428 ns | 115.2 µs |

- unwind는 프레임당 약 12ns로 무시할 만함. **resolve는 호출당 약 33µs의 고정비에 프레임당 약 3.6µs**가 붙음.
  (작은 함수가 하나의 CU에 모여 있는 체인으로 잰 값이므로, 실제 glutil, glad, 사용자 CU에서는 프레임당 비용이 다를 수 있음. 본 벤치마크에서 실측할 예정.)
- 따라서 **K를 1 늘리면 매 호출마다 약 3.6µs가 추가됨**(스택이 충분히 깊을 때).
- 위 표로 추정한 old 대비 new의 추가 비용:

| | old | new K=6 | new K=8 |
|---|---|---|---|
| getCalledGLfunctionName (n) | 1 → 33µs | 7 → 56µs (+23µs, +68%) | 9 → 63µs (+30µs, +89%) |
| printStackTrace, resolve 부분만 (n) | 15 → 86µs | 21 → 108µs (+22µs, +26%) | 23 → 115µs (+29µs, +35%) |

### 7.4 K = 6 vs 8

| | K=6 | K=8 |
|---|---|---|
| 현재 코드 정확성 | 모든 경로 정확 (C7 print만 여유 0) | 모든 경로 정확 |
| 미래에 중첩 +1, +2 | getCalled는 +2까지 견딤. print C7은 1~2프레임 짧아짐 | print C7도 +2까지 견딤 |
| 매 호출 비용 | 기준 | +2 resolve ≈ +7µs (getCalled 기준 약 +13%) |
| 영향 범위 | getCalled는 autoLabel로 **GL 객체를 생성하고 바인드할 때마다** 호출됨 → 빈도가 높은 경로에 비용이 붙음 | |

- 권장: **K=6을 유지**하고, 여유분 대신 `P ≤ K`를 확인하는 테스트로 보장함. 정정한 주석에 근거(최대 P=6은 C7, 최적화는 P를 늘리지 않음)를 적음.
- 참고(범위 밖, 측정해 볼 만한 선택지): K가 하나의 상수일 필요는 없음. getCalled 경로는 P≤4이므로 API별로 K를 다르게 주면(getCalled 4, print 6) getCalled에서 resolve가 2개 줄어듦(≈−7µs).
  반대로 "작게 잡고, 사용자 프레임이 모자라면 다시 캡처"하는 방식은 재캡처할 때마다 고정비 33µs가 다시 들기 때문에 오히려 불리함.

---

## 8. 실측 결과 (실제 glutil + glad 하네스)

- 하네스: `test/stacktrace/` (빌드 타깃 `stacktrace_bench_{O0,O1,O2,O3,Os}`, 모두 `-g`, `GDM_DEBUG=1`), 실행 드라이버 `tools/run_stacktrace_bench.py`
- 원본 결과: `results/stacktrace-darwin-arm64-20261123-113852-K6both/ (K 분리 전, getCalled·print 모두 K=6)` (`summary.md`, `warm.tsv`, `cold.tsv`, `verify.txt`)
- 조건: warm은 7회 실행 × (12라운드 × 10회, old/new 교차)이고, 실행별 median을 다시 median으로 집계함. cold는 매번 새 프로세스로 15회 실행함. 사용자 호출 지점 아래에 32프레임을 둠
- 정확성: 5개 최적화 레벨 × 15개 검사, 합계 75건이 모두 OK임(old와 new의 결과 동일, 측정한 old skip과 테이블 값 일치)
- 표의 프레임 표기: old는 `skip s → 캡처 d → resolve d`(s개는 resolve하지 않음), new는 `캡처 K+d → resolve R = 내부 + 사용 + 잉여`

### 8.1 getCalledGLfunctionName (라벨링, 빈번함), warm T_total µs

| 빌드 | case | old | new K=6 | new K=4 |
|---|---|---|---|---|
| O0 | C2 | skip3→1→1: **35.6** | 7→7 (3+1+3): **242.8** | 5→5 (3+1+1): **176.9** |
| O0 | C4 | skip4→1→1: **33.5** | 7→7 (4+1+2): **243.4** | 5→5 (4+1+0): **176.3** |
| O0 | C5 | skip4→1→1: **32.6** | 7→7 (4+1+2): **238.4** | 5→5 (4+1+0): **173.7** |
| O2 | C2 | skip3→1→1: **13.2** | 7→7 (3+1+3): **172.0** | 5→5 (3+1+1): **150.0** |
| O2 | C4 | skip4→1→1: **11.9** | 7→7 (4+1+2): **298.3** | 5→5 (4+1+0): **273.1** |
| O2 | C5 | skip3→1→1: **12.0** | 7→8 (4[물리 3+inline 1]+1+3): **201.1** | 5→6 (4+1+1): **179.2** |

O1, O3, Os는 O2와 ±15% 이내임(Os가 약간 낮음). cold(프로세스 첫 호출): old 1.5–1.8ms(O0은 4.3ms), new 22–23ms. K에 따른 차이는 없음.

### 8.2 printStackTrace (에러 출력, 드묾), warm T_total µs

| 빌드 | case | old | new K=6 | new K=8 |
|---|---|---|---|---|
| O0 | C1 | skip3→15→15: 535 | 21→21 (3+15+3): 738 | 23→23 (3+15+5): 801 |
| O0 | C3 | 534 | 739 | 806 |
| O0 | C6 | 544 | 750 | 814 |
| O0 | C7a/C7b | skip6: 533 | 21→21 (6+15+0): 745 | 23→23 (6+15+2): 815 |
| O2 | C1 | skip3: 218 | 21→21 (3+15+3): 328 | 348 |
| O2 | C3 | skip3: 219 | 371 | 396 |
| O2 | C6 | skip3: 418 | 526 | 548 |
| O2 | C7a | skip5: 215 | 21→22 (6+15+1): 415 | 437 |
| O2 | C7b | skip6: 216 | 21→21 (6+15+0): 547 | 572 |

### 8.3 프레임 하나를 resolve하는 비용 (warm, 단일 프레임, 호출당 고정비 포함)

| 프레임 | O0 | O2 |
|---|---|---|
| `getCalledGLfunctionName` / `printStackTrace` (bench_stacktrace.cpp) | 28–35µs | 20–24µs |
| `callbacks::autoLabelGLObjects` (debug_callback.cpp) | 35µs | **84–122µs** |
| `callbacks::fullPostCallback` (debug_callback.cpp) | 34µs | **115–119µs** |
| `callbacks::checkGLErrorPostCallback` | 34µs | 20µs |
| `glad_debug_impl_*` (gl.c) | 32µs | 10–12µs |
| 사용자 프레임 | 32µs | 12µs |

- O1 이상에서는 **인라인된 callee가 많은 큰 함수**(`autoLabelGLObjects`, `fullPostCallback`)의 프레임이 resolve 비용을 지배함. DWARF에서 inline 체인을 찾는 비용으로 보임.
  이 프레임들은 old가 skip으로 건너뛰는 프레임이고, new는 이름을 보려면 반드시 resolve해야 하는 프레임임. "스킵되는 함수의 특성"이 실제로 영향을 주는 지점이 여기임.
  switch, va_list 같은 본문 특성이 아니라 **인라인된 내용이 많은 함수인지**가 중요함.
- O0에서는 인라인이 없어서 모든 프레임 비용이 비슷함(약 30µs). 그래서 new의 추가 비용이 "추가 프레임 수 × 약 30µs"에 가까움.
- glad 래퍼와 사용자 프레임은 쌈. 따라서 K를 줄여서 없애는 잉여(사용자) 프레임은 절약 효과가 작음.

### 8.4 K 분리(getCalled K=4, print K=6)에 대한 결론

- 안전성: getCalled는 autoLabel, autoPost, fullPost:661에서만 호출되고, 중첩 경로로는 도달하지 않음. 그래서 P 최대값은 구조적으로 4이고 모든 빌드에서 실측도 4임.
  K=4이면 여유가 0이고, 넘치면 `"unknown source location"`이 됨(661에서는 에러 해시 키도 함께 뭉개짐). `--verify`(P ≤ K 검사)로 막을 수 있음.
- 효과: 호출당 −22~25µs(O1 이상, new 대비 −8~13%), O0에서는 −66µs(−27%). 그래도 old 대비 ×11~24임.
  주 비용인 내부 프레임 resolve는 K로 줄일 수 없기 때문임.
- print의 K=8: +20~25µs(O1 이상), +65µs(O0)가 늘고 현재 이득은 없음. 에러 출력은 드물어서 어느 쪽이든 체감 영향은 작음.
- 규모 감: 라벨링 대상이 되는 GL 객체 1,000개를 생성하면 O2에서 old 약 12ms, new 약 300ms(K=4이면 약 275ms). 여기에 프로세스당 첫 호출 +20ms가 더해짐.

### 8.5 cold 표를 읽을 때 주의할 점
C3, C6, C7a의 "cold"는 같은 트리거 안에서 먼저 다른 API(getCalled)가 호출된 뒤의 값이라 진짜 첫 호출이 아님.
특히 C6은 old가 오히려 더 느리게 나옴. old가 앞의 getCalled에서 사용자 프레임 1개만 resolve했기 때문에, debug_callback.cpp의 DWARF 로딩 비용이 C6의 print로 넘어온 것임.
진짜 첫 호출로 볼 수 있는 값은 C1, C2, C4, C5, C7b임.

## 9. 추가로 발견해서 수정한 버그

### B6. `debug_snapshot.cpp` Snapshot 비동기 큐의 lost-wakeup 경쟁 (멈춤)
- 증상: 에러 경로(fullPostCallback → errorSnapshot → `Snapshot::capture().wait()`)를 반복 호출하다 보면 프로세스가 영구히 멈춤. 벤치마크 O0 warm 실행에서 재현했고, 스택 샘플로 `SnapshotAsyncHandle::wait()`의 condition_variable 대기임을 확인함.
- 원인: `finished`, `done`, 큐 노드를 바꾸고 `notify`하는 쪽이 대기 측 mutex를 잡지 않음. 대기 측이 predicate를 확인하고 잠들기 직전에 변경과 notify가 끼어들면 wakeup이 사라짐.
- 수정(`../gdm`, 커밋 안 함): 세 곳(~SnapshotQueue의 done, push, worker의 finished)에서 해당 mutex를 잡은 상태로 값을 publish한 다음 notify함. 수정 후 O0 warm 6회 연속 실행과 전체 벤치마크에서 재발하지 않음.

### 8.6 K 분리 적용 후 재측정 (최종)
- gdm에 적용: `kInternalScanDepthGetCalled = 4`, `kInternalScanDepthPrint = 6`(근거 주석 포함, B4 주석 정정).
  하네스는 이 값을 configure 시점에 gdm 소스에서 읽어 오므로, "new"는 항상 gdm이 실제로 쓰는 값으로 측정됨.
- 결과: `results/stacktrace-darwin-arm64-20261123-115957/`, 요약은 `BENCHMARK.md`. 75/75 검증 OK.
- getCalled 호출당 비용(K=6 → K=4): O2 C4 294.8 → 272.6µs(−22µs), O0 265.6 → 192.6µs(−73µs).

---

## 10. 오버헤드가 실제로 문제가 되는가

전제: GDM_DEBUG 빌드(Debug, RelWithDebInfo)에서만 해당됨. Release는 API가 빈 stub이라 영향이 없음.
아래 수치는 macOS arm64에서 측정한 값임(O2 = RelWithDebInfo, O0 = Debug).

### 10.1 언제 호출되는가

| 이벤트 | 호출 | 빈도 |
|---|---|---|
| 객체 생성: `glGen*`, `glCreateShader/Program`, `glCompileShader` | getCalled 1회 (C4) | 객체당 1회 |
| 객체의 **첫** bind: `glBindBuffer/VertexArray/Texture` | getCalled 1회 (C4) | 객체당 1회. 이후 bind는 `autoLabeled` 플래그 때문에 호출하지 않음 |
| VAO나 program 없이 draw | getCalled 2회 (C5) | **매 draw마다** |
| GL 에러 발생 | getCalled 1회 (C2, 해시) + 보고될 때만 printStackTrace (C3) | 에러가 날 때마다 getCalled를 호출함(**억제된 중복 에러도 포함**). print는 처음 한 번과 3초 집계마다 |
| glutil 내부 GL 호출의 에러 | printStackTrace (C6, C7) | 드묾 |
| 프로세스의 첫 API 호출 | cold +약 20ms | 1회 |

### 10.2 시나리오별 추가 비용 (new − old)

| 시나리오 | O2 (RelWithDebInfo) | O0 (Debug) | 판단 |
|---|---|---|---|
| 프로세스 시작 시 첫 호출 | +약 21ms (1회) | +약 19ms | 무시 가능 |
| 객체 하나 생성 + 첫 bind (getCalled 2회) | +약 0.5ms | +약 0.3ms | |
| 예제 수준 앱(객체 약 50개) 로딩 | +약 26ms | +약 16ms | 무시 가능 |
| 중간 규모 씬(객체 1,000개) 로딩 | **+약 0.5초** | +약 0.3초 | 체감 가능(시작 지연) |
| 큰 씬(객체 10,000개) 로딩 | **+약 5초** | +약 3초 | 디버그 빌드 개발 흐름에 지장을 줌 |
| 정상 코드의 렌더 루프(객체 재사용) | **0** | 0 | 문제없음 |
| 매 프레임 객체 생성/삭제(스트리밍 버퍼 등) N개 | +N × 0.5ms/frame | +N × 0.3ms | N≈10이면 16.7ms 예산의 30%. **문제 가능** |
| 매 프레임 같은 GL 에러가 M번 반복 | +M × 0.135ms/frame | +M × 0.15ms | M≈100이면 +13.5ms/frame. **문제 가능**(old는 +1.3ms) |
| VAO/program 없는 draw가 프레임당 D번 | +D × 0.33ms/frame | +D × 0.31ms | D≈50이면 +16ms/frame. **문제 가능** |
| 추적되지 않는 객체(콜백 설치 전에 만들었거나 다른 GL 로더로 만든 객체)를 매 프레임 bind | +bind당 0.26ms | +0.16ms | `info == nullptr`이라 매번 다시 라벨링함. **문제 가능** |

### 10.3 결론
- **정상적인 사용**(디버그 빌드, 올바른 코드, 일반적인 에셋 로딩)에서는 문제가 아님. 렌더 루프의 정상 상태에는 비용이 0이고, 시작 시 1회와 객체 생성 시에만 비용이 듦.
- 다만 old 대비 호출당 비용이 O2에서 11~22배, O0에서 5배라서, 다음 경우에는 **체감 가능한 성능 저하**가 됨:
  1. 객체가 많은 씬의 로딩(1,000개당 약 0.5초).
  2. 핫 루프에서 호출이 반복되는 경우: 매 프레임 객체 생성, 반복되는 에러나 경고, 추적되지 않는 객체.
  2번은 대부분 이미 버그가 있는(에러나 경고가 나는) 상태라서 프로그램이 느려지는 것 자체는 감수할 수 있음. 그래도 디버깅 중 프레임레이트가 크게 떨어지면 다른 문제를 가릴 수 있음.
- 비용의 정체: old는 resolve하지 않는 내부 프레임(특히 O1 이상에서 `autoLabelGLObjects`, `fullPostCallback`, 프레임당 84~122µs)을 new는 이름을 보려고 resolve해야 함. K 조정으로는 줄일 수 없음.

### 10.4 완화 방안 (→ 하네스 실측 §10.5, gdm 적용 §13.0)
- **raw 주소 기반 결과 캐시** (getCalledGLfunctionName): `cpptrace::generate_raw_trace(0, K + skip + 1)`은 resolve 없이 0.2µs 수준임(§7.3 실측).
  이 raw 주소 열을 키로 결과 문자열을 캐시하고, 미스일 때만 지금처럼 resolve하면 됨.
  - 같은 호출 지점의 반복(에러 루프, VAO 없는 draw, 추적되지 않는 객체 bind, 루프 안에서 하는 객체 생성)은 수 µs로 떨어져서 old보다도 빨라짐.
  - 처음 보는 호출 지점만 지금 비용을 냄.
  - 키에 맨 위 K+skip+1개 주소를 모두 넣으면 같은 경로, 같은 결과가 보장됨(보수적). 메모리는 서로 다른 호출 경로 수에 비례함.
- printStackTrace는 드물어서 그대로 둬도 됨.

---

### 10.5 raw 주소 캐시 실측 (하네스에만 구현, gdm 미변경)
- 구현(`bench_stacktrace.cpp`, `cache_hit`/`cache_miss` 변형):
  1. `generate_raw_trace(0, K+skip+1)`로 resolve 없이 주소만 캡처함.
  2. 그 주소 열을 키로 `unordered_map`(mutex 보호)을 조회함.
  3. 미스일 때만 `raw.resolve()`, `keepCallerFrames`, 스니펫을 거쳐 결과를 저장함.
  - `--verify`가 raw 캡처 경로와 기존 경로의 결과가 같은지도 검사함(75/75 OK).
- 키의 의미: getCalled에서 K=4이면 C4는 캡처 5개가 "내부 4 + 사용자 호출 지점 1"이 됨. 그래서 키는 사실상 **(GL 함수 경로, 호출 지점 주소)**이고, 루프 안에서 같은 줄이 반복되면 호출한 쪽이 달라도 적중함. C2는 잉여 1개가 들어가므로 호출자까지 같아야 적중함.
- 결과: `results/stacktrace-darwin-arm64-20261123-124336/`. 이 실행에서 new는 직전 실행보다 약 15% 높게 나왔음(세션 간 편차). 같은 실행 안에서의 비교는 유효함.

| 호출 1회 (getCalled T_total) | old | new | 캐시 적중 | 캐시 미스 |
|---|---|---|---|---|
| O2 C4 라벨링 | 12.1µs | 320.2µs | **0.2µs** | 313.4µs |
| O2 C2 에러 해시 | 13.5µs | 191.8µs | **0.5µs** | 185.9µs |
| O2 C5 draw 경고(호출 1회) | 12.2µs | 223.7µs | **0.2µs** | 217.6µs |
| O0 C4 | 34.2µs | 181.6µs | **0.5µs** | 184.9µs |
| O0 C2 | 37.0µs | 183.9µs | **0.9µs** | 186.2µs |
| 프로세스 첫 호출 (cold, O2 / O0) | 1.5 / 4.6ms | 24.3 / 25.7ms | (첫 호출은 미스) | 24.3 / 25.7ms |

- 적중하면 old보다도 25~60배 빠름. old는 호출할 때마다 사용자 프레임 1개를 resolve하기 때문임. 미스 비용은 new와 같음(차이는 측정 편차 범위).
- printStackTrace는 캐시 대상이 아님(드물게 호출되고 출력이 매번 필요함).

시나리오별 비교(T_total 기준. 로딩 시나리오는 "객체당 getCalled 2회" 가정이고, S는 실행되는 서로 다른 GL 호출 경로 수(가정)):

| 시나리오 | O2 old | O2 new | O2 new+캐시 | O0 old | O0 new | O0 new+캐시 |
|---|---|---|---|---|---|---|
| 프로세스 첫 호출 | 1.5ms | 24ms | 24ms | 4.6ms | 26ms | 26ms |
| 예제 앱: 객체 50개(100회), 경로가 모두 다름 | 1.2ms | 32ms | 31ms | 3.4ms | 18ms | 18ms |
| 객체 1,000개: 경로가 모두 다름(최악) | 24ms | 0.64s | 0.63s | 68ms | 0.36s | 0.37s |
| 객체 1,000개: 로더 공용 코드, S=100 | 24ms | 0.64s | **32ms** | 68ms | 0.36s | **19ms** |
| 객체 10,000개: S=100 | 0.24s | 6.4s | **35ms** | 0.68s | 3.6s | **28ms** |
| 정상 렌더 루프 | 0 | 0 | 0 | 0 | 0 | 0 |
| 매 프레임 객체 10개 생성(20회/frame) | 0.24ms (1.4%) | 6.4ms (38%) | **0.004ms** | 0.68ms (4%) | 3.6ms (22%) | **0.01ms** |
| 같은 GL 에러 100회/frame | 1.35ms (8%) | 19.2ms (115%) | **0.05ms** | 3.7ms (22%) | 18.4ms (110%) | **0.09ms** |
| VAO 없는 draw 50회/frame(100회) | 1.2ms (7%) | 22.4ms (134%) | **0.02ms** | 3.4ms (21%) | 18.1ms (108%) | **0.05ms** |
| 추적 안 되는 객체 bind 100회/frame | 1.2ms (7%) | 32ms (192%) | **0.02ms** | 3.4ms (21%) | 18.2ms (109%) | **0.05ms** |

(괄호 안은 60fps 프레임 예산 16.7ms 대비 비율. 로깅 등 old와 new에 공통인 비용은 제외함.)

- 결론: 캐시를 넣으면 **반복 경로(핫 루프의 모든 위험 시나리오, 루프 기반 로딩)에서는 new가 old보다 빨라짐.**
  남는 비용은 (1) 프로세스당 첫 호출 +약 20ms와 (2) 처음 보는 호출 경로마다 약 0.2~0.3ms뿐임.
  최악의 경우(모든 경로가 서로 다른 로딩)에는 캐시를 넣어도 이득이 없음.
- 비용과 위험:
  - 메모리는 경로당 키 약 40B와 문자열 약 100B 정도라서 경로 1만 개여도 약 1.5MB임.
  - 스레드 안전을 위해 mutex를 씀(적중 0.2µs에 포함된 비용).
  - 같은 raw 주소이면 같은 코드 위치이므로 결과가 같음. 예외는 실행 중에 코드 모듈을 내리고 다시 올리는 경우(hot reload, dlclose)로, 이때 stale 항목이 생길 수 있음(gdm은 해당 없음).
- 키는 "파일, 줄, 코드"가 아님. 그것은 저장되는 **값**이고, 키는 resolve하기 전의 return address 열임. 언제 적중하는지는 §10.6 참고.

### 10.6 캐시 키는 무엇이고 언제 적중하는가
- **키**: `skip`과 **resolve하기 전의 return address 5개**(K=4 + skip 0 + depth 1)임. "파일, 줄, 코드"는 미스일 때 resolve해서 만드는 **값**임.
  파일과 줄을 키로 쓰면 그것을 얻으려고 resolve를 해야 하는데, resolve가 바로 비싼 부분이라 의미가 없음. 그래서 주소로 찾음.
- 주소 5개 = getCalled 자신, autoLabel/autoPost/fullPost, glad 래퍼, **사용자의 GL 호출 명령**의 return address.
  return address는 "이 call 명령 바로 다음"을 가리키는 코드 안의 고정 위치임. 그래서 키는 **"어떤 내부 경로로, 어느 GL 호출 명령에서 왔는가"**와 같음.
  데이터(객체 id, 인자 값)는 키에 들어가지 않음.
- **적중하는 경우**: 같은 GL 호출 줄이 다시 실행되는 모든 경우임. 이것은 예외가 아니라 일반적인 경우임. 프로그램에 GL 호출 줄은 적고, 그 줄들은 함수 안에 있어서 여러 번 실행되기 때문임.
  - 객체 생성 헬퍼: `Buffer::Buffer() { glGenBuffers(1, &id); }`처럼 생성자나 헬퍼 안의 같은 줄이 객체마다 실행됨(버퍼 1,000개 = 같은 줄 1,000번).
  - 루프: `for (...) glGenTextures(1, &tex[i]);`
  - 첫 bind 라벨링: `Mesh::draw()` 안의 `glBindVertexArray(vao)` 줄이 메시마다 첫 draw에서 실행됨.
  - 재생성: 창 크기를 바꿀 때 FBO와 텍스처를 다시 만들거나, 레벨이나 에셋을 다시 로드하거나, 스트리밍 버퍼를 만드는 경우.
  - 매 프레임 반복: 같은 줄의 GL 에러(C2), VAO 없는 draw 경고(C5), 추적되지 않는 객체의 bind(C4).
- **미스가 나는 경우**: 각 GL 호출 줄이 처음 실행될 때 한 번씩임.
  - C2(에러 해시)와 O1 이상의 C5는 내부 물리 프레임이 3개라서 키의 5번째 주소가 "사용자 함수를 부른 쪽"임. 그래서 같은 줄이라도 호출자가 다르면 별도 항목이 됨.
  - C4(라벨링)는 내부 프레임이 4개라서 키에 호출 지점까지만 들어감. 그래서 호출자와 무관하게 적중함.

---

## 11. Windows에서 테스트하기

### 11.1 mac에서 크로스 빌드할 수 있는가
- **빌드 자체는 가능하지만 실익이 거의 없음.** `brew install mingw-w64`(현재 미설치)로 툴체인 파일을 만들고, cpptrace, libdwarf, zstd, glfw까지 크로스 빌드하면 컴파일 확인 정도는 됨.
- 실행은 mac에서 할 수 없음(Wine 미설치). Wine에서 돌리더라도 언와인드, 디버그 정보, 타이밍이 실제 Windows와 달라서 측정 의미가 없음.
- 따라서 **Windows PC에서 네이티브로 빌드하고 실행하는 것을 권장함.**

### 11.2 MinGW로 해도 되는가
- **됨. 오히려 맞는 선택임.** 그 PC에서 gdm을 쓰는 툴체인이 MinGW이므로 실제 사용 환경과 같음.
- cpptrace는 MinGW를 지원함. 디버그 정보는 PE에 들어간 DWARF를 libdwarf로 읽음(mac과 같은 계열의 resolver). MSVC(PDB, dbghelp)는 비용 구조가 다르므로 필요하면 별도로 측정해야 함.
- Win64 ABI는 인라인이나 tail call 결정이 다를 수 있음. `--verify`가 각 경로의 P와 old skip 테이블을 확인하고, 다르면 FAIL로 알려 줌(그러면 `kOldSkip`에 플랫폼별 값을 추가해야 함).
- 포팅 준비: `_WIN32` 분기를 gdm 원본 그대로 복원함. `%zu` 출력을 제거함. MinGW에서는 `-Wa,-mbig-obj`를 씀.
  의존성은 gdm의 `GDM_EXTERNAL_DIR`(cpptrace-1.0.4, glfw3-3.5.1, glad-gl4.6core_DEBUG*, stb 등)을 그대로 재사용함. 전역 `-Werror`를 제거해서 소스에서 빌드하는 의존성이 깨지지 않게 함.
  드라이버는 `.exe`를 인식함. **실제 Windows 빌드는 아직 검증하지 못했음.**
- 폴더를 그대로 복사해 쓸 수 있도록 추가로 수정한 것:
  - **old skip 테이블은 툴체인별로 적용함.** 테이블은 AppleClang-arm64에서 측정한 값임. GCC는 인라인 결정이 다를 수 있음(예: 한 번만 호출되는 `autoLabelGLObjects`까지 인라인할 수 있음).
    그래서 다른 툴체인에서는 verify의 1단계가 실제 스택에서 정답 skip을 측정해서 씀. 측정값은 `S` 줄로 출력됨(`S <opt> <toolchain> measured C1=3 ...`).
    드라이버는 이 값을 cold 실행에 `BENCH_OLD_SKIPS`로 넘김. mac에서 `BENCH_FORCE_MEASURED=1`로 이 경로를 검증함(15/15 OK, cold, 드라이버 모두 동작).
  - **`-Werror`는 옵트인**(`-DGDM_BENCH_WERROR=ON`)으로 바꿈. GCC의 새 경고 때문에 `encoding_benchmark` 빌드가 실패하는 것을 막기 위함.
  - **드라이버는 하위 프로세스 출력을 UTF-8(`errors=replace`)로 읽음.** 한국어 Windows의 cp949 기본값에서 경로에 비ASCII가 있으면 실패하던 것을 막기 위함.
  - 복사할 때 `build*/`, `build_vscode/` 같은 빌드 디렉터리는 제외해도 됨(새로 만듦). `external/`은 복사해도 무방함(gdm 소스와 플랫폼 무관한 생성 소스).

### 11.3 명령 (PowerShell, `gdm_benchmarks`가 있는 폴더에서 실행)
gdm을 미리 configure할 필요가 없음. 루트 CMake가 ogl-gdm과 같은 `FetchGDM`으로 gdm을 `gdm_benchmarks\external\gdm`에 받아 subproject로 추가하고,
gdm 자신의 configure가 glad 생성과 glfw, cpptrace, stb fetch를 `GDM_EXTERNAL_DIR`에 함. 기존 `external`을 지정하면 다운로드를 공유함.
cpptrace를 소스에서 빌드하므로 gdm 구성과 맞추기 위해 빌드 타입별로 디렉터리를 나눔.
- Debug에서 O0을 측정함.
- RelWithDebInfo에서 O1, O2, O3, Os를 측정함.

**주의**: 기본값으로 받는 gdm은 GitHub의 `master`임. 이번 세션의 gdm 변경 상태(2026-11-23 기준)는 다음과 같음. K 분리, glad prefix 수정, Snapshot 멈춤 수정은 로컬 커밋 `9009d3c`에 들어 있고, call-site 캐시는 커밋되지 않은 변경임. 로컬 master는 origin보다 2커밋 앞서 있으며 **아직 push하지 않았음**.
그래서 그대로 받으면 K=12 상태로 측정되고, Snapshot 경쟁 때문에 멈출 수 있음. 둘 중 하나로 해결함:
(a) gdm 변경을 push함(기본 fetch가 그대로 동작함). (b) 변경이 반영된 gdm 체크아웃을 `-DGDM_SOURCE_DIR="<경로>\gdm"`로 지정함.

```powershell
# 1) Debug (cpptrace도 Debug로 빌드됨) -> O0
cmake -S gdm_benchmarks -B "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TLS_VERIFY=OFF `
  -DCMAKE_C_COMPILER="c:\mingw64\bin\gcc.exe" -DCMAKE_CXX_COMPILER="c:\mingw64\bin\g++.exe" `
  -DGDM_EXTERNAL_DIR="$env:USERPROFILE\Downloads\external" `
  -DSTACKTRACE_BENCH_OPTS=O0
  # (b)의 경우 다음을 추가함: -DGDM_SOURCE_DIR="$PWD\gdm"
cmake --build "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" -j 8
& "$env:USERPROFILE\Downloads\build_gdm_bench_dbg\test\stacktrace\stacktrace_bench_O0.exe" --verify -v

# 2) RelWithDebInfo -> O1 O2 O3 Os
cmake -S gdm_benchmarks -B "$env:USERPROFILE\Downloads\build_gdm_bench_rel" -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TLS_VERIFY=OFF `
  -DCMAKE_C_COMPILER="c:\mingw64\bin\gcc.exe" -DCMAKE_CXX_COMPILER="c:\mingw64\bin\g++.exe" `
  -DGDM_EXTERNAL_DIR="$env:USERPROFILE\Downloads\external" `
  "-DSTACKTRACE_BENCH_OPTS=O1;O2;O3;Os"
cmake --build "$env:USERPROFILE\Downloads\build_gdm_bench_rel" -j 8

# 3) 반복 측정과 집계 (Python 3)
cd gdm_benchmarks
python tools\run_stacktrace_bench.py --build "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" --opts O0
python tools\run_stacktrace_bench.py --build "$env:USERPROFILE\Downloads\build_gdm_bench_rel" --opts O1,O2,O3,Os
```
- configure 로그의 `[stacktrace bench] K: getCalled=4 print=6`으로 어떤 gdm을 측정하는지 확인할 수 있음(12/12이면 변경 전 master임).
- `--verify -v` 출력과 `results\...` 폴더를 가져오면 mac 결과와 비교 분석할 수 있음.

---

## 13. 실제 예제 분석: ogl-gdm, glutil-examples (코드 분석 기반 예상치)

### 13.0 gdm에 캐시 적용
- `../gdm/external/glutil/src/debug_stacktrace.cpp`(커밋 안 함, `9009d3c` 위의 변경): §10.5의 raw 주소 캐시를 `getCalledGLfunctionName`에 적용함.
  - 키 = skip + raw return address(K+skip+1개). 기존 본문은 `describeCallSite()`로 옮기기만 함.
  - mutex로 보호하고, 캐시는 의도적으로 leak시킴(static 소멸 순서 문제 회피).
- 검증: 실제 수정된 `debug_stacktrace.cpp`를 실제 glutil, glad와 함께 프로브에 링크함(O0, O2).
  C1~C7의 출력과, 같은 경로를 3회 반복할 때(캐시 적중)의 출력이 처음 호출과 같음을 확인함. `-Wall -Wextra -Wpedantic`에서 경고 없이 컴파일됨.
- 하네스와의 대응: `new` = 캐시 없는 skip-by-name(캐시 적용 전 gdm), `cache_hit` / `cache_miss` = 현재 gdm. 캐시 키도 gdm과 같게 skip을 포함하도록 맞춤.

### 13.1 계산 규칙 (glutil 코드에서 확인)
getCalledGLfunctionName이 호출되는 횟수(라벨링 이벤트):

| GL 호출 | 이벤트 | 근거 |
|---|---|---|
| `glGen{Buffers,VertexArrays,Textures}` | 호출 1회당 1 | `autoLabelGLObjects:407`. bind 전이라 `glIs*`가 false이므로 라벨은 실패함 |
| 객체의 첫 `glBind{Buffer,VertexArray,Texture}`(id≠0) | 1 | tracker에 `autoLabeled`가 설정됨. 이후 bind는 0 |
| `glCreateShader` / `glCreateProgram` | 1 | `:327`, `:332` |
| `glCompileShader` / `glLinkProgram` | 0 | 생성할 때 붙인 라벨이 이미 있음 |
| VAO나 program 없이 draw | 1~2/draw | `autoPostInspcector` |
| GL 에러 | 1/발생 | `:661`. 중복으로 억제되는 에러도 포함함 |

- 캐시 미스 수 M = 서로 다른 (GL 호출 줄, 내부 경로)의 수. 라벨링(C4)은 K=4라서 키가 호출 지점에서 끝나므로, **같은 헬퍼 함수 안의 줄은 호출자와 무관하게 두 번째부터 적중함.**
  에러(C2)는 키에 호출자 한 칸이 더 들어감.
- 비용 식(O2 = RelWithDebInfo, O0 = Debug). 단가는 실측 실행 `124336`(§10.5, C4 라벨링)을 씀:
  - old = cold_old + (N−1)·12.1µs   [O0: 4.61ms + (N−1)·34.2µs], cold_old(O2) = 1.55ms
  - new = cold_new + (N−1)·320.2µs  [O0: 25.7ms + (N−1)·181.6µs], cold_new(O2) = 24.3ms
  - new+cache = cold_new + (M−1)·313.4µs + (N−M)·0.2µs   [O0: 184.9µs / 0.5µs]
  - cold = 프로세스의 첫 getCalled(DWARF 로딩 포함). 첫 호출은 항상 미스임.
- 한계: GL 드라이버 시간처럼 세 방식에 공통인 비용은 제외함. cold 값은 하네스 바이너리에서 측정한 것이라 실제 실행 파일 크기에 따라 달라질 수 있음. 런타임에 일어나는 GL 에러는 코드만으로 다 알 수 없으므로, 의도적으로 넣은 에러만 반영함.

### 13.2 ogl-gdm (GDM_GL_LOADER=glew-glad)
- glew-glad는 GLEW의 `gl*` 매크로만 제거한 헤더임. 그래서 GL 호출은 glad의 `glad_debug_glXxx`로 가고, 스택은 glad 모드와 같음(P와 K 조건이 같음).
- 모든 튜토리얼이 같은 구조임. 초기화 코드에서 객체마다 `glGen*` 직후 `glBind*`(첫 bind)를 하고, `LoadShaders`(common/shader.cpp: 이벤트 3)와 `loadBMP_custom`/`loadDDS`(이벤트 2)를 씀.
  **렌더 루프에는 기존 객체의 bind와 draw만 있고**(`gen-in-loop=0`), VAO와 program이 항상 바인드되어 있음. → **매 프레임 비용은 세 방식 모두 0**임.
- 시작 시 비용 (ms, 세 방식 모두 프로세스 첫 호출 포함):

| 실행 파일 | 이벤트 N | 미스 M | O2 old | O2 new | O2 new+캐시 | O0 old | O0 new | O0 new+캐시 |
|---|---|---|---|---|---|---|---|---|
| tutorial01_first_window, playground | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| tutorial02, tutorial03 | 7 | 7 | 1.6 | 26.2 | 26.2 | 4.8 | 26.8 | 26.8 |
| tutorial04 | 9 | 9 | 1.6 | 26.9 | 26.8 | 4.9 | 27.2 | 27.2 |
| tutorial05, 06, 07, 15 | 11 | 11 | 1.7 | 27.5 | 27.4 | 5.0 | 27.5 | 27.5 |
| tutorial08, tutorial18_particles | 13 | 13 | 1.7 | 28.1 | 28.1 | 5.0 | 27.9 | 27.9 |
| tutorial09(3종), 10, 12, 17, misc05_custom, misc05_Bullet | 15 | 15 | 1.7 | 28.8 | 28.7 | 5.1 | 28.2 | 28.3 |
| tutorial18_billboards | 16 | 13 | 1.7 | 29.1 | 28.1 | 5.1 | 28.4 | 27.9 |
| misc05_picking_slow_easy | 18 | 15 | 1.8 | 29.7 | 28.7 | 5.2 | 28.8 | 28.3 |
| tutorial16_shadowmaps_simple | 20 | 17 | 1.8 | 30.4 | 29.3 | 5.3 | 29.2 | 28.7 |
| tutorial14_render_to_texture | 22 | 19 | 1.8 | 31.0 | 29.9 | 5.3 | 29.5 | 29.0 |
| tutorial13_normal_mapping | 23 | 21 | 1.8 | 31.3 | 30.6 | 5.4 | 29.7 | 29.4 |
| tutorial11_2d_fonts | 24 | 19 | 1.8 | 31.7 | 29.9 | 5.4 | 29.9 | 29.0 |
| tutorial16_shadowmaps | 25 | 19 | 1.8 | 32.0 | 29.9 | 5.4 | 30.1 | 29.0 |

- 해석:
  - 객체 수가 적고(N ≤ 25) 대부분 서로 다른 줄에서 만들어서 미스 비율이 높음. 그래서 캐시의 이득이 작음(최대 −2ms).
  - new의 비용은 거의 전부 **프로세스 첫 호출 약 24ms**와 이벤트당 약 0.3ms임. 시작 시 1회 약 +25~30ms이고, 창이 뜨는 시간(수백 ms) 대비 체감되지 않음.
  - `tutorial14`의 `glGenFramebuffers`/`glGenRenderbuffers`는 autoLabel 대상이 아니라서 0으로 셈.

### 13.3 glutil-examples

| 실행 파일 | 이벤트 N | 미스 M | O2 old | O2 new | O2 new+캐시 | O0 old | O0 new | O0 new+캐시 |
|---|---|---|---|---|---|---|---|---|
| 01_ManyCubes | 23 | 18 | 1.8 | 31.3 | 29.6 | 5.4 | 29.7 | 28.8 |
| 02_Excavator_bad (기본 `STEP 1`) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 02_Excavator_bad (`STEP 6`) | 35 | 13 | 2.0 | 35.2 | 28.1 | 5.8 | 31.9 | 27.9 |
| 03_Excavator_good | 33 | 11 | 1.9 | 34.5 | 27.4 | 5.7 | 31.5 | 27.6 |
| **04_AIMLAB** | **309** | **10** | 5.3 | **122.9** | **27.2** | 15.1 | **81.6** | **27.5** |

- **01_ManyCubes**:
  - 초기화 이벤트: VAO 2개와 VBO 4개의 gen+첫 bind 12회, 텍스처 4개(`uploadDDS2D` 2회, `uploadStandard2D` 2회) 8회, 셰이더(같은 `compileShader` 헬퍼 2회 + program) 3회.
  - 루프: 큐브 150개를 draw하지만 VAO bind와 draw뿐이라 0임.
  - 키 입력 시 의도적 에러 `glBindTexture(9999, 9999)` 1회 → getCalled 1회(old 13.5µs / new 192µs / 캐시 미스 186µs) + printStackTrace 1회(old 220µs / new 327µs). 체감 차이 없음.
- **02_Excavator_bad**:
  - 기본 `STEP 1`(`USEGLUTIL && STEP != 6`)에서는 `gladLoadGL` 직후 `disableDebugCallbacks()`를 호출함 → 콜백이 꺼져서 **세 방식 모두 0**임.
  - `STEP 6`(디버그를 켠 교육용 단계)의 초기화:
    - modelData 3개의 생성자에서 gen 3 + 첫 bind 2, 그리고 vertexbuffer의 첫 bind가 draw에서 일어남.
    - VAO 2, `LoadShaders` 3, `loadBMP_custom` ×6 = 12.
    - STEP ≥ 4의 bind 누락 때문에 초기화 GL 에러 약 1회가 추가됨.
  - **매 프레임**: `draw()`의 `glBindBuffer(..., normalbuffer + 10)`.
    버퍼 이름이 1~9라면(이보다 먼저 만든 버퍼가 없다고 가정) +10은 모두 존재하지 않는 이름임 → draw 10회마다 `GL_INVALID_OPERATION` → **프레임당 getCalled 10회**(C2 경로).

    | 프레임당 | old | new | new+캐시(첫 프레임 이후) |
    |---|---|---|---|
    | O2 | 0.135ms (0.8%) | **1.92ms (11.5%)** | **0.005ms** |
    | O0 | 0.37ms (2.2%) | **1.84ms (11.0%)** | **0.009ms** |

    (괄호 안은 60fps 예산 대비 비율.) 같은 해시(같은 getCalled 결과)라서 printStackTrace와 errorSnapshot은 3초 집계마다 1회뿐이고, 무시할 수 있음.
- **03_Excavator_good**: modelData 3개 × (VAO, VBO, EBO의 gen+첫 bind) = 18회(같은 생성자 줄이라 미스 6), `loadBMP` ×6 = 12회(미스 2), 셰이더 3회. 루프 비용은 0이고, 키 입력 시 에러 1회는 ManyCubes와 같음.
- **04_AIMLAB** (**캐시 효과가 가장 큰 예제**):
  - 메시 6종(`plane`, `cube`, `target`, `particle`(sphere), `crosshair`, `gun`(ak47.obj: 그룹 36개))의 submesh **41개** × glutil `uploadMeshToGL`(model.cpp의 VAO, VBO, EBO gen+첫 bind = 6) = **246회**.
    model.cpp의 같은 6줄이라서 **미스 6, 적중 240**.
  - 텍스처 30장(`AddTexture` 29 + 기본 텍스처 1) × `loadImageToGL`(texture.cpp: gen+첫 bind) = 60회(미스 2).
  - `loadProgramToGL` 1회 = 3회(미스 2).
  - 런타임에 생기는 타깃, 데칼, 파티클은 `GetMesh`/`GetTexture`로 캐시된 GL 객체를 재사용하므로 **GL 객체를 새로 만들지 않음** → 매 프레임 0.
  - 메시 부분만 보면: O2 old 3.0ms → new 78.8ms → 캐시 1.9ms. O0 8.4 → 44.7 → 1.2ms.
  - 시작 시 전체: O2 **122.9ms → 27.2ms**, O0 **81.6ms → 27.5ms**. 남는 것은 거의 첫 호출 cold 약 24ms뿐임.

### 13.4 결론
- 분석한 실행 파일 30개 모두 **정상 동작 중 매 프레임 getCalled 호출이 없음**. 따라서 new와 new+캐시 모두 렌더링 성능(FPS)에는 영향이 없음.
- 차이는 **시작 시 1회**에만 남:
  - 작은 예제(ogl-gdm 전부, ManyCubes): old 약 2~5ms → new 약 26~32ms. 캐시의 이득은 작음(대부분 미스).
  - 공용 로더로 리소스를 많이 올리는 앱(AIMLAB): new 123ms(O2)에서 캐시로 27ms까지 줄어듦. 리소스가 많을수록 차이가 커짐.
- 매 프레임 비용이 생기는 유일한 경우는 **에러가 반복되는 코드**임(Excavator_bad STEP 6). 이때 new는 프레임 예산의 약 11%를 쓰는데, 캐시를 넣으면 0.005ms로 old(0.135ms)보다도 작아짐.
- 남는 비용인 프로세스당 첫 호출 +약 20ms(DWARF 로딩)는 캐시로 줄일 수 없음.

---

## 14. 의미적 정확성 점검 (new + 캐시, Debug / RelWithDebInfo)

### 14.1 검증된 것
| 항목 | 근거 | 범위 |
|---|---|---|
| new가 의도한 프레임에 도달함(사이트 12곳, 경로 8개: C1~C7b) | 하네스 `--verify`: 기대 file:line 일치, old(정답 skip)와 출력 동일 | macOS arm64, AppleClang, O0/O1/O2/O3/Os |
| 캐시 미스 경로(raw 캡처 → resolve)가 기존 경로와 결과가 같음 | `--verify`에서 raw 경로와 `current()` 경로를 비교함 | 위와 같음 |
| 캐시 적중 결과가 첫 호출 결과와 같음 | 실제 gdm 코드 프로브에서 C2/C3, C5 경로를 3회 반복 | O0, O2 |
| `P ≤ K` (getCalled 4, print 6) | 모든 경로를 실측함. O0(인라인 없음)의 값이 상한이고, 최적화는 물리 프레임을 줄이기만 함 | 위와 같음 + 구조적 논증 |
| 캐시 키의 결정성 | 결과는 키(skip + 캡처한 raw 주소)의 결정적 함수임. 해시 충돌은 전체 키 비교로 처리됨 | 구성상 성립 |

### 14.2 의도와 다르거나 확인되지 않은 것
1. **[RelWithDebInfo 한정, 실측] 사용자 GL 호출이 꼬리 위치에 있는 경우**
   - `void bindTex(GLuint t) { glBindTexture(GL_TEXTURE_2D, t); }`처럼 GL 호출이 함수의 마지막 문장이면, O2에서 sibling call(jmp)이 되어 `bindTex`의 프레임이 스택에서 사라짐.
   - 그래서 보고되는 위치가 **`bindTex`를 호출한 줄**이 됨. 프로브 실측: O0은 `:43`(정확), O2는 `:46`(호출자).
   - old도 같은 결과임(물리 프레임 기준이라 같은 곳에 도달함). 회귀는 아니고, 언와인드 방식 자체의 한계임.
   - 인라인된 헬퍼(`always_inline`)는 DWARF inline 프레임 덕분에 O2에서도 정확함.
   - 하네스는 모든 호출 지점에 `asm volatile("")`을 넣어 꼬리 호출을 막았기 때문에, 이 경우를 다루지 않았음.
2. **[설계 판단 필요] C6과 C7의 비대칭**: 두 경우 모두 glutil 자신의 GL 호출이 실패한 것인데 보고 위치가 다름.
   - C6(`labelGLobject` 안의 `glObjectLabel`이 실패): `glutil::debug::labelGLobject`(debug.cpp)부터 출력함. 의도대로임("glutil's own code … reported at its own location").
   - C7(`autoPostInspcector:526`의 `glGetIntegerv`가 실패): callbacks 네임스페이스 전체가 internal이므로 **사용자의 `glDrawArrays` 줄**부터 출력하고, 헤더는 "in function glGetIntegerv"임.
   - C7이 의도한 동작인지, 아니면 glutil 위치(`debug_callback.cpp:526`)를 보여야 하는지 결정이 필요함.
3. **[미검증] 다른 툴체인**: Windows/MinGW(GCC + libdwarf), MSVC(dbghelp).
   - 심볼 표기(GCC의 `[clone .constprop.0]` / `.part.0`, MSVC의 undecorated 이름)와 인라인 결정은 prefix 매칭과 `P ≤ K`로 견딜 것으로 예상하지만, 실측하지 않았음. `--verify`로 확인해야 함.
4. **[동작 차이] `depth`의 의미**: old는 물리 프레임 15개(+그 inline 확장)를, new는 resolve된 프레임(inline 포함) 15개를 출력함.
   - 사용자 코드에 인라인된 프레임이 있으면 old보다 적은 물리 프레임을 보여 줌.
   - 문서("maximum number of frames to print")에는 new가 더 맞음.
5. **[캐시] stale이 생길 수 있는 조건**
   - 실행 중에 코드 모듈을 내리고 같은 주소에 다른 모듈을 올리는 경우(dlclose / hot reload / JIT).
   - 첫 호출 때 소스 파일이 없거나 바뀌어서, 스니펫이 없거나 오래된 문자열이 캐시되는 경우.
   - 디버그 정보가 없는 바이너리는 `"unknown source location"`이 일관되게 캐시됨(캐시 전과 같은 결과).
   - gdm 사용 형태(정적 링크, 실행 중 언로드 없음)에서는 해당하지 않음.
6. **[미검증] LTO**: `-flto`로 `getCalledGLfunctionName`이 호출자에 인라인되어도 이름은 inline 프레임으로 남으므로 동작할 것으로 예상함. gdm 기본 빌드는 LTO를 쓰지 않음.
7. **[보장 장치 부재] gdm 안에 테스트가 없음**: `P ≤ K`와 "의도한 프레임"을 확인하는 것은 gdm_benchmarks의 `--verify`뿐임.
   - 앞으로 콜백 중첩이 깊어지면 getCalled는 `"unknown source location"`이 되고, print는 출력이 짧아짐. 둘 다 아무 신호 없이 일어남.
   - gdm의 glutil 테스트(CI)에 같은 검사를 두는 것을 권장함.

### 14.3 결론
- **Debug**: 코드베이스의 모든 경로에서 의도한 값을 반환함(실측). 캐시는 결과를 바꾸지 않음(구성상 + 실측).
- **RelWithDebInfo**: 코드베이스의 모든 경로(glutil 쪽 프레임 구성)에서는 정확함(실측).
  단, **사용자 코드의 GL 호출이 꼬리 위치이면 한 칸 위(호출자)가 보고됨.** old도 같은 한계가 있음.
- 확신의 범위: macOS arm64 clang에서 실측함. Windows는 `--verify` 결과로 확인해야 함. C7의 보고 위치는 설계 결정 사항임.

---

## 15. Windows (MinGW GCC) 1차 실행 결과와 수정 (B8)

- 입력: `memo.txt`(사용자가 Windows에서 실행한 `--verify -v` 출력, O0~Os). 툴체인 `GNU-AMD64`, `P<=K(12)`.
  → 측정 대상은 **GitHub master의 gdm**(K=12, 이번 세션 변경 전)이었음.
- 결과: **전 레벨 77건이 모두 FAIL이고, 원인은 하나임.** new가 첫 콜백 프레임(`checkGLErrorPostCallback`, `fullPostCallback`, `autoLabelGLObjects`, `autoPostInspcector`)에서 멈춤(`target WRONG`).
- 원인 (**B8, 실제 gdm 버그**):
  - GCC(MinGW)에서 cpptrace가 `debug_callback.cpp`의 **static 함수를 네임스페이스 없이 보고함**. 예: `fullPostCallback`(clang에서는 `glutil::debug::callbacks::fullPostCallback(...)`).
  - 외부 링키지 함수는 GCC에서도 이름이 정규화됨(`glutil::debug::labelGLobject`, `glutil::debug::getCalledGLfunctionName[abi:cxx11](int)`).
  - internal linkage 함수에 대해 정규화된(linkage) 이름이 디버그 정보에 기록되지 않는 것으로 보임.
  - 그래서 `glutil::debug::callbacks::` prefix가 매칭되지 않았음. 실제 Windows/MinGW 사용자 환경에서도 getCalled와 printStackTrace가 콜백 프레임을 사용자 위치로 보고하게 됨.
- 확인된 것: 물리 스택 구성은 GCC에서도 모든 레벨이 clang O0과 같음.
  내부 물리 프레임 수 = C1/C2/C3/C6: 3, C4/C5: 4, C7a/C7b: 6. GCC는 O1~Os에서도 `autoPostInspcector`를 인라인하지 않음.
- 수정(`../gdm` `debug_stacktrace.cpp`와 하네스 사본에 동일하게 적용, 커밋 안 함):
  - `isInternalFrame`에 **소스 파일 기준 판정**을 추가함. 프레임의 파일이 `glutil/src/debug_callback.cpp`이면(구분자 `/`, `\`를 가리지 않고 마지막 세 경로 요소를 비교) 내부 프레임으로 봄.
  - 이름 prefix 판정은 그대로 둠.
  - 사용자 프로젝트에 같은 이름의 `debug_callback.cpp`가 있어도 `glutil/src/` 아래가 아니면 매칭되지 않음.
  - 라인 정보가 없는 프레임은 이름으로만 판정함(이전과 같음).
- 검증(mac):
  - 하네스 `--verify`에 **분류기 self-test 12개**를 추가함: clang 형식, MinGW 형식(이름에 네임스페이스 없음, `\`/혼합 구분자), `[abi:cxx11]`, Mach-O `_glad`, 같은 이름의 사용자 파일, 라인 정보 없음. 5개 레벨 모두 OK이고 경로 검사 15/15도 OK.
  - 실제 gdm 코드 프로브(O0/O2)의 결과도 변경 전과 같음. gdm 파일은 `-Wall -Wextra -Wpedantic`에서 경고 없음.
  - **Windows 재실행은 아직 하지 않았음.**
- **2차 Windows 실행(B8 반영, K=4/6)**:
  - O1, O2, O3의 `--verify`는 통과함 → B8 수정이 MinGW에서 동작함을 확인.
  - GCC가 측정한 old skip: O1/O3 = C5 4, C7a 6. O2 = C5 3, C7a 5(clang O1+와 같음).
  - **Os는 1건 FAIL**(출력이 잘려서 출력 모양으로 C6로 추정함): `old==new NO`.
  - 추정 원인: GCC Os가 출력되는 부분(labelGLobject 아래 콜백 프레임)에 함수를 인라인함. old는 물리 프레임 15개 + 그 inline 프레임을, new는 resolve된 프레임 15개를 출력하므로 old가 한 줄 길어짐(§14.2-4의 depth 의미 차이). 대상 프레임 오류는 아님.
  - 하네스 수정:
    - print 경로는 프레임 단위로 비교함. new가 old의 앞부분과 같고 길이가 depth이면 `prefix(depth)`(OK)로 판정하고 차이를 표시함. 그 밖의 불일치는 첫 번째로 다른 프레임을 출력함.
    - 드라이버는 실패 시 전체 출력을 `results/.../failed-<exe>.txt`에 저장하고, FAIL 케이스와 그 상세만 출력함.
  - 추정이 맞는지는 다음 Windows 실행 결과로 확인해야 함.
- 재실행 조건: 측정 대상이 **이번 세션의 gdm**이어야 함. 이번 세션의 변경은 `9009d3c`(로컬 커밋, push 안 함) + call-site 캐시 + B8(커밋 안 함)임.
  configure 로그가 `K: getCalled=4 print=6`이어야 함. master(K=12)를 쓰면 B8 수정이 없고 Snapshot 멈춤 버그도 남아 있음.

---

## 16. Windows (MinGW GCC, x86-64) 측정 결과

- 원본: `results_window/` (rel 4회: `stacktrace-rel-windows-amd64-20261123-213151 … 214737`, deb 3회: `stacktrace-deb-… 213556 … 214618`). 각 회차는 warm 7회 × 12라운드 × 10회, cold 15회.
- gdm: `external/gdm`(9009d3c + 캐시 + B8), K = getCalled 4 / print 6.
- 아래 값은 회차별 median의 median(µs, cold는 ms)이며, 회차 간 편차는 대부분 2% 이내.

### 16.1 검증
- 7회 모두 FAIL 0(rel 64/64, deb 16/16), 분류기 self-test OK.
- Os C6은 `prefix(depth)`: old가 `skip 3 → 캡처 15 → resolve 16`(inline 1개). §15의 추정(depth 의미 차이)이 맞았음.
- GCC가 측정한 old skip:

| opt | C1 | C2 | C3 | C4 | C5 | C6 | C7a | C7b |
|---|---|---|---|---|---|---|---|---|
| O0, O1, O3 | 3 | 3 | 3 | 4 | 4 | 3 | 6 | 6 |
| O2 | 3 | 3 | 3 | 4 | **3** | 3 | **5** | 6 |
| Os | 3 | 3 | 3 | **3** | **3** | 3 | **5** | **5** |

  (clang은 O1+에서 C5 3, C7a 5로 고정이었음. GCC는 레벨마다 인라인 결정이 다름 → 하드코딩 old는 툴체인·레벨마다 값이 달라야 했다는 뜻이고, new는 모든 경우에 맞음.)

### 16.2 getCalledGLfunctionName (warm T_total, µs)

| 구성 | case | old | new | Δ new−old | cache_hit | cache_miss | mac 같은 레벨 (old / new / hit) |
|---|---|---|---|---|---|---|---|
| deb O0 | C2 | 189 | 396 | +207 (×2.09) | 133 | 389 | |
| deb O0 | C4 | 175 | 347 | +172 (×1.99) | 105 | 339 | 34 / 182 / 0.5 |
| deb O0 | C5 | 173 | 338 | +165 (×1.96) | 104 | 331 | |
| rel O2 | C2 | 142 | 236 | +94 (×1.66) | 130 | 236 | |
| rel O2 | C4 | 133 | 220 | +87 (×1.65) | 110 | 216 | 12 / 318 / 0.2 |
| rel O2 | C5 | 113 | 200 | +87 (×1.77) | 118 | 214 | |
| rel Os | C4 | 111 | 177 | +66 (×1.59) | 103 | 175 | 13 / 257 / 0.2 |

- O1/O3는 O2와 비슷함(C4: O1 130→191, O3 131→215).
- **mac과 구조가 다름**:
  - mac: 캡처는 거의 0(약 12ns/프레임)이고 비용은 resolve. old 12µs, 캐시 적중 0.2µs.
  - Windows: **캐시 적중(=raw 캡처 5프레임 + 조회)만으로 100~130µs**. 즉 스택 캡처(언와인드) 자체가 비쌈. old도 캡처가 대부분이라 old가 이미 110~190µs임.
  - 그래서 new의 추가분(+66~207µs)은 mac(C4: O0 +148, O2 +306µs)과 비슷하거나 작지만, **비율은 ×1.6~2.1**(mac ×5~26).
  - 캐시 적중은 old보다 약간 빠른 정도(O2 C4 133→110)이고, C5 O2/Os는 old보다 약간 느림(113→118, 115→121). old는 4프레임만 걷고(skip 3 + 1) 캐시는 5프레임(K 4 + 1)을 걷기 때문으로 보임 → 캡처 비용이 프레임 수에 비례한다는 추정과 맞음.
  - 캡처가 왜 비싼지(cpptrace의 MinGW 언와인더: dbghelp `StackWalk64` 추정)는 **미확인**. Windows 빌드 디렉터리의 `CMakeCache.txt`에서 `CPPTRACE_UNWIND_WITH_*` 값으로 확인 가능.
- new_kget6(K=6)은 new보다 +30~50µs → Windows에서는 K를 줄인 효과(K 분리)가 캡처·resolve 양쪽에서 나타남.

### 16.3 printStackTrace (warm T_total, µs)

| 구성 | C1 old → new | C3 old → new |
|---|---|---|
| deb O0 | 1177 → 1474 (+297, ×1.25) | 1200 → 1498 (+298) |
| rel O2 | 426 → 522 (+96, ×1.23) | 442 → 547 (+105) |
| rel Os | 409 → 503 (+94, ×1.23) | 425 → 517 (+92) |

- mac O2 C1: 220 → 327 (+107). 추가분은 비슷하고 old 자체가 Windows에서 약 2배.

### 16.4 cold (프로세스 첫 호출, ms)

| 구성 | old | new | Δ |
|---|---|---|---|
| deb O0 | 66 | 116 | +50 |
| rel O1~O3 | 63~64 | 89~93 | +26~29 |
| rel Os | 62 | 84 | +22 |

- mac: old 1.5 / 4.6ms, new 24 / 26ms. Windows는 **old의 첫 호출부터 약 63ms**(디버그 정보·심볼 초기화로 보임)이고, new는 그 위에 +22~50ms.

### 16.5 예제 영향 재계산 (Windows 수치, §13과 같은 방식)

계산: 시작 비용 = cold 1회 + 나머지 이벤트 × warm(라벨링은 C4, 반복 에러는 C2). 캐시는 미스 M−1회 + 적중 N−M회.

| 예제 | 구성 | old | new | new+캐시 | (mac O2 old / new / 캐시) |
|---|---|---|---|---|---|
| 작은 예제 (N 7~25, 대부분 미스) | rel O2 | 64~67ms | 92~96ms | 92~95ms | 1.6~2.0 / 26~35 / 26~30 |
| 04_AIMLAB 시작 (N 309, M 10) | rel O2 | **104ms** | **158ms** | **125ms** | 5.3 / 122.9 / 27.2 |
| 04_AIMLAB 시작 | deb O0 | 120ms | 223ms | 150ms | |
| Excavator_bad STEP 6, 프레임당(getCalled 10회) | rel O2 | 1.42ms (8.5%) | **2.36ms (14.2%)** | 1.30ms (7.8%) | 0.135 / 1.92 / 0.005 |
| 같은 경우 | deb O0 | 1.89ms (11.3%) | **3.96ms (23.7%)** | 1.33ms (8.0%) | |

(괄호는 60fps 예산 16.7ms 대비)

### 16.6 결론 (Windows)
- **정확성**: B8 수정 후 모든 레벨에서 new가 의도한 프레임을 반환함. old의 정답 skip은 GCC에서 레벨마다 다름(Os는 C4까지 바뀜).
- **오버헤드**: new − old = 호출당 +66~207µs(getCalled), +92~298µs(print), 첫 호출 +22~50ms. 절대량은 mac과 같은 수준이고, Windows는 old 자체가 비싸서 상대 증가는 ×1.2~2.1.
- **영향**: 시작 시 +30~100ms 수준. 반복 에러 코드(Excavator STEP 6)는 프레임 예산의 14%(O2) / 24%(O0)까지 감. 이 경우는 old도 8.5% / 11%를 씀.
- **캐시**: Windows에서는 캡처 비용이 남아서 효과가 "new → old 수준"까지임(mac처럼 0에 가깝지 않음). 그래도 AIMLAB 시작 −33ms(O2)/−73ms(O0), Excavator 프레임당 −1.06ms(O2)/−2.63ms(O0)로 new의 추가분을 대부분 없앰.
- Windows에서 더 줄이려면 캡처 자체를 줄여야 함(언와인더 선택, 또는 캡처 깊이 K 최소화). 이 부분은 미측정.

---

## 12. 부록: 측정 기록 전체 (시간순)

본문 표는 대표 레벨(O0, O2)만 발췌했음. 아래는 모든 측정의 전체 기록이고, 표는 각 결과 폴더의 `summary.md`에서 그대로 옮긴 것임.
값은 µs/호출이며, 괄호 안은 실행 간 min–max임.

### 12.1 예비 측정 (1회 실행, 참고용)
| 측정 | 내용 | 결과 |
|---|---|---|
| 합성 체인 unwind/resolve 비용 (§7.3) | 40단계 체인, O2 -g, warm | unwind 프레임당 약 12ns. `current(0,n)` 약 33µs + 3.6µs×n |
| 첫 warm 실행 (K=6, O2, 하네스 초판) | getCalled C2 | old 17.7µs, new 234.4µs |
| 〃 | print C1 / C3 | old 241.8 / 240.6µs, new 400.0 / 454.1µs, new K=8 425.7µs |
| 첫 `--frames` (O2, 단일 프레임 resolve) | C2: getCalled / fullPostCallback / glad / 사용자 | 60.0 / 191.5 / 27.0 / 16~28µs |
| 〃 | C4: getCalled / autoLabelGLObjects / fullPostCallback / glad / 사용자 | 51.2 / 246.0 / 353.4 / 16.0 / 17~28µs |
| 〃 | C5: getCalled / (autoPost inline + fullPost) / glad / 사용자 | 39.5 / 277.4 / 15.7 / 16~28µs |
| cpptrace cache mode (O2, C4 `--frames`) | default / prioritize_speed / hybrid | autoLabel 154 / 156 / 22,472µs, fullPost 215 / 184 / 21,886µs. 기본값이 prioritize_speed와 같으므로 조정할 것이 없음 |
| `--frames` 재측정 (§8.3) | O2 autoLabel / fullPost, O0 전 프레임 | 84~122µs / 115~119µs, O0 약 28~35µs (실행 간 편차가 큼) |
| 첫 cold 확인 (O2, 1회) | C4 old / new, C2C3 new의 getCalled / print | 7.26ms / 35.4ms, 31.5ms / 0.48ms |
| Snapshot 수정 확인 | O0 `--warm` 6회 연속 실행 | 2.6~3.8s, 모두 정상 종료 (수정 전에는 영구 멈춤) |
| 캐시 첫 확인 (O2, 1회) | C4 7곳 hit / miss / new | 0.2µs / 257~345µs / 260~352µs. C2 hit 0.5µs. C5 hit 0.2µs |


### 12.2 K 분리 적용 후 최종 실행 (115957): getCalled K=4, print K=6, new_kget6 비교 포함
원본: `results/stacktrace-darwin-arm64-20261123-115957/`

#### getCalledGLfunctionName: warm: T_total (API 1회), µs

| opt | case | old | new | new_kget6 | Δ new−old |
|---|---|---|---|---|---|
| O0 | C2 fullPostCallback:661 (에러 해시) | 37.4 (34.1–38.1) | 190.3 (172.2–192.9) | 261.3 (248.9–263.6) | +152.9 (×5.09) |
| O0 | C4 autoLabelGLObjects 7곳 (라벨링) | 36.5 (36.2–36.8) | 192.6 (191.0–195.0) | 265.6 (261.1–266.4) | +156.0 (×5.27) |
| O0 | C5 autoPostInspcector:522/529 | 36.5 (35.3–36.6) | 191.2 (184.5–194.0) | 264.7 (258.9–265.6) | +154.8 (×5.25) |
| O1 | C2 fullPostCallback:661 (에러 해시) | 13.4 (12.6–14.1) | 145.0 (144.2–146.6) | 167.4 (165.6–170.4) | +131.6 (×10.81) |
| O1 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.1 (11.7–12.4) | 275.2 (273.8–276.1) | 298.0 (296.4–299.2) | +263.1 (×22.81) |
| O1 | C5 autoPostInspcector:522/529 | 12.2 (11.8–12.3) | 176.5 (175.2–177.8) | 199.3 (197.6–200.1) | +164.4 (×14.51) |
| O2 | C2 fullPostCallback:661 (에러 해시) | 13.2 (12.8–14.2) | 148.0 (146.2–149.5) | 169.5 (168.0–170.1) | +134.7 (×11.17) |
| O2 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.2 (12.0–12.3) | 272.6 (271.5–273.7) | 294.8 (294.3–298.0) | +260.4 (×22.36) |
| O2 | C5 autoPostInspcector:522/529 | 11.9 (11.2–12.6) | 178.0 (175.0–179.5) | 200.9 (197.8–202.3) | +166.1 (×14.94) |
| O3 | C2 fullPostCallback:661 (에러 해시) | 13.4 (13.0–14.3) | 147.5 (147.0–148.0) | 171.1 (169.1–171.9) | +134.1 (×11.05) |
| O3 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.2 (10.9–12.5) | 284.6 (280.6–288.3) | 308.5 (306.7–309.7) | +272.4 (×23.23) |
| O3 | C5 autoPostInspcector:522/529 | 12.1 (12.0–12.6) | 186.7 (185.5–190.9) | 210.2 (209.5–212.9) | +174.6 (×15.40) |
| Os | C2 fullPostCallback:661 (에러 해시) | 14.7 (13.1–15.4) | 128.9 (128.2–130.7) | 153.7 (152.7–155.1) | +114.3 (×8.79) |
| Os | C4 autoLabelGLObjects 7곳 (라벨링) | 13.4 (13.3–13.7) | 225.4 (223.2–226.6) | 250.5 (250.2–252.4) | +212.0 (×16.80) |
| Os | C5 autoPostInspcector:522/529 | 13.4 (13.1–13.5) | 142.6 (142.0–143.3) | 168.5 (167.7–169.1) | +129.2 (×10.66) |

#### getCalledGLfunctionName: warm: e2e (사용자 GL 호출 1회), µs

| opt | case | old | new | new_kget6 | Δ new−old |
|---|---|---|---|---|---|
| O0 | C2  | 709.3 (624.8–716.1) | 1091.6 (973.3–1104.7) | 1158.2 (1093.2–1168.6) | +382.3 (×1.54) |
| O0 | C4  | 39.5 (39.2–39.6) | 196.4 (194.3–198.8) | 268.6 (264.5–270.4) | +156.9 (×4.98) |
| O0 | C5  | 81.8 (79.6–82.8) | 394.9 (383.1–398.2) | 539.3 (526.6–546.5) | +313.0 (×4.83) |
| O1 | C2  | 267.6 (266.8–269.2) | 551.3 (550.8–553.5) | 573.3 (570.2–576.6) | +283.7 (×2.06) |
| O1 | C4  | 12.8 (12.5–13.2) | 276.1 (274.9–276.9) | 298.9 (297.3–300.2) | +263.3 (×21.55) |
| O1 | C5  | 25.5 (25.4–25.7) | 356.7 (354.4–358.4) | 402.1 (400.3–403.6) | +331.2 (×13.99) |
| O2 | C2  | 267.7 (266.1–268.6) | 553.6 (550.3–556.6) | 575.8 (575.4–577.6) | +285.9 (×2.07) |
| O2 | C4  | 12.9 (12.8–13.1) | 273.4 (272.4–274.5) | 296.3 (295.2–298.9) | +260.5 (×21.20) |
| O2 | C5  | 25.5 (24.9–26.1) | 360.0 (355.6–361.8) | 405.6 (399.2–407.0) | +334.5 (×14.09) |
| O3 | C2  | 266.1 (264.6–267.8) | 553.9 (552.2–556.2) | 578.5 (575.4–581.3) | +287.8 (×2.08) |
| O3 | C4  | 12.9 (11.5–13.1) | 285.3 (281.4–289.2) | 309.4 (307.7–310.6) | +272.4 (×22.05) |
| O3 | C5  | 25.6 (25.2–26.2) | 376.9 (375.6–382.5) | 423.0 (421.8–428.5) | +351.3 (×14.70) |
| Os | C2  | 285.9 (281.8–287.1) | 539.5 (538.3–540.9) | 562.8 (560.2–565.4) | +253.6 (×1.89) |
| Os | C4  | 14.2 (14.1–14.5) | 226.3 (224.2–227.4) | 251.4 (251.2–253.3) | +212.1 (×15.95) |
| Os | C5  | 28.2 (28.0–28.6) | 288.5 (286.1–289.6) | 339.6 (338.4–341.8) | +260.2 (×10.23) |

#### getCalledGLfunctionName: cold: 프로세스 첫 호출 T_total, ms (median, min–max). *는 같은 트리거에서 앞선 API 호출 뒤의 값

| opt | case | old | new | new_kget6 |
|---|---|---|---|---|
| O0 | C2 | 4.26 (4.07–4.67) | 23.11 (22.80–27.18) | 23.13 (22.74–23.94) |
| O0 | C4 | 4.27 (4.06–4.69) | 23.38 (20.33–25.19) | 23.41 (22.79–24.19) |
| O0 | C5 | 4.30 (4.21–4.63) | 23.04 (22.70–24.43) | 23.16 (22.56–30.62) |
| O1 | C2 | 1.51 (1.45–1.61) | 22.15 (21.56–22.39) | 22.23 (21.62–23.14) |
| O1 | C4 | 1.54 (1.43–1.80) | 22.28 (21.80–23.37) | 22.28 (21.25–22.57) |
| O1 | C5 | 1.55 (1.35–1.63) | 22.33 (21.67–22.71) | 22.26 (22.02–22.64) |
| O2 | C2 | 1.49 (1.42–1.59) | 22.16 (21.90–22.41) | 22.37 (21.88–22.57) |
| O2 | C4 | 1.51 (1.21–1.57) | 22.41 (19.03–76.29) | 22.49 (21.71–31.01) |
| O2 | C5 | 1.49 (1.25–1.61) | 22.17 (18.89–23.12) | 22.33 (21.09–22.67) |
| O3 | C2 | 1.51 (1.33–1.61) | 22.43 (22.14–22.74) | 22.59 (21.82–23.28) |
| O3 | C4 | 1.51 (1.39–1.62) | 22.59 (22.05–22.90) | 22.62 (22.35–22.80) |
| O3 | C5 | 1.55 (1.42–1.75) | 22.49 (22.22–23.04) | 22.64 (22.15–22.91) |
| Os | C2 | 1.69 (1.55–1.81) | 22.75 (22.12–23.08) | 23.06 (22.45–23.16) |
| Os | C4 | 1.75 (1.65–1.85) | 22.24 (21.87–23.20) | 22.18 (21.29–22.40) |
| Os | C5 | 1.69 (1.63–1.78) | 22.12 (21.22–23.24) | 22.12 (20.83–22.33) |

#### printStackTrace: warm: T_total (API 1회), µs

| opt | case | old | new | Δ new−old |
|---|---|---|---|---|
| O0 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 584.0 (557.6–591.4) | 809.2 (762.6–816.9) | +225.2 (×1.39) |
| O0 | C3 fullPostCallback:671 (에러 출력) | 586.1 (504.0–592.1) | 814.7 (718.4–827.4) | +228.6 (×1.39) |
| O0 | C6 중첩: labelGLobject의 GL 에러 | 594.0 (562.1–605.5) | 824.3 (810.2–833.1) | +230.3 (×1.39) |
| O0 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 583.1 (579.6–592.1) | 814.0 (803.4–823.9) | +230.9 (×1.40) |
| O0 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 579.0 (569.2–586.5) | 819.0 (814.4–832.7) | +240.1 (×1.41) |
| O1 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 216.0 (214.6–218.4) | 322.3 (318.7–323.3) | +106.3 (×1.49) |
| O1 | C3 fullPostCallback:671 (에러 출력) | 217.5 (216.4–219.1) | 369.6 (367.2–370.8) | +152.1 (×1.70) |
| O1 | C6 중첩: labelGLobject의 GL 에러 | 418.0 (416.5–421.2) | 526.0 (524.1–529.1) | +108.0 (×1.26) |
| O1 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 215.6 (214.8–216.9) | 413.2 (410.5–416.4) | +197.6 (×1.92) |
| O1 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 216.3 (213.4–217.1) | 548.5 (546.7–551.9) | +332.1 (×2.54) |
| O2 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 217.2 (215.1–218.3) | 321.4 (320.2–324.3) | +104.2 (×1.48) |
| O2 | C3 fullPostCallback:671 (에러 출력) | 217.8 (216.5–218.3) | 368.1 (366.8–370.3) | +150.4 (×1.69) |
| O2 | C6 중첩: labelGLobject의 GL 에러 | 412.9 (392.3–414.7) | 520.0 (486.7–523.4) | +107.1 (×1.26) |
| O2 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 214.4 (207.6–216.9) | 411.5 (407.3–417.9) | +197.1 (×1.92) |
| O2 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 216.0 (213.9–218.6) | 542.3 (540.1–544.8) | +326.3 (×2.51) |
| O3 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 214.6 (213.2–218.8) | 321.2 (319.9–326.2) | +106.6 (×1.50) |
| O3 | C3 fullPostCallback:671 (에러 출력) | 217.2 (214.2–219.0) | 370.7 (369.1–371.3) | +153.4 (×1.71) |
| O3 | C6 중첩: labelGLobject의 GL 에러 | 426.8 (424.0–430.4) | 536.6 (533.3–541.2) | +109.9 (×1.26) |
| O3 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 216.4 (214.1–218.1) | 422.0 (419.9–424.7) | +205.6 (×1.95) |
| O3 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 214.9 (212.4–217.6) | 557.9 (554.5–566.0) | +343.0 (×2.60) |
| Os | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 233.5 (226.1–235.4) | 339.9 (324.7–340.9) | +106.4 (×1.46) |
| Os | C3 fullPostCallback:671 (에러 출력) | 234.9 (231.0–235.7) | 373.5 (372.1–374.1) | +138.5 (×1.59) |
| Os | C6 중첩: labelGLobject의 GL 에러 | 384.3 (382.4–388.3) | 492.7 (488.0–498.7) | +108.4 (×1.28) |
| Os | C7a 중첩: autoPostInspcector:526의 GL 에러 | 232.9 (231.6–233.9) | 399.4 (396.9–400.0) | +166.5 (×1.72) |
| Os | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 233.3 (231.6–235.0) | 516.9 (516.2–520.1) | +283.6 (×2.22) |

#### printStackTrace: warm: e2e (사용자 GL 호출 1회), µs

| opt | case | old | new | Δ new−old |
|---|---|---|---|---|
| O0 | C1  | 587.1 (562.8–594.8) | 812.3 (769.0–819.6) | +225.1 (×1.38) |
| O0 | C3  | 709.3 (624.8–716.1) | 1091.6 (973.3–1104.7) | +382.3 (×1.54) |
| O0 | C6  | 639.8 (605.9–651.1) | 1025.1 (1005.7–1029.0) | +385.4 (×1.60) |
| O0 | C7a  | 671.4 (663.9–679.9) | 1217.6 (1202.9–1228.8) | +546.2 (×1.81) |
| O0 | C7b  | 628.1 (613.6–630.5) | 1021.1 (1018.6–1032.1) | +393.0 (×1.63) |
| O1 | C1  | 217.7 (216.6–220.1) | 324.2 (320.5–325.0) | +106.5 (×1.49) |
| O1 | C3  | 267.6 (266.8–269.2) | 551.3 (550.8–553.5) | +283.7 (×2.06) |
| O1 | C6  | 433.8 (432.8–437.2) | 805.4 (803.4–810.4) | +371.6 (×1.86) |
| O1 | C7a  | 243.9 (243.7–245.2) | 775.9 (770.1–779.9) | +532.0 (×3.18) |
| O1 | C7b  | 232.0 (228.5–232.8) | 863.2 (861.5–866.6) | +631.2 (×3.72) |
| O2 | C1  | 218.8 (216.6–220.2) | 323.0 (321.6–326.1) | +104.2 (×1.48) |
| O2 | C3  | 267.7 (266.1–268.6) | 553.6 (550.3–556.6) | +285.9 (×2.07) |
| O2 | C6  | 429.1 (407.1–430.2) | 799.8 (741.7–802.7) | +370.7 (×1.86) |
| O2 | C7a  | 243.1 (236.5–245.5) | 775.9 (768.3–786.6) | +532.8 (×3.19) |
| O2 | C7b  | 231.1 (230.4–233.5) | 855.3 (854.2–862.7) | +624.2 (×3.70) |
| O3 | C1  | 216.4 (214.8–220.9) | 323.0 (321.6–327.7) | +106.6 (×1.49) |
| O3 | C3  | 266.1 (264.6–267.8) | 553.9 (552.2–556.2) | +287.8 (×2.08) |
| O3 | C6  | 443.0 (439.3–446.7) | 827.7 (824.4–837.3) | +384.7 (×1.87) |
| O3 | C7a  | 245.5 (242.4–247.0) | 804.0 (800.5–811.5) | +558.5 (×3.27) |
| O3 | C7b  | 230.4 (227.6–233.0) | 882.2 (879.5–894.8) | +651.8 (×3.83) |
| Os | C1  | 235.0 (227.6–237.0) | 341.6 (326.1–342.8) | +106.6 (×1.45) |
| Os | C3  | 285.9 (281.8–287.1) | 539.5 (538.3–540.9) | +253.6 (×1.89) |
| Os | C6  | 400.8 (399.1–405.8) | 721.8 (716.2–730.2) | +321.0 (×1.80) |
| Os | C7a  | 263.3 (262.8–264.4) | 692.4 (687.9–693.8) | +429.1 (×2.63) |
| Os | C7b  | 250.3 (248.4–251.2) | 782.7 (781.6–786.0) | +532.4 (×3.13) |

#### printStackTrace: cold: 프로세스 첫 호출 T_total, ms (median, min–max). *는 같은 트리거에서 앞선 API 호출 뒤의 값

| opt | case | old | new |
|---|---|---|---|
| O0 | C1 | 5.32 (4.98–5.70) | 26.37 (24.01–29.77) |
| O0 | C3* | 0.57 (0.53–0.59) | 0.80 (0.74–0.88) |
| O0 | C6* | 21.23 (21.04–21.46) | 6.91 (6.70–10.59) |
| O0 | C7a* | 0.56 (0.53–0.73) | 0.83 (0.79–0.91) |
| O0 | C7b | 4.81 (4.54–5.04) | 23.69 (22.91–24.85) |
| O1 | C1 | 1.75 (1.54–2.11) | 22.39 (21.99–23.16) |
| O1 | C3* | 0.24 (0.19–0.28) | 0.44 (0.37–0.72) |
| O1 | C6* | 22.29 (21.97–22.81) | 6.20 (5.86–6.43) |
| O1 | C7a* | 0.24 (0.18–0.26) | 0.47 (0.37–0.78) |
| O1 | C7b | 1.76 (1.61–1.85) | 22.68 (22.20–25.95) |
| O2 | C1 | 1.74 (1.61–1.99) | 22.48 (22.33–22.73) |
| O2 | C3* | 0.24 (0.21–0.29) | 0.42 (0.39–0.51) |
| O2 | C6* | 22.77 (22.56–23.95) | 6.55 (6.18–7.80) |
| O2 | C7a* | 0.26 (0.23–0.50) | 0.48 (0.47–0.52) |
| O2 | C7b | 1.75 (1.70–1.85) | 22.64 (19.43–25.12) |
| O3 | C1 | 1.76 (1.47–1.87) | 22.73 (22.42–24.68) |
| O3 | C3* | 0.24 (0.20–0.29) | 0.43 (0.39–0.45) |
| O3 | C6* | 22.79 (21.84–24.35) | 6.52 (5.54–6.64) |
| O3 | C7a* | 0.24 (0.22–0.29) | 0.48 (0.44–0.52) |
| O3 | C7b | 1.76 (1.67–1.86) | 22.96 (22.21–24.20) |
| Os | C1 | 1.96 (1.90–2.06) | 22.22 (21.65–22.53) |
| Os | C3* | 0.26 (0.25–0.28) | 0.44 (0.42–0.47) |
| Os | C6* | 22.39 (19.53–22.63) | 6.42 (6.32–6.59) |
| Os | C7a* | 0.26 (0.25–0.28) | 0.45 (0.43–0.50) |
| Os | C7b | 1.96 (1.87–2.00) | 22.45 (22.24–22.60) |


### 12.3 캐시 변형 포함 실행 (124336): cache_hit / cache_miss
원본: `results/stacktrace-darwin-arm64-20261123-124336/`

#### getCalledGLfunctionName: warm: T_total (API 1회), µs

| opt | case | old | new | new_kget6 | cache_hit | cache_miss | Δ new−old |
|---|---|---|---|---|---|---|---|
| O0 | C2 fullPostCallback:661 (에러 해시) | 37.0 (35.7–46.0) | 183.9 (181.8–208.8) | 250.2 (248.5–284.7) | 0.9 (0.9–1.2) | 186.2 (185.0–215.5) | +146.9 (×4.97) |
| O0 | C4 autoLabelGLObjects 7곳 (라벨링) | 34.2 (33.5–34.8) | 181.6 (181.2–184.5) | 250.7 (248.8–252.0) | 0.5 (0.4–0.5) | 184.9 (183.9–186.0) | +147.3 (×5.30) |
| O0 | C5 autoPostInspcector:522/529 | 34.3 (31.3–35.4) | 181.0 (178.3–218.2) | 248.2 (247.2–325.7) | 0.5 (0.4–0.5) | 182.7 (181.5–235.7) | +146.7 (×5.28) |
| O1 | C2 fullPostCallback:661 (에러 해시) | 14.0 (12.9–14.3) | 186.3 (184.6–187.0) | 209.3 (207.6–210.4) | 0.5 (0.5–0.5) | 180.2 (178.9–182.1) | +172.2 (×13.29) |
| O1 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.4 (11.6–12.5) | 317.0 (311.4–320.6) | 341.0 (337.6–342.2) | 0.2 (0.2–0.2) | 310.1 (308.8–311.5) | +304.6 (×25.66) |
| O1 | C5 autoPostInspcector:522/529 | 11.8 (10.5–12.2) | 217.1 (210.9–217.9) | 240.6 (238.2–243.2) | 0.2 (0.2–0.2) | 211.7 (203.1–212.3) | +205.4 (×18.48) |
| O2 | C2 fullPostCallback:661 (에러 해시) | 13.5 (12.0–14.1) | 191.8 (188.2–194.6) | 215.1 (209.1–216.1) | 0.5 (0.5–0.5) | 185.9 (177.8–188.0) | +178.2 (×14.16) |
| O2 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.1 (10.5–12.5) | 320.2 (298.3–325.1) | 345.6 (321.1–353.4) | 0.2 (0.2–0.2) | 313.4 (292.6–314.4) | +308.1 (×26.36) |
| O2 | C5 autoPostInspcector:522/529 | 12.2 (10.6–12.5) | 223.7 (209.7–224.8) | 246.9 (231.9–247.9) | 0.2 (0.2–0.2) | 217.6 (204.8–218.2) | +211.5 (×18.29) |
| O3 | C2 fullPostCallback:661 (에러 해시) | 13.7 (12.9–14.1) | 193.8 (191.0–194.1) | 214.8 (211.4–219.1) | 0.5 (0.5–0.5) | 185.7 (185.0–186.9) | +180.2 (×14.18) |
| O3 | C4 autoLabelGLObjects 7곳 (라벨링) | 12.3 (11.9–12.4) | 331.4 (327.8–338.0) | 355.4 (352.9–361.2) | 0.2 (0.2–0.2) | 325.1 (322.4–329.5) | +319.1 (×26.96) |
| O3 | C5 autoPostInspcector:522/529 | 12.4 (11.9–12.6) | 232.8 (230.6–235.2) | 255.6 (253.4–260.9) | 0.2 (0.2–0.2) | 226.2 (224.4–230.4) | +220.4 (×18.72) |
| Os | C2 fullPostCallback:661 (에러 해시) | 14.6 (13.9–15.4) | 161.9 (160.2–162.5) | 186.6 (184.7–189.4) | 0.5 (0.5–0.5) | 157.9 (157.2–159.1) | +147.3 (×11.08) |
| Os | C4 autoLabelGLObjects 7곳 (라벨링) | 13.4 (13.2–13.6) | 257.4 (256.0–258.9) | 284.1 (281.0–285.1) | 0.2 (0.2–0.2) | 253.8 (252.0–256.4) | +244.0 (×19.18) |
| Os | C5 autoPostInspcector:522/529 | 13.2 (13.0–13.8) | 174.8 (173.8–176.2) | 199.8 (199.4–202.2) | 0.2 (0.2–0.2) | 172.6 (171.6–173.5) | +161.5 (×13.19) |

#### getCalledGLfunctionName: warm: e2e (사용자 GL 호출 1회), µs

| opt | case | old | new | new_kget6 | cache_hit | cache_miss | Δ new−old |
|---|---|---|---|---|---|---|---|
| O0 | C2  | 666.0 (659.4–954.7) | 1021.5 (1014.3–1241.1) | 1087.6 (1083.0–1254.0) | 843.1 (835.6–995.4) | 1029.7 (1021.7–1261.9) | +355.5 (×1.53) |
| O0 | C4  | 37.2 (36.2–37.7) | 184.5 (184.3–188.2) | 253.8 (252.0–255.0) | 2.7 (2.6–2.9) | 187.7 (187.1–189.8) | +147.3 (×4.96) |
| O0 | C5  | 77.6 (76.4–79.9) | 371.6 (367.1–477.1) | 507.5 (503.9–730.8) | 7.5 (6.8–7.8) | 376.6 (370.5–527.7) | +293.9 (×4.79) |
| O1 | C2  | 270.7 (268.1–273.6) | 598.5 (595.0–599.7) | 620.9 (617.6–628.2) | 411.9 (408.9–415.6) | 591.3 (589.2–597.0) | +327.8 (×2.21) |
| O1 | C4  | 13.1 (12.3–13.2) | 318.0 (312.6–321.6) | 341.9 (338.6–343.4) | 0.8 (0.8–0.8) | 311.1 (309.6–312.9) | +304.9 (×24.34) |
| O1 | C5  | 26.1 (23.2–26.4) | 437.1 (431.0–441.1) | 485.4 (480.0–490.1) | 1.6 (1.6–2.0) | 426.9 (411.3–428.0) | +411.0 (×16.76) |
| O2 | C2  | 268.6 (263.9–276.5) | 604.1 (581.3–607.0) | 626.1 (617.6–630.3) | 413.0 (401.5–415.2) | 598.3 (584.1–601.6) | +335.5 (×2.25) |
| O2 | C4  | 12.9 (11.2–13.2) | 321.2 (299.1–326.3) | 346.5 (322.2–354.4) | 0.8 (0.6–0.8) | 314.4 (293.7–315.5) | +308.3 (×24.91) |
| O2 | C5  | 25.9 (25.6–27.5) | 450.1 (422.6–452.7) | 496.9 (468.6–499.8) | 2.0 (1.6–2.0) | 438.4 (414.6–440.8) | +424.2 (×17.40) |
| O3 | C2  | 268.7 (266.4–271.9) | 608.8 (603.4–613.4) | 626.3 (623.5–638.8) | 412.2 (402.0–420.9) | 598.8 (596.3–606.0) | +340.0 (×2.27) |
| O3 | C4  | 13.0 (12.6–13.1) | 332.4 (328.7–339.0) | 356.7 (353.8–362.5) | 0.7 (0.7–0.8) | 326.1 (323.2–330.6) | +319.3 (×25.53) |
| O3 | C5  | 26.2 (25.8–27.1) | 470.2 (463.9–474.2) | 514.2 (510.2–526.9) | 1.9 (1.7–2.0) | 455.5 (451.6–464.7) | +444.0 (×17.93) |
| Os | C2  | 288.5 (286.8–290.6) | 579.6 (573.9–580.9) | 601.8 (597.7–606.9) | 417.8 (413.3–418.6) | 575.1 (572.8–580.0) | +291.2 (×2.01) |
| Os | C4  | 14.1 (13.9–14.4) | 258.6 (256.9–259.9) | 284.9 (282.0–286.7) | 0.8 (0.7–0.8) | 255.1 (252.8–257.6) | +244.4 (×18.31) |
| Os | C5  | 28.1 (28.0–28.9) | 352.8 (350.5–355.6) | 403.9 (401.6–406.3) | 2.1 (1.9–2.1) | 349.1 (344.8–350.0) | +324.7 (×12.57) |

#### getCalledGLfunctionName: cold: 프로세스 첫 호출 T_total, ms (median, min–max). *는 같은 트리거에서 앞선 API 호출 뒤의 값

| opt | case | old | new | new_kget6 | cache_hit | cache_miss |
|---|---|---|---|---|---|---|
| O0 | C2 | 5.62 (4.42–11.06) | 26.79 (24.65–36.30) | 25.43 (24.79–29.30) | - | - |
| O0 | C4 | 4.61 (4.29–5.34) | 25.68 (23.56–27.15) | 25.68 (24.72–36.43) | - | - |
| O0 | C5 | 5.02 (4.31–7.17) | 25.18 (24.66–26.78) | 25.56 (24.84–35.05) | - | - |
| O1 | C2 | 1.69 (1.43–2.42) | 24.67 (23.83–36.80) | 24.59 (24.13–26.37) | - | - |
| O1 | C4 | 1.67 (1.55–5.08) | 24.23 (23.68–25.66) | 24.09 (23.75–26.21) | - | - |
| O1 | C5 | 1.66 (1.55–2.12) | 24.09 (23.21–42.42) | 24.23 (23.51–25.16) | - | - |
| O2 | C2 | 1.53 (1.34–1.97) | 24.16 (21.28–25.28) | 24.10 (23.87–24.90) | - | - |
| O2 | C4 | 1.55 (1.38–2.14) | 24.29 (23.87–25.04) | 24.19 (23.71–25.08) | - | - |
| O2 | C5 | 1.53 (1.30–1.81) | 24.71 (23.52–25.82) | 24.10 (23.72–24.87) | - | - |
| O3 | C2 | 1.58 (1.52–2.38) | 24.36 (23.92–27.10) | 24.36 (23.75–36.44) | - | - |
| O3 | C4 | 1.58 (1.48–2.24) | 24.26 (23.06–25.31) | 24.67 (24.02–35.70) | - | - |
| O3 | C5 | 1.60 (1.35–2.29) | 24.90 (24.05–26.16) | 24.32 (23.96–25.15) | - | - |
| Os | C2 | 1.78 (1.57–2.38) | 24.10 (23.38–25.37) | 23.73 (23.11–26.84) | - | - |
| Os | C4 | 1.74 (1.50–2.31) | 23.86 (23.31–25.04) | 23.81 (21.73–25.78) | - | - |
| Os | C5 | 1.76 (1.54–1.85) | 23.81 (21.99–25.25) | 23.86 (23.35–24.39) | - | - |

#### printStackTrace: warm: T_total (API 1회), µs

| opt | case | old | new | Δ new−old |
|---|---|---|---|---|
| O0 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 544.5 (490.5–573.9) | 758.1 (679.8–804.2) | +213.7 (×1.39) |
| O0 | C3 fullPostCallback:671 (에러 출력) | 547.3 (543.6–717.9) | 757.7 (754.0–867.7) | +210.4 (×1.38) |
| O0 | C6 중첩: labelGLobject의 GL 에러 | 558.4 (555.9–821.3) | 770.8 (763.5–1000.8) | +212.4 (×1.38) |
| O0 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 548.2 (539.7–583.2) | 764.4 (760.1–794.8) | +216.1 (×1.39) |
| O0 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 543.6 (533.6–576.4) | 764.9 (758.6–810.3) | +221.3 (×1.41) |
| O1 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 218.7 (217.1–220.8) | 327.9 (324.0–333.6) | +109.1 (×1.50) |
| O1 | C3 fullPostCallback:671 (에러 출력) | 220.6 (218.7–221.0) | 373.9 (371.6–375.9) | +153.2 (×1.69) |
| O1 | C6 중첩: labelGLobject의 GL 에러 | 425.1 (412.5–437.4) | 532.8 (506.0–544.5) | +107.6 (×1.25) |
| O1 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 217.8 (214.4–219.8) | 419.5 (408.3–423.2) | +201.7 (×1.93) |
| O1 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 218.7 (217.4–221.5) | 561.2 (554.2–573.9) | +342.5 (×2.57) |
| O2 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 219.5 (217.1–220.8) | 326.6 (323.8–329.2) | +107.2 (×1.49) |
| O2 | C3 fullPostCallback:671 (에러 출력) | 219.6 (211.5–223.5) | 374.8 (360.4–377.1) | +155.1 (×1.71) |
| O2 | C6 중첩: labelGLobject의 GL 에러 | 420.5 (418.7–422.5) | 532.4 (527.5–535.8) | +111.9 (×1.27) |
| O2 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 219.0 (217.7–221.6) | 420.6 (417.9–422.8) | +201.6 (×1.92) |
| O2 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 219.2 (214.9–220.0) | 550.7 (547.4–555.9) | +331.5 (×2.51) |
| O3 | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 217.1 (215.9–219.0) | 326.9 (321.7–331.3) | +109.8 (×1.51) |
| O3 | C3 fullPostCallback:671 (에러 출력) | 219.3 (215.5–221.0) | 376.5 (374.6–380.5) | +157.2 (×1.72) |
| O3 | C6 중첩: labelGLobject의 GL 에러 | 431.7 (420.9–440.6) | 543.8 (539.5–546.8) | +112.1 (×1.26) |
| O3 | C7a 중첩: autoPostInspcector:526의 GL 에러 | 218.4 (217.6–328.3) | 431.2 (428.0–678.9) | +212.8 (×1.97) |
| O3 | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 218.5 (205.4–220.2) | 566.4 (536.2–568.3) | +347.9 (×2.59) |
| Os | C1 checkGLErrorPostCallback:32 ← 사용자 GL 에러 | 235.6 (233.5–237.4) | 344.1 (342.6–348.0) | +108.5 (×1.46) |
| Os | C3 fullPostCallback:671 (에러 출력) | 237.3 (235.8–239.2) | 378.1 (375.5–380.1) | +140.8 (×1.59) |
| Os | C6 중첩: labelGLobject의 GL 에러 | 389.9 (388.3–392.1) | 502.0 (497.9–503.6) | +112.1 (×1.29) |
| Os | C7a 중첩: autoPostInspcector:526의 GL 에러 | 235.3 (222.4–236.5) | 404.6 (401.8–408.1) | +169.3 (×1.72) |
| Os | C7b 중첩: autoLabelGLObjects:338의 GL 에러 | 235.6 (235.1–237.0) | 524.8 (523.1–529.6) | +289.2 (×2.23) |

#### printStackTrace: warm: e2e (사용자 GL 호출 1회), µs

| opt | case | old | new | Δ new−old |
|---|---|---|---|---|
| O0 | C1  | 547.3 (492.8–576.7) | 761.0 (682.2–807.4) | +213.7 (×1.39) |
| O0 | C3  | 666.0 (659.4–954.7) | 1021.5 (1014.3–1241.1) | +355.5 (×1.53) |
| O0 | C6  | 599.6 (597.3–893.4) | 957.2 (950.6–1294.3) | +357.6 (×1.60) |
| O0 | C7a  | 630.8 (623.4–665.0) | 1145.4 (1138.3–1219.1) | +514.6 (×1.82) |
| O0 | C7b  | 584.5 (574.6–619.8) | 958.9 (951.2–1013.6) | +374.3 (×1.64) |
| O1 | C1  | 220.9 (218.9–222.6) | 329.7 (325.7–335.4) | +108.8 (×1.49) |
| O1 | C3  | 270.7 (268.1–273.6) | 598.5 (595.0–599.7) | +327.8 (×2.21) |
| O1 | C6  | 441.2 (425.9–455.4) | 858.8 (818.5–869.8) | +417.6 (×1.95) |
| O1 | C7a  | 248.2 (244.9–249.7) | 862.9 (834.4–870.9) | +614.7 (×3.48) |
| O1 | C7b  | 234.3 (232.8–237.6) | 919.0 (914.6–937.0) | +684.6 (×3.92) |
| O2 | C1  | 220.9 (218.8–222.5) | 328.2 (325.6–331.1) | +107.3 (×1.49) |
| O2 | C3  | 268.6 (263.9–276.5) | 604.1 (581.3–607.0) | +335.5 (×2.25) |
| O2 | C6  | 436.7 (435.1–438.3) | 858.2 (850.0–862.7) | +421.5 (×1.97) |
| O2 | C7a  | 247.8 (247.0–250.8) | 874.4 (871.6–884.5) | +626.6 (×3.53) |
| O2 | C7b  | 234.3 (231.3–236.0) | 912.4 (905.4–920.7) | +678.1 (×3.89) |
| O3 | C1  | 219.3 (217.4–220.6) | 328.4 (323.4–333.5) | +109.1 (×1.50) |
| O3 | C3  | 268.7 (266.4–271.9) | 608.8 (603.4–613.4) | +340.0 (×2.27) |
| O3 | C6  | 448.0 (438.1–457.4) | 881.2 (879.2–886.0) | +433.1 (×1.97) |
| O3 | C7a  | 247.1 (246.1–387.4) | 901.2 (897.5–1409.1) | +654.1 (×3.65) |
| O3 | C7b  | 234.4 (219.2–235.5) | 938.2 (907.6–940.8) | +703.9 (×4.00) |
| Os | C1  | 237.1 (235.2–239.1) | 345.8 (344.6–349.7) | +108.8 (×1.46) |
| Os | C3  | 288.5 (286.8–290.6) | 579.6 (573.9–580.9) | +291.2 (×2.01) |
| Os | C6  | 408.6 (405.0–410.1) | 767.8 (762.4–769.4) | +359.2 (×1.88) |
| Os | C7a  | 267.7 (255.6–268.9) | 763.1 (754.3–769.1) | +495.4 (×2.85) |
| Os | C7b  | 252.6 (251.4–255.2) | 824.9 (820.8–829.5) | +572.3 (×3.27) |

#### printStackTrace: cold: 프로세스 첫 호출 T_total, ms (median, min–max). *는 같은 트리거에서 앞선 API 호출 뒤의 값

| opt | case | old | new |
|---|---|---|---|
| O0 | C1 | 4.93 (4.72–6.72) | 26.08 (25.01–28.02) |
| O0 | C3* | 0.62 (0.52–1.59) | 0.85 (0.72–1.14) |
| O0 | C6* | 22.26 (21.60–25.78) | 7.07 (6.39–7.69) |
| O0 | C7a* | 0.56 (0.47–0.63) | 0.84 (0.68–1.21) |
| O0 | C7b | 5.05 (4.32–9.70) | 26.26 (24.91–39.11) |
| O1 | C1 | 1.78 (1.50–2.44) | 24.09 (23.87–25.38) |
| O1 | C3* | 0.27 (0.24–0.46) | 0.47 (0.41–0.76) |
| O1 | C6* | 23.45 (20.94–25.75) | 6.44 (6.12–7.11) |
| O1 | C7a* | 0.25 (0.23–0.42) | 0.49 (0.41–0.64) |
| O1 | C7b | 1.82 (1.75–2.44) | 24.14 (23.67–24.70) |
| O2 | C1 | 1.76 (1.53–2.44) | 24.31 (22.58–25.27) |
| O2 | C3* | 0.25 (0.20–0.39) | 0.44 (0.34–0.63) |
| O2 | C6* | 23.31 (22.59–25.08) | 6.61 (6.09–7.36) |
| O2 | C7a* | 0.25 (0.23–0.44) | 0.47 (0.38–0.54) |
| O2 | C7b | 1.76 (1.62–1.83) | 24.70 (23.89–26.87) |
| O3 | C1 | 1.80 (1.48–2.33) | 24.75 (23.26–25.75) |
| O3 | C3* | 0.26 (0.23–0.44) | 0.44 (0.42–0.50) |
| O3 | C6* | 23.36 (22.87–24.75) | 6.75 (5.59–7.72) |
| O3 | C7a* | 0.25 (0.20–0.55) | 0.49 (0.46–0.54) |
| O3 | C7b | 1.77 (1.68–2.28) | 25.09 (24.40–26.09) |
| Os | C1 | 1.98 (1.77–2.36) | 24.38 (22.93–26.83) |
| Os | C3* | 0.27 (0.20–0.33) | 0.44 (0.40–0.64) |
| Os | C6* | 23.50 (20.75–25.92) | 6.59 (6.35–9.31) |
| Os | C7a* | 0.27 (0.21–0.54) | 0.48 (0.47–0.74) |
| Os | C7b | 2.02 (1.76–2.71) | 23.95 (22.13–25.37) |


(K 분리 전 실행 113852의 전체 표: `results/stacktrace-darwin-arm64-20261123-113852-K6both/summary.md`. 본문 §8.1, §8.2에 O0/O2를 발췌함.)
