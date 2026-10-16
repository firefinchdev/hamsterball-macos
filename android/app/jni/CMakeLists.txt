# Native build for the Android app: libmain.so (the compatibility layer) plus SDL2.
cmake_minimum_required(VERSION 3.18)
project(hamsterball C CXX)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
if(NOT EXISTS ${TP}/src/SDL2 OR NOT EXISTS ${TP}/unicorn-android)
  message(FATAL_ERROR "Run android/third_party/fetch_deps.sh and android/third_party/build_unicorn_android.sh first")
endif()

# SDL2 (shared library, loaded by SDLActivity)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/src/SDL2 sdl2)

# gl4es: desktop OpenGL (fixed function) on top of OpenGL ES 2, linked statically and
# initialised by us after SDL creates the ES context.
set(STATICLIB ON CACHE BOOL "" FORCE)
set(NO_INIT_CONSTRUCTOR ON CACHE BOOL "" FORCE)
set(NOX11 ON CACHE BOOL "" FORCE)
set(USE_ANDROID_LOG ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/src/gl4es gl4es)

# libopenmpt (MO3 music) with its built-in Vorbis and MP3 decoders
set(OMPT ${TP}/src/libopenmpt)
# take the source list from libopenmpt's own Android makefile
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
target_compile_options(openmpt PRIVATE -fexceptions -frtti -w)
set_target_properties(openmpt PROPERTIES CXX_STANDARD 20 C_STANDARD 17)

# Unicorn (prebuilt by build_unicorn_android.sh)
add_library(unicorn STATIC IMPORTED)
set_target_properties(unicorn PROPERTIES
  IMPORTED_LOCATION ${TP}/unicorn-android/lib/${ANDROID_ABI}/libunicorn.a
  INTERFACE_INCLUDE_DIRECTORIES ${TP}/unicorn-android/include)

# the compatibility layer itself (SDL's Java side loads it as libmain.so)
file(GLOB HB_SRC CONFIGURE_DEPENDS ${REPO}/src/*.cpp)
add_library(main SHARED ${HB_SRC})
target_include_directories(main PRIVATE ${TP}/src/SDL2/include ${TP}/src/gl4es/include)
target_compile_definitions(main PRIVATE GL_GLEXT_PROTOTYPES)
target_compile_options(main PRIVATE -O2 -Wno-unused-function -Wno-deprecated-declarations)
set_target_properties(main PROPERTIES CXX_STANDARD 17)
target_link_libraries(main SDL2 GL openmpt unicorn z log android dl m)
