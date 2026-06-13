/* akira_platform/audit_hmac.h — bench stub for HMAC-SHA256 audit signing */
#pragma once
#include <stdint.h>

/* Sign an audit entry with HMAC-SHA256.
 * tag_out must point to a 32-byte buffer.
 * Returns 0 on success, negative on error.
 */
int akira_platform_audit_hmac_sign(uint32_t event_type,
                                   uint32_t timestamp_ms,
                                   const char *app_name,
                                   uint32_t detail,
                                   uint8_t tag_out[32]);
