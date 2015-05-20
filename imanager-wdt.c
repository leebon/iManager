/*
 * Advantech iManager Watchdog
 *
 * Copyright (C) 2015 Advantech Co., Ltd., Irvine, CA, USA
 * Author: Richard Vidal-Dorsch <richard.dorsch@advantech.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include "compat.h"
#include "core.h"
#include "wdt.h"

#define WATCHDOG_TIMEOUT 30 /* in seconds */

static unsigned long last_updated = -1;

static uint timeout = WATCHDOG_TIMEOUT;
module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout,
	"Watchdog timeout in seconds. 1 <= timeout <= 65534, default="
	__MODULE_STRING(WATCHDOG_TIMEOUT) ".");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int wdt_start_timer(const struct device *dev)
{
	int ret;

	ret = wdt_core_start_timer();
	if (ret < 0) {
		dev_err(dev, "Failed to start timer\n");
		return ret;
	}

	last_updated = jiffies;

	return 0;
}

static int wdt_stop_timer(const struct device *dev)
{
	int ret;

	ret = wdt_core_stop_timer();
	if (ret < 0) {
		dev_err(dev, "Failed to stop timer\n");
		return ret;
	}

	last_updated = 0;

	return 0;
}

static int wdt_ping(const struct device *dev)
{
	int ret;

	ret = wdt_core_reset_timer();
	if (ret < 0) {
		dev_err(dev, "Failed to reset timer\n");
		return ret;
	}

	last_updated = jiffies;

	return 0;
}

static int imanager_wdt_set(const struct device *dev, unsigned int _timeout)
{
	int ret;

	if (timeout != _timeout)
		timeout = _timeout;

	ret = wdt_core_set_timeout(PWRBTN, timeout);
	if (ret < 0) {
		dev_err(dev, "Failed to set timeout\n");
		return ret;
	}

	if (last_updated != 0)
		last_updated = jiffies;

	return 0;
}

static int imanager_wdt_set_timeout(struct watchdog_device *wdt_dev,
				    unsigned int timeout)
{
	struct imanager_device_data *ec = watchdog_get_drvdata(wdt_dev);
	int ret = 0;

	mutex_lock(&ec->lock);

	ret = imanager_wdt_set(wdt_dev->dev, timeout);

	mutex_unlock(&ec->lock);

	return ret;
}

static unsigned int imanager_wdt_get_timeleft(struct watchdog_device *wdt_dev)
{
	unsigned int timeleft = 0;
	unsigned long time_diff = ((jiffies - last_updated) / HZ);

	if (last_updated && (timeout > time_diff))
		timeleft = timeout - time_diff;

	return timeleft;
}

static int imanager_wdt_start(struct watchdog_device *wdt_dev)
{
	struct imanager_device_data *ec = watchdog_get_drvdata(wdt_dev);
	struct device *dev = wdt_dev->dev;
	int ret = 0;

	mutex_lock(&ec->lock);

	ret = wdt_start_timer(dev);

	mutex_unlock(&ec->lock);

	return ret;
}

static int imanager_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct imanager_device_data *ec = watchdog_get_drvdata(wdt_dev);
	int ret = 0;

	mutex_lock(&ec->lock);

	ret = wdt_stop_timer(wdt_dev->dev);

	mutex_unlock(&ec->lock);

	return ret;
}

static int imanager_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct imanager_device_data *ec = watchdog_get_drvdata(wdt_dev);
	int ret = 0;

	mutex_lock(&ec->lock);

	ret = wdt_ping(wdt_dev->dev);

	mutex_unlock(&ec->lock);

	return ret;
}

static long imanager_wdt_ioctl(struct watchdog_device *wdt_dev,
			       unsigned int cmd, unsigned long arg)
{
	struct imanager_device_data *ec = watchdog_get_drvdata(wdt_dev);
	struct device *dev = wdt_dev->dev;
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	int ret = 0;
	int timeval, options;
	static const struct watchdog_info ident = {
		.options = WDIOF_KEEPALIVEPING |
			   WDIOF_SETTIMEOUT |
			   WDIOF_MAGICCLOSE,
		.firmware_version = 0,
		.identity = "imanager_wdt",
	};

	mutex_lock(&ec->lock);

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		if (copy_to_user(argp, &ident, sizeof(ident)))
			ret = -EFAULT;
		break;
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		ret = put_user(0, p);
		break;
	case WDIOC_SETOPTIONS:
		if (get_user(options, p)) {
			ret = -EFAULT;
			goto out;
		}
		if (options & WDIOS_DISABLECARD)
			wdt_stop_timer(dev);
		if (options & WDIOS_ENABLECARD) {
			wdt_ping(dev);
			wdt_start_timer(dev);
		}
		break;
	case WDIOC_KEEPALIVE:
		wdt_ping(dev);
		break;
	case WDIOC_SETTIMEOUT:
		if (get_user(timeval, p)) {
			ret = -EFAULT;
			goto out;
		}
		if (imanager_wdt_set(dev, timeval)) {
			ret = -EINVAL;
			goto out;
		}
		wdt_ping(dev);
		/* Fall through */
	case WDIOC_GETTIMEOUT:
		ret = put_user(timeout, p);
		break;
	case WDIOC_GETTIMELEFT:
		timeval = imanager_wdt_get_timeleft(wdt_dev);
		ret = put_user(timeval, p);
		break;
	default:
		ret = -ENOTTY;
	}

out:
	mutex_unlock(&ec->lock);

	return ret;
}

static const struct watchdog_info imanager_wdt_info = {
	.options		= WDIOF_SETTIMEOUT |
				  WDIOF_KEEPALIVEPING |
				  WDIOF_MAGICCLOSE,
	.identity		= "imanager_wdt",
	.firmware_version	= 0,
};

static const struct watchdog_ops imanager_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= imanager_wdt_start,
	.stop		= imanager_wdt_stop,
	.ping		= imanager_wdt_ping,
	.set_timeout	= imanager_wdt_set_timeout,
	.get_timeleft	= imanager_wdt_get_timeleft,
	.ioctl		= imanager_wdt_ioctl,
};

static struct watchdog_device imanager_wdt_dev = {
	.info		= &imanager_wdt_info,
	.ops		= &imanager_wdt_ops,
	.timeout	= WATCHDOG_TIMEOUT,
	.min_timeout	= 1,
	.max_timeout	= 0xfffe,
};

static int imanager_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imanager_device_data *ec = dev_get_drvdata(dev->parent);
	int ret;

	if (!ec) {
		dev_err(dev, "Invalid platform data\n");
		return -EINVAL;
	}

	watchdog_set_nowayout(&imanager_wdt_dev, nowayout);
	watchdog_set_drvdata(&imanager_wdt_dev, ec);

	ret = watchdog_register_device(&imanager_wdt_dev);
	if (ret) {
		dev_err(dev, "Failed to register watchdog device\n");
		return ret;
	}

	ret = wdt_core_init();
	if (ret) {
		dev_err(dev, "Failed to initialize watchdog core\n");
		goto unregister_driver;
	}

	wdt_core_disable_all();

	imanager_wdt_set_timeout(&imanager_wdt_dev, timeout);

	dev_info(dev, "Driver loaded (timeout=%d seconds)\n", timeout);

	return 0;

unregister_driver:
	watchdog_unregister_device(&imanager_wdt_dev);
	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int imanager_wdt_remove(struct platform_device *pdev)
{
	wdt_core_disable_all();
	wdt_core_release();

	watchdog_unregister_device(&imanager_wdt_dev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver imanager_wdt_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "imanager_wdt",
	},
	.probe	= imanager_wdt_probe,
	.remove	= imanager_wdt_remove,
};

module_platform_driver(imanager_wdt_driver);

MODULE_DESCRIPTION("Advantech iManager Watchdog Driver");
MODULE_AUTHOR("Richard Vidal-Dorsch <richard.dorsch at advantech.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:imanager_wdt");
