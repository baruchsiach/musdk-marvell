/*
 * User I/O driver for Armada 7K/8K Packet Processor.
 *
 * Copyright (C) 2016, ENEA AB
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uio_driver.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mm.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define DRIVER_NAME	"mv_sam_uio_drv"
#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Marvell"
#define DRIVER_DESC	"UIO platform driver for Security Accelerator"

#define SAM_EIP197_xDR_REGS_OFFS	0x80000
#define SAM_EIP97_xDR_REGS_OFFS		0x0

#define SAM_RING_OFFS(ring)		((ring) * 0x1000)

#define HIA_CDR_BASE_ADDR_LO_REG	0x0

#define HIA_RDR_REGS_OFFSET		0x800
#define HIA_RDR_STAT_REG		(HIA_RDR_REGS_OFFSET + 0x3C)

/* HIA_RDR_y_STAT register */
#define SAM_RDR_STAT_IRQ_OFFS		0
#define SAM_RDR_STAT_IRQ_BITS		8
#define SAM_RDR_STAT_IRQ_MASK		GENMASK(SAM_RDR_STAT_IRQ_OFFS + SAM_RDR_STAT_IRQ_BITS - 1, \
						SAM_RDR_STAT_IRQ_OFFS)

/* HIA Options Register */
#define SAM_EIP197_HIA_OPTIONS_REG	0x9FFF8
#define SAM_EIP97_HIA_OPTIONS_REG	0x0FFF8

#define SAM_N_RINGS_OFFS		0
#define SAM_N_RINGS_BITS		4
#define SAM_N_RINGS_MASK		GENMASK(SAM_N_RINGS_OFFS + SAM_N_RINGS_BITS - 1, SAM_N_RINGS_OFFS)

enum sam_hw_type {
	HW_EIP197,
	HW_EIP97,
	HW_TYPE_LAST
};

struct sam_uio_ring_info {
	int ring;
	char ring_name[32];
	char *xdr_regs_vbase;
	struct uio_info uio;
};

/*
 * sam_uio_pdev_info
 * local information for uio module driver
 *
 * @uio_num:  number of uio devices
 * @dev:      device pointer
 * @info:     uio_info array
 *
 */
struct sam_uio_pdev_info {
	int uio_num;
	struct device *dev;
	struct clk *clk;
	char name[16];
	enum sam_hw_type type;
	char *regs_vbase;
	u32 hw_rings_num;
	u32 rings_map;
	u32 busy_rings_map;
	struct sam_uio_ring_info *rings;
};


static int sam_uio_remove(struct platform_device *pdev);

static inline int sam_uio_get_rings_num(struct sam_uio_pdev_info *pdev_info)
{
	u32 *addr, val32;

	if (pdev_info->type == HW_EIP197)
		addr = (u32 *)(pdev_info->regs_vbase + SAM_EIP197_HIA_OPTIONS_REG);
	else if (pdev_info->type == HW_EIP97)
		addr = (u32 *)(pdev_info->regs_vbase + SAM_EIP97_HIA_OPTIONS_REG);
	else {
		dev_err(pdev_info->dev, "unknown HW type %d\n", pdev_info->type);
		return 0;
	}
	val32 = readl(addr);
	pdev_info->hw_rings_num = val32 & SAM_N_RINGS_MASK;

	return pdev_info->hw_rings_num;
}

static inline int sam_uio_ring_is_busy(struct sam_uio_ring_info *ring_priv)
{
	u32 *addr, val32;

	addr = (u32 *)(ring_priv->xdr_regs_vbase + HIA_CDR_BASE_ADDR_LO_REG);

	val32 = readl(addr);

	return (val32 != 0);
}

static inline void sam_uio_reset_ring(struct sam_uio_ring_info *ring_priv)
{
	u32 *addr;

	addr = (u32 *)(ring_priv->xdr_regs_vbase + HIA_CDR_BASE_ADDR_LO_REG);

	writel(0, addr);
}

static inline void sam_uio_ring_ack_irq(struct sam_uio_ring_info *ring_priv, u32 mask)
{
	u32 *addr, val32;

	addr = (u32 *)(ring_priv->xdr_regs_vbase + HIA_RDR_STAT_REG);

	val32 = mask & SAM_RDR_STAT_IRQ_MASK;

	/* pr_debug("%s: writel: %p = 0x%08x\n", __func__, addr, val32); */
	writel(val32, addr);
}

irqreturn_t sam_ring_irq_handler(int irq, struct uio_info *dev_info)
{
	struct sam_uio_ring_info *ring_priv;

	ring_priv = (struct sam_uio_ring_info *)dev_info->priv;

	/* Ack all interrupts */
	sam_uio_ring_ack_irq(ring_priv, SAM_RDR_STAT_IRQ_MASK);

	return IRQ_HANDLED;
}

/*
 * sam_uio_probe() - mv_pp_uio_drv platform driver probe routine
 * - register uio devices filled with memory maps retrieved from device tree
 */
static int sam_uio_probe(struct platform_device *pdev)
{
	struct platform_device *eip_pdev;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *eip_node;
	struct sam_uio_pdev_info  *pdev_info;
	struct uio_info *uio;
	struct resource *res;
	int rings_num, err = 0;
	char irq_name[6] = {0};
	char *xdr_regs_vbase;
	static u8 cpn_count;

	if (!np) {
		dev_err(dev, "Non DT case is not supported\n");
		return -EINVAL;
	}
	pdev_info = devm_kzalloc(dev, sizeof(struct sam_uio_pdev_info),
				   GFP_KERNEL);
	if (!pdev_info) {
		dev_err(dev, "Can't allocate memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, pdev_info);

	pdev_info->dev = dev;

	eip_node = of_parse_phandle(np, "eip_access", 0);
	if (!eip_node) {
		dev_err(dev, "eip_access node is not found\n");
		err = -EINVAL;
		goto fail_uio;
	}
	eip_pdev = of_find_device_by_node(eip_node);
	if (!eip_pdev) {
		err = -EINVAL;
		dev_err(dev, "Device for node %s is not found\n", eip_node->full_name);
		goto fail_uio;
	}
	/* Enable clock for EIP197/97 device if not enabled by crypto_safexcel.ko */
	pdev_info->clk = of_clk_get(eip_node, 0);
	if (!IS_ERR(pdev_info->clk)) {
		err = clk_prepare_enable(pdev_info->clk);
		if (err) {
			dev_err(dev, "unable to enable clk (%d)\n", err);
			goto fail_uio;
		}
	}

	res = platform_get_resource(eip_pdev, IORESOURCE_MEM, 0);
	if (!res) {
		err = -EINVAL;
		dev_err(dev, "failed to get resource\n");
		goto fail_uio;
	}

	pdev_info->regs_vbase = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(pdev_info->regs_vbase)) {
		err = -EINVAL;
		dev_err(dev, "failed to to ioremap\n");
		goto fail_uio;
	}

	if (!strcmp(eip_node->name, "eip197")) {
		xdr_regs_vbase = pdev_info->regs_vbase + SAM_EIP197_xDR_REGS_OFFS;
		pdev_info->type = HW_EIP197;
	} else if (!strcmp(eip_node->name, "eip97")) {
		xdr_regs_vbase = pdev_info->regs_vbase + SAM_EIP97_xDR_REGS_OFFS;
		pdev_info->type = HW_EIP97;
	} else {
		err = -EINVAL;
		dev_err(dev, "unknown HW type - %s\n", eip_node->name);
		goto fail_uio;
	}

	/* Read number of HW Rings from HIA_OPTIONS register */
	rings_num = sam_uio_get_rings_num(pdev_info);

	pdev_info->rings = devm_kzalloc(dev, rings_num * sizeof(struct sam_uio_ring_info),
					GFP_KERNEL);
	if (!pdev_info->rings) {
		err = -ENOMEM;
		goto fail_uio;
	}

	snprintf(pdev_info->name, sizeof(pdev_info->name), "uio_%s_%d",  eip_node->name, cpn_count);

	for (int idx = 0; idx < rings_num; idx++) {
		struct sam_uio_ring_info *ring_info = &pdev_info->rings[idx];

		uio = &ring_info->uio;

		ring_info->ring = idx;
		ring_info->xdr_regs_vbase = xdr_regs_vbase + SAM_RING_OFFS(ring_info->ring);
		/* Check how many rings already in use */
		if (sam_uio_ring_is_busy(ring_info)) {
			pdev_info->busy_rings_map |= BIT(idx);
			continue;
		}

		snprintf(ring_info->ring_name, sizeof(ring_info->ring_name), "%s:%d",  pdev_info->name, idx);
		uio->name = ring_info->ring_name;
		uio->version = DRIVER_VERSION;

		snprintf(irq_name, sizeof(irq_name), "ring%d", idx);
		uio->irq = platform_get_irq_byname(eip_pdev, irq_name);
		if (uio->irq < 0) {
			dev_warn(dev, "unable to get IRQ number for '%s'\n", irq_name);
		} else {
			pr_debug("%s - irq #%ld\n", irq_name, uio->irq);
			uio->irq_flags = IRQF_SHARED;
			uio->handler = sam_ring_irq_handler;
		}
		uio->priv = (void *)ring_info;

		uio->mem[0].memtype = UIO_MEM_PHYS;
		uio->mem[0].addr = res->start & PAGE_MASK;
		uio->mem[0].size = PAGE_ALIGN(resource_size(res));
		uio->mem[0].name = "regs";

		err = uio_register_device(dev, uio);
		if (err) {
			dev_warn(dev, "Failed to register uio device\n");
			memset(ring_info, 0, sizeof(*ring_info));
			continue;
		}
		pdev_info->rings_map |= BIT(idx);
		pdev_info->uio_num++;
	}
	dev_info(dev, "Registered %d uio devices, Rings: free = 0x%x, busy = 0x%x\n",
		pdev_info->uio_num, pdev_info->rings_map, pdev_info->busy_rings_map);

	cpn_count++;
	return 0;

fail_uio:
	sam_uio_remove(pdev);

	return err;
}

/*
 * sam_uio_remove() - SAM UIO platform driver release routine
 * - unregister uio devices
 */
static int sam_uio_remove(struct platform_device *pdev)
{
	struct sam_uio_pdev_info *pdev_info = platform_get_drvdata(pdev);

	if (!pdev_info)
		return -EINVAL;

	if (pdev_info->uio_num != 0) {
		for (int idx = 0; idx < pdev_info->hw_rings_num; idx++) {
			if (pdev_info->rings_map & BIT(idx)) {
				sam_uio_reset_ring(&pdev_info->rings[idx]);
				uio_unregister_device(&pdev_info->rings[idx].uio);
			}
		}
	}
	if (pdev_info->rings)
		devm_kfree(&pdev->dev, pdev_info->rings);

	if (pdev_info->clk)
		clk_disable_unprepare(pdev_info->clk);

	devm_kfree(&pdev->dev, pdev_info);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id sam_uio_of_match[] = {
	{ .compatible	= "marvell,uio-sam", },
	{ }
};

static struct platform_driver sam_uio_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= DRIVER_NAME,
		.of_match_table	= sam_uio_of_match,
	},
	.probe	= sam_uio_probe,
	.remove	= sam_uio_remove,
};

/* *INDENT-ON* */

module_platform_driver(sam_uio_driver);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_ALIAS("platform:" DRIVER_NAME);
