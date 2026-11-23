cmake_minimum_required(VERSION 3.16)
project(ygo_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ygopro-core)
if(NOT EXISTS ${CORE_DIR}/ocgapi.h)
  message(FATAL_ERROR "ygopro-core not found. Run tools/bootstrap.sh first.")
endif()

# --- Lua, compiled as C++ with the core's custom config prepended (same as the core's own build)
file(GLOB LUA_SRC ${CORE_DIR}/lua/src/l*.c)
# Same exclusions as the core's own Android build: only base/string/table/math(/io) are opened.
list(FILTER LUA_SRC EXCLUDE REGEX ".*/(lua|luac|onelua|ltests|lbitlib|lcorolib|ldblib|linit|loadlib|loslib|lutf8lib)\\.c$")
set_source_files_properties(${LUA_SRC} PROPERTIES LANGUAGE CXX)
add_library(ocglua STATIC ${LUA_SRC})
target_include_directories(ocglua PUBLIC ${CORE_DIR}/lua/src)
target_compile_options(ocglua PRIVATE -x c++ -fexceptions -include ${CORE_DIR}/lua/luaconf-customize.h -w)

# --- ygopro-core (AGPL-3.0)
set(CORE_SRC card.cpp duel.cpp effect.cpp field.cpp interpreter.cpp libcard.cpp libdebug.cpp
    libduel.cpp libeffect.cpp libgroup.cpp ocgapi.cpp operations.cpp playerop.cpp processor.cpp
    processor_visit.cpp scriptlib.cpp)
list(TRANSFORM CORE_SRC PREPEND ${CORE_DIR}/)
add_library(ocgcore STATIC ${CORE_SRC})
target_include_directories(ocgcore PUBLIC ${CORE_DIR})
target_link_libraries(ocgcore PUBLIC ocglua)
target_compile_options(ocgcore PRIVATE -fno-rtti -fexceptions -w)

# --- engine
add_library(ygoe STATIC
  engine/src/carddb.cpp
  engine/src/duel.cpp
  engine/src/deck.cpp
  engine/src/profile.cpp
  engine/src/evaluate.cpp
  engine/src/valuenet.cpp
  engine/src/search.cpp
  engine/src/side.cpp
  engine/src/analyze.cpp
  engine/src/json.cpp
  engine/src/live.cpp
  engine/src/reference.cpp
  engine/src/replay.cpp
  engine/src/policy.cpp
  engine/src/combo.cpp
  engine/src/pieces.cpp
  engine/src/explain.cpp
  engine/src/api.cpp)
target_include_directories(ygoe PUBLIC engine/src)
target_link_libraries(ygoe PUBLIC ocgcore)
target_compile_options(ygoe PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(ygoe PUBLIC Threads::Threads)

if(ANDROID)
  set(_cli_default OFF)
  set(_jni_default ON)
else()
  set(_cli_default ON)
  set(_jni_default OFF)
endif()
option(YGOE_BUILD_CLI "Build the ygoe-cli command-line tool" ${_cli_default})
option(YGOE_BUILD_JNI "Build libygoe_jni for the Android/Kotlin module" ${_jni_default})

if(YGOE_BUILD_CLI)
  add_executable(ygoe-cli engine/cli/main.cpp engine/cli/bench.cpp engine/cli/ml.cpp)
  target_link_libraries(ygoe-cli PRIVATE ygoe)
endif()

if(YGOE_BUILD_JNI)
  add_library(ygoe_jni SHARED engine/jni/ygoe_jni.cpp)
  target_link_libraries(ygoe_jni PRIVATE ygoe)
  if(NOT ANDROID)
    if(YGOE_JNI_INCLUDE_DIRS)
      # Cross-compiling (e.g. for Windows): JDK headers given by hand.
      target_include_directories(ygoe_jni PRIVATE ${YGOE_JNI_INCLUDE_DIRS})
    else()
      # Desktop build (tests, desktop app): headers from the local JDK.
      find_package(JNI REQUIRED COMPONENTS JVM)
      target_include_directories(ygoe_jni PRIVATE ${JNI_INCLUDE_DIRS})
    endif()
  endif()
  if(WIN32)
    # ygoe_jni.dll (no "lib" prefix, so System.loadLibrary("ygoe_jni") finds it), with the C++
    # runtime linked in so the DLL needs nothing else installed.
    set_target_properties(ygoe_jni PROPERTIES PREFIX "")
    if(MINGW)
      target_link_options(ygoe_jni PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
    endif()
  endif()
  # Only the JNI entry points need to be visible.
  set_target_properties(ygoe_jni PROPERTIES CXX_VISIBILITY_PRESET hidden)
  if(ANDROID)
    target_link_options(ygoe_jni PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
    target_link_libraries(ygoe_jni PRIVATE log)
  endif()
endif()
