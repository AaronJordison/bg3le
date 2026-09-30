# Builds against the Steam Runtime 3 ("sniper") SDK instead of the host, with the host's clang.
#
# The game runs inside the sniper container, which guarantees glibc 2.31. Built against the host's newer glibc, the
# library asks for symbol versions (acosf@GLIBC_2.43, __isoc23_strtol@GLIBC_2.38) that other machines don't have,
# and fails to load. glibc can't be linked statically into a library the game loads, so the fix is to build against
# the oldest glibc it has to run on. tools/build-sniper.sh sets both variables below and drives the whole build.
#
#   BG3LE_SNIPER_SYSROOT  the sniper SDK's filesystem
#   BG3LE_SNIPER_LIBCXX   libc++ built against it (unset while building libc++ itself)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)

# Environment, not cache: toolchain files are re-read inside try_compile, which doesn't see the cache.
set(CMAKE_SYSROOT "$ENV{BG3LE_SNIPER_SYSROOT}")
if(NOT IS_DIRECTORY "${CMAKE_SYSROOT}/usr/include")
    message(FATAL_ERROR "BG3LE_SNIPER_SYSROOT ('${CMAKE_SYSROOT}') is not a sysroot; run tools/build-sniper.sh")
endif()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
if(DEFINED ENV{BG3LE_SNIPER_LIBCXX})
    # Our libc++, not the host's: clang searches its own install's headers before the sysroot's, and those carry
    # the host's __config_site.
    set(_libcxx "$ENV{BG3LE_SNIPER_LIBCXX}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${_libcxx}")
    set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${_libcxx}/include/c++/v1")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${_libcxx}/lib")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${_libcxx}/lib")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
