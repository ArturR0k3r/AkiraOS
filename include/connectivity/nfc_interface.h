/**
 * @file nfc_interface.h
 * @brief Hardware-agnostic NFC tag abstraction for AkiraOS
 *
 * Mirrors the Radio Abstraction Layer (radio_interface.h) pattern: NFC tag
 * drivers register a vtable + handle with the manager at boot, and callers
 * (shell, WASM apps) go through the manager instead of a chip-specific API.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRA_NFC_INTERFACE_H
#define AKIRA_NFC_INTERFACE_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NFC_TYPE_NONE = 0,
    NFC_TYPE_ST25DV,
    NFC_TYPE_MAX
} nfc_type_t;

/* Forward declaration */
struct nfc_handle;

/* NFC operations interface (vtable pattern) */
typedef struct {
    /* Read the tag's unique identifier into an 8-byte buffer */
    int (*read_uid)(struct nfc_handle *handle, uint8_t uid[8]);

    /* Read len bytes of user memory starting at addr */
    int (*read_mem)(struct nfc_handle *handle, uint16_t addr, uint8_t *buf, size_t len);

    /* Write len bytes of user memory starting at addr */
    int (*write_mem)(struct nfc_handle *handle, uint16_t addr, const uint8_t *buf, size_t len);

    /* Report whether an RF field is currently present */
    int (*field_present)(struct nfc_handle *handle, bool *present);

    /* Enable/disable FTM mailbox. NULL if chip doesn't support FTM. */
    int (*mb_enable)(struct nfc_handle *handle, bool enable, uint8_t wdg);

    /* Put a message in the FTM mailbox for an RF reader to read. */
    int (*mb_put_msg)(struct nfc_handle *handle, const uint8_t *buf, size_t len);

    /* Get the message an RF reader put in the FTM mailbox. */
    int (*mb_get_msg)(struct nfc_handle *handle, uint8_t *buf, size_t cap, size_t *len_out);

    /* Query FTM mailbox control/status bits and current message length. */
    int (*mb_status)(struct nfc_handle *handle, uint8_t *ctrl, size_t *msg_len);
} nfc_ops_t;

/* NFC handle structure */
typedef struct nfc_handle {
    nfc_type_t type;         /* Tag chip type */
    const char *name;        /* Human-readable name, e.g. "ST25DV" */
    const nfc_ops_t *ops;    /* Operations vtable */
    void *priv_data;         /* Private driver data */
    struct k_mutex lock;     /* Handle lock for thread safety */
} nfc_handle_t;

/**
 * @brief Initialize the NFC manager subsystem.
 * @return 0 on success, negative errno on failure
 */
int nfc_manager_init(void);

/**
 * @brief Register an NFC tag driver with the manager.
 * @param handle Initialized NFC handle
 * @return 0 on success, negative errno on failure
 */
int nfc_manager_register(nfc_handle_t *handle);

/**
 * @brief Unregister an NFC tag driver from the manager.
 * @param handle NFC handle to unregister
 * @return 0 on success, negative errno on failure
 */
int nfc_manager_unregister(nfc_handle_t *handle);

/**
 * @brief Get the first registered NFC handle of the given type.
 * @param type Tag type to look up, or NFC_TYPE_NONE for "any"
 * @return Handle or NULL if none registered
 */
nfc_handle_t *nfc_manager_get(nfc_type_t type);

/**
 * @brief Get an NFC handle by driver name (e.g. "ST25DV").
 * @param name Driver name string
 * @return Handle or NULL if not found
 */
nfc_handle_t *nfc_manager_get_by_name(const char *name);

/* Convenience wrappers — thread-safe, NULL/ENOSYS-safe */

static inline int nfc_read_uid(nfc_handle_t *h, uint8_t uid[8])
{
    if (!h || !h->ops || !h->ops->read_uid) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->read_uid(h, uid);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_read_mem(nfc_handle_t *h, uint16_t addr, uint8_t *buf, size_t len)
{
    if (!h || !h->ops || !h->ops->read_mem) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->read_mem(h, addr, buf, len);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_write_mem(nfc_handle_t *h, uint16_t addr, const uint8_t *buf, size_t len)
{
    if (!h || !h->ops || !h->ops->write_mem) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->write_mem(h, addr, buf, len);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_field_present(nfc_handle_t *h, bool *present)
{
    if (!h || !h->ops || !h->ops->field_present) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->field_present(h, present);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_mailbox_enable(nfc_handle_t *h, bool enable, uint8_t wdg)
{
    if (!h || !h->ops || !h->ops->mb_enable) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->mb_enable(h, enable, wdg);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_mailbox_put_msg(nfc_handle_t *h, const uint8_t *buf, size_t len)
{
    if (!h || !h->ops || !h->ops->mb_put_msg) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->mb_put_msg(h, buf, len);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_mailbox_get_msg(nfc_handle_t *h, uint8_t *buf, size_t cap, size_t *len_out)
{
    if (!h || !h->ops || !h->ops->mb_get_msg) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->mb_get_msg(h, buf, cap, len_out);
    k_mutex_unlock(&h->lock);
    return ret;
}

static inline int nfc_mailbox_status(nfc_handle_t *h, uint8_t *ctrl, size_t *msg_len)
{
    if (!h || !h->ops || !h->ops->mb_status) {
        return -ENOSYS;
    }
    k_mutex_lock(&h->lock, K_FOREVER);
    int ret = h->ops->mb_status(h, ctrl, msg_len);
    k_mutex_unlock(&h->lock);
    return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NFC_INTERFACE_H */
