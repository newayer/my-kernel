/*
 * Copyright (C) 2012 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* Rock-chips rfkill driver for wifi
*/

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/rfkill.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/rfkill-wlan.h>
#include <linux/rfkill-bt.h>
#include <linux/wakelock.h>
#include <linux/interrupt.h>
#include <asm/irq.h>
#include <linux/suspend.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <dt-bindings/gpio/gpio.h>
#include <linux/skbuff.h>
#include <linux/fb.h>
#include <linux/rockchip/grf.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif
#include <linux/soc/rockchip/rk_vendor_storage.h>
#include <linux/device.h>

#include "../../drivers/mmc/core/pwrseq.h"

#if 0
#define DBG(x...) pr_info("[WLAN_RFKILL]: " x)
#else
#define DBG(x...)
#endif

#define LOG(x...) pr_info("[WLAN_RFKILL]: " x)

struct rfkill_wlan_data {
	struct rksdmmc_gpio_wifi_moudle *pdata;
	struct wake_lock wlan_irq_wl;
};


struct rksdmmc_gpio_modem_moudle {
    struct rksdmmc_gpio   fastboot;
    struct rksdmmc_gpio   ap_wake_dq;
    struct rksdmmc_gpio   reset_gpio;
    struct rksdmmc_gpio   poweren_gpio;
    struct rksdmmc_gpio   usb_detect;
    struct rksdmmc_gpio   usbsel;
    struct rksdmmc_gpio   usim_det;
    struct rksdmmc_gpio   pa_28v_en;
    struct rksdmmc_gpio   dq_4v0_en;
    struct rksdmmc_gpio   dq_wake_irq;
    struct rksdmmc_gpio   dq_slp_stat;
    struct rksdmmc_gpio   dq_irig;
    struct rksdmmc_gpio   wifi_pdn;
	struct rksdmmc_gpio   wifi_heat;
};

static struct rfkill_wlan_data *g_rfkill = NULL;
static int power_set_time = 0;
static int wifi_bt_vbat_state;
static int wifi_power_state;
static int wifi_reset_state = 0;

static const char wlan_name[] = "rkwifi";

static char wifi_chip_type_string[64];

static struct rksdmmc_gpio_modem_moudle g_modem = {0};

/***********************************************************
 * 
 * Broadcom Wifi Static Memory
 * 
 **********************************************************/
#ifdef CONFIG_RKWIFI
#define BCM_STATIC_MEMORY_SUPPORT 0
#else
#define BCM_STATIC_MEMORY_SUPPORT 0
#endif
//===========================
#if BCM_STATIC_MEMORY_SUPPORT
#define PREALLOC_WLAN_SEC_NUM 4
#define PREALLOC_WLAN_BUF_NUM 160
#define PREALLOC_WLAN_SECTION_HEADER 0
#define WLAN_SKB_BUF_NUM 16

#define WLAN_SECTION_SIZE_0 (12 * 1024)
#define WLAN_SECTION_SIZE_1 (12 * 1024)
#define WLAN_SECTION_SIZE_2 (32 * 1024)
#define WLAN_SECTION_SIZE_3 (136 * 1024)
#define WLAN_SECTION_SIZE_4 (4 * 1024)
#define WLAN_SECTION_SIZE_5 (64 * 1024)
#define WLAN_SECTION_SIZE_6 (4 * 1024)
#define WLAN_SECTION_SIZE_7 (4 * 1024)

static struct sk_buff *wlan_static_skb[WLAN_SKB_BUF_NUM + 1];

struct wifi_mem_prealloc {
	void *mem_ptr;
	unsigned long size;
};

static struct wifi_mem_prealloc wifi_mem_array[8] = {
	{ NULL, (WLAN_SECTION_SIZE_0) }, { NULL, (WLAN_SECTION_SIZE_1) },
	{ NULL, (WLAN_SECTION_SIZE_2) }, { NULL, (WLAN_SECTION_SIZE_3) },
	{ NULL, (WLAN_SECTION_SIZE_4) }, { NULL, (WLAN_SECTION_SIZE_5) },
	{ NULL, (WLAN_SECTION_SIZE_6) }, { NULL, (WLAN_SECTION_SIZE_7) }
};

static int rockchip_init_wifi_mem(void)
{
	int i;
	int j;

	for (i = 0; i < WLAN_SKB_BUF_NUM; i++) {
		wlan_static_skb[i] =
			dev_alloc_skb(((i < (WLAN_SKB_BUF_NUM / 2)) ?
				(PAGE_SIZE * 1) :
				(PAGE_SIZE * 2)));

		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	wlan_static_skb[i] = dev_alloc_skb((PAGE_SIZE * 4));
	if (!wlan_static_skb[i])
		goto err_skb_alloc;

	for (i = 0; i <= 7; i++) {
		wifi_mem_array[i].mem_ptr =
			kmalloc(wifi_mem_array[i].size, GFP_KERNEL);

		if (!wifi_mem_array[i].mem_ptr)
			goto err_mem_alloc;
	}
	return 0;

err_mem_alloc:
	pr_err("Failed to mem_alloc for WLAN\n");
	for (j = 0; j < i; j++)
		kfree(wifi_mem_array[j].mem_ptr);
	i = WLAN_SKB_BUF_NUM;
err_skb_alloc:
	pr_err("Failed to skb_alloc for WLAN\n");
	for (j = 0; j < i; j++)
		dev_kfree_skb(wlan_static_skb[j]);
	dev_kfree_skb(wlan_static_skb[j]);

	return -ENOMEM;
}

void *rockchip_mem_prealloc(int section, unsigned long size)
{
	if (section == PREALLOC_WLAN_SEC_NUM)
		return wlan_static_skb;

	if (section < 0 || section > 7)
		return NULL;

	if (wifi_mem_array[section].size < size)
		return NULL;

	return wifi_mem_array[section].mem_ptr;
}
#else
void *rockchip_mem_prealloc(int section, unsigned long size)
{
	return NULL;
}
#endif
EXPORT_SYMBOL(rockchip_mem_prealloc);

int rfkill_set_wifi_bt_power(int on)
{
	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *vbat;

	LOG("%s: %d\n", __func__, on);

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	vbat = &mrfkill->pdata->vbat_n;
	if (on) {
		if (gpio_is_valid(vbat->io))
			gpio_direction_output(vbat->io, vbat->enable);
	} else {
		if (gpio_is_valid(vbat->io))
			gpio_direction_output(vbat->io, !(vbat->enable));
	}
	wifi_bt_vbat_state = on;
	return 0;
}

/**************************************************************************
 *
 * get wifi power state Func
 *
 *************************************************************************/
int rfkill_get_wifi_power_state(int *power)
{
	struct rfkill_wlan_data *mrfkill = g_rfkill;

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	*power = wifi_power_state;

	return 0;
}
EXPORT_SYMBOL(rfkill_get_wifi_power_state);

/**************************************************************************
 *
 * Wifi Power Control Func
 * 0 -> power off
 * 1 -> power on
 *
 *************************************************************************/
int rockchip_wifi_power(int on)
{
	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *poweron, *reset;
	struct regulator *ldo = NULL;
	int bt_power = 0;
	bool toggle = false;

	LOG("%s: %d\n", __func__, on);

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	if (mrfkill->pdata->wifi_power_remain && power_set_time) {
		LOG("%s: wifi power is setted to be remain on.", __func__);
		return 0;
	}
	power_set_time++;

	if (mrfkill->pdata->mregulator.power_ctrl_by_pmu) {
		int ret = -1;
		char *ldostr;
		int level = mrfkill->pdata->mregulator.enable;

		ldostr = mrfkill->pdata->mregulator.pmu_regulator;
		if (!ldostr)
			return -1;
		ldo = regulator_get(NULL, ldostr);
		if (!ldo || IS_ERR(ldo)) {
			LOG("\n\n\n%s get ldo error,please mod this\n\n\n",
			    __func__);
			return -1;
		}
		if (on == level) {
			regulator_set_voltage(ldo, 3000000, 3000000);
			LOG("%s: %s enabled\n", __func__, ldostr);
			ret = regulator_enable(ldo);
			if (ret)
				LOG("ldo enable failed\n");
			wifi_power_state = 1;
			LOG("wifi turn on power.\n");
		} else {
			LOG("%s: %s disabled\n", __func__, ldostr);
			while (regulator_is_enabled(ldo) > 0) {
				ret = regulator_disable(ldo);
				if (ret)
					LOG("ldo disable failed\n");
			}
			wifi_power_state = 0;
			LOG("wifi shut off power.\n");
		}
		regulator_put(ldo);
		msleep(100);
	} else {
		poweron = &mrfkill->pdata->power_n;
		reset = &mrfkill->pdata->reset_n;

		if (on) {
			if (toggle) {
				rfkill_set_wifi_bt_power(1);
				msleep(100);
			}

			if (gpio_is_valid(poweron->io)) {
				gpio_direction_output(poweron->io, poweron->enable);
				msleep(100);
			}

			if (gpio_is_valid(reset->io)) {
				gpio_direction_output(reset->io, reset->enable);
				msleep(100);
			}

			wifi_power_state = 1;
			LOG("wifi turn on power [GPIO%d-%d]\n", poweron->io, poweron->enable);
		} else {
			if (gpio_is_valid(poweron->io)) {
				printk("wifi power off\n");
				gpio_direction_output(poweron->io, !(poweron->enable));
				msleep(100);
			}

			if (gpio_is_valid(reset->io)) {
				gpio_direction_output(reset->io, !(reset->enable));
			}

			wifi_power_state = 0;
			if (toggle) {
				if (!bt_power) {
					LOG("%s: wifi will set vbat to low\n", __func__);
					rfkill_set_wifi_bt_power(0);
				} else {
					LOG("%s: wifi shouldn't control the vbat\n", __func__);
				}
			}
			LOG("wifi shut off power [GPIO%d-%d]\n", poweron->io, !poweron->enable);
		}
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_wifi_power);

/**************************************************************************
 *
 * Wifi Sdio Detect Func
 *
 *************************************************************************/
int rockchip_wifi_set_carddetect(int val)
{
	return 0;
}
EXPORT_SYMBOL(rockchip_wifi_set_carddetect);

/**************************************************************************
 *
 * Wifi Get Interrupt irq Func
 *
 *************************************************************************/
int rockchip_wifi_get_oob_irq(void)
{
	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *wifi_int_irq;

	LOG("%s: Enter\n", __func__);

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	wifi_int_irq = &mrfkill->pdata->wifi_int_b;
	if (gpio_is_valid(wifi_int_irq->io)) {
		return gpio_to_irq(wifi_int_irq->io);
		//return wifi_int_irq->io;
	} else {
		LOG("%s: wifi OOB pin isn't defined.\n", __func__);
	}

	return -1;
}
EXPORT_SYMBOL(rockchip_wifi_get_oob_irq);

int rockchip_wifi_get_oob_irq_flag(void)
{
	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *wifi_int_irq;
	int gpio_flags = -1;

	if (mrfkill) {
		wifi_int_irq = &mrfkill->pdata->wifi_int_b;
		if (gpio_is_valid(wifi_int_irq->io))
			gpio_flags = wifi_int_irq->enable;
	}

	return gpio_flags;
}
EXPORT_SYMBOL(rockchip_wifi_get_oob_irq_flag);

/**************************************************************************
 *
 * Wifi Reset Func
 *
 *************************************************************************/
int rockchip_wifi_reset(int on)
{
	return 0;
}
EXPORT_SYMBOL(rockchip_wifi_reset);

/**************************************************************************
 *
 * Wifi MAC custom Func
 *
 *************************************************************************/
#include <linux/etherdevice.h>
#include <linux/errno.h>
u8 wifi_custom_mac_addr[6] = { 0, 0, 0, 0, 0, 0 };

//#define RANDOM_ADDRESS_SAVE
static int get_wifi_addr_vendor(unsigned char *addr)
{
	int ret;
	int count = 5;

	while (count-- > 0) {
		if (is_rk_vendor_ready())
			break;
		/* sleep 500ms wait rk vendor driver ready */
		msleep(500);
	}
	ret = rk_vendor_read(WIFI_MAC_ID, addr, 6);
	if (ret != 6 || is_zero_ether_addr(addr)) {
		LOG("%s: rk_vendor_read wifi mac address failed (%d)\n",
		    __func__, ret);
#ifdef CONFIG_WIFI_GENERATE_RANDOM_MAC_ADDR
		random_ether_addr(addr);
		LOG("%s: generate random wifi mac address: "
		    "%02x:%02x:%02x:%02x:%02x:%02x\n",
		    __func__, addr[0], addr[1], addr[2], addr[3], addr[4],
		    addr[5]);
		ret = rk_vendor_write(WIFI_MAC_ID, addr, 6);
		if (ret != 0) {
			LOG("%s: rk_vendor_write failed %d\n",
			    __func__, ret);
			memset(addr, 0, 6);
			return -1;
		}
#else
		return -1;
#endif
	} else {
		LOG("%s: rk_vendor_read wifi mac address: "
		    "%02x:%02x:%02x:%02x:%02x:%02x\n",
		    __func__, addr[0], addr[1], addr[2], addr[3], addr[4],
		    addr[5]);
	}
	return 0;
}

int rockchip_wifi_mac_addr(unsigned char *buf)
{
	char mac_buf[20] = { 0 };

	LOG("%s: enter.\n", __func__);

	// from vendor storage
	if (is_zero_ether_addr(wifi_custom_mac_addr)) {
		if (get_wifi_addr_vendor(wifi_custom_mac_addr) != 0)
			return -1;
	}

	sprintf(mac_buf, "%02x:%02x:%02x:%02x:%02x:%02x",
		wifi_custom_mac_addr[0], wifi_custom_mac_addr[1],
		wifi_custom_mac_addr[2], wifi_custom_mac_addr[3],
		wifi_custom_mac_addr[4], wifi_custom_mac_addr[5]);
	LOG("falsh wifi_custom_mac_addr=[%s]\n", mac_buf);

	if (is_valid_ether_addr(wifi_custom_mac_addr)) {
		if (!strncmp(wifi_chip_type_string, "rtl", 3))
			wifi_custom_mac_addr[0] &= ~0x2; // for p2p
	} else {
		LOG("This mac address is not valid, ignored...\n");
		return -1;
	}

	memcpy(buf, wifi_custom_mac_addr, 6);

	return 0;
}
EXPORT_SYMBOL(rockchip_wifi_mac_addr);

/**************************************************************************
 *
 * wifi get country code func
 *
 *************************************************************************/
struct cntry_locales_custom {
	char iso_abbrev[4]; /* ISO 3166-1 country abbreviation */
	char custom_locale[4]; /* Custom firmware locale */
	int custom_locale_rev; /* Custom local revisin default -1 */
};

static struct cntry_locales_custom country_cloc;

void *rockchip_wifi_country_code(char *ccode)
{
	struct cntry_locales_custom *mcloc;

	LOG("%s: set country code [%s]\n", __func__, ccode);
	mcloc = &country_cloc;
	memcpy(mcloc->custom_locale, ccode, 4);
	mcloc->custom_locale_rev = 0;

	return mcloc;
}
EXPORT_SYMBOL(rockchip_wifi_country_code);
/**************************************************************************/

static int rfkill_rk_setup_gpio(struct rksdmmc_gpio *gpio, const char *prefix,
				const char *name)
{
	if (gpio_is_valid(gpio->io)) {
		int ret = 0;

		sprintf(gpio->name, "%s_%s", prefix, name);
		ret = gpio_request(gpio->io, gpio->name);
		if (ret) {
			LOG("Failed to get %s gpio.\n", gpio->name);
			return -1;
		}
	}

	return 0;
}

#ifdef CONFIG_OF


static void modem_parse_dt(struct device_node *node)
{
	int gpio;
	enum of_gpio_flags flags;

	//memset(g_modem, 0, sizeof(*g_modem));

	gpio = of_get_named_gpio_flags(node, "WIFI,pdn_gpio", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.wifi_pdn.io = gpio;
		g_modem.wifi_pdn.enable = !flags;
		LOG("%s: MODEM,wifi_pdn = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.wifi_pdn.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "WIFI,heat_gpio", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.wifi_heat.io = gpio;
		g_modem.wifi_heat.enable = !flags;
		LOG("%s: MODEM,wifi_heat = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.wifi_heat.io = -1;
	}

	
	gpio = of_get_named_gpio_flags(node, "MODEM,fastboot", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.fastboot.io = gpio;
		g_modem.fastboot.enable = !flags;
		LOG("%s: MODEM,fastboot = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.fastboot.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,ap_wake_dq", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.ap_wake_dq.io = gpio;
		g_modem.ap_wake_dq.enable = !flags;
		LOG("%s: MODEM,ap_wake_dq = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.ap_wake_dq.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,reset_gpio", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.reset_gpio.io = gpio;
		g_modem.reset_gpio.enable = !flags;
		LOG("%s: MODEM,reset_gpio = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.reset_gpio.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,poweren_gpio", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.poweren_gpio.io = gpio;
		g_modem.poweren_gpio.enable = !flags;
		LOG("%s: MODEM,poweren_gpio = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.poweren_gpio.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,usb_detect", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.usb_detect.io = gpio;
		g_modem.usb_detect.enable = !flags;
		LOG("%s: MODEM,usb_detect = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.usb_detect.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,usbsel", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.usbsel.io = gpio;
		g_modem.usbsel.enable = !flags;
		LOG("%s: MODEM,usbsel = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.usbsel.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,usim_det", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.usim_det.io = gpio;
		g_modem.usim_det.enable = !flags;
		LOG("%s: MODEM,usim_det = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.usim_det.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,pa_28v_en", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.pa_28v_en.io = gpio;
		g_modem.pa_28v_en.enable = !flags;
		LOG("%s: MODEM,pa_28v_en = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.pa_28v_en.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_4v0_en", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_4v0_en.io = gpio;
		g_modem.dq_4v0_en.enable = !flags;
		LOG("%s: MODEM,dq_4v0_en = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_4v0_en.io = -1;
	}

/*
	gpio = of_get_named_gpio_flags(node, "MODEM,dq_id0", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_id0.io = gpio;
		g_modem.dq_id0.enable = !flags;
		LOG("%s: MODEM,dq_id0 = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_id0.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_id1", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_id1.io = gpio;
		g_modem.dq_id1.enable = !flags;
		LOG("%s: MODEM,dq_id1 = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_id1.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_id2", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_id2.io = gpio;
		g_modem.dq_id2.enable = !flags;
		LOG("%s: MODEM,dq_id2 = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_id2.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_id3", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_id3.io = gpio;
		g_modem.dq_id3.enable = !flags;
		LOG("%s: MODEM,dq_id3 = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_id3.io = -1;
	}
*/

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_wake_irq", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_wake_irq.io = gpio;
		g_modem.dq_wake_irq.enable = !flags;
		LOG("%s: MODEM,dq_wake_irq = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_wake_irq.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_slp_stat", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_slp_stat.io = gpio;
		g_modem.dq_slp_stat.enable = !flags;
		LOG("%s: MODEM,dq_slp_stat = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_slp_stat.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,dq_irig", 0,
						&flags);
	if (gpio_is_valid(gpio)) {
		g_modem.dq_irig.io = gpio;
		g_modem.dq_irig.enable = !flags;
		LOG("%s: MODEM,dq_irig = %d, flags = %d.\n",
			__func__, gpio, flags);
	} else {
		g_modem.dq_irig.io = -1;
	}

}

static int wlan_platdata_parse_dt(struct device *dev,
				  struct rksdmmc_gpio_wifi_moudle *data)
{
	struct device_node *node = dev->of_node;
	const char *strings;
	u32 value;
	int gpio, ret;
	enum of_gpio_flags flags;
	u32 ext_clk_value = 0;

	if (!node)
		return -ENODEV;

	memset(data, 0, sizeof(*data));

#ifdef CONFIG_MFD_SYSCON
	data->grf = syscon_regmap_lookup_by_phandle(node, "rockchip,grf");
	if (IS_ERR(data->grf)) {
		LOG("can't find rockchip,grf property\n");
		//return -1;
	}
#endif

	ret = of_property_read_string(node, "wifi_chip_type", &strings);
	if (ret) {
		LOG("%s: Can not read wifi_chip_type, set default to rkwifi.\n",
		    __func__);
		strcpy(wifi_chip_type_string, "rkwifi");
	} else {
		if (strings && strlen(strings) < 64)
			strcpy(wifi_chip_type_string, strings);
	}
	LOG("%s: wifi_chip_type = %s\n", __func__, wifi_chip_type_string);

	if (of_find_property(node, "keep_wifi_power_on", NULL)) {
		data->wifi_power_remain = true;
		LOG("%s: wifi power remain\n", __func__);
	} else {
		data->wifi_power_remain = false;
		LOG("%s: enable wifi power control.\n", __func__);
	}

	if (of_find_property(node, "power_ctrl_by_pmu", NULL)) {
		data->mregulator.power_ctrl_by_pmu = true;
		ret = of_property_read_string(node, "power_pmu_regulator",
					      &strings);
		if (ret) {
			LOG("%s: Can not read property: power_pmu_regulator.\n",
			    __func__);
			data->mregulator.power_ctrl_by_pmu = false;
		} else {
			LOG("%s: wifi power controlled by pmu(%s).\n", __func__,
			    strings);
			sprintf(data->mregulator.pmu_regulator, "%s", strings);
		}
		ret = of_property_read_u32(node, "power_pmu_enable_level",
					   &value);
		if (ret) {
			LOG("%s: Can not read: power_pmu_enable_level.\n",
			    __func__);
			data->mregulator.power_ctrl_by_pmu = false;
		} else {
			LOG("%s: wifi power controlled by pmu(level = %s).\n",
			    __func__, (value == 1) ? "HIGH" : "LOW");
			data->mregulator.enable = value;
		}
	} else {
		data->mregulator.power_ctrl_by_pmu = false;
		LOG("%s: wifi power controled by gpio.\n", __func__);
		gpio = of_get_named_gpio_flags(node, "WIFI,poweren_gpio", 0,
					       &flags);
		if (gpio_is_valid(gpio)) {
			data->power_n.io = gpio;
			data->power_n.enable =
				(flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
			LOG("%s: WIFI,poweren_gpio = %d flags = %d.\n",
			    __func__, gpio, flags);
		} else {
			data->power_n.io = -1;
		}
		gpio = of_get_named_gpio_flags(node, "WIFI,vbat_gpio", 0,
					       &flags);
		if (gpio_is_valid(gpio)) {
			data->vbat_n.io = gpio;
			data->vbat_n.enable =
				(flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
			LOG("%s: WIFI,vbat_gpio = %d, flags = %d.\n",
			    __func__, gpio, flags);
		} else {
			data->vbat_n.io = -1;
		}
		gpio = of_get_named_gpio_flags(node, "WIFI,reset_gpio", 0,
					       &flags);
		if (gpio_is_valid(gpio)) {
			data->reset_n.io = gpio;
			data->reset_n.enable =
				(flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
			LOG("%s: WIFI,reset_gpio = %d, flags = %d.\n",
			    __func__, gpio, flags);
		} else {
			data->reset_n.io = -1;
		}
		gpio = of_get_named_gpio_flags(node, "WIFI,host_wake_irq", 0,
					       &flags);
		if (gpio_is_valid(gpio)) {
			data->wifi_int_b.io = gpio;
			data->wifi_int_b.enable = !flags;
			LOG("%s: WIFI,host_wake_irq = %d, flags = %d.\n",
			    __func__, gpio, flags);
		} else {
			data->wifi_int_b.io = -1;
		}

        modem_parse_dt(node);

	}

	data->ext_clk = devm_clk_get(dev, "clk_wifi");
	if (IS_ERR(data->ext_clk)) {
		LOG("%s: The ref_wifi_clk not found !\n", __func__);
	} else {
		of_property_read_u32(node, "ref-clock-frequency",
				     &ext_clk_value);
		if (ext_clk_value > 0) {
			ret = clk_set_rate(data->ext_clk, ext_clk_value);
			if (ret)
				LOG("%s: set ref clk error!\n", __func__);
		}

		ret = clk_prepare_enable(data->ext_clk);
		if (ret)
			LOG("%s: enable ref clk error!\n", __func__);

		/* WIFI clock (REF_CLKOUT) output enable.
		 * 1'b0: drive disable
		 * 1'b1: output enable
		 */
		if (of_machine_is_compatible("rockchip,rk3308"))
			regmap_write(data->grf, 0x0314, 0x00020002);
	}

	return 0;
}
#endif //CONFIG_OF

#if defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>

static void wlan_early_suspend(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

static void wlan_late_resume(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

struct early_suspend wlan_early_suspend {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 20;
	.suspend = wlan_early_suspend;
	.resume = wlan_late_resume;
}
#endif

static void
rfkill_wlan_early_suspend(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static void rfkill_wlan_later_resume(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static int rfkill_wlan_fb_event_notify(struct notifier_block *self,
				       unsigned long action, void *data)
{
	struct fb_event *event = data;
	int blank_mode = *((int *)event->data);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		rfkill_wlan_later_resume();
		break;
	case FB_BLANK_NORMAL:
		rfkill_wlan_early_suspend();
		break;
	default:
		rfkill_wlan_early_suspend();
		break;
	}

	return 0;
}

static struct notifier_block rfkill_wlan_fb_notifier = {
	.notifier_call = rfkill_wlan_fb_event_notify,
};

static ssize_t wifi_power_show(struct class *cls, struct class_attribute *attr, char *_buf)
{
	return sprintf(_buf, "%d\n", wifi_power_state);
}

static ssize_t wifi_power_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long poweren = 0;

	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *poweron;

	poweron = &mrfkill->pdata->power_n;
	if (kstrtol(_buf, 10, &poweren) < 0)
		return -EINVAL;

	LOG("%s: poweren = %ld\n", __func__, poweren);

	if (poweren > 0){
		//rockchip_wifi_power(1);
		if (gpio_is_valid(poweron->io)) {
			gpio_direction_output(poweron->io, poweron->enable);
			msleep(100);
		}
	}

	else {
		//rockchip_wifi_power(0);
		if (gpio_is_valid(poweron->io)) {
			gpio_direction_output(poweron->io, !(poweron->enable));
			msleep(100);
		}
	}

	return _count;
}

static CLASS_ATTR_RW(wifi_power);

static ssize_t wifi_bt_vbat_show(struct class *cls, struct class_attribute *attr, char *_buf)
{
	return sprintf(_buf, "%d\n", wifi_bt_vbat_state);
}

static ssize_t wifi_bt_vbat_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long vbat = 0;

	if (kstrtol(_buf, 10, &vbat) < 0)
		return -EINVAL;

	LOG("%s: vbat = %ld\n", __func__, vbat);

	if (vbat > 0)
		rfkill_set_wifi_bt_power(1);
	else
		rfkill_set_wifi_bt_power(0);

	return _count;
}

static CLASS_ATTR_RW(wifi_bt_vbat);

static ssize_t wifi_set_carddetect_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0)
		rockchip_wifi_set_carddetect(1);
	else
		rockchip_wifi_set_carddetect(0);

	return _count;
}

static CLASS_ATTR_WO(wifi_set_carddetect);

static ssize_t wifi_reset_show(struct class *cls, struct class_attribute *attr, char *_buf)
{
	return sprintf(_buf, "%d\n", wifi_reset_state);
}

static ssize_t wifi_reset_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long reseten = 0;

	struct rfkill_wlan_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *reset;

	reset = &mrfkill->pdata->reset_n;

	if (kstrtol(_buf, 10, &reseten) < 0)
		return -EINVAL;

	LOG("%s: reseten = %ld\n", __func__, reseten);

	if (reseten > 0) {
		//rockchip_wifi_power(1);
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, reset->enable);
			msleep(100);
		}
		wifi_reset_state = 1;
	} else {
		//rockchip_wifi_power(0);
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, !(reset->enable));
			msleep(100);
		}
		wifi_reset_state = 0;
	}

	return _count;
}

static CLASS_ATTR_RW(wifi_reset);

static ssize_t modem_fastboot_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.fastboot.io)) {
			gpio_direction_output(g_modem.fastboot.io, g_modem.fastboot.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.fastboot.io)) {
			gpio_direction_output(g_modem.fastboot.io, !(g_modem.fastboot.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_fastboot);

static ssize_t modem_ap_wake_dq_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.ap_wake_dq.io)) {
			gpio_direction_output(g_modem.ap_wake_dq.io, g_modem.ap_wake_dq.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.ap_wake_dq.io)) {
			gpio_direction_output(g_modem.ap_wake_dq.io, !(g_modem.ap_wake_dq.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_ap_wake_dq);

static ssize_t modem_reset_gpio_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.reset_gpio.io)) {
			gpio_direction_output(g_modem.reset_gpio.io, g_modem.reset_gpio.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.reset_gpio.io)) {
			gpio_direction_output(g_modem.reset_gpio.io, !(g_modem.reset_gpio.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_reset_gpio);

static ssize_t modem_poweren_gpio_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.poweren_gpio.io)) {
			gpio_direction_output(g_modem.poweren_gpio.io, g_modem.poweren_gpio.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.poweren_gpio.io)) {
			gpio_direction_output(g_modem.poweren_gpio.io, !(g_modem.poweren_gpio.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_poweren_gpio);

static ssize_t modem_usb_detect_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.usb_detect.io)) {
			gpio_direction_output(g_modem.usb_detect.io, g_modem.usb_detect.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.usb_detect.io)) {
			gpio_direction_output(g_modem.usb_detect.io, !(g_modem.usb_detect.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_usb_detect);

static ssize_t modem_usbsel_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.usbsel.io)) {
			gpio_direction_output(g_modem.usbsel.io, g_modem.usbsel.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.usbsel.io)) {
			gpio_direction_output(g_modem.usbsel.io, !(g_modem.usbsel.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_usbsel);

static ssize_t modem_usim_det_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.usim_det.io)) {
			gpio_direction_output(g_modem.usim_det.io, g_modem.usim_det.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.usim_det.io)) {
			gpio_direction_output(g_modem.usim_det.io, !(g_modem.usim_det.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_usim_det);

static ssize_t modem_pa_28v_en_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.pa_28v_en.io)) {
			gpio_direction_output(g_modem.pa_28v_en.io, g_modem.pa_28v_en.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.pa_28v_en.io)) {
			gpio_direction_output(g_modem.pa_28v_en.io, !(g_modem.pa_28v_en.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_pa_28v_en);

static ssize_t modem_dq_4v0_en_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.dq_4v0_en.io)) {
			gpio_direction_output(g_modem.dq_4v0_en.io, g_modem.dq_4v0_en.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.dq_4v0_en.io)) {
			gpio_direction_output(g_modem.dq_4v0_en.io, !(g_modem.dq_4v0_en.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(modem_dq_4v0_en);

static ssize_t wifi_pdn_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.wifi_pdn.io)) {
			gpio_direction_output(g_modem.wifi_pdn.io, g_modem.wifi_pdn.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.wifi_pdn.io)) {
			gpio_direction_output(g_modem.wifi_pdn.io, !(g_modem.wifi_pdn.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(wifi_pdn);

static ssize_t wifi_heat_store(struct class *cls, struct class_attribute *attr, const char *_buf, size_t _count)
{
	long val = 0;

	if (kstrtol(_buf, 10, &val) < 0)
		return -EINVAL;

	LOG("%s: val = %ld\n", __func__, val);

	if (val > 0){
		if (gpio_is_valid(g_modem.wifi_heat.io)) {
			gpio_direction_output(g_modem.wifi_heat.io, g_modem.wifi_heat.enable);
		};
	} else {
		if (gpio_is_valid(g_modem.wifi_heat.io)) {
			gpio_direction_output(g_modem.wifi_heat.io, !(g_modem.wifi_heat.enable));
		};
	}

	return _count;
}
static CLASS_ATTR_WO(wifi_heat);



static struct attribute *rkwifi_power_attrs[] = {
	&class_attr_wifi_power.attr,
	&class_attr_wifi_bt_vbat.attr,
	&class_attr_wifi_set_carddetect.attr,
	&class_attr_wifi_reset.attr,
	&class_attr_wifi_pdn.attr,
	&class_attr_wifi_heat.attr,
	&class_attr_modem_fastboot.attr,
	&class_attr_modem_ap_wake_dq.attr,
	&class_attr_modem_reset_gpio.attr,
	&class_attr_modem_poweren_gpio.attr,
	&class_attr_modem_usb_detect.attr,
	&class_attr_modem_usbsel.attr,
	&class_attr_modem_usim_det.attr,
	&class_attr_modem_pa_28v_en.attr,
	&class_attr_modem_dq_4v0_en.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rkwifi_power);

/** Device model classes */
static struct class rkwifi_power = {
	.name        = "rkwifi",
	.class_groups = rkwifi_power_groups,
};

static int rfkill_wlan_probe(struct platform_device *pdev)
{
	struct rfkill_wlan_data *rfkill;
	struct rksdmmc_gpio_wifi_moudle *pdata = pdev->dev.platform_data;

    //struct rksdmmc_gpio_modem_moudle *modem;

	int ret = -1;

	LOG("Enter %s\n", __func__);

	class_register(&rkwifi_power);

	if (!pdata) {
#ifdef CONFIG_OF
		pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		ret = wlan_platdata_parse_dt(&pdev->dev, pdata);
		if (ret < 0) {
#endif
			LOG("%s: No platform data specified\n", __func__);
			return ret;
#ifdef CONFIG_OF
		}
#endif
	}

	rfkill = kzalloc(sizeof(*rfkill), GFP_KERNEL);
	if (!rfkill)
		goto rfkill_alloc_fail;

	rfkill->pdata = pdata;
	g_rfkill = rfkill;

//	modem = kzalloc(sizeof(*modem), GFP_KERNEL);
//	if (!modem)
//		goto modem_alloc_fail;

//	g_modem = modem;

	LOG("%s: init gpio\n", __func__);

	if (!pdata->mregulator.power_ctrl_by_pmu) {
		ret = rfkill_rk_setup_gpio(&pdata->vbat_n, wlan_name,
					   "wlan_vbat");
		if (ret)
			goto fail_alloc;

		ret = rfkill_rk_setup_gpio(&pdata->reset_n, wlan_name,
					   "wlan_reset");
		if (ret)
			goto fail_alloc;
	}

	wake_lock_init(&rfkill->wlan_irq_wl, WAKE_LOCK_SUSPEND,
		       "rfkill_wlan_wake");

	rfkill_set_wifi_bt_power(1);

#ifdef CONFIG_SDIO_KEEPALIVE
	if (gpio_is_valid(pdata->power_n.io) &&
		gpio_direction_output(pdata->power_n.io, pdata->power_n.enable);
#endif


	if (pdata->wifi_power_remain)
		rockchip_wifi_power(1);

#if BCM_STATIC_MEMORY_SUPPORT
	rockchip_init_wifi_mem();
#endif

#if defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(wlan_early_suspend);
#endif

	fb_register_client(&rfkill_wlan_fb_notifier);

	LOG("Exit %s\n", __func__);

	return 0;

//modem_alloc_fail:
//	g_modem = NULL;

fail_alloc:
	kfree(rfkill);
rfkill_alloc_fail:
	kfree(pdata);

	g_rfkill = NULL;

	return ret;
}

static int rfkill_wlan_remove(struct platform_device *pdev)
{
	struct rfkill_wlan_data *rfkill = platform_get_drvdata(pdev);

	LOG("Enter %s\n", __func__);

	wake_lock_destroy(&rfkill->wlan_irq_wl);

	fb_unregister_client(&rfkill_wlan_fb_notifier);

	if (gpio_is_valid(rfkill->pdata->power_n.io))
		gpio_free(rfkill->pdata->power_n.io);

	if (gpio_is_valid(rfkill->pdata->reset_n.io))
		gpio_free(rfkill->pdata->reset_n.io);

	kfree(rfkill);
	g_rfkill = NULL;

	return 0;
}

static void rfkill_wlan_shutdown(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);

	rockchip_wifi_power(0);
	rfkill_set_wifi_bt_power(0);
}

static int rfkill_wlan_suspend(struct platform_device *pdev, pm_message_t state)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

static int rfkill_wlan_resume(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id wlan_platdata_of_match[] = {
	{ .compatible = "wlan-platdata" },
	{}
};
MODULE_DEVICE_TABLE(of, wlan_platdata_of_match);
#endif //CONFIG_OF

static struct platform_driver rfkill_wlan_driver = {
	.probe = rfkill_wlan_probe,
	.remove = rfkill_wlan_remove,
	.shutdown = rfkill_wlan_shutdown,
    .suspend = rfkill_wlan_suspend,
    .resume = rfkill_wlan_resume,
	.driver = {
		.name = "wlan-platdata",
		.owner = THIS_MODULE,
        .of_match_table = of_match_ptr(wlan_platdata_of_match),
	},
};

int __init rfkill_wlan_init(void)
{
	LOG("Enter %s\n", __func__);
	return platform_driver_register(&rfkill_wlan_driver);
}

void __exit rfkill_wlan_exit(void)
{
	LOG("Enter %s\n", __func__);
	platform_driver_unregister(&rfkill_wlan_driver);
}

MODULE_DESCRIPTION("rock-chips rfkill for wifi v0.1");
MODULE_AUTHOR("gwl@rock-chips.com");
MODULE_LICENSE("GPL");
