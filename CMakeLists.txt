# SPDX-License-Identifier: GPL-2.0
#
# User-space side of the lab: libraries, CLIs and test programs.
# Kernel modules are built by kbuild (see Makefile / Kbuild), not CMake.
#
#   cmake -S . -B build && cmake --build build
#   (cd build && ctest)          # unit tests that need no kernel
#
cmake_minimum_required(VERSION 3.16)
project(linux-kernel-lab-user LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(LKL_SANITIZE "" CACHE STRING "e.g. address,undefined")

add_compile_options(-Wall -Wextra -Werror -Wshadow -Wformat=2
                    -Wstrict-prototypes -Wvla)
if(LKL_SANITIZE)
  add_compile_options(-fsanitize=${LKL_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LKL_SANITIZE})
endif()
find_package(Threads REQUIRED)

# UAPI headers are shared with the kernel modules.
set(LKL_UAPI ${PROJECT_SOURCE_DIR}/include/uapi)

# --- M2: lkl_buf ---------------------------------------------------------------
add_library(lklbuf STATIC drivers/chardev/user/lklbuf.c)
target_include_directories(lklbuf PUBLIC drivers/chardev/user ${LKL_UAPI})

add_executable(lklbuf-cli drivers/chardev/user/lklbuf_cli.c)
set_target_properties(lklbuf-cli PROPERTIES OUTPUT_NAME lklbuf)
target_link_libraries(lklbuf-cli PRIVATE lklbuf)

# Device test: needs the module loaded; run by tests/drivers/test_lkl_buf.sh.
add_executable(lklbuf_test tests/drivers/lklbuf_test.c)
target_include_directories(lklbuf_test PRIVATE tests/lib)
target_link_libraries(lklbuf_test PRIVATE lklbuf Threads::Threads)

# --- M3: lkl_events ----------------------------------------------------------------
# Interface test: needs the module loaded; run by tests/drivers/test_lkl_events.sh.
add_executable(lkl_events_test tests/drivers/lkl_events_test.c)
target_include_directories(lkl_events_test PRIVATE tests/lib ${LKL_UAPI})
target_link_libraries(lkl_events_test PRIVATE Threads::Threads)

# --- M5: lkl_mem ---------------------------------------------------------------------
# Interface test: needs the module loaded; run by tests/memory/test_lkl_mem.sh.
add_executable(lkl_mmap_test tests/memory/lkl_mmap_test.c)
target_include_directories(lkl_mmap_test PRIVATE tests/lib)

# --- M7: eBPF ------------------------------------------------------------------------
# Needs clang (BPF target) and libbpf >= 1.0: a build from
# scripts/build-libbpf.sh (-DLKL_LIBBPF_DIR=...) or a distro package found
# with pkg-config. Without them the rest of the build goes on, and the eBPF
# tests skip with the reason.
set(LKL_LIBBPF_DIR "" CACHE PATH "Prefix of a static libbpf build (default: pkg-config)")
option(LKL_BPF "Build the eBPF programs and lklbpf when clang and libbpf are found" ON)

set(LKL_BPF_ENABLED OFF)
if(LKL_BPF)
  set(_bpf_why "")
  find_program(LKL_BPF_CLANG NAMES clang)
  if(LKL_LIBBPF_DIR)
    if(EXISTS ${LKL_LIBBPF_DIR}/lib/libbpf.a AND
       EXISTS ${LKL_LIBBPF_DIR}/include/bpf/libbpf.h)
      set(LKL_LIBBPF_INCLUDE ${LKL_LIBBPF_DIR}/include)
      set(LKL_LIBBPF_LIBS ${LKL_LIBBPF_DIR}/lib/libbpf.a elf z)
    else()
      set(_bpf_why "no lib/libbpf.a and include/bpf/libbpf.h under ${LKL_LIBBPF_DIR}")
    endif()
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBBPF QUIET libbpf>=1.0)
    endif()
    if(LIBBPF_FOUND)
      set(LKL_LIBBPF_INCLUDE ${LIBBPF_INCLUDE_DIRS})
      set(LKL_LIBBPF_LIBS ${LIBBPF_LINK_LIBRARIES})
    else()
      set(_bpf_why "libbpf >= 1.0 not found by pkg-config and LKL_LIBBPF_DIR not set")
    endif()
  endif()
  if(NOT LKL_BPF_CLANG)
    set(_bpf_why "clang not found")
  endif()
  if(_bpf_why)
    message(STATUS "eBPF: skipped: ${_bpf_why} (see ebpf/README.md)")
  else()
    set(LKL_BPF_ENABLED ON)
    message(STATUS "eBPF: enabled (clang ${LKL_BPF_CLANG}, libbpf ${LKL_LIBBPF_LIBS})")
  endif()
endif()

if(LKL_BPF_ENABLED)
  set(_bpf_flags -g -O2 -target bpf -Wall -Werror
      -I${PROJECT_SOURCE_DIR}/ebpf/include)
  foreach(_d ${LKL_LIBBPF_INCLUDE})
    list(APPEND _bpf_flags -I${_d})
  endforeach()
  # -target bpf does not search the multiarch directory (asm/types.h).
  # CMake knows it on Debian-style systems, whatever the C compiler's
  # own triple is (gcc: x86_64-linux-gnu, clang: x86_64-pc-linux-gnu).
  if(CMAKE_LIBRARY_ARCHITECTURE AND
     EXISTS /usr/include/${CMAKE_LIBRARY_ARCHITECTURE})
    list(APPEND _bpf_flags -idirafter /usr/include/${CMAKE_LIBRARY_ARCHITECTURE})
  endif()
  set(_bpf_objs)
  foreach(_p xdp_filter tc_dscp napi_hist)
    set(_out ${CMAKE_BINARY_DIR}/ebpf/${_p}.bpf.o)
    add_custom_command(OUTPUT ${_out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/ebpf
      COMMAND ${LKL_BPF_CLANG} ${_bpf_flags}
              -c ${PROJECT_SOURCE_DIR}/ebpf/progs/${_p}.bpf.c -o ${_out}
      DEPENDS ebpf/progs/${_p}.bpf.c ebpf/include/lklbpf.h
      COMMENT "BPF ${_p}.bpf.o" VERBATIM)
    list(APPEND _bpf_objs ${_out})
  endforeach()
  add_custom_target(bpf-objects ALL DEPENDS ${_bpf_objs})

  # Loads the objects from ebpf/ next to the executable.
  add_executable(lklbpf ebpf/user/lklbpf.c)
  target_include_directories(lklbpf PRIVATE ebpf/include)
  target_include_directories(lklbpf SYSTEM PRIVATE ${LKL_LIBBPF_INCLUDE})
  target_link_libraries(lklbpf PRIVATE ${LKL_LIBBPF_LIBS})
  add_dependencies(lklbpf bpf-objects)
endif()

# XDP benchmark traffic generator (benchmarks/xdp/run.sh); no libbpf needed.
add_executable(udpflood benchmarks/xdp/udpflood.c)

# --- unit tests (no kernel needed) ------------------------------------------------
enable_testing()
add_executable(test_lklbuf_lib tests/unit/test_lklbuf_lib.c)
target_include_directories(test_lklbuf_lib PRIVATE tests/lib)
target_link_libraries(test_lklbuf_lib PRIVATE lklbuf)
add_test(NAME test_lklbuf_lib COMMAND test_lklbuf_lib)
