CPMAddPackage("gh:google/googletest@1.16.0")

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(qunet_tests ${TEST_SOURCES})
target_link_libraries(qunet_tests PRIVATE qunet gtest_main)

include(GoogleTest)
gtest_discover_tests(qunet_tests)
