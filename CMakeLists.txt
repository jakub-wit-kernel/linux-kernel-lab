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

# --- unit tests (no kernel needed) ------------------------------------------------
enable_testing()
add_executable(test_lklbuf_lib tests/unit/test_lklbuf_lib.c)
target_include_directories(test_lklbuf_lib PRIVATE tests/lib)
target_link_libraries(test_lklbuf_lib PRIVATE lklbuf)
add_test(NAME test_lklbuf_lib COMMAND test_lklbuf_lib)
