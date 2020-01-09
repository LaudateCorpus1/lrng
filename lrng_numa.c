// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG NUMA support
 *
 * Copyright (C) 2016 - 2020, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/lrng.h>
#include <linux/slab.h>

#include "lrng_internal.h"

static struct lrng_drng **lrng_drng __read_mostly = NULL;

struct lrng_drng **lrng_drng_instances(void)
{
	return lrng_drng;
}

/* Allocate the data structures for the per-NUMA node DRNGs */
static void _lrng_drngs_numa_alloc(struct work_struct *work)
{
	struct lrng_drng **drngs;
	struct lrng_drng *lrng_drng_init = lrng_drng_init_instance();
	u32 node;
	bool init_drng_used = false;

	mutex_lock(&lrng_crypto_cb_update);

	/* per-NUMA-node DRNGs are already present */
	if (lrng_drng)
		goto unlock;

	drngs = kcalloc(nr_node_ids, sizeof(void *), GFP_KERNEL|__GFP_NOFAIL);
	for_each_online_node(node) {
		struct lrng_drng *drng;

		if (!init_drng_used) {
			drngs[node] = lrng_drng_init;
			init_drng_used = true;
			continue;
		}

		drng = kmalloc_node(sizeof(struct lrng_drng),
				     GFP_KERNEL|__GFP_NOFAIL, node);
		memset(drng, 0, sizeof(lrng_drng));

		drng->crypto_cb = lrng_drng_init->crypto_cb;
		drng->drng = drng->crypto_cb->lrng_drng_alloc(
					LRNG_DRNG_SECURITY_STRENGTH_BYTES);
		if (IS_ERR(drng->drng)) {
			kfree(drng);
			goto err;
		}

		mutex_init(&drng->lock);
		spin_lock_init(&drng->spin_lock);

		/*
		 * No reseeding of NUMA DRNGs from previous DRNGs as this
		 * would complicate the code. Let it simply reseed.
		 */
		lrng_drng_reset(drng);
		drngs[node] = drng;

		lrng_pool_inc_numa_node();
		pr_info("DRNG for NUMA node %d allocated\n", node);
	}

	/* Ensure that all NUMA nodes receive changed memory here. */
	mb();

	if (!cmpxchg(&lrng_drng, NULL, drngs))
		goto unlock;

err:
	for_each_online_node(node) {
		struct lrng_drng *drng = drngs[node];

		if (drng == lrng_drng_init)
			continue;

		if (drng) {
			drng->crypto_cb->lrng_drng_dealloc(drng->drng);
			kfree(drng);
		}
	}
	kfree(drngs);

unlock:
	mutex_unlock(&lrng_crypto_cb_update);
}

static DECLARE_WORK(lrng_drngs_numa_alloc_work, _lrng_drngs_numa_alloc);

void lrng_drngs_numa_alloc(void)
{
	schedule_work(&lrng_drngs_numa_alloc_work);
}