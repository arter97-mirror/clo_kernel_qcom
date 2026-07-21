/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IEEE 802.11 WLAN skb_ext definitions
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef LINUX_SKB_WIRELESS_H
#define LINUX_SKB_WIRELESS_H

#include <linux/types.h>
#include <linux/bitmap.h>

#define IEEE80211_SMD_CTX_NUM_TIDS 8
#define IEEE80211_SMD_CTX_MAX_PN_LEN 16
#define IEEE80211_SMD_CTX_PN_OCTETS 16
#define IEEE80211_SMD_CTX_NUM_SCSID 16

#define IEEE80211_SMD_CTX_NUM_VALID_CTX    8

#define IEEE80211_SMD_CTX_VALID_DL_SN      0
#define IEEE80211_SMD_CTX_VALID_UL_SN      1
#define IEEE80211_SMD_CTX_VALID_PN         2
#define IEEE80211_SMD_CTX_VALID_BA_PARAMS  3
#define IEEE80211_SMD_CTX_VALID_QOS        4
/* Positions 5 to 7 are reserved */

/**
 * struct ieee80211_smd_ctx_ba - BlockAck parameters for DL and UL
 *
 * @amsdu_supported: Peer's capability to support A-MSDU within A-MPDU.
 * @ba_policy: BlockAck policy (0 = delayed BlockAck, 1 = immediate BlockAck)
 * @buffer_size: Reorder buffer size from ADDBA Request (10-bit, max 1023)
 * @timeout: BlockAck session timeout
 * @ext_no_frag: ADDBA Extension fragmentation support
 * @extfrag_level: ADDBA Extension HE fragmentation level
 * @ext_buffer_size: ADDBA Extension buffer size; combined with @buffer_size as
 *	(@ext_buffer_size << 10 | @buffer_size) to get the full reorder buffer size
 */
struct ieee80211_smd_ctx_ba {
	u16 amsdu_supported:1,
	    ba_policy:1,
	    buffer_size:10;
	u16 timeout;
	u16 ext_no_frag:1,
	    extfrag_level:2,
	    ext_buffer_size:10;
};

/**
 * struct ieee80211_smd_ctx - IEEE 802.11bn SMD Roaming Context
 *
 * @valid_ctx_bmap: Bitmap indicating which context fields are valid; bit positions
 *	defined by IEEE80211_SMD_CTX_VALID_* constants
 * @pn_len: Length of PN in bytes; varies by cipher type (e.g. CCMP (6), GCMP-256 (16))
 * @dl: Down-link context such as valid TIDs, Sequence Numbers, Packet Number,
 *	and BlockAck parameters
 * @ul: Up-link context such as valid TIDs, Sequence Numbers, Packet Numbers,
 *	and BlockAck parameters
 * @qos: QoS context holding SCS descriptor elements (indexed by SCS ID) and
 *	the MSCS descriptor element
 * @drv_ctx: Driver-specific context opaque to the core; interpreted by the driver
 * @drv_ctx_size: Size in bytes of @drv_ctx buffer
 */
struct ieee80211_smd_ctx {
	DECLARE_BITMAP(valid_ctx_bmap, IEEE80211_SMD_CTX_NUM_VALID_CTX);
	u8 pn_len;

	struct {
		DECLARE_BITMAP(valid_tid_bmap, IEEE80211_SMD_CTX_NUM_TIDS);
		u16 sn[IEEE80211_SMD_CTX_NUM_TIDS];
		u8 pn[IEEE80211_SMD_CTX_MAX_PN_LEN];
		struct ieee80211_smd_ctx_ba ba[IEEE80211_SMD_CTX_NUM_TIDS];
	} dl;
	struct {
		DECLARE_BITMAP(valid_tid_bmap, IEEE80211_SMD_CTX_NUM_TIDS);
		u16 sn[IEEE80211_SMD_CTX_NUM_TIDS];
		u8 pn[IEEE80211_SMD_CTX_NUM_TIDS][IEEE80211_SMD_CTX_MAX_PN_LEN];
		struct ieee80211_smd_ctx_ba ba[IEEE80211_SMD_CTX_NUM_TIDS];
	} ul;
	struct {
		u8 *scs_descriptors[IEEE80211_SMD_CTX_NUM_SCSID];
		u8 *mscs_descriptor;
	} qos;

	void *drv_ctx;
	size_t drv_ctx_size;
};

/**
 * struct wireless_skb_ext - SKB extension data for wireless module use
 *
 * Carried as an skb extension (SKB_EXT_WIRELESS). The union is reserved
 * for future wireless extension types; only one member is active per skb.
 *
 * @uhr_smd_ctx: IEEE 802.11bn UHR SMD roaming context
 */
struct wireless_skb_ext {
	union {
		struct ieee80211_smd_ctx uhr_smd_ctx;
	};
};

#endif /* LINUX_SKB_WIRELESS_H */

