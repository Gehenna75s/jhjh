/*
 * Allwinner sunxi resistive touchscreen controller driver
 *
 * Copyright (C) 2013 - 2014 Hans de Goede <hdegoede@redhat.com>
 *
 * The hwmon parts are based on work by Corentin LABBE which is:
 * Copyright (C) 2013 Corentin LABBE <clabbe.montjoie@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * The sun4i-ts controller is capable of detecting a second touch, but when a
 * second touch is present then the accuracy becomes so bad the reported touch
 * location is not useable.
 *
 * The original android driver contains some complicated heuristics using the
 * aprox. distance between the 2 touches to see if the user is making a pinch
 * open / close movement, and then reports emulated multi-touch events around
 * the last touch coordinate (as the dual-touch coordinates are worthless).
 *
 * These kinds of heuristics are just asking for trouble (and don't belong
 * in the kernel). So this driver offers straight forward, reliable single
 * touch functionality only.
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define TP_CTRL0		0x00
#define TP_CTRL1		0x04
#define TP_CTRL2		0x08
#define TP_CTRL3		0x0c
#define TP_INT_FIFOC		0x10
#define TP_INT_FIFOS		0x14
#define TP_TPR			0x18
#define TP_CDAT			0x1c
#define TEMP_DATA		0x20
#define TP_DATA			0x24

/* TP_CTRL0 bits */
#define ADC_FIRST_DLY(x)	((x) << 24) /* 8 bits */
#define ADC_FIRST_DLY_MODE(x)	((x) << 23)
#define ADC_CLK_SEL(x)		((x) << 22)
#define ADC_CLK_DIV(x)		((x) << 20) /* 3 bits */
#define FS_DIV(x)		((x) << 16) /* 4 bits */
#define T_ACQ(x)		((x) << 0) /* 16 bits */

/* TP_CTRL1 bits */
#define STYLUS_UP_DEBOUN(x)	((x) << 12) /* 8 bits */
#define STYLUS_UP_DEBOUN_EN(x)	((x) << 9)
#define TOUCH_PAN_CALI_EN(x)	((x) << 6)
#define TP_DUAL_EN(x)		((x) << 5)
#define TP_MODE_EN(x)		((x) << 4)
#define TP_ADC_SELECT(x)	((x) << 3)
#define ADC_CHAN_SELECT(x)	((x) << 0)  /* 3 bits */

/* TP_CTRL2 bits */
#define TP_SENSITIVE_ADJUST(x)	((x) << 28) /* 4 bits */
#define TP_MODE_SELECT(x)	((x) << 26) /* 2 bits */
#define PRE_MEA_EN(x)		((x) << 24)
#define PRE_MEA_THRE_CNT(x)	((x) << 0) /* 24 bits */

/* TP_CTRL3 bits */
#define FILTER_EN(x)		((x) << 2)
#define FILTER_TYPE(x)		((x) << 0)  /* 2 bits */

/* TP_INT_FIFOC irq and fifo mask / control bits */
#define TEMP_IRQ_EN(x)		((x) << 18)
#define OVERRUN_IRQ_EN(x)	((x) << 17)
#define DATA_IRQ_EN(x)		((x) << 16)
#define TP_DATA_XY_CHANGE(x)	((x) << 13)
#define FIFO_TRIG(x)		((x) << 8)  /* 5 bits */
#define DATA_DRQ_EN(x)		((x) << 7)
#define FIFO_FLUSH(x)		((x) << 4)
#define TP_UP_IRQ_EN(x)		((x) << 1)
#define TP_DOWN_IRQ_EN(x)	((x) << 0)

/* TP_INT_FIFOS irq and fifo status bits */
#define TEMP_DATA_PENDING	BIT(18)
#define FIFO_OVERRUN_PENDING	BIT(17)
#define FIFO_DATA_PENDING	BIT(16)
#define TP_IDLE_FLG		BIT(2)
#define TP_UP_PENDING		BIT(1)
#define TP_DOWN_PENDING		BIT(0)

/* TP_TPR bits */
#define TEMP_ENABLE(x)		((x) << 16)
#define TEMP_PERIOD(x)		((x) << 0)  /* t = x * 256 * 16 / clkin */

struct sun4i_ts_data {
	struct device *dev;
	struct input_dev *input;
	void __iomem *base;
	unsigned int irq;
	bool ignore_fifo_data;
	int temp_data;
};

static void sun4i_ts_irq_handle_input(struct sun4i_ts_data *ts, u32 reg_val)
{
	u32 x, y;

	if (reg_val & FIFO_DATA_PENDING) {
		x = readl(ts->base + TP_DATA);
		y = readl(ts->base + TP_DATA);
		/* The 1st location reported after an up event is unreliable */
		if (!ts->ignore_fifo_data) {
			input_report_abs(ts->input, ABS_X, x);
			input_report_abs(ts->input, ABS_Y, y);
			/*
			 * The hardware has a separate down status bit, but
			 * that gets set before we get the first location,
			 * resulting in reporting a click on the old location.
			 */
			input_report_key(ts->input, BTN_TOUCH, 1);
			input_sync(ts->input);
		} else {
			ts->ignore_fifo_data = false;
		}
	}

	if (reg_val & TP_UP_PENDING) {
		ts->ignore_fifo_data = true;
		input_report_key(ts->input, BTN_TOUCH, 0);
		input_sync(ts->input);
	}
}

static irqreturn_t sun4i_ts_irq(int irq, void *dev_id)
{
	struct sun4i_ts_data *ts = dev_id;
	u32 reg_val;

	reg_val  = readl(ts->base + TP_INT_FIFOS);

	if (reg_val & TEMP_DATA_PENDING)
		ts->temp_data = readl(ts->base + TEMP_DATA);

	if (ts->input)
		sun4i_ts_irq_handle_input(ts, reg_val);

	writel(reg_val, ts->base + TP_INT_FIFOS);

	return IRQ_HANDLED;
}

static int sun4i_ts_open(struct input_dev *dev)
{
	struct sun4i_ts_data *ts = input_get_drvdata(dev);

	/* Flush, set trig level to 1, enable temp, data and up irqs */
	writel(TEMP_IRQ_EN(1) | DATA_IRQ_EN(1) | FIFO_TRIG(1) | FIFO_FLUSH(1) |
		TP_UP_IRQ_EN(1), ts->base + TP_INT_FIFOC);

	return 0;
}

static void sun4i_ts_close(struct input_dev *dev)
{
	struct sun4i_ts_data *ts = input_get_drvdata(dev);

	/* Deactivate all input IRQs */
	writel(TEMP_IRQ_EN(1), ts->base + TP_INT_FIFOC);
}

static ssize_t show_temp(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	struct sun4i_ts_data *ts = dev_get_drvdata(dev);

	/* No temp_data until the first irq */
	if (ts->temp_data == -1)
		return -EAGAIN;

	return sprintf(buf, "%d\n", (ts->temp_data - 1447) * 100);
}

static ssize_t show_temp_label(struct device *dev,
			      struct device_attribute *devattr, char *buf)
{
	return sprintf(buf, "SoC temperature\n");
}

static DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL);
static DEVICE_ATTR(temp1_label, S_IRUGO, show_temp_label, NULL);

static struct attribute *sun4i_ts_attrs[] = {
	&dev_attr_temp1_input.attr,
	&dev_attr_temp1_label.attr,
	NULL
};
ATTRIBUTE_GROUPS(sun4i_ts);

static int sun4i_ts_probe(struct platform_device *pdev)
{
	struct sun4i_ts_data *ts;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device *hwmon;
	int error;
	bool ts_attached;

	ts = devm_kzalloc(dev, sizeof(struct sun4i_ts_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = dev;
	ts->ignore_fifo_data = true;
	ts->temp_data = -1;

	ts_attached = of_property_read_bool(np, "allwinner,ts-attached");
	if (ts_attached) {
		ts->input = devm_input_allocate_device(dev);
		if (!ts->input)
			return -ENOMEM;

		ts->input->name = pdev->name;
		ts->input->phys = "sun4i_ts/input0";
		ts->input->open = sun4i_ts_open;
		ts->input->close = sun4i_ts_close;
		ts->input->id.bustype = BUS_HOST;
		ts->input->id.vendor = 0x0001;
		ts->input->id.product = 0x0001;
		ts->input->id.version = 0x0100;
		ts->input->evbit[0] =  BIT(EV_SYN) | BIT(EV_KEY) | BIT(EV_ABS);
		__set_bit(BTN_TOUCH, ts->input->keybit);
		input_set_abs_params(ts->input, ABS_X, 0, 4095, 0, 0);
		input_set_abs_params(ts->input, ABS_Y, 0, 4095, 0, 0);
		input_set_drvdata(ts->input, ts);
	}

	ts->base = devm_ioremap_resource(dev,
			      platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(ts->base))
		return PTR_ERR(ts->base);

	ts->irq = platform_get_irq(pdev, 0);
	error = devm_request_irq(dev, ts->irq, sun4i_ts_irq, 0, "sun4i-ts", ts);
	if (error)
		return error;

	/*
	 * Select HOSC clk, clkin = clk / 6, adc samplefreq = clkin / 8192,
	 * t_acq = clkin / (16 * 64)
	 */
	writel(ADC_CLK_SEL(0) | ADC_CLK_DIV(2) | FS_DIV(7) | T_ACQ(63),
	       ts->base + TP_CTRL0);

	/*
	 * sensitive_adjust = 15 : max, which is not all that sensitive,
	 * tp_mode = 0 : only x and y coordinates, as we don't use dual touch
	 */
	writel(TP_SENSITIVE_ADJUST(15) | TP_MODE_SELECT(0),
	       ts->base + TP_CTRL2);

	/* Enable median filter, type 1 : 5/3 */
	writel(FILTER_EN(1) | FILTER_TYPE(1), ts->base + TP_CTRL3);

	/* Enable temperature measurement, period 1953 (2 seconds) */
	writel(TEMP_ENABLE(1) | TEMP_PERIOD(1953), ts->base + TP_TPR);

	/*
	 * Set stylus up debounce to aprox 10 ms, enable debounce, and
	 * finally enable tp mode.
	 */
	writel(STYLUS_UP_DEBOUN(5) | STYLUS_UP_DEBOUN_EN(1) | TP_MODE_EN(1),
	       ts->base + TP_CTRL1);

	hwmon = devm_hwmon_device_register_with_groups(ts->dev, "sun4i_ts",
						       ts, sun4i_ts_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	writel(TEMP_IRQ_EN(1), ts->base + TP_INT_FIFOC);

	if (ts_attached) {
		error = input_register_device(ts->input);
		if (error) {
			writel(0, ts->base + TP_INT_FIFOC);
			return error;
		}
	}

	platform_set_drvdata(pdev, ts);
	return 0;
}

static int sun4i_ts_remove(struct platform_device *pdev)
{
	struct sun4i_ts_data *ts = platform_get_drvdata(pdev);

	/* Explicit unregister to avoid open/close changing the imask later */
	if (ts->input)
		input_unregister_device(ts->input);

	/* Deactivate all IRQs */
	writel(0, ts->base + TP_INT_FIFOC);

	return 0;
}

static const struct of_device_id sun4i_ts_of_match[] = {
	{ .compatible = "allwinner,sun4i-a10-ts", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun4i_ts_of_match);

static struct platform_driver sun4i_ts_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "sun4i-ts",
		.of_match_table = of_match_ptr(sun4i_ts_of_match),
	},
	.probe	= sun4i_ts_probe,
	.remove	= sun4i_ts_remove,
};

module_platform_driver(sun4i_ts_driver);

MODULE_DESCRIPTION("Allwinner sun4i resistive touchscreen controller driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
