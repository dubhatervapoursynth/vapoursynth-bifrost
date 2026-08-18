project('Bifrost', 'cpp',
    default_options: ['buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'WTFPL',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '4.0',
)

py = import('python').find_installation(pure: false)

# Ask the VapourSynth Python module where its headers live rather than going through
# pkg-config, which is often absent on Windows and unnecessary here: the module is installed
# alongside every VapourSynth install and knows the answer already. Use the same interpreter
# that provides the install directory below, so the headers and the plugin directory always
# come from one VapourSynth install.
incdir = include_directories(
    run_command(
        py,
        '-c',
        'import vapoursynth as vs; print(vs.get_include())',
        check: true,
    ).stdout().strip(),
)

shared_module('bifrost',
    files('src/bifrost.cpp'),
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    name_prefix: '',
)
