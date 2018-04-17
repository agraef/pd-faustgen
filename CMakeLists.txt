cmake_minimum_required(VERSION 2.8)

set(CMAKE_SUPPRESS_REGENERATION true)
set(CMAKE_MACOSX_RPATH Off)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.4)
set(CMAKE_OSX_ARCHITECTURES "i386;x86_64")

include(pd.build/pd.cmake)

project(faust~ CXX)
set_pd_sources(${PROJECT_SOURCE_DIR}/pure-data/src/)
set_pd_external_path("${PROJECT_SOURCE_DIR}/external/")
add_pd_external(faust_tilde_project faust~ ${PROJECT_SOURCE_DIR}/src/faust_tilde.cpp)

add_library(faust STATIC IMPORTED)
set_target_properties(faust PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/faust/build/lib/libfaust.a)
target_link_libraries(faust_tilde_project faust)
target_include_directories(faust_tilde_project PRIVATE "${PROJECT_SOURCE_DIR}/faust/architecture")