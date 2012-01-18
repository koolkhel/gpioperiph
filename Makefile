obj-m += indigo-gpioperiph.o board_file.o
EXTRA_CFLAGS=-W -Wall
EXTRA_LDFLAGS=-W -Wall
CC=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi-gcc
default: indigo-gpioperiph.ko relay

indigo-gpioperiph.ko: indigo-gpioperiph.c indigo-gpioperiph.h board_file.c
	make ARCH=arm CROSS_COMPILE=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi- -C linux M=`pwd`
	strings piopins.ko | grep vermagic

clean:
	make ARCH=arm CROSS_COMPILE=/home/yury/toolchain/arm-indigo-linux-gnueabi/bin/arm-indigo-linux-gnueabi- -C linux M=`pwd` clean

install: default
	lftp -f ftp-script-install