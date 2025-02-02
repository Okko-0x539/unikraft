/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2022, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/essentials.h>
#include <uk/arch/limits.h>
#include <uk/arch/types.h>
#include <uk/arch/paging.h>
#include <uk/plat/bootstrap.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/plat/common/lcpu.h>
#include <uk/plat/common/memory.h>
#include <uk/plat/common/sections.h>
#include <uk/reloc.h>
#include <vmware-x86/multiboot.h>

#include <errno.h>
#include <string.h>

#define multiboot_crash(msg, rc)	ukplat_crash()

void _ukplat_entry(struct lcpu *lcpu, struct ukplat_bootinfo *bi);

static inline void mrd_insert(struct ukplat_bootinfo *bi,
			      const struct ukplat_memregion_desc *mrd)
{
	int rc;

	if (unlikely(mrd->len == 0))
		return;

	rc = ukplat_memregion_list_insert(&bi->mrds, mrd);
	if (unlikely(rc < 0))
		multiboot_crash("Cannot insert bootinfo memory region", rc);
}

/**
 * Multiboot entry point called after lcpu initialization. We enter with the
 * 1:1 boot page table set. Physical and virtual addresses thus match for all
 * regions in the mapped range.
 */
void multiboot_entry(struct lcpu *lcpu, struct multiboot_info *mi)
{
	struct ukplat_bootinfo *bi;
	struct ukplat_memregion_desc mrd = {0};
	multiboot_memory_map_t *m;
	multiboot_module_t *mods;
	__sz offset;
	__paddr_t start, end;
	__u32 i;
	int rc;

	bi = ukplat_bootinfo_get();
	if (unlikely(!bi))
		multiboot_crash("Incompatible or corrupted bootinfo", -EINVAL);

	/* We have to call this here as the very early do_uk_reloc32 relocator
	* does not also relocate the UKPLAT_MEMRT_KERNEL mrd's like its C
	* equivalent, do_uk_reloc, does.
	*/
	do_uk_reloc_kmrds(0, 0);
	/* Ensure that the memory map contains the legacy high mem area */
	rc = ukplat_memregion_list_insert_legacy_hi_mem(&bi->mrds);
	if (unlikely(rc))
			multiboot_crash("Could not insert legacy memory region", rc);

	if ((mi->flags & MULTIBOOT_INFO_CMDLINE) && mi->cmdline) {
		bi->cmdline = mi->cmdline;
		bi->cmdline_len = strlen((const char *)(__uptr)mi->cmdline);
	}

	/* Copy boot loader */
	if (mi->flags & MULTIBOOT_INFO_BOOT_LOADER_NAME) {
		if (mi->boot_loader_name) {
			strncpy(bi->bootloader,
				(const char *)(__uptr)mi->boot_loader_name,
				sizeof(bi->bootloader) - 1);
		}
	}

	memcpy(bi->bootprotocol, "multiboot", sizeof("multiboot"));

	/* Add modules from the multiboot info to the memory region list */
	if (mi->flags & MULTIBOOT_INFO_MODS) {
		mods = (multiboot_module_t *)(__uptr)mi->mods_addr;
		for (i = 0; i < mi->mods_count; i++) {
			mrd.pbase = PAGE_ALIGN_DOWN(mods[i].mod_start);
			mrd.vbase = mrd.pbase; /* 1:1 mapping */
			mrd.pg_off = mods[i].mod_start - mrd.pbase;
			mrd.len = mods[i].mod_end - mods[i].mod_start;
			mrd.pg_count = PAGE_COUNT(mrd.pg_off + mrd.len);
			mrd.type  = UKPLAT_MEMRT_INITRD;
			mrd.flags = UKPLAT_MEMRF_READ;

#ifdef CONFIG_UKPLAT_MEMRNAME
			strncpy(mrd.name, (char *)(__uptr)mods[i].cmdline,
				sizeof(mrd.name) - 1);
#endif /* CONFIG_UKPLAT_MEMRNAME */

			mrd_insert(bi, &mrd);
		}
	}

#ifdef CONFIG_UKPLAT_MEMRNAME
	memset(mrd.name, 0, sizeof(mrd.name));
#endif /* CONFIG_UKPLAT_MEMRNAME */

	/* Add the map ranges provided by the bootloader
	 * from the multiboot info structure to the memory region list.
	 */
	if (mi->flags & MULTIBOOT_INFO_MEM_MAP) {
		for (offset = 0; offset < mi->mmap_length;
		     offset += m->size + sizeof(m->size)) {
			m = (void *)(__uptr)(mi->mmap_addr + offset);

			start = MAX(m->addr, __PAGE_SIZE);
			end   = m->addr + m->len;
			if (unlikely(end <= start || end - start < PAGE_SIZE))
				continue;

			mrd.pbase = PAGE_ALIGN_DOWN(start);
			mrd.vbase = mrd.pbase; /* 1:1 mapping */
			mrd.pg_off = start - mrd.pbase;
			mrd.len = end - start;
			mrd.pg_count = PAGE_COUNT(mrd.pg_off + mrd.len);

			if (m->type == MULTIBOOT_MEMORY_AVAILABLE) {
				mrd.type  = UKPLAT_MEMRT_FREE;
				mrd.flags = UKPLAT_MEMRF_READ |
					    UKPLAT_MEMRF_WRITE;
				/* Free memory regions have
				 * mrd.len == mrd.pg_count * PAGE_SIZE
				 */
				mrd.len = PAGE_ALIGN_UP(mrd.len + mrd.pg_off);
			} else {
				mrd.type  = UKPLAT_MEMRT_RESERVED;
				mrd.flags = UKPLAT_MEMRF_READ;

				/* We assume that reserved regions cannot
				 * overlap with loaded modules.
				 */
			}

			mrd_insert(bi, &mrd);
		}
	}


	_ukplat_entry(lcpu, bi);
}
