// SPDX-License-Identifier: GPL-2.0-only
/*
 * SEMCO SEM1215SA lens driver (focus only)
 *
 * The SEM1215SA is Samsung Electro-Mechanics' combined autofocus and
 * optical image stabilisation controller with the module's calibration
 * memory behind it; the Pixel 7 Pro's folded telephoto uses one. It has
 * 16-bit register addresses and 8-bit values. This driver uses its focus
 * part only: after power-up it waits for the controller to report ready,
 * switches the focus servo on, and moves the lens to a 10-bit position.
 * OIS is left off.
 *
 * Registers used (all others, the calibration memory included, are never
 * written):
 *   0x0200 focus servo on (1) / off (0)
 *   0x0201 ready (reads 1)
 *   0x0204 position, low byte; 0x0205 high byte (the move starts on it)
 *   0x0206/0x0207 position read back, low and high byte
 *   0x0b00 status monitor on (1)
 * The module's factory focus calibration (big-endian, 16-bit), believed to
 * hold infinity at 0xc7b6 and macro at 0xc7bc, is read once at probe and
 * limits the focus control to the calibrated span plus a margin; values
 * that make no sense leave the whole range.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define SEM1215_REG_AF_CTRL		CCI_REG8(0x0200)
#define SEM1215_AF_ON			0x01
#define SEM1215_AF_OFF			0x00
#define SEM1215_REG_STATUS		CCI_REG8(0x0201)
#define SEM1215_STATUS_READY		0x01
#define SEM1215_REG_POS_L		CCI_REG8(0x0204)
#define SEM1215_REG_POS_H		CCI_REG8(0x0205)
#define SEM1215_REG_POS_RB		CCI_REG16_LE(0x0206)
#define SEM1215_REG_MONITOR		CCI_REG8(0x0b00)
#define SEM1215_REG_CAL_INF		CCI_REG16(0xc7b6)
#define SEM1215_REG_CAL_MACRO		CCI_REG16(0xc7bc)

#define SEM1215_POS_MAX			1023
#define SEM1215_CAL_MARGIN		40
#define SEM1215_READY_TIMEOUT_US	100000

static bool debug_position;
module_param(debug_position, bool, 0644);
MODULE_PARM_DESC(debug_position, "Log the position read back after each move");

static const char * const sem1215_supply_names[] = {
	"vbb", "vio", "vdd", "vsw",
};

#define SEM1215_NUM_SUPPLIES	ARRAY_SIZE(sem1215_supply_names)
#define SEM1215_SUPPLY_VIO	1

struct sem1215 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct regulator *supplies[SEM1215_NUM_SUPPLIES];
	bool active;
};

static inline struct sem1215 *to_sem1215(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sem1215, sd);
}

static int sem1215_move(struct sem1215 *lens, u32 pos)
{
	int ret = 0;

	cci_write(lens->regmap, SEM1215_REG_POS_L, pos & 0xff, &ret);
	cci_write(lens->regmap, SEM1215_REG_POS_H, pos >> 8, &ret);

	if (!ret && debug_position) {
		u64 rb = 0;

		usleep_range(5000, 6000);
		cci_read(lens->regmap, SEM1215_REG_POS_RB, &rb, NULL);
		dev_info(lens->dev, "position %u, read back %llu\n", pos, rb);
	}

	return ret;
}

static int sem1215_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sem1215 *lens = container_of(ctrl->handler, struct sem1215, ctrls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	/* Only while powered; resume applies the value. */
	if (pm_runtime_get_if_in_use(lens->dev) <= 0)
		return 0;

	ret = sem1215_move(lens, ctrl->val);
	pm_runtime_put(lens->dev);

	return ret;
}

static const struct v4l2_ctrl_ops sem1215_ctrl_ops = {
	.s_ctrl = sem1215_set_ctrl,
};

static int sem1215_supplies_on(struct sem1215 *lens)
{
	int i, ret;

	for (i = 0; i < SEM1215_NUM_SUPPLIES; i++) {
		if (!lens->supplies[i])
			continue;
		ret = regulator_enable(lens->supplies[i]);
		if (ret)
			goto err;
		/* stock: 1 ms after the I/O supply */
		if (i == SEM1215_SUPPLY_VIO)
			usleep_range(1000, 1100);
	}

	return 0;

err:
	while (--i >= 0)
		if (lens->supplies[i])
			regulator_disable(lens->supplies[i]);
	return ret;
}

static void sem1215_supplies_off(struct sem1215 *lens)
{
	int i;

	for (i = SEM1215_NUM_SUPPLIES - 1; i >= 0; i--)
		if (lens->supplies[i])
			regulator_disable(lens->supplies[i]);
}

static int sem1215_wait_ready(struct sem1215 *lens)
{
	u64 status = 0;
	int ret, err = 0;

	ret = read_poll_timeout(cci_read, err,
				err || status == SEM1215_STATUS_READY,
				2000, SEM1215_READY_TIMEOUT_US, false,
				lens->regmap, SEM1215_REG_STATUS, &status, NULL);
	if (err)
		return err;
	if (ret)
		dev_err(lens->dev, "not ready: status 0x%02llx\n", status);

	return ret;
}

static int sem1215_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sem1215 *lens = to_sem1215(sd);
	int ret;

	if (lens->active)
		return 0;

	ret = sem1215_supplies_on(lens);
	if (ret)
		return ret;

	ret = sem1215_wait_ready(lens);
	if (!ret)
		ret = cci_write(lens->regmap, SEM1215_REG_AF_CTRL, SEM1215_AF_ON, NULL);
	if (!ret)
		ret = cci_write(lens->regmap, SEM1215_REG_MONITOR, 1, NULL);
	if (!ret)
		ret = sem1215_move(lens, lens->focus->val);
	if (ret) {
		sem1215_supplies_off(lens);
		return ret;
	}

	lens->active = true;

	return 0;
}

static int sem1215_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sem1215 *lens = to_sem1215(sd);

	if (!lens->active)
		return 0;

	cci_write(lens->regmap, SEM1215_REG_AF_CTRL, SEM1215_AF_OFF, NULL);
	sem1215_supplies_off(lens);
	lens->active = false;

	return 0;
}

static int sem1215_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int sem1215_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops sem1215_internal_ops = {
	.open = sem1215_open,
	.close = sem1215_close,
};

static const struct v4l2_subdev_ops sem1215_ops = { };

/*
 * The focus range: the module's calibrated infinity and macro positions
 * plus a margin either side, or the whole range if the calibration can't
 * be read or makes no sense. Powers the controller for the read.
 */
static void sem1215_range(struct sem1215 *lens, s32 *min, s32 *max)
{
	u64 inf = 0, macro = 0;
	int ret;

	*min = 0;
	*max = SEM1215_POS_MAX;

	ret = sem1215_supplies_on(lens);
	if (ret)
		return;
	ret = sem1215_wait_ready(lens);
	cci_read(lens->regmap, SEM1215_REG_CAL_INF, &inf, &ret);
	cci_read(lens->regmap, SEM1215_REG_CAL_MACRO, &macro, &ret);
	sem1215_supplies_off(lens);

	if (ret || inf >= macro || macro > SEM1215_POS_MAX) {
		dev_info(lens->dev, "no focus calibration (%d), range 0-%u\n",
			 ret, SEM1215_POS_MAX);
		return;
	}

	*min = max_t(s32, (s32)inf - SEM1215_CAL_MARGIN, 0);
	*max = min_t(s32, (s32)macro + SEM1215_CAL_MARGIN, SEM1215_POS_MAX);
	dev_info(lens->dev, "focus range %d-%d (calibrated %llu-%llu)\n",
		 *min, *max, inf, macro);
}

static int sem1215_init_controls(struct sem1215 *lens)
{
	struct v4l2_ctrl_handler *hdl = &lens->ctrls;
	s32 min, max;

	sem1215_range(lens, &min, &max);

	v4l2_ctrl_handler_init(hdl, 1);
	/* start a quarter of the way from infinity */
	lens->focus = v4l2_ctrl_new_std(hdl, &sem1215_ctrl_ops,
					V4L2_CID_FOCUS_ABSOLUTE, min, max, 1,
					min + (max - min) / 4);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	lens->sd.ctrl_handler = hdl;

	return 0;
}

static int sem1215_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sem1215 *lens;
	unsigned int i;
	int ret;

	lens = devm_kzalloc(dev, sizeof(*lens), GFP_KERNEL);
	if (!lens)
		return -ENOMEM;

	lens->dev = dev;
	lens->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(lens->regmap))
		return PTR_ERR(lens->regmap);

	for (i = 0; i < SEM1215_NUM_SUPPLIES; i++) {
		const char *name = sem1215_supply_names[i];
		/* the board supply and the switched one are optional */
		bool optional = i == 0 || i == SEM1215_NUM_SUPPLIES - 1;

		lens->supplies[i] = optional ?
			devm_regulator_get_optional(dev, name) :
			devm_regulator_get(dev, name);
		if (IS_ERR(lens->supplies[i])) {
			ret = PTR_ERR(lens->supplies[i]);
			if (optional && ret == -ENODEV) {
				lens->supplies[i] = NULL;
				continue;
			}
			return dev_err_probe(dev, ret, "%s supply\n", name);
		}
	}

	v4l2_i2c_subdev_init(&lens->sd, client, &sem1215_ops);
	lens->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	lens->sd.internal_ops = &sem1215_internal_ops;
	lens->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = sem1215_init_controls(lens);
	if (ret)
		return ret;

	ret = media_entity_pads_init(&lens->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrls;

	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev(&lens->sd);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	media_entity_cleanup(&lens->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&lens->ctrls);
	return ret;
}

static void sem1215_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sem1215 *lens = to_sem1215(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&lens->ctrls);
	media_entity_cleanup(&sd->entity);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		sem1215_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(sem1215_pm_ops, sem1215_power_off,
				 sem1215_power_on, NULL);

static const struct of_device_id sem1215_of_match[] = {
	{ .compatible = "semco,sem1215sa" },
	{ }
};
MODULE_DEVICE_TABLE(of, sem1215_of_match);

static struct i2c_driver sem1215_driver = {
	.probe = sem1215_probe,
	.remove = sem1215_remove,
	.driver = {
		.name = "sem1215",
		.of_match_table = sem1215_of_match,
		.pm = pm_ptr(&sem1215_pm_ops),
	},
};
module_i2c_driver(sem1215_driver);

MODULE_DESCRIPTION("SEMCO SEM1215SA lens driver");
MODULE_LICENSE("GPL");
