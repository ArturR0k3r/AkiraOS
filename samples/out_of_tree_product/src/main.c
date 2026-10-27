/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Product firmware entry point: run product-specific setup, then hand over to
 * the standard AkiraOS boot sequence.
 */

#include <zephyr/logging/log.h>
#include <akira.h>

LOG_MODULE_REGISTER(product_main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Product firmware starting on AkiraOS %s", AKIRA_VERSION_STRING);

    /* Product-specific initialisation (sensors, board bring-up) goes here. */

    return akira_start();
}
