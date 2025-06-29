cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(qunet VERSION 0.1.0)

option(QUNET_QUIC_SUPPORT "Enable QUIC support" ON)
option(QUNET_DEBUG "Enable debug mode" OFF)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(qunet STATIC ${SOURCES})

target_include_directories(qunet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (QUNET_DEBUG)
    target_compile_definitions(qunet PUBLIC QUNET_DEBUG=1)
    message(STATUS "Qunet: debug mode enabled!")
endif()

# cpm
if (NOT CURRENT_CPM_VERSION)
    include(cmake/CPM.cmake)
endif()

# add deps
CPMAddPackage("gh:dankmeme01/qsox#main")
CPMAddPackage("gh:dankmeme01/asp2#fa6a253")
CPMAddPackage("gh:c-ares/c-ares#v1.34.5")
CPMAddPackage("gh:fmtlib/fmt#11.2.0")

target_link_libraries(qunet PRIVATE c-ares fmt)
target_link_libraries(qunet PUBLIC qsox asp)

if (QUNET_QUIC_SUPPORT)
    # TODO: we might move this out of here if we will need TLS but not for QUIC
    set(WOLFSSL_DEBUG_OPTIONS "")

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(WOLFSSL_DEBUG_OPTIONS "WOLFSSL_DEBUG ON")
    endif()

    CPMAddPackage(
        NAME wolfssl
        GIT_REPOSITORY "https://github.com/wolfSSL/wolfssl.git"
        GIT_TAG "v5.8.0-stable"
        OPTIONS "WOLFSSL_QUIC ON"
                "WOLFSSL_HARDEN OFF"
                "WOLFSSL_OLD_TLS OFF"
                "WOLFSSL_TLSV12 OFF" # TODO websockets: enable this?
                "WOLFSSL_SESSION_TICKET ON"
                "WOLFSSL_INSTALL OFF"
                "WOLFSSL_CRYPT_TESTS OFF"
                "WOLFSSL_EXAMPLES OFF"
                "BUILD_SHARED_LIBS OFF"
                ${WOLFSSL_DEBUG_OPTIONS}
    )

    get_target_property(WOLFSSL_DEFINITIONS wolfssl COMPILE_DEFINITIONS)
    get_target_property(WOLFSSL_INCLUDE_DIRS wolfssl INCLUDE_DIRECTORIES)

    CPMAddPackage(
        NAME ngtcp2
        GIT_REPOSITORY "https://github.com/ngtcp2/ngtcp2.git"
        GIT_TAG "v1.13.0"
        OPTIONS "ENABLE_WOLFSSL ON"
                "WOLFSSL_INCLUDE_DIR ${wolfssl_SOURCE_DIR}"
                "WOLFSSL_LIBRARY $<TARGET_FILE:wolfssl>"
                "ENABLE_OPENSSL OFF"
                "ENABLE_GNUTLS OFF"
                "ENABLE_PICOTLS OFF"
                "ENABLE_BORINGSSL OFF"
                "ENABLE_LIB_ONLY ON"
                "ENABLE_STATIC_LIB ON"
                "ENABLE_SHARED_LIB OFF"
                "BUILD_TESTING OFF"
    )

    if (ngtcp2_ADDED)
        target_compile_definitions(ngtcp2_crypto_wolfssl_static PRIVATE ${WOLFSSL_DEFINITIONS})
        target_include_directories(ngtcp2_crypto_wolfssl_static PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    endif()

    target_link_libraries(qunet PRIVATE wolfssl ngtcp2 ngtcp2_crypto_wolfssl_static)
endif()

# Qunet tester
file(GLOB_RECURSE TESTER_SOURCES CONFIGURE_DEPENDS tester/*.cpp)
add_executable(qunet_tester ${TESTER_SOURCES})
target_link_libraries(qunet_tester PRIVATE qunet qsox)
