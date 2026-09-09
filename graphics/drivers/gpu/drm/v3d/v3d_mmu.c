// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2017-2018 Broadcom */

/**
 * DOC: Broadcom V3D MMU
 *
 * The V3D 3.x hardware (compared to VC4) now includes an MMU. It has
 * a single level of page tables for the V3D's 4GB address space to
 * map to AXI bus addresses, thus it could need up to 4MB of
 * physically contiguous memory to store the PTEs.
 *
 * Because the 4MB of contiguous memory for page tables is precious,
 * and switching between them is expensive, we load all BOs into the
 * same 4GB address space.
 *
 * To protect clients from each other, we should use the GMP to
 * quickly mask out (at 128kb granularity) what pages are available to
 * each client. This is not yet implemented.
 */

#include "v3d_drv.h"
#include "v3d_regs.h"

/* Note: All PTEs for the 64KB bigpage or 1MB superpage must be filled
 * with the bigpage/superpage bit set.
 */
#define V3D_PTE_SUPERPAGE BIT(31)
#define V3D_PTE_BIGPAGE BIT(30)
#define V3D_PTE_WRITEABLE BIT(29)
#define V3D_PTE_VALID BIT(28)

static bool v3d_mmu_is_aligned(u32 page, u32 page_address, size_t alignment)
{
	return IS_ALIGNED(page, alignment >> V3D_MMU_PAGE_SHIFT) &&
		IS_ALIGNED(page_address, alignment >> V3D_MMU_PAGE_SHIFT);
}

int v3d_mmu_flush_all(struct v3d_dev *v3d)
{
	int ret;

	V3D_WRITE(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_FLUSH |
		  V3D_MMUC_CONTROL_ENABLE);

	ret = wait_for(!(V3D_READ(V3D_MMUC_CONTROL) &
			 V3D_MMUC_CONTROL_FLUSHING), 100);
	if (ret) {
		dev_err(v3d->drm.dev, "MMUC flush wait idle failed\n");
		return ret;
	}

	V3D_WRITE(V3D_MMU_CTL, V3D_READ(V3D_MMU_CTL) |
		  V3D_MMU_CTL_TLB_CLEAR);

	ret = wait_for(!(V3D_READ(V3D_MMU_CTL) &
			 V3D_MMU_CTL_TLB_CLEARING), 100);
	if (ret)
		dev_err(v3d->drm.dev, "MMU TLB clear wait idle failed\n");

	return ret;
}

int v3d_mmu_set_page_table(struct v3d_dev *v3d)
{
	V3D_WRITE(V3D_MMU_PT_PA_BASE, v3d->pt_paddr >> V3D_MMU_PAGE_SHIFT);
	V3D_WRITE(V3D_MMU_CTL,
		  V3D_MMU_CTL_ENABLE |
		  V3D_MMU_CTL_PT_INVALID_ENABLE |
		  V3D_MMU_CTL_PT_INVALID_ABORT |
		  V3D_MMU_CTL_PT_INVALID_INT |
		  V3D_MMU_CTL_WRITE_VIOLATION_ABORT |
		  V3D_MMU_CTL_WRITE_VIOLATION_INT |
		  V3D_MMU_CTL_CAP_EXCEEDED_ABORT |
		  V3D_MMU_CTL_CAP_EXCEEDED_INT);
	V3D_WRITE(V3D_MMU_ILLEGAL_ADDR,
		  (v3d->mmu_scratch_paddr >> V3D_MMU_PAGE_SHIFT) |
		  V3D_MMU_ILLEGAL_ADDR_ENABLE);
	V3D_WRITE(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);

	return v3d_mmu_flush_all(v3d);
}

void v3d_mmu_insert_ptes(struct v3d_bo *bo)
{
	struct drm_gem_shmem_object *shmem_obj = &bo->base;
	struct v3d_dev *v3d = to_v3d_dev(shmem_obj->base.dev);
	u32 page = bo->node.start;
	struct scatterlist *sgl;
	unsigned int count;
	/*
	 * DEVIATION from the vendored source (nextbsd-kernel-extensions#72):
	 * temporary diagnostic + overrun guard, to be reverted once the cause
	 * is understood.
	 *
	 * The WARN at the end of this function fires on a Pi 500+ and the box
	 * then hard-resets with no panic string and no dump. Measured: a bare
	 * CREATE_BO (no GPU work at all) survives at 4K and 8K and resets at
	 * 16K, which puts the fault in this function rather than in submit.
	 *
	 * `page` advances by sum(sg_dma_len) over sgt->nents entries. If nents
	 * overstates the real chain, this loop writes past the end of v3d->pt
	 * and corrupts kernel memory -- which is consistent with a silent
	 * reset. So: report the inputs BEFORE looping (so the numbers are out
	 * even if we die), and clamp the write so it physically cannot leave
	 * the page table. The clamp is a guard to keep the machine alive long
	 * enough to be diagnosed, not a fix.
	 */
	u32 dbg_max_pages = shmem_obj->base.size >> V3D_MMU_PAGE_SHIFT;
	unsigned int dbg_nents = shmem_obj->sgt ? shmem_obj->sgt->nents : 0;
	unsigned int dbg_orig_nents =
	    shmem_obj->sgt ? shmem_obj->sgt->orig_nents : 0;
	bool dbg_overrun = false;

	/* 4K and 8K are known good; gate the entry trace so a Mesa run does not
	 * flood the console with one line per BO. */
	if (shmem_obj->base.size > 8192)
		dev_err(v3d->drm.dev,
		"v3d_mmu: enter size=%llu expect_pages=%u nents=%u orig_nents=%u start=%u\n",
		(unsigned long long)shmem_obj->base.size, dbg_max_pages,
		dbg_nents, dbg_orig_nents, (unsigned int)bo->node.start);

	for_each_sgtable_dma_sg(shmem_obj->sgt, sgl, count) {
		dma_addr_t dma_addr = sg_dma_address(sgl);
		u32 pfn = dma_addr >> V3D_MMU_PAGE_SHIFT;
		unsigned int len = sg_dma_len(sgl);

		while (len > 0) {
			u32 page_prot = V3D_PTE_WRITEABLE | V3D_PTE_VALID;
			u32 page_address = page_prot | pfn;
			unsigned int i, page_size;

			BUG_ON(pfn + V3D_PAGE_FACTOR >= BIT(24));

			if (len >= SZ_1M &&
			    v3d_mmu_is_aligned(page, page_address, SZ_1M)) {
				page_size = SZ_1M;
				page_address |= V3D_PTE_SUPERPAGE;
			} else if (len >= SZ_64K &&
				   v3d_mmu_is_aligned(page, page_address, SZ_64K)) {
				page_size = SZ_64K;
				page_address |= V3D_PTE_BIGPAGE;
			} else {
				page_size = SZ_4K;
			}

			for (i = 0; i < page_size >> V3D_MMU_PAGE_SHIFT; i++) {
				/* DIAG guard: never write outside the BO's slot */
				if (page - bo->node.start >= dbg_max_pages) {
					dbg_overrun = true;
					break;
				}
				v3d->pt[page++] = page_address + i;
				pfn++;
			}

			if (dbg_overrun)
				break;

			len -= page_size;
		}

		if (dbg_overrun)
			break;
	}

	if (dbg_overrun)
		dev_err(v3d->drm.dev,
			"v3d_mmu: OVERRUN clamped: would have written past %u pages (size=%llu nents=%u orig_nents=%u)\n",
			dbg_max_pages, (unsigned long long)shmem_obj->base.size,
			dbg_nents, dbg_orig_nents);
	else if (page - bo->node.start != dbg_max_pages)
		dev_err(v3d->drm.dev,
			"v3d_mmu: SHORT: wrote %u pages, expected %u (size=%llu nents=%u orig_nents=%u)\n",
			(unsigned int)(page - bo->node.start), dbg_max_pages,
			(unsigned long long)shmem_obj->base.size,
			dbg_nents, dbg_orig_nents);

	WARN_ON_ONCE(page - bo->node.start !=
		     shmem_obj->base.size >> V3D_MMU_PAGE_SHIFT);

	if (v3d_mmu_flush_all(v3d))
		dev_err(v3d->drm.dev, "MMU flush timeout\n");
}

void v3d_mmu_remove_ptes(struct v3d_bo *bo)
{
	struct v3d_dev *v3d = to_v3d_dev(bo->base.base.dev);
	u32 npages = bo->base.base.size >> V3D_MMU_PAGE_SHIFT;
	u32 page;

	for (page = bo->node.start; page < bo->node.start + npages; page++)
		v3d->pt[page] = 0;

	if (v3d_mmu_flush_all(v3d))
		dev_err(v3d->drm.dev, "MMU flush timeout\n");
}
