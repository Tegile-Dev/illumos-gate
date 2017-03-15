/*
 * Copyright (c) 2016, Tegile Systems Inc. All rights reserved.
 */

#include <inet/tcp.h>
#include <inet/tcp_impl.h>
#include <inet/tcp_cc_cubic.h>

extern void tcp_cc_cwr_set(tcp_t *tcp);
extern void tcp_cc_set_rexmit(struct cc_st *cc);
extern void newreno_ack_received(struct cc_st *cc);
extern void newreno_cong_detected(struct cc_st *cc);
extern void newreno_cong_recovered(struct cc_st *cc);

/*
 * +-------------------------+
 * | CUBIC support functions |
 * +-------------------------+
 */
void
cubic_init(struct cc_st *cc)
{
	cubic_priv_t *cubic;

	cubic = kmem_zalloc(sizeof (*cubic), KM_SLEEP);
	cubic->last_cong = LBOLT_FASTPATH;
	cubic->min_rtt = 0;
	cubic->mean_rtt = 1;

	cc->cc_priv = (void *)cubic;
}

void
cubic_fini(struct cc_st *cc)
{
	if (cc->cc_priv)
		kmem_free(cc->cc_priv, sizeof (cubic_priv_t));
}

void
cubic_conn_init(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	cubic_priv_t 	*cubic = (cubic_priv_t *)cc->cc_priv;

	/* Initializing wmax */
	cubic->wmax = tcp->tcp_cwnd;
}

static void
cubic_record_rtt(struct cc_st *cc)
{
	cubic_priv_t 	*cubic = (cubic_priv_t *)cc->cc_priv;
	tcp_t		*tcp = cc->cc_tcp;
	clock_t		sa = tcp->tcp_rtt_sa >> 3; /* keeps 8 avg samples */

	if (cubic->min_rtt > sa) {
		cubic->min_rtt = max(1, sa);

		/*
		 * Ensure mean_rtt has reasonable value, note that epoch average
		 * rtt is calculated only after congestion.
		 */
		if (cubic->min_rtt > cubic->mean_rtt) {
			cubic->mean_rtt = cubic->min_rtt;
		}
	}

	cubic->sum_rtt += sa;
	cubic->ack_count++;
}

void
cubic_ack_received(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	cubic_priv_t 	*cubic = (cubic_priv_t *)cc->cc_priv;
	uint32_t	cwnd = tcp->tcp_cwnd;
	uint32_t	w_aimd, w_cubic, tm;

	if (cc->cc_flags & TCP_CC_FLG_DUPACK) {
		/*
		 * Cubic does not make any changes to fast recovery and
		 * retransmit. It uses standard TCP algorithms for window
		 * update during fast recovery and congestion avoidance phase
		 */
		newreno_ack_received(cc);
		return;
	}

	/* Record rtt, but Skip first CUBIC_MIN_RTT_SAMPLES */
	if (tcp->tcp_rtt_update >= CUBIC_MIN_RTT_SAMPLES)
		cubic_record_rtt(cc);

	/* During Slow Start phase, cubic uses standard window update */
	if ((!tcp->tcp_ecn_ok || !(cc->cc_flags & TCP_CC_FLG_ECE)) &&
	    (cwnd < tcp->tcp_cwnd_ssthresh)) {
		tcp->tcp_cwnd = MIN(cwnd + tcp->tcp_mss, tcp->tcp_cwnd_max);
		return;
	}

	/* 'standard' and 'cubic' window estimations. */
	tm = LBOLT_FASTPATH - cubic->last_cong;
	w_aimd = std_cwnd(tm, cubic->mean_rtt, cubic->wmax, tcp->tcp_mss);
	w_cubic = cubic_cwnd(tm + cubic->mean_rtt, cubic->wmax,
	    tcp->tcp_mss, cubic->K);

	if (w_cubic < w_aimd) {
		/* If TCP friendly region, use standard window growth */
		tcp->tcp_cwnd = MIN(tcp->tcp_cwnd_max, w_aimd);

	} else if (cwnd < w_cubic) {
		/*
		 * If concave or convex region, use cubic growth
		 * i.e for each ACK received, cwnd must be incremented by
		 * (w_cubic - cwnd)/cwnd
		 */
		tcp->tcp_cwnd = MIN(tcp->tcp_cwnd_max,
		    cwnd + ((w_cubic - cwnd) * tcp->tcp_mss) / cwnd);
	}

	/* Set wmax, if no congestion happened yet */
	if (cubic->cong_count == 0 && cubic->wmax < tcp->tcp_cwnd)
		cubic->wmax = tcp->tcp_cwnd;
}

void
cubic_cong_detected(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	cubic_priv_t 	*cubic = (cubic_priv_t *)cc->cc_priv;
	uint32_t	mss = tcp->tcp_mss;
	int		npkt;

	if (cc->cc_flags & TCP_CC_FLG_DUPACK) {
		if (!tcp->tcp_cwr) {
			cubic->cong_count++;
			cubic->last_cong = LBOLT_FASTPATH;
			cubic->prev_wmax = cubic->wmax;
			cubic->wmax = tcp->tcp_cwnd;

			/*
			 * congestion window reduction expected to happen
			 * during cngestion avoidence.
			 */
			npkt = CUBIC_CONG_W(tcp->tcp_snxt - tcp->tcp_suna,
			    cubic->cong_count);
			tcp->tcp_cwnd_ssthresh = MAX(npkt / mss, 2) * mss;
			tcp->tcp_cwnd = (npkt + tcp->tcp_dupack_cnt) * mss;
		}
		tcp_cc_cwr_set(tcp);
		tcp_cc_set_rexmit(cc);

	/* Congestion detected due to ECE flag set */
	} else if (cc->cc_flags & TCP_CC_FLG_ECE) {
		if (tcp->tcp_ecn_ok && !tcp->tcp_cwr) {
			cubic->cong_count++;
			cubic->last_cong = LBOLT_FASTPATH;
			cubic->prev_wmax = cubic->wmax;
			cubic->wmax = tcp->tcp_cwnd;

			npkt = CUBIC_CONG_W(tcp->tcp_snxt - tcp->tcp_suna,
			    cubic->cong_count);
			tcp->tcp_cwnd_ssthresh = MAX(npkt / mss, 2) * mss;
			tcp->tcp_cwnd = npkt * mss;
			tcp_cc_cwr_set(tcp);
		}

	/* Congestion detected due to Timeout Retransmission */
	} else if (cc->cc_flags & TCP_CC_FLG_RTO) {

		/*
		 * The congestion counter will only be incremented second time
		 * onwards to avoid any possible alarms, though we decrement
		 */
		if (tcp->tcp_timer_backoff) {
			cubic->cong_count++;
			cubic->last_cong = LBOLT_FASTPATH;
		}

		/* For RTO case, cwnd changes for cubic is same as newreno. */
		newreno_cong_detected(cc);
	}
}

void
cubic_cong_recovered(struct cc_st * cc)
{
	tcp_t		*tcp = cc->cc_tcp;
	cubic_priv_t 	*cubic = (cubic_priv_t *)cc->cc_priv;
	uint32_t	mss = tcp->tcp_mss;

	/* Fast convergence heuristic, reduce wmax for downward trend */
	if (cubic->wmax < cubic->prev_wmax)
		cubic->wmax = CALC_CUBIC_FACTOR(cubic->wmax, CUBIC_FC_FACTOR);

	/* Leverage newreno routine and we can adjust cwnd later */
	newreno_cong_recovered(cc);

	/* Adjust cwnd as per CUBIC algorithm for congestion avoidence phase */
	if (tcp->tcp_cwnd_ssthresh < tcp->tcp_snxt - tcp->tcp_suna) {
		tcp->tcp_cwnd = MIN(tcp->tcp_cwnd_max,
		    CALC_CUBIC_FACTOR(cubic->wmax, CUBIC_BETA));
	}

	/* Average RTT between congestion epochs */
	if (cubic->ack_count && cubic->sum_rtt >= cubic->ack_count)
		cubic->mean_rtt = cubic->sum_rtt / cubic->ack_count;

	cubic->last_cong = LBOLT_FASTPATH;
	cubic->ack_count = 0;
	cubic->sum_rtt = 0;
	cubic->K = cubic_k(cubic->wmax / mss);
}

void
cubic_post_idle(struct cc_st *cc)
{
	tcp_t		*tcp = cc->cc_tcp;

	TCP_SET_INIT_CWND(tcp, tcp->tcp_mss,
		tcp->tcp_tcps->tcps_slow_start_after_idle);
}

/* End of CUBIC support functions */
