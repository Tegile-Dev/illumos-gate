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
 * Copyright (c) 2016, Tegile Systems Inc. All rights reserved.
 */

#ifndef	_INET_TCP_CC_H
#define	_INET_TCP_CC_H

#ifdef	__cplusplus
extern "C" {
#endif

#define	TCP_CC_ALGO_NAME_LENGTH	32 /* Maximum length for an algorithm name */
#define	TCP_CC_ALGO_NEWRENO	"newreno"
#define	TCP_CC_ALGO_CUBIC	"cubic"
#define	TCP_CC_ALGO_DEFAULT	TCP_CC_ALGO_NEWRENO /* default algorithm */
#define	TCP_CC_ALGO_LIST	{ TCP_CC_ALGO_NEWRENO, TCP_CC_ALGO_CUBIC }
#define	TCP_CC_ALGO_NUM		2 /* Total number of supported algorithms */

#ifdef _KERNEL
#include <sys/inttypes.h>

typedef struct cc_st {
	void		*cc_priv;	/* per connection private data */
	struct cc_cb	*cc_ptr;	/* pointer to CC call back structure */
	union {
		void		*cp_proto;
		struct tcp_s	*cp_tcp;
	} cc_proto_priv;	/* pointer to netstack instance private data */

#define	cc_proto	cc_proto_priv.cp_proto
#define	cc_tcp		cc_proto_priv.cp_tcp

	uint32_t	cc_seg_ack;	/* Current seg_ack */
	uint32_t	cc_flags;	/* CC specific flags */
} cc_st_t;

/*
 * CC CB specific flags
 * lower 16bits are used for data processing
 * higer 16bits are used for keeping generic CC status/state.
 */
#define	TCP_CC_FLG_ECE		0x00000001
#define	TCP_CC_FLG_RTO		0x00000002
#define	TCP_CC_FLG_DUPACK	0x00000004

/* XMIT flags for CC */
#define	TCP_CC_OUT_XMIT		0x00000100
#define	TCP_CC_OUT_REXMIT	0x00000200
#define	TCP_CC_OUT_LIMIT_XMIT	0x00000400
#define	TCP_CC_OUT_SACK_REXMIT	0x00000800

#define	TCP_CC_FLAGS_RESET(tcp) 	((tcp)->tcp_cc.cc_flags = 0x0)
#define	TCP_CC_DATA_FLAGS_RESET(tcp)	((tcp)->tcp_cc.cc_flags &= 0xffff0000)
#define	TCP_CC_XMIT_FLAGS_CHECK(tcp)	((tcp)->tcp_cc.cc_flags & 0x00000f00)

/* CC specific status/state  flags */
/* XXX not yet used, place holder for now */
#define	TCP_CC_SLOW_START	0x00010000
#define	TCP_CC_FAST_RETRANSMIT	0x00020000
#define	TCP_CC_FAST_RECOVERY	0x00040000
#define	TCP_CC_CONG_AVOIDENCE	0x00080000

typedef struct cc_cb {
	char name[TCP_CC_ALGO_NAME_LENGTH];

	void (*init)(struct cc_st *);		/* CC initialization */
	void (*fini)(struct cc_st *);		/* CC cleanup */
	void (*conn_init)(struct cc_st *);	/* new connection */
	void (*ack_received)(struct cc_st *);	/* ack received */
	void (*cong_detected)(struct cc_st *);	/* congestion detected */
	void (*cong_recovered)(struct cc_st *);	/* full/partial cong recovery */
	void (*post_idle)(struct cc_st *);	/* resuming xfer after idle */

	uint32_t flags;
} cc_cb_t;

#define	tcp_cc_cb(cc, cb) do {					\
	ASSERT((cc)->cc_ptr);					\
	if ((cc)->cc_ptr->cb != NULL) (cc)->cc_ptr->cb(cc);	\
_NOTE(CONSTCOND)						\
} while (0)

#define	tcp_cc_init(cc)			tcp_cc_cb(cc, init)
#define	tcp_cc_fini(cc)			tcp_cc_cb(cc, fini)
#define	tcp_cc_conn_init(cc)		tcp_cc_cb(cc, conn_init)
#define	tcp_cc_ack_received(cc)		tcp_cc_cb(cc, ack_received)
#define	tcp_cc_cong_detected(cc)	tcp_cc_cb(cc, cong_detected)
#define	tcp_cc_cong_recovered(cc)	tcp_cc_cb(cc, cong_recovered)
#define	tcp_cc_post_idle(cc)		tcp_cc_cb(cc, post_idle)

#define	tcp_cc_default(tcps) (&(tcps)->tcps_cc_list[(tcps)->tcps_cong_default])

#endif /* _KERNEL */

#ifdef	__cplusplus
}
#endif

#endif	/* _INET_TCP_CC_H */
