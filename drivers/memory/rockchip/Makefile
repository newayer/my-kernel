# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Rockchip DSMC drivers.
#

ccflags-y := -Wno-unused-function -Wall

obj-$(CONFIG_ROCKCHIP_DSMC) += dsmc.o
dsmc-y += dsmc-controller.o dsmc-lb-device.o dsmc-host.o

obj-$(CONFIG_ROCKCHIP_DSMC_SLAVE)      += dsmc-lb-slave.o
