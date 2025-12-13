cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(qunet VERSION 1.0.0)

option(QUNET_QUIC_SUPPORT "Enable QUIC support" OFF)
option(QUNET_ADVANCED_DNS "Enable advanced DNS resolver" ON)
option(QUNET_DEBUG "Enable debug mode" OFF)
option(QUNET_BUILD_TESTER OFF)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(qunet STATIC ${SOURCES})

target_include_directories(qunet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(qunet PRIVATE src)

if (QUNET_DEBUG)
    target_compile_definitions(qunet PUBLIC QUNET_DEBUG=1)
    message(STATUS "Qunet: debug mode enabled!")
endif()

# i absolutely hate microsoft
if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _HAS_ITERATOR_DEBUGGING=0)
    target_compile_definitions(qunet PUBLIC NOMINMAX _WINSOCKAPI_)
endif()

# cpm
if (NOT CURRENT_CPM_VERSION)
    include(cmake/CPM.cmake)
endif()

# add deps
set(ARC_DEBUG ${QUNET_DEBUG})
CPMAddPackage("gh:dankmeme01/asp2#c8328fc")
CPMAddPackage("gh:dankmeme01/arc#9998913")
CPMAddPackage("gh:fmtlib/fmt#11.2.0")
CPMAddPackage("gh:zhihaoy/nontype_functional#bdb0987")

# pain and suffering

CPMAddPackage("gh:facebook/zstd#v1.5.7")
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_STATIC ON)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_TESTS OFF)
set(ZSTD_BUILD_CONTRIB OFF)
set(ZSTD_MULTITHREAD_SUPPORT OFF)
set(ZSTD_LEGACY_SUPPORT OFF)
add_subdirectory(${zstd_SOURCE_DIR}/build/cmake zstd)

CPMAddPackage("gh:lz4/lz4#v1.10.0")
set(LZ4_BUILD_CLI OFF)
add_subdirectory(${lz4_SOURCE_DIR}/build/cmake lz4)

CPMAddPackage("gh:BLAKE3-team/BLAKE3#1.8.2")
if (WIN32)
    set(BLAKE3_SIMD_TYPE "none")
endif()
add_subdirectory(${BLAKE3_SOURCE_DIR}/c blake3)

target_link_libraries(qunet PRIVATE libzstd_static lz4_static blake3)
target_link_libraries(qunet PUBLIC qsox asp arc fmt std23::nontype_functional)

if (QUNET_ADVANCED_DNS)
    CPMAddPackage(
        NAME c-ares
        GIT_REPOSITORY "https://github.com/c-ares/c-ares.git"
        GIT_TAG "main" # TODO: use stable version when a 1.34.6 or 1.35 is out
        OPTIONS "CARES_STATIC ON"
                "CARES_SHARED OFF"
                "CARES_INSTALL OFF"
                "CARES_BUILD_TOOLS OFF"
    )

    target_compile_options(c-ares PRIVATE "-Wno-sign-conversion")
    target_compile_definitions(qunet PUBLIC QUNET_ADVANCED_DNS=1)
    target_link_libraries(qunet PRIVATE c-ares)
endif()

if (QUNET_QUIC_SUPPORT)
    target_compile_definitions(qunet PUBLIC QUNET_QUIC_SUPPORT=1 QUNET_TLS_SUPPORT=1)

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

    if (MSVC)
        target_compile_options(wolfssl PRIVATE /w)
        target_compile_options(ngtcp2_static PRIVATE /w)
        target_compile_options(ngtcp2_crypto_wolfssl_static PRIVATE /w)
    else()
        target_compile_options(wolfssl PRIVATE -w)
        target_compile_options(ngtcp2_static PRIVATE -w)
        target_compile_options(ngtcp2_crypto_wolfssl_static PRIVATE -w)
    endif()

    target_link_libraries(qunet PRIVATE wolfssl ngtcp2_static ngtcp2_crypto_wolfssl_static)
endif()

if (QUNET_BUILD_TESTER)
    # Qunet tester
    file(GLOB_RECURSE TESTER_SOURCES CONFIGURE_DEPENDS tester/*.cpp)
    add_executable(qunet_tester ${TESTER_SOURCES})
    target_link_libraries(qunet_tester PRIVATE qunet qsox)

    # add asan
    target_compile_options(qunet_tester PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(qunet_tester PRIVATE -fsanitize=address)
endif()
