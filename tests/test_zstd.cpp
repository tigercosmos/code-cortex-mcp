/*
 * test_zstd.c — Tests for zstd compression wrappers.
 */
#include "test_framework.h"

/* Use the real header instead of hand-rolled prototypes: the local copies had
 * drifted to int/int for lengths the wrappers now take as size_t, which is an
 * ABI mismatch even where the values are small. */
#include "zstd_store.h"

#include <limits.h>

TEST(zstd_roundtrip) {
    const char *data = "Hello, zstd compression roundtrip test!";
    int len = (int)strlen(data);

    size_t bound = cbm_zstd_compress_bound((size_t)len);
    ASSERT_GT((int)bound, 0);

    char *cbuf = (char *)malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int64_t clen = cbm_zstd_compress(data, len, cbuf, bound, 3);
    ASSERT_GT(clen, 0);

    char *dbuf = (char *)malloc(len);
    ASSERT_NOT_NULL(dbuf);

    int64_t dlen = cbm_zstd_decompress(cbuf, clen, dbuf, len);
    ASSERT_EQ(dlen, len);
    ASSERT_MEM_EQ(dbuf, data, len);

    free(cbuf);
    free(dbuf);
    PASS();
}

TEST(zstd_roundtrip_large) {
    int len = 100000;
    char *data = (char *)malloc(len);
    ASSERT_NOT_NULL(data);

    /* Repetitive data — should compress well */
    for (int i = 0; i < len; i++) {
        data[i] = "function_name_pattern_abcdef"[i % 28];
    }

    size_t bound = cbm_zstd_compress_bound((size_t)len);
    char *cbuf = (char *)malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int64_t clen = cbm_zstd_compress(data, len, cbuf, bound, 9);
    ASSERT_GT(clen, 0);
    /* Repetitive data should compress at least 2:1 */
    ASSERT_LT(clen, len / 2);

    char *dbuf = (char *)malloc(len);
    ASSERT_NOT_NULL(dbuf);

    int64_t dlen = cbm_zstd_decompress(cbuf, clen, dbuf, len);
    ASSERT_EQ(dlen, len);
    ASSERT_MEM_EQ(dbuf, data, len);

    free(data);
    free(cbuf);
    free(dbuf);
    PASS();
}

TEST(zstd_compress_levels) {
    const char *data = "test data for different compression levels";
    int len = (int)strlen(data);
    size_t bound = cbm_zstd_compress_bound((size_t)len);
    char *cbuf = (char *)malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    /* Both level 3 (fast) and level 9 (best) should produce valid output */
    int64_t clen3 = cbm_zstd_compress(data, len, cbuf, bound, 3);
    ASSERT_GT(clen3, 0);

    int64_t clen9 = cbm_zstd_compress(data, len, cbuf, bound, 9);
    ASSERT_GT(clen9, 0);

    free(cbuf);
    PASS();
}

TEST(zstd_decompress_too_small_output) {
    const char *data = "this is test data that will be compressed";
    int len = (int)strlen(data);
    size_t bound = cbm_zstd_compress_bound((size_t)len);
    char *cbuf = (char *)malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int64_t clen = cbm_zstd_compress(data, len, cbuf, bound, 3);
    ASSERT_GT(clen, 0);

    /* Try decompressing with too-small output buffer — should return 0 (error) */
    char small[4];
    int64_t dlen = cbm_zstd_decompress(cbuf, clen, small, 4);
    ASSERT_EQ(dlen, 0);

    free(cbuf);
    PASS();
}

TEST(zstd_bound_positive) {
    ASSERT_GT((int)cbm_zstd_compress_bound(1), 0);
    ASSERT_GT((int)cbm_zstd_compress_bound(100), 0);
    ASSERT_GT((int)cbm_zstd_compress_bound(1000000), 0);
    PASS();
}

/* Regression: the compression side took int for the source length and the
 * destination capacity while decompression already took size_t, so a >2 GiB
 * database wrapped through int and handed the encoder a negative length. With
 * the int parameter, an input just past INT_MAX truncated to INT_MIN, whose
 * size_t reinterpretation is above zstd's max input size — ZSTD_compressBound
 * then answers 0. size_t parameters give a real bound above INT_MAX. No 2 GiB
 * buffer is needed: the bound helper is pure arithmetic. */
TEST(zstd_compress_bound_past_int_max_does_not_wrap) {
    size_t huge = (size_t)INT_MAX + 1U;
    size_t bound = cbm_zstd_compress_bound(huge);
    ASSERT_GT(bound, (size_t)INT_MAX);
    /* And the compressed length is reported through a 64-bit return, like the
     * decompressing counterpart, so a >2 GiB result cannot come back negative. */
    static_assert(sizeof(cbm_zstd_compress(nullptr, 0, nullptr, 0, 1)) == sizeof(int64_t),
                  "cbm_zstd_compress must return int64_t");
    PASS();
}

SUITE(zstd) {
    RUN_TEST(zstd_roundtrip);
    RUN_TEST(zstd_roundtrip_large);
    RUN_TEST(zstd_compress_levels);
    RUN_TEST(zstd_decompress_too_small_output);
    RUN_TEST(zstd_bound_positive);
    RUN_TEST(zstd_compress_bound_past_int_max_does_not_wrap);
}
