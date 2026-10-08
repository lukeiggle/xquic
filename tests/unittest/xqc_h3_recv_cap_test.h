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

/* The suppression itself: while the latch is set, process_data must not
 * call xqc_stream_recv(), so stream_data_in.next_read_offset does not
 * advance and no MAX_STREAM_DATA is written. This is the assertion the
 * whole feature exists for — the accounting and the latch are inert
 * without it. */
void xqc_test_h3_recv_paused_stops_transport_read(void);

/* The flow-control consequence, asserted on the credit itself rather than
 * on the read offset: while paused, xqc_stream_do_recv_flow_ctl() is never
 * reached, so fc_max_stream_data_can_recv does not move and no
 * MAX_STREAM_DATA frame is queued — even with the window deliberately
 * armed one step below the threshold that would otherwise force one. After
 * the application drains below the low-water, both resume. Without this
 * the peer would keep receiving credit and the cap would bound nothing. */
void xqc_test_h3_recv_paused_withholds_stream_credit(void);

/* Terminal-state escapes. A paused stream must still process a peer reset
 * and must still be torn down cleanly by a connection close: the drain
 * that would otherwise release the latch is never coming once the peer has
 * gone, so a suppression that swallowed these would strand the stream and
 * its charge for the life of the connection. */
void xqc_test_h3_recv_reset_while_paused(void);
void xqc_test_h3_recv_close_while_paused(void);

/* The in-loop half of the suppression. process_data reads in 4 KiB steps and
 * loops while each step fills the buffer, so a burst larger than one step
 * would be drained whole -- latching on the first step and then ignoring the
 * latch for the rest -- if the check only sat at the function entry. The
 * overshoot the feature promises is one transport read, not one burst. */
void xqc_test_h3_recv_pause_bounds_overshoot_to_one_read(void);

/* The public accessor, including its NULL contract. */
void xqc_test_h3_conn_get_recv_buffered_bytes(void);

/* Resuming without a new packet. While paused the peer is held at the
 * stream window, so once the application drains below max/2 nothing on the
 * wire will schedule the stream again; the release itself has to. From a
 * read notify that means the next read happens in the same engine pass;
 * from outside the engine it means the engine timer is re-armed so the
 * connection, now on the active queue, is serviced promptly. A partial
 * drain that stops at or above max/2 schedules nothing. */
void xqc_test_h3_recv_resume_within_engine_pass(void);
void xqc_test_h3_recv_resume_outside_engine_wakes_engine(void);

#endif /* _XQC_H3_RECV_CAP_TEST_H */
