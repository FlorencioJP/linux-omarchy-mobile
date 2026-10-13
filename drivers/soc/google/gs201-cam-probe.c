// SPDX-License-Identifier: GPL-2.0-only
/*
 * gs201 camera receiver: read-only power and register probe, for bring-up
 *
 * Before a CSIS driver touches the camera receiver it must know that the
 * block is powered: a read of an unpowered or isolated block aborts the SoC.
 * The bootloader leaves the camera power domains on, and no Linux driver
 * turns them off (there is no gs201 power-domain driver yet), but that has
 * not been checked on our kernel.
 *
 * On load this module always reads, through the exynos-pmu regmap (the
 * same plain read Google's power-domain code uses for these registers):
 *
 *   PMU 0x2404  CSIS_STATUS    bit 0 = pd-csis on
 *   PMU 0x2484  PDP_STATUS     bit 0 = pd-pdp on
 *   PMU 0x3ebc  PHY_CTRL_MIPI_DCPHY_S4S4S4S4, bit 0 = isolation released
 *
 * and logs them. With step= set to anything but "pmu" it then reads exactly
 * one more register, mapped for that read only, and only if the power check
 * above passed:
 *
 *   link0..link7  CSIS link n version (0x1a440000 + n * 0x10000)
 *   wdma          CSIS WDMA context 0 interrupt mask (0x1a4d7404)
 *   phybias       DC-PHY shared bias control 0 (0x1a4f1000); also needs
 *                 the PHY isolation released
 *
 * The addresses come from Google's GPL gs201 device tree (the lwis CSIS,
 * DMA and PHY nodes) and PHY driver. Every address is logged before it is
 * read, so a read that hangs the SoC is the last line in a streamed log.
 * Nothing is ever written. The module stays loaded until removed.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/string.h>

#define PMU_CSIS_STATUS			0x2404
#define PMU_PDP_STATUS			0x2484
#define PMU_PHY_CTRL_DCPHY_S4S4S4S4	0x3ebc
#define PMU_PD_ON			BIT(0)
#define PMU_PHY_ENABLE			BIT(0)

#define CSIS_LINK_BASE			0x1a440000
#define CSIS_LINK_STRIDE		0x10000
#define CSIS_LINK_VERSION		0x0
#define CSIS_DMA_BASE			0x1a4d0000
#define CSIS_WDMA0_INT_MASK		0x7404
#define DCPHY_BASE			0x1a4f0000
#define DCPHY_M_BIAS_CON0		0x1000

static char *step = "pmu";
module_param(step, charp, 0444);
MODULE_PARM_DESC(step, "pmu (default), link0..link7, wdma or phybias");

struct cam_probe_state {
	bool csis_on;
	bool pdp_on;
	bool phy_enabled;
};

static int cam_probe_pmu(struct cam_probe_state *st)
{
	static const struct {
		unsigned int reg;
		const char *name;
	} regs[] = {
		{ PMU_CSIS_STATUS, "CSIS_STATUS" },
		{ PMU_PDP_STATUS, "PDP_STATUS" },
		{ PMU_PHY_CTRL_DCPHY_S4S4S4S4, "PHY_CTRL_MIPI_DCPHY_S4S4S4S4" },
	};
	unsigned int val[ARRAY_SIZE(regs)];
	struct regmap *pmu;
	int i, ret;

	pmu = exynos_get_pmu_regmap();
	if (IS_ERR(pmu)) {
		pr_err("no PMU regmap: %pe\n", pmu);
		return PTR_ERR(pmu);
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		pr_info("reading PMU 0x%04x (%s)\n", regs[i].reg, regs[i].name);
		ret = regmap_read(pmu, regs[i].reg, &val[i]);
		if (ret) {
			pr_err("PMU 0x%04x read failed: %d\n", regs[i].reg, ret);
			return ret;
		}
		pr_info("PMU 0x%04x %s = 0x%08x\n", regs[i].reg, regs[i].name,
			val[i]);
	}

	st->csis_on = val[0] & PMU_PD_ON;
	st->pdp_on = val[1] & PMU_PD_ON;
	st->phy_enabled = val[2] & PMU_PHY_ENABLE;

	pr_info("pd-csis %s, pd-pdp %s, DC-PHY %s\n",
		st->csis_on ? "on" : "OFF", st->pdp_on ? "on" : "OFF",
		st->phy_enabled ? "enabled" : "isolated");

	return 0;
}

static int cam_probe_read_one(phys_addr_t addr, const char *what)
{
	void __iomem *base;
	u32 val;

	base = ioremap(addr, sizeof(u32));
	if (!base)
		return -ENOMEM;

	pr_info("reading 0x%08llx (%s)\n", (unsigned long long)addr, what);
	val = readl(base);
	pr_info("0x%08llx %s = 0x%08x\n", (unsigned long long)addr, what, val);

	iounmap(base);

	return 0;
}

static int __init cam_probe_init(void)
{
	struct cam_probe_state st = { };
	unsigned int link;
	char name[32];
	int ret;

	ret = cam_probe_pmu(&st);
	if (ret)
		return ret;

	if (!strcmp(step, "pmu"))
		return 0;

	if (!st.csis_on || !st.pdp_on) {
		pr_err("step %s refused: a camera power domain is off\n", step);
		return 0;
	}

	if (sscanf(step, "link%u", &link) == 1 && link < 8) {
		snprintf(name, sizeof(name), "CSIS link %u version", link);
		return cam_probe_read_one(CSIS_LINK_BASE + link * CSIS_LINK_STRIDE +
					  CSIS_LINK_VERSION, name);
	}

	if (!strcmp(step, "wdma"))
		return cam_probe_read_one(CSIS_DMA_BASE + CSIS_WDMA0_INT_MASK,
					  "CSIS WDMA0 interrupt mask");

	if (!strcmp(step, "phybias")) {
		if (!st.phy_enabled) {
			pr_err("step phybias refused: the DC-PHY is isolated\n");
			return 0;
		}
		return cam_probe_read_one(DCPHY_BASE + DCPHY_M_BIAS_CON0,
					  "DC-PHY M_BIAS_CON0");
	}

	pr_err("unknown step '%s'\n", step);
	return -EINVAL;
}
module_init(cam_probe_init);

static void __exit cam_probe_exit(void)
{
}
module_exit(cam_probe_exit);

MODULE_DESCRIPTION("gs201 camera receiver read-only probe (bring-up)");
MODULE_LICENSE("GPL");
