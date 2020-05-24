EXTRA_CFLAGS += -O2
#EXTRA_CFLAGS += -I..

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m	:= dcsc.o

else

#KERNELDIR ?= /usr/src/kernels/3.10.0-1127.8.2.el7.x86_64
KERNELDIR ?= /usr/src/kernels/`uname -r`
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif



clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions Module.symvers modules.order
