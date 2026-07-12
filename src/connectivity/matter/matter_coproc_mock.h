/**
 * @file matter_coproc_mock.h
 * @brief In-firmware mock Matter co-processor (CONFIG_AKIRA_MATTER_COPROC_MOCK)
 *
 * Answers the co-processor IPC frames locally so the AkiraOS-side Matter
 * accessory path can be exercised end-to-end on native_sim (or hardware)
 * without a real esp-matter co-processor. Development / CI only.
 *
 * The byte-stream hooks (matter_coproc_mock_rx_byte / _tx_byte) are consumed
 * by src/runtime/akira_matter_ipc.c which loops its transport through them.
 */

#ifndef AKIRA_MATTER_COPROC_MOCK_H
#define AKIRA_MATTER_COPROC_MOCK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Feed one byte written by AkiraOS toward the (mock) co-processor. */
void matter_coproc_mock_rx_byte(uint8_t b);

/**
 * Pop one byte the mock co-processor is sending back to AkiraOS.
 * @return 0 and writes *out if a byte is available, -1 if the queue is empty.
 */
int matter_coproc_mock_tx_byte(uint8_t *out);

/**
 * Inject an unsolicited inbound command targeting a local endpoint, as if a
 * Matter controller (Home Assistant, Google Home, ...) issued it. Useful for
 * tests and the `matter mock-cmd` shell helper.
 *
 * @param endpoint  Target endpoint ID.
 * @param cluster   Cluster ID (e.g. MATTER_CLUSTER_ONOFF).
 * @param cmd       Command ID (e.g. MATTER_CMD_ON).
 * @param val       Optional command payload (may be NULL).
 * @param len       Payload length (0 if none).
 */
void matter_coproc_mock_inject_command(uint8_t endpoint, uint32_t cluster,
                                       uint32_t cmd, const uint8_t *val,
                                       uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MATTER_COPROC_MOCK_H */
