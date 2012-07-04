stk1160-y := stk1160-core.o stk1160-v4l.o stk1160-video.o stk1160-i2c.o stk1160-ac97.o

obj-m += stk1160.o

ccflags-y += -Wall
ccflags-y += -Idrivers/media/video

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean
install:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules_install
