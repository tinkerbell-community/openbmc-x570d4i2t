project(
    'asrock-ipmi-oem',
    'cpp',
    version: '0.1',
    meson_version: '>=1.1.1',
    default_options: [
        'warning_level=3',
        'werror=false',
        'cpp_std=c++23',
    ],
)

# Boost and ASIO are header-only in phosphor; suppress thread noise
add_project_arguments(
    [
        '-DBOOST_ERROR_CODE_HEADER_ONLY',
        '-DBOOST_SYSTEM_NO_DEPRECATED',
        '-DBOOST_ALL_NO_LIB',
        '-DBOOST_NO_RTTI',
        '-DBOOST_NO_TYPEID',
        '-DBOOST_ASIO_DISABLE_THREADS',
        '-DBOOST_COROUTINES_NO_DEPRECATION_WARNING',
        # Silence PSABI/pedantic warnings that fire for packed structs
        '-Wno-psabi',
        '-Wno-pedantic',
    ],
    language: 'cpp',
)

# -----------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------

nlohmann_json_dep        = dependency('nlohmann_json',           include_type: 'system')
phosphor_dbus_ifaces_dep = dependency('phosphor-dbus-interfaces')
phosphor_logging_dep     = dependency('phosphor-logging')
sdbusplus_dep            = dependency('sdbusplus')
ipmid_dep                = dependency('libipmid')

asrock_oem_deps = [
    ipmid_dep,
    nlohmann_json_dep,
    phosphor_dbus_ifaces_dep,
    phosphor_logging_dep,
    sdbusplus_dep,
]

# -----------------------------------------------------------------------
# Shared library – loaded by phosphor-ipmi-host at runtime
# -----------------------------------------------------------------------

asrockoemcmds_src = [
    'src/amiconverter.cpp',
    'src/spdreader.cpp',
    'src/smbiosbuilder.cpp',
    'src/appcommands.cpp',
    'src/chassiscommands.cpp',
    'src/oemcommands.cpp',
    'src/sensorcommands.cpp',
    'src/storagecommands.cpp',
]

library(
    'zasrockoemcmds',
    sources: asrockoemcmds_src,
    include_directories: include_directories('.', 'include'),
    implicit_include_directories: false,
    dependencies: asrock_oem_deps,
    version: meson.project_version(),
    # Allow unresolved symbols – resolved by the ipmid host at load time
    override_options: ['b_lundef=false'],
    install: true,
    install_dir: get_option('libdir') / 'ipmid-providers',
)
