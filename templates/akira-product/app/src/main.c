/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Product firmware entry point. Do product-specific setup, then hand over to
 * the AkiraOS boot sequence. The widget subsystem registers itself through
 * hooks/native/capability tables, so main() has nothing to wire up here.
 */

#include <zephyr/logging/log.h>
#include <akira.h>

LOG_MODULE_REGISTER(product_main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Widget product on AkiraOS %s", AKIRA_VERSION_STRING);
    return akira_start();
}
