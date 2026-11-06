cmake_minimum_required(VERSION 3.20)
project(rpk VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LibLZMA REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static)
    set(RPK_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(RPK_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd)
    set(RPK_ZSTD_TARGET zstd::libzstd)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(RPK_ZSTD_TARGET PkgConfig::ZSTD)
endif()

add_library(rpk_core
    src/hash.cpp
    src/chunker.cpp
    src/scanner.cpp
    src/precomp.cpp
    src/archive.cpp
)
target_include_directories(rpk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rpk_core PUBLIC LibLZMA::LibLZMA OpenSSL::Crypto ZLIB::ZLIB Threads::Threads ${RPK_ZSTD_TARGET})

add_executable(rpk src/main.cpp)
target_link_libraries(rpk PRIVATE rpk_core)

if(MSVC)
    target_compile_options(rpk_core PRIVATE /W4 /permissive-)
    target_compile_options(rpk PRIVATE /W4 /permissive-)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/rpk.manifest" RPK_MANIFEST_PATH)
    target_link_options(rpk PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${RPK_MANIFEST_PATH}")
else()
    target_compile_options(rpk_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(rpk PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(WIN32)
    add_executable(rpk_gui WIN32 src/gui_win32.cpp)
    target_link_libraries(rpk_gui PRIVATE rpk_core comctl32 comdlg32 shell32 ole32)
    if(MSVC)
        target_compile_options(rpk_gui PRIVATE /W4 /permissive-)
        target_link_options(rpk_gui PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${RPK_MANIFEST_PATH}")
    else()
        target_compile_options(rpk_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

enable_testing()
add_executable(rpk_tests tests/test_main.cpp)
target_link_libraries(rpk_tests PRIVATE rpk_core)
target_compile_definitions(rpk_tests PRIVATE RPK_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(MSVC)
    target_compile_options(rpk_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(rpk_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
add_test(NAME rpk_core_tests COMMAND rpk_tests)

install(TARGETS rpk RUNTIME DESTINATION bin)
if(WIN32)
    install(TARGETS rpk_gui RUNTIME DESTINATION bin)
endif()
