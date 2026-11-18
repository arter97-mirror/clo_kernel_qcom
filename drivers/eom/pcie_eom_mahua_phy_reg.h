/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
/* Unified register definitions for Mahua PCIe Gen4 and Gen5 EOM. */

#ifndef PCIE_EOM_MAHUA_PHY_REG_H
#define PCIE_EOM_MAHUA_PHY_REG_H

#include <linux/bitops.h>

/* Common sequence values. */
/* Note: NEGATIVE_SEQUENCE and POSITIVE_SEQUENCE are intentionally omitted here.
 * They are defined in eom_driver.h (which every consumer of this header already
 * includes) and re-defining them here would produce -Wmacro-redefined warnings.
 */
#define EOM_REG_WRITE_DELAY            100

/*
 * MAHUA_EOM_MAX_DWELL_MS - upper bound on a single EOM sample's dwell time,
 * shared by both Gen4 and Gen5
 *
 * lane->dwell_time_us is user-controlled (EOM_IOCTL_SELECT_DEVICE) and is
 * clamped once at the ioctl boundary in eom_driver.c (EOM_MAX_DWELL_TIME_US),
 * but each generation converts it to milliseconds independently, so it is
 * re-clamped here as well: a few hundred ms is already far beyond any real
 * measurement need, and this keeps the sample loop's own bound self-evident
 * without relying solely on the shared ioctl-boundary clamp.
 */
#define MAHUA_EOM_MAX_DWELL_MS         500

/*
 * PCIE4_MAHUA_SCAN_DIRECTION_SETTLE_NS - settle delay between the Gen4
 * upper (positive) and lower (negative) eye scans
 *
 * Taken from the CMM script's inter-direction delay. Undocumented beyond
 * that in the original script; kept as a short settle window between
 * re-initialising EOM_CTRL1 for the opposite scan direction.
 */
#define PCIE4_MAHUA_SCAN_DIRECTION_SETTLE_NS 1000

/* --------------------------- Mahua Gen4 --------------------------- */
#define PCIE4_MAHUA_PHY_REG_ADDR(base, lane) ((base) + ((lane) * PCIE4_MAHUA_QSERDES_OFFSET_SIZE))
#define PCIE4_MAHUA_MAX_VERTICAL_THERMAL_CODE      11
#define PCIE4_MAHUA_MAX_EYE_HEIGHT          PCIE4_MAHUA_MAX_VERTICAL_THERMAL_CODE
#define PCIE_PHY_QSERDES_RX0_IA_ERROR_COUNTER_HIGH 0x5C8
#define PCIE_PHY_QSERDES_RX0_RX_MARG_VERTICAL_CODE 0x524
#define PCIE_PHY_QSERDES_RX0_IA_ERROR_COUNTER_LOW  0x5C4
#define PCIE_PHY_QSERDES_RX0_CDR_RESET_OVERRIDE    0x3EC
#define PCIE_PHY_QSERDES_RX0_RCLK_AUXDATA_SEL      0x310
#define PCIE_PHY_QSERDES_TX0_RESET_GEN_MUXES       0x0B0
#define PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL2         0x4FC
#define PCIE_PHY_QSERDES_RX0_RX_MARG_CTRL3         0x500
#define PCIE_PHY_QSERDES_RX0_AUX_CONTROL           0x2FC
#define PCIE_PHY_QSERDES_RX0_AUXDATA_TB            0x300
#define PCIE_PHY_QSERDES_RX0_EOM_CTRL1             0x314
#define PCIE_PHY_QSERDES_RX0_EOM_CTRL2             0x318
#define PCIE4_MAHUA_QSERDES_OFFSET_SIZE            0x800
#define PCIE4_MAHUA_EOM_CTRL1_UPPER                0x028
#define PCIE4_MAHUA_EOM_CTRL1_LOWER                0x038
#define PCIE4_MAHUA_EOM_CTRL2_VAL                  0x008
#define PCIE4_MAHUA_AUXDATA_TB_MUX_BIT             0x08  /* bit 3: TB mux select enable */
#define PCIE4_MAHUA_AUX_CONTROL_MUX_BIT            0x40  /* bit 6: AUX path mux enable */
/* idle/de-asserted state of margin ctrl strobe */
#define PCIE4_MAHUA_RX_MARG_CTRL3_DEASSERT         0xEA
/* asserted state of margin ctrl strobe, ahead of the strobe pulse below */
#define PCIE4_MAHUA_RX_MARG_CTRL3_ASSERT           0xEB
/* momentary strobe pulse value, bracketed by ASSERT writes before/after */
#define PCIE4_MAHUA_RX_MARG_CTRL3_STROBE           0xEF
/* TX_RESET_GEN_MUXES value written by the Gen4 EOM pre-sequence */
#define PCIE4_MAHUA_TX_RESET_GEN_MUXES_VAL          0x03
/* CDR_RESET_OVERRIDE value written by the Gen4 EOM pre-sequence */
#define PCIE4_MAHUA_CDR_RESET_OVERRIDE_VAL          0x0A
/* RCLK_AUXDATA_SEL enabled/disabled values toggled by the Gen4 EOM sequences */
#define PCIE4_MAHUA_RCLK_AUXDATA_SEL_ENABLE         0xFC
#define PCIE4_MAHUA_RCLK_AUXDATA_SEL_DISABLE        0xF4
/* RX_MARG_CTRL2 value written once during Gen4 EOM post-init */
#define PCIE4_MAHUA_RX_MARG_CTRL2_VAL                0x80
#define PCIE4_MAHUA_MAX_NUM_LANES                   2
#define PCIE4_MAHUA_MAX_EYE_WIDTH                   64
/*
 * PCIE4_MAHUA_EYE_HALF_WIDTH - half the Gen4 EOM horizontal sweep range
 *
 * The Gen4 x-coordinate sweep covers [0..PCIE4_MAHUA_MAX_EYE_WIDTH-1].
 * Samples with a zero-based index above (PCIE4_MAHUA_EYE_HALF_WIDTH - 1)
 * represent negative offsets from the eye centre and are remapped by
 * subtracting PCIE4_MAHUA_MAX_EYE_WIDTH, yielding a signed range of
 * [-(PCIE4_MAHUA_EYE_HALF_WIDTH)..(PCIE4_MAHUA_EYE_HALF_WIDTH - 1)].
 */
#define PCIE4_MAHUA_EYE_HALF_WIDTH                  (PCIE4_MAHUA_MAX_EYE_WIDTH / 2)
#define PCIE4_MAHUA_MAX_VTH_CONTROL                 8
/*
 * PCIE4_MAHUA_VTH_Y_STRIDE - vertical y-index stride between VTH control levels
 *
 * The Gen4 EOM scan uses a two-level vertical sweep: an outer vth_control
 * loop [0..7] and an inner ycoord loop [0..PCIE4_MAHUA_MAX_EYE_HEIGHT-1].
 * The effective y-index stored in each EOM entry is:
 *   y_effective = ycoord + (PCIE4_MAHUA_VTH_Y_STRIDE * vth_control)
 * This stride of 16 (0x10) is the fixed spacing between VTH levels in the
 * CMM script and must match the hardware's VTH code allocation exactly.
 */
#define PCIE4_MAHUA_VTH_Y_STRIDE                    0x10
/*
 * MAHUA_EOM_ERROR_COUNTER_SATURATED - sentinel value indicating IA error
 * counter saturation
 *
 * The IA error counter is 16 bits wide (LOW byte | HIGH byte). When the
 * counter reaches its maximum value of 0xFFFF it stops incrementing and
 * holds at this value for the remainder of the dwell period. A reading of
 * 0xFFFF therefore means "at least 65535 errors" rather than exactly that
 * count. The driver logs a warning when this sentinel is observed so that
 * the caller can treat the sample as a lower bound rather than an exact
 * measurement. The U suffix ensures the comparison against the u32
 * errorcntr variable is unsigned throughout.
 */
#define MAHUA_EOM_ERROR_COUNTER_SATURATED           0xFFFFU

/* ---------------------------- Mahua Gen5 --------------------------- */
#define MAHUA_PHY_REG_ADDR(base, lane) ((base) + ((lane) * MAHUA_QSERDES_OFFSET_SIZE))
#define PCIE_PHY_QSERDES_TXRX0_IA_ERROR_COUNTER_HIGH 0x0708
#define PCIE_PHY_QSERDES_TXRX0_AUX_OFFSET_CONTROL   0x041C
#define PCIE_PHY_QSERDES_TXRX0_IA_OFFSET_CAL_STATUS 0x079C
/*
 * MAHUA_IA_OFFSET_CAL_MASK - field mask for the signed 6-bit IA offset
 * calibration code in IA_OFFSET_CAL_STATUS
 *
 * Bits [5:0] of PCIE_PHY_QSERDES_TXRX0_IA_OFFSET_CAL_STATUS hold the
 * two's-complement calibration code that the PHY's internal IA offset
 * calibration engine settled on at link-up. This mask isolates those six
 * bits before sign-extension. It is numerically equal to
 * MAHUA_AUX_OFFSET_DATA_MASK (also 0x3F) but is kept as a separate
 * constant because the two fields belong to different registers with
 * independent semantics; a future change to either field width must not
 * silently affect the other.
 */
#define MAHUA_IA_OFFSET_CAL_MASK            0x3F
#define PCIE_PHY_QSERDES_TXRX0_IA_ERROR_COUNTER_LOW 0x0704
#define PCIE_PHY_QSERDES_TXRXZ_CDR_IQTUNE_ANA_CTRL  0xD584
#define PCIE_PHY_QSERDES_TXRX0_CDR_IQTUNE_ANA_CTRL  0x0584
#define PCIE_PHY_QSERDES_TXRXZ_AUX_OFFSET_CONTROL   0xD41C
#define PCIE_PHY_QSERDES_TXRXZ_CDR_EN_RXEQ_RESET    0xD048
#define PCIE_PHY_QSERDES_TXRXZ_CDR_VCO_CAL_CTRL     0xD1B0
#define PCIE_PHY_QSERDES_TXRXZ_AUX_CLK_CTRL         0xD418
#define PCIE_PHY_QSERDES_TXRXZ_AUXDATA_TB           0xD420
#define PCIE_PHY_QSERDES_TXRX0_AUXDATA_TB           0x0420
#define PCIE_PHY_QSERDES_TXRXZ_RESET_GEN            0xD044
#define PCIE_PHY_QSERDES_TXRXZ_EOM_CTRL1            0xD424
#define PCIE_PHY_QSERDES_TXRX0_EOM_CTRL2            0x0428
#define PCIE_PHY_QSERDES_TXRX0_EOM_CTRL3            0x042C
#define MAHUA_QSERDES_OFFSET_SIZE                   0x1000
#define MAHUA_RESET_GEN_MASK               0x28
#define MAHUA_RESET_GEN_VAL                0x28
#define MAHUA_CDR_EN_RXEQ_RESET_MASK       0x0B
#define MAHUA_CDR_EN_RXEQ_RESET_VAL        0x0B
#define MAHUA_CDR_VCO_CAL_CTRL_MASK        0x30
#define MAHUA_CDR_VCO_CAL_CTRL_VAL         0x30
#define MAHUA_CDR_IQTUNE_IQ_CLK_SEL_BIT    BIT(3)
#define MAHUA_CDR_IQTUNE_AUX_DIR_BIT       BIT(2)
#define MAHUA_AUXDATA_TB_MUX_MASK          0x80
#define MAHUA_AUXDATA_TB_MUX_VAL           0x80
#define MAHUA_AUX_CLK_CTRL_MASK            0x3A
#define MAHUA_AUX_CLK_CTRL_VAL             0x3A
#define MAHUA_AUX_CLK_RESET_BIT            BIT(2)
#define MAHUA_EOM_CTRL1_INIT_MASK          0xBB
#define MAHUA_EOM_CTRL1_INIT_VAL           0xBB
#define MAHUA_EOM_CTRL1_UPDATE_BIT         BIT(6)
#define MAHUA_EOM_CTRL1_CLEAR_IA_MASK      0xBF
#define MAHUA_EOM_CTRL1_CLEAR_IA_VAL       0xBF
#define MAHUA_AUX_OFFSET_CTRL_MUX_MASK     0x40
#define MAHUA_AUX_OFFSET_CTRL_MUX_VAL      0x40
#define MAHUA_EOM_CTRL2_POS_Y              0xD8
#define MAHUA_EOM_CTRL2_NEG_Y              0x98
#define MAHUA_EOM_CTRL3_VAL                0xA8
/*
 * MAHUA_EOM_PRE_INIT_SETTLE_MS - stabilisation delay after the Gen5 EOM
 * pre-initialisation sequence
 *
 * After UpdateAuxOffset and AssertDeassertAuxClkReset complete, the AUX
 * path requires this dwell period before the main GEN5Init register
 * sequence is safe to run. Without it the AUX clock domain may not have
 * fully settled, causing X-axis collapse in the resulting eye diagram.
 * The 10 ms value is taken directly from the CMM script.
 */
#define MAHUA_EOM_PRE_INIT_SETTLE_MS        10
#define MAHUA_AUXDATA_TB_DATA_MASK         0x7F
#define MAHUA_AUX_OFFSET_DATA_MASK         0x3F
#define POS_Y_LIMIT                    127
#define NEG_Y_LIMIT                    (-127)
#define NEG_X_LIMIT                    (-31)
#define EOM_EYE_WIDTH_LEN              65
#define EOM_EYE_HEIGHT_LEN             255
#define MAHUA_MAX_EYE_WIDTH            64
/*
 * MAHUA_EYE_HALF_WIDTH - half the Gen5 EOM horizontal sweep range
 *
 * After IA offset calibration is applied, remapped_xcoord values that
 * exceed +MAHUA_EYE_HALF_WIDTH or fall below -MAHUA_EYE_HALF_WIDTH have
 * wrapped past the eye boundary and are corrected by subtracting or
 * adding MAHUA_MAX_EYE_WIDTH respectively, keeping the coordinate within
 * the valid signed range.
 */
#define MAHUA_EYE_HALF_WIDTH           (MAHUA_MAX_EYE_WIDTH / 2)
#define MAHUA_PHY_B_LANE_START         4
#define MAHUA_PHY_MAX_RC_INSTANCES     8
#define MAHUA_PHY_MAX_NUM_LANES        8  /* 4 lanes PHY-A (0-3) + 4 lanes PHY-B (4-7) */
#define X_STEP_SIZE                    1
#define Y_STEP_SIZE                    5
#define POS_X_LIMIT                    0
/*
 * POS_X_LIMIT / NEG_X_LIMIT / POS_Y_LIMIT / NEG_Y_LIMIT - Gen5 eye-boundary
 * gate used in mahua_eom_eye_seq()
 *
 * The raw sweep covers xcoord_len in [0, EOM_EYE_WIDTH_LEN) and ycoord_len
 * in [0, EOM_EYE_HEIGHT_LEN). After IA-offset remapping and wrap correction,
 * only points with remapped_xcoord in (NEG_X_LIMIT, POS_X_LIMIT) and
 * remapped_ycoord in (NEG_Y_LIMIT, POS_Y_LIMIT) are kept. The Y bounds are
 * symmetric (+-127) because the vertical sweep is centred on the eye. The X
 * bounds are intentionally asymmetric (0 to -31, i.e. only the negative
 * half): the CMM script's Gen5 horizontal sweep only samples the left half
 * of the eye, relying on hardware symmetry for the right half, whereas the
 * Y (vertical) axis is not symmetric in hardware and must be swept in full.
 */

/*
 * MAHUA_GEN4_RC_IDX / MAHUA_GEN5_RC_IDX - RC instance indices for the
 * Mahua PCIe Gen4 and Gen5 controllers
 *
 * phy_pcie_eom_sequence() dispatches to the correct generation-specific
 * EOM implementation based on phy->index. These constants name the two
 * supported RC indices so that the switch cases are self-documenting.
 * RC4 is the Mahua PCIe Gen4 instance; RC5 is the Mahua PCIe Gen5
 * instance. Both must remain within [0, MAHUA_PHY_MAX_RC_INSTANCES).
 */
#define MAHUA_GEN4_RC_IDX               4
#define MAHUA_GEN5_RC_IDX               5

#endif /* PCIE_EOM_MAHUA_PHY_REG_H */
