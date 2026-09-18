/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#ifndef _XQC_H3_RECV_CAP_TEST_H
#define _XQC_H3_RECV_CAP_TEST_H

/* Accounting for xqc_h3_request_t.body_buf_bytes and the per-connection
 * sum xqc_h3_conn_t.h3_recv_buffered_bytes.
 *
 * What these catch: an append site that grows body_buf without charging
 * the counter, a drain site that frees a buffer without crediting it back,
 * and a request destroy that leaks its remaining charge into the
 * connection sum (which would make the exported gauge drift upward for
 * the life of the connection and read as a leak that isn't one). */
void xqc_test_h3_recv_body_buf_accounting(void);
void xqc_test_h3_recv_body_buf_conn_sum_on_destroy(void);

/* The high-water latch and its hysteresis. Pause is level-triggered on the
 * way up (>= max) and released only below max/2, so a request sitting
 * between the two watermarks stays paused — without that gap the stream
 * would resume reading for every few bytes the application drained and
 * issue a MAX_STREAM_DATA per chunk. */
void xqc_test_h3_recv_pause_latches_at_high_water(void);
void xqc_test_h3_recv_pause_holds_within_hysteresis(void);
void xqc_test_h3_recv_pause_releases_below_low_water(void);

/* The two escapes beside the low-water. Without them the latch can outlive
 * the setting that armed it: at max == 0 (the feature switched off while a
 * stream was paused) and at max == 1 the low-water is 0, so "below half"
 * is unreachable and the stream stays paused for the life of the
 * connection — a silent stall, since the peer is blocked by the very
 * credit the pause stopped issuing. */
void xqc_test_h3_recv_pause_releases_when_max_cleared(void);
void xqc_test_h3_recv_pause_releases_at_max_one(void);

/* The connection sum across more than one live request — the case the
 * single-request tests cannot distinguish from "the gauge is just a copy of
 * body_buf_bytes", and the case that matters in production, where the
 * gauge's whole purpose is to total concurrently backpressured requests.
 * Destroying a latched request must credit its full charge and leave its
 * peers' untouched. */
void xqc_test_h3_recv_conn_sum_spans_requests(void);
void xqc_test_h3_recv_destroy_while_latched(void);

/* Default 0 must be byte-for-byte today's behaviour: unbounded buffering,
 * flag never set. This is the assertion that makes the feature safe to
 * carry upstream. */
void xqc_test_h3_recv_unbounded_by_default(void);

#endif /* _XQC_H3_RECV_CAP_TEST_H */
