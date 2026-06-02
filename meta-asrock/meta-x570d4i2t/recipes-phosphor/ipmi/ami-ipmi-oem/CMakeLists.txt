cmake_minimum_required(VERSION 3.21)
project(ami-ipmi-oem CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBIPMID      REQUIRED libipmid)
pkg_check_modules(SDBUSPLUS     REQUIRED sdbusplus)
pkg_check_modules(PHOS_LOGGING  REQUIRED phosphor-logging)

include(GNUInstallDirs)

add_library(ami-ipmi-oem SHARED ami-ipmi-oem.cpp)

set_target_properties(ami-ipmi-oem PROPERTIES
    VERSION 0.1
    SOVERSION 0)

target_compile_options(ami-ipmi-oem PRIVATE
    ${LIBIPMID_CFLAGS_OTHER}
    ${SDBUSPLUS_CFLAGS_OTHER}
    ${PHOS_LOGGING_CFLAGS_OTHER}
    -DBOOST_ASIO_DISABLE_THREADS
    -DBOOST_SYSTEM_NO_DEPRECATED
    -DBOOST_ERROR_CODE_HEADER_ONLY
    -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
    -DBOOST_ALL_NO_LIB)

target_include_directories(ami-ipmi-oem PRIVATE
    ${LIBIPMID_INCLUDE_DIRS}
    ${SDBUSPLUS_INCLUDE_DIRS}
    ${PHOS_LOGGING_INCLUDE_DIRS})

target_link_libraries(ami-ipmi-oem PRIVATE
    ${LIBIPMID_LIBRARIES}
    ${SDBUSPLUS_LIBRARIES}
    ${PHOS_LOGGING_LIBRARIES})

# Install to ipmid-providers/; the OE obmc-phosphor-ipmiprovider-symlink class
# globs for libami-ipmi-oem.so.? here and creates the host-ipmid/ symlink.
install(TARGETS ami-ipmi-oem
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ipmid-providers)
