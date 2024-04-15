KERNEL := /home/namubuntu/linux
TOOLCHAIN := aarch64-linux-gnu-

EXTRA_CFLAGS = -Wall
obj-m += ssd1306.o
all:
	make ARCH=arm64 CROSS_COMPILE=${TOOLCHAIN} -C ${KERNEL} M=`pwd` modules
clean:
	make -C ${KERNEL} M=`pwd` clean
