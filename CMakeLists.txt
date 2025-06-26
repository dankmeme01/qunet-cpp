cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(qunet VERSION 0.1.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(qunet STATIC ${SOURCES})

target_include_directories(qunet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# cpm
if (NOT CURRENT_CPM_VERSION)
    include(cmake/CPM.cmake)
endif()

# add deps
CPMAddPackage("gh:dankmeme01/qsox#main")
CPMAddPackage("gh:dankmeme01/asp2#fa6a253")
CPMAddPackage("gh:c-ares/c-ares#v1.34.5")
CPMAddPackage("gh:fmtlib/fmt#11.2.0")
target_link_libraries(qunet qsox asp c-ares fmt)

# Qunet tester
file(GLOB_RECURSE TESTER_SOURCES CONFIGURE_DEPENDS tester/*.cpp)
add_executable(qunet_tester ${TESTER_SOURCES})
target_link_libraries(qunet_tester PRIVATE qunet qsox)
