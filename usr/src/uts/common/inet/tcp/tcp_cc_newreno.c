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

extern void tcp_cc_cwr_set(tcp_t *tcp);
extern void tcp_cc_set_rexmit(struct cc_st *cc);

/*
 * +---------------------------+
 * | Newreno support functions |
 * +---------------------------+
 */
void
newreno_ack_received(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	tcp_stack_t	*tcps = tcp->tcp_tcps;
	uint32_t	mss = tcp->tcp_mss;
	uint32_t	cwnd, add;

	if (cc->cc_flags & TCP_CC_FLG_DUPACK) {
		/* Do Limited Transmit */
		if (tcp->tcp_dupack_cnt <
			tcps->tcps_dupack_fast_retransmit) {
			/*
			 * RFC 3042
			 *
			 * What we need to do is temporarily increase tcp_cwnd
			 * so that new data can be sent if it is allowed by the
			 * receive window (tcp_rwnd). tcp_wput_data() will take
			 * care of the rest.
			 *
			 * If the connection is SACK capable, only do limited
			 * xmit when there is SACK info.
			 *
			 * Note how tcp_cwnd is incremented. The first dup ACK
			 * will increase it by 1 MSS. The second dup ACK will
			 * increase it by 2 MSS.  This means that only 1 new
			 * segment will be sent for each dup ACK.
			 */
			if (tcp->tcp_unsent > 0 &&
			    (!tcp->tcp_snd_sack_ok ||
			    (tcp->tcp_snd_sack_ok &&
			    tcp->tcp_notsack_list != NULL))) {
				tcp->tcp_cwnd += mss <<
				    (tcp->tcp_dupack_cnt - 1);
				cc->cc_flags |= TCP_CC_OUT_LIMIT_XMIT;
			}

		/*
		 * cong_detected call back function will deal with
		 * tcp_dupack_cnt == tcps_dupack_fast_retransmit case
		 */
		} else if (tcp->tcp_dupack_cnt >
		    tcps->tcps_dupack_fast_retransmit) {
			/*
			 * Here we perform congestion avoidance, NOT slow start.
			 * This is known as the Fast Recovery Algorithm.
			 */
			if (tcp->tcp_snd_sack_ok &&
			    tcp->tcp_notsack_list != NULL) {
				cc->cc_flags |= TCP_CC_OUT_SACK_REXMIT;
				tcp->tcp_pipe -= mss;
				if (tcp->tcp_pipe < 0)
					tcp->tcp_pipe = 0;
			} else {
				/*
				 * We know that one more packet has left
				 * the pipe thus we can update cwnd.
				 */
				cwnd = tcp->tcp_cwnd + mss;
				if (cwnd > tcp->tcp_cwnd_max)
					cwnd = tcp->tcp_cwnd_max;
				tcp->tcp_cwnd = cwnd;
				if (tcp->tcp_unsent > 0)
					cc->cc_flags |= TCP_CC_OUT_XMIT;
			}
		}
	} else { /* .i.e the ACK is not a duplicate ack */

		/*
		 * The congestion window update for slow start and
		 * congestion avoidance
		 *
		 * If TCP is not ECN capable or TCP is ECN capable
		 * but the congestion experience bit is not set,
		 * increase the tcp_cwnd as usual.
		 */
		if (!tcp->tcp_ecn_ok || !(cc->cc_flags & TCP_CC_FLG_ECE)) {
			cwnd = tcp->tcp_cwnd;
			add = tcp->tcp_mss;

			if (cwnd >= tcp->tcp_cwnd_ssthresh) {
				/*
				 * This is to prevent an increase of less than
				 * 1 MSS of tcp_cwnd.  With partial increase,
				 * tcp_wput_data() may send out tinygrams in
				 * order to preserve mblk boundaries.
				 *
				 * By initializing tcp_cwnd_cnt to new tcp_cwnd
				 * and decrementing it by 1 MSS for every ACKs,
				 * tcp_cwnd is increased by 1 MSS for every RTTs
				 */
				if (tcp->tcp_cwnd_cnt <= 0) {
					tcp->tcp_cwnd_cnt = cwnd + add;
				} else {
					tcp->tcp_cwnd_cnt -= add;
					add = 0;
				}
			}
			tcp->tcp_cwnd = MIN(cwnd + add, tcp->tcp_cwnd_max);
		}
	}
}

void
newreno_cong_detected(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	uint32_t	mss = tcp->tcp_mss;
	int		npkt;

	/* npkt is equal to half the number of segments in flight */
	npkt = ((tcp->tcp_snxt - tcp->tcp_suna) >> 1) / mss;

	if (cc->cc_flags & TCP_CC_FLG_DUPACK) {

		/*
		 * If we have reduced tcp_ssthresh because of ECN, do not reduce
		 * it again unless it is already one window of data away.
		 * After one window of data, tcp_cwr should then be cleared.
		 * Note that for non ECN capable connection, tcp_cwr should
		 * always be false.
		 *
		 * Adjust cwnd since the duplicate ack indicates that a packet
		 * was dropped (due to congestion.)
		 */
		if (!tcp->tcp_cwr) {
			tcp->tcp_cwnd_ssthresh = MAX(npkt, 2) * mss;
			tcp->tcp_cwnd = (npkt + tcp->tcp_dupack_cnt) * mss;
		}

		tcp_cc_cwr_set(tcp);
		tcp_cc_set_rexmit(cc);

	/* Congestion detected due to ECE flag set */
	} else if (cc->cc_flags & TCP_CC_FLG_ECE) {
		if (tcp->tcp_ecn_ok && !tcp->tcp_cwr) {
			tcp->tcp_cwnd_ssthresh = MAX(npkt, 2) * mss;
			tcp->tcp_cwnd = npkt * mss;
			tcp_cc_cwr_set(tcp);
		}

	/* Congestion detected due to Timeout Retransmission */
	} else if (cc->cc_flags & TCP_CC_FLG_RTO) {
		/*
		 * After retransmission, we need to do slow start. Set the
		 * ssthresh to one half of current effective window and
		 * cwnd to one MSS.  Also reset tcp_cwnd_cnt.
		 *
		 * Note that if tcp_ssthresh is reduced because of ECN, do not
		 * reduce it again unless it is already one window of data away
		 * (tcp_cwr should then be cleared) or this is a timeout for a
		 * retransmitted segment.
		 */
		if (!tcp->tcp_cwr || tcp->tcp_rexmit) {
			if (tcp->tcp_timer_backoff)
				npkt = tcp->tcp_cwnd_ssthresh / mss;
			tcp->tcp_cwnd_ssthresh = MAX(npkt, 2) * mss;
		}
		tcp->tcp_cwnd = mss;
		tcp->tcp_cwnd_cnt = 0;
		tcp_cc_cwr_set(tcp);

	}
}

void
newreno_cong_recovered(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	tcp_stack_t	*tcps = tcp->tcp_tcps;
	uint32_t	mss = tcp->tcp_mss;

	ASSERT(tcp->tcp_dupack_cnt >= tcps->tcps_dupack_fast_retransmit);
	ASSERT(tcp->tcp_rexmit == B_FALSE);

	/*
	 * If we got an ACK after fast retransmit, check to see if it is a
	 * partial ACK.  If it is not and the congestion window was inflated
	 * to account for the other side's cached packets, retract it.
	 * If it is a partial ACK, retransmit next packet after acked pkt
	 */
	if (SEQ_GEQ(cc->cc_seg_ack, tcp->tcp_rexmit_max)) {
		tcp->tcp_dupack_cnt = 0;
		/*
		 * Restore the orig tcp_cwnd_ssthresh after
		 * fast retransmit phase.
		 */
		if (tcp->tcp_cwnd > tcp->tcp_cwnd_ssthresh) {
			tcp->tcp_cwnd = tcp->tcp_cwnd_ssthresh;
		}
		tcp->tcp_rexmit_max = cc->cc_seg_ack;
		tcp->tcp_cwnd_cnt = 0;

		/*
		 * Remove all notsack info to avoid confusion with
		 * the next fast retrasnmit/recovery phase.
		 */
		if (tcp->tcp_snd_sack_ok) {
			TCP_NOTSACK_REMOVE_ALL(tcp->tcp_notsack_list, tcp);
		}
	} else {
		if (tcp->tcp_snd_sack_ok && tcp->tcp_notsack_list != NULL) {
			tcp->tcp_pipe -= mss;
			if (tcp->tcp_pipe < 0)
				tcp->tcp_pipe = 0;
			cc->cc_flags |= TCP_CC_OUT_SACK_REXMIT;
		} else {
			/*
			 * Partial Ack Processing.
			 *
			 * Retransmit the unack'ed segment and restart fast
			 * recovery.  Note that we need to scale back tcp_cwnd
			 * to the original value when we started fast recovery.
			 * This is to prevent overly aggressive behaviour in
			 * sending new segments.
			 */
			tcp->tcp_cwnd = tcp->tcp_cwnd_ssthresh +
			    tcps->tcps_dupack_fast_retransmit * mss;
			tcp->tcp_cwnd_cnt = tcp->tcp_cwnd;
			cc->cc_flags |= TCP_CC_OUT_REXMIT;
		}
	}
}

void
newreno_post_idle(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;

	TCP_SET_INIT_CWND(tcp, tcp->tcp_mss,
		tcp->tcp_tcps->tcps_slow_start_after_idle);
}
/* End of Newreno support functions */
