cmake_minimum_required(VERSION 3.20)
project(thearchive LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# clangd needs a compilation database to know the include paths and flags. Without one
# it guesses, which on this project means failing to find drogon, sqlite3 and argon2.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Drogon and its trantor submodule still declare pre-3.5 minimums in places, which
# CMake 4 refuses outright rather than warning about. This is the supported escape
# hatch; drop it once upstream raises its own minimums.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
endif()

# Only the pieces we actually use. Postgres/MySQL/Redis would drag in client libraries
# the container has no reason to carry.
set(BUILD_CTL           OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(BUILD_DOC           OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(BUILD_ORM           OFF CACHE BOOL "" FORCE)
set(BUILD_POSTGRESQL    OFF CACHE BOOL "" FORCE)
set(BUILD_MYSQL         OFF CACHE BOOL "" FORCE)
set(BUILD_SQLITE        OFF CACHE BOOL "" FORCE)
set(BUILD_REDIS         OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.13
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(drogon)

# SQLite is driven directly rather than through Drogon's ORM: migrations and the GC
# sweep want plain synchronous transactions, and the query surface here is small
# enough that an ORM would be more indirection than help.
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# FindSQLite3's imported target changed name: CMake 4.x introduced SQLite3::SQLite3 and
# deprecated SQLite::SQLite3, while older releases — including the one in Debian
# bookworm, which the container builds on — define only the latter. Pick whichever
# exists so the same tree builds under both.
if(TARGET SQLite3::SQLite3)
    set(ARCHIVE_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(ARCHIVE_SQLITE_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR "SQLite3 found but neither imported target exists")
endif()

# argon2 ships no CMake config, so go through pkg-config and fall back to a plain
# library search for distributions that package it without a .pc file.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(ARGON2 IMPORTED_TARGET libargon2)
endif()
# libvips does the thumbnailing. It shrinks on load rather than decoding the full
# image, and brings HEIC/AVIF decode with it — which is the only way an iPhone upload
# becomes viewable, since browsers cannot render HEIC natively.
if(PkgConfig_FOUND)
    pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)
endif()

if(NOT TARGET PkgConfig::ARGON2)
    find_library(ARGON2_LIBRARY NAMES argon2 REQUIRED)
    find_path(ARGON2_INCLUDE_DIR NAMES argon2.h REQUIRED)
    add_library(argon2_fallback INTERFACE)
    target_link_libraries(argon2_fallback INTERFACE ${ARGON2_LIBRARY})
    target_include_directories(argon2_fallback INTERFACE ${ARGON2_INCLUDE_DIR})
    add_library(PkgConfig::ARGON2 ALIAS argon2_fallback)
endif()

add_executable(thearchive
    src/main.cc
    src/db.cc
    src/crypto.cc
    src/auth.cc
    src/tus.cc
    src/mimetype.cc
    src/shares.cc
    src/gc.cc
    src/zipstream.cc
    src/thumbnail.cc
    src/admin.cc
    src/storage.cc
    src/chat.cc
    src/csp.cc
    src/throttle.cc
    src/privacy.cc
    src/profiles.cc
    src/text.cc
    src/inbox.cc
)

target_include_directories(thearchive PRIVATE src)
target_link_libraries(thearchive PRIVATE
    drogon
    ${ARCHIVE_SQLITE_TARGET}
    OpenSSL::Crypto
    ZLIB::ZLIB
    PkgConfig::ARGON2
    PkgConfig::VIPS
)
target_compile_options(thearchive PRIVATE -Wall -Wextra -Wpedantic)

# The build tree deliberately lives outside the repo (on the Linux filesystem, for
# speed), so clangd would never find the database on its own. Copy it back beside the
# sources on every build.
add_custom_target(compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
    COMMENT "Refreshing compile_commands.json for clangd"
    VERBATIM
)
