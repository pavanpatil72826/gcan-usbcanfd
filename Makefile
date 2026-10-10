# @Copyright: Copyright (C) 2026 Gahan Ai Pvt Ltd
# @Author: Pavan Patil
# @Date: 2026-10-08
# @Last Modified by:   Pavan Patil
# @Last Modified time: 2026-10-10 13:39:11
# @Description: Build, load, install and uninstall the gcan_usbcanfd kernel module.

obj-m := gcan_usbcanfd.o

KVERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVERSION)/build
BUILDDIR := $(CURDIR)/build

all:
	mkdir -p $(BUILDDIR)
	$(MAKE) -C $(KDIR) M=$(BUILDDIR) src=$(CURDIR) modules

clean:
	rm -rf $(BUILDDIR)

# Load for testing (needs root): the CAN core modules first, then the driver.
load: all
	sudo modprobe can
	sudo modprobe can_raw
	sudo modprobe can_dev
	sudo insmod $(BUILDDIR)/gcan_usbcanfd.ko

unload:
	-sudo rmmod gcan_usbcanfd

install: all
	sudo mkdir -p /lib/modules/$(KVERSION)/extra
	sudo cp $(BUILDDIR)/gcan_usbcanfd.ko /lib/modules/$(KVERSION)/extra/
	sudo depmod -a

# Remove the copy that `make install` put into /lib/modules and refresh the module index.
# (A module that is currently loaded stays loaded until `make unload` or a reboot.)
uninstall:
	sudo rm -f /lib/modules/$(KVERSION)/extra/gcan_usbcanfd.ko
	sudo depmod -a

.PHONY: all clean load unload install uninstall
