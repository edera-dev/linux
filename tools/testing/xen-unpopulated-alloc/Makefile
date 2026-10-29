# SPDX-License-Identifier: GPL-2.0
#
# Runs the Xen unpopulated allocator in userspace:
#   make && ./unpopulated-alloc
# Needs the AddressSanitizer runtime (libasan).

CFLAGS += -g -O1 -Wall -fsanitize=address -fsanitize=undefined
LDFLAGS += -fsanitize=address -fsanitize=undefined

TARGETS = unpopulated-alloc

unpopulated-alloc: main.c shim.h generated/unpopulated-alloc.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c

# The kernel source as it is, without its #include lines: shim.h stands in for
# every header it uses.
generated/unpopulated-alloc.c: ../../../drivers/xen/unpopulated-alloc.c
	@mkdir -p generated
	sed -e '/^#include/d' < $< > $@

clean:
	$(RM) -r $(TARGETS) generated

.PHONY: clean
