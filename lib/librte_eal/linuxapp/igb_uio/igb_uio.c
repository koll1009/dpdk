/*-
 *
 * Copyright (c) 2010-2012, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * GNU GPL V2: http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/msi.h>
#include <linux/version.h>

/* Some function names changes between 3.2.0 and 3.3.0... */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
#define PCI_LOCK pci_block_user_cfg_access
#define PCI_UNLOCK pci_unblock_user_cfg_access
#else
#define PCI_LOCK pci_cfg_access_lock
#define PCI_UNLOCK pci_cfg_access_unlock
#endif

/**
 * MSI-X related macros, copy from linux/pci_regs.h in kernel 2.6.39,
 * but none of them in kernel 2.6.35.
 */
#ifndef PCI_MSIX_ENTRY_SIZE
#define PCI_MSIX_ENTRY_SIZE             16
#define PCI_MSIX_ENTRY_LOWER_ADDR       0
#define PCI_MSIX_ENTRY_UPPER_ADDR       4
#define PCI_MSIX_ENTRY_DATA             8
#define PCI_MSIX_ENTRY_VECTOR_CTRL      12
#define PCI_MSIX_ENTRY_CTRL_MASKBIT     1
#endif

#define IGBUIO_NUM_MSI_VECTORS 1

/* interrupt mode */
enum igbuio_intr_mode {
	IGBUIO_LEGACY_INTR_MODE = 0,
	IGBUIO_MSI_INTR_MODE,
	IGBUIO_MSIX_INTR_MODE,
	IGBUIO_INTR_MODE_MAX
};

/**
 * A structure describing the private information for a uio device.
 */
struct rte_uio_pci_dev {
	struct uio_info info;
	struct pci_dev *pdev;
	spinlock_t lock; /* spinlock for accessing PCI config space or msix data in multi tasks/isr */
	enum igbuio_intr_mode mode;
	struct msix_entry \
		msix_entries[IGBUIO_NUM_MSI_VECTORS]; /* pointer to the msix vectors to be allocated later */
};

static const enum igbuio_intr_mode igbuio_intr_mode_preferred = IGBUIO_MSIX_INTR_MODE;

/* PCI device id table */
static struct pci_device_id igbuio_pci_ids[] = {
#define RTE_PCI_DEV_ID_DECL(vend, dev) {PCI_DEVICE(vend, dev)},
#include <rte_pci_dev_ids.h>
{ 0, },
};

static inline struct rte_uio_pci_dev *
igbuio_get_uio_pci_dev(struct uio_info *info)
{
	return container_of(info, struct rte_uio_pci_dev, info);
}

/**
 * It masks the msix on/off of generating MSI-X messages.
 */
static int
igbuio_msix_mask_irq(struct msi_desc *desc, int32_t state)
{
	uint32_t mask_bits = desc->masked;
	unsigned offset = desc->msi_attrib.entry_nr * PCI_MSIX_ENTRY_SIZE +
						PCI_MSIX_ENTRY_VECTOR_CTRL;

	if (state != 0)
		mask_bits &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	else
		mask_bits |= PCI_MSIX_ENTRY_CTRL_MASKBIT;

	if (mask_bits != desc->masked) {
		writel(mask_bits, desc->mask_base + offset);
		readl(desc->mask_base);
		desc->masked = mask_bits;
	}

	return 0;
}

/**
 * This function sets/clears the masks for generating LSC interrupts.
 *
 * @param info
 *   The pointer to struct uio_info.
 * @param on
 *   The on/off flag of masking LSC.
 * @return
 *   -On success, zero value.
 *   -On failure, a negative value.
 */
static int
igbuio_set_interrupt_mask(struct rte_uio_pci_dev *udev, int32_t state)
{
	struct pci_dev *pdev = udev->pdev;

	if (udev->mode == IGBUIO_MSIX_INTR_MODE) {
		struct msi_desc *desc;

		list_for_each_entry(desc, &pdev->msi_list, list) {
			igbuio_msix_mask_irq(desc, state);
		}
	}
	else if (udev->mode == IGBUIO_LEGACY_INTR_MODE) {
		uint32_t status;
		uint16_t old, new;

		pci_read_config_dword(pdev, PCI_COMMAND, &status);
		old = status;
		if (state != 0)
			new = old & (~PCI_COMMAND_INTX_DISABLE);
		else
			new = old | PCI_COMMAND_INTX_DISABLE;

		if (old != new)
			pci_write_config_word(pdev, PCI_COMMAND, new);
	}

	return 0;
}

/**
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 *
 * @return
 *  - On success, 0.
 *  - On failure, a negative value.
 */
static int
igbuio_pci_irqcontrol(struct uio_info *info, s32 irq_state)
{
	unsigned long flags;
	struct rte_uio_pci_dev *udev = igbuio_get_uio_pci_dev(info);
	struct pci_dev *pdev = udev->pdev;

	spin_lock_irqsave(&udev->lock, flags);
	PCI_LOCK(pdev);

	igbuio_set_interrupt_mask(udev, irq_state);

	PCI_UNLOCK(pdev);
	spin_unlock_irqrestore(&udev->lock, flags);

	return 0;
}

/**
 * This is interrupt handler which will check if the interrupt is for the right device.
 * If yes, disable it here and will be enable later.
 */
static irqreturn_t
igbuio_pci_irqhandler(int irq, struct uio_info *info)
{
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	struct rte_uio_pci_dev *udev = igbuio_get_uio_pci_dev(info);
	struct pci_dev *pdev = udev->pdev;
	uint32_t cmd_status_dword;
	uint16_t status;

	spin_lock_irqsave(&udev->lock, flags);
	/* block userspace PCI config reads/writes */
	PCI_LOCK(pdev);

	/* for legacy mode, interrupt maybe shared */
	if (udev->mode == IGBUIO_LEGACY_INTR_MODE) {
		pci_read_config_dword(pdev, PCI_COMMAND, &cmd_status_dword);
		status = cmd_status_dword >> 16;
		/* interrupt is not ours, goes to out */
		if (!(status & PCI_STATUS_INTERRUPT))
			goto done;
	}

	igbuio_set_interrupt_mask(udev, 0);
	ret = IRQ_HANDLED;
done:
	/* unblock userspace PCI config reads/writes */
	PCI_UNLOCK(pdev);
	spin_unlock_irqrestore(&udev->lock, flags);
	printk(KERN_INFO "irq 0x%x %s\n", irq, (ret == IRQ_HANDLED) ? "handled" : "not handled");

	return ret;
}

/* Remap pci resources described by bar #pci_bar in uio resource n. */
static int
igbuio_pci_setup_iomem(struct pci_dev *dev, struct uio_info *info,
		       int n, int pci_bar, const char *name)
{
	unsigned long addr, len;
	void *internal_addr;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -1;
	internal_addr = ioremap(addr, len);
	if (internal_addr == NULL)
		return -1;
	info->mem[n].name = name;
	info->mem[n].addr = addr;
	info->mem[n].internal_addr = internal_addr;
	info->mem[n].size = len;
	info->mem[n].memtype = UIO_MEM_PHYS;
	return 0;
}

/* Unmap previously ioremap'd resources */
static void
igbuio_pci_release_iomem(struct uio_info *info)
{
	int i;
	for (i = 0; i < MAX_UIO_MAPS; i++) {
		if (info->mem[i].internal_addr)
			iounmap(info->mem[i].internal_addr);
	}
}

static int __devinit
igbuio_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct rte_uio_pci_dev *udev;

	udev = kzalloc(sizeof(struct rte_uio_pci_dev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	/*
	 * enable device: ask low-level code to enable I/O and
	 * memory
	 */
	if (pci_enable_device(dev)) {
		printk(KERN_ERR "Cannot enable PCI device\n");
		goto fail_free;
	}

	/* XXX should we use 64 bits ? */
	/* set 32-bit DMA mask */
	if (pci_set_dma_mask(dev,(uint64_t)0xffffffff)) {
		printk(KERN_ERR "Cannot set DMA mask\n");
		goto fail_disable;
	}

	/*
	 * reserve device's PCI memory regions for use by this
	 * module
	 */
	if (pci_request_regions(dev, "igb_uio")) {
		printk(KERN_ERR "Cannot request regions\n");
		goto fail_disable;
	}

	/* enable bus mastering on the device */
	pci_set_master(dev);

	/* remap IO memory */
	if (igbuio_pci_setup_iomem(dev, &udev->info, 0, 0, "config"))
		goto fail_release_regions;

	/* fill uio infos */
	udev->info.name = "Intel IGB UIO";
	udev->info.version = "0.1";
	udev->info.handler = igbuio_pci_irqhandler;
	udev->info.irqcontrol = igbuio_pci_irqcontrol;
	udev->info.priv = udev;
	udev->pdev = dev;
	udev->mode = 0; /* set the default value for interrupt mode */
	spin_lock_init(&udev->lock);

	/* check if it need to try msix first */
	if (igbuio_intr_mode_preferred == IGBUIO_MSIX_INTR_MODE) {
		int vector;

		for (vector = 0; vector < IGBUIO_NUM_MSI_VECTORS; vector ++)
			udev->msix_entries[vector].entry = vector;

		if (pci_enable_msix(udev->pdev, udev->msix_entries, IGBUIO_NUM_MSI_VECTORS) == 0) {
			udev->mode = IGBUIO_MSIX_INTR_MODE;
		}
		else {
			pci_disable_msix(udev->pdev);
			printk(KERN_INFO "fail to enable pci msix, or not enough msix entries\n");
		}
	}
	switch (udev->mode) {
	case IGBUIO_MSIX_INTR_MODE:
		udev->info.irq_flags = 0;
		udev->info.irq = udev->msix_entries[0].vector;
		break;
	case IGBUIO_MSI_INTR_MODE:
		break;
	case IGBUIO_LEGACY_INTR_MODE:
		udev->info.irq_flags = IRQF_SHARED;
		udev->info.irq = dev->irq;
		break;
	default:
		break;
	}

	pci_set_drvdata(dev, udev);
	igbuio_pci_irqcontrol(&udev->info, 0);

	/* register uio driver */
	if (uio_register_device(&dev->dev, &udev->info))
		goto fail_release_iomem;

	printk(KERN_INFO "uio device registered with irq %lx\n", udev->info.irq);

	return 0;

fail_release_iomem:
	igbuio_pci_release_iomem(&udev->info);
	if (udev->mode == IGBUIO_MSIX_INTR_MODE)
		pci_disable_msix(udev->pdev);
fail_release_regions:
	pci_release_regions(dev);
fail_disable:
	pci_disable_device(dev);
fail_free:
	kfree(udev);

	return -ENODEV;
}

static void
igbuio_pci_remove(struct pci_dev *dev)
{
	struct uio_info *info = pci_get_drvdata(dev);

	uio_unregister_device(info);
	if (((struct rte_uio_pci_dev *)info->priv)->mode == IGBUIO_MSIX_INTR_MODE)
		pci_disable_msix(dev);
	pci_release_regions(dev);
	pci_disable_device(dev);
	pci_set_drvdata(dev, NULL);
	kfree(info);
}

static struct pci_driver igbuio_pci_driver = {
	.name = "igb_uio",
	.id_table = igbuio_pci_ids,
	.probe = igbuio_pci_probe,
	.remove = igbuio_pci_remove,
};

static int __init
igbuio_pci_init_module(void)
{
	return pci_register_driver(&igbuio_pci_driver);//注册pci driver
}

static void __exit
igbuio_pci_exit_module(void)
{
	pci_unregister_driver(&igbuio_pci_driver);
}

module_init(igbuio_pci_init_module);
module_exit(igbuio_pci_exit_module);

MODULE_DESCRIPTION("UIO driver for Intel IGB PCI cards");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
