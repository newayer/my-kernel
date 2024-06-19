/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PLAT_GPS_H
#define __PLAT_GPS_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/clk.h>

struct rksdmmc_iomux {
	char *name; //set the MACRO of gpio
	int fgpio;
	int fmux;
};

struct rksdmmc_gpio {
	int io; //set the address of gpio
	char name[64]; //
	int enable; // disable = !enable   //set the default value,i.e,GPIO_HIGH or GPIO_LOW
	struct rksdmmc_iomux iomux;
};

struct rksdmmc_gpio_modem_moudle {
	struct rksdmmc_gpio poweren_n; //5GPWR-EN-L
	struct rksdmmc_gpio on; //5G-ON
	struct regmap *grf;
};

#endif
