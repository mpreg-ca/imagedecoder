# Lets a meson project consume this library: builds the CMake superbuild
# and hands back a dependency. A checkout can be dropped straight into
# subprojects/ and used as-is, with no wrap download.
#
# The CMake build is the real one; this is a thin front for it.
project('imagedecoder', 'cpp', version: '1.0.0', meson_version: '>=1.1.0')

fs = import('fs')
cmake_prog = find_program('cmake')

source_dir = meson.current_source_dir()
# Built inside the checkout, not the consumer's build directory: a wheel
# build globs that directory for shared objects.
build_dir = source_dir / 'build'

# IMAGEDECODER_FETCH_PREBUILT_DEPS downloads the dependency tarball for
# this checkout's pins - a minute instead of twenty-five. Falls back to a
# source build if none exists; that's also when this step goes silent for a
# while, since run_command doesn't stream.
run_command(cmake_prog, '-S', source_dir, '-B', build_dir,
  '-DIMAGEDECODER_FETCH_PREBUILT_DEPS=ON',
  check: true)

# The library only, not the CLI example.
run_command(cmake_prog, '--build', build_dir,
  '--target', 'ep_imagedecoder', '--config', 'Release', '--parallel',
  check: true)

# Asked of pkg-config directly rather than dependency(), which can't be
# pointed at a prefix from inside meson.build. --static pulls in the private
# dependency chains (libpng's Requires.private: zlib, etc), so the consumer
# ends up self-contained.
pkgconfig_dir = build_dir / 'fakeroot' / 'lib' / 'pkgconfig'
if not fs.exists(pkgconfig_dir / 'imagedecoder.pc')
  error('imagedecoder built but left no imagedecoder.pc in ' + pkgconfig_dir)
endif
# Windows has no pkg-config, so the CMake build above pins one into the
# fakeroot; elsewhere that path is absent and the system's is found.
pkgconfig_prog = find_program(
  build_dir / 'fakeroot' / 'bin' / 'pkg-config.exe',
  build_dir / 'fakeroot' / 'bin' / 'pkg-config',
  'pkg-config', 'pkgconf')
pkgconfig_env = {'PKG_CONFIG_PATH': pkgconfig_dir}

cflags = run_command(pkgconfig_prog, '--static', '--cflags', 'imagedecoder',
  env: pkgconfig_env, check: true).stdout().split()
libs = run_command(pkgconfig_prog, '--static', '--libs', 'imagedecoder',
  env: pkgconfig_env, check: true).stdout().split()

# MSVC: map pkg-config's -lNAME to the file actually installed, and drop
# Unix runtimes.
if meson.get_compiler('cpp').get_argument_syntax() == 'msvc'
  libdir = build_dir / 'fakeroot' / 'lib'
  unix_only = ['stdc++', 'c++', 'pthread', 'm', 'dl', 'rt', 'atomic']
  msvc_libs = []
  foreach arg : libs
    if not arg.startswith('-l')
      msvc_libs += arg
      continue
    endif
    name = arg.substring(2)
    if name in unix_only
      continue
    endif
    base = name
    if base.endswith('-static') or base.endswith('_static')
      base = base.substring(0, -7)
    elif base.endswith('static')
      base = base.substring(0, -6)
    endif
    stems = []
    foreach b : [name, base]
      foreach prefix : ['', 'lib']
        foreach suffix : ['', '-static', '_static', 'static']
          stems += prefix + b + suffix
        endforeach
      endforeach
    endforeach
    if name == 'z'
      stems += ['zlibstatic', 'zlib', 'zs', 'zlib-ng', 'zlibstatic-ng']
    endif
    found = ''
    foreach stem : stems
      foreach ext : ['.lib', '.a']
        if found == '' and fs.exists(libdir / stem + ext)
          found = libdir / stem + ext
        endif
      endforeach
    endforeach
    # Unresolved names pass through, so the linker's error names them.
    if found == ''
      warning('no library in ' + libdir + ' for ' + arg + '; files there: ' +
        ' '.join(run_command('cmd', '/c', 'dir', '/b', libdir,
          check: false).stdout().split()))
    endif
    msvc_libs += found == '' ? arg : found
  endforeach
  libs = msvc_libs
endif

imagedecoder_dep = declare_dependency(
  compile_args: cflags,
  link_args: libs,
)
