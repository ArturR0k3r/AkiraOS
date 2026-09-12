/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <zephyr/ztest.h>
#include <string.h>
#include "catalog_net.h"

ZTEST_SUITE(catalog_net, NULL, NULL, NULL, NULL, NULL);

ZTEST(catalog_net, test_init_sets_capacity)
{
    uint8_t buf[64];
    catalog_ring_init(buf, sizeof(buf));

    uint32_t write_idx, read_idx, capacity, flags;
    memcpy(&write_idx, buf + 0, 4);
    memcpy(&read_idx, buf + 4, 4);
    memcpy(&capacity, buf + 8, 4);
    memcpy(&flags, buf + 12, 4);

    zassert_equal(write_idx, 0, "write_idx must start at 0");
    zassert_equal(read_idx, 0, "read_idx must start at 0");
    zassert_equal(capacity, sizeof(buf) - CATALOG_NET_RING_HDR_SIZE, "capacity is buffer size minus header");
    zassert_equal(flags, 0, "flags must be 0");
}

ZTEST(catalog_net, test_write_then_read_roundtrip)
{
    uint8_t buf[64];
    catalog_ring_init(buf, sizeof(buf));

    const uint8_t msg[] = "GET /catalogue.json";
    int ret = catalog_ring_write(buf, msg, sizeof(msg));
    zassert_equal(ret, 0, "write should succeed");

    uint8_t out[64];
    int n = catalog_ring_read(buf, out, sizeof(out));
    zassert_equal(n, sizeof(msg), "read should return the written length");
    zassert_mem_equal(out, msg, sizeof(msg), "read payload must match written payload");
}

ZTEST(catalog_net, test_read_empty_ring_returns_enodata)
{
    uint8_t buf[64];
    catalog_ring_init(buf, sizeof(buf));

    uint8_t out[64];
    int n = catalog_ring_read(buf, out, sizeof(out));
    zassert_equal(n, -ENODATA, "reading an empty ring must return -ENODATA");
}

ZTEST(catalog_net, test_write_too_large_returns_enospc)
{
    uint8_t buf[32]; /* capacity = 32 - 16 = 16 bytes of data area */
    catalog_ring_init(buf, sizeof(buf));

    uint8_t msg[20]; /* larger than the 16-byte data area even before framing overhead */
    memset(msg, 'A', sizeof(msg));
    int ret = catalog_ring_write(buf, msg, sizeof(msg));
    zassert_equal(ret, -ENOSPC, "oversized write must return -ENOSPC");
}

ZTEST(catalog_net, test_wraparound_roundtrip)
{
    uint8_t buf[32]; /* small ring to force wraparound: 32 - 16 = 16-byte data area */
    catalog_ring_init(buf, sizeof(buf));

    const uint8_t msg1[] = "abc";  /* 2-byte len prefix + 4 bytes = 6 bytes */
    const uint8_t msg2[] = "de";   /* 2-byte len prefix + 3 bytes = 5 bytes */

    zassert_equal(catalog_ring_write(buf, msg1, sizeof(msg1)), 0, "first write");
    uint8_t out[16];
    zassert_equal(catalog_ring_read(buf, out, sizeof(out)), sizeof(msg1), "first read");

    /* Second write should wrap past the end of the 16-byte data area */
    zassert_equal(catalog_ring_write(buf, msg2, sizeof(msg2)), 0, "second write (wraps)");
    int n = catalog_ring_read(buf, out, sizeof(out));
    zassert_equal(n, sizeof(msg2), "second read after wraparound");
    zassert_mem_equal(out, msg2, sizeof(msg2), "wrapped payload must match");
}
