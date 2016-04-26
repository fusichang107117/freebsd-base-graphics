/*-
 * Copyright (c) 2009 Robert C. Noland III <rnoland@FreeBSD.org>
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/** @file drm_scatter.c
 * Allocation of memory for scatter-gather mappings by the graphics chip.
 * The memory allocated here is then made into an aperture in the card
 * by mapping the pages into the GART.
 */

#include <drm/drmP.h>
#include "drm_legacy.h"

#define DEBUG_SCATTER 0

static inline vm_offset_t drm_vmalloc_dma(vm_size_t size)
{
	return kmem_alloc_attr(kernel_arena, size, M_NOWAIT | M_ZERO,
	    0, BUS_SPACE_MAXADDR_32BIT, VM_MEMATTR_WRITE_COMBINING);
}

static void drm_sg_cleanup(struct drm_sg_mem * entry)
{
	if (entry == NULL)
		return;

	if (entry->virtual != 0)
		kmem_free(kernel_arena, (vm_offset_t)entry->virtual, IDX_TO_OFF(entry->pages));

	kfree(entry->busaddr);
	kfree(entry);
}

void drm_legacy_sg_cleanup(struct drm_device *dev)
{
	if (drm_core_check_feature(dev, DRIVER_SG) && dev->sg &&
	    !drm_core_check_feature(dev, DRIVER_MODESET)) {
		drm_sg_cleanup(dev->sg);
		dev->sg = NULL;
	}
}
#ifdef _LP64
# define ScatterHandle(x) (unsigned int)((x >> 32) + (x & ((1L << 32) - 1)))
#else
# define ScatterHandle(x) (unsigned int)(x)
#endif


int drm_legacy_sg_alloc(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_scatter_gather *request;
	struct drm_sg_mem *entry;
	vm_size_t size;
	vm_pindex_t pindex;

	DRM_DEBUG("\n");

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_SG))
		return -EINVAL;

	if (dev->sg)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	request = data;
	DRM_DEBUG("request size=%ld\n", request->size);

	size = round_page(request->size);
	entry->pages = OFF_TO_IDX(size);
	entry->busaddr = kcalloc(entry->pages, sizeof(*entry->busaddr), GFP_KERNEL);
	if (!entry->busaddr) {
		kfree(entry);
		return -ENOMEM;
	}

	entry->virtual = drm_vmalloc_dma(size);
	if (entry->virtual == 0) {
		kfree(entry->busaddr);
		kfree(entry);
		return -ENOMEM;
	}

	entry->handle = ScatterHandle((unsigned long)entry->virtual);
	for (pindex = 0; pindex < entry->pages; pindex++) {
		entry->busaddr[pindex] =
		    vtophys(entry->virtual + IDX_TO_OFF(pindex));
	}

	request->handle = entry->handle;

	dev->sg = entry;

	DRM_DEBUG("allocated %ju pages @ %p, contents=%08lx\n",
	    entry->pages, entry->virtual, *(unsigned long *)entry->virtual);

	return 0;
}

int drm_legacy_sg_free(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_scatter_gather *request = data;
	struct drm_sg_mem *entry;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_SG))
		return -EINVAL;

	entry = dev->sg;
	dev->sg = NULL;

	if (!entry || entry->handle != request->handle)
		return -EINVAL;

	DRM_DEBUG("free %p\n", entry->virtual);

	drm_sg_cleanup(entry);

	return 0;
}
