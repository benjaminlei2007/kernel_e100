/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * Port from i2c-octeon.c
 * (C) Copyright 2009-2010
 * Nokia Siemens Networks, michael.lawnick.ext@nsn.com
 *
 * Portions Copyright (C) 2010, 2011 Cavium Networks, Inc.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/of.h>

#define DRV_NAME "i2c-thunderx"

/* The previous out-of-tree version was implicitly version 1.0. */
#define DRV_VERSION	"1.0"

/* register offsets */
#define SW_TWSI			0x1000
#define TWSI_INT		0x1010
#define TWSI_INT_ENA_W1C	0x1028
#define TWSI_INT_ENA_W1S	0x1030

/* Controller command patterns */
#define SW_TWSI_V               0x8000000000000000ull
#define SW_TWSI_EOP_TWSI_DATA   0x0C00000100000000ull
#define SW_TWSI_EOP_TWSI_CTL    0x0C00000200000000ull
#define SW_TWSI_EOP_TWSI_CLKCTL 0x0C00000300000000ull
#define SW_TWSI_EOP_TWSI_STAT   0x0C00000300000000ull
#define SW_TWSI_EOP_TWSI_RST    0x0C00000700000000ull
#define SW_TWSI_OP_TWSI_CLK     0x0800000000000000ull
#define SW_TWSI_R               0x0100000000000000ull

/* Controller command and status bits */
#define TWSI_CTL_CE   0x80
#define TWSI_CTL_ENAB 0x40
#define TWSI_CTL_STA  0x20
#define TWSI_CTL_STP  0x10
#define TWSI_CTL_IFLG 0x08
#define TWSI_CTL_AAK  0x04

/* Some status values */
#define STAT_START      0x08
#define STAT_RSTART     0x10
#define STAT_TXADDR_ACK 0x18
#define STAT_TXDATA_ACK 0x28
#define STAT_RXADDR_ACK 0x40
#define STAT_RXDATA_ACK 0x50
#define STAT_IDLE       0xF8

/* Int ena value */
#define INT_ENA_CORE	0x4

#define PCI_CFG_REG_BAR_NUM	0
#define PCI_DEVICE_ID_THUNDER_TWSI	0xa012

#define TWSI_DFL_RATE		100000

#define REF_CLOCK_RATE          50000000ul

/* ThunderX RST reg */
#define RST_BASE                0x87e006000000ull
#define RST_SIZE                0x2000
#define RST_BOOT                0x1600
#define RST_PNR_MUL_MASK        0x007e00000000ull
#define RST_PNR_MUL_SHIFT       33

struct thunderx_i2c {
	wait_queue_head_t queue;
	struct i2c_adapter adap;
	u32 twsi_freq;
	int sys_freq;
	void __iomem *twsi_base;
	struct pci_dev *pdev;

	/* MSI-X */
	u8 msix_enabled;
	struct	msix_entry msix_entry;	/* 1 for twsi */
	bool irq_allocated;
};

static const struct pci_device_id thunderx_i2c_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_TWSI) },
	{ 0, }  /* end of table */
};

MODULE_AUTHOR("Cavium Inc");
MODULE_DESCRIPTION("Cavium ThunderX I2C Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DEVICE_TABLE(pci, thunderx_i2c_id_table);


/**
 * thunderx_i2c_write_sw - write an I2C core register.
 * @i2c: The struct thunderx_i2c.
 * @eop_reg: Register selector.
 * @data: Value to be written.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static void thunderx_i2c_write_sw(struct thunderx_i2c *i2c,
				u64 eop_reg,
				u8 data)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | data, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);
}

/**
 * thunderx_i2c_read_sw - write an I2C core register.
 * @i2c: The struct thunderx_i2c.
 * @eop_reg: Register selector.
 *
 * Returns the data.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static u8 thunderx_i2c_read_sw(struct thunderx_i2c *i2c, u64 eop_reg)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | SW_TWSI_R, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);

	return tmp & 0xFF;
}

/**
 * thunderx_i2c_write_int - write the TWSI_INT register
 * @i2c: The struct thunderx_i2c.
 * @data: Value to be written.
 */
static void thunderx_i2c_write_int(struct thunderx_i2c *i2c, u64 data)
{
	__raw_writeq(data, i2c->twsi_base + TWSI_INT);
	__raw_readq(i2c->twsi_base + TWSI_INT);
}

/**
 * thunderx_i2c_int_enable - enable the TS interrupt.
 * @i2c: The struct thunderx_i2c.
 *
 * The interrupt will be asserted when there is non-STAT_IDLE state in
 * the SW_TWSI_EOP_TWSI_STAT register.
 */
static void thunderx_i2c_int_enable(struct thunderx_i2c *i2c)
{
	__raw_writeq(INT_ENA_CORE, i2c->twsi_base + TWSI_INT_ENA_W1S);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1S);
}

/**
 * thunderx_i2c_int_disable - disable the TS interrupt.
 * @i2c: The struct thunderx_i2c.
 */
static void thunderx_i2c_int_disable(struct thunderx_i2c *i2c)
{
	__raw_writeq(INT_ENA_CORE, i2c->twsi_base + TWSI_INT_ENA_W1C);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1C);
}

/**
 * thunderx_i2c_unblock - unblock the bus.
 * @i2c: The struct thunderx_i2c.
 *
 * If there was a reset while a device was driving 0 to bus,
 * bus is blocked. We toggle it free manually by some clock
 * cycles and send a stop.
 */
static void thunderx_i2c_unblock(struct thunderx_i2c *i2c)
{
	int i;

	dev_dbg(&i2c->pdev->dev, "%s\n", __func__);
	for (i = 0; i < 9; i++) {
		thunderx_i2c_write_int(i2c, 0x0);
		udelay(5);
		thunderx_i2c_write_int(i2c, 0x200);
		udelay(5);
	}
	thunderx_i2c_write_int(i2c, 0x300);
	udelay(5);
	thunderx_i2c_write_int(i2c, 0x100);
	udelay(5);
	thunderx_i2c_write_int(i2c, 0x0);
}

/**
 * thunderx_i2c_isr - the interrupt service routine.
 * @int: The irq, unused.
 * @dev_id: Our struct thunderx_i2c.
 */
static irqreturn_t thunderx_i2c_isr(int irq, void *dev_id)
{
	struct thunderx_i2c *i2c = dev_id;

	thunderx_i2c_int_disable(i2c);
	wake_up(&i2c->queue);

	return IRQ_HANDLED;
}


static int thunderx_i2c_test_iflg(struct thunderx_i2c *i2c)
{
	return (thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_CTL) & TWSI_CTL_IFLG) != 0;
}

/**
 * thunderx_i2c_wait - wait for the IFLG to be set.
 * @i2c: The struct thunderx_i2c.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int thunderx_i2c_wait(struct thunderx_i2c *i2c)
{
	int result;

	thunderx_i2c_int_enable(i2c);

	result = wait_event_timeout(i2c->queue,
					thunderx_i2c_test_iflg(i2c),
					i2c->adap.timeout);

	thunderx_i2c_int_disable(i2c);

	if (result < 0) {
		dev_dbg(&i2c->pdev->dev, "%s: wait interrupted\n", __func__);
		return result;
	} else if (result == 0) {
		dev_dbg(&i2c->pdev->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * thunderx_i2c_start - send START to the bus.
 * @i2c: The struct thunderx_i2c.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int thunderx_i2c_start(struct thunderx_i2c *i2c)
{
	u8 data;
	int result;

	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
				TWSI_CTL_ENAB | TWSI_CTL_STA);

	result = thunderx_i2c_wait(i2c);
	if (result) {
		if (thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT) == STAT_IDLE) {
			/*
			 * Controller refused to send start flag May
			 * be a client is holding SDA low - let's try
			 * to free it.
			 */
			thunderx_i2c_unblock(i2c);
			thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
					    TWSI_CTL_ENAB | TWSI_CTL_STA);

			result = thunderx_i2c_wait(i2c);
		}
		if (result)
			return result;
	}

	data = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
	if ((data != STAT_START) && (data != STAT_RSTART)) {
		dev_err(&i2c->pdev->dev, "%s: bad status (0x%x)\n", __func__, data);
		return -EIO;
	}

	return 0;
}

/**
 * thunderx_i2c_stop - send STOP to the bus.
 * @i2c: The struct thunderx_i2c.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int thunderx_i2c_stop(struct thunderx_i2c *i2c)
{
	u8 data;

	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
			    TWSI_CTL_ENAB | TWSI_CTL_STP);

	data = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);

	if (data != STAT_IDLE) {
		dev_err(&i2c->pdev->dev, "%s: bad status(0x%x)\n", __func__, data);
		return -EIO;
	}
	return 0;
}

/**
 * thunderx_i2c_write - send data to the bus.
 * @i2c: The struct thunderx_i2c.
 * @target: Target address.
 * @data: Pointer to the data to be sent.
 * @length: Length of the data.
 *
 * The address is sent over the bus, then the data.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int thunderx_i2c_write(struct thunderx_i2c *i2c, int target,
			    const u8 *data, int length)
{
	int i, result;
	u8 tmp;

	result = thunderx_i2c_start(i2c);
	if (result)
		return result;

	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, target << 1);
	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

	result = thunderx_i2c_wait(i2c);
	if (result)
		return result;

	for (i = 0; i < length; i++) {
		tmp = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
		if ((tmp != STAT_TXADDR_ACK) && (tmp != STAT_TXDATA_ACK)) {
			dev_err(&i2c->pdev->dev,
				"%s: bad status before write (0x%x)\n",
				__func__, tmp);
			return -EIO;
		}

		thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, data[i]);
		thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

		result = thunderx_i2c_wait(i2c);
		if (result)
			return result;
	}

	return 0;
}

/**
 * thunderx_i2c_read - receive data from the bus.
 * @i2c: The struct thunderx_i2c.
 * @target: Target address.
 * @data: Pointer to the location to store the datae .
 * @length: Length of the data.
 *
 * The address is sent over the bus, then the data is read.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int thunderx_i2c_read(struct thunderx_i2c *i2c, int target,
			   u8 *data, int length)
{
	int i, result;
	u8 tmp;

	if (length < 1)
		return -EINVAL;

	result = thunderx_i2c_start(i2c);
	if (result)
		return result;

	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, (target<<1) | 1);
	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

	result = thunderx_i2c_wait(i2c);
	if (result)
		return result;

	for (i = 0; i < length; i++) {
		tmp = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
		if ((tmp != STAT_RXDATA_ACK) && (tmp != STAT_RXADDR_ACK)) {
			dev_err(&i2c->pdev->dev,
				"%s: bad status before read (0x%x)\n",
				__func__, tmp);
			return -EIO;
		}

		if (i+1 < length)
			thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
						TWSI_CTL_ENAB | TWSI_CTL_AAK);
		else
			thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
						TWSI_CTL_ENAB);

		result = thunderx_i2c_wait(i2c);
		if (result)
			return result;

		data[i] = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_DATA);
	}
	return 0;
}

/**
 * thunderx_i2c_xfer - The driver's master_xfer function.
 * @adap: Pointer to the i2c_adapter structure.
 * @msgs: Pointer to the messages to be processed.
 * @num: Length of the MSGS array.
 *
 * Returns the number of messages processed, or a negative errno on
 * failure.
 */
static int thunderx_i2c_xfer(struct i2c_adapter *adap,
			   struct i2c_msg *msgs,
			   int num)
{
	struct i2c_msg *pmsg;
	int i;
	int ret = 0;
	struct thunderx_i2c *i2c = i2c_get_adapdata(adap);

	for (i = 0; ret == 0 && i < num; i++) {
		pmsg = &msgs[i];
		dev_dbg(&i2c->pdev->dev,
			"Doing %s %d byte(s) to/from 0x%02x - %d of %d messages\n",
			 pmsg->flags & I2C_M_RD ? "read" : "write",
			 pmsg->len, pmsg->addr, i + 1, num);
		if (pmsg->flags & I2C_M_RD)
			ret = thunderx_i2c_read(i2c, pmsg->addr, pmsg->buf,
						pmsg->len);
		else
			ret = thunderx_i2c_write(i2c, pmsg->addr, pmsg->buf,
						pmsg->len);
	}
	thunderx_i2c_stop(i2c);

	return (ret != 0) ? ret : num;
}

static u32 thunderx_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm thunderx_i2c_algo = {
	.master_xfer = thunderx_i2c_xfer,
	.functionality = thunderx_i2c_functionality,
};

static struct i2c_adapter thunderx_i2c_ops = {
	.owner = THIS_MODULE,
	.name = "ThunderX adapter",
	.algo = &thunderx_i2c_algo,
	.timeout = HZ / 50,
};

/**
 * thunderx_i2c_setclock - Calculate and set clock divisors.
 */
static int thunderx_i2c_setclock(struct thunderx_i2c *i2c)
{
	int tclk, thp_base, inc, thp_idx, mdiv_idx, ndiv_idx, foscl, diff;
	int thp = 0x18, mdiv = 2, ndiv = 0, delta_hz = 1000000;

	for (ndiv_idx = 0; ndiv_idx < 8 && delta_hz != 0; ndiv_idx++) {
		/*
		 * An mdiv value of less than 2 seems to not work well
		 * with ds1337 RTCs, so we constrain it to larger
		 * values.
		 */
		for (mdiv_idx = 15; mdiv_idx >= 2 && delta_hz != 0; mdiv_idx--) {
			/*
			 * For given ndiv and mdiv values check the
			 * two closest thp values.
			 */
			tclk = i2c->twsi_freq * (mdiv_idx + 1) * 10;
			tclk *= (1 << ndiv_idx);
			thp_base = (i2c->sys_freq / (tclk * 2)) - 1;
			for (inc = 0; inc <= 1; inc++) {
				thp_idx = thp_base + inc;
				if (thp_idx < 5 || thp_idx > 0xff)
					continue;

				foscl = i2c->sys_freq / (2 * (thp_idx + 1));
				foscl = foscl / (1 << ndiv_idx);
				foscl = foscl / (mdiv_idx + 1) / 10;
				diff = abs(foscl - i2c->twsi_freq);
				if (diff < delta_hz) {
					delta_hz = diff;
					thp = thp_idx;
					mdiv = mdiv_idx;
					ndiv = ndiv_idx;
				}
			}
		}
	}
	thunderx_i2c_write_sw(i2c, SW_TWSI_OP_TWSI_CLK, thp);
	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CLKCTL, (mdiv << 3) | ndiv);

	return 0;
}

static int thunderx_i2c_initlowlevel(struct thunderx_i2c *i2c)
{
	u8 status;
	int tries;

	/* disable high level controller, enable bus access */
	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

	/* reset controller */
	thunderx_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_RST, 0);

	for (tries = 10; tries; tries--) {
		udelay(1);
		status = thunderx_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
		if (status == STAT_IDLE)
			return 0;
	}
	dev_err(&i2c->pdev->dev, "%s: TWSI_RST failed! (0x%x)\n", __func__, status);
	return -EIO;
}

static u32 get_io_clock_rate(struct device *dev)
{
	void __iomem *rst_base;
	u64 io_clock_scale;
	u32 io_clock_rate = 0;

	rst_base = devm_ioremap(dev, RST_BASE, RST_SIZE);
	if (!rst_base) {
		dev_err(dev, "Cannot map RST, usinging default io clock rate\n");
		goto clk_err;
	}
	io_clock_scale = (__raw_readq(rst_base + RST_BOOT) & RST_PNR_MUL_MASK) >> RST_PNR_MUL_SHIFT;
	iounmap(rst_base);
	io_clock_rate = (u32)io_clock_scale * REF_CLOCK_RATE;
clk_err:
	return io_clock_rate;
}

static int thunderx_i2c_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int result = 0;
	struct device *dev = &pdev->dev;
	struct thunderx_i2c *i2c;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c) {
		dev_err(&pdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}
	i2c->pdev = pdev;

	pci_set_drvdata(pdev, i2c);

	result = pci_enable_device(pdev);
	if (result) {
		dev_err(dev, "Failed to enable PCI device\n");
		goto err_free_i2c;
	}

	result = pci_request_regions(pdev, DRV_NAME);
	if (result) {
		dev_err(dev, "PCI request regions failed 0x%x\n", result);
		goto err_disable_device;
	}

	i2c->twsi_base = pci_ioremap_bar(pdev, PCI_CFG_REG_BAR_NUM);
	if (!i2c->twsi_base) {
		dev_err(dev, "I2C: Cannot map CSR memory space, aborting\n");
		result = -ENOMEM;
		goto err_release_regions;
	}

	/* Enable MSI-X */
	i2c->msix_entry.entry = 0;
	if (pci_enable_msix(pdev, &i2c->msix_entry, 1)) {
		dev_err(dev, "I2C: Unable to enable MSI-X\n");
		goto err_unmap;
	}
	i2c->msix_enabled = 1;
	result = devm_request_irq(dev, i2c->msix_entry.vector, thunderx_i2c_isr,
				  0, DRV_NAME, i2c);
	if (result < 0) {
		dev_err(dev, "failed to attach interrupt\n");
		goto err_msix;
	}
	i2c->irq_allocated = 1;

	i2c->twsi_freq = TWSI_DFL_RATE;
	i2c->sys_freq = get_io_clock_rate(dev);

	init_waitqueue_head(&i2c->queue);

	result = thunderx_i2c_initlowlevel(i2c);
	if (result) {
		dev_err(dev, "init low level failed\n");
		goto err_irq;
	}

	result = thunderx_i2c_setclock(i2c);
	if (result) {
		dev_err(dev, "clock init failed\n");
		goto err_irq;
	}

	i2c->adap = thunderx_i2c_ops;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c_set_adapdata(&i2c->adap, i2c);

	result = i2c_add_adapter(&i2c->adap);
	if (result < 0) {
		dev_err(dev, "failed to add adapter\n");
		goto err_irq;
	}
	dev_info(dev, "version %s\n", DRV_VERSION);

	return 0;

err_irq:
	free_irq(i2c->msix_entry.vector, i2c);
err_msix:
	pci_disable_msix(pdev);
err_unmap:
	iounmap(i2c->twsi_base);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
err_free_i2c:
	kfree(i2c);
	return result;
};

static void thunderx_i2c_remove(struct pci_dev *pdev)
{
	struct thunderx_i2c *i2c = pci_get_drvdata(pdev);

	if (!i2c)
		return;
	i2c_del_adapter(&i2c->adap);
	if (i2c->irq_allocated)
		free_irq(i2c->msix_entry.vector, i2c);
	if (i2c->msix_enabled)
		pci_disable_msix(pdev);
	if (i2c->twsi_base)
		iounmap(i2c->twsi_base);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(i2c);
};


static struct pci_driver thunderx_i2c_driver = {
	.name		= DRV_NAME,
	.id_table	= thunderx_i2c_id_table,
	.probe		= thunderx_i2c_probe,
	.remove		= thunderx_i2c_remove,
};

module_pci_driver(thunderx_i2c_driver)
