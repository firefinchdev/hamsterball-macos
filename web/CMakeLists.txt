# Browser build (Emscripten): the compatibility layer with the statically recompiled game code
# (build/recomp, from recomp/recomp.py) instead of Unicorn. Driven by web/build.sh.
cmake_minimum_required(VERSION 3.18)
project(hamsterball_web C CXX)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TP ${REPO}/android/third_party/src)
# How the game's endless main loop yields to the page: ASYNCIFY (any browser) or JSPI
# (recent Chrome, Edge, Firefox; smaller)
set(HB_YIELD "ASYNCIFY" CACHE STRING "ASYNCIFY or JSPI")

# exceptions (libopenmpt) and setjmp/longjmp (the game): native WebAssembly EH with JSPI;
# ASYNCIFY can't unwind through that, so it uses Emscripten's JavaScript-based EH
if(HB_YIELD STREQUAL "JSPI")
  set(EH -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
else()
  set(EH -fexceptions -sSUPPORT_LONGJMP=emscripten)
endif()
add_compile_options(${EH})
add_link_options(${EH})

# gl4es: desktop OpenGL fixed function on WebGL
set(STATICLIB ON CACHE BOOL "" FORCE)
set(NO_INIT_CONSTRUCTOR ON CACHE BOOL "" FORCE)
set(NOX11 ON CACHE BOOL "" FORCE)
set(NOEGL ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/gl4es gl4es)

# libopenmpt (MO3 music), same source list as the Android build
set(OMPT ${TP}/libopenmpt)
file(READ ${OMPT}/build/android_ndk/Android.mk OMPT_MK)
string(REGEX MATCHALL "(common|libopenmpt|soundlib|sounddsp)/[A-Za-z0-9_/]+\\.cpp" OMPT_FILES "${OMPT_MK}")
list(REMOVE_DUPLICATES OMPT_FILES)
set(OMPT_SRC ${OMPT}/include/stb_vorbis/stb_vorbis.c ${OMPT}/include/minimp3/minimp3.c)
foreach(f ${OMPT_FILES})
  list(APPEND OMPT_SRC ${OMPT}/${f})
endforeach()
add_library(openmpt STATIC ${OMPT_SRC})
target_include_directories(openmpt PUBLIC ${OMPT} PRIVATE ${OMPT}/src ${OMPT}/common ${OMPT}/include)
target_compile_definitions(openmpt PRIVATE LIBOPENMPT_BUILD MPT_WITH_ZLIB MPT_WITH_STBVORBIS MPT_WITH_MINIMP3)
target_compile_options(openmpt PRIVATE -O2 -frtti -w -sUSE_ZLIB=1)
set_target_properties(openmpt PROPERTIES CXX_STANDARD 20 C_STANDARD 17)

# the recompiled game code
file(GLOB GEN_SRC ${REPO}/build/recomp/gen_*.c)
if(NOT GEN_SRC)
  message(FATAL_ERROR "build/recomp is empty: run make recomp-gen first")
endif()
add_library(game STATIC ${GEN_SRC})
target_include_directories(game PRIVATE ${REPO}/src ${REPO}/build/recomp)
target_compile_options(game PRIVATE -O2 -g0 -w -fno-strict-aliasing)

file(GLOB HB_SRC CONFIGURE_DEPENDS ${REPO}/src/*.cpp)
add_executable(hamsterball ${HB_SRC})
set_target_properties(hamsterball PROPERTIES CXX_STANDARD 17 SUFFIX ".html")
target_include_directories(hamsterball PRIVATE ${TP}/gl4es/include)
target_compile_definitions(hamsterball PRIVATE HB_RECOMP GL_GLEXT_PROTOTYPES)
target_compile_options(hamsterball PRIVATE -O2 -sUSE_SDL=2 -Wno-unused-function -Wno-deprecated-declarations)
target_link_libraries(hamsterball game GL openmpt)
target_link_options(hamsterball PRIVATE
  -O2 -sUSE_SDL=2 -sUSE_ZLIB=1 -s${HB_YIELD}
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=400MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=16MB
  -sGL_ENABLE_GET_PROC_ADDRESS=1 -sFULL_ES2=1 -sENVIRONMENT=web -lidbfs.js
  -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=FS,ENV,callMain
  "--preload-file=${REPO}/game@/game"
  "--shell-file=${CMAKE_BINARY_DIR}/shell.html")
if(HB_YIELD STREQUAL "JSPI")
  set(JSPI_BUILD true)
else()
  set(JSPI_BUILD false)
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/shell.html ${CMAKE_BINARY_DIR}/shell.html @ONLY)
set_property(TARGET hamsterball APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
