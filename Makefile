obj-m := gcan_usbcanfd.o

KVERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVERSION)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# Load for testing (needs root): the CAN core modules first, then the driver.
load: all
	sudo modprobe can
	sudo modprobe can_raw
	sudo modprobe can_dev
	sudo insmod gcan_usbcanfd.ko

unload:
	-sudo rmmod gcan_usbcanfd

install: all
	sudo mkdir -p /lib/modules/$(KVERSION)/extra
	sudo cp gcan_usbcanfd.ko /lib/modules/$(KVERSION)/extra/
	sudo depmod -a

.PHONY: all clean load unload install
