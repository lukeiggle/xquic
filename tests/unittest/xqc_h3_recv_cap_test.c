/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#include <CUnit/CUnit.h>
#include <string.h>
#include "xquic/xquic.h"
#include "src/http3/xqc_h3_conn.h"
#include "src/http3/xqc_h3_stream.h"
#include "src/http3/xqc_h3_request.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_frame.h"
#include "src/http3/xqc_h3_defs.h"
#include "xqc_h3_test.h"
#include "xqc_test_helpers.h"
#include "xqc_h3_recv_cap_test.h"

/* Same construction xqc_h3_test.c's xqc_h3_msgerr_setup uses (a real
 * engine-backed client connection, an h3 conn, a CLI_BID transport stream,
 * a REQUEST h3 stream and an eagerly created request), reached through
 * that file's existing helpers so this file owns no duplicate scaffolding.
 */
static xqc_h3_stream_t *
recv_cap_setup(xqc_connection_t **out_conn, xqc_h3_conn_t **out_h3c)
{
    return xqc_h3_msgerr_setup(out_conn, out_h3c);
}

static void
recv_cap_teardown(xqc_h3_stream_t *h3s, xqc_h3_conn_t *h3c, xqc_connection_t *conn)
{
    xqc_h3_msgerr_teardown(h3s, h3c, conn);
}

/* A second request stream on the SAME h3 connection, built the way the
 * fixture above builds its first one. Needed because h3_recv_buffered_bytes
 * is a sum: with one request live it is indistinguishable from a copy of
 * that request's body_buf_bytes. */
static xqc_h3_stream_t *
recv_cap_add_request(xqc_connection_t *conn, xqc_h3_conn_t *h3c)
{
    xqc_stream_t *stream = xqc_create_stream_with_conn(conn,
            XQC_UNDEFINE_STREAM_ID, XQC_CLI_BID, NULL, NULL);
    if (stream == NULL) {
        return NULL;
    }

    xqc_h3_stream_t *h3s = xqc_h3_stream_create(h3c, stream,
            XQC_H3_STREAM_TYPE_REQUEST, NULL);
    if (h3s == NULL) {
        return NULL;
    }

    h3s->h3r = xqc_h3_request_create_inner(h3c, h3s, NULL);
    if (h3s->h3r == NULL) {
        return NULL;
    }

    return h3s;
}

/* Tears down only the extra stream; the h3 conn and transport conn stay up
 * so the caller can still read the connection sum afterwards. */
static void
recv_cap_drop_request(xqc_h3_stream_t *h3s)
{
    xqc_stream_t *stream = h3s->stream;
    stream->stream_flag |= XQC_STREAM_FLAG_DISCARDED;
    xqc_h3_stream_destroy(h3s);
    xqc_destroy_stream(stream);
}

/* Feed `len` bytes of body as one well-formed HTTP/3 DATA frame through
 * the real parser (xqc_h3_stream_process_in), so the accounting under test
 * is exercised at its actual call site rather than by poking the counter.
 * len is kept < 64 per call so the frame length is a one-byte varint;
 * callers needing more send several frames. */
static void
feed_data_frame(xqc_h3_stream_t *h3s, size_t len)
{
    unsigned char frame[128];
    size_t fl = 0;
    CU_ASSERT_FATAL(len < 64 && len + 2 <= sizeof(frame));
    frame[fl++] = 0x00;                 /* DATA frame type */
    frame[fl++] = (unsigned char)len;   /* length, 1-byte varint */
    memset(frame + fl, 'x', len);
    fl += len;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_in(h3s, frame, fl, XQC_FALSE), XQC_OK);
}

void
xqc_test_h3_recv_body_buf_accounting(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    feed_data_frame(h3s, 40);
    feed_data_frame(h3s, 20);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 60);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 60);

    /* Partial drain: a read smaller than the head buffer must credit back
     * exactly what it took, not the whole buffer. */
    unsigned char out[16];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 16);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 44);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 44);

    /* Full drain across the buffer boundary returns to zero on both. */
    unsigned char big[128];
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, big, sizeof(big), &fin), 44);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_body_buf_conn_sum_on_destroy(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    feed_data_frame(h3s, 50);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 50);

    /* Destroying a request with body still buffered must return its charge
     * to the connection sum. Leaking it here is invisible locally and shows
     * up in production as a gauge that only ever climbs. */
    xqc_h3_request_destroy(h3s->h3r);
    h3s->h3r = NULL;
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_latches_at_high_water(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;

    feed_data_frame(h3s, 60);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    feed_data_frame(h3s, 10); /* 70 >= 64 */
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_holds_within_hysteresis(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s, 50);
    feed_data_frame(h3s, 50);   /* 100 >= 64 -> paused */
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    /* Drain to 60: above max/2 (32), so the latch must hold. Releasing here
     * is the bug that would turn one MAX_STREAM_DATA per window into one per
     * application read. */
    unsigned char out[40];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 40);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 60);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_releases_below_low_water(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s, 50);
    feed_data_frame(h3s, 50);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    /* xqc_stream_recv() unconditionally shuts the transport stream's read
     * side at the end of every call, so the re-arm has to put it back on
     * conn_read_streams itself. Clear the flag first so the assertion below
     * proves the re-arm set it, not that it was left over. */
    xqc_stream_shutdown_read(h3s->stream);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    unsigned char out[80];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 80);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 20);   /* 20 < 32 */
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_releases_when_max_cleared(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s, 50);
    feed_data_frame(h3s, 50);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    /* Feature switched off while this stream is paused. The low-water is now
     * 0, so "body_buf_bytes < max/2" can never hold again; only the max == 0
     * escape releases the latch, and it must do so even though the buffer is
     * still nearly full -- the setting that justified holding it is gone. */
    conn->conn_settings.h3_recv_body_buf_max = 0;

    unsigned char out[10];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 10);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 90);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_releases_at_max_one(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    /* max == 1 is the smallest value that still arms the feature, and its
     * low-water rounds to 0. Draining the buffer empty has to release, or a
     * caller who picked 1 gets a stream that pauses on its first byte and
     * never reads again. */
    conn->conn_settings.h3_recv_body_buf_max = 1;
    feed_data_frame(h3s, 10);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    unsigned char out[32];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 10);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_conn_sum_spans_requests(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    xqc_h3_stream_t *h3s2 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s2);

    feed_data_frame(h3s, 40);
    feed_data_frame(h3s2, 50);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 40);
    CU_ASSERT_EQUAL(h3s2->h3r->body_buf_bytes, 50);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 90);

    /* Draining one request must move the sum by exactly that request's
     * delta and leave the other's charge alone. */
    unsigned char out[64];
    uint8_t fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin), 40);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3s2->h3r->body_buf_bytes, 50);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 50);

    recv_cap_drop_request(h3s2);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_destroy_while_latched(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    xqc_h3_stream_t *h3s2 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s2);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s2, 50);
    feed_data_frame(h3s2, 20);   /* 70 >= 64 -> latched */
    feed_data_frame(h3s, 20);
    CU_ASSERT_TRUE(h3s2->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 90);

    /* A latched request torn down without ever being drained -- the shape a
     * peer reset or a connection close takes. Its whole charge has to come
     * back, and only its own: a sum that keeps the 70 would climb for the
     * life of the connection and read as a leak that isn't one. */
    recv_cap_drop_request(h3s2);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 20);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 20);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_unbounded_by_default(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    /* h3_recv_body_buf_max left at its 0 default. */
    CU_ASSERT_EQUAL(conn->conn_settings.h3_recv_body_buf_max, 0);

    for (int i = 0; i < 40; i++) {
        feed_data_frame(h3s, 60);
    }
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 2400);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_paused_stops_transport_read(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;

    /* Park a readable DATA frame in the transport stream's receive queue so
     * an unsuppressed process_data would certainly consume it. */
    unsigned char frame[32];
    frame[0] = 0x00;                    /* DATA */
    frame[1] = 20;                      /* length */
    memset(frame + 2, 'y', 20);
    xqc_test_push_stream_frame(h3s->stream, 0, frame, 22, 0);

    uint64_t before = h3s->stream->stream_data_in.next_read_offset;

    /* Not paused: the read happens. */
    xqc_bool_t fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_TRUE(h3s->stream->stream_data_in.next_read_offset > before);

    /* Now pause and push more: next_read_offset must not move, and the
     * stream must be off the read list so the engine does not spin on it. */
    h3s->flags |= XQC_HTTP3_STREAM_FLAG_RECV_PAUSED;
    xqc_test_push_stream_frame(h3s->stream, 22, frame, 22, 0);
    xqc_stream_ready_to_read(h3s->stream);

    before = h3s->stream->stream_data_in.next_read_offset;
    fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset, before);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    recv_cap_teardown(h3s, h3c, conn);
}

/* Put the stream's receive window one step past the point where
 * xqc_stream_do_recv_flow_ctl() must issue credit (available window below
 * half the window size), so any call to xqc_stream_recv() -- whatever it
 * reads -- grows fc_max_stream_data_can_recv and queues a MAX_STREAM_DATA.
 * With the trigger forced this way, the absence of credit is evidence that
 * the read did not happen, not evidence that the window was simply roomy. */
static void
arm_stream_credit(xqc_stream_t *stream)
{
    stream->stream_flow_ctl.fc_stream_recv_window_size = 1000;
    stream->stream_flow_ctl.fc_max_stream_data_can_recv =
        stream->stream_data_in.next_read_offset + 400;
    /* 0 suppresses the window-doubling branch, which would otherwise make
     * the post-condition depend on the fixture's srtt. */
    stream->stream_flow_ctl.fc_last_window_update_time = 0;
}

void
xqc_test_h3_recv_paused_withholds_stream_credit(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;

    unsigned char frame[64];
    frame[0] = 0x00;                    /* DATA */
    frame[1] = 50;                      /* length */
    memset(frame + 2, 'z', 50);

    /* Control: unpaused, the read issues credit. */
    xqc_test_push_stream_frame(h3s->stream, 0, frame, 52, 0);
    arm_stream_credit(h3s->stream);
    uint64_t credit = h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv;
    int msd = xqc_test_count_packets_with_frame(conn, XQC_FRAME_BIT_MAX_STREAM_DATA);

    xqc_bool_t fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 50);
    CU_ASSERT_TRUE(h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv > credit);
    CU_ASSERT_TRUE(xqc_test_count_packets_with_frame(conn,
            XQC_FRAME_BIT_MAX_STREAM_DATA) > msd);

    /* One more frame crosses the high-water and latches inside process_in;
     * the bytes it read are the bounded overshoot, so credit for them is
     * expected. What must stop is everything after. */
    xqc_test_push_stream_frame(h3s->stream, 52, frame, 52, 0);
    fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 100);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    /* Paused: arm the trigger again and prove nothing moves. */
    xqc_test_push_stream_frame(h3s->stream, 104, frame, 52, 0);
    arm_stream_credit(h3s->stream);
    xqc_stream_ready_to_read(h3s->stream);
    credit = h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv;
    msd = xqc_test_count_packets_with_frame(conn, XQC_FRAME_BIT_MAX_STREAM_DATA);
    uint64_t read_off = h3s->stream->stream_data_in.next_read_offset;

    fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset, read_off);
    CU_ASSERT_EQUAL(h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv, credit);
    CU_ASSERT_EQUAL(xqc_test_count_packets_with_frame(conn,
            XQC_FRAME_BIT_MAX_STREAM_DATA), msd);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 100);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* Drain below the low-water: the latch clears, the stream goes back on
     * the read list, and the next pass reads and issues credit again. */
    unsigned char out[128];
    uint8_t body_fin = 0;
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &body_fin), 100);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    CU_ASSERT_TRUE(h3s->stream->stream_data_in.next_read_offset > read_off);
    CU_ASSERT_TRUE(h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv > credit);
    CU_ASSERT_TRUE(xqc_test_count_packets_with_frame(conn,
            XQC_FRAME_BIT_MAX_STREAM_DATA) > msd);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_reset_while_paused(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s, 50);
    feed_data_frame(h3s, 30);   /* 80 >= 64 -> latched */
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);

    /* The state xqc_process_reset_stream_frame() leaves behind: recv side
     * in RESET_RECVD, buffered frames dropped, read side re-armed so the
     * application layer is notified. */
    xqc_stream_recv_state_update(h3s->stream, XQC_RECV_STREAM_ST_RESET_RECVD);
    xqc_stream_ready_to_read(h3s->stream);

    /* The pause must not swallow the reset. xqc_stream_recv() is the only
     * place the terminal recv state is recognised -- it moves the stream to
     * RESET_READ, shuts the read side and calls xqc_stream_maybe_need_close().
     * Suppressing here would strand the stream: the peer is gone, so the
     * drain that releases the latch is never coming. */
    xqc_bool_t fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin),
                    -XQC_H3_STREAM_RECV_ERROR);
    CU_ASSERT_EQUAL(h3s->stream->stream_state_recv, XQC_RECV_STREAM_ST_RESET_READ);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* The charge is still the reset request's until it is destroyed. */
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 80);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_close_while_paused(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    xqc_h3_stream_t *h3s2 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s2);

    conn->conn_settings.h3_recv_body_buf_max = 64;
    feed_data_frame(h3s2, 50);
    feed_data_frame(h3s2, 30);  /* 80 >= 64 -> latched */
    feed_data_frame(h3s, 20);
    CU_ASSERT_TRUE(h3s2->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 100);

    /* Suppression leaves undrained bytes on BOTH sides of the H3 boundary:
     * parsed body in body_buf and unparsed STREAM frames still queued in
     * the transport reassembly list. A connection close has to unwind both.
     * The parked frame is what distinguishes this from the destroy-while-
     * latched test, which only has the body side. */
    unsigned char frame[32];
    frame[0] = 0x00;
    frame[1] = 20;
    memset(frame + 2, 'q', 20);
    xqc_test_push_stream_frame(h3s2->stream, 0, frame, 22, 0);
    CU_ASSERT_EQUAL(h3s2->stream->stream_data_in.buffered_frame_count, 1);

    /* Same teardown a connection close drives per stream. A leaked charge
     * shows up in the sum; a leaked or doubly freed frame shows up in the
     * allocator. */
    recv_cap_drop_request(h3s2);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 20);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 20);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_pause_bounds_overshoot_to_one_read(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    conn->conn_settings.h3_recv_body_buf_max = 64;

    /* One DATA frame far larger than the 4 KiB read buffer, delivered as a
     * single contiguous run of transport bytes -- the shape a fast peer
     * produces. process_data's loop re-reads while each read fills the
     * buffer, so without an in-loop check the latch set on the first 4 KiB
     * would be ignored for the remaining five. */
    size_t body = 5 * XQC_DATA_BUF_SIZE_4K;
    size_t hdr = 5;                     /* type + 4-byte length varint */
    unsigned char *frame = xqc_malloc(hdr + body);
    CU_ASSERT_PTR_NOT_NULL_FATAL(frame);
    frame[0] = 0x00;                                    /* DATA */
    frame[1] = (unsigned char)(0x80 | ((body >> 24) & 0x3f));
    frame[2] = (unsigned char)((body >> 16) & 0xff);
    frame[3] = (unsigned char)((body >> 8) & 0xff);
    frame[4] = (unsigned char)(body & 0xff);
    memset(frame + hdr, 'w', body);
    xqc_test_push_stream_frame(h3s->stream, 0, frame, hdr + body, 0);
    xqc_free(frame);

    xqc_bool_t fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);

    /* Exactly one transport read happened. */
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset,
                    XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, XQC_DATA_BUF_SIZE_4K - hdr);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, XQC_DATA_BUF_SIZE_4K - hdr);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* The rest is still sitting in the transport queue, unread and
     * uncredited -- which is the point. */
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.merged_offset_end, hdr + body);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_conn_get_recv_buffered_bytes(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);

    CU_ASSERT_EQUAL(xqc_h3_conn_get_recv_buffered_bytes(NULL), 0);
    CU_ASSERT_EQUAL(xqc_h3_conn_get_recv_buffered_bytes(h3c), 0);

    feed_data_frame(h3s, 30);
    CU_ASSERT_EQUAL(xqc_h3_conn_get_recv_buffered_bytes(h3c), 30);

    /* Per-request export mirrors the same number. */
    xqc_request_stats_t st = xqc_h3_request_get_stats(h3s->h3r);
    CU_ASSERT_EQUAL(st.recv_body_buffered_size, 30);

    recv_cap_teardown(h3s, h3c, conn);
}
