// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
/* EOM driver for Mahua PCIe Gen4 and Mahua PCIe Gen5. */

#include <linux/delay.h>
#include <linux/eom_ioctl.h>
#include <linux/kthread.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/phy_core.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include "buffer_manager.h"
#include "eom_driver.h"
#include "pcie_eom_mahua_phy_reg.h"

#if IS_ENABLED(CONFIG_PCI_MSM_EOM)

/**
 * mahua_eom_seq_should_stop - check whether an EOM sequence has been asked
 * to stop
 * @lane: EOM lane structure being scanned
 *
 * Checks the same per-lane/global stop flags eom_driver.h's own
 * eom_seq_should_stop() checks, plus kthread_should_stop(). Defined locally,
 * against only the atomic_t fields and g_eom_seq_stop that every
 * eom_driver.h revision provides, instead of calling that header's helper
 * directly, so this backend builds and behaves the same way regardless of
 * which eom_driver.h revision it is paired with (mirrors the pre-existing
 * USB/Canoe backends' own direct checks of these same flags). Only ever
 * called from within the scan kthread itself (every call site is reached
 * through this backend's run_eom() entry points), so kthread_should_stop()
 * is always legal here.
 *
 * Return: true if the running sequence should stop, false otherwise
 */
static inline bool mahua_eom_seq_should_stop(struct eom_lane *lane)
{
	return kthread_should_stop() ||
	       atomic_read(&g_eom_seq_stop) ||
	       atomic_read(&lane->eom_seq_stop);
}

/**
 * pcie4_mahua_phy_get_reg_offset - Compute lane-strided register address
 * @base_reg:  Lane-0 base offset (from pcie_eom_mahua_phy_reg.h)
 * @lanenum:   0-based lane index
 *
 * Return: Register offset for the requested lane.
 */
static inline u32 pcie4_mahua_phy_get_reg_offset(u32 base_reg, u32 lanenum)
{
	return PCIE4_MAHUA_PHY_REG_ADDR(base_reg, lanenum);
}

/**
 * struct eom_reg_seq - One write entry in an EOM register sequence
 * @offset:    Lane-0 base register offset
 * @value:     Value to write
 * @delay_ns:  Optional ndelay() after the write (0 = no delay)
 */
struct eom_reg_seq {
	u32 offset;
	u32 value;
	u32 delay_ns;
};

static struct mutex mahua_eom_locks[MAHUA_PHY_MAX_RC_INSTANCES];
static bool mahua_eom_locks_ready;
static DEFINE_MUTEX(mahua_eom_locks_init_lock);

/**
 * mahua_get_instance_lock - Return the per-RC-instance mutex, initialising
 * the lock array on first use.
 * @rc_idx: RC instance index (0-based). Must be < MAHUA_PHY_MAX_RC_INSTANCES.
 *
 * This function is the sole point of access to mahua_eom_locks[]. The bounds
 * check here is intentional and must not be removed: the array has a fixed
 * size of MAHUA_PHY_MAX_RC_INSTANCES (8) and indexing it with an out-of-range
 * rc_idx would silently corrupt adjacent globals or mutex metadata.
 *
 * Callers MUST check the return value for NULL before calling mutex_lock().
 * Passing a NULL pointer to mutex_lock() is undefined behaviour.
 *
 * Defined here, before its first caller (mahua_gen4_pcie_eom_sequence()),
 * so the compiler always has the correct 'struct mutex *' prototype in
 * scope; calling it via an implicit declaration would truncate the
 * returned pointer on 64-bit builds.
 *
 * Return: Pointer to the mutex for @rc_idx, or NULL if @rc_idx is out of range.
 */
static struct mutex *mahua_get_instance_lock(u32 rc_idx)
{
	int i;

	if (rc_idx >= MAHUA_PHY_MAX_RC_INSTANCES) {
		pr_err("%s: rc_idx %u out of range (max %u)\n",
		       __func__, rc_idx, MAHUA_PHY_MAX_RC_INSTANCES - 1);
		return NULL;
	}

	mutex_lock(&mahua_eom_locks_init_lock);
	if (!mahua_eom_locks_ready) {
		for (i = 0; i < MAHUA_PHY_MAX_RC_INSTANCES; i++)
			mutex_init(&mahua_eom_locks[i]);
		mahua_eom_locks_ready = true;
	}
	mutex_unlock(&mahua_eom_locks_init_lock);

	return &mahua_eom_locks[rc_idx];
}

/**
 * mahua_lock_instance_or_stop - acquire a per-RC instance lock, but give up
 * promptly if @lane is asked to stop while still waiting for it
 * @lock: per-RC-instance lock, as returned by mahua_get_instance_lock()
 * @lane: lane on whose behalf the lock is being acquired
 *
 * mahua_gen4/5_pcie_eom_sequence() hold @lock across an entire scan (up to
 * ~112 s / ~33 s at the ioctl-boundary dwell clamp; see the locking
 * rationale in their kdocs) so that a concurrent lane's register writes on
 * the same RC cannot interleave mid-measurement. A caller queued behind that
 * hold via a blind mutex_lock() has no way to notice its own eom_seq_stop
 * until it finally acquires the lock -- e.g. eom_lane_restart_scan() joining
 * a lane's scan thread via kthread_stop_put() could block for the full
 * worst-case scan duration if that thread was queued here rather than
 * actually running.
 *
 * Poll with mutex_trylock() instead of blocking on mutex_lock(), so a stop
 * request raised while still queued is noticed within one poll interval.
 * This only bounds the wait to *acquire* the lock -- once acquired, it is
 * still held across the whole scan exactly as before, so the
 * hardware-correctness rationale for that hold is unaffected.
 *
 * This function always runs on the scan kthread itself (called from
 * run_eom()), so mahua_eom_seq_should_stop()'s own kthread_should_stop() check
 * covers a kthread_stop() request regardless of whether the stopper
 * remembered to raise @lane's eom_seq_stop first -- the two callers that
 * bound this wait (eom_lane_restart_scan() and
 * eom_teardown_collect_tasks()) both happen to raise the flag before
 * calling kthread_stop_put(), but that ordering is a call-site convention,
 * not something this function needs to rely on.
 *
 * Return: 0 with @lock held, or -EINTR if @lane's stop flag (or the global
 *         stop request), or a kthread_stop() request, was raised before the
 *         lock could be acquired.
 */
static int mahua_lock_instance_or_stop(struct mutex *lock, struct eom_lane *lane)
{
	while (!mutex_trylock(lock)) {
		if (mahua_eom_seq_should_stop(lane))
			return -EINTR;
		usleep_range(1000, 2000);
	}

	return 0;
}

/**
 * mahua_eom_dwell_ms - Convert a lane's userspace-provided dwell time to a
 * clamped millisecond value
 * @dwell_time_us: lane->dwell_time_us, already clamped to
 *                 EOM_MAX_DWELL_TIME_US at the ioctl boundary
 *
 * Shared by the Gen4 and Gen5 eye-sweep loops. Re-clamps to
 * MAHUA_EOM_MAX_DWELL_MS independently of the ioctl-boundary clamp so this
 * loop's own bound is self-evident, and enforces a 1 ms floor so a very
 * small dwell_time_us does not collapse to a zero-length sleep.
 *
 * Return: Dwell time in milliseconds, in [1, MAHUA_EOM_MAX_DWELL_MS].
 */
static inline u32 mahua_eom_dwell_ms(u32 dwell_time_us)
{
	u32 dtime = dwell_time_us / 1000;

	if (dtime == 0)
		dtime = 1;
	if (dtime > MAHUA_EOM_MAX_DWELL_MS)
		dtime = MAHUA_EOM_MAX_DWELL_MS;

	return dtime;
}

/**
 * mahua_eom_write_entry - Persist one eye-diagram sample into a lane's
 * output buffer
 * @buffer: Lane's EOM output buffer
 * @rc_idx: RC instance index, for logging only
 * @lanenum: Lane number, for logging only
 * @entry: Sample to write
 *
 * eom_buffer_write() returns ssize_t, but only ever returns the byte count
 * written (always sizeof(*entry), a fixed compile-time constant) or one of
 * -EINVAL / -ENOMEM on error — all well within int range, so the cast below
 * cannot truncate.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_eom_write_entry(struct eom_buffer *buffer, u32 rc_idx, u32 lanenum,
				 const struct eom_entry *entry)
{
	ssize_t wret = eom_buffer_write(buffer, (char *)entry, sizeof(*entry));

	if (wret < 0) {
		pr_err("RC%d Lane:%d Failed to write EOM buffer x=%d y=%d: %zd\n",
		       rc_idx, lanenum, entry->x, entry->y, wret);
		return (int)wret;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * EOM initialisation sequences
 *
 * Pre-sequence: common to both upper and lower scans
 * -------------------------------------------------------------------------
 */
static const struct eom_reg_seq mahua_pcie4_eom_init_seq_pre[] = {
	{ PCIE_PHY_QSERDES_TX0_RESET_GEN_MUXES, PCIE4_MAHUA_TX_RESET_GEN_MUXES_VAL, 0 },
	{ PCIE_PHY_QSERDES_RX0_CDR_RESET_OVERRIDE, PCIE4_MAHUA_CDR_RESET_OVERRIDE_VAL, 0 },
};

/* Post-sequence: register writes that follow EOM_CTRL1 programming */
static const struct eom_reg_seq mahua_pcie4_eom_init_seq_post[] = {
	{ PCIE_PHY_QSERDES_RX0_AUX_CONTROL, PCIE4_MAHUA_AUX_CONTROL_MUX_BIT, 0 },
	{ PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL, PCIE4_MAHUA_RCLK_AUXDATA_SEL_ENABLE, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL2, PCIE4_MAHUA_RX_MARG_CTRL2_VAL, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3, PCIE4_MAHUA_RX_MARG_CTRL3_DEASSERT, 0 },
	{ PCIE_PHY_QSERDES_RX0_AUXDATA_TB, PCIE4_MAHUA_AUXDATA_TB_MUX_BIT, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_VERTICAL_CODE, 0x00, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3,
	  PCIE4_MAHUA_RX_MARG_CTRL3_ASSERT, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3, PCIE4_MAHUA_RX_MARG_CTRL3_STROBE, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3,
	  PCIE4_MAHUA_RX_MARG_CTRL3_ASSERT, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL,
	  PCIE4_MAHUA_RCLK_AUXDATA_SEL_ENABLE, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL, PCIE4_MAHUA_RCLK_AUXDATA_SEL_DISABLE, 0 },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3, PCIE4_MAHUA_RX_MARG_CTRL3_DEASSERT, 0 },
};

/* -------------------------------------------------------------------------
 * Per-sample sequence  (inner loop body for each (vth_control, ycoord, xcoord))
 * -------------------------------------------------------------------------
 */
static const struct eom_reg_seq mahua_pcie4_eom_sample_seq[] = {
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3,
	  PCIE4_MAHUA_RX_MARG_CTRL3_ASSERT, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3,
	  PCIE4_MAHUA_RX_MARG_CTRL3_STROBE, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3, PCIE4_MAHUA_RX_MARG_CTRL3_ASSERT, 0 },
	{ PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL,
	  PCIE4_MAHUA_RCLK_AUXDATA_SEL_ENABLE, EOM_REG_WRITE_DELAY },
	{ PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL, PCIE4_MAHUA_RCLK_AUXDATA_SEL_DISABLE, 0 },
};

/* =========================================================================
 * mahua_eom_write_seq – execute a flat register-write table
 * =========================================================================
 */
static int mahua_eom_write_seq(struct eom_phy_device *phy, u32 lanenum,
			       const struct eom_reg_seq *seq, size_t count)
{
	u32 rc_idx = phy->index;
	u32 reg_addr;
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		reg_addr = pcie4_mahua_phy_get_reg_offset(seq[i].offset, lanenum);
		ret = phy_write(phy, reg_addr, seq[i].value);
		if (ret < 0) {
			pr_err("RC%d: mahua seq write failed offset=0x%x val=0x%x at [%zu]\n",
			       rc_idx, seq[i].offset, seq[i].value, i);
			return ret;
		}
		if (seq[i].delay_ns)
			ndelay(seq[i].delay_ns);
	}

	return 0;
}

/**
 * mahua_gen4_write_eom_ctrl - Write EOM_CTRL1 (direction-dependent) and
 * EOM_CTRL2 (fixed) for one Gen4 lane
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @is_positive_seq: POSITIVE_SEQUENCE for the upper scan, NEGATIVE_SEQUENCE
 *                    for the lower scan
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_gen4_write_eom_ctrl(struct eom_phy_device *phy, u32 lanenum,
				     u32 is_positive_seq)
{
	u32 rc_idx = phy->index;
	u32 reg_addr, eom_ctrl1_val;
	int ret;

	eom_ctrl1_val = is_positive_seq ? PCIE4_MAHUA_EOM_CTRL1_UPPER
					: PCIE4_MAHUA_EOM_CTRL1_LOWER;
	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_EOM_CTRL1, lanenum);
	ret = phy_write(phy, reg_addr, eom_ctrl1_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to write EOM_CTRL1 for lane %d\n", rc_idx, lanenum);
		return ret;
	}

	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_EOM_CTRL2, lanenum);
	ret = phy_write(phy, reg_addr, PCIE4_MAHUA_EOM_CTRL2_VAL);
	if (ret < 0) {
		pr_err("RC%d: Failed to write EOM_CTRL2 for lane %d\n", rc_idx, lanenum);
		return ret;
	}

	return 0;
}

/* =========================================================================
 * mahua_gen4_eom_init – per-lane EOM PHY initialisation
 *
 * Mirrors the pcie_eom_init CMM subroutine.
 * is_positive_seq selects EOM_CTRL1 value:
 *   POSITIVE_SEQUENCE → 0x28  (upper/positive eye scan)
 *   NEGATIVE_SEQUENCE → 0x38  (lower/negative eye scan)
 *
 * Caller must hold the per-RC instance lock for @phy's RC index.
 *
 * Return: 0 on success; -EINTR if @lane->eom_seq_stop was already set on
 *         entry (no register writes are performed in that case); negative
 *         error code from PHY register I/O on failure.
 * =========================================================================
 */
static int mahua_gen4_eom_init(struct eom_phy_device *phy, struct eom_lane *lane,
			       u32 lanenum, u32 is_positive_seq)
{
	u32 rc_idx = phy->index;
	int ret;

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("RC%d Lane:%d EOM init: scan stop requested, aborting\n",
			 rc_idx, lanenum);
		return -EINTR;
	}

	pr_debug("RC%d EOM Initializing lane %d (Seq: %s)\n", rc_idx, lanenum,
		is_positive_seq ? "Upper/Positive" : "Lower/Negative");

	/* --- Pre-sequence (TX reset mux + CDR reset override) --- */
	ret = mahua_eom_write_seq(phy, lanenum,
				  mahua_pcie4_eom_init_seq_pre,
				  ARRAY_SIZE(mahua_pcie4_eom_init_seq_pre));
	if (ret < 0) {
		pr_err("RC%d: EOM pre-init failed for lane %d\n", rc_idx, lanenum);
		return ret;
	}

	ret = mahua_gen4_write_eom_ctrl(phy, lanenum, is_positive_seq);
	if (ret < 0)
		return ret;

	/* --- Post-sequence --- */
	ret = mahua_eom_write_seq(phy, lanenum,
				  mahua_pcie4_eom_init_seq_post,
				  ARRAY_SIZE(mahua_pcie4_eom_init_seq_post));
	if (ret < 0) {
		pr_err("RC%d: EOM post-init failed for lane %d\n", rc_idx, lanenum);
		return ret;
	}

	pr_debug("RC%d EOM Init done for lane %d\n", rc_idx, lanenum);

	return 0;
}

/**
 * mahua_gen4_program_sample_point - Program the AUXDATA_TB, RX_MARG_VERTICAL_CODE
 * and AUX_CONTROL registers for one Gen4 EOM sample point
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @vth_control: Outer vertical threshold control level [0..7]
 * @xcoord: Horizontal sweep index [0..63]
 * @ycoord: Mid-level vertical sweep index [0..10]
 *
 * Caller must hold the per-RC instance lock for @phy's RC index.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_gen4_program_sample_point(struct eom_phy_device *phy, u32 lanenum,
					   u32 vth_control, u32 xcoord, u32 ycoord)
{
	u32 rc_idx = phy->index;
	u32 reg_addr, val;
	int ret;

	val = vth_control | PCIE4_MAHUA_AUXDATA_TB_MUX_BIT;
	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_AUXDATA_TB, lanenum);
	ret = phy_write(phy, reg_addr, val);
	if (ret < 0) {
		pr_err("RC%d: Failed to write AUXDATA_TB\n", rc_idx);
		return ret;
	}

	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_RX_MARG_VERTICAL_CODE,
						  lanenum);
	ret = phy_write(phy, reg_addr, ycoord);
	if (ret < 0) {
		pr_err("RC%d: Failed to write RX_MARG_VERTICAL_CODE\n", rc_idx);
		return ret;
	}

	val = xcoord | PCIE4_MAHUA_AUX_CONTROL_MUX_BIT;
	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_AUX_CONTROL, lanenum);
	ret = phy_write(phy, reg_addr, val);
	if (ret < 0) {
		pr_err("RC%d: Failed to write AUX_CONTROL\n", rc_idx);
		return ret;
	}

	ndelay(EOM_REG_WRITE_DELAY);

	return 0;
}

/**
 * mahua_gen4_read_error_counter - Read and combine the Gen4 IA error counter
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @vth_control: Outer vertical threshold control level, for logging only
 * @xcoord: Horizontal sweep index, for logging only
 * @ycoord: Mid-level vertical sweep index, for logging only
 * @errorcntr: Output combined 16-bit error count
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_gen4_read_error_counter(struct eom_phy_device *phy, u32 lanenum,
					 u32 vth_control, u32 xcoord, u32 ycoord,
					 u32 *errorcntr)
{
	u32 rc_idx = phy->index;
	u32 temp_err_low, temp_err_high;
	u32 reg_addr;
	int ret;

	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_IA_ERROR_COUNTER_LOW,
						  lanenum);
	ret = phy_read(phy, reg_addr, &temp_err_low);
	if (ret < 0) {
		pr_err("RC%d Lane:%d: Failed to read IA_ERROR_COUNTER_LOW at vth=%d x=%d y=%d\n",
		       rc_idx, lanenum, vth_control, xcoord, ycoord);
		return ret;
	}

	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_IA_ERROR_COUNTER_HIGH,
						  lanenum);
	ret = phy_read(phy, reg_addr, &temp_err_high);
	if (ret < 0) {
		pr_err("RC%d Lane:%d: Failed to read IA_ERROR_COUNTER_HIGH at vth=%d x=%d y=%d\n",
		       rc_idx, lanenum, vth_control, xcoord, ycoord);
		return ret;
	}

	*errorcntr = (temp_err_low & 0xFF) | ((temp_err_high & 0xFF) << 8);
	if (*errorcntr == MAHUA_EOM_ERROR_COUNTER_SATURATED) {
		pr_warn_ratelimited("RC%d Lane:%d Error counter saturated at vth=%d x=%d y=%d\n",
				    rc_idx, lanenum, vth_control, xcoord, ycoord);
	}

	return 0;
}

/**
 * mahua_gen4_build_entry - Construct one Gen4 EOM entry from raw sweep
 * indices and an already-read error count
 * @vth_control: Outer vertical threshold control level [0..7]
 * @xcoord: Horizontal sweep index [0..63]
 * @ycoord: Mid-level vertical sweep index [0..10]
 * @is_positive_seq: POSITIVE_SEQUENCE for the upper scan, NEGATIVE_SEQUENCE
 *                    for the lower scan
 * @errorcntr: Combined IA error count for this sample
 * @entry: Output entry
 *
 * Effective y-index matches CMM:
 *   &temp2 = &ycoord + (PCIE4_MAHUA_VTH_Y_STRIDE * &vth_control)
 * Sign is applied based on scan direction.
 */
static void mahua_gen4_build_entry(u32 vth_control, u32 xcoord, u32 ycoord,
				   u32 is_positive_seq, u32 errorcntr,
				   struct eom_entry *entry)
{
	entry->x = (xcoord > (PCIE4_MAHUA_EYE_HALF_WIDTH - 1))
		   ? (s32)xcoord - PCIE4_MAHUA_MAX_EYE_WIDTH
		   : (s32)xcoord;
	entry->y = (s32)(ycoord + (PCIE4_MAHUA_VTH_Y_STRIDE * vth_control)) *
		   (is_positive_seq ? 1 : -1);
	entry->error_count = errorcntr;
}

/* =========================================================================
 * mahua_gen4_process_eye_sample
 *
 * Mahua-pcie4-specific sample loop body.
 *
 * Mahua-pcie4 introduces a two-level vertical scan:
 *   Outer: vth_control [0..7]  – written to AUXDATA_TB bits[2:0] | 0x08
 *   Mid  : ycoord     [0..10]  – written to RX_MARG_VERTICAL_CODE
 *   Inner: xcoord     [0..63]  – written to AUX_CONTROL bits[5:0] | 0x40
 *
 * The combined y-index stored in the EOM entry is:
 *   y_effective = ycoord + (PCIE4_MAHUA_VTH_Y_STRIDE * vth_control)
 * with sign applied for lower scan.
 *
 * Caller must hold the per-RC instance lock for @phy's RC index across the
 * full call, including across the msleep(dtime) dwell below: the dwell
 * period is the measurement window, and the sample-pulse sequence and the
 * counter read that bracket it must not be interleaved with a concurrent
 * lane's broadcast-adjacent writes on the same RC.
 * =========================================================================
 */
static int mahua_gen4_process_eye_sample(struct eom_lane *lane, struct eom_phy_device *phy,
					 u32 lanenum, u32 vth_control,
					 u32 xcoord, u32 ycoord, u32 dtime,
					 u32 is_positive_seq)
{
	u32 rc_idx = phy->index;
	struct eom_entry entry;
	u32 errorcntr;
	u32 reg_addr;
	int ret, deassert_ret;

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested before sample, aborting\n",
			 rc_idx, lanenum);
		return -EINTR;
	}

	if (!lane->buffer) {
		pr_err("RC%d Lane:%d EOM buffer not initialized\n", rc_idx, lanenum);
		return -EINVAL;
	}

	ret = mahua_gen4_program_sample_point(phy, lanenum, vth_control, xcoord, ycoord);
	if (ret < 0)
		return ret;

	/* Sample pulse + IA counter clear. */
	ret = mahua_eom_write_seq(phy, lanenum,
				  mahua_pcie4_eom_sample_seq,
				  ARRAY_SIZE(mahua_pcie4_eom_sample_seq));
	if (ret < 0) {
		pr_err("RC%d: Sample seq failed for lane %d\n", rc_idx, lanenum);
		/*
		 * mahua_pcie4_eom_sample_seq[]'s writes may have already left
		 * RX_MARG_CTRL3 asserted before this failure; go through the
		 * same deassert cleanup as every other exit path instead of
		 * stranding the PHY mid-margining-sequence.
		 */
		goto deassert;
	}

	/* Dwell wait: this IS the measurement window; see the function kdoc. */
	msleep(dtime);

	/*
	 * From here on, RX_MARG_CTRL3 is left asserted until the cleanup
	 * below runs — on every exit path, success or failure — so a failure
	 * never leaves the PHY with a half-applied margining sequence for
	 * the next sample or scan.
	 */
	ret = mahua_gen4_read_error_counter(phy, lanenum, vth_control, xcoord, ycoord,
					    &errorcntr);
	if (ret < 0)
		goto deassert;

	mahua_gen4_build_entry(vth_control, xcoord, ycoord, is_positive_seq, errorcntr, &entry);

	ret = mahua_eom_write_entry(lane->buffer, rc_idx, lanenum, &entry);

deassert:
	reg_addr = pcie4_mahua_phy_get_reg_offset(PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3, lanenum);
	deassert_ret = phy_write(phy, reg_addr, PCIE4_MAHUA_RX_MARG_CTRL3_DEASSERT);
	if (deassert_ret < 0)
		pr_err("RC%d: Failed to de-assert RX_MARG_CTRL3 update for lane %d\n",
		       rc_idx, lanenum);

	return ret < 0 ? ret : deassert_ret;
}

/* =========================================================================
 * mahua_gen4_eye_seq
 *
 * Implements the three-level scan loop from the Mahua-pcie4 CMM:
 *   WHILE vth_control < PCIE4_MAHUA_MAX_VTH_CONTROL (0..7)
 *     WHILE ycoord < PCIE4_MAHUA_MAX_EYE_HEIGHT     (0..10)
 *       WHILE xcoord < PCIE4_MAHUA_MAX_EYE_WIDTH    (0..63)
 *
 * eom_seq_stop is checked at two points only:
 *   1. On entry, before any loop iteration begins.
 *   2. Inside the innermost xcoord loop, after each call to
 *      mahua_gen4_process_eye_sample().  That callee checks the flag on its
 *      own entry and returns -EINTR (caught by the ret < 0 check right
 *      below the call) when set, so the inner check here catches the stop
 *      signal within one xcoord step — at most one no-op sample call is
 *      made before the loop exits. Checks at the ycoord and vth_control
 *      loop boundaries are omitted because the inner check already unwinds
 *      all three levels via the shared ret < 0 return path without any
 *      additional overhead.
 *
 * Caller must hold the per-RC instance lock for @lane's RC index across the
 * full call.
 *
 * Return: 0 on success; -EINTR if @lane->eom_seq_stop is (or becomes) set
 *         before the scan completes; negative error code from PHY register
 *         I/O on failure.
 * =========================================================================
 */
static int mahua_gen4_eye_seq(struct eom_lane *lane, u32 is_positive_seq, u32 lanenum)
{
	struct eom_phy_device *phy = lane->phy_dev;
	u32 vth_control, ycoord, xcoord;
	u32 dtime;
	int ret;

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("Lane:%d EOM eye seq: scan stop requested, aborting\n", lanenum);
		return -EINTR;
	}

	dtime = mahua_eom_dwell_ms(lane->dwell_time_us);

	for (vth_control = 0; vth_control < PCIE4_MAHUA_MAX_VTH_CONTROL; vth_control++) {
		for (ycoord = 0; ycoord < PCIE4_MAHUA_MAX_EYE_HEIGHT; ycoord++) {
			for (xcoord = 0; xcoord < PCIE4_MAHUA_MAX_EYE_WIDTH; xcoord++) {
				ret = mahua_gen4_process_eye_sample(lane, phy, lanenum,
								    vth_control, xcoord, ycoord,
								    dtime, is_positive_seq);
				if (ret < 0)
					return ret;
				if (mahua_eom_seq_should_stop(lane)) {
					pr_debug("Lane:%d EOM eye seq: scan stop requested mid-sweep, aborting\n",
						 lanenum);
					return -EINTR;
				}
			}
		}
	}

	return 0;
}

/**
 * mahua_gen4_run_direction - Init the PHY and run one full eye scan for one
 * Gen4 scan direction
 * @lane: EOM lane descriptor
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @is_positive_seq: POSITIVE_SEQUENCE for the upper scan, NEGATIVE_SEQUENCE
 *                    for the lower scan
 *
 * Caller must hold the per-RC instance lock for @phy's RC index across the
 * full call.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_gen4_run_direction(struct eom_lane *lane, struct eom_phy_device *phy,
				    u32 lanenum, u32 is_positive_seq)
{
	u32 rc_idx = phy->index;
	int ret;

	pr_debug("RC%d: Initializing PHY and running %s EOM scan\n", rc_idx,
		is_positive_seq ? "Upper/Positive" : "Lower/Negative");

	ret = mahua_gen4_eom_init(phy, lane, lanenum, is_positive_seq);
	if (ret < 0) {
		if (ret != -EINTR)
			pr_err("RC%d: EOM PHY init failed (%s): %d\n", rc_idx,
			       is_positive_seq ? "upper" : "lower", ret);
		return ret;
	}

	ret = mahua_gen4_eye_seq(lane, is_positive_seq, lanenum);
	if (ret < 0 && ret != -EINTR)
		pr_err("RC%d: EOM %s eye seq failed: %d\n", rc_idx,
		       is_positive_seq ? "upper" : "lower", ret);

	return ret;
}

/**
 * mahua_gen4_validate_lane - Validate lane/phy pointers and bounds for a
 * Gen4 EOM sequence call
 * @lane: EOM lane descriptor to validate
 * @phy: Output: @lane->phy_dev, once validated non-NULL
 * @rc_idx: Output: @phy->index, once validated in range
 * @lanenum: Output: @lane->lane_num, once validated in range
 *
 * Return: 0 on success, -EINVAL if any pointer or bound is invalid.
 */
static int mahua_gen4_validate_lane(struct eom_lane *lane, struct eom_phy_device **phy,
				    u32 *rc_idx, u32 *lanenum)
{
	if (!lane || !lane->phy_dev || !lane->seq) {
		pr_err("Mahua Gen4 EOM: Invalid lane/phy_dev/seq pointer\n");
		return -EINVAL;
	}

	*phy = lane->phy_dev;
	*rc_idx = (*phy)->index;

	if (*rc_idx >= MAHUA_PHY_MAX_RC_INSTANCES) {
		pr_err("RC%d: Invalid RC index, max supported: %d\n",
		       *rc_idx, MAHUA_PHY_MAX_RC_INSTANCES - 1);
		return -EINVAL;
	}

	/*
	 * Validate lane_num before it reaches pcie4_mahua_phy_get_reg_offset().
	 * lane->lane_num is signed 'int'; copy it into a u32 first (as the
	 * Gen5 path already does) so a negative value is rejected by this
	 * unsigned comparison instead of silently overflowing the u32 offset
	 * arithmetic in pcie4_mahua_phy_get_reg_offset(), which has no
	 * internal bounds check and would otherwise write to an arbitrary
	 * register offset on hardware.
	 */
	*lanenum = lane->lane_num;
	if (*lanenum >= PCIE4_MAHUA_MAX_NUM_LANES) {
		pr_err("RC%d: Invalid lane number %u, max supported: %u\n",
		       *rc_idx, *lanenum, PCIE4_MAHUA_MAX_NUM_LANES - 1);
		return -EINVAL;
	}

	return 0;
}

/**
 * mahua_gen4_pcie_eom_sequence - Run the full Gen4 EOM eye scan for one lane
 * @lane: EOM lane descriptor. Must be fully initialised before calling:
 *        @lane->phy_dev and @lane->seq must be non-NULL; @lane->lane_num must
 *        be in [0, PCIE4_MAHUA_MAX_NUM_LANES); @lane->dwell_time_us sets the
 *        per-sample dwell period; @lane->buffer must be ready to accept
 *        eom_entry writes; @lane->eom_seq_stop may be set asynchronously to
 *        abort the scan early.
 *
 * Overrides the weak fallback in eom_driver.c when CONFIG_PCI_MSM_EOM is
 * enabled. Runs an Upper (positive) eye scan followed by a Lower (negative)
 * eye scan for the lane identified by @lane->lane_num.
 *
 * The per-RC instance mutex is held for the *entire* scan, including across
 * both directions and every msleep() dwell inside them: EOM_CTRL1/2, the AUX
 * mux/RCLK overrides written by mahua_gen4_eom_init(), and the per-sample
 * trigger/dwell/read block are all state shared by every lane on the RC
 * instance, so a concurrent lane scan interleaving its own writes mid-dwell
 * would corrupt this lane's measurement. Holding a mutex across msleep() is
 * legal here because the caller is a kthread running in process context.
 * This serialises all lane scans on a given RC instance to run one at a
 * time; see the module-level correctness-over-throughput rationale in the
 * equivalent Gen5 function.
 *
 * Worst-case lock-hold duration: PCIE4_MAHUA_MAX_VTH_CONTROL (8) *
 * PCIE4_MAHUA_MAX_EYE_HEIGHT (11) * PCIE4_MAHUA_MAX_EYE_WIDTH (64) samples
 * per direction, times 2 directions, times the dwell ceiling that
 * dwell_time_us is actually clamped to before it ever reaches
 * mahua_eom_dwell_ms() — EOM_MAX_DWELL_TIME_US (10 ms) at the ioctl
 * boundary in eom_driver.c — is approximately 112 s, plus a small
 * per-sample register I/O overhead on top. (MAHUA_EOM_MAX_DWELL_MS's own
 * 500 ms ceiling in mahua_eom_dwell_ms() is a second, defensive clamp that
 * is unreachable in practice as long as the ioctl-boundary clamp stays in
 * place; if it were ever the effective bound instead, the same product
 * scales to roughly 94 minutes.) Narrowing the lock's scope below "whole
 * scan" was considered and rejected: doing so would let a concurrent
 * lane's writes on the same RC interleave mid-measurement (see the
 * paragraph above), which is a correctness regression that cannot be
 * validated without real hardware.
 *
 * Note: This function sleeps. msleep() is called once per sample point
 * inside the innermost scan loop (up to vth_control × ycoord × xcoord
 * iterations per direction, multiplied by the dwell time derived from
 * @lane->dwell_time_us). It must be called from process context and must
 * not be called with any spinlock held.
 *
 * Neither this scan nor its Gen5 counterpart restores the PHY to its
 * pre-EOM operational configuration on completion, error, or early abort
 * (see phy_pcie_eom_sequence()'s kdoc for the full contract).
 *
 * Return: 0 on success; -EINTR if the scan is aborted early via
 *         @lane->eom_seq_stop; -EINVAL if @lane, @lane->phy_dev, or
 *         @lane->seq is NULL, if the RC index is out of range, or if
 *         @lane->lane_num exceeds PCIE4_MAHUA_MAX_NUM_LANES; negative
 *         error code from PHY register I/O on hardware failure.
 */
static int mahua_gen4_pcie_eom_sequence(struct eom_lane *lane)
{
	struct eom_phy_device *phy;
	struct mutex *lock; /* per-RC instance lock, shared across lanes of the same RC */
	u32 rc_idx, lanenum;
	int ret;

	ret = mahua_gen4_validate_lane(lane, &phy, &rc_idx, &lanenum);
	if (ret < 0)
		return ret;

	lock = mahua_get_instance_lock(rc_idx);
	if (!lock) {
		pr_err("RC%d: Failed to get instance lock\n", rc_idx);
		return -EINVAL;
	}

	pr_debug("Running Mahua PCIe Gen4 EOM for %s instance %u lane %d\n",
		lane->seq->name, rc_idx, lanenum);

	ret = mahua_lock_instance_or_stop(lock, lane);
	if (ret < 0) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested while waiting for instance lock\n",
			 rc_idx, lanenum);
		return ret;
	}
	lockdep_assert_held(lock);

	ret = mahua_gen4_run_direction(lane, phy, lanenum, POSITIVE_SEQUENCE);
	if (ret < 0) {
		mutex_unlock(lock);
		return ret;
	}

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested between directions, aborting\n",
			 rc_idx, lanenum);
		mutex_unlock(lock);
		return -EINTR;
	}

	ndelay(PCIE4_MAHUA_SCAN_DIRECTION_SETTLE_NS);

	ret = mahua_gen4_run_direction(lane, phy, lanenum, NEGATIVE_SEQUENCE);
	mutex_unlock(lock);
	if (ret < 0)
		return ret;

	pr_debug("RC%d: Mahua Gen4 EOM scan completed for lane %d\n", rc_idx, lanenum);

	return 0;
}

/**
 * mahua_read_phy_reg - Read from appropriate PHY based on lane number
 * @phy: EOM PHY device
 * @reg_offset: Register offset (unadjusted for broadcast regs, pre-adjusted
 *              for per-lane regs)
 * @lanenum: Lane number (0-based)
 * @val: Pointer to store read value
 *
 * For bifurcated configurations, lanes 0-3 use PHY A and lanes 4-7 use PHY B.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_read_phy_reg(struct eom_phy_device *phy, u32 reg_offset,
			      u32 lanenum, u32 *val)
{
	if (phy_has_phy_b(phy) && lanenum >= MAHUA_PHY_B_LANE_START)
		return phy_b_read(phy, reg_offset, val);

	return phy_read(phy, reg_offset, val);
}

/**
 * mahua_write_phy_reg - Write to appropriate PHY based on lane number
 * @phy: EOM PHY device
 * @reg_offset: Register offset (unadjusted for broadcast regs, pre-adjusted
 *              for per-lane regs)
 * @lanenum: Lane number (0-based)
 * @val: Value to write
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_write_phy_reg(struct eom_phy_device *phy, u32 reg_offset,
			       u32 lanenum, u32 val)
{
	if (phy_has_phy_b(phy) && lanenum >= MAHUA_PHY_B_LANE_START)
		return phy_b_write(phy, reg_offset, val);

	return phy_write(phy, reg_offset, val);
}

/**
 * mahua_phy_get_reg_offset - Calculate per-lane register offset with
 * PHY B adjustment
 * @phy: EOM PHY device
 * @base_reg: Base per-lane (TXRX0) register address
 * @lanenum: Lane number (0-based)
 *
 * Return: Adjusted register offset
 */
static inline u32 mahua_phy_get_reg_offset(struct eom_phy_device *phy,
					   u32 base_reg, u32 lanenum)
{
	u32 adjusted_lanenum = lanenum;

	if (phy_has_phy_b(phy) && lanenum >= MAHUA_PHY_B_LANE_START)
		adjusted_lanenum = lanenum - MAHUA_PHY_B_LANE_START;

	return MAHUA_PHY_REG_ADDR(base_reg, adjusted_lanenum);
}

/**
 * struct mahua_gen5_rmw_seq - One RMW entry in the Gen5 PHY init table
 * @reg:  Absolute broadcast (TXRXZ) register address
 * @mask: AND mask applied to the hardware read value to preserve other bits
 * @val:  OR value ORed in after masking to set the target bits
 *
 * The RMW formula applied by mahua_gen5_rmw_seq_exec() is:
 *   write_val = (read_val & mask) | val
 *
 * For entries where only bits are set and none cleared, mask is 0xFF so
 * that (read_val & 0xFF) | val == read_val | val.
 */
struct mahua_gen5_rmw_seq {
	u32 reg;
	u32 mask;
	u32 val;
};

/*
 * mahua_gen5_init_step - CMM script order for mahua_gen5_init_rmw_seq[]
 *
 * Used as designated-initializer indices below so the array's execution
 * order (mahua_gen5_rmw_seq_exec() always iterates index 0..count-1) is
 * tied to this enum rather than to source line order alone: reordering the
 * initializers on the page below without updating their designators would
 * leave the runtime step order unchanged, and a duplicate or out-of-range
 * designator is a compile error instead of a silent CMM-order mismatch.
 */
enum mahua_gen5_init_step {
	MAHUA_GEN5_INIT_RESET_GEN,
	MAHUA_GEN5_INIT_CDR_EN_RXEQ_RESET,
	MAHUA_GEN5_INIT_CDR_VCO_CAL_CTRL,
	MAHUA_GEN5_INIT_CDR_IQTUNE_ANA_CTRL,
	MAHUA_GEN5_INIT_AUXDATA_TB,
	MAHUA_GEN5_INIT_AUX_CLK_CTRL,
	MAHUA_GEN5_INIT_EOM_CTRL1,
	MAHUA_GEN5_INIT_AUX_OFFSET_CONTROL,
	MAHUA_GEN5_INIT_STEP_COUNT
};

/*
 * mahua_gen5_init_rmw_seq - Gen5 PHY EOM initialisation RMW table
 *
 * Each entry mirrors one read-modify-write step from the CMM script.
 * Registers are TXRXZ broadcast addresses; no lane stride is applied.
 * Entries must remain in CMM script order: RESET_GEN -> CDR_EN_RXEQ_RESET
 * -> CDR_VCO_CAL_CTRL -> CDR_IQTUNE_ANA_CTRL -> AUXDATA_TB -> AUX_CLK_CTRL
 * -> EOM_CTRL1 -> AUX_OFFSET_CONTROL; see enum mahua_gen5_init_step, whose
 * designators below are what actually pins each entry to its slot.
 *
 * Most entries below have mask == val. Per the RMW formula in the struct's
 * kdoc, write_val = (read_val & mask) | val: when mask and val are equal,
 * every masked-in bit is forced to 1 (read_bit | 1) and every masked-out
 * bit is forced to 0 (read_bit & 0 | 0), so the result no longer depends on
 * read_val at all — these steps are effectively direct writes of the
 * constant, kept as an RMW only to match the CMM script's literal form.
 */
static const struct mahua_gen5_rmw_seq mahua_gen5_init_rmw_seq[] = {
	[MAHUA_GEN5_INIT_RESET_GEN] = {
		PCIE_PHY_QSERDES_TXRXZ_RESET_GEN,
		MAHUA_RESET_GEN_MASK,          MAHUA_RESET_GEN_VAL },
	[MAHUA_GEN5_INIT_CDR_EN_RXEQ_RESET] = {
		PCIE_PHY_QSERDES_TXRXZ_CDR_EN_RXEQ_RESET,
		MAHUA_CDR_EN_RXEQ_RESET_MASK,  MAHUA_CDR_EN_RXEQ_RESET_VAL },
	[MAHUA_GEN5_INIT_CDR_VCO_CAL_CTRL] = {
		PCIE_PHY_QSERDES_TXRXZ_CDR_VCO_CAL_CTRL,
		MAHUA_CDR_VCO_CAL_CTRL_MASK,   MAHUA_CDR_VCO_CAL_CTRL_VAL },
	/*
	 * CDR_IQTUNE_ANA_CTRL: only BIT(3) is set; no bits are cleared.
	 * mask=0xFF preserves all existing bits: (val & 0xFF) | BIT(3)
	 * is identical to val | BIT(3).
	 */
	[MAHUA_GEN5_INIT_CDR_IQTUNE_ANA_CTRL] = {
		PCIE_PHY_QSERDES_TXRXZ_CDR_IQTUNE_ANA_CTRL,
		0xFF,                           MAHUA_CDR_IQTUNE_IQ_CLK_SEL_BIT },
	[MAHUA_GEN5_INIT_AUXDATA_TB] = {
		PCIE_PHY_QSERDES_TXRXZ_AUXDATA_TB,
		MAHUA_AUXDATA_TB_MUX_MASK,      MAHUA_AUXDATA_TB_MUX_VAL },
	[MAHUA_GEN5_INIT_AUX_CLK_CTRL] = {
		PCIE_PHY_QSERDES_TXRXZ_AUX_CLK_CTRL,
		MAHUA_AUX_CLK_CTRL_MASK,        MAHUA_AUX_CLK_CTRL_VAL },
	[MAHUA_GEN5_INIT_EOM_CTRL1] = {
		PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1,
		MAHUA_EOM_CTRL1_INIT_MASK,      MAHUA_EOM_CTRL1_INIT_VAL },
	[MAHUA_GEN5_INIT_AUX_OFFSET_CONTROL] = {
		PCIE_PHY_QSERDES_TXRXZ_AUX_OFFSET_CONTROL,
		MAHUA_AUX_OFFSET_CTRL_MUX_MASK, MAHUA_AUX_OFFSET_CTRL_MUX_VAL },
};

static_assert(ARRAY_SIZE(mahua_gen5_init_rmw_seq) == MAHUA_GEN5_INIT_STEP_COUNT,
	      "mahua_gen5_init_rmw_seq[] is missing an enum mahua_gen5_init_step entry");

/**
 * mahua_gen5_rmw_seq_exec - Execute a Gen5 RMW initialisation table
 * @phy:    EOM PHY device
 * @lanenum: Lane number, used only to route reads/writes to PHY-A or PHY-B
 * @seq:    Pointer to the first entry of the RMW table
 * @count:  Number of entries in @seq
 *
 * For each entry, reads the register, applies (read_val & mask) | val,
 * and writes the result back.  All registers in the table are TXRXZ
 * broadcast addresses; @lanenum is passed to mahua_read_phy_reg() and
 * mahua_write_phy_reg() solely for PHY-A/B routing — no lane stride is
 * applied to the address.
 *
 * Caller must hold the instance lock for @phy's RC index.
 *
 * Return: 0 on success, negative error code on the first failing step.
 */
static int mahua_gen5_rmw_seq_exec(struct eom_phy_device *phy, u32 lanenum,
				   const struct mahua_gen5_rmw_seq *seq,
				   size_t count)
{
	u32 rc_idx = phy->index;
	u32 reg_val, new_val;
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = mahua_read_phy_reg(phy, seq[i].reg, lanenum, &reg_val);
		if (ret < 0) {
			pr_err("RC%d: Gen5 init RMW read failed reg=0x%x at [%zu]\n",
			       rc_idx, seq[i].reg, i);
			return ret;
		}
		new_val = (reg_val & seq[i].mask) | seq[i].val;
		ret = mahua_write_phy_reg(phy, seq[i].reg, lanenum, new_val);
		if (ret < 0) {
			pr_err("RC%d: Gen5 init RMW write failed reg=0x%x val=0x%x at [%zu]\n",
			       rc_idx, seq[i].reg, new_val, i);
			return ret;
		}
	}

	return 0;
}

/**
 * mahua_gen5_init - One-time broadcast PHY initialization for EOM
 * @phy: EOM PHY device
 * @lanenum: Lane number, used only to route to the correct PHY (A/B)
 *
 * Iterates mahua_gen5_init_rmw_seq[] via mahua_gen5_rmw_seq_exec(),
 * applying each RMW step in CMM script order.  The mask/val pairs
 * mirror the CMM exactly — only the designated bits are changed and
 * all boot-time values in the remaining bits are preserved.
 *
 * Caller must hold the instance lock for @phy's RC index.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_gen5_init(struct eom_phy_device *phy, u32 lanenum)
{
	return mahua_gen5_rmw_seq_exec(phy, lanenum,
				       mahua_gen5_init_rmw_seq,
				       ARRAY_SIZE(mahua_gen5_init_rmw_seq));
}

/**
 * mahua_update_aux_offset - Trigger AUX offset code update (broadcast)
 * @phy: EOM PHY device
 * @lanenum: Lane number, used only to route to the correct PHY (A/B)
 *
 * Sets bit 6 (AUX_OFFSET_CODE_UPDATE) in EOM_CTRL1 via RMW.
 * CMM: reg_val | (1 << 6)
 *
 * Caller must hold the instance lock for @phy's RC index.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_update_aux_offset(struct eom_phy_device *phy, u32 lanenum)
{
	u32 rc_idx = phy->index;
	u32 reg_val;
	int ret;

	ret = mahua_read_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to read EOM_CTRL1\n", rc_idx, lanenum);
		return ret;
	}
	ret = mahua_write_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum,
				  reg_val | MAHUA_EOM_CTRL1_UPDATE_BIT);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to write EOM_CTRL1\n", rc_idx, lanenum);
		return ret;
	}

	return 0;
}

/**
 * mahua_write_phy_reg_retry - Write a broadcast PHY register, retrying once
 * on failure
 * @phy: EOM PHY device
 * @reg_offset: Register offset
 * @lanenum: Lane number, used only to route to the correct PHY (A/B)
 * @val: Value to write
 * @action: Short description of the write, for the initial failure log
 * @stuck_desc: Description of the stuck hardware state, logged only if the
 *              retry also fails
 *
 * A single transient write failure must not permanently strand the PHY in
 * a half-applied state (e.g. AUX_CLK_RESET left asserted, or
 * IACOMPARE_CLEAR left set), so one best-effort retry with the same value
 * is attempted before giving up.
 *
 * Return: 0 on success, negative error code from the first failed write
 *         (regardless of whether the retry subsequently succeeds — the
 *         caller treats this call as failed either way).
 */
static int mahua_write_phy_reg_retry(struct eom_phy_device *phy, u32 reg_offset,
				     u32 lanenum, u32 val, const char *action,
				     const char *stuck_desc)
{
	u32 rc_idx = phy->index;
	int ret = mahua_write_phy_reg(phy, reg_offset, lanenum, val);

	if (ret < 0) {
		int retry_ret;

		pr_err("RC%d lane %u: Failed to %s (err %d), retrying\n",
		       rc_idx, lanenum, action, ret);
		retry_ret = mahua_write_phy_reg(phy, reg_offset, lanenum, val);
		if (retry_ret < 0)
			pr_err("RC%d lane %u: %s retry also failed (err %d); %s\n",
			       rc_idx, lanenum, action, retry_ret, stuck_desc);
	}

	return ret;
}

/**
 * mahua_assert_deassert_aux_clk_reset_clear_ia_err - Toggle AUX clk reset and
 * clear the IA error counter (broadcast)
 * @phy: EOM PHY device
 * @lanenum: Lane number, used only to route to the correct PHY (A/B)
 *
 * All EOM_CTRL1/AUX_CLK_CTRL writes here use RMW, matching the CMM script:
 *   - Assert:   AUX_CLK_CTRL |= MAHUA_AUX_CLK_RESET_BIT
 *   - Deassert: AUX_CLK_CTRL &= ~MAHUA_AUX_CLK_RESET_BIT
 *   - Clear IA: EOM_CTRL1 =
 *               (reg_val & MAHUA_EOM_CTRL1_CLEAR_IA_MASK) | MAHUA_EOM_CTRL1_CLEAR_IA_VAL
 *               (sets bit 6)
 *   - Restore:  EOM_CTRL1 =
 *               (reg_val & MAHUA_EOM_CTRL1_INIT_MASK) | MAHUA_EOM_CTRL1_INIT_VAL
 *               (clears bit 6)
 *
 * The deassert and restore steps reuse the value this same function just
 * wrote for the assert/clear-IA step instead of re-reading the register:
 * the caller holds the instance lock for the whole call, and no other code
 * path touches these broadcast registers while it is held. If the deassert
 * or restore write itself fails, mahua_write_phy_reg_retry() makes one
 * best-effort retry (with the known-good value) before returning the
 * original error, so a single transient failure does not permanently
 * strand the PHY with AUX_CLK_RESET asserted or IACOMPARE_CLEAR set.
 *
 * Caller must hold the instance lock for @phy's RC index.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_assert_deassert_aux_clk_reset_clear_ia_err(struct eom_phy_device *phy,
							    u32 lanenum)
{
	u32 rc_idx = phy->index;
	u32 reg_val;
	int ret;

	/* Assert AUX_CLK_RESET (bit 2). CMM: reg_val | (1 << 2) */
	ret = mahua_read_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_AUX_CLK_CTRL, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to read AUX_CLK_CTRL\n", rc_idx, lanenum);
		return ret;
	}
	reg_val |= MAHUA_AUX_CLK_RESET_BIT;
	ret = mahua_write_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_AUX_CLK_CTRL, lanenum, reg_val);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to assert AUX_CLK_CTRL reset\n", rc_idx, lanenum);
		return ret;
	}
	ndelay(EOM_REG_WRITE_DELAY);

	/* Deassert AUX_CLK_RESET (bit 2); reuse the value just written above. */
	reg_val &= ~MAHUA_AUX_CLK_RESET_BIT;
	ret = mahua_write_phy_reg_retry(phy, PCIE_PHY_QSERDES_TXRXZ_AUX_CLK_CTRL, lanenum,
					reg_val, "deassert AUX_CLK_CTRL reset",
					"AUX clock may be stuck in reset");
	if (ret < 0)
		return ret;

	/*
	 * Clear IA counter: set bit 6 (IACOMPARE_CLEAR) in EOM_CTRL1.
	 * CMM: (reg_val & MAHUA_EOM_CTRL1_CLEAR_IA_MASK) | MAHUA_EOM_CTRL1_CLEAR_IA_VAL
	 */
	ret = mahua_read_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to read EOM_CTRL1 (clear-IA)\n", rc_idx);
		return ret;
	}
	reg_val = (reg_val & MAHUA_EOM_CTRL1_CLEAR_IA_MASK) | MAHUA_EOM_CTRL1_CLEAR_IA_VAL;
	ret = mahua_write_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum, reg_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to write EOM_CTRL1 (clear-IA)\n", rc_idx);
		return ret;
	}
	ndelay(EOM_REG_WRITE_DELAY);

	/* Restore EOM_CTRL1: clear bit 6; reuse the value just written above. */
	reg_val = (reg_val & MAHUA_EOM_CTRL1_INIT_MASK) | MAHUA_EOM_CTRL1_INIT_VAL;

	return mahua_write_phy_reg_retry(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum,
					 reg_val, "restore EOM_CTRL1",
					 "IACOMPARE_CLEAR bit may remain stuck set");
}

/**
 * mahua_restore_eom_ctrl1_update_bit - clear EOM_CTRL1's bit 6 after a failed
 * assert/deassert call that followed a successful mahua_update_aux_offset()
 * @phy: EOM PHY device
 * @lanenum: Lane number, used only to route to the correct PHY (A/B)
 *
 * mahua_update_aux_offset() sets EOM_CTRL1 bit 6 (AUX_OFFSET_CODE_UPDATE);
 * mahua_assert_deassert_aux_clk_reset_clear_ia_err() normally clears the same
 * bit 6 (as IACOMPARE_CLEAR) as its own last step. If that second call fails
 * before reaching its restore step, bit 6 is left stuck set for the next
 * sample. Callers must invoke this only when mahua_update_aux_offset()
 * itself succeeded and the following assert/deassert call then failed --
 * if mahua_update_aux_offset() itself failed, EOM_CTRL1 was never changed
 * and there is nothing to restore.
 */
static void mahua_restore_eom_ctrl1_update_bit(struct eom_phy_device *phy, u32 lanenum)
{
	u32 rc_idx = phy->index;
	u32 reg_val;

	if (mahua_read_phy_reg(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum, &reg_val) < 0) {
		pr_err("RC%d: Failed to read EOM_CTRL1 for post-failure restore\n", rc_idx);
		return;
	}
	reg_val = (reg_val & MAHUA_EOM_CTRL1_INIT_MASK) | MAHUA_EOM_CTRL1_INIT_VAL;
	mahua_write_phy_reg_retry(phy, PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1, lanenum, reg_val,
				  "restore EOM_CTRL1 after aux-offset failure",
				  "EOM_CTRL1 UPDATE_BIT may remain stuck set");
}

/**
 * mahua_set_vth_aux_offset_direction - Set vertical threshold and AUX offset
 * direction for a lane based on the sign of the sample coordinates
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @xcoord: Signed, remapped horizontal coordinate
 * @ycoord: Signed, remapped vertical coordinate
 *
 * EOM_CTRL2 and EOM_CTRL3 are direct writes (full byte is known from CMM).
 * CDR_IQTUNE_ANA_CTRL bit 2 (AUX offset direction) is set via RMW.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_set_vth_aux_offset_direction(struct eom_phy_device *phy, u32 lanenum,
					      s32 xcoord, s32 ycoord)
{
	u32 rc_idx = phy->index;
	u32 reg_addr, reg_val;
	int ret;

	/*
	 * EOM_CTRL2: direct write — full byte is determined by y-direction.
	 * ycoord > 0: 0xD8 (VTH_DIR_MUX=1, VTH_DIR=1, DTAP1_E=1, EOM_MODE=0x18)
	 * ycoord <= 0: 0x98 (VTH_DIR_MUX=1, VTH_DIR=0, DTAP1_E=1, EOM_MODE=0x18)
	 */
	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_EOM_CTRL2, lanenum);
	ret = mahua_write_phy_reg(phy, reg_addr, lanenum,
				  ycoord > 0 ? MAHUA_EOM_CTRL2_POS_Y
					     : MAHUA_EOM_CTRL2_NEG_Y);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to write EOM_CTRL2\n", rc_idx, lanenum);
		return ret;
	}

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_EOM_CTRL3, lanenum);
	ret = mahua_write_phy_reg(phy, reg_addr, lanenum, MAHUA_EOM_CTRL3_VAL);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to write EOM_CTRL3\n", rc_idx, lanenum);
		return ret;
	}

	/*
	 * CDR_IQTUNE_ANA_CTRL bit 2: AUX offset direction.
	 * CMM: xcoord < 0 → reg_val | (1<<2), else → reg_val & ~(1<<2)
	 */
	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_CDR_IQTUNE_ANA_CTRL,
					    lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to read CDR_IQTUNE_ANA_CTRL\n", rc_idx, lanenum);
		return ret;
	}
	ret = mahua_write_phy_reg(phy, reg_addr, lanenum,
				  xcoord < 0 ? reg_val | MAHUA_CDR_IQTUNE_AUX_DIR_BIT
					     : reg_val & ~MAHUA_CDR_IQTUNE_AUX_DIR_BIT);
	if (ret < 0) {
		pr_err("RC%d lane %u: Failed to write CDR_IQTUNE_ANA_CTRL\n", rc_idx, lanenum);
		return ret;
	}

	return 0;
}

/**
 * mahua_set_sampling_point - Program the vertical/horizontal sampling point
 * for a lane
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @xcoord: Signed, remapped horizontal coordinate
 * @ycoord: Signed, remapped vertical coordinate
 *
 * Both writes use RMW: only the data bits [6:0] / [5:0] are updated;
 * the MUX bits (7 and 6) are preserved from the hardware register. The
 * abs() magnitude is explicitly masked to the destination field width
 * before being ORed in, so a caller-supplied coordinate wider than the
 * field cannot bleed into the preserved MUX bits.
 * CMM: AUXDATA_TB      = (reg_val & ~MAHUA_AUXDATA_TB_DATA_MASK) |
 *                        ((u32)abs(ycoord) & MAHUA_AUXDATA_TB_DATA_MASK)
 *      AUX_OFFSET_CTRL = (reg_val & ~MAHUA_AUX_OFFSET_DATA_MASK) |
 *                        ((u32)abs(xcoord) & MAHUA_AUX_OFFSET_DATA_MASK)
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_set_sampling_point(struct eom_phy_device *phy, u32 lanenum,
				    s32 xcoord, s32 ycoord)
{
	u32 rc_idx = phy->index;
	u32 reg_addr, reg_val;
	int ret;

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_AUXDATA_TB, lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to read AUXDATA_TB\n", rc_idx);
		return ret;
	}
	ret = mahua_write_phy_reg(phy, reg_addr, lanenum,
				  (reg_val & ~MAHUA_AUXDATA_TB_DATA_MASK) |
				  ((u32)abs(ycoord) & MAHUA_AUXDATA_TB_DATA_MASK));
	if (ret < 0) {
		pr_err("RC%d: Failed to write AUXDATA_TB\n", rc_idx);
		return ret;
	}

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_AUX_OFFSET_CONTROL,
					    lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &reg_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to read AUX_OFFSET_CONTROL\n", rc_idx);
		return ret;
	}
	ret = mahua_write_phy_reg(phy, reg_addr, lanenum,
				  (reg_val & ~MAHUA_AUX_OFFSET_DATA_MASK) |
				  ((u32)abs(xcoord) & MAHUA_AUX_OFFSET_DATA_MASK));
	if (ret < 0) {
		pr_err("RC%d: Failed to write AUX_OFFSET_CONTROL\n", rc_idx);
		return ret;
	}

	return 0;
}

/**
 * mahua_read_ia_error - Read the IA error counter for a lane and store the
 * eye sample in the lane's buffer
 * @lane: EOM lane structure
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @xcoord: Signed, remapped horizontal coordinate
 * @ycoord: Signed, remapped vertical coordinate
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_read_ia_error(struct eom_lane *lane, struct eom_phy_device *phy,
			       u32 lanenum, s32 xcoord, s32 ycoord)
{
	u32 rc_idx = phy->index;
	u32 err_low, err_high;
	struct eom_entry entry;
	u32 reg_addr;
	int ret;

	/*
	 * Reject the call immediately if the lane's output buffer has not been
	 * initialised or has been torn down mid-scan. Proceeding without this
	 * check would pass NULL to eom_buffer_write(), which is undefined
	 * behaviour. The hardware counter reads that precede the write would
	 * also be wasted work. This mirrors the identical guard in the Gen4
	 * path (mahua_gen4_process_eye_sample()) and must remain the first
	 * substantive check in this function.
	 */
	if (!lane->buffer) {
		pr_err("RC%d Lane:%d EOM buffer not initialized\n", rc_idx, lanenum);
		return -EINVAL;
	}

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_IA_ERROR_COUNTER_LOW,
					    lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &err_low);
	if (ret < 0) {
		pr_err("RC%d Lane:%d: Failed to read IA_ERROR_COUNTER_LOW at x=%d y=%d\n",
		       rc_idx, lanenum, xcoord, ycoord);
		return ret;
	}

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_IA_ERROR_COUNTER_HIGH,
					    lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &err_high);
	if (ret < 0) {
		pr_err("RC%d Lane:%d: Failed to read IA_ERROR_COUNTER_HIGH at x=%d y=%d\n",
		       rc_idx, lanenum, xcoord, ycoord);
		return ret;
	}

	entry.x = xcoord;
	entry.y = ycoord;
	entry.error_count = (err_low & 0xFF) | ((err_high & 0xFF) << 8);

	return mahua_eom_write_entry(lane->buffer, rc_idx, lanenum, &entry);
}

/**
 * mahua_process_eye_sample - Program and sample a single gated eye point
 * @lane: EOM lane structure
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @xcoord: Signed, remapped horizontal coordinate
 * @ycoord: Signed, remapped vertical coordinate
 * @dtime: Dwell time in milliseconds
 *
 * Caller must hold the per-RC instance lock for @phy's RC index across the
 * full call, including across the msleep(dtime) dwell below: the dwell
 * period is the measurement window, and the broadcast trigger
 * (mahua_update_aux_offset() / mahua_assert_deassert_aux_clk_reset_clear_ia_err())
 * that starts it and the counter read that ends it must not be interleaved
 * with a concurrent lane's own broadcast trigger on the same RC — see the
 * locking rationale in mahua_gen5_pcie_eom_sequence().
 *
 * Return: 0 on success, negative error code on failure
 */
static int mahua_process_eye_sample(struct eom_lane *lane, struct eom_phy_device *phy,
				    u32 lanenum, s32 xcoord, s32 ycoord, u32 dtime)
{
	int ret;

	ret = mahua_set_vth_aux_offset_direction(phy, lanenum, xcoord, ycoord);
	if (ret < 0)
		return ret;
	ret = mahua_set_sampling_point(phy, lanenum, xcoord, ycoord);
	if (ret < 0)
		return ret;
	ret = mahua_update_aux_offset(phy, lanenum);
	if (ret >= 0) {
		ret = mahua_assert_deassert_aux_clk_reset_clear_ia_err(phy, lanenum);
		if (ret < 0)
			mahua_restore_eom_ctrl1_update_bit(phy, lanenum);
	}
	if (ret < 0)
		return ret;

	/* Dwell wait: this IS the measurement window; see the function kdoc. */
	msleep(dtime);

	return mahua_read_ia_error(lane, phy, lanenum, xcoord, ycoord);
}

/**
 * mahua_gen5_remap_coord - Remap one Gen5 sweep index pair to signed,
 * IA-offset-corrected eye coordinates
 * @xcoord_len: Raw horizontal sweep index, in [0, EOM_EYE_WIDTH_LEN)
 * @ycoord_len: Raw vertical sweep index, in [0, EOM_EYE_HEIGHT_LEN)
 * @ia_offset_cal_code: Signed IA offset calibration code (x-axis origin)
 * @xcoord: Output signed, wrap-corrected horizontal coordinate
 * @ycoord: Output signed, mirrored vertical coordinate
 */
static void mahua_gen5_remap_coord(u32 xcoord_len, u32 ycoord_len, s32 ia_offset_cal_code,
				   s32 *xcoord, s32 *ycoord)
{
	s32 x = (s32)xcoord_len + ia_offset_cal_code - (EOM_EYE_WIDTH_LEN - 1);
	s32 y = (s32)ycoord_len;

	if (x > MAHUA_EYE_HALF_WIDTH)
		x -= MAHUA_MAX_EYE_WIDTH;
	if (x < -MAHUA_EYE_HALF_WIDTH)
		x += MAHUA_MAX_EYE_WIDTH;
	if (y > (EOM_EYE_HEIGHT_LEN - 1) / 2)
		y = (EOM_EYE_HEIGHT_LEN - 1) / 2 - (s32)ycoord_len;

	*xcoord = x;
	*ycoord = y;
}

/**
 * mahua_gen5_coord_in_bounds - Check whether a remapped Gen5 sample
 * coordinate lies within the swept eye boundary
 * @xcoord: Signed, remapped horizontal coordinate
 * @ycoord: Signed, remapped vertical coordinate
 *
 * See the POS_X_LIMIT/NEG_X_LIMIT/POS_Y_LIMIT/NEG_Y_LIMIT kdoc in
 * pcie_eom_mahua_phy_reg.h for why the X bounds are asymmetric.
 *
 * Return: true if the coordinate should be sampled, false to skip it.
 */
static inline bool mahua_gen5_coord_in_bounds(s32 xcoord, s32 ycoord)
{
	return ycoord > NEG_Y_LIMIT && ycoord < POS_Y_LIMIT &&
	       xcoord < POS_X_LIMIT && xcoord > NEG_X_LIMIT;
}

/**
 * mahua_read_ia_offset_cal_code - Read and sign-extend the Gen5 IA offset
 * calibration code for a lane
 * @phy: EOM PHY device
 * @lanenum: Lane number
 * @ia_offset_cal_code: Output signed, sign-extended calibration code
 *
 * Bits [5:0] of IA_OFFSET_CAL_STATUS hold a signed 6-bit two's-complement
 * code that the PHY's internal calibration engine settled on at link-up.
 * Positive values (bit 5 clear, raw 0-31) represent a rightward offset;
 * negative values (bit 5 set, raw 32-63) represent a leftward offset
 * (-32 to -1 in two's complement).
 *
 * The value is sign-extended from 6 bits to s32 by shifting the 6-bit
 * field into the MSB of a 32-bit integer (left-shift by 26) and then
 * performing an arithmetic right-shift back (right-shift by 26), which
 * replicates bit 5 into bits [31:6]. This is the standard portable
 * sign-extension idiom for C on two's-complement platforms (all Linux
 * targets). Storing as u32 and casting later would preserve raw values
 * 32-63 as large positive integers (+32 to +63), shifting the x-axis
 * sweep in the wrong direction for negative PHY calibration codes.
 *
 * This calibration code is the x-axis origin for the entire sweep in
 * mahua_eom_eye_seq(), so unlike every other register read in this file
 * that simply defaults and continues, a failed read here is fatal to the
 * caller: continuing with an assumed code of 0 would silently publish an
 * eye diagram built on an unknown horizontal origin as though it were a
 * successful, calibrated measurement.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mahua_read_ia_offset_cal_code(struct eom_phy_device *phy, u32 lanenum,
					 s32 *ia_offset_cal_code)
{
	u32 rc_idx = phy->index;
	u32 ia_raw_val;
	u32 reg_addr;
	int ret;

	reg_addr = mahua_phy_get_reg_offset(phy, PCIE_PHY_QSERDES_TXRX0_IA_OFFSET_CAL_STATUS,
					    lanenum);
	ret = mahua_read_phy_reg(phy, reg_addr, lanenum, &ia_raw_val);
	if (ret < 0) {
		pr_err("RC%d: Failed to read IA_OFFSET_CAL_STATUS for lane %u: %d\n",
		       rc_idx, lanenum, ret);
		return ret;
	}
	*ia_offset_cal_code = (s32)((ia_raw_val & MAHUA_IA_OFFSET_CAL_MASK) << 26) >> 26;

	return 0;
}

/**
 * mahua_eom_eye_seq - Sweep the eye diagram for a single lane
 * @lane: EOM lane structure
 * @lanenum: Lane number
 *
 * Caller must hold the per-RC instance lock for @lane's RC index across the
 * full call.
 *
 * Return: 0 on success; -EINTR if @lane->eom_seq_stop becomes set before
 *         the sweep completes; negative error code from PHY register I/O
 *         on failure.
 */
static int mahua_eom_eye_seq(struct eom_lane *lane, u32 lanenum)
{
	struct eom_phy_device *phy = lane->phy_dev;
	s32 remapped_xcoord, remapped_ycoord;
	u32 xcoord_len, ycoord_len;
	u32 dtime;
	s32 ia_offset_cal_code;
	int ret;

	dtime = mahua_eom_dwell_ms(lane->dwell_time_us);

	ret = mahua_read_ia_offset_cal_code(phy, lanenum, &ia_offset_cal_code);
	if (ret < 0)
		return ret;

	for (ycoord_len = 0; ycoord_len < EOM_EYE_HEIGHT_LEN; ycoord_len += Y_STEP_SIZE) {
		if (mahua_eom_seq_should_stop(lane)) {
			pr_debug("Lane:%d EOM eye seq: scan stop requested, aborting\n", lanenum);
			return -EINTR;
		}
		for (xcoord_len = 0; xcoord_len < EOM_EYE_WIDTH_LEN; xcoord_len += X_STEP_SIZE) {
			if (mahua_eom_seq_should_stop(lane)) {
				pr_debug("Lane:%d EOM eye seq: scan stop requested mid-sweep, aborting\n",
					 lanenum);
				return -EINTR;
			}
			mahua_gen5_remap_coord(xcoord_len, ycoord_len, ia_offset_cal_code,
					       &remapped_xcoord, &remapped_ycoord);
			if (!mahua_gen5_coord_in_bounds(remapped_xcoord, remapped_ycoord))
				continue;
			ret = mahua_process_eye_sample(lane, phy, lanenum, remapped_xcoord,
						       remapped_ycoord, dtime);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

/**
 * mahua_gen5_validate_lane - Validate lane/phy pointers and bounds for a
 * Gen5 EOM sequence call
 * @lane: EOM lane descriptor to validate
 * @phy: Output: @lane->phy_dev, once validated non-NULL
 * @rc_idx: Output: @phy->index, once validated in range
 * @lanenum: Output: @lane->lane_num, once validated in range and PHY-B
 *           availability
 *
 * Return: 0 on success, -EINVAL if any pointer or bound is invalid, or if
 *         @lanenum requires PHY-B and this device does not have one.
 */
static int mahua_gen5_validate_lane(struct eom_lane *lane, struct eom_phy_device **phy,
				    u32 *rc_idx, u32 *lanenum)
{
	if (!lane || !lane->phy_dev || !lane->seq) {
		pr_err("Invalid lane or phy_dev pointer or seq pointer\n");
		return -EINVAL;
	}
	*phy = lane->phy_dev;
	*lanenum = lane->lane_num;
	*rc_idx = (*phy)->index;
	if (*rc_idx >= MAHUA_PHY_MAX_RC_INSTANCES) {
		pr_err("Invalid RC index %d, max supported: %d\n",
		       *rc_idx, MAHUA_PHY_MAX_RC_INSTANCES - 1);
		return -EINVAL;
	}

	/*
	 * Validate lanenum before it reaches mahua_phy_get_reg_offset().
	 * That helper computes base_reg + adjusted_lanenum * MAHUA_QSERDES_OFFSET_SIZE
	 * (0x1000) in u32 arithmetic with no internal bounds check. adjusted_lanenum
	 * is reduced by MAHUA_PHY_B_LANE_START (4) for PHY-B lanes but is never
	 * clamped to a maximum, so an out-of-range lanenum silently overflows u32
	 * and produces an arbitrary register offset written to hardware.
	 * Valid range: 0 to MAHUA_PHY_MAX_NUM_LANES-1 (lanes 0-3 on PHY-A,
	 * lanes 4-7 on PHY-B).
	 */
	if (*lanenum >= MAHUA_PHY_MAX_NUM_LANES) {
		pr_err("RC%d: Invalid lane number %u, max supported: %u\n",
		       *rc_idx, *lanenum, MAHUA_PHY_MAX_NUM_LANES - 1);
		return -EINVAL;
	}

	/*
	 * MAHUA_PHY_MAX_NUM_LANES assumes a fully bifurcated PHY (PHY-A +
	 * PHY-B). On a device without PHY-B, mahua_read_phy_reg()/
	 * mahua_write_phy_reg() would still route lanenum >=
	 * MAHUA_PHY_B_LANE_START to phy_b_read()/phy_b_write() below, which
	 * targets an MMIO region this device may not actually have. Reject
	 * that case explicitly rather than relying on phy_has_phy_b() being
	 * re-checked on every register access.
	 */
	if (*lanenum >= MAHUA_PHY_B_LANE_START && !phy_has_phy_b(*phy)) {
		pr_err("RC%d: Lane %u requires PHY-B, which this device does not have\n",
		       *rc_idx, *lanenum);
		return -EINVAL;
	}

	return 0;
}

/**
 * mahua_gen5_pcie_eom_sequence - Run the full Gen5 EOM eye scan for one lane
 * @lane: EOM lane descriptor
 *
 * The per-RC instance mutex is held for the *entire* scan: pre-init
 * (mahua_update_aux_offset() + mahua_assert_deassert_aux_clk_reset_clear_ia_err()),
 * the MAHUA_EOM_PRE_INIT_SETTLE_MS settle delay, mahua_gen5_init(), and the
 * full eye sweep (mahua_eom_eye_seq()) are all one critical section. All of
 * these touch TXRXZ broadcast registers (RESET_GEN, CDR_EN_RXEQ_RESET,
 * AUX_CLK_CTRL, EOM_CTRL1, AUX_OFFSET_CONTROL) shared by every lane on the
 * RC instance: splitting the lock into separate per-step sections would let
 * a concurrent lane's kthread re-disturb this RC's AUX clock domain mid-settle
 * (defeating the very purpose of the settle delay) or re-run its own
 * broadcast init while this lane is mid-sweep, corrupting both lanes'
 * samples. Holding a mutex across msleep() is legal here because the caller
 * is a kthread running in process context. This serialises all lane scans
 * on a given RC instance to run one at a time — correctness of the
 * broadcast-register-dependent measurement takes priority over per-lane
 * scan concurrency, which this driver's EOM diagnostic use case does not
 * require.
 *
 * Neither this scan nor its Gen4 counterpart restores the PHY to its
 * pre-EOM operational configuration on completion, error, or early abort
 * (see phy_pcie_eom_sequence()'s kdoc for the full contract).
 *
 * Worst-case lock-hold duration: up to EOM_EYE_HEIGHT_LEN/Y_STEP_SIZE (51)
 * times EOM_EYE_WIDTH_LEN/X_STEP_SIZE (65) in-bounds samples, times the
 * dwell ceiling that dwell_time_us is actually clamped to before it ever
 * reaches mahua_eom_dwell_ms() — EOM_MAX_DWELL_TIME_US (10 ms) at the ioctl
 * boundary in eom_driver.c — plus the MAHUA_EOM_PRE_INIT_SETTLE_MS settle
 * delay and the Gen5 init/pre-init register I/O: tens of seconds in the
 * worst case (roughly 33 s at the 10 ms ioctl-boundary ceiling). Narrowing
 * the lock's scope below "whole scan" was considered and rejected: doing so
 * would let a concurrent lane's kthread re-disturb this RC's shared
 * broadcast registers mid-measurement (see the paragraph above), which is
 * a correctness regression that cannot be validated without real hardware.
 *
 * Return: 0 on success; -EINTR if the scan is aborted early via
 *         @lane->eom_seq_stop; -EINVAL if @lane, @lane->phy_dev, or
 *         @lane->seq is NULL, if the RC index is out of range, or if
 *         @lane->lane_num exceeds MAHUA_PHY_MAX_NUM_LANES; negative error
 *         code from PHY register I/O on hardware failure.
 */
static int mahua_gen5_pcie_eom_sequence(struct eom_lane *lane)
{
	struct eom_phy_device *phy;
	u32 lanenum, rc_idx;
	struct mutex *lock; /* per-RC instance lock, shared across lanes of the same RC */
	int ret;

	ret = mahua_gen5_validate_lane(lane, &phy, &rc_idx, &lanenum);
	if (ret < 0)
		return ret;

	pr_debug("Running Mahua PCIe EOM for %s instance %u lane %d\n",
		lane->seq->name, rc_idx, lanenum);
	lock = mahua_get_instance_lock(rc_idx);
	if (!lock) {
		pr_err("RC%d: Failed to get instance lock\n", rc_idx);
		return -EINVAL;
	}

	ret = mahua_lock_instance_or_stop(lock, lane);
	if (ret < 0) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested while waiting for instance lock\n",
			 rc_idx, lanenum);
		return ret;
	}
	lockdep_assert_held(lock);

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested before pre-init, aborting\n",
			 rc_idx, lanenum);
		mutex_unlock(lock);
		return -EINTR;
	}

	ret = mahua_update_aux_offset(phy, lanenum);
	if (ret >= 0) {
		ret = mahua_assert_deassert_aux_clk_reset_clear_ia_err(phy, lanenum);
		if (ret < 0)
			mahua_restore_eom_ctrl1_update_bit(phy, lanenum);
	}
	if (ret < 0) {
		mutex_unlock(lock);
		pr_err("RC%d: EOM pre-init failed: %d\n", rc_idx, ret);
		return ret;
	}
	msleep(MAHUA_EOM_PRE_INIT_SETTLE_MS);

	if (mahua_eom_seq_should_stop(lane)) {
		pr_debug("RC%d Lane:%d EOM: scan stop requested before Gen5 init, aborting\n",
			 rc_idx, lanenum);
		mutex_unlock(lock);
		return -EINTR;
	}

	ret = mahua_gen5_init(phy, lanenum);
	if (ret < 0) {
		mutex_unlock(lock);
		pr_err("RC%d: EOM Gen5 init failed: %d\n", rc_idx, ret);
		return ret;
	}

	ret = mahua_eom_eye_seq(lane, lanenum);
	mutex_unlock(lock);
	if (ret < 0) {
		if (ret != -EINTR)
			pr_err("RC%d: EOM eye sequence failed: %d\n", rc_idx, ret);
		return ret;
	}
	pr_debug("RC%d EOM eye sequence completed for lane %d\n", rc_idx, lanenum);

	return 0;
}

/**
 * phy_pcie_eom_sequence - Mahua PCIe PHY EOM sequence implementation
 * @lane: EOM lane descriptor. @lane->phy_dev->index selects the Gen4
 *        (MAHUA_GEN4_RC_IDX) or Gen5 (MAHUA_GEN5_RC_IDX) implementation.
 *
 * Strong symbol that overrides the weak fallback in eom_driver.c when
 * CONFIG_PCI_MSM_EOM is enabled. Dispatches to mahua_gen4_pcie_eom_sequence()
 * or mahua_gen5_pcie_eom_sequence() based on the RC index.
 *
 * Neither generation's sequence restores the PHY to its pre-EOM operational
 * configuration on completion, error, or early abort — the PHY is
 * intentionally left in EOM/margining mode. This matches the existing,
 * pre-existing behaviour of every sibling per-PHY EOM driver in this
 * directory (pcie_eom_hamoa_phy.c, pcie_eom_canoe_phy.c,
 * pcie_eom_seraph_phy.c): the caller is responsible for re-initialising the
 * PHY link before resuming normal (non-EOM) operation.
 *
 * This function sleeps (msleep() is called from the per-sample loop of both
 * generations) and must therefore be called from process context and never
 * with any spinlock held.
 *
 * Return: 0 on success; -EINTR on early abort via @lane->eom_seq_stop;
 *         -EINVAL if @lane or @lane->phy_dev is NULL, or if the RC index is
 *         neither MAHUA_GEN4_RC_IDX nor MAHUA_GEN5_RC_IDX; negative error
 *         code from PHY register I/O on hardware failure.
 */
int phy_pcie_eom_sequence(struct eom_lane *lane)
{
	u32 rc_idx;

	if (!lane || !lane->phy_dev) {
		pr_err("%s: NULL lane or phy_dev pointer\n", __func__);
		return -EINVAL;
	}

	rc_idx = lane->phy_dev->index;
	switch (rc_idx) {
	case MAHUA_GEN4_RC_IDX:
		return mahua_gen4_pcie_eom_sequence(lane);
	case MAHUA_GEN5_RC_IDX:
		return mahua_gen5_pcie_eom_sequence(lane);
	default:
		pr_err("Mahua EOM: unsupported RC index %u (expected RC4 or RC5)\n", rc_idx);

		return -EINVAL;
	}
}

#endif /* CONFIG_PCI_MSM_EOM */
