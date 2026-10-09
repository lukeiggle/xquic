/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#ifndef _XQC_BBR2_GUARD_TEST_H
#define _XQC_BBR2_GUARD_TEST_H

/* Fast convergence alone: the srtt-increase advance of the bw_hi filter is
 * taken on any ACK, in PROBE_DOWN and in PROBE_CRUISE, app-limited or not. */
void xqc_test_bbr2_fc_advance_unguarded(void);

/* With XQC_BBR2_FLAG_FC_ADVANCE_GUARDED the advance needs a round start and
 * a sample that is not app-limited, and is taken once per probing cycle;
 * PROBE_DOWN and PROBE_CRUISE are covered separately. */
void xqc_test_bbr2_fc_advance_guarded_down(void);
void xqc_test_bbr2_fc_advance_guarded_cruise(void);

/* The once-per-cycle budget is restored when a new cycle starts in
 * PROBE_DOWN, and is not spent by a condition that finds nothing to promote. */
void xqc_test_bbr2_fc_advance_guarded_new_cycle(void);

#endif /* _XQC_BBR2_GUARD_TEST_H */
