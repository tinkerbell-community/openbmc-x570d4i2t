cmake_minimum_required(VERSION 3.21)
project(ami-ipmi-oem CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBIPMID      REQUIRED libipmid)
pkg_check_modules(PHOS_LOGGING  REQUIRED phosphor-logging)
pkg_check_modules(LIBSYSTEMD    REQUIRED libsystemd)

include(GNUInstallDirs)

add_library(ami-ipmi-oem SHARED ami-ipmi-oem.cpp)

set_target_properties(ami-ipmi-oem PROPERTIES
    VERSION 0.1
    SOVERSION 0)

target_include_directories(ami-ipmi-oem PRIVATE
    ${LIBIPMID_INCLUDE_DIRS}
    ${PHOS_LOGGING_INCLUDE_DIRS}
    ${LIBSYSTEMD_INCLUDE_DIRS})

target_link_libraries(ami-ipmi-oem PRIVATE
    ${LIBIPMID_LIBRARIES}
    ${PHOS_LOGGING_LIBRARIES}
    ${LIBSYSTEMD_LIBRARIES})

# Install to ipmid-providers/; the OE obmc-phosphor-ipmiprovider-symlink class
# globs for libami-ipmi-oem.so.? here and creates the host-ipmid/ symlink.
install(TARGETS ami-ipmi-oem
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ipmid-providers)
