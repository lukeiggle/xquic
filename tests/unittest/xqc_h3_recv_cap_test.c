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
#include "src/common/utils/vint/xqc_variable_len_int.h"
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

/* ------------------------------------------------------------------ */
/* Resuming a paused request without a new packet.                     */
/* ------------------------------------------------------------------ */

/* Give the fixture connection the transport stream callbacks an h3 ALPN
 * connection gets (the fixture connects with a bare transport ALPN), so
 * xqc_process_read_streams() -- the read step of an engine pass -- reaches
 * xqc_h3_stream_process_data() through the real read notify. */
static void
resume_route_stream_to_h3(xqc_h3_stream_t *h3s)
{
    xqc_connection_t *conn = h3s->stream->stream_conn;
    CU_ASSERT_PTR_EQUAL_FATAL(h3s->stream->user_data, h3s);
    CU_ASSERT_PTR_EQUAL_FATAL(h3s->stream->stream_if, &conn->app_proto_cbs.stream_cbs);
    conn->app_proto_cbs.stream_cbs = h3_stream_callbacks;
}

/* Park one DATA frame of `body` bytes in the transport receive queue as a
 * single contiguous run and put the stream on the connection read list, as
 * an inbound STREAM frame would. `body` must be larger than one 4 KiB read
 * for the pause to have something left to hold back. Returns the total
 * transport bytes parked. */
static size_t
resume_park_body(xqc_h3_stream_t *h3s, size_t body)
{
    size_t hdr = 5;                     /* type + 4-byte length varint */
    unsigned char *frame = xqc_malloc(hdr + body);
    CU_ASSERT_PTR_NOT_NULL_FATAL(frame);
    frame[0] = 0x00;                                    /* DATA */
    frame[1] = (unsigned char)(0x80 | ((body >> 24) & 0x3f));
    frame[2] = (unsigned char)((body >> 16) & 0xff);
    frame[3] = (unsigned char)((body >> 8) & 0xff);
    frame[4] = (unsigned char)(body & 0xff);
    memset(frame + hdr, 'r', body);
    xqc_test_push_stream_frame(h3s->stream, 0, frame, hdr + body, 0);
    xqc_free(frame);
    xqc_stream_ready_to_read(h3s->stream);
    return hdr + body;
}

static int      resume_timer_calls;
static xqc_usec_t resume_timer_last;

static void
resume_record_timer(xqc_usec_t wake_after, void *user_data)
{
    resume_timer_calls++;
    resume_timer_last = wake_after;
}

typedef struct {
    size_t  drained;
    int     notifies;
    int     notifies_while_paused;
    int     close_after_drain;
} resume_app_t;

/* An application that keeps up: on every read notify it drains the request
 * until -XQC_EAGAIN. */
static int
resume_drain_all_on_notify(xqc_h3_request_t *h3r, xqc_request_notify_flag_t flag,
    void *user_data)
{
    resume_app_t *app = (resume_app_t *)user_data;
    unsigned char buf[1024];
    uint8_t fin = 0;
    ssize_t n;

    app->notifies++;
    if (h3r->h3_stream->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED) {
        app->notifies_while_paused++;
    }

    while ((n = xqc_h3_request_recv_body(h3r, buf, sizeof(buf), &fin)) > 0) {
        app->drained += (size_t)n;
    }
    CU_ASSERT_EQUAL(n, -XQC_EAGAIN);

    if (app->close_after_drain) {
        CU_ASSERT_EQUAL(xqc_h3_request_close(h3r), XQC_OK);
    }
    return 0;
}

/* Run one read step the way the engine does, with XQC_ENG_FLAG_RUNNING set
 * for its duration, recording any engine timer re-arm made from inside it. */
static void
resume_read_step_in_engine_run(xqc_connection_t *conn)
{
    xqc_engine_t *engine = conn->engine;
    xqc_set_event_timer_pt saved = engine->eng_callback.set_event_timer;

    engine->eng_callback.set_event_timer = resume_record_timer;
    resume_timer_calls = 0;
    engine->eng_flag |= XQC_ENG_FLAG_RUNNING;
    xqc_process_read_streams(conn);
    engine->eng_flag &= ~XQC_ENG_FLAG_RUNNING;
    engine->eng_callback.set_event_timer = saved;
}

void
xqc_test_h3_recv_resume_within_engine_pass(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);

    resume_app_t app = {0};
    h3c->h3_request_callbacks.h3_request_read_notify = resume_drain_all_on_notify;
    h3s->h3r->user_data = &app;

    conn->conn_settings.h3_recv_body_buf_max = 64;
    size_t body = 5 * XQC_DATA_BUF_SIZE_4K;
    size_t parked = resume_park_body(h3s, body);

    /* One read step. Each 4 KiB transport read crosses the high-water and
     * pauses the stream; the notify that follows drains it to empty, which
     * releases the latch. Nothing else is coming to schedule this stream --
     * the peer is held at the stream window by the credit the pause did not
     * issue -- so the release has to lead to the next read within this same
     * step, not one packet or timer later. Being inside an engine run, the
     * releases must not re-arm the engine timer: the run does that on its
     * way out, and a re-arm per 4 KiB is a timer syscall per 4 KiB for an
     * event-loop embedder. */
    resume_read_step_in_engine_run(conn);

    CU_ASSERT_EQUAL(resume_timer_calls, 0);
    CU_ASSERT_TRUE(app.notifies_while_paused > 0);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset, parked);
    CU_ASSERT_EQUAL(app.drained, body);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_resume_not_after_close_in_notify(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);

    resume_app_t app = {0};
    app.close_after_drain = 1;
    h3c->h3_request_callbacks.h3_request_read_notify = resume_drain_all_on_notify;
    h3s->h3r->user_data = &app;

    conn->conn_settings.h3_recv_body_buf_max = 64;
    resume_park_body(h3s, 5 * XQC_DATA_BUF_SIZE_4K);

    /* The application drains below the low-water, which releases the latch
     * and re-arms the stream, and then cancels the request in the same
     * notify. The release must not turn into another read and another
     * notify for a request the application has just closed. */
    resume_read_step_in_engine_run(conn);

    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_ACTIVELY_CLOSED);
    CU_ASSERT_EQUAL(app.notifies, 1);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset, XQC_DATA_BUF_SIZE_4K);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_resume_outside_engine_wakes_engine(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);
    xqc_engine_t *engine = conn->engine;

    /* No read notify: this application drains later, from its own event
     * loop, when its consumer has room. */
    conn->conn_settings.h3_recv_body_buf_max = 64;
    resume_park_body(h3s, 5 * XQC_DATA_BUF_SIZE_4K);

    xqc_process_read_streams(conn);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset, XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    size_t buffered = h3s->h3r->body_buf_bytes;

    /* The engine is idle: the pass is over and the connection waits in the
     * wakeup queue for a timer well in the future. */
    xqc_engine_remove_active_queue(engine, conn);
    conn->next_tick_time = xqc_monotonic_timestamp() + 10 * 1000000;
    xqc_engine_add_wakeup_queue(engine, conn);
    CU_ASSERT_TRUE(conn->conn_flag & XQC_CONN_FLAG_WAIT_WAKEUP);
    engine->eng_callback.set_event_timer = resume_record_timer;
    resume_timer_calls = 0;

    /* A partial drain that stops at or above max/2 is the documented
     * hysteresis: still paused, nothing scheduled. */
    unsigned char out[XQC_DATA_BUF_SIZE_4K];
    uint8_t fin = 0;
    size_t keep = 40;                   /* >= 64 / 2 */
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, buffered - keep, &fin),
                    (ssize_t)(buffered - keep));
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_FALSE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    CU_ASSERT_TRUE(conn->conn_flag & XQC_CONN_FLAG_WAIT_WAKEUP);
    CU_ASSERT_EQUAL(resume_timer_calls, 0);

    /* Draining below max/2 releases the latch. The stream goes back on the
     * read list and the connection onto the active queue -- but the active
     * queue is only serviced by xqc_engine_main_logic(), which the
     * application runs from packet input and from the engine timer. No
     * packet is coming, and the timer is still set for the old deadline, so
     * the release has to re-arm the timer itself. */
    CU_ASSERT_EQUAL(xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin),
                    (ssize_t)keep);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(h3s->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    CU_ASSERT_TRUE(conn->conn_flag & XQC_CONN_FLAG_TICKING);
    CU_ASSERT_FALSE(conn->conn_flag & XQC_CONN_FLAG_WAIT_WAKEUP);
    CU_ASSERT_EQUAL(resume_timer_calls, 1);
    CU_ASSERT_TRUE(resume_timer_last <= 1);

    /* And the read step of that pass picks the stream up. */
    xqc_process_read_streams(conn);
    CU_ASSERT_EQUAL(h3s->stream->stream_data_in.next_read_offset,
                    2 * XQC_DATA_BUF_SIZE_4K);

    recv_cap_teardown(h3s, h3c, conn);
}

/* ------------------------------------------------------------------ */
/* Closing a paused request after the peer has sent everything.        */
/* ------------------------------------------------------------------ */

static int close_paused_read_notifies;
static int close_paused_request_destroyed;

/* An application that has stopped reading: it is told about the body and
 * leaves it where it is, which is what holds the request at the high-water. */
static int
close_paused_ignore_body(xqc_h3_request_t *h3r, xqc_request_notify_flag_t flag,
    void *user_data)
{
    close_paused_read_notifies++;
    return 0;
}

static int
close_paused_count_destroy(xqc_h3_request_t *h3r, void *user_data)
{
    close_paused_request_destroyed++;
    return 0;
}

void
xqc_test_h3_recv_close_paused_after_fin(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);
    xqc_stream_t *stream = h3s->stream;

    close_paused_read_notifies = 0;
    close_paused_request_destroyed = 0;
    h3c->h3_request_callbacks.h3_request_read_notify = close_paused_ignore_body;
    h3c->h3_request_callbacks.h3_request_close_notify = close_paused_count_destroy;
    conn->conn_settings.h3_recv_body_buf_max = 64;

    /* The whole body, FIN included, arrives inside the credit the stream had
     * before it paused: the receive side is Data Recvd, with most of the body
     * still unread in the transport queue. */
    size_t body = 5 * XQC_DATA_BUF_SIZE_4K;
    size_t hdr = 5;                     /* type + 4-byte length varint */
    unsigned char *frame = xqc_malloc(hdr + body);
    CU_ASSERT_PTR_NOT_NULL_FATAL(frame);
    frame[0] = 0x00;                                    /* DATA */
    frame[1] = (unsigned char)(0x80 | ((body >> 24) & 0x3f));
    frame[2] = (unsigned char)((body >> 16) & 0xff);
    frame[3] = (unsigned char)((body >> 8) & 0xff);
    frame[4] = (unsigned char)(body & 0xff);
    memset(frame + hdr, 'f', body);
    xqc_test_push_stream_frame(stream, 0, frame, hdr + body, 1);
    xqc_free(frame);
    xqc_stream_recv_state_update(stream, XQC_RECV_STREAM_ST_DATA_RECVD);
    xqc_stream_ready_to_read(stream);

    xqc_process_read_streams(conn);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_EQUAL(stream->stream_state_recv, XQC_RECV_STREAM_ST_DATA_RECVD);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, XQC_DATA_BUF_SIZE_4K - hdr);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, XQC_DATA_BUF_SIZE_4K - hdr);
    int notifies = close_paused_read_notifies;
    CU_ASSERT_TRUE(notifies > 0);

    /* The application gives up on the request without reading it. The
     * parked body has to go with it, and the unread transport data has to be
     * consumed so the receive side reaches Data Read: a peer in Data Recvd
     * has nothing to reset, so no peer action is coming to do it instead.
     * The dropped data is not offered to the application it was closed by. */
    CU_ASSERT_EQUAL(xqc_h3_request_close(h3s->h3r), XQC_OK);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(stream->stream_state_recv, XQC_RECV_STREAM_ST_DATA_READ);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, hdr + body);
    CU_ASSERT_EQUAL(stream->stream_data_in.buffered_frame_count, 0);
    CU_ASSERT_EQUAL(stream->stream_state_send, XQC_SEND_STREAM_ST_RESET_SENT);
    CU_ASSERT_EQUAL(close_paused_read_notifies, notifies);
    CU_ASSERT_EQUAL(close_paused_request_destroyed, 0);

    /* The peer acknowledges the RESET_STREAM the close sent; with both sides
     * terminal the stream joins the closing list, and the close timer
     * destroys it together with its h3 stream and request. */
    xqc_stream_send_state_update(stream, XQC_SEND_STREAM_ST_RESET_RECVD);
    xqc_stream_maybe_need_close(stream);
    CU_ASSERT_TRUE(stream->stream_flag & XQC_STREAM_FLAG_NEED_CLOSE);
    CU_ASSERT_FALSE(xqc_list_empty(&conn->conn_closing_streams));

    xqc_timer_t *timer = &conn->conn_timer_manager.timer[XQC_TIMER_STREAM_CLOSE];
    timer->timeout_cb(XQC_TIMER_STREAM_CLOSE, stream->stream_close_time, timer->user_data);
    CU_ASSERT_EQUAL(close_paused_request_destroyed, 1);
    CU_ASSERT_TRUE(xqc_list_empty(&conn->conn_closing_streams));
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    /* The stream and h3 stream are gone; only the connections remain. */
    xqc_h3_conn_destroy(h3c);
    if (conn->alpn) {
        xqc_free(conn->alpn);
    }
}

/* ------------------------------------------------------------------ */
/* Closing a paused request before the peer has sent everything.       */
/* ------------------------------------------------------------------ */

void
xqc_test_h3_recv_close_paused_then_fin(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *h3s = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);
    xqc_stream_t *stream = h3s->stream;

    close_paused_read_notifies = 0;
    close_paused_request_destroyed = 0;
    h3c->h3_request_callbacks.h3_request_read_notify = close_paused_ignore_body;
    h3c->h3_request_callbacks.h3_request_close_notify = close_paused_count_destroy;
    conn->conn_settings.h3_recv_body_buf_max = 64;

    /* One DATA frame of 5 x 4 KiB. Only the first three quarters have
     * arrived and no FIN yet: the receive side is Recv, and the pause holds
     * the rest of what did arrive in the transport queue. */
    size_t body = 5 * XQC_DATA_BUF_SIZE_4K;
    size_t first = 3 * XQC_DATA_BUF_SIZE_4K;
    size_t hdr = 5;                     /* type + 4-byte length varint */
    unsigned char *frame = xqc_malloc(hdr + body);
    CU_ASSERT_PTR_NOT_NULL_FATAL(frame);
    frame[0] = 0x00;                                    /* DATA */
    frame[1] = (unsigned char)(0x80 | ((body >> 24) & 0x3f));
    frame[2] = (unsigned char)((body >> 16) & 0xff);
    frame[3] = (unsigned char)((body >> 8) & 0xff);
    frame[4] = (unsigned char)(body & 0xff);
    memset(frame + hdr, 'f', body);
    xqc_test_push_stream_frame(stream, 0, frame, hdr + first, 0);
    xqc_stream_ready_to_read(stream);

    xqc_process_read_streams(conn);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_EQUAL(stream->stream_state_recv, XQC_RECV_STREAM_ST_RECV);

    /* The application gives up on the request. The parked body goes and the
     * pause is cleared; the receive side is still Recv, so the unread
     * transport data stays queued for the frames that are still to come. */
    CU_ASSERT_EQUAL(xqc_h3_request_close(h3s->h3r), XQC_OK);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_ACTIVELY_CLOSED);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);
    CU_ASSERT_EQUAL(stream->stream_state_recv, XQC_RECV_STREAM_ST_RECV);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, XQC_DATA_BUF_SIZE_4K);

    /* The rest of the body and the FIN arrive before the peer acts on the
     * STOP_SENDING the close sent. Reading them re-crosses the high-water on
     * the closed request's own buffer, which must not hold the stream: no
     * drain is coming for a closed request, so a pause here would leave the
     * receive side short of Data Read for good. */
    xqc_test_push_stream_frame(stream, hdr + first, frame + hdr + first, body - first, 1);
    xqc_free(frame);
    xqc_stream_recv_state_update(stream, XQC_RECV_STREAM_ST_DATA_RECVD);
    xqc_stream_ready_to_read(stream);

    xqc_process_read_streams(conn);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, hdr + body);
    CU_ASSERT_EQUAL(stream->stream_data_in.buffered_frame_count, 0);
    CU_ASSERT_EQUAL(stream->stream_state_recv, XQC_RECV_STREAM_ST_DATA_READ);
    CU_ASSERT_TRUE(xqc_list_empty(&conn->conn_read_streams));

    /* What was read after the close sits in the closed request's buffer, in
     * step with the connection sum, until the request is destroyed. */
    CU_ASSERT_TRUE(h3s->h3r->body_buf_bytes > 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, h3s->h3r->body_buf_bytes);

    /* The peer acknowledges the RESET_STREAM; with both sides terminal the
     * stream joins the closing list, and the close timer destroys it together
     * with its h3 stream and request. */
    CU_ASSERT_EQUAL(stream->stream_state_send, XQC_SEND_STREAM_ST_RESET_SENT);
    xqc_stream_send_state_update(stream, XQC_SEND_STREAM_ST_RESET_RECVD);
    xqc_stream_maybe_need_close(stream);
    CU_ASSERT_TRUE(stream->stream_flag & XQC_STREAM_FLAG_NEED_CLOSE);
    CU_ASSERT_FALSE(xqc_list_empty(&conn->conn_closing_streams));

    xqc_timer_t *timer = &conn->conn_timer_manager.timer[XQC_TIMER_STREAM_CLOSE];
    timer->timeout_cb(XQC_TIMER_STREAM_CLOSE, stream->stream_close_time, timer->user_data);
    CU_ASSERT_EQUAL(close_paused_request_destroyed, 1);
    CU_ASSERT_TRUE(xqc_list_empty(&conn->conn_closing_streams));
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);

    xqc_h3_conn_destroy(h3c);
    if (conn->alpn) {
        xqc_free(conn->alpn);
    }
}

/* ------------------------------------------------------------------ */
/* A paused request fed in many STREAM frames.                         */
/* ------------------------------------------------------------------ */

#define FRAME_GUARD_MAX_CHUNK 1024  /* largest STREAM frame payload used here */
#define FRAME_GUARD_BATCH   100     /* frames between read steps, one per packet */
#define FRAME_GUARD_HDR     5       /* DATA type + 4-byte length varint */

/* Where the guard gives way, against each transport reassembly limit
 * (xqc_insert_stream_frame): a quarter of the sparse-fragment limit, so
 * three quarters are left for the small frames that arrive between two read
 * steps, and half of the hard limit. */
#define FRAME_GUARD_SPARSE  (XQC_MAX_STREAM_FRAME_BUFFERED_COUNT / 4)
#define FRAME_GUARD_HARD    (XQC_MAX_STREAM_FRAME_BUFFERED_COUNT_HARD / 2)

/* Shape of the stream the current case feeds: frame_guard_frames frames of
 * frame_guard_chunk bytes each, carrying one DATA frame. */
static size_t frame_guard_chunk;
static size_t frame_guard_frames;
static int frame_guard_read_notifies;

static int
frame_guard_ignore_body(xqc_h3_request_t *h3r, xqc_request_notify_flag_t flag,
    void *user_data)
{
    frame_guard_read_notifies++;
    return 0;
}

/* Byte `off` of a request stream that carries one DATA frame whose body
 * fills the rest of the frame_guard_frames * frame_guard_chunk bytes. */
static unsigned char
frame_guard_stream_byte(size_t off)
{
    size_t body = frame_guard_frames * frame_guard_chunk - FRAME_GUARD_HDR;
    switch (off) {
    case 0: return 0x00;                                        /* DATA */
    case 1: return (unsigned char)(0x80 | ((body >> 24) & 0x3f));
    case 2: return (unsigned char)((body >> 16) & 0xff);
    case 3: return (unsigned char)((body >> 8) & 0xff);
    case 4: return (unsigned char)(body & 0xff);
    default: return 'g';
    }
}

/* Deliver stream bytes [off, off + frame_guard_chunk) as one STREAM frame
 * in a 1-RTT packet, through the same frame processing an inbound packet
 * gets, so the transport's per-stream frame limits apply. */
static xqc_int_t
frame_guard_deliver(xqc_connection_t *conn, xqc_stream_t *stream, size_t off)
{
    unsigned char buf[32 + FRAME_GUARD_MAX_CHUNK];
    unsigned char *p = buf;
    size_t i;

    *p++ = 0x0e;                        /* STREAM, OFF and LEN bits set */
    p = xqc_put_varint(p, stream->stream_id);
    p = xqc_put_varint(p, off);
    p = xqc_put_varint(p, frame_guard_chunk);
    for (i = 0; i < frame_guard_chunk; i++) {
        *p++ = frame_guard_stream_byte(off + i);
    }

    xqc_packet_in_t pi;
    memset(&pi, 0, sizeof(pi));
    pi.pi_pkt.pkt_type = XQC_PTYPE_SHORT_HEADER;
    pi.pi_path_id = XQC_INITIAL_PATH_ID;
    pi.pos = buf;
    pi.last = p;
    return xqc_process_frames(conn, &pi);
}

typedef struct {
    int     failed_at;              /* frame index refused, or -1 */
    xqc_int_t failed_ret;
    uint64_t max_frames_before_read;
    uint64_t max_frames_after_read;
    int     reads_while_paused_below_guard;
    int     reads_while_paused_over_guard;
} frame_guard_run_t;

/* Feed the whole stream with a read step after every FRAME_GUARD_BATCH
 * frames, as an engine pass would, while the application reads nothing. */
static void
frame_guard_feed(xqc_connection_t *conn, xqc_h3_stream_t *h3s, frame_guard_run_t *run)
{
    xqc_stream_t *stream = h3s->stream;
    size_t i;

    memset(run, 0, sizeof(*run));
    run->failed_at = -1;

    for (i = 0; i < frame_guard_frames; i++) {
        xqc_int_t ret = frame_guard_deliver(conn, stream, i * frame_guard_chunk);
        if (ret != XQC_OK) {
            run->failed_at = (int)i;
            run->failed_ret = ret;
            return;
        }

        if ((i + 1) % FRAME_GUARD_BATCH != 0 && i + 1 != frame_guard_frames) {
            continue;
        }

        uint64_t before = stream->stream_data_in.buffered_frame_count;
        uint64_t read_off = stream->stream_data_in.next_read_offset;
        xqc_bool_t paused = (h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED) != 0;
        xqc_bool_t over = xqc_h3_stream_recv_frames_over_guard(stream);

        xqc_process_read_streams(conn);

        uint64_t after = stream->stream_data_in.buffered_frame_count;
        run->max_frames_before_read = xqc_max(run->max_frames_before_read, before);
        run->max_frames_after_read = xqc_max(run->max_frames_after_read, after);
        if (paused && stream->stream_data_in.next_read_offset > read_off) {
            if (over) {
                run->reads_while_paused_over_guard++;
            } else {
                run->reads_while_paused_below_guard++;
            }
        }
    }
}

static xqc_h3_stream_t *
frame_guard_setup(xqc_connection_t **conn, xqc_h3_conn_t **h3c, uint32_t max,
    size_t chunk, size_t frames)
{
    CU_ASSERT_FATAL(chunk <= FRAME_GUARD_MAX_CHUNK);
    frame_guard_chunk = chunk;
    frame_guard_frames = frames;

    xqc_h3_stream_t *h3s = recv_cap_setup(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(h3s);
    resume_route_stream_to_h3(h3s);

    frame_guard_read_notifies = 0;
    (*h3c)->h3_request_callbacks.h3_request_read_notify = frame_guard_ignore_body;
    (*conn)->conn_settings.h3_recv_body_buf_max = max;

    /* Room for the whole stream, so neither window is what stops the peer:
     * the case is a chatty peer inside an ordinary window. */
    (*conn)->conn_flow_ctl.fc_max_data_can_recv = 16 * 1024 * 1024;
    h3s->stream->stream_flow_ctl.fc_max_stream_data_can_recv = 16 * 1024 * 1024;
    return h3s;
}

void
xqc_test_h3_recv_pause_yields_to_frame_guard(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    frame_guard_run_t run;
    size_t chunk = 64;
    size_t frames = 10000;          /* more than XQC_MAX_STREAM_FRAME_BUFFERED_COUNT */
    size_t total = frames * chunk;
    size_t body = total - FRAME_GUARD_HDR;

    /* Cap on. The first read latches the pause at the high-water and the
     * application never drains, so from then on every frame stays in the
     * transport queue as its own node. Unchecked, the queue would reach the
     * sparse-fragment limit of 8192 nodes and every packet carrying more of
     * the stream would be dropped unacknowledged. The pause has to hold while the
     * queue is below the guard and give way above it, reading just enough
     * to bring the queue back under. */
    xqc_h3_stream_t *h3s = frame_guard_setup(&conn, &h3c, 1024, chunk, frames);
    xqc_stream_t *stream = h3s->stream;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(run.failed_ret, XQC_OK);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_FALSE(conn->conn_flag & XQC_CONN_FLAG_ERROR);
    CU_ASSERT_TRUE(run.max_frames_before_read <= FRAME_GUARD_SPARSE + FRAME_GUARD_BATCH);
    CU_ASSERT_TRUE(run.max_frames_after_read <= FRAME_GUARD_SPARSE);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_TRUE(run.reads_while_paused_over_guard > 0);
    /* The margin between read steps: a read step leaves at most a quarter
     * of the transport's limit queued, so the frames that arrive before the
     * next one have three quarters of it to fill. */
    CU_ASSERT_TRUE(run.max_frames_after_read
                   <= XQC_MAX_STREAM_FRAME_BUFFERED_COUNT / 4);

    /* Still latched: the guard reads past the high-water but does not end
     * the pause, so release still waits for the application to drain. Every
     * byte read is in the body buffer and accounted on both counters. */
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(frame_guard_read_notifies > 0);
    CU_ASSERT_TRUE(stream->stream_data_in.next_read_offset > XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_TRUE(total - stream->stream_data_in.next_read_offset
                   <= FRAME_GUARD_SPARSE * chunk);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes,
                    stream->stream_data_in.next_read_offset - FRAME_GUARD_HDR);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, h3s->h3r->body_buf_bytes);
    CU_ASSERT_EQUAL(h3s->h3r->body_recvd, 0);

    /* The application drains. Release and the reads that follow deliver the
     * rest, and the counters return to zero with every body byte delivered
     * exactly once. */
    unsigned char out[XQC_DATA_BUF_SIZE_4K];
    size_t drained = 0;
    int rounds = 0;
    while (drained < body && rounds++ < 10000) {
        uint8_t fin = 0;
        ssize_t n;
        while ((n = xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin)) > 0) {
            drained += (size_t)n;
        }
        CU_ASSERT_EQUAL_FATAL(n, -XQC_EAGAIN);
        xqc_process_read_streams(conn);
    }
    CU_ASSERT_EQUAL(drained, body);
    CU_ASSERT_EQUAL(h3s->h3r->body_recvd, body);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, total);
    CU_ASSERT_EQUAL(stream->stream_data_in.buffered_frame_count, 0);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(conn->conn_err, 0);

    recv_cap_teardown(h3s, h3c, conn);

    /* Control, cap off: never paused, every read step empties the queue,
     * and the guard has nothing to do. */
    h3s = frame_guard_setup(&conn, &h3c, 0, chunk, frames);
    stream = h3s->stream;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_EQUAL(run.max_frames_before_read, FRAME_GUARD_BATCH);
    CU_ASSERT_EQUAL(run.max_frames_after_read, 0);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_EQUAL(run.reads_while_paused_over_guard, 0);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, total);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, body);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, body);

    recv_cap_teardown(h3s, h3c, conn);
}

void
xqc_test_h3_recv_frame_guard_follows_transport_limits(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    frame_guard_run_t run;
    xqc_h3_stream_t *h3s;
    xqc_stream_t *stream;

    /* Packet-sized frames: 9000 of 1 KiB is past the sparse-fragment node
     * count but dense enough that the transport keeps accepting them, so
     * the pause must hold for the whole stream. The high-water is the only
     * thing read past. */
    h3s = frame_guard_setup(&conn, &h3c, 1024, 1024, 9000);
    stream = h3s->stream;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_EQUAL(run.reads_while_paused_over_guard, 0);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(stream->stream_data_in.next_read_offset <= XQC_DATA_BUF_SIZE_4K);
    CU_ASSERT_TRUE(stream->stream_data_in.buffered_frame_count
                   >= 9000 - XQC_DATA_BUF_SIZE_4K / 1024);
    CU_ASSERT_TRUE(h3s->h3r->body_buf_bytes <= 1024 + XQC_DATA_BUF_SIZE_4K);
    recv_cap_teardown(h3s, h3c, conn);

    /* Frames dense enough for the sparse tier but small enough that the hard
     * node limit binds inside the window: the guard gives way at half of
     * it. */
    h3s = frame_guard_setup(&conn, &h3c, 1024, 300, FRAME_GUARD_HARD + 1000);
    stream = h3s->stream;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_TRUE(run.max_frames_before_read > FRAME_GUARD_SPARSE);
    CU_ASSERT_TRUE(run.max_frames_after_read <= FRAME_GUARD_HARD);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_TRUE(run.reads_while_paused_over_guard > 0);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    recv_cap_teardown(h3s, h3c, conn);

    /* A configured max_stream_frame_buffered_cnt replaces both transport
     * limits, so the guard sits at half of it for frames of any size... */
    h3s = frame_guard_setup(&conn, &h3c, 1024, 1024, 2000);
    stream = h3s->stream;
    conn->conn_settings.max_stream_frame_buffered_cnt = 1000;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_TRUE(run.max_frames_before_read > 250);
    CU_ASSERT_TRUE(run.max_frames_after_read <= 500);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_TRUE(run.reads_while_paused_over_guard > 0);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    recv_cap_teardown(h3s, h3c, conn);

    /* ...and at a quarter of it for frames under 256 bytes. */
    h3s = frame_guard_setup(&conn, &h3c, 1024, 64, 2000);
    stream = h3s->stream;
    conn->conn_settings.max_stream_frame_buffered_cnt = 1000;
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_TRUE(run.max_frames_after_read <= 250);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_TRUE(run.reads_while_paused_over_guard > 0);
    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    recv_cap_teardown(h3s, h3c, conn);
}

/* ------------------------------------------------------------------ */
/* The connection-wide high-water (h3_recv_total_buf_max).             */
/* ------------------------------------------------------------------ */

#define TOTAL_KIB 1024

/* Feed `len` bytes of body as one DATA frame through the real parser, like
 * feed_data_frame() but with a 4-byte length varint so a single call can
 * carry the hundreds of KiB these tests need to get past the floor. */
static void
total_feed_body(xqc_h3_stream_t *h3s, size_t len)
{
    size_t hdr = 5;
    unsigned char *frame = xqc_malloc(hdr + len);
    CU_ASSERT_PTR_NOT_NULL_FATAL(frame);
    frame[0] = 0x00;                                    /* DATA */
    frame[1] = (unsigned char)(0x80 | ((len >> 24) & 0x3f));
    frame[2] = (unsigned char)((len >> 16) & 0xff);
    frame[3] = (unsigned char)((len >> 8) & 0xff);
    frame[4] = (unsigned char)(len & 0xff);
    memset(frame + hdr, 't', len);
    CU_ASSERT_EQUAL(xqc_h3_stream_process_in(h3s, frame, hdr + len, XQC_FALSE), XQC_OK);
    xqc_free(frame);
}

/* The application takes exactly `len` bytes of body. */
static void
total_drain(xqc_h3_stream_t *h3s, size_t len)
{
    unsigned char out[XQC_DATA_BUF_SIZE_4K];
    uint8_t fin = 0;
    size_t taken = 0;

    while (taken < len) {
        size_t want = xqc_min(sizeof(out), len - taken);
        ssize_t n = xqc_h3_request_recv_body(h3s->h3r, out, want, &fin);
        CU_ASSERT_FATAL(n > 0);
        taken += (size_t)n;
    }
    CU_ASSERT_EQUAL(taken, len);
}

static xqc_bool_t
total_paused(xqc_h3_stream_t *h3s)
{
    return (h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED) != 0;
}

/* Park a DATA frame in the request's transport queue and run its read step.
 * Returns the number of transport bytes the step consumed. */
static uint64_t
total_read_step(xqc_h3_stream_t *h3s, size_t body)
{
    xqc_bool_t fin = XQC_FALSE;
    uint64_t before = h3s->stream->stream_data_in.next_read_offset;

    resume_park_body(h3s, body);
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(h3s->stream, h3s, &fin), XQC_OK);
    return h3s->stream->stream_data_in.next_read_offset - before;
}

void
xqc_test_h3_recv_total_off_by_default(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *r1 = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r1);
    xqc_h3_stream_t *r2 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r2);
    xqc_h3_stream_t *r3 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r3);

    CU_ASSERT_EQUAL(conn->conn_settings.h3_recv_total_buf_max, 0);

    /* Neither limit set: nothing is paused however much the connection
     * holds. */
    total_feed_body(r1, 300 * TOTAL_KIB);
    total_feed_body(r2, 300 * TOTAL_KIB);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 600 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(r1));
    CU_ASSERT_FALSE(total_paused(r2));
    CU_ASSERT_EQUAL(total_read_step(r1, 8 * TOTAL_KIB), 8 * TOTAL_KIB + 5);

    /* Per-request limit only: a request below its own high-water keeps
     * reading, whatever its neighbours hold. */
    conn->conn_settings.h3_recv_body_buf_max = 512 * TOTAL_KIB;
    total_feed_body(r3, 400 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(r3));
    CU_ASSERT_EQUAL(total_read_step(r3, 8 * TOTAL_KIB), 8 * TOTAL_KIB + 5);
    CU_ASSERT_FALSE(total_paused(r3));

    recv_cap_drop_request(r3);
    recv_cap_drop_request(r2);
    recv_cap_teardown(r1, h3c, conn);
}

void
xqc_test_h3_recv_total_pauses_and_releases(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *r1 = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r1);
    xqc_h3_stream_t *r2 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r2);
    xqc_h3_stream_t *r3 = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(r3);

    /* Per-request 256 KiB (low-water 128 KiB), connection 2.5 times that:
     * three requests can reach the total with none at its own high-water. */
    conn->conn_settings.h3_recv_body_buf_max = 256 * TOTAL_KIB;
    conn->conn_settings.h3_recv_total_buf_max = 640 * TOTAL_KIB;

    total_feed_body(r1, 200 * TOTAL_KIB);
    total_feed_body(r2, 200 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(r1));
    CU_ASSERT_FALSE(total_paused(r2));

    /* The third request takes the connection to its total. It is the one
     * reading, and it holds more than the floor, so it pauses at once. */
    total_feed_body(r3, 240 * TOTAL_KIB);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 640 * TOTAL_KIB);
    CU_ASSERT_TRUE(total_paused(r3));

    /* The other two hold more than the floor as well: their next read step
     * reads nothing, pauses them and takes them off the read list, so the
     * peer is held at their stream windows too. */
    CU_ASSERT_EQUAL(total_read_step(r1, 8 * TOTAL_KIB), 0);
    CU_ASSERT_TRUE(total_paused(r1));
    CU_ASSERT_FALSE(r1->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    CU_ASSERT_EQUAL(total_read_step(r2, 8 * TOTAL_KIB), 0);
    CU_ASSERT_TRUE(total_paused(r2));
    CU_ASSERT_FALSE(r2->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 640 * TOTAL_KIB);

    /* Draining that leaves the connection at or above half its total (320
     * KiB) releases nobody, even a request below its own low-water. */
    total_drain(r3, 90 * TOTAL_KIB);    /* r3 150, total 550 */
    total_drain(r1, 130 * TOTAL_KIB);   /* r1  70, total 420 */
    total_drain(r2, 100 * TOTAL_KIB);   /* r2 100, total 320 */
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 320 * TOTAL_KIB);
    CU_ASSERT_TRUE(total_paused(r1));
    CU_ASSERT_TRUE(total_paused(r2));
    CU_ASSERT_TRUE(total_paused(r3));
    CU_ASSERT_FALSE(r1->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* Below half: the request whose drain took it there is released and
     * put back on the read list. */
    total_drain(r2, 30 * TOTAL_KIB);    /* r2  70, total 290 */
    CU_ASSERT_FALSE(total_paused(r2));
    CU_ASSERT_TRUE(r2->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* The others are released by their own next drain, and only once their
     * own low-water holds too: r3 still holds 150 KiB, above 128 KiB. */
    total_drain(r3, 1);
    CU_ASSERT_TRUE(total_paused(r3));
    total_drain(r1, 1);
    CU_ASSERT_FALSE(total_paused(r1));
    CU_ASSERT_TRUE(r1->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);
    total_drain(r3, 50 * TOTAL_KIB - 1);
    CU_ASSERT_FALSE(total_paused(r3));
    CU_ASSERT_TRUE(r3->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* Released, r1 reads the body the pause held back. */
    xqc_bool_t fin = XQC_FALSE;
    CU_ASSERT_EQUAL(xqc_h3_stream_process_data(r1->stream, r1, &fin), XQC_OK);
    CU_ASSERT_EQUAL(r1->stream->stream_data_in.next_read_offset, 8 * TOTAL_KIB + 5);
    CU_ASSERT_FALSE(total_paused(r1));

    /* Hysteresis: back between half and the total, nothing pauses again
     * until the total itself is reached. */
    total_feed_body(r1, 150 * TOTAL_KIB);
    total_feed_body(r2, 150 * TOTAL_KIB);
    CU_ASSERT_TRUE(h3c->h3_recv_buffered_bytes > 320 * TOTAL_KIB);
    CU_ASSERT_TRUE(h3c->h3_recv_buffered_bytes < 640 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(r1));
    CU_ASSERT_FALSE(total_paused(r2));
    total_feed_body(r3, 100 * TOTAL_KIB);
    CU_ASSERT_TRUE(h3c->h3_recv_buffered_bytes >= 640 * TOTAL_KIB);
    CU_ASSERT_TRUE(r3->h3r->body_buf_bytes < 256 * TOTAL_KIB);
    CU_ASSERT_TRUE(total_paused(r3));

    recv_cap_drop_request(r3);
    recv_cap_drop_request(r2);
    recv_cap_teardown(r1, h3c, conn);
}

void
xqc_test_h3_recv_total_spares_requests_under_floor(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    xqc_h3_stream_t *big = recv_cap_setup(&conn, &h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(big);
    xqc_h3_stream_t *small = recv_cap_add_request(conn, h3c);
    CU_ASSERT_PTR_NOT_NULL_FATAL(small);

    /* Connection limit only. One stalled transfer takes the connection over
     * its total and stays there. */
    conn->conn_settings.h3_recv_total_buf_max = 128 * TOTAL_KIB;
    total_feed_body(big, 200 * TOTAL_KIB);
    CU_ASSERT_TRUE(total_paused(big));

    /* A small request keeps reading up to the floor, inclusive. */
    total_feed_body(small, XQC_H3_RECV_TOTAL_FLOOR - 8 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(small));
    CU_ASSERT_EQUAL(total_read_step(small, 8 * TOTAL_KIB - 5), 8 * TOTAL_KIB);
    CU_ASSERT_EQUAL(small->h3r->body_buf_bytes, XQC_H3_RECV_TOTAL_FLOOR - 5);
    feed_data_frame(small, 5);
    CU_ASSERT_EQUAL(small->h3r->body_buf_bytes, XQC_H3_RECV_TOTAL_FLOOR);
    CU_ASSERT_FALSE(total_paused(small));

    /* Past the floor it is paused like the others... */
    feed_data_frame(small, 1);
    CU_ASSERT_TRUE(total_paused(small));

    /* ...and released as soon as it is back at the floor, though the
     * connection is still over half its total. */
    xqc_stream_shutdown_read(small->stream);
    total_drain(small, 1);
    CU_ASSERT_TRUE(h3c->h3_recv_buffered_bytes >= 64 * TOTAL_KIB);
    CU_ASSERT_FALSE(total_paused(small));
    CU_ASSERT_TRUE(small->stream->stream_flag & XQC_STREAM_FLAG_READY_TO_READ);

    /* The large one stays held: it is above the floor and the connection
     * has not come down to half. */
    total_drain(big, 100 * TOTAL_KIB);
    CU_ASSERT_TRUE(total_paused(big));

    recv_cap_drop_request(small);
    recv_cap_teardown(big, h3c, conn);
}

void
xqc_test_h3_recv_total_pause_yields_to_frame_guard(void)
{
    xqc_connection_t *conn = NULL;
    xqc_h3_conn_t *h3c = NULL;
    frame_guard_run_t run;
    size_t chunk = 64;
    size_t frames = 10000;
    size_t total = frames * chunk;
    size_t body = total - FRAME_GUARD_HDR;

    /* No per-request limit; the connection limit is what pauses the
     * request, once it holds 80 KiB (past the floor). The application never
     * drains, so every frame after that stays in the transport queue, and
     * more than XQC_MAX_STREAM_FRAME_BUFFERED_COUNT of them are still to
     * come. The guard must override this pause exactly as it does the
     * per-request one, or the connection is closed. */
    xqc_h3_stream_t *h3s = frame_guard_setup(&conn, &h3c, 0, chunk, frames);
    xqc_stream_t *stream = h3s->stream;
    conn->conn_settings.h3_recv_total_buf_max = 80 * TOTAL_KIB;
    CU_ASSERT_TRUE(frames - 80 * TOTAL_KIB / chunk
                   > XQC_MAX_STREAM_FRAME_BUFFERED_COUNT);
    frame_guard_feed(conn, h3s, &run);

    CU_ASSERT_EQUAL(run.failed_at, -1);
    CU_ASSERT_EQUAL(run.failed_ret, XQC_OK);
    CU_ASSERT_EQUAL(conn->conn_err, 0);
    CU_ASSERT_FALSE(conn->conn_flag & XQC_CONN_FLAG_ERROR);
    CU_ASSERT_TRUE(run.max_frames_before_read <= FRAME_GUARD_SPARSE + FRAME_GUARD_BATCH);
    CU_ASSERT_TRUE(run.max_frames_after_read <= FRAME_GUARD_SPARSE);
    CU_ASSERT_EQUAL(run.reads_while_paused_below_guard, 0);
    CU_ASSERT_TRUE(run.reads_while_paused_over_guard > 0);

    CU_ASSERT_TRUE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_TRUE(total - stream->stream_data_in.next_read_offset
                   <= FRAME_GUARD_SPARSE * chunk);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes,
                    stream->stream_data_in.next_read_offset - FRAME_GUARD_HDR);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, h3s->h3r->body_buf_bytes);

    /* The application drains: the pause releases and the body is delivered
     * whole. */
    unsigned char out[XQC_DATA_BUF_SIZE_4K];
    size_t drained = 0;
    int rounds = 0;
    while (drained < body && rounds++ < 10000) {
        uint8_t fin = 0;
        ssize_t n;
        while ((n = xqc_h3_request_recv_body(h3s->h3r, out, sizeof(out), &fin)) > 0) {
            drained += (size_t)n;
        }
        CU_ASSERT_EQUAL_FATAL(n, -XQC_EAGAIN);
        xqc_process_read_streams(conn);
    }
    CU_ASSERT_EQUAL(drained, body);
    CU_ASSERT_EQUAL(h3s->h3r->body_buf_bytes, 0);
    CU_ASSERT_EQUAL(h3c->h3_recv_buffered_bytes, 0);
    CU_ASSERT_EQUAL(stream->stream_data_in.next_read_offset, total);
    CU_ASSERT_FALSE(h3s->flags & XQC_HTTP3_STREAM_FLAG_RECV_PAUSED);
    CU_ASSERT_EQUAL(conn->conn_err, 0);

    recv_cap_teardown(h3s, h3c, conn);
}
