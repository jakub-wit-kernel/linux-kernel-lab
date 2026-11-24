# SPDX-License-Identifier: GPL-2.0
#
#   make                  build every module against the running kernel
#   make KDIR=/path       build against another kernel tree
#   make check            W=1 and sparse builds, then checkpatch
#   make modules-list     print the paths of all built .ko files
#   make clean
#
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

.PHONY: all modules w1 sparse checkpatch check modules-list clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Extra warnings; fails only on warnings in our own sources.
w1:
	$(MAKE) clean
	$(MAKE) -C $(KDIR) M=$(PWD) W=1 LKL_WERROR=0 modules 2>&1 | tee .w1.log
	@! grep -E '^($(PWD)/)?(src|drivers|networking)/[^:]+:[0-9]+:[0-9]+: (warning|error)' .w1.log

# C=2 runs sparse on every file; fails on any finding in our sources.
sparse:
	$(MAKE) clean
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 modules 2>&1 | tee .sparse.log
	@! grep -E '^($(PWD)/)?(src|drivers|networking)/[^:]+:[0-9]+:[0-9]+: (warning|error)' .sparse.log

checkpatch:
	scripts/checkpatch.sh

check: w1 sparse checkpatch

modules-list:
	@find src drivers networking -name '*.ko' | sort

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null
	rm -f .w1.log .sparse.log
	find src drivers networking -name '*.ko' -o -name '*.o' -o -name '*.mod' -o -name '*.mod.c' \
		-o -name '.*.cmd' -o -name 'modules.order' | xargs rm -f
	rm -f Module.symvers modules.order
