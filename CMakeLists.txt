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
list(FILTER LUA_SRC EXCLUDE REGEX ".*/(lua|luac|onelua|ltests)\\.c$")
set_source_files_properties(${LUA_SRC} PROPERTIES LANGUAGE CXX)
add_library(ocglua STATIC ${LUA_SRC})
target_include_directories(ocglua PUBLIC ${CORE_DIR}/lua/src)
target_compile_options(ocglua PRIVATE -x c++ -include ${CORE_DIR}/lua/luaconf-customize.h -w)

# --- ygopro-core (AGPL-3.0)
set(CORE_SRC card.cpp duel.cpp effect.cpp field.cpp interpreter.cpp libcard.cpp libdebug.cpp
    libduel.cpp libeffect.cpp libgroup.cpp ocgapi.cpp operations.cpp playerop.cpp processor.cpp
    processor_visit.cpp scriptlib.cpp)
list(TRANSFORM CORE_SRC PREPEND ${CORE_DIR}/)
add_library(ocgcore STATIC ${CORE_SRC})
target_include_directories(ocgcore PUBLIC ${CORE_DIR})
target_link_libraries(ocgcore PUBLIC ocglua)
target_compile_options(ocgcore PRIVATE -fno-rtti -w)

# --- engine
add_library(ygoe STATIC
  engine/src/carddb.cpp
  engine/src/duel.cpp
  engine/src/deck.cpp
  engine/src/profile.cpp
  engine/src/evaluate.cpp
  engine/src/search.cpp
  engine/src/side.cpp
  engine/src/analyze.cpp)
target_include_directories(ygoe PUBLIC engine/src)
target_link_libraries(ygoe PUBLIC ocgcore)
target_compile_options(ygoe PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(ygoe PUBLIC Threads::Threads)

add_executable(ygoe-cli engine/cli/main.cpp)
target_link_libraries(ygoe-cli PRIVATE ygoe)
