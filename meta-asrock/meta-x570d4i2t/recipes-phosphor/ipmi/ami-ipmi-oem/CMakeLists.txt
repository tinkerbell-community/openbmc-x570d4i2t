cmake_minimum_required(VERSION 3.21)
project(ami-ipmi-oem CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBIPMID      REQUIRED libipmid)
pkg_check_modules(SDBUSPLUS     REQUIRED sdbusplus)
pkg_check_modules(PHOS_LOGGING  REQUIRED phosphor-logging)

include(GNUInstallDirs)

add_library(ami-ipmi-oem SHARED ami-ipmi-oem.cpp)

target_compile_options(ami-ipmi-oem PRIVATE
    ${LIBIPMID_CFLAGS_OTHER}
    ${SDBUSPLUS_CFLAGS_OTHER}
    ${PHOS_LOGGING_CFLAGS_OTHER})

target_include_directories(ami-ipmi-oem PRIVATE
    ${LIBIPMID_INCLUDE_DIRS}
    ${SDBUSPLUS_INCLUDE_DIRS}
    ${PHOS_LOGGING_INCLUDE_DIRS})

target_link_libraries(ami-ipmi-oem PRIVATE
    ${LIBIPMID_LIBRARIES}
    ${SDBUSPLUS_LIBRARIES}
    ${PHOS_LOGGING_LIBRARIES})

# Install to ${libdir}; the OE obmc-phosphor-ipmiprovider-symlink class
# creates the ipmid-providers/ symlink automatically.
install(TARGETS ami-ipmi-oem
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
