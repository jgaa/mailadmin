project(mailadmin_api_server VERSION "${mailadmin_VERSION}" LANGUAGES CXX)

# Source dependencies are always fetched from GitHub. Platform dependencies
# (Boost >= 1.81, OpenSSL, zlib and Connector/C) may use distribution binaries.
set(YAHAT_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(YAHAT_WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_METRICS ON CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_MCP OFF CACHE BOOL "" FORCE)
set(YAHAT_ENABLE_JSON_RPC OFF CACHE BOOL "" FORCE)
set(USE_LOGFAULT ON CACHE BOOL "" FORCE)
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
    src/Sql.cpp src/Administration.cpp src/Mailstore.cpp src/Reconciler.cpp src/Dns.cpp)
target_compile_definitions(mailadmin_core PRIVATE RESTINCURL_ENABLE_ASIO=1)
target_include_directories(mailadmin_core SYSTEM PRIVATE "${restincurl_SOURCE_DIR}/include")
target_include_directories(mailadmin_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mailadmin_core PUBLIC
    yahat-cpp::yahat-cpp RESTinCurl::RESTinCurl logfault::logfault Boost::program_options OpenSSL::Crypto PkgConfig::MARIADB ${MAILADMIN_CRYPT_LIBRARY})
target_compile_options(mailadmin_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(mailadmin src/main.cpp)
target_link_libraries(mailadmin PRIVATE mailadmin_core)
install(TARGETS mailadmin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MAILADMIN_BUILD_TESTS)
    add_executable(mailadmin_security_test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/security.cpp src/Security.cpp)
    target_include_directories(mailadmin_security_test PRIVATE src)
    target_link_libraries(mailadmin_security_test PRIVATE OpenSSL::Crypto ${MAILADMIN_CRYPT_LIBRARY})
    add_test(NAME security COMMAND mailadmin_security_test)
    add_executable(mailadmin_async_test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/async.cpp)
    target_include_directories(mailadmin_async_test PRIVATE src)
    target_link_libraries(mailadmin_async_test PRIVATE yahat-cpp::yahat-cpp)
    add_test(NAME coroutine_bridge COMMAND mailadmin_async_test)
    add_executable(mailadmin_mailstore_test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/mailstore.cpp)
    target_link_libraries(mailadmin_mailstore_test PRIVATE mailadmin_core)
    add_test(NAME mailstore COMMAND mailadmin_mailstore_test)
    add_executable(mailadmin_dns_test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/dns.cpp)
    target_link_libraries(mailadmin_dns_test PRIVATE mailadmin_core)
    add_test(NAME dns_configuration COMMAND mailadmin_dns_test)
endif()
