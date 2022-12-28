#!/bin/bash

export PATH=~/buildroot/output/host/bin:$PATH

#make ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf- distclean &&

make ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf- imx_v7_epc_m6g2c_wifi_defconfig &&

#make ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf- menuconfig

make ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf- all -j$(nproc)
