#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/timer.h>

#define DRIVER_NAME "my-gpio-init"

struct gpio_sequence_config {
    int gpio_index;
    unsigned int init_delay_ms;  // 初始化延时
    unsigned int state_delay_ms; // 状态变更延时
    int init_state;              // 初始状态
    int target_state;            // 目标状态
    struct hrtimer init_timer;   // 初始化定时器
    struct hrtimer state_timer;  // 状态变更定时器
    struct my_gpio_data *data;
};

struct my_gpio_data {
    struct device *dev;

    // 输出GPIO相关
    struct gpio_desc **output_gpios;
    struct gpio_sequence_config *sequence_configs;
    const char **output_names;
    int num_outputs;

    // 延时配置
    unsigned int global_init_delay_ms;

    // 工作队列用于延时初始化
    struct workqueue_struct *workqueue;
    struct delayed_work init_work;
};

// 状态变更定时器回调函数
static enum hrtimer_restart gpio_state_delay_timer_handler(struct hrtimer *timer) {
    struct gpio_sequence_config *seq_cfg = container_of(timer, struct gpio_sequence_config, state_timer);
    struct my_gpio_data *data = seq_cfg->data;
    int gpio_index = seq_cfg->gpio_index;

    if (gpio_index < data->num_outputs && data->output_gpios[gpio_index]) {
        int current_state = gpiod_get_value(data->output_gpios[gpio_index]);
        int target_state = seq_cfg->target_state;

        // 只有当当前状态不等于目标状态时才进行变更
        if (current_state != target_state) {
            gpiod_set_value(data->output_gpios[gpio_index], target_state);

            dev_info(data->dev, "State change: GPIO %d (%s) changed from %s to %s after %d ms\n",
                     gpio_index,
                     data->output_names ? data->output_names[gpio_index] : "unnamed",
                     current_state ? "HIGH" : "LOW",
                     target_state ? "HIGH" : "LOW",
                     seq_cfg->state_delay_ms);
        } else {
            dev_dbg(data->dev, "State change: GPIO %d already at target state %s\n",
                    gpio_index, target_state ? "HIGH" : "LOW");
        }

        // 状态变更完成后释放GPIO控制权
        gpiod_put(data->output_gpios[gpio_index]);
        data->output_gpios[gpio_index] = NULL;

        dev_info(data->dev, "Released control of output GPIO %d after state change\n", gpio_index);
    }

    return HRTIMER_NORESTART;
}

// 初始化定时器回调函数
static enum hrtimer_restart gpio_init_delay_timer_handler(struct hrtimer *timer) {
    struct gpio_sequence_config *seq_cfg = container_of(timer, struct gpio_sequence_config, init_timer);
    struct my_gpio_data *data = seq_cfg->data;
    int gpio_index = seq_cfg->gpio_index;

    if (gpio_index < data->num_outputs && data->output_gpios[gpio_index]) {
        int state = seq_cfg->init_state;

        // 配置为输出并设置初始状态
        gpiod_direction_output(data->output_gpios[gpio_index], state);

        dev_info(data->dev, "Delayed GPIO %d (%s) set to %s after %d ms\n",
                 gpio_index,
                 data->output_names ? data->output_names[gpio_index] : "unnamed",
                 state ? "HIGH" : "LOW",
                 seq_cfg->init_delay_ms);

        // 如果有状态变更延时配置，启动状态变更定时器
        if (seq_cfg->state_delay_ms > 0) {
            ktime_t ktime_state_delay = ms_to_ktime(seq_cfg->state_delay_ms);
            hrtimer_init(&seq_cfg->state_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
            seq_cfg->state_timer.function = gpio_state_delay_timer_handler;
            hrtimer_start(&seq_cfg->state_timer, ktime_state_delay, HRTIMER_MODE_REL);

            dev_info(data->dev, "Scheduled GPIO %d state change to %s in %d ms\n",
                     gpio_index,
                     seq_cfg->target_state ? "HIGH" : "LOW",
                     seq_cfg->state_delay_ms);
        } else {
            // 没有状态变更延时，直接释放GPIO控制权
            gpiod_put(data->output_gpios[gpio_index]);
            data->output_gpios[gpio_index] = NULL;

            dev_info(data->dev, "Released control of output GPIO %d after initialization\n", gpio_index);
        }
    }

    return HRTIMER_NORESTART;
}

// 工作队列处理函数 - 用于处理带相对延时的GPIO初始化
static void delayed_gpio_init_work(struct work_struct *work) {
    struct my_gpio_data *data = container_of(work, struct my_gpio_data, init_work.work);
    int i;
    ktime_t ktime_delay;

    dev_info(data->dev, "Starting delayed GPIO initialization\n");

    // 处理所有GPIO的初始化
    for (i = 0; i < data->num_outputs; i++) {
        struct gpio_sequence_config *seq_cfg = &data->sequence_configs[i];

        if (seq_cfg->gpio_index < data->num_outputs && data->output_gpios[seq_cfg->gpio_index]) {
            seq_cfg->data = data;

            // 如果有初始化延时，启动初始化定时器
            if (seq_cfg->init_delay_ms > 0) {
                ktime_delay = ms_to_ktime(seq_cfg->init_delay_ms);
                hrtimer_init(&seq_cfg->init_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
                seq_cfg->init_timer.function = gpio_init_delay_timer_handler;
                hrtimer_start(&seq_cfg->init_timer, ktime_delay, HRTIMER_MODE_REL);

                dev_info(data->dev, "Scheduled GPIO %d for initialization in %d ms\n",
                         seq_cfg->gpio_index, seq_cfg->init_delay_ms);
            } else {
                // 没有初始化延时，立即初始化
                int state = seq_cfg->init_state;
                gpiod_direction_output(data->output_gpios[seq_cfg->gpio_index], state);

                dev_info(data->dev, "Immediate GPIO %d (%s) set to %s\n",
                         seq_cfg->gpio_index,
                         data->output_names ? data->output_names[seq_cfg->gpio_index] : "unnamed",
                         state ? "HIGH" : "LOW");

                // 如果有状态变更延时，启动状态变更定时器
                if (seq_cfg->state_delay_ms > 0) {
                    ktime_delay = ms_to_ktime(seq_cfg->state_delay_ms);
                    hrtimer_init(&seq_cfg->state_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
                    seq_cfg->state_timer.function = gpio_state_delay_timer_handler;
                    hrtimer_start(&seq_cfg->state_timer, ktime_delay, HRTIMER_MODE_REL);

                    dev_info(data->dev, "Scheduled GPIO %d state change to %s in %d ms\n",
                             seq_cfg->gpio_index,
                             seq_cfg->target_state ? "HIGH" : "LOW",
                             seq_cfg->state_delay_ms);
                } else {
                    // 没有状态变更延时，直接释放GPIO控制权
                    gpiod_put(data->output_gpios[seq_cfg->gpio_index]);
                    data->output_gpios[seq_cfg->gpio_index] = NULL;

                    dev_info(data->dev, "Released control of immediate output GPIO %d\n", seq_cfg->gpio_index);
                }
            }
        }
    }
}

// 从设备树解析GPIO配置
static int parse_dt(struct platform_device *pdev, struct my_gpio_data *data) {
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    int ret, i;

    // 获取全局初始化延时
    if (of_property_read_u32(np, "init-delay-ms", &data->global_init_delay_ms)) {
        data->global_init_delay_ms = 0; // 默认无延时
    }

    // 解析输出GPIO
    data->num_outputs = gpiod_count(dev, NULL);
    if (data->num_outputs > 0) {
        data->output_gpios = devm_kcalloc(dev, data->num_outputs, sizeof(struct gpio_desc *), GFP_KERNEL);
        data->sequence_configs = devm_kcalloc(dev, data->num_outputs, sizeof(struct gpio_sequence_config), GFP_KERNEL);
        if (!data->output_gpios || !data->sequence_configs)
            return -ENOMEM;

        // 初始化序列配置
        for (i = 0; i < data->num_outputs; i++) {
            data->sequence_configs[i].gpio_index = i;
            data->sequence_configs[i].init_delay_ms = 0;
            data->sequence_configs[i].state_delay_ms = 0;
            data->sequence_configs[i].init_state = 0;   // 默认低电平
            data->sequence_configs[i].target_state = 0; // 默认低电平
        }

        // 获取输出GPIO描述符
        for (i = 0; i < data->num_outputs; i++) {
            data->output_gpios[i] = devm_gpiod_get_index(dev, NULL, i, GPIOD_ASIS);
            if (IS_ERR(data->output_gpios[i])) {
                ret = PTR_ERR(data->output_gpios[i]);
                dev_err(dev, "Failed to get output GPIO %d: %d\n", i, ret);
                return ret;
            }
        }

        // 获取输出初始状态
        if (of_find_property(np, "gpio-init-states", NULL)) {
            int *init_states = devm_kcalloc(dev, data->num_outputs, sizeof(int), GFP_KERNEL);
            if (!init_states)
                return -ENOMEM;

            ret = of_property_read_u32_array(np, "gpio-init-states", (u32 *)init_states, data->num_outputs);
            if (ret) {
                dev_err(dev, "Failed to read gpio-init-states: %d\n", ret);
                devm_kfree(dev, init_states);
                return ret;
            }

            for (i = 0; i < data->num_outputs; i++) {
                data->sequence_configs[i].init_state = init_states[i] ? 1 : 0;
            }

            devm_kfree(dev, init_states);
        } else {
            // 如果没有提供output-init-states，则默认为0（低电平）
            dev_warn(dev, "No output-init-states specified, defaulting all to LOW\n");
            for (i = 0; i < data->num_outputs; i++) {
                data->sequence_configs[i].init_state = 0;
            }
        }

        // 解析初始化延时配置
        if (of_find_property(np, "gpio-delays", NULL)) {
            int len = of_property_count_u32_elems(np, "gpio-delays");
            if (len > 0 && len % 2 == 0) {
                for (i = 0; i < len / 2; i++) {
                    int gpio_index;
                    unsigned int delay_ms;

                    of_property_read_u32_index(np, "gpio-delays", i * 2, (u32 *)&gpio_index);
                    of_property_read_u32_index(np, "gpio-delays", i * 2 + 1, (u32 *)&delay_ms);

                    if (gpio_index >= 0 && gpio_index < data->num_outputs) {
                        data->sequence_configs[gpio_index].init_delay_ms = delay_ms;
                    }
                }
            }
        }

        // 解析状态变更延时配置
        if (of_find_property(np, "gpio-state-delays", NULL)) {
            int len = of_property_count_u32_elems(np, "gpio-state-delays");
            if (len > 0 && len % 3 == 0) {
                for (i = 0; i < len / 3; i++) {
                    int gpio_index;
                    unsigned int delay_ms;
                    int target_state;

                    of_property_read_u32_index(np, "gpio-state-delays", i * 3, (u32 *)&gpio_index);
                    of_property_read_u32_index(np, "gpio-state-delays", i * 3 + 1, (u32 *)&delay_ms);
                    of_property_read_u32_index(np, "gpio-state-delays", i * 3 + 2, (u32 *)&target_state);

                    if (gpio_index >= 0 && gpio_index < data->num_outputs) {
                        data->sequence_configs[gpio_index].state_delay_ms = delay_ms;
                        data->sequence_configs[gpio_index].target_state = target_state ? 1 : 0;
                    }
                }
            }
        }

        // 获取输出GPIO名称（可选）
        data->output_names = devm_kcalloc(dev, data->num_outputs, sizeof(const char *), GFP_KERNEL);
        if (data->output_names) {
            for (i = 0; i < data->num_outputs; i++) {
                ret = of_property_read_string_index(np, "gpio-names", i, &data->output_names[i]);
                if (ret)
                    data->output_names[i] = "unnamed";
            }
        }
    }

    return 0;
}

// 平台驱动probe函数
static int my_gpio_probe(struct platform_device *pdev) {
    struct my_gpio_data *data;
    int ret;
    int i;

    dev_info(&pdev->dev, "My GPIO init driver probing...\n");

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->dev = &pdev->dev;
    platform_set_drvdata(pdev, data);

    // 解析设备树配置
    ret = parse_dt(pdev, data);
    if (ret) {
        dev_err(&pdev->dev, "Failed to parse device tree: %d\n", ret);
        return ret;
    }

    // 创建工作队列
    data->workqueue = create_singlethread_workqueue("my_gpio_init");
    if (!data->workqueue) {
        dev_err(&pdev->dev, "Failed to create workqueue\n");
        return -ENOMEM;
    }

    // 应用全局延时初始化
    if (data->global_init_delay_ms > 0) {
        dev_info(&pdev->dev, "Delaying GPIO initialization by %d ms\n",
                 data->global_init_delay_ms);
        msleep(data->global_init_delay_ms);
    }

    // 打印序列配置信息用于调试
    dev_info(&pdev->dev, "GPIO sequence configurations:\n");
    for (i = 0; i < data->num_outputs; i++) {
        struct gpio_sequence_config *cfg = &data->sequence_configs[i];
        dev_info(&pdev->dev, "GPIO %d (%s): init_state=%s, init_delay=%d ms, state_delay=%d ms, target_state=%s\n",
                 i,
                 data->output_names ? data->output_names[i] : "unnamed",
                 cfg->init_state ? "HIGH" : "LOW",
                 cfg->init_delay_ms,
                 cfg->state_delay_ms,
                 cfg->target_state ? "HIGH" : "LOW");
    }

    // 使用工作队列进行GPIO初始化（支持相对时延）
    INIT_DELAYED_WORK(&data->init_work, delayed_gpio_init_work);
    queue_delayed_work(data->workqueue, &data->init_work, 0);

    dev_info(&pdev->dev, "My GPIO init driver probed successfully\n");
    dev_info(&pdev->dev, "Outputs: %d\n", data->num_outputs);

    return 0;
}

static int my_gpio_remove(struct platform_device *pdev) {
    struct my_gpio_data *data = platform_get_drvdata(pdev);
    int i;

    // 取消工作队列
    if (data->workqueue) {
        cancel_delayed_work_sync(&data->init_work);
        destroy_workqueue(data->workqueue);
    }

    // 取消所有定时器
    if (data->sequence_configs) {
        for (i = 0; i < data->num_outputs; i++) {
            hrtimer_cancel(&data->sequence_configs[i].init_timer);
            hrtimer_cancel(&data->sequence_configs[i].state_timer);
        }
    }

    // 释放输出GPIO（如果还有未释放的）
    for (i = 0; i < data->num_outputs; i++) {
        if (data->output_gpios[i]) {
            gpiod_put(data->output_gpios[i]);
            data->output_gpios[i] = NULL;
        }
    }

    dev_info(&pdev->dev, "My GPIO init driver removed\n");
    return 0;
}

static const struct of_device_id my_gpio_of_match[] = {
    {
        .compatible = "dnxt,my-gpio-init",
    },
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, my_gpio_of_match);

static struct platform_driver my_gpio_driver = {
    .probe = my_gpio_probe,
    .remove = my_gpio_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = my_gpio_of_match,
        .owner = THIS_MODULE,
    },
};

module_platform_driver(my_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Newayer");
MODULE_DESCRIPTION("Custom GPIO initialization driver for RK3506");
MODULE_VERSION("1");
