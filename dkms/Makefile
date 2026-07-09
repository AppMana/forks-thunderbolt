KDIR ?= /lib/modules/$(shell uname -r)/build
TBFIX_SOURCE_DIR ?= $(shell readlink -f $(CURDIR)/../source 2>/dev/null || pwd)

.PHONY: all clean

# Source is staged preserving the real drivers/ tree so that net's in-tree
# include "../../thunderbolt/thunderbolt_negotiation.h" (the one canonical
# negotiation header shared by core and net) resolves exactly as it does
# in-tree. net links against the core's freshly built Module.symvers.
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt modules
	cp $(CURDIR)/drivers/thunderbolt/Module.symvers $(CURDIR)/Module.symvers
	cp $(CURDIR)/drivers/thunderbolt/Module.symvers $(TBFIX_SOURCE_DIR)/Module.symvers
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt \
		KBUILD_EXTRA_SYMBOLS=$(CURDIR)/drivers/thunderbolt/Module.symvers modules
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/infiniband/sw/rxe modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt clean
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt clean
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/infiniband/sw/rxe clean
	$(RM) $(CURDIR)/Module.symvers
