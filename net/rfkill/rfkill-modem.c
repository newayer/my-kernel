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
#include <linux/rfkill-modem.h>
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

struct rfkill_modem_data {
	struct rksdmmc_gpio_modem_moudle *pdata;
};

static struct rfkill_modem_data *g_rfkill = NULL;

static int modem_poweren_state;
static int modem_on_state = 0;

static const char modem_name[] = "rkmodem";

int rfkill_set_modem_power(int on)
{
	struct rfkill_modem_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *poweren;

	LOG("%s: %d\n", __func__, on);

	if (!mrfkill) {
		LOG("%s: rfkill-wlan driver has not Successful initialized\n",
		    __func__);
		return -1;
	}

	poweren = &mrfkill->pdata->poweren_n;
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
static int modem_platdata_parse_dt(struct device *dev,
				   struct rksdmmc_gpio_modem_moudle *data)
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

	gpio = of_get_named_gpio_flags(node, "MODEM,poweren_gpio", 0, &flags);
	if (gpio_is_valid(gpio)) {
		data->poweren_n.io = gpio;
		data->poweren_n.enable = (flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
		LOG("%s: modem,stdb_gpio = %d flags = %d.\n", __func__, gpio,
		    flags);
	} else {
		data->poweren_n.io = -1;
	}

	gpio = of_get_named_gpio_flags(node, "MODEM,on_gpio", 0, &flags);
	if (gpio_is_valid(gpio)) {
		data->on.io = gpio;
		data->on.enable = (flags == GPIO_ACTIVE_HIGH) ? 1 : 0;
		LOG("%s: modem,rst_gpio = %d, flags = %d.\n", __func__, gpio,
		    flags);
	} else {
		data->on.io = -1;
	}

	return 0;
}
#endif //CONFIG_OF

#if defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>

static void modem_early_suspend(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

static void modem_late_resume(struct early_suspend *h)
{
	LOG("%s :enter\n", __func__);

	return;
}

struct early_suspend modem_early_suspend {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 20;
	.suspend = modem_early_suspend;
	.resume = modem_late_resume;
}
#endif

static void
rfkill_modem_early_suspend(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static void rfkill_modem_later_resume(void)
{
	//LOG("%s :enter\n", __func__);

	return;
}

static int rfkill_modem_fb_event_notify(struct notifier_block *self,
					unsigned long action, void *data)
{
	struct fb_event *event = data;
	int blank_mode = *((int *)event->data);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		rfkill_modem_later_resume();
		break;
	case FB_BLANK_NORMAL:
		rfkill_modem_early_suspend();
		break;
	default:
		rfkill_modem_early_suspend();
		break;
	}

	return 0;
}

static struct notifier_block rfkill_modem_fb_notifier = {
	.notifier_call = rfkill_modem_fb_event_notify,
};

static ssize_t modem_poweren_show(struct class *cls,
				  struct class_attribute *attr, char *_buf)
{
	return sprintf(_buf, "%d\n", modem_poweren_state);
}

static ssize_t modem_poweren_store(struct class *cls,
				   struct class_attribute *attr,
				   const char *_buf, size_t _count)
{
	long poweren = 0;

	struct rfkill_modem_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *poweren_gpio;

	poweren_gpio = &mrfkill->pdata->poweren_n;
	if (kstrtol(_buf, 10, &poweren) < 0)
		return -EINVAL;

	LOG("%s: poweren = %ld\n", __func__, poweren);

	if (poweren > 0) {
		if (gpio_is_valid(poweren_gpio->io)) {
			gpio_direction_output(poweren_gpio->io,
					      poweren_gpio->enable);
			msleep(100);
		}
		modem_poweren_state = 1;
	} else {
		if (gpio_is_valid(poweren_gpio->io)) {
			gpio_direction_output(poweren_gpio->io,
					      !(poweren_gpio->enable));
			msleep(100);
		}
		modem_poweren_state = 0;
	}

	return _count;
}

static CLASS_ATTR_RW(modem_poweren);

static ssize_t modem_on_show(struct class *cls, struct class_attribute *attr,
			     char *_buf)
{
	return sprintf(_buf, "%d\n", modem_on_state);
}

static ssize_t modem_on_store(struct class *cls, struct class_attribute *attr,
			      const char *_buf, size_t _count)
{
	long on = 0;

	struct rfkill_modem_data *mrfkill = g_rfkill;
	struct rksdmmc_gpio *reset;

	reset = &mrfkill->pdata->on;

	if (kstrtol(_buf, 10, &on) < 0)
		return -EINVAL;

	LOG("%s: reseten = %ld\n", __func__, on);

	if (on > 0) {
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, reset->enable);
			msleep(100);
		}
		modem_on_state = 1;
	} else {
		if (gpio_is_valid(reset->io)) {
			gpio_direction_output(reset->io, !(reset->enable));
			msleep(100);
		}
		modem_on_state = 0;
	}

	return _count;
}

static CLASS_ATTR_RW(modem_on);

static struct attribute *rkmodem_power_attrs[] = {
	&class_attr_modem_poweren.attr,
	&class_attr_modem_on.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rkmodem_power);

/** Device model classes */
static struct class rkmodem_power = {
	.name = "rkmodem",
	.class_groups = rkmodem_power_groups,
};

static int rfkill_modem_probe(struct platform_device *pdev)
{
	struct rfkill_modem_data *rfkill;
	struct rksdmmc_gpio_modem_moudle *pdata = pdev->dev.platform_data;
	int ret = -1;

	LOG("Enter %s\n", __func__);

	class_register(&rkmodem_power);

	if (!pdata) {
#ifdef CONFIG_OF
		pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		ret = modem_platdata_parse_dt(&pdev->dev, pdata);
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

	ret = rfkill_rk_setup_gpio(&pdata->poweren_n, modem_name,
				   "modem_poweren");
	if (ret)
		goto fail_alloc;

	ret = rfkill_rk_setup_gpio(&pdata->on, modem_name, "modem_on");
	if (ret)
		goto fail_alloc;

	rfkill_set_modem_power(1);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(modem_early_suspend);
#endif

	fb_register_client(&rfkill_modem_fb_notifier);

	LOG("Exit %s\n", __func__);

	return 0;

fail_alloc:
	kfree(rfkill);
rfkill_alloc_fail:
	kfree(pdata);

	g_rfkill = NULL;

	return ret;
}

static int rfkill_modem_remove(struct platform_device *pdev)
{
	struct rfkill_modem_data *rfkill = platform_get_drvdata(pdev);

	LOG("Enter %s\n", __func__);

	fb_unregister_client(&rfkill_modem_fb_notifier);

	if (gpio_is_valid(rfkill->pdata->poweren_n.io))
		gpio_free(rfkill->pdata->poweren_n.io);

	if (gpio_is_valid(rfkill->pdata->on.io))
		gpio_free(rfkill->pdata->on.io);

	kfree(rfkill);
	g_rfkill = NULL;

	return 0;
}

static void rfkill_modem_shutdown(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);

	rfkill_set_modem_power(0);
}

static int rfkill_modem_suspend(struct platform_device *pdev,
				pm_message_t state)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

static int rfkill_modem_resume(struct platform_device *pdev)
{
	LOG("Enter %s\n", __func__);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id modem_platdata_of_match[] = {
	{ .compatible = "modem-platdata" },
	{}
};
MODULE_DEVICE_TABLE(of, modem_platdata_of_match);
#endif //CONFIG_OF

static struct platform_driver rfkill_modem_driver = {
	.probe = rfkill_modem_probe,
	.remove = rfkill_modem_remove,
	.shutdown = rfkill_modem_shutdown,
    .suspend = rfkill_modem_suspend,
    .resume = rfkill_modem_resume,
	.driver = {
		.name = "modem-platdata",
		.owner = THIS_MODULE,
        .of_match_table = of_match_ptr(modem_platdata_of_match),
	},
};

static int __init rfkill_modem_rk_init(void)
{
	LOG("Enter %s\n", __func__);
	return platform_driver_register(&rfkill_modem_driver);
}

static void __exit rfkill_modem_rk_exit(void)
{
	LOG("Enter %s\n", __func__);
	platform_driver_unregister(&rfkill_modem_driver);
}

module_init(rfkill_modem_rk_init);
module_exit(rfkill_modem_rk_exit);

MODULE_DESCRIPTION("rock-chips rfkill for modem");
MODULE_AUTHOR("newayer <cn_gwq@sina.com>");
MODULE_LICENSE("GPL");
