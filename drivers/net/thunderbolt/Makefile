# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_USB4_NET) := thunderbolt_net.o
thunderbolt_net-objs := main.o trace.o

# Login-handshake KUnit tests, built into the module only on a CONFIG_KUNIT
# kernel. Run via drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py).
thunderbolt_net-$(CONFIG_USB4_NET_KUNIT_TEST) += tests/tbnet_login_test.o

# Tracepoints need to know where to find trace.h
CFLAGS_trace.o := -I$(src)
