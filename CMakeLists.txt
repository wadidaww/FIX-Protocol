cmake_minimum_required(VERSION 3.25)
project(FIXEngine
    VERSION 1.0.0
    DESCRIPTION "Production-ready FIX Protocol Engine"
    LANGUAGES CXX
)

# =============================================================================
# Global settings
# =============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# =============================================================================
# Options
# =============================================================================
option(FIX_BUILD_TESTS   "Build unit tests"          ON)
option(FIX_BUILD_APPS    "Build reference apps"      ON)
option(FIX_BUILD_SHARED  "Build shared library"      OFF)
option(FIX_ENABLE_ASAN   "Enable AddressSanitizer"   OFF)
option(FIX_ENABLE_TSAN   "Enable ThreadSanitizer"    OFF)
option(FIX_ENABLE_UBSAN  "Enable UBSanitizer"        OFF)
option(FIX_ENABLE_LTO    "Enable Link-Time Optimisation" OFF)
option(FIX_NO_EXCEPTIONS "Build without exceptions (-fno-exceptions)" OFF)

# =============================================================================
# Compiler flags
# =============================================================================
add_library(fix_compile_options INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(fix_compile_options INTERFACE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    )
    if(FIX_ENABLE_ASAN)
        target_compile_options(fix_compile_options INTERFACE -fsanitize=address,undefined)
        target_link_options(fix_compile_options    INTERFACE -fsanitize=address,undefined)
    endif()
    if(FIX_ENABLE_TSAN)
        target_compile_options(fix_compile_options INTERFACE -fsanitize=thread)
        target_link_options(fix_compile_options    INTERFACE -fsanitize=thread)
    endif()
    if(FIX_ENABLE_UBSAN AND NOT FIX_ENABLE_ASAN)
        target_compile_options(fix_compile_options INTERFACE -fsanitize=undefined)
        target_link_options(fix_compile_options    INTERFACE -fsanitize=undefined)
    endif()
    if(FIX_NO_EXCEPTIONS)
        target_compile_options(fix_compile_options INTERFACE -fno-exceptions -fno-rtti)
    endif()
    if(FIX_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_supported OUTPUT lto_output)
        if(lto_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()
elseif(MSVC)
    target_compile_options(fix_compile_options INTERFACE
        /W4 /WX-
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
endif()

# =============================================================================
# Dependencies
# =============================================================================
find_package(Threads REQUIRED)

# Optional: spdlog for structured logging
find_package(spdlog QUIET)

# =============================================================================
# Library sources
# =============================================================================
set(FIX_SOURCES
    src/parser/parser.cpp
    src/parser/serializer.cpp
    src/dictionary/data_dictionary.cpp
    src/session/session.cpp
    src/session/session_manager.cpp
    src/store/file_store.cpp
    src/transport/tcp_transport.cpp
    src/log/message_log.cpp
    src/engine.cpp
)

set(FIX_HEADERS
    include/fix/core/types.hpp
    include/fix/core/field.hpp
    include/fix/core/message.hpp
    include/fix/core/constants.hpp
    include/fix/parser/parser.hpp
    include/fix/parser/serializer.hpp
    include/fix/dictionary/data_dictionary.hpp
    include/fix/session/session.hpp
    include/fix/session/session_manager.hpp
    include/fix/store/message_store.hpp
    include/fix/store/memory_store.hpp
    include/fix/store/file_store.hpp
    include/fix/transport/transport.hpp
    include/fix/transport/tcp_transport.hpp
    include/fix/log/message_log.hpp
    include/fix/engine.hpp
)

if(FIX_BUILD_SHARED)
    add_library(fixengine SHARED ${FIX_SOURCES} ${FIX_HEADERS})
else()
    add_library(fixengine STATIC ${FIX_SOURCES} ${FIX_HEADERS})
endif()

target_include_directories(fixengine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(fixengine
    PUBLIC  fix_compile_options Threads::Threads
)

if(spdlog_FOUND)
    target_link_libraries(fixengine PUBLIC spdlog::spdlog)
    target_compile_definitions(fixengine PUBLIC FIX_USE_SPDLOG=1)
endif()

# On Linux, link against libatomic if needed
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(fixengine PUBLIC atomic)
endif()

set_target_properties(fixengine PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME fixengine
)

# =============================================================================
# Tests
# =============================================================================
if(FIX_BUILD_TESTS)
    enable_testing()

    # Try to find GTest via package manager or FetchContent
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(fix_tests
        tests/test_parser.cpp
        tests/test_message.cpp
        tests/test_session.cpp
        tests/test_dictionary.cpp
        tests/test_store.cpp
    )

    target_link_libraries(fix_tests
        PRIVATE fixengine GTest::gtest GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(fix_tests)

    add_test(NAME fix_unit_tests COMMAND fix_tests)
endif()

# =============================================================================
# Applications
# =============================================================================
if(FIX_BUILD_APPS)
    add_executable(fix_oms apps/oms/main.cpp)
    target_link_libraries(fix_oms PRIVATE fixengine)
    set_target_properties(fix_oms PROPERTIES OUTPUT_NAME fix_oms)
endif()

# =============================================================================
# Install rules
# =============================================================================
install(TARGETS fixengine
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

# =============================================================================
# Package config
# =============================================================================
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FIXEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# =============================================================================
# clang-format targets
# =============================================================================
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-18 clang-format-17)
if(CLANG_FORMAT_EXE)
    file(
        GLOB_RECURSE ALL_CXX_SOURCE_FILES
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
        "${PROJECT_SOURCE_DIR}/apps/*.cpp"
    )
    add_custom_target(
        format
        COMMAND ${CLANG_FORMAT_EXE} -i --style=file ${ALL_CXX_SOURCE_FILES}
        COMMENT "Running clang-format on all sources"
        VERBATIM
    )
    add_custom_target(
        format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror --style=file ${ALL_CXX_SOURCE_FILES}
        COMMENT "Checking clang-format compliance"
        VERBATIM
    )
endif()

# =============================================================================
# Summary
# =============================================================================
message(STATUS "=== FIX Protocol Engine ===")
message(STATUS "  Version:     ${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:${CMAKE_CXX_STANDARD}")
message(STATUS "  Tests:       ${FIX_BUILD_TESTS}")
message(STATUS "  Apps:        ${FIX_BUILD_APPS}")
message(STATUS "  Shared lib:  ${FIX_BUILD_SHARED}")
message(STATUS "  ASAN:        ${FIX_ENABLE_ASAN}")
message(STATUS "  TSAN:        ${FIX_ENABLE_TSAN}")
message(STATUS "  LTO:         ${FIX_ENABLE_LTO}")
