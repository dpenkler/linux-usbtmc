ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := usbtmc.o
else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build
default:
	$(MAKE) -C $(KDIR) M=$$PWD 

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
	rm -f ttmc

endif
