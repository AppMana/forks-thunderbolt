KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

# Source is staged preserving the real drivers/ tree so that net's in-tree
# include "../../thunderbolt/thunderbolt_negotiation.h" (the one canonical
# negotiation header shared by core and net) resolves exactly as it does
# in-tree. net links against the core's freshly built Module.symvers.
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt modules
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt \
		KBUILD_EXTRA_SYMBOLS=$(CURDIR)/drivers/thunderbolt/Module.symvers modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt clean
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt clean
