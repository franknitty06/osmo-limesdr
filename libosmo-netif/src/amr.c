/*
 * (C) 2012 by Pablo Neira Ayuso <pablo@gnumonks.org>
 * (C) 2012 by On Waves ehf <http://www.on-waves.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <osmocom/netif/amr.h>

/* According to TS 26.101:
 *
 * Frame type    AMR code    bits  bytes
 *      0          4.75       95    12
 *      1          5.15      103    13
 *      2          5.90      118    15
 *      3          6.70      134    17
 *      4          7.40      148    19
 *      5          7.95      159    20
 *      6         10.20      204    26
 *      7         12.20      244    31
 */

static size_t amr_ft_to_bytes[AMR_FT_MAX] = {
	[AMR_FT_0]	= AMR_FT_0_LEN,
	[AMR_FT_1]	= AMR_FT_1_LEN,
	[AMR_FT_2]	= AMR_FT_2_LEN,
	[AMR_FT_3]	= AMR_FT_3_LEN,
	[AMR_FT_4]	= AMR_FT_4_LEN,
	[AMR_FT_5]	= AMR_FT_5_LEN,
	[AMR_FT_6]	= AMR_FT_6_LEN,
	[AMR_FT_7]	= AMR_FT_7_LEN,
	[AMR_FT_SID]	= AMR_FT_SID_LEN,
};

size_t osmo_amr_bytes(uint8_t amr_ft)
{
	return amr_ft_to_bytes[amr_ft];
}

int osmo_amr_ft_valid(uint8_t amr_ft)
{
	/*
	 * Extracted from RFC3267:
	 *
	 * "... with a FT value in the range 9-14 for AMR ... the whole packet
	 *  SHOULD be discarded."
	 *
	 * "... packets containing only NO_DATA frames (FT=15) SHOULD NOT be
	 *  transmitted."
	 *
	 * So, let's discard frames with a AMR FT >= 9.
	 */
	if (amr_ft >= AMR_FT_MAX)
		return 0;

	return 1;
}

/*! Check if an AMR frame is octet aligned by looking at the padding bits.
 *  \param[inout] payload user provided memory containing the AMR payload.
 *  \param[in] payload_len overall length of the AMR payload.
 *  \returns true when the payload is octet aligned. */
bool osmo_amr_is_oa(uint8_t *payload, unsigned int payload_len)
{
	/* NOTE: The distinction between octet-aligned and bandwith-efficient
	 * mode normally relys on out of band methods that explicitly select
	 * one of the two modes. (See also RFC 3267, chapter 3.8). However the
	 * A interface in GSM does not provide ways to communicate which mode
	 * is used exactly used. The following functions uses some heuristics
	 * to check if an AMR payload is octet aligned or not. */

	struct amr_hdr *oa_hdr = (struct amr_hdr *)payload;
	unsigned int frame_len;

	/* Broken payload? */
	if (!payload || payload_len < sizeof(struct amr_hdr))
		return false;

	/* In octet aligned mode, padding bits are specified to be
	 * set to zero. (However, there is a remaining risk that the FT0 or FT1
	 * is selected and the first two bits of the frame are zero as well,
	 * in this case a bandwith-efficient mode payload would look like an
	 * octet-aligned payload, thats why additional checks are required.) */
	if (oa_hdr->pad1 != 0)
		return false;
	if (oa_hdr->pad2 != 0)
		return false;

	/* This implementation is limited to single-frame payloads only and
	 * since multi-frame payloads are not common in GSM anyway, we may
	 * include the final bit of the first header into this check. */
	if (oa_hdr->f != 0)
		return false;

	/* Match the length of the received payload against the expected frame
	 * length that is defined by the frame type. */
	if(!osmo_amr_ft_valid(oa_hdr->ft))
		return false;
	frame_len = osmo_amr_bytes(oa_hdr->ft);
	if (frame_len != payload_len - sizeof(struct amr_hdr))
		return false;

	return true;
}

/*! Convert an AMR frame from octet-aligned mode to bandwith-efficient mode.
 *  \param[inout] payload user provided memory containing the AMR payload.
 *  \param[in] payload_len overall length of the AMR payload.
 *  \returns resulting payload length, -1 on error. */
int osmo_amr_oa_to_bwe(uint8_t *payload, unsigned int payload_len)
{
	struct amr_hdr *oa_hdr = (struct amr_hdr *)payload;
	unsigned int frame_len = payload_len - sizeof(struct amr_hdr);
	unsigned int i;

	/* This implementation is not capable to handle multi-frame
	 * packets, so we need to make sure that the frame we operate on
	 * contains only one payload. */
	if (oa_hdr->f != 0)
		return -1;

	/* Move TOC close to CMR */
	payload[0] |= (payload[1] >> 4) & 0x0f;
	payload[1] = (payload[1] << 4) & 0xf0;

	for (i = 0; i < frame_len; i++) {
		payload[i + 1] |= payload[i + 2] >> 2;
		payload[i + 2] = payload[i + 2] << 6;
	}

	/* The overall saving is one byte! */
	return payload_len - 1;
}

/*! Convert an AMR frame from bandwith-efficient mode to octet-aligned mode.
 *  \param[inout] payload user provided memory containing the AMR payload.
 *  \param[in] payload_len overall length of the AMR payload.
 *  \param[in] payload_maxlen maximum length of the user provided memory.
 *  \returns resulting payload length, -1 on error. */
int osmo_amr_bwe_to_oa(uint8_t *payload, unsigned int payload_len,
		       unsigned int payload_maxlen)
{
	uint8_t buf[256];
	unsigned int frame_len = payload_len - 1;
	unsigned int i;

	memset(buf, 0, sizeof(buf));

	if (payload_len + 1 > payload_maxlen)
		return -1;

	if (payload_len + 1 > sizeof(buf))
		return -1;

	buf[0] = payload[0] & 0xf0;
	buf[1] = payload[0] << 4;
	buf[1] |= (payload[1] >> 4) & 0x0c;

	for (i = 0; i < frame_len - 1; i++) {
		buf[i + 2] = payload[i + 1] << 2;
		buf[i + 2] |= payload[i + 2] >> 6;
	}
	buf[i + 2] = payload[i + 1] << 2;

	memcpy(payload, buf, payload_len + 1);
	return payload_len + 1;
}
