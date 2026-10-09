/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#include <CUnit/CUnit.h>
#include <string.h>
#include "xquic/xquic.h"
#include "src/common/xqc_time.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/congestion_control/xqc_sample.h"
#include "src/congestion_control/xqc_bbr2.h"
#include "xqc_common_test.h"
#include "xqc_bbr2_guard_test.h"

/*
 * These tests put a BBRv2 instance directly into a PROBE_BW phase and feed
 * it ACK samples through the congestion-control callback, so that every
 * other part of the ACK processing runs as it does on a connection. A
 * fixture connection supplies the send_ctl the controller logs through.
 *
 * Bandwidths are in bytes per second. A sample's bandwidth is delivered over
 * a fixed 20 ms interval, so every value below is exact.
 */

#define RIG_MIN_RTT       20000     /* us */
#define RIG_INTERVAL      20000     /* us, per sample */
#define RIG_BW_EST        12500000  /* bw_hi[0] the rig starts from */
#define RIG_SRTT_BASE     20000     /* per-round srtt before a jump */
#define RIG_SRTT_JUMP     30000     /* 1.5x: above the 1.25x threshold */

typedef struct {
    xqc_connection_t *conn;
    xqc_send_ctl_t   *send_ctl;
    xqc_bbr2_t        bbr2;
    xqc_usec_t        now;
    uint64_t          delivered;
} bbr2_rig_t;

typedef struct {
    int       round_start;
    int       app_limited;
    int       srtt_jump;
    uint32_t  bw;
    uint32_t  prior_inflight;
    uint64_t  tx_in_flight;
    uint32_t  lost_pkts;
} rig_ack_t;

static void
rig_setup(bbr2_rig_t *rig, uint32_t flags, xqc_bbr2_pacing_gain_phase phase)
{
    memset(rig, 0, sizeof(*rig));
    rig->conn = test_engine_connect();
    CU_ASSERT_PTR_NOT_NULL_FATAL(rig->conn);
    CU_ASSERT_PTR_NOT_NULL_FATAL(rig->conn->conn_initial_path);
    rig->send_ctl = rig->conn->conn_initial_path->path_send_ctl;

    xqc_cc_params_t params;
    memset(&params, 0, sizeof(params));
    params.cc_optimization_flags = flags;
    xqc_sample_t init_sample;
    memset(&init_sample, 0, sizeof(init_sample));
    init_sample.srtt = RIG_MIN_RTT;
    init_sample.send_ctl = rig->send_ctl;
    xqc_bbr2_cb.xqc_cong_ctl_init_bbr(&rig->bbr2, &init_sample, params);

    rig->now = xqc_monotonic_timestamp();
    rig->delivered = 1000000;

    xqc_bbr2_t *b = &rig->bbr2;
    b->full_bandwidth_reached = TRUE;
    b->mode = BBR2_PROBE_BW;
    b->cycle_idx = phase;
    b->ack_phase = BBR2_ACKS_INIT;
    b->min_rtt = RIG_MIN_RTT;
    b->min_rtt_stamp = rig->now;
    b->probe_rtt_min_us = RIG_MIN_RTT;
    b->probe_rtt_min_us_stamp = rig->now;
    b->bw_hi[0] = RIG_BW_EST;
    b->bw_hi[1] = 0;
    b->congestion_window = 1000000;
    /* No clock- or Reno-driven probe during the test. */
    b->cycle_start_stamp = rig->now;
    b->probe_wait_us = 10 * 1000000;
    b->rounds_since_probe = 0;
    b->next_round_delivered = rig->delivered + 1;
    b->loss_round_delivered = rig->delivered + 1;
    b->srtt_in_last_round = RIG_SRTT_BASE;
    b->srtt_in_current_round = RIG_SRTT_BASE;
}

static void
rig_teardown(bbr2_rig_t *rig)
{
    xqc_engine_destroy(rig->conn->engine);
}

static void
rig_ack(bbr2_rig_t *rig, const rig_ack_t *a)
{
    xqc_bbr2_t *b = &rig->bbr2;
    xqc_sample_t s;
    memset(&s, 0, sizeof(s));

    /* The per-round srtt pair the fast-convergence rule compares on this
     * ACK; the controller shifts it after the comparison. */
    b->srtt_in_last_round = RIG_SRTT_BASE;
    b->srtt_in_current_round = a->srtt_jump ? RIG_SRTT_JUMP : RIG_SRTT_BASE;

    rig->now += 1000;
    rig->delivered += XQC_MSS;

    s.now = rig->now;
    s.send_ctl = rig->send_ctl;
    s.interval = RIG_INTERVAL;
    s.delivered = (uint32_t)((uint64_t)a->bw * RIG_INTERVAL / 1000000);
    s.acked = XQC_MSS;
    s.prior_delivered = a->round_start ? b->next_round_delivered
                                       : b->next_round_delivered - 1;
    s.total_acked = rig->delivered;
    s.prior_inflight = a->prior_inflight ? a->prior_inflight : 1000000;
    s.bytes_inflight = s.prior_inflight;
    s.rtt = RIG_SRTT_BASE;
    s.srtt = RIG_SRTT_BASE;
    s.is_app_limited = a->app_limited;
    s.tx_in_flight = a->tx_in_flight;
    s.lost_pkts = a->lost_pkts;
    s.loss = a->lost_pkts * XQC_MSS;
    s.po_sent_time = rig->now;

    xqc_bbr2_cb.xqc_cong_ctl_on_ack_multiple_pkts(b, &s);
}

static void
ack(bbr2_rig_t *rig, int round_start, int app_limited, int srtt_jump,
    uint32_t bw)
{
    rig_ack_t a = {
        .round_start = round_start,
        .app_limited = app_limited,
        .srtt_jump = srtt_jump,
        .bw = bw,
    };
    rig_ack(rig, &a);
}

static void
fc_unguarded_in_phase(xqc_bbr2_pacing_gain_phase phase)
{
    bbr2_rig_t rig;
    rig_setup(&rig, XQC_BBR2_FLAG_FAST_CONVERGENCE, phase);

    /* A jump on an ACK in the middle of a round promotes that ACK's sample. */
    ack(&rig, 0, 0, 1, 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 0);

    /* So does the next one, in the same round. */
    ack(&rig, 0, 0, 1, 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 2000000);

    /* An ACK without a jump only fills the slot... */
    ack(&rig, 0, 0, 0, 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 1000000);
    /* ...and a jump on an app-limited ACK promotes what it holds. */
    ack(&rig, 1, 1, 1, 500000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 0);

    rig_teardown(&rig);
}

void
xqc_test_bbr2_fc_advance_unguarded(void)
{
    fc_unguarded_in_phase(BBR2_BW_PROBE_DOWN);
    fc_unguarded_in_phase(BBR2_BW_PROBE_CRUISE);
}

static void
fc_guarded_in_phase(xqc_bbr2_pacing_gain_phase phase)
{
    bbr2_rig_t rig;
    rig_setup(&rig, XQC_BBR2_FLAG_FAST_CONVERGENCE
                    | XQC_BBR2_FLAG_FC_ADVANCE_GUARDED, phase);

    /* No round start: the sample is kept, nothing is promoted. */
    ack(&rig, 0, 0, 1, 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], RIG_BW_EST);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 3000000);

    /* Round start, but the ACK's sample is app-limited. */
    ack(&rig, 1, 1, 1, 500000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], RIG_BW_EST);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 3000000);

    /* Round start without a jump. */
    ack(&rig, 1, 0, 0, 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], RIG_BW_EST);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 3000000);

    /* Round start, not app-limited, jump: the one advance of this cycle. */
    ack(&rig, 1, 0, 1, 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 0);

    /* A second qualifying jump in the same cycle does not advance again. */
    ack(&rig, 1, 0, 1, 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, phase);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 1000000);

    rig_teardown(&rig);
}

void
xqc_test_bbr2_fc_advance_guarded_down(void)
{
    fc_guarded_in_phase(BBR2_BW_PROBE_DOWN);
}

void
xqc_test_bbr2_fc_advance_guarded_cruise(void)
{
    fc_guarded_in_phase(BBR2_BW_PROBE_CRUISE);
}

void
xqc_test_bbr2_fc_advance_guarded_new_cycle(void)
{
    bbr2_rig_t rig;
    rig_setup(&rig, XQC_BBR2_FLAG_FAST_CONVERGENCE
                    | XQC_BBR2_FLAG_FC_ADVANCE_GUARDED, BBR2_BW_PROBE_DOWN);

    /* Spend this cycle's advance. */
    ack(&rig, 1, 0, 1, 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    ack(&rig, 1, 0, 1, 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 2000000);

    /* A probe that has run for min_rtt with 1.25 x BDP in flight and found
     * no more bandwidth ends, and the controller starts a new cycle in
     * PROBE_DOWN. */
    rig.bbr2.cycle_idx = BBR2_BW_PROBE_UP;
    rig.bbr2.cycle_start_stamp = rig.now - 2 * RIG_MIN_RTT;
    rig.bbr2.bw_before_probe = 3000000;
    ack(&rig, 0, 0, 0, 1000000);
    CU_ASSERT_EQUAL_FATAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.ack_phase, BBR2_ACKS_PROBE_STOPPING);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 3000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 2000000);

    /* The first round start of the new cycle makes the end-of-probe
     * advance; the jump on the same ACK finds the slot empty and must not
     * use up the cycle's budget. */
    ack(&rig, 1, 0, 1, 1500000);
    CU_ASSERT_EQUAL(rig.bbr2.ack_phase, BBR2_ACKS_INIT);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 2000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 0);

    /* The new cycle's advance. */
    ack(&rig, 1, 0, 1, 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 0);

    /* And only one. */
    ack(&rig, 1, 0, 1, 500000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[0], 1000000);
    CU_ASSERT_EQUAL(rig.bbr2.bw_hi[1], 500000);

    rig_teardown(&rig);
}

/*
 * inflight_hi. The rig is in PROBE_UP on samples from a bandwidth probe,
 * with an estimate low enough that 0.7 * target_inflight (67500 * 0.7 here)
 * is below the in-flight level of the lossy sample.
 */
#define RIG_LOW_BW        3375000
#define RIG_CEILING       (400 * XQC_MSS)
/* 1 - xqc_bbr2_inflight_lo_beta, computed as the controller does (beta is
 * a float there). */
#define RIG_BETA_KEEP     (1.0 - (double)0.3f)

static void
rig_setup_probe_up(bbr2_rig_t *rig, uint32_t flags, uint32_t inflight_hi)
{
    rig_setup(rig, flags, BBR2_BW_PROBE_UP);
    rig->bbr2.bw_hi[0] = RIG_LOW_BW;
    rig->bbr2.bw_probe_samples = 1;
    rig->bbr2.inflight_hi = inflight_hi;
}

static void
loss_ack(bbr2_rig_t *rig, uint32_t inflight_pkts, uint32_t lost_pkts,
    int app_limited)
{
    rig_ack_t a = {
        .app_limited = app_limited,
        .bw = 3000000,
        .prior_inflight = inflight_pkts * XQC_MSS,
        .tx_in_flight = (uint64_t)inflight_pkts * XQC_MSS,
        .lost_pkts = lost_pkts,
    };
    rig_ack(rig, &a);
}

void
xqc_test_bbr2_inflight_hi_cut_unfloored(void)
{
    bbr2_rig_t rig;
    rig_setup_probe_up(&rig, 0, RIG_CEILING);

    /* 2 of 85 packets lost is above the 2% threshold. */
    loss_ack(&rig, 85, 2, 0);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_hi, 85 * XQC_MSS);

    rig_teardown(&rig);
}

void
xqc_test_bbr2_inflight_hi_cut_floored(void)
{
    bbr2_rig_t rig;
    const uint32_t flag = XQC_BBR2_FLAG_INFLIGHT_HI_FLOOR;
    const uint32_t floor = (uint32_t)(RIG_CEILING * RIG_BETA_KEEP);

    /* The same loss far below inflight_hi takes off at most 30%. */
    rig_setup_probe_up(&rig, flag, RIG_CEILING);
    loss_ack(&rig, 85, 2, 0);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_hi, floor);
    CU_ASSERT(rig.bbr2.inflight_hi > 85 * XQC_MSS);
    rig_teardown(&rig);

    /* A loss above the floor still sets inflight_hi to its in-flight level. */
    rig_setup_probe_up(&rig, flag, RIG_CEILING);
    loss_ack(&rig, 380, 8, 0);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_hi, 380 * XQC_MSS);
    rig_teardown(&rig);

    /* No inflight_hi yet: there is nothing to keep. */
    rig_setup_probe_up(&rig, flag, 0xffffffffU);
    loss_ack(&rig, 85, 2, 0);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_hi, 85 * XQC_MSS);
    rig_teardown(&rig);

    /* An app-limited sample does not move inflight_hi. */
    rig_setup_probe_up(&rig, flag, RIG_CEILING);
    loss_ack(&rig, 85, 2, 1);
    CU_ASSERT_EQUAL(rig.bbr2.cycle_idx, BBR2_BW_PROBE_DOWN);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_hi, RIG_CEILING);
    rig_teardown(&rig);
}
