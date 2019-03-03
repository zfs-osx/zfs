/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016 by Delphix. All rights reserved.
 * Copyright (c) 2019 by Lawrence Livermore National Security, LLC.
 */

#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/txg.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/refcount.h>
#include <sys/metaslab_impl.h>
#include <sys/dsl_synctask.h>
#include <sys/zap.h>
#include <sys/dmu_tx.h>

/*
 * Maximum size of TRIM command, ranges will be chunked in to 128MiB extents.
 */
unsigned int zfs_trim_extent_bytes_max = 128 * 1024 * 1024;

/*
 * Minimum size of TRIM commands, extents smaller than 32Kib will be skipped.
 */
unsigned int zfs_trim_extent_bytes_min = 32 * 1024;

/*
 * Maximum number of queued TRIMs outstanding per leaf vdev.  The number of
 * concurrent TRIM commands issued to the device is controlled by the
 * zfs_vdev_trim_min_active and zfs_vdev_trim_max_active module options.
 */
unsigned int zfs_trim_queue_limit = 10;

/*
 * How many transaction groups worth of updates should be aggregated before
 * TRIM operations are issued to the device.  This setting represents a
 * trade-off between issuing more efficient TRIM operations, by allowing
 * them to be aggregated longer, and issuing them promptly enough that the
 * space is trimmed and available for use by the device.
 *
 * Increasing this value will allow frees to be aggregated for a longer
 * time.  This will result is larger TRIM operations, and increased memory
 * usage in order to track the pending TRIMs.  Decreasing this value will
 * have the opposite effect.  The default value of 32 was determined to be
 * a reasonable compromise.
 */
int zfs_trim_txg_batch = 32;

typedef struct trim_args {
	vdev_t		*trim_vdev;
	metaslab_t	*trim_msp;
	range_tree_t	*trim_tree;
	trim_type_t	trim_type;
	hrtime_t	trim_start_time;
	uint64_t	trim_bytes_done;
	uint64_t	trim_extent_bytes_max;
	uint64_t	trim_extent_bytes_min;
	enum trim_flag	trim_flags;
} trim_args_t;

static boolean_t
vdev_trim_should_stop(vdev_t *vd)
{
	return (vd->vdev_trim_exit_wanted || !vdev_writeable(vd) ||
	    vd->vdev_detached || vd->vdev_top->vdev_removing);
}

static void
vdev_trim_zap_update_sync(void *arg, dmu_tx_t *tx)
{
	/*
	 * We pass in the guid instead of the vdev_t since the vdev may
	 * have been freed prior to the sync task being processed. This
	 * happens when a vdev is detached as we call spa_config_vdev_exit(),
	 * stop the trimming thread, schedule the sync task, and free
	 * the vdev. Later when the scheduled sync task is invoked, it would
	 * find that the vdev has been freed.
	 */
	uint64_t guid = *(uint64_t *)arg;
	uint64_t txg = dmu_tx_get_txg(tx);
	kmem_free(arg, sizeof (uint64_t));

	vdev_t *vd = spa_lookup_by_guid(tx->tx_pool->dp_spa, guid, B_FALSE);
	if (vd == NULL || vd->vdev_top->vdev_removing || !vdev_is_concrete(vd))
		return;

	uint64_t last_offset = vd->vdev_trim_offset[txg & TXG_MASK];
	vd->vdev_trim_offset[txg & TXG_MASK] = 0;

	VERIFY3U(vd->vdev_leaf_zap, !=, 0);

	objset_t *mos = vd->vdev_spa->spa_meta_objset;

	if (last_offset > 0 || vd->vdev_trim_last_offset == UINT64_MAX) {

		if (vd->vdev_trim_last_offset == UINT64_MAX)
			last_offset = 0;

		vd->vdev_trim_last_offset = last_offset;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_LAST_OFFSET,
		    sizeof (last_offset), 1, &last_offset, tx));
	}

	if (vd->vdev_trim_action_time > 0) {
		uint64_t val = (uint64_t)vd->vdev_trim_action_time;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_ACTION_TIME, sizeof (val),
		    1, &val, tx));
	}

	if (vd->vdev_trim_rate > 0) {
		uint64_t rate = (uint64_t)vd->vdev_trim_rate;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_RATE, sizeof (rate), 1, &rate, tx));
	}

	uint64_t partial = vd->vdev_trim_partial;
	if (partial == UINT64_MAX)
		partial = 0;

	VERIFY0(zap_update(mos, vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_PARTIAL,
	    sizeof (partial), 1, &partial, tx));

	uint64_t secure = vd->vdev_trim_secure;
	if (secure == UINT64_MAX)
		secure = 0;

	VERIFY0(zap_update(mos, vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_SECURE,
	    sizeof (secure), 1, &secure, tx));


	uint64_t trim_state = vd->vdev_trim_state;
	VERIFY0(zap_update(mos, vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_STATE,
	    sizeof (trim_state), 1, &trim_state, tx));
}

static void
vdev_trim_change_state(vdev_t *vd, vdev_trim_state_t new_state,
    uint64_t rate, boolean_t partial, boolean_t secure)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	spa_t *spa = vd->vdev_spa;

	if (new_state == vd->vdev_trim_state)
		return;

	/*
	 * Copy the vd's guid, this will be freed by the sync task.
	 */
	uint64_t *guid = kmem_zalloc(sizeof (uint64_t), KM_SLEEP);
	*guid = vd->vdev_guid;

	/*
	 * If we're suspending, then preserve the original start time.
	 */
	if (vd->vdev_trim_state != VDEV_TRIM_SUSPENDED) {
		vd->vdev_trim_action_time = gethrestime_sec();
	}

	/*
	 * If we're activating, then preserve the requested rate and trim
	 * method.  Setting the last offset to UINT64_MAX is used as a
	 * sentinel to indicate the offset should be reset to be start.
	 */
	if (new_state == VDEV_TRIM_ACTIVE) {
		if (vd->vdev_trim_state == VDEV_TRIM_COMPLETE) {
			vd->vdev_trim_last_offset = UINT64_MAX;
			vd->vdev_trim_rate = UINT64_MAX;
			vd->vdev_trim_partial = UINT64_MAX;
			vd->vdev_trim_secure = UINT64_MAX;
		}

		if (rate != 0)
			vd->vdev_trim_rate = rate;

		if (partial != 0)
			vd->vdev_trim_partial = partial;

		if (secure != 0)
			vd->vdev_trim_secure = secure;
	}

	boolean_t resumed = !!(vd->vdev_trim_state == VDEV_TRIM_SUSPENDED);
	vd->vdev_trim_state = new_state;

	dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	dsl_sync_task_nowait(spa_get_dsl(spa), vdev_trim_zap_update_sync,
	    guid, 2, ZFS_SPACE_CHECK_RESERVED, tx);

	switch (new_state) {
	case VDEV_TRIM_ACTIVE:
		spa_event_notify(spa, vd, NULL,
		    resumed ? ESC_ZFS_TRIM_RESUME : ESC_ZFS_TRIM_START);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s activated", vd->vdev_path);
		break;
	case VDEV_TRIM_SUSPENDED:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_SUSPEND);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s suspended", vd->vdev_path);
		break;
	case VDEV_TRIM_CANCELED:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_CANCEL);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s canceled", vd->vdev_path);
		break;
	case VDEV_TRIM_COMPLETE:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_FINISH);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s complete", vd->vdev_path);
		break;
	default:
		panic("invalid state %llu", (unsigned long long)new_state);
	}

	dmu_tx_commit(tx);
}

static void
vdev_trim_cb(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	mutex_enter(&vd->vdev_trim_io_lock);
	if (zio->io_error == ENXIO && !vdev_writeable(vd)) {
		/*
		 * The I/O failed because the vdev was unavailable; roll the
		 * last offset back. (This works because spa_sync waits on
		 * spa_txg_zio before it runs sync tasks.)
		 */
		uint64_t *offset =
		    &vd->vdev_trim_offset[zio->io_txg & TXG_MASK];
		*offset = MIN(*offset, zio->io_offset);
	} else {
		if (zio->io_error != 0) {
			vd->vdev_stat.vs_trim_errors++;
			spa_iostats_trim_add(vd->vdev_spa, TRIM_TYPE_MANUAL,
			    0, 0, 0, 0, 1, zio->io_orig_size);
		} else {
			spa_iostats_trim_add(vd->vdev_spa, TRIM_TYPE_MANUAL,
			    1, zio->io_orig_size, 0, 0, 0, 0);
		}

		vd->vdev_trim_bytes_done += zio->io_orig_size;
	}

	ASSERT3U(vd->vdev_trim_inflight[TRIM_TYPE_MANUAL], >, 0);
	vd->vdev_trim_inflight[TRIM_TYPE_MANUAL]--;
	cv_broadcast(&vd->vdev_trim_io_cv);
	mutex_exit(&vd->vdev_trim_io_lock);

	spa_config_exit(vd->vdev_spa, SCL_STATE_ALL, vd);
}

static void
vdev_autotrim_cb(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	mutex_enter(&vd->vdev_trim_io_lock);

	if (zio->io_error != 0) {
		vd->vdev_stat.vs_trim_errors++;
		spa_iostats_trim_add(vd->vdev_spa, TRIM_TYPE_AUTO,
		    0, 0, 0, 0, 1, zio->io_orig_size);
	} else {
		spa_iostats_trim_add(vd->vdev_spa, TRIM_TYPE_AUTO,
		    1, zio->io_orig_size, 0, 0, 0, 0);
	}

	ASSERT3U(vd->vdev_trim_inflight[TRIM_TYPE_AUTO], >, 0);
	vd->vdev_trim_inflight[TRIM_TYPE_AUTO]--;
	cv_broadcast(&vd->vdev_trim_io_cv);
	mutex_exit(&vd->vdev_trim_io_lock);

	spa_config_exit(vd->vdev_spa, SCL_STATE_ALL, vd);
}

/*
 * Returns the average trim rate in bytes/sec for the ta->trim_vdev.
 */
static uint64_t
vdev_trim_calculate_rate(trim_args_t *ta)
{
	return (ta->trim_bytes_done * 1000 /
	    (NSEC2MSEC(gethrtime() - ta->trim_start_time) + 1));
}

/* Takes care of physical discards and limiting # of concurrent ZIOs. */
static int
vdev_trim_range(trim_args_t *ta, uint64_t start, uint64_t size)
{
	vdev_t *vd = ta->trim_vdev;
	spa_t *spa = vd->vdev_spa;

	mutex_enter(&vd->vdev_trim_io_lock);

	/* Limit trim to requested rate */
	while (vd->vdev_trim_rate != 0 && !vdev_trim_should_stop(vd) &&
	    vdev_trim_calculate_rate(ta) > vd->vdev_trim_rate) {
		cv_timedwait_sig(&vd->vdev_trim_io_cv, &vd->vdev_trim_io_lock,
		    ddi_get_lbolt() + MSEC_TO_TICK(10));
	}
	ta->trim_bytes_done += size;

	/* Limit inflight trimming I/Os */
	while (vd->vdev_trim_inflight[0] + vd->vdev_trim_inflight[1] >=
	    zfs_trim_queue_limit) {
		cv_wait(&vd->vdev_trim_io_cv, &vd->vdev_trim_io_lock);
	}
	vd->vdev_trim_inflight[ta->trim_type]++;
	mutex_exit(&vd->vdev_trim_io_lock);

	dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	uint64_t txg = dmu_tx_get_txg(tx);

	spa_config_enter(spa, SCL_STATE_ALL, vd, RW_READER);
	mutex_enter(&vd->vdev_trim_lock);

	if (ta->trim_type == TRIM_TYPE_MANUAL &&
	    vd->vdev_trim_offset[txg & TXG_MASK] == 0) {
		uint64_t *guid = kmem_zalloc(sizeof (uint64_t), KM_SLEEP);
		*guid = vd->vdev_guid;

		/* This is the first write of this txg. */
		dsl_sync_task_nowait(spa_get_dsl(spa),
		    vdev_trim_zap_update_sync, guid, 2,
		    ZFS_SPACE_CHECK_RESERVED, tx);
	}

	/*
	 * We know the vdev struct will still be around since all
	 * consumers of vdev_free must stop the trimming first.
	 */
	if (vdev_trim_should_stop(vd)) {
		mutex_enter(&vd->vdev_trim_io_lock);
		vd->vdev_trim_inflight[ta->trim_type]--;
		mutex_exit(&vd->vdev_trim_io_lock);
		spa_config_exit(vd->vdev_spa, SCL_STATE_ALL, vd);
		mutex_exit(&vd->vdev_trim_lock);
		dmu_tx_commit(tx);
		return (SET_ERROR(EINTR));
	}
	mutex_exit(&vd->vdev_trim_lock);

	if (ta->trim_type == TRIM_TYPE_MANUAL)
		vd->vdev_trim_offset[txg & TXG_MASK] = start + size;

	zio_nowait(zio_trim(spa->spa_txg_zio[txg & TXG_MASK], vd,
	    start, size, ta->trim_type == TRIM_TYPE_MANUAL ?
	    vdev_trim_cb : vdev_autotrim_cb, NULL,
	    ZIO_PRIORITY_TRIM, ZIO_FLAG_CANFAIL, ta->trim_flags));
	/* vdev_trim_cb and vdev_autotrim_cb release SCL_STATE_ALL */

	dmu_tx_commit(tx);

	return (0);
}

static int
vdev_trim_ranges(trim_args_t *ta)
{
	vdev_t *vd = ta->trim_vdev;
	avl_tree_t *rt = &ta->trim_tree->rt_root;
	uint64_t extent_bytes_max = ta->trim_extent_bytes_max;
	uint64_t extent_bytes_min = ta->trim_extent_bytes_min;
	spa_t *spa = vd->vdev_spa;

	ta->trim_start_time = gethrtime();
	ta->trim_bytes_done = 0;

	for (range_seg_t *rs = avl_first(rt); rs != NULL;
	    rs = AVL_NEXT(rt, rs)) {
		uint64_t size = rs->rs_end - rs->rs_start;

		if (extent_bytes_min && size < extent_bytes_min) {
			spa_iostats_trim_add(spa, ta->trim_type,
			    0, 0, 1, size, 0, 0);
			continue;
		}

		/* Split range into legally-sized physical chunks */
		uint64_t writes_required = ((size - 1) / extent_bytes_max) + 1;

		for (uint64_t w = 0; w < writes_required; w++) {
			int error;

			error = vdev_trim_range(ta, VDEV_LABEL_START_SIZE +
			    rs->rs_start + (w * extent_bytes_max),
			    MIN(size - (w * extent_bytes_max),
			    extent_bytes_max));
			if (error != 0) {
				return (error);
			}
		}
	}

	return (0);
}

static void
vdev_trim_calculate_progress(vdev_t *vd)
{
	ASSERT(spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_READER) ||
	    spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_WRITER));
	ASSERT(vd->vdev_leaf_zap != 0);

	vd->vdev_trim_bytes_est = 0;
	vd->vdev_trim_bytes_done = 0;

	for (uint64_t i = 0; i < vd->vdev_top->vdev_ms_count; i++) {
		metaslab_t *msp = vd->vdev_top->vdev_ms[i];
		mutex_enter(&msp->ms_lock);

		uint64_t ms_free = msp->ms_size -
		    metaslab_allocated_space(msp);

		if (vd->vdev_top->vdev_ops == &vdev_raidz_ops)
			ms_free /= vd->vdev_top->vdev_children;

		/*
		 * Convert the metaslab range to a physical range
		 * on our vdev. We use this to determine if we are
		 * in the middle of this metaslab range.
		 */
		range_seg_t logical_rs, physical_rs;
		logical_rs.rs_start = msp->ms_start;
		logical_rs.rs_end = msp->ms_start + msp->ms_size;
		vdev_xlate(vd, &logical_rs, &physical_rs);

		if (vd->vdev_trim_last_offset <= physical_rs.rs_start) {
			vd->vdev_trim_bytes_est += ms_free;
			mutex_exit(&msp->ms_lock);
			continue;
		} else if (vd->vdev_trim_last_offset > physical_rs.rs_end) {
			vd->vdev_trim_bytes_done += ms_free;
			vd->vdev_trim_bytes_est += ms_free;
			mutex_exit(&msp->ms_lock);
			continue;
		}

		/*
		 * If we get here, we're in the middle of trimming this
		 * metaslab. Load it and walk the free tree for more accurate
		 * progress estimation.
		 */
		VERIFY0(metaslab_load(msp));

		for (range_seg_t *rs = avl_first(&msp->ms_allocatable->rt_root);
		    rs; rs = AVL_NEXT(&msp->ms_allocatable->rt_root, rs)) {
			logical_rs.rs_start = rs->rs_start;
			logical_rs.rs_end = rs->rs_end;
			vdev_xlate(vd, &logical_rs, &physical_rs);

			uint64_t size = physical_rs.rs_end -
			    physical_rs.rs_start;
			vd->vdev_trim_bytes_est += size;
			if (vd->vdev_trim_last_offset >= physical_rs.rs_end) {
				vd->vdev_trim_bytes_done += size;
			} else if (vd->vdev_trim_last_offset >
			    physical_rs.rs_start &&
			    vd->vdev_trim_last_offset <=
			    physical_rs.rs_end) {
				vd->vdev_trim_bytes_done +=
				    vd->vdev_trim_last_offset -
				    physical_rs.rs_start;
			}
		}
		mutex_exit(&msp->ms_lock);
	}
}

static int
vdev_trim_load(vdev_t *vd)
{
	int err = 0;
	ASSERT(spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_READER) ||
	    spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_WRITER));
	ASSERT(vd->vdev_leaf_zap != 0);

	if (vd->vdev_trim_state == VDEV_TRIM_ACTIVE ||
	    vd->vdev_trim_state == VDEV_TRIM_SUSPENDED) {
		err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_LAST_OFFSET,
		    sizeof (vd->vdev_trim_last_offset), 1,
		    &vd->vdev_trim_last_offset);
		if (err == ENOENT) {
			vd->vdev_trim_last_offset = 0;
			err = 0;
		}

		if (err == 0) {
			err = zap_lookup(vd->vdev_spa->spa_meta_objset,
			    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_RATE,
			    sizeof (vd->vdev_trim_rate), 1,
			    &vd->vdev_trim_rate);
			if (err == ENOENT) {
				vd->vdev_trim_rate = 0;
				err = 0;
			}
		}

		if (err == 0) {
			err = zap_lookup(vd->vdev_spa->spa_meta_objset,
			    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_PARTIAL,
			    sizeof (vd->vdev_trim_partial), 1,
			    &vd->vdev_trim_partial);
			if (err == ENOENT) {
				vd->vdev_trim_partial = 0;
				err = 0;
			}
		}

		if (err == 0) {
			err = zap_lookup(vd->vdev_spa->spa_meta_objset,
			    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_SECURE,
			    sizeof (vd->vdev_trim_secure), 1,
			    &vd->vdev_trim_secure);
			if (err == ENOENT) {
				vd->vdev_trim_secure = 0;
				err = 0;
			}
		}
	}

	vdev_trim_calculate_progress(vd);

	return (err);
}

/*
 * Convert the logical range into a physical range and add it to the
 * range tree passed in the trim_args_t.
 */
void
vdev_trim_range_add(void *arg, uint64_t start, uint64_t size)
{
	trim_args_t *ta = arg;
	vdev_t *vd = ta->trim_vdev;
	range_seg_t logical_rs, physical_rs;
	logical_rs.rs_start = start;
	logical_rs.rs_end = start + size;

	/*
	 * Every range to be trimmed must be part of ms_allocatable.
	 */
	ASSERT3B(ta->trim_msp->ms_loaded, ==, B_TRUE);
	ASSERT(range_tree_find(ta->trim_msp->ms_allocatable,
	    start, size) != NULL);

	ASSERT(vd->vdev_ops->vdev_op_leaf);
	vdev_xlate(vd, &logical_rs, &physical_rs);

	IMPLY(vd->vdev_top == vd,
	    logical_rs.rs_start == physical_rs.rs_start);
	IMPLY(vd->vdev_top == vd,
	    logical_rs.rs_end == physical_rs.rs_end);

	/*
	 * Only a manual trim will be traversing the vdev sequentially.
	 * For an auto trim all valid ranges should be added.
	 */
	if (ta->trim_type == TRIM_TYPE_MANUAL) {

		/* Only add segments that we have not visited yet */
		if (physical_rs.rs_end <= vd->vdev_trim_last_offset)
			return;

		/* Pick up where we left off mid-range. */
		if (vd->vdev_trim_last_offset > physical_rs.rs_start) {
			ASSERT3U(physical_rs.rs_end, >,
			    vd->vdev_trim_last_offset);
			physical_rs.rs_start = vd->vdev_trim_last_offset;
		}
	}

	ASSERT3U(physical_rs.rs_end, >=, physical_rs.rs_start);

	/*
	 * With raidz, it's possible that the logical range does not live on
	 * this leaf vdev. We only add the physical range to this vdev's if it
	 * has a length greater than 0.
	 */
	if (physical_rs.rs_end > physical_rs.rs_start) {
		range_tree_add(ta->trim_tree, physical_rs.rs_start,
		    physical_rs.rs_end - physical_rs.rs_start);
	} else {
		ASSERT3U(physical_rs.rs_end, ==, physical_rs.rs_start);
	}
}

/*
 * Each (manual) trim thread is responsible for trimming the unallocated
 * space for each leaf vdev as described by its top-level ms_allocatable.
 */
static void
vdev_trim_thread(void *arg)
{
	vdev_t *vd = arg;
	spa_t *spa = vd->vdev_spa;
	trim_args_t ta;
	int error = 0;

	/*
	 * The VDEV_LEAF_ZAP_TRIM_* entries may have been updated by
	 * vdev_trim().  Wait for the updated values to be reflected
	 * in the zap in order to start with the requested settings.
	 */
	txg_wait_synced(spa_get_dsl(vd->vdev_spa), 0);

	ASSERT(vdev_is_concrete(vd));
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

	vd->vdev_trim_last_offset = 0;
	vd->vdev_trim_rate = 0;
	vd->vdev_trim_partial = 0;
	vd->vdev_trim_secure = 0;

	VERIFY0(vdev_trim_load(vd));

	ta.trim_vdev = vd;
	ta.trim_extent_bytes_max = zfs_trim_extent_bytes_max;
	ta.trim_extent_bytes_min = zfs_trim_extent_bytes_min;
	ta.trim_tree = range_tree_create(NULL, NULL);
	ta.trim_type = TRIM_TYPE_MANUAL;
	ta.trim_flags = 0;

	/*
	 * When a secure TRIM has been requested infer that the intent
	 * is that everything must be trimmed.  Override the default
	 * minimum TRIM size to prevent ranges from being skipped.
	 */
	if (vd->vdev_trim_secure) {
		ta.trim_flags |= ZIO_TRIM_SECURE;
		ta.trim_extent_bytes_min = SPA_MINBLOCKSIZE;
	}

	uint64_t ms_count = 0;
	for (uint64_t i = 0; !vd->vdev_detached &&
	    i < vd->vdev_top->vdev_ms_count; i++) {
		metaslab_t *msp = vd->vdev_top->vdev_ms[i];

		/*
		 * If we've expanded the top-level vdev or it's our
		 * first pass, calculate our progress.
		 */
		if (vd->vdev_top->vdev_ms_count != ms_count) {
			vdev_trim_calculate_progress(vd);
			ms_count = vd->vdev_top->vdev_ms_count;
		}

		metaslab_disable(msp);
		mutex_enter(&msp->ms_lock);
		VERIFY0(metaslab_load(msp));

		/*
		 * If a partial TRIM was requested skip metaslabs which have
		 * never been initialized and thus have never been written.
		 */
		if (msp->ms_sm == NULL && vd->vdev_trim_partial) {
			mutex_exit(&msp->ms_lock);
			metaslab_enable(msp, B_FALSE);
			vdev_trim_calculate_progress(vd);
			continue;
		}

		ta.trim_msp = msp;
		range_tree_walk(msp->ms_allocatable, vdev_trim_range_add, &ta);
		range_tree_vacate(msp->ms_trim, NULL, NULL);
		mutex_exit(&msp->ms_lock);

		spa_config_exit(spa, SCL_CONFIG, FTAG);
		error = vdev_trim_ranges(&ta);
		metaslab_enable(msp, B_TRUE);
		spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

		range_tree_vacate(ta.trim_tree, NULL, NULL);
		if (error != 0)
			break;
	}

	spa_config_exit(spa, SCL_CONFIG, FTAG);
	mutex_enter(&vd->vdev_trim_io_lock);
	while (vd->vdev_trim_inflight[0] > 0) {
		cv_wait(&vd->vdev_trim_io_cv, &vd->vdev_trim_io_lock);
	}
	mutex_exit(&vd->vdev_trim_io_lock);

	range_tree_destroy(ta.trim_tree);

	mutex_enter(&vd->vdev_trim_lock);
	if (!vd->vdev_trim_exit_wanted && vdev_writeable(vd)) {
		vdev_trim_change_state(vd, VDEV_TRIM_COMPLETE,
		    vd->vdev_trim_rate, vd->vdev_trim_partial,
		    vd->vdev_trim_secure);
	}
	ASSERT(vd->vdev_trim_thread != NULL || vd->vdev_trim_inflight[0] == 0);

	/*
	 * Drop the vdev_trim_lock while we sync out the txg since it's
	 * possible that a device might be trying to come online and must
	 * check to see if it needs to restart a trim. That thread will be
	 * holding the spa_config_lock which would prevent the txg_wait_synced
	 * from completing.
	 */
	mutex_exit(&vd->vdev_trim_lock);
	txg_wait_synced(spa_get_dsl(spa), 0);
	mutex_enter(&vd->vdev_trim_lock);

	vd->vdev_trim_thread = NULL;
	cv_broadcast(&vd->vdev_trim_cv);
	mutex_exit(&vd->vdev_trim_lock);
}

/*
 * Initiates a device. Caller must hold vdev_trim_lock.
 * Device must be a leaf and not already be trimming.
 */
void
vdev_trim(vdev_t *vd, uint64_t rate, boolean_t partial, boolean_t secure)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	ASSERT(vd->vdev_ops->vdev_op_leaf);
	ASSERT(vdev_is_concrete(vd));
	ASSERT3P(vd->vdev_trim_thread, ==, NULL);
	ASSERT(!vd->vdev_detached);
	ASSERT(!vd->vdev_trim_exit_wanted);
	ASSERT(!vd->vdev_top->vdev_removing);

	vdev_trim_change_state(vd, VDEV_TRIM_ACTIVE, rate, partial, secure);
	vd->vdev_trim_thread = thread_create(NULL, 0,
	    vdev_trim_thread, vd, 0, &p0, TS_RUN, maxclsyspri);
}

/*
 * Wait for the trimming thread to be terminated (cancelled or stopped).
 */
static void
vdev_trim_stop_wait_impl(vdev_t *vd)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));

	while (vd->vdev_trim_thread != NULL)
		cv_wait(&vd->vdev_trim_cv, &vd->vdev_trim_lock);

	ASSERT3P(vd->vdev_trim_thread, ==, NULL);
	vd->vdev_trim_exit_wanted = B_FALSE;
}

/*
 * Wait for vdev trim threads which were either to cleanly exit.
 */
void
vdev_trim_stop_wait(spa_t *spa, list_t *vd_list)
{
	vdev_t *vd;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	while ((vd = list_remove_head(vd_list)) != NULL) {
		mutex_enter(&vd->vdev_trim_lock);
		vdev_trim_stop_wait_impl(vd);
		mutex_exit(&vd->vdev_trim_lock);
	}
}

/*
 * Stop trimming a device, with the resultant trimming state being tgt_state.
 * For blocking behavior pass NULL for vd_list.  Otherwise, when a list_t is
 * provided the stopping vdev is inserted in to the list.  Callers are then
 * required to call vdev_trim_stop_wait() to block for all the trim threads
 * to exit.  The caller must hold vdev_trim_lock and must not be writing to
 * the spa config, as the trimming thread may try to enter the config as a
 * reader before exiting.
 */
void
vdev_trim_stop(vdev_t *vd, vdev_trim_state_t tgt_state, list_t *vd_list)
{
	ASSERT(!spa_config_held(vd->vdev_spa, SCL_CONFIG|SCL_STATE, RW_WRITER));
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	ASSERT(vd->vdev_ops->vdev_op_leaf);
	ASSERT(vdev_is_concrete(vd));

	/*
	 * Allow cancel requests to proceed even if the trim thread has
	 * stopped.
	 */
	if (vd->vdev_trim_thread == NULL && tgt_state != VDEV_TRIM_CANCELED)
		return;

	vdev_trim_change_state(vd, tgt_state, 0, 0, 0);
	vd->vdev_trim_exit_wanted = B_TRUE;

	if (vd_list == NULL) {
		vdev_trim_stop_wait_impl(vd);
	} else {
		ASSERT(MUTEX_HELD(&spa_namespace_lock));
		list_insert_tail(vd_list, vd);
	}
}

static void
vdev_trim_stop_all_impl(vdev_t *vd, vdev_trim_state_t tgt_state,
    list_t *vd_list)
{
	if (vd->vdev_ops->vdev_op_leaf && vdev_is_concrete(vd)) {
		mutex_enter(&vd->vdev_trim_lock);
		vdev_trim_stop(vd, tgt_state, vd_list);
		mutex_exit(&vd->vdev_trim_lock);
		return;
	}

	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		vdev_trim_stop_all_impl(vd->vdev_child[i], tgt_state,
		    vd_list);
	}
}

/*
 * Convenience function to stop trimming of a vdev tree and set all trim
 * thread pointers to NULL.
 */
void
vdev_trim_stop_all(vdev_t *vd, vdev_trim_state_t tgt_state)
{
	spa_t *spa = vd->vdev_spa;
	list_t vd_list;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	list_create(&vd_list, sizeof (vdev_t),
	    offsetof(vdev_t, vdev_trim_node));

	vdev_trim_stop_all_impl(vd, tgt_state, &vd_list);
	vdev_trim_stop_wait(spa, &vd_list);

	if (vd->vdev_spa->spa_sync_on) {
		/* Make sure that our state has been synced to disk */
		txg_wait_synced(spa_get_dsl(vd->vdev_spa), 0);
	}

	list_destroy(&vd_list);
}

void
vdev_trim_restart(vdev_t *vd)
{
	ASSERT(MUTEX_HELD(&spa_namespace_lock));
	ASSERT(!spa_config_held(vd->vdev_spa, SCL_ALL, RW_WRITER));

	if (vd->vdev_leaf_zap != 0) {
		mutex_enter(&vd->vdev_trim_lock);
		uint64_t trim_state = VDEV_TRIM_NONE;
		int err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_STATE,
		    sizeof (trim_state), 1, &trim_state);
		ASSERT(err == 0 || err == ENOENT);
		vd->vdev_trim_state = trim_state;

		uint64_t timestamp = 0;
		err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_ACTION_TIME,
		    sizeof (timestamp), 1, &timestamp);
		ASSERT(err == 0 || err == ENOENT);
		vd->vdev_trim_action_time = (time_t)timestamp;

		if (vd->vdev_trim_state == VDEV_TRIM_SUSPENDED ||
		    vd->vdev_offline) {
			/* load progress for reporting, but don't resume */
			VERIFY0(vdev_trim_load(vd));
		} else if (vd->vdev_trim_state == VDEV_TRIM_ACTIVE &&
		    vdev_writeable(vd) && !vd->vdev_top->vdev_removing &&
		    vd->vdev_trim_thread == NULL) {
			VERIFY0(vdev_trim_load(vd));
			vdev_trim(vd, vd->vdev_trim_rate,
			    vd->vdev_trim_partial, vd->vdev_trim_secure);
		}

		mutex_exit(&vd->vdev_trim_lock);
	}

	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		vdev_trim_restart(vd->vdev_child[i]);
	}
}

static void
vdev_trim_range_verify(void *arg, uint64_t start, uint64_t size)
{
	trim_args_t *ta = arg;
	metaslab_t *msp = ta->trim_msp;

	VERIFY3B(msp->ms_loaded, ==, B_TRUE);
	VERIFY3U(msp->ms_disabled, >, 0);
	VERIFY(range_tree_find(msp->ms_allocatable, start, size) != NULL);
}

/*
 * Each auto-trim thread is responsible for managing the auto-trimming for
 * a top-level vdev in the pool.  No auto-trim state is maintained on-disk.
 *
 * N.B. This behavior is different from a manual TRIM where a thread
 * is created for each leaf vdev, instead of each top-level vdev.
 */
static void
vdev_autotrim_thread(void *arg)
{
	vdev_t *vd = arg;
	spa_t *spa = vd->vdev_spa;
	int shift = 0;

	mutex_enter(&vd->vdev_autotrim_lock);
	ASSERT3P(vd->vdev_top, ==, vd);
	ASSERT3P(vd->vdev_autotrim_thread, !=, NULL);
	mutex_exit(&vd->vdev_autotrim_lock);
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

	uint64_t extent_bytes_max = zfs_trim_extent_bytes_max;
	uint64_t extent_bytes_min = zfs_trim_extent_bytes_min;

	while (!vd->vdev_autotrim_exit_wanted && vdev_writeable(vd) &&
	    !vd->vdev_removing && spa_get_autotrim(spa) == SPA_AUTOTRIM_ON) {
		int txgs_per_trim = MAX(zfs_trim_txg_batch, 1);
		boolean_t issued_trim = B_FALSE;

		/*
		 * The metaslabs are rotated over in groups of num_metaslabs /
		 * zfs_trim_txg_batch per txg.  The intent is to always allow
		 * a minimum number of txgs to be processed before revisiting
		 * a metaslab.  In this way, free space can be aggregated and
		 * sufficiently large TRIM commands issued.  Depending on how
		 * much free space needs to be trimmed, it may take longer to
		 * revisit a metaslab allowing additional aggregation.
		 */
		for (uint64_t i = shift % txgs_per_trim; i < vd->vdev_ms_count;
		    i += txgs_per_trim) {
			metaslab_t *msp = vd->vdev_ms[i];
			range_tree_t *trim_tree;

			metaslab_disable(msp);
			mutex_enter(&msp->ms_lock);

			/*
			 * Skip the metaslab when it has never been allocated
			 * or when there are no recent frees to trim.
			 */
			if (msp->ms_sm == NULL ||
			    range_tree_is_empty(msp->ms_trim)) {
				mutex_exit(&msp->ms_lock);
				metaslab_enable(msp, B_FALSE);
				continue;
			}

			/*
			 * Skip the metaslab when it has already been disabled.
			 * This may happen when a manual TRIM or initialize
			 * operation is running concurrently.  In the case
			 * of a manual TRIM, the ms_trim tree will have been
			 * vacated.  Only ranges added after the manual TRIM
			 * disabled the metaslab will be included in the tree.
			 * These will be processed on the next autotrim pass.
			 */
			if (msp->ms_disabled > 1) {
				mutex_exit(&msp->ms_lock);
				metaslab_enable(msp, B_FALSE);
				continue;
			}

			/*
			 * The ms_trim tree is a subset of the ms_allocatable
			 * tree.  When ZFS_DEBUG_TRIM is set load the metaslab
			 * in order to verify the trim ranges both before and
			 * after issuing the TRIM IO.
			 */
			if (zfs_flags & ZFS_DEBUG_TRIM)
				VERIFY0(metaslab_load(msp));

			/*
			 * Allocate an empty range tree which is swapped in
			 * for the existing ms_trim tree while it is processed.
			 */
			trim_tree = range_tree_create(NULL, NULL);
			range_tree_swap(&msp->ms_trim, &trim_tree);
			ASSERT(range_tree_is_empty(msp->ms_trim));

			/*
			 * There are two cases when constructing the per-vdev
			 * trim trees for a metaslab.  If the top-level vdev
			 * has no children then it is also a leaf and should
			 * be trimmed.  Otherwise our children are the leaves
			 * and a trim tree should be constructed for each.
			 */
			trim_args_t *tap;
			uint64_t children = vd->vdev_children;
			if (children == 0) {
				children = 1;
				tap = kmem_zalloc(sizeof (trim_args_t) *
				    children, KM_SLEEP);
				tap[0].trim_vdev = vd;
			} else {
				tap = kmem_zalloc(sizeof (trim_args_t) *
				    children, KM_SLEEP);

				for (uint64_t c = 0; c < children; c++) {
					tap[c].trim_vdev = vd->vdev_child[c];
				}
			}

			for (uint64_t c = 0; c < children; c++) {
				trim_args_t *ta = &tap[c];
				vdev_t *cvd = ta->trim_vdev;

				ta->trim_msp = msp;
				ta->trim_extent_bytes_max = extent_bytes_max;
				ta->trim_extent_bytes_min = extent_bytes_min;
				ta->trim_type = TRIM_TYPE_AUTO;
				ta->trim_flags = 0;

				if (cvd->vdev_detached ||
				    !vdev_writeable(cvd) ||
				    !cvd->vdev_has_trim ||
				    !cvd->vdev_ops->vdev_op_leaf ||
				    cvd->vdev_trim_thread != NULL)
					continue;

				ta->trim_tree = range_tree_create(NULL, NULL);
				range_tree_walk(trim_tree,
				    vdev_trim_range_add, ta);
			}

			mutex_exit(&msp->ms_lock);
			spa_config_exit(spa, SCL_CONFIG, FTAG);

			/*
			 * Issue the trims for all ranges covered by the trim
			 * trees.  These ranges are safe to trim because no
			 * new allocations will be performed until the call
			 * to metaslab_enabled() below.
			 */
			for (uint64_t c = 0; c < children; c++) {
				trim_args_t *ta = &tap[c];

				/*
				 * Always yield to a manual TRIM if one has
				 * been started for the child vdev.
				 */
				if (ta->trim_tree == NULL ||
				    ta->trim_vdev->vdev_trim_thread != NULL) {
					continue;
				}

				int error = vdev_trim_ranges(ta);
				if (error)
					break;

				issued_trim = B_TRUE;
			}

			/*
			 * Verify every range which was trimmed is still
			 * contained within the ms_allocatable tree.
			 */
			if (zfs_flags & ZFS_DEBUG_TRIM) {
				mutex_enter(&msp->ms_lock);
				VERIFY0(metaslab_load(msp));
				ASSERT3P(tap[0].trim_msp, ==, msp);
				range_tree_walk(trim_tree,
				    vdev_trim_range_verify, &tap[0]);
				mutex_exit(&msp->ms_lock);
			}

			range_tree_vacate(trim_tree, NULL, NULL);
			range_tree_destroy(trim_tree);

			metaslab_enable(msp, issued_trim);
			spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

			for (uint64_t c = 0; c < children; c++) {
				trim_args_t *ta = &tap[c];

				if (ta->trim_tree == NULL)
					continue;

				range_tree_vacate(ta->trim_tree, NULL, NULL);
				range_tree_destroy(ta->trim_tree);
			}

			kmem_free(tap, sizeof (trim_args_t) * children);
		}

		spa_config_exit(spa, SCL_CONFIG, FTAG);

		/*
		 * When there was no need to issue TRIM commands for any
		 * of the scanned metaslabs (and therefore no reason to wait
		 * on outstanding IO), then artifically delay before checking
		 * the next group of metaslabs.
		 */
		if (!issued_trim)
			delay(hz);

		shift++;
		spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	}

	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];
		mutex_enter(&cvd->vdev_trim_io_lock);

		while (cvd->vdev_trim_inflight[1] > 0) {
			cv_wait(&cvd->vdev_trim_io_cv,
			    &cvd->vdev_trim_io_lock);
		}
		mutex_exit(&cvd->vdev_trim_io_lock);
	}

	spa_config_exit(spa, SCL_CONFIG, FTAG);

	/*
	 * When exiting because the autotrim property was set to off, then
	 * abandon any unprocessed auto-trim ranges to reclaim the memory.
	 */
	if (spa_get_autotrim(spa) == SPA_AUTOTRIM_OFF) {
		for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
			metaslab_t *msp = vd->vdev_ms[i];

			mutex_enter(&msp->ms_lock);
			range_tree_vacate(msp->ms_trim, NULL, NULL);
			mutex_exit(&msp->ms_lock);
		}
	}

	mutex_enter(&vd->vdev_autotrim_lock);
	ASSERT(vd->vdev_autotrim_thread != NULL);
	vd->vdev_autotrim_thread = NULL;
	cv_broadcast(&vd->vdev_autotrim_cv);
	mutex_exit(&vd->vdev_autotrim_lock);
}

/*
 * Starts an autotrim thread, if needed, for each top-level vdev which can be
 * trimmed.  A top-level vdev which has been evacuated will never be trimmed.
 */
void
vdev_autotrim(spa_t *spa)
{
	vdev_t *root_vd = spa->spa_root_vdev;

	for (uint64_t i = 0; i < root_vd->vdev_children; i++) {
		vdev_t *tvd = root_vd->vdev_child[i];

		mutex_enter(&tvd->vdev_autotrim_lock);
		if (vdev_writeable(tvd) && !tvd->vdev_removing &&
		    tvd->vdev_autotrim_thread == NULL) {
			ASSERT3P(tvd->vdev_top, ==, tvd);

			tvd->vdev_autotrim_thread = thread_create(NULL, 0,
			    vdev_autotrim_thread, tvd, 0, &p0, TS_RUN,
			    maxclsyspri);
			ASSERT(tvd->vdev_autotrim_thread != NULL);
		}
		mutex_exit(&tvd->vdev_autotrim_lock);
	}
}

/*
 * Wait for the autotrim thread associated with the passed top-level vdev
 * to be terminated (cancelled or stopped).
 */
void
vdev_autotrim_stop_wait(vdev_t *tvd)
{
	mutex_enter(&tvd->vdev_autotrim_lock);
	if (tvd->vdev_autotrim_thread != NULL) {
		tvd->vdev_autotrim_exit_wanted = B_TRUE;

		while (tvd->vdev_autotrim_thread != NULL) {
			cv_wait(&tvd->vdev_autotrim_cv,
			    &tvd->vdev_autotrim_lock);
		}

		ASSERT3P(tvd->vdev_autotrim_thread, ==, NULL);
		tvd->vdev_autotrim_exit_wanted = B_FALSE;
	}
	mutex_exit(&tvd->vdev_autotrim_lock);
}

void
vdev_autotrim_stop_all(spa_t *spa)
{
	vdev_t *root_vd = spa->spa_root_vdev;

	for (uint64_t i = 0; i < root_vd->vdev_children; i++)
		vdev_autotrim_stop_wait(root_vd->vdev_child[i]);
}

void
vdev_autotrim_restart(spa_t *spa)
{
	if (spa->spa_autotrim)
		vdev_autotrim(spa);
}

#if defined(_KERNEL)
EXPORT_SYMBOL(vdev_trim);
EXPORT_SYMBOL(vdev_trim_stop);
EXPORT_SYMBOL(vdev_trim_stop_all);
EXPORT_SYMBOL(vdev_trim_stop_wait);
EXPORT_SYMBOL(vdev_trim_restart);
EXPORT_SYMBOL(vdev_autotrim);
EXPORT_SYMBOL(vdev_autotrim_stop_all);
EXPORT_SYMBOL(vdev_autotrim_stop_wait);
EXPORT_SYMBOL(vdev_autotrim_restart);

/* BEGIN CSTYLED */
module_param(zfs_trim_extent_bytes_max, uint, 0644);
MODULE_PARM_DESC(zfs_trim_extent_bytes_max,
    "Max size of TRIM commands, larger will be split");

module_param(zfs_trim_extent_bytes_min, uint, 0644);
MODULE_PARM_DESC(zfs_trim_extent_bytes_min,
    "Min size of TRIM commands, smaller will be skipped");

module_param(zfs_trim_txg_batch, uint, 0644);
MODULE_PARM_DESC(zfs_trim_txg_batch,
    "Number of txgs to aggregate frees before issuing TRIM");

module_param(zfs_trim_queue_limit, uint, 0644);
MODULE_PARM_DESC(zfs_trim_queue_limit,
    "Max queued TRIMs outstanding per leaf vdev");
/* END CSTYLED */
#endif
