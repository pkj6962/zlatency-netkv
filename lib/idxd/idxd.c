/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd.h"

#define ALIGN_4K 0x1000
#define USERSPACE_DRIVER_NAME "user"
#define KERNEL_DRIVER_NAME "kernel"
/*
 * Need to limit how many completions we reap in one poller to avoid starving
 * other threads as callers can submit new operations on the polling thread.
 */
#define MAX_COMPLETIONS_PER_POLL 16

static STAILQ_HEAD(, spdk_idxd_impl) g_idxd_impls = STAILQ_HEAD_INITIALIZER(g_idxd_impls);
static struct spdk_idxd_impl *g_idxd_impl;

/*
 * g_dev_cfg gives us 2 pre-set configurations of DSA to choose from
 * via RPC.
 */
struct device_config *g_dev_cfg = NULL;

/*
 * Pre-built configurations. Variations depend on various factors
 * including how many different types of target latency profiles there
 * are, how many different QOS requirements there might be, etc.
 */
struct device_config g_dev_cfg0 = {
	.config_num = 0,
	.num_groups = 1,
	.total_wqs = 1,
	.total_engines = 4,
};

struct device_config g_dev_cfg1 = {
	.config_num = 1,
	.num_groups = 2,
	.total_wqs = 4,
	.total_engines = 4,
};

static inline void
_submit_to_hw(struct spdk_idxd_io_channel *chan, struct idxd_hw_desc *desc)
{
	movdir64b(chan->portal + chan->portal_offset, desc);
	chan->portal_offset = (chan->portal_offset + chan->idxd->chan_per_device * PORTAL_STRIDE) &
			      PORTAL_MASK;

}

struct spdk_idxd_io_channel *
spdk_idxd_get_channel(struct spdk_idxd_device *idxd)
{
	struct spdk_idxd_io_channel *chan;
	struct idxd_batch *batch;
	int i;

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}

	chan->batch_base = calloc(NUM_BATCHES_PER_CHANNEL, sizeof(struct idxd_batch));
	if (chan->batch_base == NULL) {
		SPDK_ERRLOG("Failed to allocate batch pool\n");
		free(chan);
		return NULL;
	}

	pthread_mutex_lock(&idxd->num_channels_lock);
	if (idxd->num_channels == idxd->chan_per_device) {
		/* too many channels sharing this device */
		pthread_mutex_unlock(&idxd->num_channels_lock);
		free(chan->batch_base);
		free(chan);
		return NULL;
	}

	/* Have each channel start at a different offset. */
	chan->portal_offset = (idxd->num_channels * PORTAL_STRIDE) & PORTAL_MASK;

	idxd->num_channels++;
	pthread_mutex_unlock(&idxd->num_channels_lock);

	chan->idxd = idxd;
	TAILQ_INIT(&chan->batches);
	TAILQ_INIT(&chan->batch_pool);
	TAILQ_INIT(&chan->comp_ctx_oustanding);

	batch = chan->batch_base;
	for (i = 0 ; i < NUM_BATCHES_PER_CHANNEL ; i++) {
		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
		batch++;
	}

	return chan;
}

void
spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	pthread_mutex_lock(&chan->idxd->num_channels_lock);
	assert(chan->idxd->num_channels > 0);
	chan->idxd->num_channels--;
	pthread_mutex_unlock(&chan->idxd->num_channels_lock);

	spdk_free(chan->completions);
	spdk_free(chan->desc);
	spdk_bit_array_free(&chan->ring_slots);
	while ((batch = TAILQ_FIRST(&chan->batch_pool))) {
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
		spdk_free(batch->user_completions);
		spdk_free(batch->user_desc);
	}
	free(chan->batch_base);
	free(chan);
}

/* returns the total max operations for channel. */
int
spdk_idxd_chan_get_max_operations(struct spdk_idxd_io_channel *chan)
{
	return chan->idxd->total_wq_size / chan->idxd->chan_per_device;
}

int
spdk_idxd_configure_chan(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;
	int rc, num_ring_slots;

	/* Round robin the WQ selection for the chan on this IDXD device. */
	chan->idxd->wq_id++;
	if (chan->idxd->wq_id == g_dev_cfg->total_wqs) {
		chan->idxd->wq_id = 0;
	}

	num_ring_slots = chan->idxd->queues[chan->idxd->wq_id].wqcfg.wq_size / chan->idxd->chan_per_device;

	chan->ring_slots = spdk_bit_array_create(num_ring_slots);
	if (chan->ring_slots == NULL) {
		SPDK_ERRLOG("Failed to allocate bit array for ring\n");
		return -ENOMEM;
	}

	/* Store the size of the ring. */
	chan->ring_size = num_ring_slots;

	chan->desc = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_hw_desc),
				  0x40, NULL,
				  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->desc == NULL) {
		SPDK_ERRLOG("Failed to allocate descriptor memory\n");
		rc = -ENOMEM;
		goto err_desc;
	}

	chan->completions = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_comp),
					 0x40, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->completions == NULL) {
		SPDK_ERRLOG("Failed to allocate completion memory\n");
		rc = -ENOMEM;
		goto err_comp;
	}

	/* Populate the batches */
	TAILQ_FOREACH(batch, &chan->batch_pool, link) {
		batch->user_desc = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_hw_desc),
						0x40, NULL,
						SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_desc == NULL) {
			SPDK_ERRLOG("Failed to allocate batch descriptor memory\n");
			rc = -ENOMEM;
			goto err_user_desc_or_comp;
		}

		batch->user_completions = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_comp),
						       0x40, NULL,
						       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_completions == NULL) {
			SPDK_ERRLOG("Failed to allocate user completion memory\n");
			rc = -ENOMEM;
			goto err_user_desc_or_comp;
		}
	}

	chan->portal = chan->idxd->impl->portal_get_addr(chan->idxd);

	return 0;

err_user_desc_or_comp:
	TAILQ_FOREACH(batch, &chan->batch_pool, link) {
		spdk_free(batch->user_desc);
		batch->user_desc = NULL;
		spdk_free(batch->user_completions);
		batch->user_completions = NULL;
	}
	spdk_free(chan->completions);
	chan->completions = NULL;
err_comp:
	spdk_free(chan->desc);
	chan->desc = NULL;
err_desc:
	spdk_bit_array_free(&chan->ring_slots);

	return rc;
}

static inline struct spdk_idxd_impl *
idxd_get_impl_by_name(const char *impl_name)
{
	struct spdk_idxd_impl *impl;

	assert(impl_name != NULL);
	STAILQ_FOREACH(impl, &g_idxd_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {
			return impl;
		}
	}

	return NULL;
}

/* Called via RPC to select a pre-defined configuration. */
void
spdk_idxd_set_config(uint32_t config_num, bool kernel_mode)
{
	if (kernel_mode) {
		g_idxd_impl = idxd_get_impl_by_name(KERNEL_DRIVER_NAME);
	} else {
		g_idxd_impl = idxd_get_impl_by_name(USERSPACE_DRIVER_NAME);
	}

	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("Cannot set the idxd implementation");
		return;
	}

	switch (config_num) {
	case 0:
		g_dev_cfg = &g_dev_cfg0;
		break;
	case 1:
		g_dev_cfg = &g_dev_cfg1;
		break;
	default:
		g_dev_cfg = &g_dev_cfg0;
		SPDK_ERRLOG("Invalid config, using default\n");
		break;
	}

	g_idxd_impl->set_config(g_dev_cfg, config_num);
}

static void
idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	assert(idxd->impl != NULL);

	idxd->impl->destruct(idxd);
}

int
spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb)
{
	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("No idxd impl is selected\n");
		return -1;
	}

	return g_idxd_impl->probe(cb_ctx, attach_cb);
}

void
spdk_idxd_detach(struct spdk_idxd_device *idxd)
{
	idxd_device_destruct(idxd);
}

inline static void
_track_comp(struct spdk_idxd_io_channel *chan, bool batch_op, uint32_t index,
	    struct idxd_comp *comp_ctx, struct idxd_hw_desc *desc, struct idxd_batch *batch)
{
	comp_ctx->desc = desc;
	comp_ctx->index = index;

	/* Only add non-batch completions here.  Batch completions are added when the batch is
	 * submitted.
	 */
	if (batch_op == false) {
		TAILQ_INSERT_TAIL(&chan->comp_ctx_oustanding, comp_ctx, link);
	}
}

inline static int
_vtophys(const void *buf, uint64_t *buf_addr, uint64_t size)
{
	uint64_t updated_size = size;

	*buf_addr = spdk_vtophys(buf, &updated_size);

	if (*buf_addr == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return -EINVAL;
	}

	if (updated_size < size) {
		SPDK_ERRLOG("Error translating size (0x%lx), return size (0x%lx)\n", size, updated_size);
		return -EINVAL;
	}

	return 0;
}

static int
_idxd_prep_command(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		   void *cb_arg, struct idxd_hw_desc **_desc, struct idxd_comp **_comp)
{
	uint32_t index;
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t comp_hw_addr;
	int rc;

	index = spdk_bit_array_find_first_clear(chan->ring_slots, 0);
	if (index == UINT32_MAX) {
		/* ran out of ring slots */
		return -EBUSY;
	}

	spdk_bit_array_set(chan->ring_slots, index);

	desc = *_desc = &chan->desc[index];
	comp = *_comp = &chan->completions[index];

	rc = _vtophys(&comp->hw, &comp_hw_addr, sizeof(struct idxd_hw_comp_record));
	if (rc) {
		spdk_bit_array_clear(chan->ring_slots, index);
		return rc;
	}

	_track_comp(chan, false, index, comp, desc, NULL);

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = comp_hw_addr;
	comp->cb_arg = cb_arg;
	comp->cb_fn = cb_fn;

	return 0;
}

int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
		      uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->src_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

/* Dual-cast copies the same source to two separate destination buffers. */
int
spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan, void *dst1, void *dst2,
			  const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst1_addr, dst2_addr;
	int rc;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst1, &dst1_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst2, &dst2_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_DUALCAST;
	desc->src_addr = src_addr;
	desc->dst_addr = dst1_addr;
	desc->dest2 = dst2_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

int
spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan, void *src1, const void *src2,
			 uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src1_addr, src2_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src1, &src1_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src2, &src2_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPARE;
	desc->src_addr = src1_addr;
	desc->src2_addr = src2_addr;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan, void *dst, uint64_t fill_pattern,
		      uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->pattern = fill_pattern;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

int
spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan, uint32_t *crc_dst, void *src,
			uint32_t seed, uint64_t nbytes,
			spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_CRC32C_GEN;
	desc->dst_addr = 0; /* Per spec, needs to be clear. */
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	comp->crc_dst = crc_dst;

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

int
spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan, void *dst, void *src,
			     uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			     spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COPY_CRC;
	desc->dst_addr = dst_addr;
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	comp->crc_dst = crc_dst;

	/* Submit operation. */
	_submit_to_hw(chan, desc);

	return 0;
}

uint32_t
spdk_idxd_batch_get_max(void)
{
	/* TODO: consider setting this via RPC. */
	return DESC_PER_BATCH;
}

struct idxd_batch *
spdk_idxd_batch_create(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	if (!TAILQ_EMPTY(&chan->batch_pool)) {
		batch = TAILQ_FIRST(&chan->batch_pool);
		batch->index = 0;
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
		TAILQ_INSERT_TAIL(&chan->batches, batch, link);
	} else {
		/* The application needs to handle this. */
		return NULL;
	}

	return batch;
}

static bool
_is_batch_valid(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	bool found = false;
	struct idxd_batch *cur_batch;

	TAILQ_FOREACH(cur_batch, &chan->batches, link) {
		if (cur_batch == batch) {
			found = true;
			break;
		}
	}

	return found;
}

static void
_free_batch(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	SPDK_DEBUGLOG(idxd, "Free batch %p\n", batch);
	TAILQ_REMOVE(&chan->batches, batch, link);
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
}

int
spdk_idxd_batch_cancel(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch)
{
	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to cancel an invalid batch.\n");
		return -EINVAL;
	}

	if (batch->index > 0) {
		SPDK_ERRLOG("Cannot cancel batch, already submitted to HW.\n");
		return -EINVAL;
	}

	_free_batch(batch, chan);

	return 0;
}

static int _idxd_batch_prep_nop(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch);

int
spdk_idxd_batch_submit(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
		       spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t desc_addr;
	int i, rc;

	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to submit an invalid batch.\n");
		return -EINVAL;
	}

	if (batch->index < MIN_USER_DESC_COUNT) {
		/* DSA needs at least MIN_USER_DESC_COUNT for a batch, add a NOP to make it so. */
		if (_idxd_batch_prep_nop(chan, batch)) {
			return -EINVAL;
		}
	}

	/* Add the batch elements completion contexts to the outstanding list to be polled. */
	for (i = 0 ; i < batch->index; i++) {
		TAILQ_INSERT_TAIL(&chan->comp_ctx_oustanding, (struct idxd_comp *)&batch->user_completions[i],
				  link);
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &comp);
	if (rc) {
		goto error;
	}

	rc = _vtophys(batch->user_desc, &desc_addr, batch->index * sizeof(struct idxd_hw_desc));
	if (rc) {
		rc = -EINVAL;
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_BATCH;
	desc->desc_list_addr = desc_addr;
	desc->desc_count = batch->index;
	comp->batch = batch;
	assert(batch->index <= DESC_PER_BATCH);

	/* Submit operation. */
	_submit_to_hw(chan, desc);
	SPDK_DEBUGLOG(idxd, "Submitted batch %p\n", batch);

	return 0;
error:
	for (i = 0 ; i < batch->index; i++) {
		comp = TAILQ_LAST(&chan->comp_ctx_oustanding, comp_head);
		TAILQ_REMOVE(&chan->comp_ctx_oustanding, comp, link);
	}
	return rc;
}

static int
_idxd_prep_batch_cmd(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		     void *cb_arg, struct idxd_batch *batch,
		     struct idxd_hw_desc **_desc, struct idxd_comp **_comp)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;

	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to add to an invalid batch.\n");
		return -EINVAL;
	}

	assert(batch != NULL); /* suppress scan-build warning. */
	if (batch->index == DESC_PER_BATCH) {
		SPDK_ERRLOG("Attempt to add to a batch that is already full.\n");
		return -EINVAL;
	}

	desc = *_desc = &batch->user_desc[batch->index];
	comp = *_comp = &batch->user_completions[batch->index];
	_track_comp(chan, true, batch->index, comp, desc, batch);
	SPDK_DEBUGLOG(idxd, "Prep batch %p index %u\n", batch, batch->index);

	batch->index++;

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = (uintptr_t)&comp->hw;
	comp->cb_arg = cb_arg;
	comp->cb_fn = cb_fn;
	comp->batch = batch;

	return 0;
}

static int
_idxd_batch_prep_nop(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_NOOP;

	if (chan->idxd->impl->nop_check && chan->idxd->impl->nop_check(chan->idxd)) {
		desc->xfer_size = 1;
	}
	return 0;
}

int
spdk_idxd_batch_prep_copy(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			  void *dst, const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->src_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_fill(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			  void *dst, uint64_t fill_pattern, uint64_t nbytes,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->pattern = fill_pattern;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_dualcast(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			      void *dst1, void *dst2, const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst1_addr, dst2_addr;
	int rc;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst1, &dst1_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst2, &dst2_addr, nbytes);
	if (rc) {
		return rc;
	}

	desc->opcode = IDXD_OPCODE_DUALCAST;
	desc->src_addr = src_addr;
	desc->dst_addr = dst1_addr;
	desc->dest2 = dst2_addr;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_crc32c(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			    uint32_t *crc_dst, void *src, uint32_t seed, uint64_t nbytes,
			    spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_CRC32C_GEN;
	desc->dst_addr = 0; /* per specification */
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	comp->crc_dst = crc_dst;

	return 0;
}

int
spdk_idxd_batch_prep_copy_crc32c(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
				 void *dst, void *src, uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
				 spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COPY_CRC;
	desc->dst_addr = dst_addr;
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	comp->crc_dst = crc_dst;

	return 0;
}

int
spdk_idxd_batch_prep_compare(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			     void *src1, void *src2, uint64_t nbytes, spdk_idxd_req_cb cb_fn,
			     void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;
	uint64_t src1_addr, src2_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch, &desc, &comp);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src1, &src1_addr, nbytes);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src2, &src2_addr, nbytes);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPARE;
	desc->src_addr = src1_addr;
	desc->src2_addr = src2_addr;
	desc->xfer_size = nbytes;

	return 0;
}

static inline void
_dump_sw_error_reg(struct spdk_idxd_io_channel *chan)
{
	struct spdk_idxd_device *idxd = chan->idxd;

	assert(idxd != NULL);
	idxd->impl->dump_sw_error(idxd, chan->portal);
}

/* TODO: there are multiple ways of getting completions but we can't really pick the best one without
 * silicon (from a perf perspective at least). The current solution has a large (> cache line) comp_ctx
 * struct but only has one polling loop that covers both batch and regular descriptors based on a list
 * of comp_ctx that we know have outstanding commands. Another experiment would be to have a 64 byte comp_ctx
 * by relying on the bit_array indicies to get all the context we need. This has been implemented in a prev
 * version but has the downside of needing to poll multiple ranges of comp records (because of batches) and
 * needs to look at each array bit to know whether it should even check that completion record. That may be
 * faster though, need to experiment.
 */
#define IDXD_COMPLETION(x) ((x) > (0) ? (1) : (0))
#define IDXD_FAILURE(x) ((x) > (1) ? (1) : (0))
#define IDXD_SW_ERROR(x) ((x) &= (0x1) ? (1) : (0))
int
spdk_idxd_process_events(struct spdk_idxd_io_channel *chan)
{
	struct idxd_comp *comp_ctx, *tmp;
	int status = 0;
	int rc = 0;

	TAILQ_FOREACH_SAFE(comp_ctx, &chan->comp_ctx_oustanding, link, tmp) {
		if (rc == MAX_COMPLETIONS_PER_POLL) {
			break;
		}

		if (IDXD_COMPLETION(comp_ctx->hw.status)) {

			TAILQ_REMOVE(&chan->comp_ctx_oustanding, comp_ctx, link);
			rc++;

			if (spdk_unlikely(IDXD_FAILURE(comp_ctx->hw.status))) {
				status = -EINVAL;
				_dump_sw_error_reg(chan);
			}

			switch (comp_ctx->desc->opcode) {
			case IDXD_OPCODE_BATCH:
				SPDK_DEBUGLOG(idxd, "Complete batch %p\n", comp_ctx->batch);
				break;
			case IDXD_OPCODE_CRC32C_GEN:
			case IDXD_OPCODE_COPY_CRC:
				*comp_ctx->crc_dst = comp_ctx->hw.crc32c_val;
				*comp_ctx->crc_dst ^= ~0;
				break;
			case IDXD_OPCODE_COMPARE:
				if (status == 0) {
					status = comp_ctx->hw.result;
				}
				break;
			}

			if (comp_ctx->cb_fn) {
				comp_ctx->cb_fn(comp_ctx->cb_arg, status);
			}

			comp_ctx->hw.status = status = 0;
			if (comp_ctx->desc->opcode != IDXD_OPCODE_BATCH) {
				assert(spdk_bit_array_get(chan->ring_slots, comp_ctx->index));
				spdk_bit_array_clear(chan->ring_slots, comp_ctx->index);
			} else {
				_free_batch(comp_ctx->batch, chan);
			}
		} else {
			/*
			 * oldest locations are at the head of the list so if
			 * we've polled a location that hasn't completed, bail
			 * now as there are unlikely to be any more completions.
			 */
			break;
		}
	}
	return rc;
}

void
idxd_impl_register(struct spdk_idxd_impl *impl)
{
	STAILQ_INSERT_HEAD(&g_idxd_impls, impl, link);
}

SPDK_LOG_REGISTER_COMPONENT(idxd)
