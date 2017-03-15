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
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2016, Tegile Systems Inc. All rights reserved.
 */

#include <inet/tcp.h>
#include <inet/tcp_impl.h>

extern void newreno_ack_received(struct cc_st *cc);
extern void newreno_cong_detected(struct cc_st *cc);
extern void newreno_cong_recovered(struct cc_st *cc);
extern void newreno_post_idle(struct cc_st *cc);

extern void cubic_init(struct cc_st *cc);
extern void cubic_fini(struct cc_st *cc);
extern void cubic_conn_init(struct cc_st *cc);
extern void cubic_ack_received(struct cc_st *cc);
extern void cubic_cong_detected(struct cc_st *cc);
extern void cubic_cong_recovered(struct cc_st *cc);
extern void cubic_post_idle(struct cc_st *cc);

/*
 * tcp_cc_tbl store the list of supported Congestion Control Alogrithms.
 * Note:
 * 	TCP_CC_ALGO_NUM should reflect any addition or deletion to this list
 *	TCP_CC_ALGO_LIST should reflect the names in exact order of this list
 */
static const cc_cb_t tcp_cc_tbl[TCP_CC_ALGO_NUM] = {
	{
		.name 		= TCP_CC_ALGO_NEWRENO,
		.ack_received	= newreno_ack_received,
		.cong_detected	= newreno_cong_detected,
		.cong_recovered	= newreno_cong_recovered,
		.post_idle	= newreno_post_idle
	},
	{
		.name		= TCP_CC_ALGO_CUBIC,
		.init		= cubic_init,
		.fini		= cubic_fini,
		.conn_init	= cubic_conn_init,
		.ack_received	= cubic_ack_received,
		.cong_detected	= cubic_cong_detected,
		.cong_recovered	= cubic_cong_recovered,
		.post_idle	= cubic_post_idle
	}
};

/*
 * Generic functions
 */

/*
 * CC table initialization during netstack initialization
 */
void
tcp_cc_stack_init(tcp_stack_t *tcps)
{
	/*
	 * Each tcp stack instance keep its own tcp_cc_tbl.
	 */
	bcopy(tcp_cc_tbl, tcps->tcps_cc_list,
		TCP_CC_ALGO_NUM * sizeof (cc_cb_t));
}

/*
 * Translates transmit requirements from CC to TCP flags
 */
uint32_t
tcp_cc_get_xmit_flags(struct cc_st *cc)
{
	uint32_t flags = 0;

	if (cc->cc_flags & TCP_CC_OUT_XMIT)
		flags |= TH_XMIT_NEEDED;
	if (cc->cc_flags & TCP_CC_OUT_REXMIT)
		flags |= TH_REXMIT_NEEDED;
	if (cc->cc_flags & TCP_CC_OUT_LIMIT_XMIT)
		flags |= TH_LIMIT_XMIT;
	if (cc->cc_flags & TCP_CC_OUT_SACK_REXMIT)
		flags |= TH_NEED_SACK_REXMIT;

	return (flags);
}

/*
 * Common funtion to set tcp_cwr if ECN enabled.
 */
void
tcp_cc_cwr_set(tcp_t *tcp)
{
	if (tcp->tcp_ecn_ok) {
		tcp->tcp_cwr = B_TRUE;
		tcp->tcp_ecn_cwr_sent = B_FALSE;
		/*
		 * This marks the end of the current window of in flight data.
		 * That is why we don't use tcp_suna + tcp_swnd. Only data
		 * in flight can provide ECN info.
		 */
		tcp->tcp_cwr_snd_max = tcp->tcp_snxt;
	}
}

/*
 * Setup Retransmit segments appropriately
 */
void
tcp_cc_set_rexmit(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;

	/*
	 * Save highest seq no we have sent so far. Be careful about
	 * the invisible FIN byte.
	 */
	if ((tcp->tcp_valid_bits & TCP_FSS_VALID) &&
	    (tcp->tcp_unsent == 0)) {
		tcp->tcp_rexmit_max = tcp->tcp_fss;
	} else {
		tcp->tcp_rexmit_max = tcp->tcp_snxt;
	}

	/*
	 * For SACK:
	 * Calculate tcp_pipe, which is the estimated number of bytes in
	 * network. tcp_fack is the highest sack'ed seq num TCP has received.
	 *
	 * tcp_pipe is explained in Fall and Floyd's paper "Simulation-based
	 * Comparisons of Tahoe, Reno and SACK TCP". tcp_fack is explained in
	 * Mathis and Mahdavi's "Forward Acknowledgment: Refining TCP
	 * Congestion Control" in SIGCOMM '96.
	 */
	if (tcp->tcp_snd_sack_ok) {
		if (tcp->tcp_notsack_list != NULL) {
			tcp->tcp_pipe = tcp->tcp_snxt - tcp->tcp_fack;
			tcp->tcp_sack_snxt = cc->cc_seg_ack;
			cc->cc_flags |= TCP_CC_OUT_SACK_REXMIT;
		} else {
			/*
			 * Always initialize tcp_pipe even though we
			 * don't have any SACK info.  If later we get
			 * SACK info and tcp_pipe is not initialized,
			 * funny things will happen.
			 */
			tcp->tcp_pipe = tcp->tcp_cwnd_ssthresh;
		}
	} else {
		cc->cc_flags |= TCP_CC_OUT_REXMIT;
	}
}
