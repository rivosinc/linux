// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/dma-direct.h>
#include <linux/swiotlb.h>
#include <linux/cc_platform.h>
#include <linux/mem_encrypt.h>
#include <linux/virtio_anchor.h>
#include <asm/covg_sbi.h>

/* Override for DMA direct allocation check - ARCH_HAS_FORCE_DMA_UNENCRYPTED */
bool force_dma_unencrypted(struct device *dev)
{
	/*
	 * For authorized devices in trusted guest, all DMA must be to/from
	 * unencrypted addresses.
	 */
	return cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT);
}

int set_memory_encrypted(unsigned long addr, int numpages)
{
	int i, rc;

	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return 0;

	if (!PAGE_ALIGNED(addr))
		return -EINVAL;

	rc = sbi_covg_unshare_memory(__pa(addr), numpages * PAGE_SIZE);
	if (rc) {
		rc = 0;
		for (i = 0; i < numpages && rc == 0; i++)
			rc = sbi_covg_unshare_memory(__pa(addr + i * PAGE_SIZE), PAGE_SIZE);
	}
	return rc;
}
EXPORT_SYMBOL_GPL(set_memory_encrypted);

int set_memory_decrypted(unsigned long addr, int numpages)
{
	int i, rc;

	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return 0;

	if (!PAGE_ALIGNED(addr))
		return -EINVAL;

	rc = sbi_covg_share_memory(__pa(addr), numpages * PAGE_SIZE);
	if (rc) {
		rc = 0;
		/* Try page by page if TSM cannot share all pages at once */
		for (i = 0; i < numpages && rc == 0; i++)
			rc = sbi_covg_share_memory(__pa(addr + i * PAGE_SIZE), PAGE_SIZE);
	}
	return rc;
}
EXPORT_SYMBOL_GPL(set_memory_decrypted);

/* Architecture __weak replacement functions */
void __init mem_encrypt_init(void)
{
	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return;

	/* Call into SWIOTLB to update the SWIOTLB DMA buffers */
	swiotlb_update_mem_attributes();

	/* Set restricted memory access for virtio. */
	virtio_set_mem_acc_cb(virtio_require_restricted_mem_acc);
}
