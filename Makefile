obj-m += indigo-gpioperiph.o board_file.o
EXTRA_CFLAGS=-W -Wall
EXTRA_LDFLAGS=-W -Wall
CC=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi-gcc
default: indigo-gpioperiph.ko

indigo-gpioperiph.ko: indigo-gpioperiph.c indigo-gpioperiph.h board_file.c
	make ARCH=arm CROSS_COMPILE=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi- -C linux M=`pwd`
	strings $@ | grep vermagic

clean:
	make ARCH=arm CROSS_COMPILE=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi- -C linux M=`pwd` clean
	rm -rf *.ko *.o

install:
	cp -fa indigo-gpioperiph.c /work/linux-2.6/drivers/misc/
	cp -fa indigo-gpioperiph.h /work/linux-2.6/include/linux/
