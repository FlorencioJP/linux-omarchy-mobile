// SPDX-License-Identifier: GPL-2.0-only
/*
 * Command path for the Google gs201 MIPI DSI master (DSIM), for a display
 * the bootloader left running.
 *
 * On gs201 the bootloader brings up DECON0 -> DSIM0 -> the panel in DSI
 * command mode and leaves the pipeline scanning out its framebuffer, which
 * simpledrm then uses. This driver does not touch that pipeline. It only
 * registers a MIPI DSI host on DSIM0, so the panel driver can bind and send
 * DCS commands (brightness, display on/off) through the DSIM packet header
 * and payload FIFOs, which the hardware interleaves with frame transfers.
 *
 * Nothing is sent until the "allow_tx" module parameter is set, so the
 * driver can first be loaded to read the hardware state only.
 *
 * Register layout from Google's GPL display driver
 * (google-modules/display/samsung/cal_9845/regs-dsim.h), which gs201
 * shares with gs101.
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include <drm/drm_mipi_dsi.h>

#define DSIM_VERSION			0x0000
#define DSIM_LINK_STATUS1		0x000c
#define DSIM_DPHY_STATUS		0x001c
#define DSIM_CLK_CTRL			0x0020
#define DSIM_ESCMODE			0x002c
#define DSIM_ESCMODE_CMD_LPDT		BIT(7)
#define DSIM_CONFIG			0x004c
#define DSIM_INTSRC			0x0050
#define DSIM_INTMSK			0x0054
#define DSIM_PKTHDR			0x0058
#define DSIM_PAYLOAD			0x005c
#define DSIM_FIFOCTRL			0x0068
#define DSIM_FIFOCTRL_FULL_PH_SFR	BIT(11)
#define DSIM_FIFOCTRL_EMPTY_PH_SFR	BIT(10)
#define DSIM_FIFOCTRL_FULL_PL_SFR	BIT(9)
#define DSIM_FIFOCTRL_EMPTY_PL_SFR	BIT(8)
#define DSIM_CMD_CONFIG			0x0080
#define DSIM_CMD_CONFIG_PKT_GO_EN	BIT(16)
#define DSIM_CMD_TE_CTRL0		0x0084
#define DSIM_CMD_TE_CTRL1		0x0088
#define DSIM_CMD_STATUS			0x008c

#define DSIM_MAX_PL_FIFO		2048
/* the vendor driver's MIPI_WR_TIMEOUT */
#define DSIM_TX_TIMEOUT_US		80000

static bool allow_tx;
module_param(allow_tx, bool, 0644);
MODULE_PARM_DESC(allow_tx, "Send DSI packets to the panel (default: off, read-only)");

struct gs_dsim {
	struct device *dev;
	void __iomem *regs;
	struct mipi_dsi_host host;
	/* serialises use of the header and payload FIFOs */
	struct mutex lock;
};

static inline struct gs_dsim *host_to_dsim(struct mipi_dsi_host *host)
{
	return container_of(host, struct gs_dsim, host);
}

static void gs_dsim_dump(struct gs_dsim *dsim)
{
	static const struct {
		const char *name;
		u32 offset;
	} regs[] = {
		{ "VERSION", DSIM_VERSION },
		{ "LINK_STATUS1", DSIM_LINK_STATUS1 },
		{ "DPHY_STATUS", DSIM_DPHY_STATUS },
		{ "CLK_CTRL", DSIM_CLK_CTRL },
		{ "ESCMODE", DSIM_ESCMODE },
		{ "CONFIG", DSIM_CONFIG },
		{ "INTSRC", DSIM_INTSRC },
		{ "INTMSK", DSIM_INTMSK },
		{ "FIFOCTRL", DSIM_FIFOCTRL },
		{ "CMD_CONFIG", DSIM_CMD_CONFIG },
		{ "CMD_TE_CTRL0", DSIM_CMD_TE_CTRL0 },
		{ "CMD_TE_CTRL1", DSIM_CMD_TE_CTRL1 },
		{ "CMD_STATUS", DSIM_CMD_STATUS },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		dev_info(dsim->dev, "%-12s (0x%03x) = 0x%08x\n", regs[i].name,
			 regs[i].offset, readl(dsim->regs + regs[i].offset));
}

static int gs_dsim_wait_fifo(struct gs_dsim *dsim, u32 mask, bool set)
{
	u32 val;

	if (set)
		return readl_poll_timeout(dsim->regs + DSIM_FIFOCTRL, val,
					  (val & mask) == mask, 10,
					  DSIM_TX_TIMEOUT_US);

	return readl_poll_timeout(dsim->regs + DSIM_FIFOCTRL, val,
				  !(val & mask), 10, DSIM_TX_TIMEOUT_US);
}

static int gs_dsim_attach(struct mipi_dsi_host *host,
			  struct mipi_dsi_device *device)
{
	struct gs_dsim *dsim = host_to_dsim(host);

	dev_info(dsim->dev, "%s attached: %u lanes, mode flags 0x%lx\n",
		 dev_name(&device->dev), device->lanes, device->mode_flags);

	return 0;
}

static int gs_dsim_detach(struct mipi_dsi_host *host,
			  struct mipi_dsi_device *device)
{
	return 0;
}

static ssize_t gs_dsim_transfer(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct gs_dsim *dsim = host_to_dsim(host);
	struct mipi_dsi_packet packet;
	const u8 *p, *end;
	u32 word;
	int ret;

	if (!READ_ONCE(allow_tx)) {
		dev_dbg_ratelimited(dsim->dev,
				    "not sending packet 0x%02x: allow_tx is off\n",
				    msg->type);
		return -EPERM;
	}

	/* Reads need BTA and the RX FIFO, which are not wired up yet. */
	if (msg->rx_len)
		return -EOPNOTSUPP;

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret)
		return ret;

	if (packet.payload_length > DSIM_MAX_PL_FIFO)
		return -EINVAL;

	mutex_lock(&dsim->lock);

	/*
	 * With packet go enabled the FIFOs hold packets until software
	 * releases them in the TE window. The bootloader is not expected to
	 * leave it on; if it did, a write here would never go out.
	 */
	if (readl(dsim->regs + DSIM_CMD_CONFIG) & DSIM_CMD_CONFIG_PKT_GO_EN) {
		dev_warn_ratelimited(dsim->dev, "packet go is enabled, not sending\n");
		ret = -EBUSY;
		goto out;
	}

	ret = gs_dsim_wait_fifo(dsim, DSIM_FIFOCTRL_FULL_PH_SFR, false);
	if (ret)
		goto timeout;

	/* payload first, packed little-endian into 32-bit words */
	p = packet.payload;
	end = p + packet.payload_length;
	while (p < end) {
		size_t i, n = min_t(size_t, 4, end - p);

		word = 0;
		for (i = 0; i < n; i++)
			word |= (u32)p[i] << (8 * i);
		writel(word, dsim->regs + DSIM_PAYLOAD);
		p += n;
	}

	writel(packet.header[0] | packet.header[1] << 8 |
	       packet.header[2] << 16, dsim->regs + DSIM_PKTHDR);

	if (packet.payload_length) {
		ret = gs_dsim_wait_fifo(dsim, DSIM_FIFOCTRL_EMPTY_PL_SFR, true);
		if (ret)
			goto timeout;
	}
	ret = gs_dsim_wait_fifo(dsim, DSIM_FIFOCTRL_EMPTY_PH_SFR, true);
	if (ret)
		goto timeout;

	ret = packet.size;
	goto out;

timeout:
	dev_err(dsim->dev, "timed out sending packet 0x%02x 0x%02x\n",
		packet.header[0], packet.header[1]);
	gs_dsim_dump(dsim);
out:
	mutex_unlock(&dsim->lock);
	return ret;
}

static const struct mipi_dsi_host_ops gs_dsim_host_ops = {
	.attach = gs_dsim_attach,
	.detach = gs_dsim_detach,
	.transfer = gs_dsim_transfer,
};

static int gs_dsim_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs_dsim *dsim;
	int ret;

	dsim = devm_kzalloc(dev, sizeof(*dsim), GFP_KERNEL);
	if (!dsim)
		return -ENOMEM;

	dsim->dev = dev;
	ret = devm_mutex_init(dev, &dsim->lock);
	if (ret)
		return ret;

	dsim->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dsim->regs))
		return PTR_ERR(dsim->regs);

	platform_set_drvdata(pdev, dsim);

	/* Bootloader state, for the record. Reads only. */
	gs_dsim_dump(dsim);

	dsim->host.dev = dev;
	dsim->host.ops = &gs_dsim_host_ops;
	ret = mipi_dsi_host_register(&dsim->host);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register DSI host\n");

	return 0;
}

static void gs_dsim_remove(struct platform_device *pdev)
{
	struct gs_dsim *dsim = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&dsim->host);
}

static const struct of_device_id gs_dsim_of_match[] = {
	{ .compatible = "google,gs201-dsim" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gs_dsim_of_match);

static struct platform_driver gs_dsim_driver = {
	.probe = gs_dsim_probe,
	.remove = gs_dsim_remove,
	.driver = {
		.name = "gs-dsim-cmd",
		.of_match_table = gs_dsim_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(gs_dsim_driver);

MODULE_DESCRIPTION("Google gs201 DSIM command path for a bootloader-initialised display");
MODULE_LICENSE("GPL");
