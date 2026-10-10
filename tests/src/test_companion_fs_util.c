/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <zephyr/ztest.h>
#include <string.h>
#include "bluetooth/companion_fs_util.h"

ZTEST_SUITE(companion_fs_util, NULL, NULL, NULL, NULL, NULL);

ZTEST(companion_fs_util, test_path_accepts_sd)
{
    zassert_true(comp_fs_path_ok("/SD:"));
    zassert_true(comp_fs_path_ok("/SD:/"));
    zassert_true(comp_fs_path_ok("/SD:/apps/a.wasm"));
    zassert_true(comp_fs_path_ok("/SD:/a..b"));
    zassert_true(comp_fs_path_ok("/SD:/.../x"));
}

ZTEST(companion_fs_util, test_path_rejects)
{
    zassert_false(comp_fs_path_ok(NULL));
    zassert_false(comp_fs_path_ok(""));
    zassert_false(comp_fs_path_ok("/lfs/x"));
    zassert_false(comp_fs_path_ok("/SD:x"));
    zassert_false(comp_fs_path_ok("/SDX:/x"));
    zassert_false(comp_fs_path_ok("/SD:/.."));
    zassert_false(comp_fs_path_ok("/SD:/a/../b"));
    zassert_false(comp_fs_path_ok("/SD:/a/.."));
    zassert_false(comp_fs_path_ok("/SD:/a\"b"));
    zassert_false(comp_fs_path_ok("/SD:/a\\b"));
}

ZTEST(companion_fs_util, test_path_length_limit)
{
    char p[100];
    memset(p, 'a', sizeof(p));
    memcpy(p, "/SD:/", 5);
    p[COMP_FS_PATH_MAX - 1] = '\0';   /* 79 chars: ok */
    zassert_true(comp_fs_path_ok(p));
    p[COMP_FS_PATH_MAX - 1] = 'a';
    p[COMP_FS_PATH_MAX] = '\0';       /* 80 chars: too long */
    zassert_false(comp_fs_path_ok(p));
}

ZTEST(companion_fs_util, test_mutable_excludes_root)
{
    zassert_false(comp_fs_path_mutable_ok("/SD:"));
    zassert_false(comp_fs_path_mutable_ok("/SD:/"));
    zassert_true(comp_fs_path_mutable_ok("/SD:/x"));
    zassert_false(comp_fs_path_mutable_ok("/lfs/x"));
}

ZTEST(companion_fs_util, test_tmp_path)
{
    char out[COMP_FS_PATH_MAX];
    zassert_equal(comp_fs_tmp_path("/SD:/a.txt", out, sizeof(out)), 0);
    zassert_str_equal(out, "/SD:/a.txt.tmp");
    char small[8];
    zassert_equal(comp_fs_tmp_path("/SD:/a.txt", small, sizeof(small)), -ENAMETOOLONG);
}

ZTEST(companion_fs_util, test_fmt_entry)
{
    char out[96];
    int n = comp_fs_fmt_entry(out, sizeof(out), "a.txt", false, 12);
    zassert_equal(n, (int)strlen(out));
    zassert_str_equal(out, "{\"name\":\"a.txt\",\"type\":\"file\",\"size\":12}");
    zassert_true(comp_fs_fmt_entry(out, 10, "a.txt", false, 12) < 0);
    n = comp_fs_fmt_entry(out, sizeof(out), "d", true, 0);
    zassert_str_equal(out, "{\"name\":\"d\",\"type\":\"dir\",\"size\":0}");
}
