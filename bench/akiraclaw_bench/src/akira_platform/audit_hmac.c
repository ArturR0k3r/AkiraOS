/* audit_hmac.c — minimal HMAC-SHA256 audit signing for akiraclaw_bench */

#ifdef CONFIG_AKIRA_AUDIT_LOG_HMAC

#include "audit_hmac.h"
#include <string.h>
#include <zephyr/kernel.h>

#ifdef CONFIG_MBEDTLS
#include <mbedtls/md.h>

/* Fixed 32-byte key for bench measurements (not secret; bench only) */
static const uint8_t k_bench_key[32] = {
    0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

int akira_platform_audit_hmac_sign(uint32_t event_type,
                                   uint32_t timestamp_ms,
                                   const char *app_name,
                                   uint32_t detail,
                                   uint8_t tag_out[32])
{
    uint8_t msg[16];
    memcpy(msg + 0, &event_type,    4);
    memcpy(msg + 4, &timestamp_ms,  4);
    memcpy(msg + 8, &detail,        4);
    uint32_t name_hash = 0;
    if (app_name) {
        for (const char *p = app_name; *p; p++)
            name_hash = name_hash * 31u + (uint8_t)*p;
    }
    memcpy(msg + 12, &name_hash, 4);

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (rc != 0) {
        mbedtls_md_free(&ctx);
        return -1;
    }
    rc = mbedtls_md_hmac_starts(&ctx, k_bench_key, sizeof(k_bench_key));
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, msg, sizeof(msg));
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, tag_out);
    mbedtls_md_free(&ctx);
    return (rc == 0) ? 0 : -1;
}

#else /* !CONFIG_MBEDTLS */

int akira_platform_audit_hmac_sign(uint32_t event_type,
                                   uint32_t timestamp_ms,
                                   const char *app_name,
                                   uint32_t detail,
                                   uint8_t tag_out[32])
{
    (void)event_type; (void)timestamp_ms; (void)app_name; (void)detail;
    memset(tag_out, 0, 32);
    return -1;
}

#endif /* CONFIG_MBEDTLS */

#endif /* CONFIG_AKIRA_AUDIT_LOG_HMAC */
