# Uncomment these lines to have ac97 support
#CONFIG_VIDEO_STK1160_AC97 := stk1160-ac97.o
#ccflags-y += -DCONFIG_VIDEO_STK1160_AC97

stk1160-y := 	stk1160-core.o \
		stk1160-v4l.o \
		stk1160-video.o \
		stk1160-i2c.o \
		$(CONFIG_VIDEO_STK1160_AC97)

obj-m += stk1160.o

ccflags-y += -Wall
ccflags-y += -Idrivers/media/video

KERNEL_PATH = /foo/bar/raspberry-github
ARCH=arm
CROSS_CC=armv6j-hardfloat-linux-gnueabi-

all:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) modules
clean:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) clean
install:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) modules_install
