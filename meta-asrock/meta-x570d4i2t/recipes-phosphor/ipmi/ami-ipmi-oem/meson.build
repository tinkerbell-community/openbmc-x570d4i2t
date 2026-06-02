project(
    'ami-ipmi-oem',
    'cpp',
    version: '0.1',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2',
        'werror=false',
    ],
)

ipmi_blob_dep        = dependency('phosphor-ipmi-blobs')
libipmid_dep         = dependency('libipmid')
libsystemd_dep       = dependency('libsystemd')
sdbusplus_dep        = dependency('sdbusplus')
phosphor_logging_dep = dependency('phosphor-logging')

# Shared converter sources used by both libraries.
converter_sources = ['converter.cpp']

# Phosphor-ipmi-blobs handler at /ami/smbios.
shared_module(
    'amismbiosstore',
    ['main.cpp', 'handler.cpp'] + converter_sources,
    dependencies: [
        ipmi_blob_dep,
        libipmid_dep.partial_dependency(includes: true),
        libsystemd_dep,
        sdbusplus_dep,
        phosphor_logging_dep,
    ],
    implicit_include_directories: false,
    install: true,
    install_dir: get_option('libdir') / 'blob-ipmid',
)

# Old-style ipmid provider exposing the AMI legacy + MDR V2 commands on
# NetFn 0x3A. Loaded by phosphor-ipmi-host from /usr/lib/ipmid-providers.
shared_module(
    'amilegacy',
    ['legacy.cpp'] + converter_sources,
    dependencies: [
        libipmid_dep,
        libsystemd_dep,
    ],
    implicit_include_directories: false,
    install: true,
    install_dir: get_option('libdir') / 'ipmid-providers',
)
