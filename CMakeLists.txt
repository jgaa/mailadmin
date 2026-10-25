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

option(MAILADMIN_BUILD_API "Build the Courier administration API server" ON)
option(MAILADMIN_BUILD_UI "Build the experimental Wt UI server" ON)
option(MAILADMIN_BUILD_TESTS "Build regression tests" OFF)
if(MAILADMIN_BUILD_TESTS)
    enable_testing()
endif()

FetchContent_Declare(logfault
    GIT_REPOSITORY https://github.com/jgaa/logfault.git
    GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(logfault)
set(RESTINCURL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RESTINCURL_WITH_APIDOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(restincurl
    GIT_REPOSITORY https://github.com/jgaa/RESTinCurl.git
    GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(restincurl)

if(MAILADMIN_BUILD_API)
    add_subdirectory(api-server)
endif()
if(MAILADMIN_BUILD_UI)
    add_subdirectory(ui-server)
endif()
