// drivers/misc/rockchip-hw-detector.c
/*
 * Rockchip HW Detector Driver
 * Supports hardware version detection and battery voltage monitoring
 *
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio/consumer.h>
#include <linux/math64.h>

#define DRIVER_NAME         "rockchip-hw-detector"
#define DRIVER_VERSION      "1.0.0"

#define MAX_SAMPLE_COUNT    10
#define DEFAULT_SAMPLE_COUNT 5
#define MAX_CHANNELS        2
#define HW_VERSION_ENTRIES  10

struct hw_version_threshold {
    u32 min_mv;
    u32 max_mv;
    u32 version;
};

struct vbat_calibration {
    u32 adc_mv;
    u32 actual_mv;
};

struct hw_detector_data {
    struct device *dev;
    struct iio_channel *adc_channels[MAX_CHANNELS];

    // 配置参数
    u32 hw_version_channel_idx;
    u32 vbat_channel_idx;
    u32 sample_count;
    u32 sample_interval_ms;
    u32 debounce_count;
    u32 vbat_divider_ratio; // 分压比 * 1000
    u32 vbat_ref_voltage;   // 参考电压 mV

    // 阈值和校准表
    struct hw_version_threshold hw_thresholds[HW_VERSION_ENTRIES];
    u32 hw_threshold_count;

    struct vbat_calibration vbat_cal[2];

    // 当前值
    u32 hw_version;
    u32 hw_version_raw_mv;
    u32 vbat_mv;
    u32 vbat_raw_mv;

    // 锁
    struct mutex lock;

    // 电源管理
    struct regulator *vref_supply;
    struct regulator *power_supply;

    // 工作队列
    struct delayed_work monitor_work;
    u32 monitor_interval;

    // 调试信息
    char hw_version_str[32];
};

// Sysfs属性
static ssize_t hw_version_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    snprintf(data->hw_version_str, sizeof(data->hw_version_str),
             "V%d.%d (raw: %dmV)",
             data->hw_version >> 4, data->hw_version & 0xF,
             data->hw_version_raw_mv);
    mutex_unlock(&data->lock);

    return sysfs_emit(buf, "%s\n", data->hw_version_str);
}

static ssize_t hw_version_raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    int ret = sysfs_emit(buf, "%d\n", data->hw_version_raw_mv);
    mutex_unlock(&data->lock);

    return ret;
}

static ssize_t vbat_show(struct device *dev,
                        struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    int ret = sysfs_emit(buf, "%d\n", data->vbat_mv);
    mutex_unlock(&data->lock);

    return ret;
}

static ssize_t vbat_raw_show(struct device *dev,
                            struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    int ret = sysfs_emit(buf, "%d\n", data->vbat_raw_mv);
    mutex_unlock(&data->lock);

    return ret;
}

static ssize_t vbat_voltage_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);
    u32 vbat_mv;
    int integer, fractional;

    mutex_lock(&data->lock);
    vbat_mv = data->vbat_mv;
    mutex_unlock(&data->lock);

    // 避免浮点运算：将毫伏转换为伏特（例如：7543mV -> 7.543V）
    // 使用整数运算计算整数部分和小数部分
    integer = vbat_mv / 1000;
    fractional = (vbat_mv % 1000); // 毫伏的小数部分

    // 显示为 "整数.小数" 格式，小数部分保留3位
    return sysfs_emit(buf, "%d.%03d\n", integer, fractional);
}

static ssize_t sample_count_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);
    u32 val;

    if (kstrtou32(buf, 0, &val))
        return -EINVAL;

    if (val < 1 || val > MAX_SAMPLE_COUNT)
        return -EINVAL;

    mutex_lock(&data->lock);
    data->sample_count = val;
    dev_info(dev, "Sample count set to %u\n", val);
    mutex_unlock(&data->lock);

    return count;
}

static ssize_t sample_count_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->lock);
    int ret = sysfs_emit(buf, "%u\n", data->sample_count);
    mutex_unlock(&data->lock);

    return ret;
}

static DEVICE_ATTR_RO(hw_version);
static DEVICE_ATTR_RO(hw_version_raw);
static DEVICE_ATTR_RO(vbat);
static DEVICE_ATTR_RO(vbat_raw);
static DEVICE_ATTR_RO(vbat_voltage);
static DEVICE_ATTR_RW(sample_count);

static struct attribute *hw_detector_attrs[] = {
    &dev_attr_hw_version.attr,
    &dev_attr_hw_version_raw.attr,
    &dev_attr_vbat.attr,
    &dev_attr_vbat_raw.attr,
    &dev_attr_vbat_voltage.attr,
    &dev_attr_sample_count.attr,
    NULL,
};

ATTRIBUTE_GROUPS(hw_detector);

// 读取ADC值（多次采样平均）
static int read_adc_channel(struct hw_detector_data *data,
                           u32 channel_idx, u32 *value_mv)
{
    struct iio_channel *channel;
    int val, ret;
    u32 sum = 0;
    u32 i;

    if (channel_idx >= MAX_CHANNELS || !data->adc_channels[channel_idx])
        return -EINVAL;

    channel = data->adc_channels[channel_idx];

    // 多次采样取平均
    for (i = 0; i < data->sample_count; i++) {
        ret = iio_read_channel_processed(channel, &val);
        if (ret < 0) {
            dev_err(data->dev, "Failed to read ADC channel %u: %d\n",
                   channel_idx, ret);
            return ret;
        }

        sum += val;

        if (i < data->sample_count - 1 && data->sample_interval_ms > 0)
            msleep(data->sample_interval_ms);
    }

    *value_mv = sum / data->sample_count;
    return 0;
}

// 检测硬件版本
static u32 detect_hardware_version(struct hw_detector_data *data, u32 adc_mv)
{
    u32 i;

    for (i = 0; i < data->hw_threshold_count; i++) {
        struct hw_version_threshold *th = &data->hw_thresholds[i];

        if (adc_mv >= th->min_mv && adc_mv <= th->max_mv) {
            dev_dbg(data->dev, "ADC %dmV -> HW version %u\n",
                   adc_mv, th->version);
            return th->version;
        }
    }

    dev_warn(data->dev, "Unknown ADC value %dmV, using default version 0\n",
            adc_mv);
    return 0;
}

// 计算电池电压 - 修复64位除法问题
static u32 calculate_vbat_voltage(struct hw_detector_data *data, u32 adc_mv)
{
    // 方法1：使用分压比计算
    if (data->vbat_divider_ratio > 0) {
        // 使用 do_div 宏进行64位除法
        u64 temp = (u64)adc_mv * data->vbat_divider_ratio;
        do_div(temp, 1000);
        return (u32)temp;
    }

    // 方法2：使用校准表进行线性插值
    if (data->vbat_cal[0].adc_mv > 0 && data->vbat_cal[1].adc_mv > 0) {
        u32 adc1 = data->vbat_cal[0].adc_mv;
        u32 vbat1 = data->vbat_cal[0].actual_mv;
        u32 adc2 = data->vbat_cal[1].adc_mv;
        u32 vbat2 = data->vbat_cal[1].actual_mv;

        // 边界检查
        if (adc_mv <= adc1) return vbat1;
        if (adc_mv >= adc2) return vbat2;
        if (adc2 == adc1) return vbat1; // 避免除以0

        // 线性插值，使用 do_div 避免64位除法链接错误
        s32 delta_adc = adc_mv - adc1;
        u32 delta_vbat = vbat2 - vbat1;
        u32 delta_adc_range = adc2 - adc1;

        u64 temp = (u64)delta_adc * delta_vbat;
        do_div(temp, delta_adc_range);

        return vbat1 + (u32)temp;
    }

    // 方法3：简单比例计算（假设参考电压1.8V）
    // 使用 do_div 进行64位除法
    u64 temp = (u64)adc_mv * 8400;
    do_div(temp, 1800);
    return (u32)temp;
}

// 更新所有检测值
static int update_detector_values(struct hw_detector_data *data)
{
    int ret;
    u32 hw_adc_mv, vbat_adc_mv;

    mutex_lock(&data->lock);

    // 读取硬件版本ADC
    ret = read_adc_channel(data, data->hw_version_channel_idx, &hw_adc_mv);
    if (ret) {
        mutex_unlock(&data->lock);
        return ret;
    }

    // 读取电池电压ADC
    ret = read_adc_channel(data, data->vbat_channel_idx, &vbat_adc_mv);
    if (ret) {
        mutex_unlock(&data->lock);
        return ret;
    }

    // 更新硬件版本
    data->hw_version_raw_mv = hw_adc_mv;
    data->hw_version = detect_hardware_version(data, hw_adc_mv);

    // 更新电池电压
    data->vbat_raw_mv = vbat_adc_mv;
    data->vbat_mv = calculate_vbat_voltage(data, vbat_adc_mv);

    dev_dbg(data->dev, "Updated: HW=%dmV(ver:%u), VBAT=%dmV(raw:%dmV)\n",
           hw_adc_mv, data->hw_version, data->vbat_mv, vbat_adc_mv);

    mutex_unlock(&data->lock);
    return 0;
}

// 工作队列函数
static void monitor_work_func(struct work_struct *work)
{
    struct hw_detector_data *data = container_of(
        work, struct hw_detector_data, monitor_work.work);

    update_detector_values(data);

    // 重新调度
    if (data->monitor_interval > 0) {
        schedule_delayed_work(&data->monitor_work,
                            msecs_to_jiffies(data->monitor_interval));
    }
}

// 从设备树解析配置
static int parse_dt(struct hw_detector_data *data)
{
    struct device *dev = data->dev;
    struct device_node *np = dev->of_node;
    int ret, i;
    u32 threshold[3];
    u32 calibration[2];

    // 获取ADC通道索引
    ret = of_property_read_u32(np, "hw-version-adc-channel",
                              &data->hw_version_channel_idx);
    if (ret) {
        dev_warn(dev, "hw-version-adc-channel not specified, using 0\n");
        data->hw_version_channel_idx = 0;
    }

    ret = of_property_read_u32(np, "vbat-adc-channel",
                              &data->vbat_channel_idx);
    if (ret) {
        dev_warn(dev, "vbat-adc-channel not specified, using 1\n");
        data->vbat_channel_idx = 1;
    }

    // 解析硬件版本阈值
    data->hw_threshold_count = 0;
    i = 0;
    while (!of_property_read_u32_index(np, "hw-version-thresholds",
                                      i * 3, &threshold[0])) {
        if (i >= HW_VERSION_ENTRIES) {
            dev_warn(dev, "Too many threshold entries, truncating\n");
            break;
        }

        of_property_read_u32_index(np, "hw-version-thresholds",
                                  i * 3 + 1, &threshold[1]);
        of_property_read_u32_index(np, "hw-version-thresholds",
                                  i * 3 + 2, &threshold[2]);

        data->hw_thresholds[i].min_mv = threshold[0];
        data->hw_thresholds[i].max_mv = threshold[1];
        data->hw_thresholds[i].version = threshold[2];

        dev_info(dev, "HW threshold[%d]: %u-%umV -> version %u\n",
                i, threshold[0], threshold[1], threshold[2]);

        data->hw_threshold_count++;
        i++;
    }

    if (data->hw_threshold_count == 0) {
        // 设置默认阈值（根据你的需求）
        data->hw_thresholds[0] = (struct hw_version_threshold){0, 200, 0};     // 0-200mV: 版本1（单下）
        data->hw_thresholds[1] = (struct hw_version_threshold){1600, 2000, 1}; // 1.6-2.0V: 版本2（单上）
        data->hw_thresholds[2] = (struct hw_version_threshold){800, 1000, 2};  // 0.8-1.0V: 版本3（上下）
        data->hw_thresholds[3] = (struct hw_version_threshold){250, 350, 3};   // 0.25-0.35V: 版本4（上下下）
        data->hw_threshold_count = 4;
    }

    // 解析电压检测参数
    if (!of_property_read_u32(np, "vbat-divider-ratio",
                             &data->vbat_divider_ratio)) {
        dev_info(dev, "VBAT divider ratio: %u/1000\n", data->vbat_divider_ratio);
    }

    // 解析校准表
    if (!of_property_read_u32_array(np, "vbat-calibration",
                                   calibration, 4)) {
        data->vbat_cal[0].adc_mv = calibration[0];
        data->vbat_cal[0].actual_mv = calibration[1];
        data->vbat_cal[1].adc_mv = calibration[2];
        data->vbat_cal[1].actual_mv = calibration[3];
        dev_info(dev, "VBAT calibration: %dmV->%dmV, %dmV->%dmV\n",
                calibration[0], calibration[1], calibration[2], calibration[3]);
    } else {
        // 设置默认校准值
        data->vbat_cal[0].adc_mv = 1422;  // 1.422V
        data->vbat_cal[0].actual_mv = 7000; // 7.0V
        data->vbat_cal[1].adc_mv = 1707;  // 1.707V
        data->vbat_cal[1].actual_mv = 8400; // 8.4V
    }

    // 采样配置
    of_property_read_u32(np, "sample-count", &data->sample_count);
    if (data->sample_count < 1 || data->sample_count > MAX_SAMPLE_COUNT)
        data->sample_count = DEFAULT_SAMPLE_COUNT;

    of_property_read_u32(np, "sample-interval-ms", &data->sample_interval_ms);
    of_property_read_u32(np, "debounce-count", &data->debounce_count);

    // 参考电压
    of_property_read_u32(np, "vbat-ref-voltage", &data->vbat_ref_voltage);
    if (!data->vbat_ref_voltage)
        data->vbat_ref_voltage = 1800; // 默认1.8V

    // 监控间隔（默认为0，不自动监控）
    data->monitor_interval = 0;
    of_property_read_u32(np, "poll-interval-ms", &data->monitor_interval);

    return 0;
}

static int rockchip_hw_detector_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct hw_detector_data *data;
    int ret, i;

    dev_info(dev, "Rockchip HW Detector Driver Probe\n");

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->dev = dev;
    mutex_init(&data->lock);

    // 解析设备树
    ret = parse_dt(data);
    if (ret) {
        dev_err(dev, "Failed to parse device tree: %d\n", ret);
        return ret;
    }

    // 获取ADC通道
    for (i = 0; i < MAX_CHANNELS; i++) {
        char channel_name[20];
        snprintf(channel_name, sizeof(channel_name), "channel%d", i);

        data->adc_channels[i] = devm_iio_channel_get(dev, channel_name);
        if (IS_ERR(data->adc_channels[i])) {
            // 尝试使用命名通道
            if (i == data->hw_version_channel_idx)
                data->adc_channels[i] = devm_iio_channel_get(dev, "hw_version");
            else if (i == data->vbat_channel_idx)
                data->adc_channels[i] = devm_iio_channel_get(dev, "vbat");

            if (IS_ERR(data->adc_channels[i])) {
                dev_err(dev, "Failed to get ADC channel %d: %ld\n",
                       i, PTR_ERR(data->adc_channels[i]));
                return PTR_ERR(data->adc_channels[i]);
            }
        }
    }

    // 获取电源
    data->vref_supply = devm_regulator_get_optional(dev, "vref");
    if (IS_ERR(data->vref_supply)) {
        data->vref_supply = NULL;
    } else {
        ret = regulator_enable(data->vref_supply);
        if (ret) {
            dev_warn(dev, "Failed to enable vref regulator: %d\n", ret);
        }
    }

    data->power_supply = devm_regulator_get_optional(dev, "power");
    if (IS_ERR(data->power_supply)) {
        data->power_supply = NULL;
    } else {
        ret = regulator_enable(data->power_supply);
        if (ret) {
            dev_warn(dev, "Failed to enable power regulator: %d\n", ret);
        }
    }

    // 初始化工作队列
    INIT_DELAYED_WORK(&data->monitor_work, monitor_work_func);

    // 首次读取
    ret = update_detector_values(data);
    if (ret) {
        dev_err(dev, "Failed to read initial values: %d\n", ret);
        goto err_disable_reg;
    }

    // 设置平台设备数据
    platform_set_drvdata(pdev, data);

    // 创建sysfs属性
    ret = sysfs_create_groups(&dev->kobj, hw_detector_groups);
    if (ret) {
        dev_err(dev, "Failed to create sysfs groups: %d\n", ret);
        goto err_disable_reg;
    }

    // 如果设置了监控间隔，启动工作队列
    if (data->monitor_interval > 0) {
        schedule_delayed_work(&data->monitor_work,
                            msecs_to_jiffies(data->monitor_interval));
    }

    dev_info(dev, "HW Detector initialized: HW version=%u, VBAT=%dmV\n",
            data->hw_version, data->vbat_mv);

    return 0;

err_disable_reg:
    if (data->vref_supply)
        regulator_disable(data->vref_supply);
    if (data->power_supply)
        regulator_disable(data->power_supply);

    return ret;
}

static int rockchip_hw_detector_remove(struct platform_device *pdev)
{
    struct hw_detector_data *data = platform_get_drvdata(pdev);

    // 取消工作队列
    cancel_delayed_work_sync(&data->monitor_work);

    // 移除sysfs属性
    sysfs_remove_groups(&pdev->dev.kobj, hw_detector_groups);

    // 禁用电源
    if (data->vref_supply)
        regulator_disable(data->vref_supply);
    if (data->power_supply)
        regulator_disable(data->power_supply);

    mutex_destroy(&data->lock);

    dev_info(&pdev->dev, "HW Detector removed\n");

    return 0;
}

#ifdef CONFIG_PM_SLEEP
static int rockchip_hw_detector_suspend(struct device *dev)
{
    struct hw_detector_data *data = dev_get_drvdata(dev);

    // 暂停监控
    cancel_delayed_work_sync(&data->monitor_work);

    // 可选：关闭ADC电源以省电
    if (data->vref_supply)
        regulator_disable(data->vref_supply);

    return 0;
}

static int rockchip_hw_detector_resume(struct device *dev)
{
    int ret;
    struct hw_detector_data *data = dev_get_drvdata(dev);

    // 恢复ADC电源
    if (data->vref_supply) {
        ret = regulator_enable(data->vref_supply);
        if (ret) {
            dev_warn(dev, "Failed to enable vref regulator: %d\n", ret);
        }
    }


    // 更新值
    update_detector_values(data);

    // 恢复监控
    if (data->monitor_interval > 0) {
        schedule_delayed_work(&data->monitor_work,
                            msecs_to_jiffies(data->monitor_interval));
    }

    return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(rockchip_hw_detector_pm_ops,
                        rockchip_hw_detector_suspend,
                        rockchip_hw_detector_resume);

static const struct of_device_id rockchip_hw_detector_of_match[] = {
    { .compatible = "rockchip,hw-detector" },
    {},
};
MODULE_DEVICE_TABLE(of, rockchip_hw_detector_of_match);

static struct platform_driver rockchip_hw_detector_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = rockchip_hw_detector_of_match,
        .pm = &rockchip_hw_detector_pm_ops,
    },
    .probe = rockchip_hw_detector_probe,
    .remove = rockchip_hw_detector_remove,
};

module_platform_driver(rockchip_hw_detector_driver);

MODULE_AUTHOR("Newayer");
MODULE_DESCRIPTION("Rockchip Hardware Detector Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);
