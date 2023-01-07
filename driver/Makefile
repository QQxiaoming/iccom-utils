ifeq ($(KERNELRELEASE), )
KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD :=$(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR)  M=$(PWD)
clean:
	rm -rf .tmp_versions Module.symvers *.mod *.mod.c *.o *.ko .*.cmd built-in.a Module.markers modules.order .cache.mk
load:
	insmod symspi.ko
	insmod iccom.ko
	insmod iccom_socket_if.ko
unload:
	rmmod symspi
	rmmod iccom
	rmmod iccom_socket_if
install:
	cp symspi.ko /lib/modules/$(shell uname -r)/kernel/drivers/symspi.ko
	cp iccom.ko /lib/modules/$(shell uname -r)/kernel/drivers/iccom.ko
	cp iccom_socket_if.ko /lib/modules/$(shell uname -r)/kernel/drivers/iccom_socket_if.ko
else
	ccflags-y := -std=gnu99 -Wno-declaration-after-statement
	obj-m := symspi.o \
			 iccom.o \
			 iccom_socket_if.o
endif

