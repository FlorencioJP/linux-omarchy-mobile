// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIOs of the Samsung S2MPG13 PMIC
 *
 * The S2MPG13 sub-PMIC paired with Google's gs201 SoC has eight GPIOs, in
 * a register block of their own that is reached through the ACPM firmware
 * like the rest of the PMIC. Each pin has a control register with an output
 * level and an output enable bit; the input levels of all pins are in one
 * status register. On the Pixel 7 boards they switch the camera supplies.
 *
 * The register layout is from Google's GPL s2mpg13 register header and its
 * s2mpg1x GPIO driver. Only the output level and output enable bits are
 * ever changed; the pin mode, pulls and drive strength stay as the
 * bootloader left them.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/samsung/s2mpg13.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct s2mpg13_gpio {
	struct gpio_chip gc;
	struct regmap *regmap;
};

static unsigned int s2mpg13_gpio_set_reg(unsigned int offset)
{
	return S2MPG13_GPIO_0_SET + offset;
}

static int s2mpg13_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg13_gpio *chip = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, s2mpg13_gpio_set_reg(offset), &val);
	if (ret)
		return ret;

	return (val & S2MPG13_GPIO_SET_OUT_EN) ? GPIO_LINE_DIRECTION_OUT :
						 GPIO_LINE_DIRECTION_IN;
}

static int s2mpg13_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg13_gpio *chip = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, s2mpg13_gpio_set_reg(offset), &val);
	if (ret)
		return ret;

	/* An output reads back the level it drives. */
	if (val & S2MPG13_GPIO_SET_OUT_EN)
		return !!(val & S2MPG13_GPIO_SET_OUT);

	ret = regmap_read(chip->regmap, S2MPG13_GPIO_STATUS, &val);
	if (ret)
		return ret;

	return !!(val & BIT(offset));
}

static int s2mpg13_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct s2mpg13_gpio *chip = gpiochip_get_data(gc);

	return regmap_update_bits(chip->regmap, s2mpg13_gpio_set_reg(offset),
				  S2MPG13_GPIO_SET_OUT,
				  value ? S2MPG13_GPIO_SET_OUT : 0);
}

static int s2mpg13_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg13_gpio *chip = gpiochip_get_data(gc);

	return regmap_update_bits(chip->regmap, s2mpg13_gpio_set_reg(offset),
				  S2MPG13_GPIO_SET_OUT_EN, 0);
}

static int s2mpg13_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset, int value)
{
	struct s2mpg13_gpio *chip = gpiochip_get_data(gc);

	/* Level and output enable in one read-modify-write, so no glitch. */
	return regmap_update_bits(chip->regmap, s2mpg13_gpio_set_reg(offset),
				  S2MPG13_GPIO_SET_OUT | S2MPG13_GPIO_SET_OUT_EN,
				  (value ? S2MPG13_GPIO_SET_OUT : 0) |
				  S2MPG13_GPIO_SET_OUT_EN);
}

static int s2mpg13_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2mpg13_gpio *chip;

	if (!dev_fwnode(dev))
		return dev_err_probe(dev, -ENODEV, "no device tree node\n");

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->regmap = dev_get_regmap(dev->parent, "gpio");
	if (!chip->regmap)
		return dev_err_probe(dev, -ENODEV, "no GPIO regmap\n");

	chip->gc.label = dev_name(dev);
	chip->gc.parent = dev;
	chip->gc.owner = THIS_MODULE;
	chip->gc.base = -1;
	chip->gc.ngpio = S2MPG13_GPIO_NUM;
	chip->gc.can_sleep = true;
	chip->gc.get_direction = s2mpg13_gpio_get_direction;
	chip->gc.direction_input = s2mpg13_gpio_direction_input;
	chip->gc.direction_output = s2mpg13_gpio_direction_output;
	chip->gc.get = s2mpg13_gpio_get;
	chip->gc.set = s2mpg13_gpio_set;

	return devm_gpiochip_add_data(dev, &chip->gc, chip);
}

static const struct of_device_id s2mpg13_gpio_of_match[] = {
	{ .compatible = "samsung,s2mpg13-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpg13_gpio_of_match);

static const struct platform_device_id s2mpg13_gpio_id[] = {
	{ "s2mpg13-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(platform, s2mpg13_gpio_id);

static struct platform_driver s2mpg13_gpio_driver = {
	.driver = {
		.name = "s2mpg13-gpio",
		.of_match_table = s2mpg13_gpio_of_match,
	},
	.probe = s2mpg13_gpio_probe,
	.id_table = s2mpg13_gpio_id,
};
module_platform_driver(s2mpg13_gpio_driver);

MODULE_DESCRIPTION("Samsung S2MPG13 PMIC GPIO driver");
MODULE_LICENSE("GPL");
