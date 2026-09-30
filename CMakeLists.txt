# SimCity 2000 (Win95) static recompilation.
#
#   1. py -3 run_lift.py                       # -> src/recomp/gen/ (gitignored)
#   2. tools\build.cmd                         # x86 clang-cl in a vcvars32 shell
#   3. build\sc2k --headless --record out.mp4 --seconds 30
#
# The host must be a 32-bit program: guest pointers are used as host pointers
# and the real Win32 API is called directly (see src/runtime/host.c).
cmake_minimum_required(VERSION 3.20)
project(sc2k C CXX)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "sc2k needs a 32-bit compiler: run tools\build.cmd (x86 clang-cl)")
endif()

set(PCRECOMP ${CMAKE_SOURCE_DIR}/../tools CACHE PATH "pcrecomp checkout")

# The frontend (src/frontend) is C++ with Dear ImGui (third_party/imgui, MIT;
# see NOTICE) over Direct3D 11. The runtime and the lifted code stay C.
set(CMAKE_CXX_STANDARD 17)
set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py` first")
endif()

add_executable(sc2k
  src/runtime/host.c
  src/runtime/overrides.c
  src/runtime/capture.c
  src/runtime/palette.c
  src/runtime/fx.c
  src/runtime/frame.c
  src/runtime/qol.c
  src/runtime/mods.c
  src/frontend/frontend.cpp
  ${IMGUI_SOURCES}
  src/runtime/input.c
  src/recomp/gen/recomp_dispatch.c
  ${GEN_SOURCES}
)
target_include_directories(sc2k PRIVATE src/recomp/gen ${PCRECOMP}/runtime/recomp32 ${IMGUI} ${IMGUI}/backends)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")

# A benign dummy return address: a stack imbalance can't turn it into a huge size.
target_compile_definitions(sc2k PRIVATE RECOMP_RETADDR=0u)

# Generated code trips every benign warning there is; the lift's own error
# count is the signal. -O1 keeps a 500K-line build to a few minutes.
target_compile_definitions(sc2k PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/O1;-fno-strict-aliasing;-fwrapv")
# Line info always: a crash address symbolizes to a generated line, and each
# generated line carries the original instruction's address in a comment.
#   llvm-symbolizer --obj=build/sc2k.exe 0x<address from the crash report>
target_compile_options(sc2k PRIVATE /Z7)

# 0x00400000 upward belongs to the game image; link the host well clear of it.
# --nxcompat so DEP is on and a jump into guest .text traps (the callback path).
target_link_options(sc2k PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO /NXCOMPAT /DEBUG)
target_link_libraries(sc2k PRIVATE user32 gdi32 comdlg32 advapi32 d3d11 dxgi d3dcompiler dwmapi winmm)

# The sample mod (mods/stipend): a DLL in build/mods, loaded at startup.
add_library(stipend SHARED mods/stipend/stipend.c)
target_include_directories(stipend PRIVATE src/runtime)
set_target_properties(stipend PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
