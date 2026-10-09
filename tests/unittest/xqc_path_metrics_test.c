/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#include <CUnit/CUnit.h>
#include <string.h>
#include "xquic/xquic.h"
#include "src/common/xqc_malloc.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/congestion_control/xqc_sample.h"
#include "src/congestion_control/xqc_bbr.h"
#include "src/congestion_control/xqc_bbr2.h"
#include "src/congestion_control/xqc_cubic.h"
#include "xqc_common_test.h"
#include "xqc_path_metrics_test.h"

extern void xqc_conn_get_stats_internal(xqc_connection_t *conn,
                                        xqc_conn_stats_t *conn_stats);

/* The path_cc_mode the single active path of conn reports. */
static int
path_cc_mode(xqc_connection_t *conn)
{
    xqc_conn_stats_t stats;
    int mode = -1;
    memset(&stats, 0, sizeof(stats));
    xqc_conn_get_stats_internal(conn, &stats);
    CU_ASSERT_EQUAL(stats.paths_info_count, 1);
    if (stats.paths_info_count == 1) {
        mode = stats.paths_info[0].path_cc_mode;
    }
    xqc_free(stats.paths_info);
    return mode;
}

void
xqc_test_path_metrics_cc_mode(void)
{
    xqc_connection_t *conn = test_engine_connect();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);
    xqc_path_ctx_t *path = conn->conn_initial_path;
    CU_ASSERT_PTR_NOT_NULL_FATAL(path);
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    const xqc_cong_ctrl_callback_t *saved_cb = ctl->ctl_cong_callback;
    void *saved_cong = ctl->ctl_cong;
    xqc_path_state_t saved_state = path->path_state;
    path->path_state = XQC_PATH_STATE_ACTIVE;

    xqc_cc_params_t params;
    memset(&params, 0, sizeof(params));
    xqc_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.srtt = 20000;
    sample.send_ctl = ctl;

    /* BBRv2: each mode as it is. */
    xqc_bbr2_t bbr2;
    xqc_bbr2_cb.xqc_cong_ctl_init_bbr(&bbr2, &sample, params);
    ctl->ctl_cong_callback = &xqc_bbr2_cb;
    ctl->ctl_cong = &bbr2;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_STARTUP);
    bbr2.mode = BBR2_DRAIN;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_DRAIN);
    bbr2.mode = BBR2_PROBE_BW;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_PROBE_BW);
    bbr2.mode = BBR2_PROBE_RTT;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_PROBE_RTT);

    /* BBR has the same modes. */
    xqc_bbr_t bbr;
    xqc_bbr_cb.xqc_cong_ctl_init_bbr(&bbr, &sample, params);
    ctl->ctl_cong_callback = &xqc_bbr_cb;
    ctl->ctl_cong = &bbr;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_STARTUP);
    bbr.mode = BBR_PROBE_BW;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_PROBE_BW);

    /* A controller without BBR modes reports none. */
    void *cubic = xqc_calloc(1, xqc_cubic_cb.xqc_cong_ctl_size());
    CU_ASSERT_PTR_NOT_NULL_FATAL(cubic);
    xqc_cubic_cb.xqc_cong_ctl_init(cubic, ctl, params);
    ctl->ctl_cong_callback = &xqc_cubic_cb;
    ctl->ctl_cong = cubic;
    CU_ASSERT_EQUAL(path_cc_mode(conn), XQC_CC_MODE_NONE);

    ctl->ctl_cong_callback = saved_cb;
    ctl->ctl_cong = saved_cong;
    path->path_state = saved_state;
    xqc_free(cubic);
    xqc_engine_destroy(conn->engine);
}
