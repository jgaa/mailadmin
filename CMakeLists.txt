cmake_minimum_required(VERSION 3.24)
project(mailadmin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <format>
int main() { return std::format(\"{}\", 42).size() != 2; }" MAILADMIN_HAS_STD_FORMAT)
if(NOT MAILADMIN_HAS_STD_FORMAT)
    message(FATAL_ERROR "Mailadmin/yahat require a C++20 standard library with std::format support")
endif()
include(FetchContent)
include(GNUInstallDirs)

# Source dependencies are always fetched from GitHub. Platform dependencies
# (Boost >= 1.81, OpenSSL, zlib and Connector/C) may use distribution binaries.
set(YAHAT_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(YAHAT_WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_METRICS ON CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_MCP OFF CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_JSON_RPC OFF CACHE BOOL "" FORCE)
set(USE_LOGFAULT ON CACHE BOOL "" FORCE)
FetchContent_Declare(logfault
    GIT_REPOSITORY https://github.com/jgaa/logfault.git
    GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(logfault)
FetchContent_Declare(yahat-cpp
    GIT_REPOSITORY https://github.com/jgaa/yahat-cpp.git
    GIT_TAG main GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(yahat-cpp)
# Yahat's existing-target branch adds ordering but does not propagate logfault's
# include directory; link the header-only target explicitly for its own sources.
target_link_libraries(yahat PUBLIC logfault::logfault)

find_package(OpenSSL REQUIRED)
find_library(MAILADMIN_CRYPT_LIBRARY crypt REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb)
configure_file(src/version.h.in generated/mailadmin/version.h @ONLY)

add_library(mailadmin_core STATIC
    src/Config.cpp src/Security.cpp src/Database.cpp src/Api.cpp
    src/Sql.cpp src/Administration.cpp src/Mailstore.cpp src/Reconciler.cpp)
target_include_directories(mailadmin_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mailadmin_core PUBLIC
    yahat-cpp::yahat-cpp logfault::logfault Boost::program_options OpenSSL::Crypto PkgConfig::MARIADB ${MAILADMIN_CRYPT_LIBRARY})
target_compile_options(mailadmin_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(mailadmin src/main.cpp)
target_link_libraries(mailadmin PRIVATE mailadmin_core)
install(TARGETS mailadmin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(MAILADMIN_BUILD_TESTS "Build security regression tests" OFF)
if(MAILADMIN_BUILD_TESTS)
    enable_testing()
    add_executable(mailadmin_security_test tests/security.cpp src/Security.cpp)
    target_include_directories(mailadmin_security_test PRIVATE src)
    target_link_libraries(mailadmin_security_test PRIVATE OpenSSL::Crypto ${MAILADMIN_CRYPT_LIBRARY})
    add_test(NAME security COMMAND mailadmin_security_test)
    add_executable(mailadmin_async_test tests/async.cpp)
    target_include_directories(mailadmin_async_test PRIVATE src)
    target_link_libraries(mailadmin_async_test PRIVATE yahat-cpp::yahat-cpp)
    add_test(NAME coroutine_bridge COMMAND mailadmin_async_test)
    add_executable(mailadmin_mailstore_test tests/mailstore.cpp)
    target_link_libraries(mailadmin_mailstore_test PRIVATE mailadmin_core)
    add_test(NAME mailstore COMMAND mailadmin_mailstore_test)
endif()
