
#####################################################################
# Uncomment these lines to have ac97 support
#CONFIG_VIDEO_STK1160_AC97 := stk1160-ac97.o
#ccflags-y += -DCONFIG_VIDEO_STK1160_AC97

#####################################################################
#
# Set the path to your compiled kernel here
#
KERNEL_PATH = /foo/bar/raspberry-github

#####################################################################
#
# Set the architecture, you want to compile to
# Leave it empty if you want to compile for your host machine
#
#ARCH=sh4
#ARCH=x86
ARCH=arm

#####################################################################
#
# Set the cross-compiler prefix
# Leave it empty if you want to compile for your host machine
#
CROSS_CC=armv6j-hardfloat-linux-gnueabi-

stk1160-y := 	stk1160-core.o \
		stk1160-v4l.o \
		stk1160-video.o \
		stk1160-i2c.o \
		$(CONFIG_VIDEO_STK1160_AC97)

obj-m += stk1160.o

ccflags-y += -Wall
ccflags-y += -Idrivers/media/video

all:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) modules
clean:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) clean
install:
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_CC) make -C $(KERNEL_PATH) M=$(shell pwd) modules_install
