/**
 * @copyright Copyright (c) 2026, mp0rta
 */

#ifndef _XQC_BBR2_LOSS_ROUND_TEST_H
#define _XQC_BBR2_LOSS_ROUND_TEST_H

/* Past 4 GiB delivered on a path, the loss round still ends once per round
 * trip, and the lower bounds respond to loss once per round, not per ACK. */
void xqc_test_bbr2_loss_round_past_4gib(void);

#endif /* _XQC_BBR2_LOSS_ROUND_TEST_H */
