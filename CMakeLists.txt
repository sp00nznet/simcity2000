# SimCity 2000 (Win95) static recompilation.
#
#   1. py -3 run_lift.py                       # -> src/recomp/gen/ (gitignored)
#   2. tools\build.cmd                         # x86 clang-cl in a vcvars32 shell
#   3. build\sc2k --headless --record out.mp4 --seconds 30
#
# The host must be a 32-bit program: guest pointers are used as host pointers
# and the real Win32 API is called directly (see src/runtime/host.c).
cmake_minimum_required(VERSION 3.20)
project(sc2k C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "sc2k needs a 32-bit compiler: run tools\build.cmd (x86 clang-cl)")
endif()

set(PCRECOMP ${CMAKE_SOURCE_DIR}/../tools CACHE PATH "pcrecomp checkout")

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py` first")
endif()

add_executable(sc2k
  src/runtime/host.c
  src/runtime/overrides.c
  src/runtime/capture.c
  src/recomp/gen/recomp_dispatch.c
  ${GEN_SOURCES}
)
target_include_directories(sc2k PRIVATE src/recomp/gen ${PCRECOMP}/runtime/recomp32)

# A benign dummy return address: a stack imbalance can't turn it into a huge size.
target_compile_definitions(sc2k PRIVATE RECOMP_RETADDR=0u)

# Generated code trips every benign warning there is; the lift's own error
# count is the signal. -O1 keeps a 500K-line build to a few minutes.
target_compile_definitions(sc2k PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/O1;-fno-strict-aliasing;-fwrapv")

# 0x00400000 upward belongs to the game image; link the host well clear of it.
# --nxcompat so DEP is on and a jump into guest .text traps (the callback path).
target_link_options(sc2k PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO /NXCOMPAT)
target_link_libraries(sc2k PRIVATE user32 gdi32)
