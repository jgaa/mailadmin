project(mailadmin_ui_server VERSION "${mailadmin_VERSION}" LANGUAGES CXX)
option(MAILADMIN_UI_DEPENDENCIES_ONLY "Prepare compiled UI dependencies for container caching" OFF)

find_package(Boost 1.81 REQUIRED COMPONENTS json program_options)
find_package(OpenSSL REQUIRED)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CONNECTOR_HTTP ON CACHE BOOL "" FORCE)
set(CONNECTOR_FCGI OFF CACHE BOOL "" FORCE)
foreach(feature LIBWTDBO LIBWTTEST SELENIUM_TESTS HARU PANGO SQLITE POSTGRES
        FIREBIRD MYSQL MSSQLSERVER QT4 QT5 QT6 SAML OPENGL UNWIND)
    set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(INSTALL_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(INSTALL_THEMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(wt
    GIT_REPOSITORY https://github.com/emweb/wt.git
    GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(wt)

# Upstream Wt exports installation includes, but not its build-tree includes.
target_include_directories(wt SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${wt_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${wt_BINARY_DIR}>")

if(NOT MAILADMIN_UI_DEPENDENCIES_ONLY)
    configure_file(src/BuildConfig.h.in generated/BuildConfig.h @ONLY)
    configure_file(wt_config.xml wt_config.xml COPYONLY)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/public")
    add_subdirectory(src)
endif()

install(FILES wt_config.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mailadmin-ui)
install(DIRECTORY "${wt_SOURCE_DIR}/resources/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mailadmin-ui/resources)
