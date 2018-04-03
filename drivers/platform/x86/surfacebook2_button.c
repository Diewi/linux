/*
 * Supports for the button array on the Surface tablets.
 *
 * (C) Copyright 2016 Red Hat, Inc
 *
 * Based on soc_button_array.c:
 *
 * {C} Copyright 2014 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <acpi/acpi_bus.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio_keys.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/uuid.h>


#define SURFACEBOOK2_BUTTON_OBJ_NAME	"VGBI"
#define MAX_NBUTTONS					3

/*******************************************************************************
 * Some of the buttons like volume up/down are auto repeat, while others
 * are not. To support both, we register two platform devices, and put
 * buttons into them based on whether the key should be auto repeat.
 ******************************************************************************/
#define BUTTON_TYPES					2

static const guid_t surfacebook2_button_dsm_guid = 
		GUID_INIT(0x6fd05c69, 0xcde3, 0x49f4,
				  0x95, 0xed, 0xab, 0x16, 0x65, 0x49, 0x80, 0x35);
#define SURFACEBOOK2_BUTTON_DSM_REV		0x01
#define SURFACEBOOK2_BUTTON_DSM_FUNC	0x02
#define SURFACEBOOK2_BUTTON_DSM_RET_ID	0x05

struct surfacebook2_button_info {
	const char *name;
	unsigned int acpi_index;
	unsigned int gpio;
	unsigned int event_type;
	unsigned int event_code;
	bool autorepeat;
	bool wakeup;
	bool active_low;
};

struct surfacebook2_button_data {
	struct platform_device *children[BUTTON_TYPES];
};

static struct surfacebook2_button_info surfacebook2_button_arr[] = {
	{ "power", 0, 0x01DE, EV_KEY, KEY_POWER, false, true, false },
	{ "voldown", 2, 0x019E,EV_KEY, KEY_VOLUMEDOWN, true, false, true },
	{ "volup", 4, 0x019F, EV_KEY, KEY_VOLUMEUP, true, false, true },
	{ }
};

/*******************************************************************************
 * The following variables are used to describe the GPIOs in a readable manner.
 * Although they are not used in this drivers as of now, it is useful to name
 * the GPIOs as every proper DSDT should do.
 ******************************************************************************/
static const struct acpi_gpio_params power_gpio = { 0, 0, false };
static const struct acpi_gpio_params voldown_gpio = { 2, 0, false };
static const struct acpi_gpio_params volup_gpio = { 4, 0, false };

static const struct acpi_gpio_mapping surfacebook2_button_gpios[] = {
  { "power-gpios", &power_gpio, 1 },
  { "voldown-gpios", &volup_gpio, 1 },
  { "volup-gpios", &voldown_gpio, 1 },
  { },
};

static int surfacebook2_button_remove(struct acpi_device *device)
{
	struct surfacebook2_button_data *priv = dev_get_drvdata(&device->dev);
	int i;

	for (i = 0; i < BUTTON_TYPES; i++) {
		if (priv->children[i]) {
			platform_device_unregister(priv->children[i]);
		}
	}

	return AE_OK;
}

static struct platform_device *
surfacebook2_button_device_create(struct acpi_device *device,
			      const struct surfacebook2_button_info *button_info,
				  bool autorepeat)
{
	const struct surfacebook2_button_info *info;
	struct platform_device *pd;
	struct gpio_keys_button *gpio_keys;
	struct gpio_keys_platform_data *gpio_keys_pdata;
	int n_buttons = 0;
	int error;

	gpio_keys_pdata = devm_kzalloc(&device->dev,
				       sizeof(*gpio_keys_pdata) +
				       sizeof(*gpio_keys) * MAX_NBUTTONS,
				       GFP_KERNEL);
	if (!gpio_keys_pdata) {
		return ERR_PTR(-ENOMEM);
	}

	gpio_keys = (void *)(gpio_keys_pdata + 1);

	for (info = button_info; info->name; info++) {
		if (info->autorepeat != autorepeat) {
			continue;
		}

		dev_dbg(&device->dev, "%s: Registering button %s.\n", __func__,
		 		info->name);

		gpio_keys[n_buttons].type = info->event_type;
		gpio_keys[n_buttons].code = info->event_code;
		gpio_keys[n_buttons].gpio = info->gpio;
		gpio_keys[n_buttons].active_low = info->active_low;
		gpio_keys[n_buttons].desc = info->name;
		gpio_keys[n_buttons].wakeup = info->wakeup;
		gpio_keys[n_buttons].debounce_interval = 1;
		n_buttons++;
	}

	if (n_buttons == 0) {
		error = -ENODEV;
		goto err_free_mem;
	}

	gpio_keys_pdata->buttons = gpio_keys;
	gpio_keys_pdata->nbuttons = n_buttons;
	gpio_keys_pdata->rep = autorepeat;

	pd = platform_device_alloc("gpio-keys", PLATFORM_DEVID_AUTO);
	if (!pd) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	error = platform_device_add_data(pd, gpio_keys_pdata,
					 sizeof(*gpio_keys_pdata));
	if (error) {
		goto err_free_pdev;
	}

	error = platform_device_add(pd);
	if (error) {
		goto err_free_pdev;
	}

	return pd;

err_free_pdev:
	platform_device_put(pd);
err_free_mem:
	devm_kfree(&device->dev, gpio_keys_pdata);
	dev_dbg(&device->dev, "%s: Error registering buttons.\n", __func__);
	return ERR_PTR(error);
}

static u64 surfacebook2_button_get_id(struct acpi_device *device) {
	acpi_handle handle = device->handle;
	union acpi_object *obj;

	obj = acpi_evaluate_dsm(handle, &surfacebook2_button_dsm_guid,
			SURFACEBOOK2_BUTTON_DSM_REV, SURFACEBOOK2_BUTTON_DSM_FUNC, 0x00);
	if (obj == NULL) {
		dev_dbg(&device->dev, "failed to evaluate _DSM.\n");
		return -EINVAL;
	} else if (obj->type != ACPI_TYPE_INTEGER) {
		dev_dbg(&device->dev, "received unexpected return type from _DSM.\n");
		return -EINVAL;
	}

	return obj->integer.value;
}

static int surfacebook2_button_add(struct acpi_device *device)
{
	struct surfacebook2_button_data *priv;
	struct platform_device *pd;
	int error, i;

	if ( (strncmp(acpi_device_bid(device), SURFACEBOOK2_BUTTON_OBJ_NAME,
	    		strlen(SURFACEBOOK2_BUTTON_OBJ_NAME)))
			|| (surfacebook2_button_get_id(device) !=
				SURFACEBOOK2_BUTTON_DSM_RET_ID) ) {
		return -ENODEV;
	}

	surfacebook2_button_get_id(device);

	priv = devm_kzalloc(&device->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		return -ENOMEM;
	}
	dev_set_drvdata(&device->dev, priv);

	for (i = 0; i < BUTTON_TYPES; i++) {
		pd = surfacebook2_button_device_create(device,
						   surfacebook2_button_arr,
						   i == 0);
		if (IS_ERR(pd)) {
			error = PTR_ERR(pd);
			if (error != -ENODEV) {
				surfacebook2_button_remove(device);
				return error;
			}
			continue;
		}

		priv->children[i] = pd;
	}

	if (!priv->children[0] && !priv->children[1]) {
		return -ENODEV;
	}

	// Add the gpio description table for completeness.
	acpi_dev_add_driver_gpios(device, surfacebook2_button_gpios);

	return AE_OK;
}

static const struct acpi_device_id surfacebook2_button_ids[] = {
	{"MSHW0040", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, surfacebook2_button_ids);

static struct acpi_driver surfacebook2_button_driver = {
	.name	= "surfacebook2_button",
	.class = "hotkey",
	.owner = THIS_MODULE,
	.ids = surfacebook2_button_ids,
	.ops	= {
		.add = surfacebook2_button_add,
		.remove = surfacebook2_button_remove,
	},
};

static int __init surfacebook2_button_init(void)
{
	return acpi_bus_register_driver(&surfacebook2_button_driver);
}
module_init(surfacebook2_button_init);

static void __exit surfacebook2_button_exit(void)
{
	acpi_bus_unregister_driver(&surfacebook2_button_driver);
}
module_exit(surfacebook2_button_exit);

MODULE_AUTHOR("Alexander Diewald <diewi@diewald-net.com>");
MODULE_DESCRIPTION("surface book 2 button array driver");
MODULE_LICENSE("GPL v2");
