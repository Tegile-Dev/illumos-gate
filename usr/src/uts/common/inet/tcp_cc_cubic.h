/*
 * Copyright (c) 2008-2010 Lawrence Stewart <lstewart@freebsd.org>
 * Copyright (c) 2010 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Lawrence Stewart while studying at the Centre
 * for Advanced Internet Architectures, Swinburne University of Technology, made
 * possible in part by a grant from the Cisco University Research Program Fund
 * at Community Foundation Silicon Valley.
 *
 * Portions of this software were developed at the Centre for Advanced
 * Internet Architectures, Swinburne University of Technology, Melbourne,
 * Australia by David Hayes under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * Copyright (c) 2016, Tegile Systems Inc. All rights reserved.
 */

/*
 * Constant declarations and implementations of formulae mentioned in
 * Internet Draft titled "CUBIC for Fast Long-Distance Networks"
 * (draft-ietf-tcpm-cubic-02)
 */

#ifndef	_INET_TCP_CC_CUBIC_H_
#define	_INET_TCP_CC_CUBIC_H_

/* Number of bits of precision for fixed point math calcs. */
#define	CUBIC_SHIFT		8
#define	CUBIC_SHIFT_4		32

/* 0.5 << CUBIC_SHIFT. */
#define	RENO_BETA		128

/* ~0.8 << CUBIC_SHIFT. cubic_beta value with CUBIC_SHIFT */
#define	CUBIC_BETA		204

/* ~0.2 << CUBIC_SHIFT. (1 - cubic_beta) value with CUBIC_SHIFT */
#define	ONE_SUB_CUBIC_BETA	51

/* 3 * ONE_SUB_CUBIC_BETA. */
#define	THREE_X_PT2		153

/* (2 << CUBIC_SHIFT) - ONE_SUB_CUBIC_BETA. (1 + cubic_beta) with CUBIC_SHIFT */
#define	TWO_SUB_PT2		461

/* ~0.4 << CUBIC_SHIFT. */
#define	CUBIC_C_FACTOR		102

/* CUBIC fast convergence factor: ~0.9 << CUBIC_SHIFT. */
#define	CUBIC_FC_FACTOR		230

/* Don't trust s_rtt until this many rtt samples have been taken. */
#define	CUBIC_MIN_RTT_SAMPLES	8

/* macro to multiply val and f (factor with CUBIC_SHIFT) */
#define	CALC_CUBIC_FACTOR(val, f) (((val) * (f)) >> CUBIC_SHIFT)

/*
 * Congestion window is reduced by half of inflight segments for the
 * first congestion, after that it reduces by CUBIC_BETA ratio.
 */
#define	CUBIC_CONG_W(pipe, count) (((count) == 0) ? ((pipe) >> 1) : \
		((CALC_CUBIC_FACTOR((pipe), CUBIC_BETA))))

typedef struct cubic_priv_s {
	uint64_t	K; 		/* cubic constant K */
	uint32_t	wmax; 		/* cwnd at recent congestion event */
	uint32_t	prev_wmax;	/* cwnd at previous congestion event */
	uint32_t	cong_count;	/* Number of congestion events */
	uint32_t	ack_count;	/* ACKs since last congestion event */
	clock_t		last_cong;	/* Time since last congestion event */
	clock_t		min_rtt;	/* Minimum observed rtt in ticks */
	clock_t		mean_rtt;	/* Mean rtt between congestion epochs */
	clock_t		sum_rtt;	/* RTT sum in ticks across an epoch */
} cubic_priv_t;

/*
 * Compute the CUBIC K value used in the cwnd calculation, using the formula
 * below, which was mentioned as (Eq. 2) in CUBIC internet draft
 *
 * 	K = cubic_root(W_max*(1-beta_cubic)/C)
 *
 * Cubic root calculation algorithm here is adapted from Apple Computer
 * Technical Report #KT-32, titled "Computing the Cube Root"
 */
static inline uint64_t
cubic_k(uint32_t wmax_pkts)
{
	uint64_t s, K;
	uint16_t p;

	K = s = 0;
	p = 0;

	/* (wmax * (1-beta_cubic)/C with CUBIC_SHIFT worth of precision. */
	s = (((uint64_t)wmax_pkts * ONE_SUB_CUBIC_BETA) << CUBIC_SHIFT) /
	    CUBIC_C_FACTOR;

	/* Rebase s to be between 1 and 1/8 with a shift of CUBIC_SHIFT. */
	while (s >= 256) {
		s >>= 3;
		p++;
	}

	/*
	 * Calculating cubic root of s
	 *
	 * Some magic constants taken from the Apple TR with appropriate
	 * shifts: 275 == 1.072302 << CUBIC_SHIFT, 98 == 0.3812513 <<
	 * CUBIC_SHIFT, 120 == 0.46946116 << CUBIC_SHIFT.
	 */
	K = (((s * 275) >> CUBIC_SHIFT) + 98) -
	    (((s * s * 120) >> CUBIC_SHIFT) >> CUBIC_SHIFT);

	/* Multiply by 2^p to undo the rebasing of s from above. */
	return (K <<= p);
}

/*
 * Compute new cubic cwnd value using the formula below, which is
 * porvided as (Eq. 1) in CUBIC Internet Draft.
 *
 * 	W_cubic(t) = C*(t-K)^3 + W_max
 *
 */
static inline uint32_t
cubic_cwnd(clock_t ticks_since_cong, uint32_t wmax, uint32_t smss, uint64_t K)
{
	uint64_t cwnd;

	/* K is in fixed point form with CUBIC_SHIFT worth of precision. */

	/* t - K, with CUBIC_SHIFT worth of precision. */
	cwnd = (((uint64_t)ticks_since_cong << CUBIC_SHIFT) - (K * hz)) / hz;

	/* (t - K)^3, with CUBIC_SHIFT^3 worth of precision. */
	cwnd *= (cwnd * cwnd);

	/*
	 * C(t - K)^3 + wmax
	 * The down shift by CUBIC_SHIFT_4 is because cwnd has 4 lots of
	 * CUBIC_SHIFT included in the value. 3 from the cubing of cwnd above,
	 * and an extra from multiplying through by CUBIC_C_FACTOR.
	 */
	cwnd = ((cwnd * CUBIC_C_FACTOR * smss) >> CUBIC_SHIFT_4) + wmax;

	return ((uint32_t)cwnd);
}

/*
 * Compute an estimation of new cwnd for Standard TCP to check whether
 * current window size is in the TCP friendly region. The computation formula
 * is given below, which is provided as (Eq. 4) in CUBIC Internet Draft.
 *
 * W_aimd(t) = W_max*beta_aimd + [3*(1-beta_aimd)/(1+beta_aimd)] * (t/RTT)
 *
 * This is an approximation Standard TCP cwnd (newreno cwnd) after 't' ticks
 * post congestion event while using CUBIC's beta value (0.8). RTT should be the
 * average RTT estimate for the path measured over the previous congestion epoch
 * and wmax is the value of cwnd at the last congestion event.
 *
 */
static inline uint32_t
std_cwnd(clock_t ticks_since_cong, clock_t rtt, uint32_t wmax, uint32_t smss)
{
	uint64_t cwnd;

	cwnd = (((uint64_t)wmax * CUBIC_BETA) + (((THREE_X_PT2 *
	    ticks_since_cong * (uint64_t)smss) << CUBIC_SHIFT) /
	    TWO_SUB_PT2 / rtt)) >> CUBIC_SHIFT;
	return ((uint32_t)cwnd);
}

#endif /* _INET_TCP_CC_CUBIC_H_ */
