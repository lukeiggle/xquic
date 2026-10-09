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
#include "xqc_bbr2_loss_round_test.h"

/*
 * The test puts a BBRv2 instance directly into PROBE_CRUISE, with more than
 * 4 GiB already delivered on its path, and feeds it ACK samples through the
 * congestion-control callback, so that every other part of the ACK
 * processing runs as it does on a connection. A fixture connection supplies
 * the send_ctl the controller logs through.
 *
 * Bandwidths are in bytes per second. A sample's bandwidth is delivered over
 * a fixed 20 ms interval, so every value below is exact.
 */

#define LR_MIN_RTT      20000       /* us */
#define LR_INTERVAL     20000       /* us, per sample */
#define LR_BW_EST       12500000    /* bw_hi[0] the controller starts from */
#define LR_SAMPLE_BW    3000000     /* every sample's delivery rate */
#define LR_CWND         1000000
#define LR_DELIVERED    ((1ULL << 32) + 1000000)

/* The first cut of bw_lo: from its unset value, max(bw_latest, (1 - beta) *
 * max_bw), and the round's delivery rate is the smaller of the two. */
#define LR_BW_LO_CUT    ((uint32_t)((1 - 0.3f) * (float)LR_BW_EST))

typedef struct {
    xqc_connection_t *conn;
    xqc_send_ctl_t   *send_ctl;
    xqc_bbr2_t        bbr2;
    xqc_usec_t        now;
    uint64_t          delivered;
    /* Delivered count at which the current loss round ends, as the sender
     * sees it; every ACK inside the round has a prior_delivered below it. */
    uint64_t          round_end;
} lr_rig_t;

static void
lr_setup(lr_rig_t *rig)
{
    memset(rig, 0, sizeof(*rig));
    rig->conn = test_engine_connect();
    CU_ASSERT_PTR_NOT_NULL_FATAL(rig->conn);
    CU_ASSERT_PTR_NOT_NULL_FATAL(rig->conn->conn_initial_path);
    rig->send_ctl = rig->conn->conn_initial_path->path_send_ctl;

    xqc_cc_params_t params;
    memset(&params, 0, sizeof(params));
    xqc_sample_t init_sample;
    memset(&init_sample, 0, sizeof(init_sample));
    init_sample.srtt = LR_MIN_RTT;
    init_sample.send_ctl = rig->send_ctl;
    xqc_bbr2_cb.xqc_cong_ctl_init_bbr(&rig->bbr2, &init_sample, params);

    rig->now = xqc_monotonic_timestamp();
    rig->delivered = LR_DELIVERED;
    rig->round_end = rig->delivered + 1;

    xqc_bbr2_t *b = &rig->bbr2;
    b->full_bandwidth_reached = TRUE;
    b->mode = BBR2_PROBE_BW;
    b->cycle_idx = BBR2_BW_PROBE_CRUISE;
    b->ack_phase = BBR2_ACKS_INIT;
    b->min_rtt = LR_MIN_RTT;
    b->min_rtt_stamp = rig->now;
    b->probe_rtt_min_us = LR_MIN_RTT;
    b->probe_rtt_min_us_stamp = rig->now;
    b->bw_hi[0] = LR_BW_EST;
    b->bw_hi[1] = 0;
    b->congestion_window = LR_CWND;
    /* No clock- or Reno-driven probe during the test. */
    b->cycle_start_stamp = rig->now;
    b->probe_wait_us = 10 * 1000000;
    b->rounds_since_probe = 0;
    b->next_round_delivered = rig->round_end;
    b->loss_round_delivered = rig->round_end;
    b->srtt_in_last_round = LR_MIN_RTT;
    b->srtt_in_current_round = LR_MIN_RTT;
}

static void
lr_teardown(lr_rig_t *rig)
{
    xqc_engine_destroy(rig->conn->engine);
}

/* One cwnd-limited ACK of a full-size packet. The one that ends the round
 * carries a prior_delivered at the round's end mark and moves the mark to
 * what has now been delivered. */
static void
lr_ack(lr_rig_t *rig, int round_end, uint32_t newly_lost)
{
    xqc_bbr2_t *b = &rig->bbr2;
    xqc_sample_t s;
    memset(&s, 0, sizeof(s));

    b->srtt_in_last_round = LR_MIN_RTT;
    b->srtt_in_current_round = LR_MIN_RTT;
    rig->now += 1000;
    rig->delivered += XQC_MSS;

    s.now = rig->now;
    s.send_ctl = rig->send_ctl;
    s.interval = LR_INTERVAL;
    s.delivered = (uint32_t)((uint64_t)LR_SAMPLE_BW * LR_INTERVAL / 1000000);
    s.acked = XQC_MSS;
    s.prior_delivered = round_end ? rig->round_end : rig->round_end - 1;
    s.total_acked = rig->delivered;
    s.prior_inflight = b->congestion_window - XQC_MSS + 1;
    s.bytes_inflight = s.prior_inflight;
    s.rtt = LR_MIN_RTT;
    s.srtt = LR_MIN_RTT;
    s.tx_in_flight = s.prior_inflight;
    s.lost_pkts = newly_lost;
    s.loss = newly_lost;
    s.po_sent_time = rig->now;

    xqc_bbr2_cb.xqc_cong_ctl_on_ack_multiple_pkts(b, &s);
    if (round_end) {
        rig->round_end = rig->delivered;
    }
}

void
xqc_test_bbr2_loss_round_past_4gib(void)
{
    lr_rig_t rig;
    uint32_t first_cut;
    int i;

    lr_setup(&rig);
    CU_ASSERT_EQUAL(rig.bbr2.bw_lo, 0xffffffffU);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_lo, 0xffffffffU);

    /* Lossy ACKs inside a round leave the lower bounds alone... */
    for (i = 0; i < 5; i++) {
        lr_ack(&rig, 0, 1);
        CU_ASSERT_EQUAL(rig.bbr2.loss_round_start, 0);
    }
    CU_ASSERT_EQUAL(rig.bbr2.bw_lo, 0xffffffffU);
    CU_ASSERT_EQUAL(rig.bbr2.inflight_lo, 0xffffffffU);

    /* ...and the ACK that ends it cuts them once. */
    lr_ack(&rig, 1, 0);
    CU_ASSERT_EQUAL(rig.bbr2.loss_round_start, 1);
    CU_ASSERT_EQUAL(rig.bbr2.bw_lo, LR_BW_LO_CUT);
    CU_ASSERT(rig.bbr2.inflight_lo < LR_CWND);
    first_cut = rig.bbr2.bw_lo;

    /* The next round again responds only at its end. */
    for (i = 0; i < 5; i++) {
        lr_ack(&rig, 0, 1);
        CU_ASSERT_EQUAL(rig.bbr2.loss_round_start, 0);
    }
    CU_ASSERT_EQUAL(rig.bbr2.bw_lo, first_cut);
    lr_ack(&rig, 1, 0);
    CU_ASSERT_EQUAL(rig.bbr2.loss_round_start, 1);
    CU_ASSERT(rig.bbr2.bw_lo < first_cut);

    lr_teardown(&rig);
}
