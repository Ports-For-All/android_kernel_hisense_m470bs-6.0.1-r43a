/*
 * drivers/misc/bcm4330_rfkill.c
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * This program was reviewed by Hisense on 2011-11-02
 *
 */

#include <linux/err.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/rfkill.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <mach/io_dpd.h>

struct bcm4330_rfkill_data {
	int gpio_reset;
	int gpio_shutdown;
	int delay;
	struct clk *bt_32k_clk;
};

static struct bcm4330_rfkill_data *bcm4330_rfkill;

static int bcm4330_bt_rfkill_set_power(void *data, bool blocked)
{
	/*
	 * check if BT gpio_shutdown line status and current request are same.
	 * If same, then return, else perform requested operation.
	 */
	if (gpio_get_value(bcm4330_rfkill->gpio_shutdown) && !blocked)
		return 0;

	struct tegra_io_dpd *sd_dpd;
	
	sd_dpd = tegra_io_dpd_get_sdhci();
	
	if (sd_dpd) {
		mutex_lock(&sd_dpd->delay_lock);
		tegra_io_dpd_disable(sd_dpd);
		mutex_unlock(&sd_dpd->delay_lock);
	}

	if (blocked) {
		printk("bcm 4330 power off! \n");
		if (bcm4330_rfkill->gpio_shutdown)
			gpio_direction_output(bcm4330_rfkill->gpio_shutdown, 0);
		if (bcm4330_rfkill->gpio_reset)
			gpio_direction_output(bcm4330_rfkill->gpio_reset, 0);
		if (bcm4330_rfkill->bt_32k_clk)
			clk_disable(bcm4330_rfkill->bt_32k_clk);
	} else {
		printk("bcm 4330 power on!\n");
		if (bcm4330_rfkill->bt_32k_clk)
			clk_enable(bcm4330_rfkill->bt_32k_clk);
		if (bcm4330_rfkill->gpio_shutdown) {
			gpio_direction_output(bcm4330_rfkill->gpio_shutdown, 0);
			msleep(100);
			gpio_direction_output(bcm4330_rfkill->gpio_shutdown, 1);
			msleep(100);
		}
		if (bcm4330_rfkill->gpio_reset) {
			gpio_direction_output(bcm4330_rfkill->gpio_reset, 0);
			msleep(100);
			gpio_direction_output(bcm4330_rfkill->gpio_reset, 1);
			msleep(100);
		}
	}

	if (sd_dpd) {
		mutex_lock(&sd_dpd->delay_lock);
		tegra_io_dpd_enable(sd_dpd);
		mutex_unlock(&sd_dpd->delay_lock);
	}
	
	return 0;
}


static const struct rfkill_ops bcm4330_bt_rfkill_ops = {
	.set_block = bcm4330_bt_rfkill_set_power,
};

static int bcm4330_rfkill_probe(struct platform_device *pdev)
{
	struct rfkill *bt_rfkill;
	struct resource *res;
	int ret;
	bool enable = false;  /* off */
	bool default_sw_block_state;

	bcm4330_rfkill = kzalloc(sizeof(*bcm4330_rfkill), GFP_KERNEL);
	if (!bcm4330_rfkill)
		return -ENOMEM;

	bcm4330_rfkill->bt_32k_clk = clk_get(&pdev->dev, "bcm4330_32k_clk");
	if (IS_ERR(bcm4330_rfkill->bt_32k_clk)) {
		pr_warn("%s: can't find bcm4330_32k_clk.\
				assuming 32k clock to chip\n", __func__);
		bcm4330_rfkill->bt_32k_clk = NULL;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_IO,
						"bcm4330_nreset_gpio");
	if (res) {
		bcm4330_rfkill->gpio_reset = res->start;
	//	tegra_gpio_enable(bcm4330_rfkill->gpio_reset);
	//	ret = gpio_request(bcm4330_rfkill->gpio_reset,
	//					"bcm4330_nreset_gpio");
	} else {
		pr_warn("%s : can't find reset gpio. "
			"reset gpio may not be defined for "
			"this platform \n", __func__);
		bcm4330_rfkill->gpio_reset = 0;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_IO,
						"bcm4330_nshutdown_gpio");
	if (res) {
		bcm4330_rfkill->gpio_shutdown = res->start;
	//	tegra_gpio_enable(bcm4330_rfkill->gpio_shutdown);
	//	ret = gpio_request(bcm4330_rfkill->gpio_shutdown,
	//					"bcm4330_nshutdown_gpio");
	} else {
		pr_warn("%s : can't find shutdown gpio "
			"shutdown gpio may not be defined for "
			"this platform \n", __func__);
		bcm4330_rfkill->gpio_shutdown = 0;
	}

	/* make sure at-least one of the GPIO is defined */
	if (!bcm4330_rfkill->gpio_reset && !bcm4330_rfkill->gpio_shutdown)
		goto free_bcm_res;

	if (bcm4330_rfkill->bt_32k_clk && enable)
		clk_enable(bcm4330_rfkill->bt_32k_clk);
	if (bcm4330_rfkill->gpio_shutdown)
		gpio_direction_output(bcm4330_rfkill->gpio_shutdown, 0);
	if (bcm4330_rfkill->gpio_reset)
		gpio_direction_output(bcm4330_rfkill->gpio_reset, 0);

	bt_rfkill = rfkill_alloc("bcm4330 Bluetooth", &pdev->dev,
				RFKILL_TYPE_BLUETOOTH, &bcm4330_bt_rfkill_ops,
				NULL);

	if (unlikely(!bt_rfkill))
		goto free_bcm_res;

	default_sw_block_state = !enable;
	rfkill_set_states(bt_rfkill, default_sw_block_state, false);

	ret = rfkill_register(bt_rfkill);

	if (unlikely(ret)) {
		rfkill_destroy(bt_rfkill);
		goto free_bcm_res;
	}

	return 0;

free_bcm_res:
	if (bcm4330_rfkill->gpio_shutdown)
		gpio_free(bcm4330_rfkill->gpio_shutdown);
	if (bcm4330_rfkill->gpio_reset)
		gpio_free(bcm4330_rfkill->gpio_reset);
	if (bcm4330_rfkill->bt_32k_clk && enable)
		clk_disable(bcm4330_rfkill->bt_32k_clk);
	if (bcm4330_rfkill->bt_32k_clk)
		clk_put(bcm4330_rfkill->bt_32k_clk);
	kfree(bcm4330_rfkill);
	return -ENODEV;
}

static int bcm4330_rfkill_remove(struct platform_device *pdev)
{
	struct rfkill *bt_rfkill = platform_get_drvdata(pdev);

	if (bcm4330_rfkill->bt_32k_clk)
		clk_put(bcm4330_rfkill->bt_32k_clk);
	rfkill_unregister(bt_rfkill);
	rfkill_destroy(bt_rfkill);
	if (bcm4330_rfkill->gpio_shutdown)
		gpio_free(bcm4330_rfkill->gpio_shutdown);
	if (bcm4330_rfkill->gpio_reset)
		gpio_free(bcm4330_rfkill->gpio_reset);
	kfree(bcm4330_rfkill);

	return 0;
}

static struct platform_driver bcm4330_rfkill_driver = {
	.probe = bcm4330_rfkill_probe,
	.remove = bcm4330_rfkill_remove,
	.driver = {
		   .name = "bcm4330_rfkill",
		   .owner = THIS_MODULE,
	},
};

static int __init bcm4330_rfkill_init(void)
{
	return platform_driver_register(&bcm4330_rfkill_driver);
}

static void __exit bcm4330_rfkill_exit(void)
{
	platform_driver_unregister(&bcm4330_rfkill_driver);
}

module_init(bcm4330_rfkill_init);
module_exit(bcm4330_rfkill_exit);

MODULE_DESCRIPTION("BCM4330 rfkill");
MODULE_AUTHOR("NVIDIA");
MODULE_LICENSE("GPL");
