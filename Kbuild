# SPDX-License-Identifier: GPL-2.0
#
# Every module in the lab, built in one kbuild invocation. Each directory
# has its own Kbuild file; the .ko files land next to their sources.

# Stage 1 / M1: module fundamentals
obj-m += src/modules/hello/
obj-m += src/modules/params/
obj-m += src/modules/logging/
obj-m += src/modules/lifecycle/

# Stage 2 / M2: character devices
obj-m += drivers/chardev/

# Warnings are errors unless LKL_WERROR=0 (used for exploratory W=1 builds).
LKL_WERROR ?= 1
ifeq ($(LKL_WERROR),1)
subdir-ccflags-y += -Werror
endif
