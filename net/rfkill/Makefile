# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the RF switch subsystem.
#

rfkill-y			+= core.o
rfkill-$(CONFIG_RFKILL_INPUT)	+= input.o
obj-$(CONFIG_RFKILL)		+= rfkill.o
obj-$(CONFIG_RFKILL_GPIO)	+= rfkill-gpio.o
rfkill-rk-$(CONFIG_RFKILL_RK) := rfkill-wlan.o rfkill-bt.o
rfkill-rk-$(CONFIG_RFKILL_JZ) := rfkill-modem.o rfkill-bt.o
obj-$(CONFIG_RFKILL_RK)		+= rfkill-rk.o
