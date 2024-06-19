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
#include <linux/rfkill-gps.h>
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
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif
#include <linux/soc/rockchip/rk_vendor_storage.h>
#include <linux/device.h>

#if 0
#define DBG(x...) pr_info("[WLAN_RFKILL]: " x)
#else
#define DBG(x...)
#endif

#define LOG(x...) pr_info("[WLAN_RFKILL]: " x)

struct rfkill_gps_data {
	struct rksdmmc_gpio_gps_moudle *pdata;
};

static struct rfkill_gps_data *g_rfkill = NULL;

static int gps_stdb_state;
static int gps_reset_state = 0;

static const char gps_name[] = "rkgps";

int rfkill_set_gps_power(int on)
{
	struct rfkill_gps_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *poweren;

	LOG("%s: %d\n", __func__, on);

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	poweren = &mrfkill->pdata->poweron;
	if (on) {
		if (gpio_is_valid(poweren->io))
			gpio_direction_output(poweren->io, poweren->enable);
	} else {
		if (gpio_is_valid(poweren->io))
			gpio_direction_output(poweren->io, !(poweren->enable));
	}

	return 0;
}

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
static int gps_platdata_parse_dt(struct device *dev,
				 struct rksdmmc_gpio_gps_moudle *data)
{
	struct device_node *node = dev->of_node;
	int gpio;
	enum of_gpio_flags flags;

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

	gpio = of_get_named_gpio_flags(node, "GPS,stdb_gpio", 0, &flags);
	if (gpio_is_valid(gpio)) {
		data->stdb.io = gpio;
		data->stdb.enable = (flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
		LOG("%s: GPS,stdb_gpio = %d flags = %d.\n", __func__, gpio,
		    flags);
	} else {
		data->stdb.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "GPS,rst_gpio", 0, &flags);
	if (gpio_is_valid(gpio)) {
		data->reset_n.io = gpio;
		data->reset_n.enable = (flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
		LOG("%s: GPS,rst_gpio = %d, flags = %d.\n", __func__, gpio,
		    flags);
	} else {
		data->reset_n.io = -1;
	}

	return 0;
}
#endif //CONFIG_OF

#if defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>

static void gps_early_suspend(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

static void gps_late_resume(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

struct early_suspend gps_early_suspend {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 20;
	.suspend = gps_early_suspend;
	.resume = gps_late_resume;
}
#endif

static void
rfkill_gps_early_suspend(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static void rfkill_gps_later_resume(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static int rfkill_gps_fb_event_notify(struct notifier_block *self,
				      unsigned long action, void *data)
{
	struct fb_event *event = data;
	int blank_mode = *((int *)event->data);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		rfkill_gps_later_resume();
		break;
	case FB_BLANK_NORMAL:
		rfkill_gps_early_suspend();
		break;
	default:
		rfkill_gps_early_suspend();
		break;
	}

	return 0;
}

static struct notifier_block rfkill_gps_fb_notifier = {
	.notifier_call = rfkill_gps_fb_event_notify,
};

static ssize_t gps_stdb_show(struct class *cls, struct class_attribute *attr,
			     char *_buf)
{
	return sprintf(_buf, "%d\n", gps_stdb_state);
}

static ssize_t gps_stdb_store(struct class *cls, struct class_attribute *attr,
			      const char *_buf, size_t _count)
{
	long stdb = 0;

	struct rfkill_gps_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *stdb_gpio;

	stdb_gpio = &mrfkill->pdata->stdb;
	if (kstrtol(_buf, 10, &stdb) < 0)
		return -EINVAL;

	LOG("%s: stdb = %ld\n", __func__, stdb);

	if (stdb > 0) {
		if (gpio_is_valid(stdb_gpio->io)) {
			gpio_direction_output(stdb_gpio->io, stdb_gpio->enable);
			msleep(100);
		}
		gps_stdb_state = 1;
	} else {
		if (gpio_is_valid(stdb_gpio->io)) {
			gpio_direction_output(stdb_gpio->io,
					      !(stdb_gpio->enable));
			msleep(100);
		}
		gps_stdb_state = 0;
	}

	return _count;
}

static CLASS_ATTR_RW(gps_stdb);

static ssize_t gps_reset_show(struct class *cls, struct class_attribute *attr,
			      char *_buf)
{
	return sprintf(_buf, "%d\n", gps_reset_state);
}

static ssize_t gps_reset_store(struct class *cls, struct class_attribute *attr,
			       const char *_buf, size_t _count)
{
	long reseten = 0;

	struct rfkill_gps_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *reset;

	reset = &mrfkill->pdata->reset_n;

	if (kstrtol(_buf, 10, &reseten) < 0)
		return -EINVAL;

	LOG("%s: reseten = %ld\n", __func__, reseten);

	if (reseten > 0) {
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, reset->enable);
			msleep(100);
		}
		gps_reset_state = 1;
	} else {
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, !(reset->enable));
			msleep(100);
		}
		gps_reset_state = 0;
	}

	return _count;
}

static CLASS_ATTR_RW(gps_reset);

static struct attribute *rkgps_power_attrs[] = {
	&class_attr_gps_stdb.attr,
	&class_attr_gps_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rkgps_power);

/** Device model classes */
static struct class rkgps_power = {
	.name = "rkgps",
	.class_groups = rkgps_power_groups,
};

static int rfkill_gps_probe(struct platform_device *pdev)
{
	struct rfkill_gps_data *rfkill;
	struct rksdmmc_gpio_gps_moudle *pdata = pdev->dev.platform_data;
	int ret = -1;

	LOG("Enter %s\n", __func__);

	class_register(&rkgps_power);

	if (!pdata) {
#ifdef CONFIG_OF
		pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		ret = gps_platdata_parse_dt(&pdev->dev, pdata);
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

	LOG("%s: init gpio\n", __func__);

	ret = rfkill_rk_setup_gpio(&pdata->stdb, gps_name, "gps_stdb");
	if (ret)
		goto fail_alloc;

	ret = rfkill_rk_setup_gpio(&pdata->reset_n, gps_name, "gps_reset");
	if (ret)
		goto fail_alloc;

	rfkill_set_gps_power(1);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(gps_early_suspend);
#endif

	fb_register_client(&rfkill_gps_fb_notifier);

	LOG("Exit %s\n", __func__);

	return 0;

fail_alloc:
	kfree(rfkill);
rfkill_alloc_fail:
	kfree(pdata);

	g_rfkill = NULL;

	return ret;
}

static int rfkill_gps_remove(struct platform_device *pdev)
{
	struct rfkill_gps_data *rfkill = platform_get_drvdata(pdev);

	LOG("Enter %s\n", __func__);

	fb_unregister_client(&rfkill_gps_fb_notifier);

	if (gpio_is_valid(rfkill->pdata->stdb.io))
		gpio_free(rfkill->pdata->stdb.io);

	if (gpio_is_valid(rfkill->pdata->reset_n.io))
		gpio_free(rfkill->pdata->reset_n.io);

	kfree(rfkill);
	g_rfkill = NULL;

	return 0;
}

static void rfkill_gps_shutdown(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);

	rfkill_set_gps_power(0);
}

static int rfkill_gps_suspend(struct platform_device *pdev, pm_message_t state)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

static int rfkill_gps_resume(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id gps_platdata_of_match[] = {
	{ .compatible = "gps-platdata" },
	{}
};
MODULE_DEVICE_TABLE(of, gps_platdata_of_match);
#endif //CONFIG_OF

static struct platform_driver rfkill_gps_driver = {
	.probe = rfkill_gps_probe,
	.remove = rfkill_gps_remove,
	.shutdown = rfkill_gps_shutdown,
    .suspend = rfkill_gps_suspend,
    .resume = rfkill_gps_resume,
	.driver = {
		.name = "gps-platdata",
		.owner = THIS_MODULE,
        .of_match_table = of_match_ptr(gps_platdata_of_match),
	},
};

static int __init rfkill_gps_rk_init(void)
{
	LOG("Enter %s\n", __func__);
	return platform_driver_register(&rfkill_gps_driver);
}

static void __exit rfkill_gps_rk_exit(void)
{
	LOG("Enter %s\n", __func__);
	platform_driver_unregister(&rfkill_gps_driver);
}

module_init(rfkill_gps_rk_init);
module_exit(rfkill_gps_rk_exit);

MODULE_DESCRIPTION("rock-chips rfkill for gps");
MODULE_AUTHOR("newayer <cn_gwq@sina.com>");
MODULE_LICENSE("GPL");
