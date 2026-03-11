# normal makefile
KDIR ?= /lib/modules/`uname -r`/build
default:
	$(MAKE) -C $(KDIR) M=$$PWD 

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

tmcterm: tmcterm.c
	cc -o tmcterm tmcterm.c -lreadline

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
	rm -f ttmc screendump tmcterm

