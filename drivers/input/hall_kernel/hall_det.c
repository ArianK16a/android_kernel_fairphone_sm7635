// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/input/mt.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/of_irq.h>
#include <linux/version.h>
#include <linux/pm_wakeup.h>

struct hall_data {
    int irq;
    int irq_gpio;
    int keycode_up;
    int keycode_down;
    struct input_dev *input;
    struct workqueue_struct *hall_wq;
    struct work_struct hall_work;
    struct platform_device *pdev;
    int hall_status;
    int power_enabled;
    bool probe_flag;
    struct wakeup_source *p_ws;
};

static irqreturn_t interrupt_hall_irq(int irq, void *dev);
static struct class *hall_class = NULL;

static void do_hall_work(struct work_struct *work)
{
    struct hall_data *hall = container_of(work, struct hall_data, hall_work);
    unsigned int gpio_status;
    //struct platform_device *pdev=hall->pdev;
    __pm_wakeup_event(hall->p_ws, jiffies_to_msecs(HZ/2));
    gpio_status = gpio_get_value(hall->irq_gpio);

    if(!gpio_status)
    {
        input_report_key(hall->input, hall->keycode_down, 1);
        input_sync(hall->input);
        input_report_key(hall->input, hall->keycode_down, 0);
        input_sync(hall->input);
        irq_set_irq_type(hall->irq, IRQ_TYPE_EDGE_RISING | IRQF_ONESHOT);
    }
    else
    {
        input_report_key(hall->input, hall->keycode_up, 1);
        input_sync(hall->input);
        input_report_key(hall->input, hall->keycode_up, 0);
        input_sync(hall->input);
        irq_set_irq_type(hall->irq, IRQ_TYPE_EDGE_FALLING | IRQF_ONESHOT);
    }
    enable_irq(hall->irq);
}

static irqreturn_t interrupt_hall_irq(int irq, void *dev)
{
    struct platform_device *pdev = dev;
    struct hall_data *hall = platform_get_drvdata(pdev);

    if (hall->probe_flag == false)
        return IRQ_HANDLED;
    disable_irq_nosync(hall->irq);
    queue_work(hall->hall_wq, &hall->hall_work);
    return IRQ_HANDLED;
}


static ssize_t hall_status_show(struct device *dev,
                                     struct device_attribute *attr, char *buf)
{
    struct hall_data *hall = dev_get_drvdata(dev);
    hall->hall_status = gpio_get_value(hall->irq_gpio);
    sprintf(buf,"%d\n", hall->hall_status);
    return strlen(buf);
}

static DEVICE_ATTR(hall_status, 0444, hall_status_show, NULL);

static int hall_probe(struct platform_device *pdev)
{
    struct hall_data *hall = NULL;
    struct device_node *np = pdev->dev.of_node;
    struct device *hall_dev = NULL;
    struct pinctrl *hall_gpio_pinctrl;
    struct pinctrl_state *hall_gpio_state;
    int rc = 0;

    if (!np)
        return -ENODEV;
    hall = devm_kzalloc(&pdev->dev, sizeof(*hall), GFP_KERNEL);
    if (hall == NULL) {
        pr_err(KERN_INFO"%s:%d Unable to allocate memory\n", __func__, __LINE__);
        return -ENOMEM;
    }
    hall->irq_gpio = of_get_named_gpio(np, "hall-gpio", 0);
    hall->irq = irq_of_parse_and_map(np, 0);
    hall->pdev = pdev;
    dev_set_drvdata(&pdev->dev, hall);

    rc = request_irq(hall->irq , interrupt_hall_irq,  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, pdev->name, pdev);
    if (rc) {
        rc = -1;
        pr_err("%s : requesting IRQ error\n", __func__);
        return rc;
    }

    hall->input = input_allocate_device();
    if (!hall->input) {
        pr_err("hall.c: Not enough memory\n");
        return -ENOMEM;
    }
    hall->input->name = pdev->name;

    if (of_property_read_u32(np, "linux,keycode_down", &hall->keycode_down)) {
        pr_err("KEY_HALL_SENSOR keycode_down without setting in dts\n");
        return -1;
    }
    if (of_property_read_u32(np, "linux,keycode_up", &hall->keycode_up)) {
        pr_err("KEY_HALL_SENSOR keycode_up without setting in dts\n");
        return -1;
    }
    input_set_capability(hall->input, EV_KEY, hall->keycode_down);
    input_set_capability(hall->input, EV_KEY, hall->keycode_up);

    rc = input_register_device(hall->input);
    if (rc) {
        pr_err("hall.c: Failed to register device\n");
        return rc;
    }

    hall->hall_wq = create_singlethread_workqueue("hall_wq");
    if (!hall->hall_wq) {
        pr_err("%s: create thread error!\n", __func__);
    }

    INIT_WORK(&hall->hall_work, do_hall_work);
    enable_irq_wake(hall->irq);
    if (!hall_class)
        hall_class= class_create(THIS_MODULE, "hall_switch");
    hall_dev = device_create(hall_class, NULL, 0, hall, pdev->name);
    if (IS_ERR(hall_dev))
        pr_err( "Failed to create device(hall_dev)!\n");

    if (device_create_file(hall_dev, &dev_attr_hall_status) < 0)
        pr_err( "Failed to create device(hall_dev)'s node hall_status!\n");

    //wake_lock_init(&hall->hall_wakelock, WAKE_LOCK_SUSPEND, pdev->name);
    hall->p_ws = wakeup_source_register(&pdev->dev, "hall_wake_lock");
    if(!hall->p_ws){
        return -1;
    }

    hall_gpio_pinctrl=devm_pinctrl_get(&pdev->dev);
    if (IS_ERR(hall_gpio_pinctrl)) {
        pr_err("Fail to get hall gpio pinctrl!\n");
        return -1;
    }
    hall_gpio_state = pinctrl_lookup_state(hall_gpio_pinctrl,np->name);
    if (IS_ERR(hall_gpio_state)) {
        pr_err("Can not get hall pinctrl state!\n");
        return -1;
    }
    rc = pinctrl_select_state(hall_gpio_pinctrl, hall_gpio_state);
    if (rc) {
        pr_err("can not set hall pinctrl state rc=%d !\n",rc);
        return -1;
    }
    hall->probe_flag = true;
    pr_err("hall_probe successful! pdev->name=%s hall->irq=%d keycode[up:down]=[%d:%d]\n",pdev->name, hall->irq, hall->keycode_up, hall->keycode_down);
    return 0;
}

int hall_remove(struct platform_device *pdev)
{
    struct hall_data *hall = platform_get_drvdata(pdev);

    free_irq(hall->irq, pdev);
    wakeup_source_unregister(hall->p_ws);

    input_unregister_device(hall->input);
    if (hall->input)
    {
        input_free_device(hall->input);
        hall->input = NULL;
    }

    return 0;
}

static struct of_device_id hall_of_match[] = {
    {.compatible = "hall,hall_switch_1", },
    {},
};

static struct platform_driver hall_driver = {
    .probe = hall_probe,
    .remove = hall_remove,
    .driver = {
           .name = "hall_switch",
           .owner = THIS_MODULE,
           .of_match_table = hall_of_match,
    }
};

static int __init hall_init(void)
{
    return platform_driver_register(&hall_driver);
}

static void __exit hall_exit(void)
{
    platform_driver_unregister(&hall_driver);
}

module_init(hall_init);
module_exit(hall_exit);
MODULE_LICENSE("GPL v2");
