/*
 * test_diagnostics.cpp — the scaling probe (src/foundation/profile.h).
 *
 * These pin the arithmetic and the checkpoint bookkeeping, deliberately WITHOUT
 * timing anything. A test that tried to prove the detector works by generating a
 * genuinely quadratic workload would be asserting on the scheduler, and would go
 * flaky on a loaded CI box — so the fit is a pure function and gets fed
 * synthetic points instead.
 */
#include "test_framework.h"
#include "../src/foundation/profile.h"

#include <stdlib.h>

TEST(scale_fit_k_recognises_linear_and_quadratic_growth) {
    /* 8x the items, 8x the time => k = 1 */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 8000, 8000) > 0.99);
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 8000, 8000) < 1.01);

    /* 8x the items, 64x the time => k = 2 */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 8000, 64000) > 1.99);
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 8000, 64000) < 2.01);

    /* n log n sits between, and must NOT be mistaken for quadratic. */
    double k_nlogn = cbm_scale_fit_k(1000, 1000, 8000, 8000 * 13 / 10);
    ASSERT_TRUE(k_nlogn > 1.0);
    ASSERT_TRUE(k_nlogn < CBM_SCALE_WARN_K);
    PASS();
}

TEST(scale_fit_k_rejects_degenerate_input) {
    ASSERT_TRUE(cbm_scale_fit_k(0, 1000, 8000, 8000) < 0.0);    /* no first point */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 0, 8000, 8000) < 0.0);    /* zero elapsed */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 1000, 8000) < 0.0); /* n did not grow */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 500, 8000) < 0.0);  /* n went backwards */
    ASSERT_TRUE(cbm_scale_fit_k(1000, 1000, 8000, 0) < 0.0);    /* zero total */
    PASS();
}

TEST(scale_probe_records_checkpoints_at_eighths) {
    cbm_scale_probe_t p;
    cbm_scale_begin(&p, "unit", 1024);

    /* Below the first threshold (1024/8 = 128), nothing is claimed. */
    for (long i = 0; i < 128; i++) {
        cbm_scale_tick(&p, i);
    }
    ASSERT_EQ(atomic_load(&p.next_cp), 0);

    cbm_scale_tick(&p, 128); /* 1/8 */
    ASSERT_EQ(atomic_load(&p.next_cp), 1);
    cbm_scale_tick(&p, 200); /* still short of 1/4 */
    ASSERT_EQ(atomic_load(&p.next_cp), 1);
    cbm_scale_tick(&p, 256); /* 1/4 */
    ASSERT_EQ(atomic_load(&p.next_cp), 2);
    cbm_scale_tick(&p, 512); /* 1/2 */
    ASSERT_EQ(atomic_load(&p.next_cp), 3);
    cbm_scale_tick(&p, 1024); /* all */
    ASSERT_EQ(atomic_load(&p.next_cp), 4);

    /* Saturates rather than overrunning the array. */
    cbm_scale_tick(&p, 2048);
    ASSERT_EQ(atomic_load(&p.next_cp), 4);
    ASSERT_EQ(p.cp_items[0], 128);
    ASSERT_EQ(p.cp_items[3], 1024);
    cbm_scale_end(&p);
    PASS();
}

TEST(scale_probe_ignores_runs_too_small_to_judge) {
    /* Under SCALE_MIN_ITEMS a pass is not where an O(n^2) hurts anyone, and the
     * fit would be measuring noise — so the probe must stay silent. */
    cbm_scale_probe_t p;
    cbm_scale_begin(&p, "tiny", 64);
    for (long i = 0; i <= 64; i++) {
        cbm_scale_tick(&p, i);
    }
    ASSERT_EQ(atomic_load(&p.next_cp), 0);
    cbm_scale_end(&p); /* must not emit, must not crash */
    PASS();
}

SUITE(diagnostics) {
    RUN_TEST(scale_fit_k_recognises_linear_and_quadratic_growth);
    RUN_TEST(scale_fit_k_rejects_degenerate_input);
    RUN_TEST(scale_probe_records_checkpoints_at_eighths);
    RUN_TEST(scale_probe_ignores_runs_too_small_to_judge);
}
