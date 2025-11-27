/**
 * @file cloud_app_handler.c
 * @brief Cloud App Handler Implementation
 *
 * Handles app downloads and integrates with app_loader/wasm_app_manager.
 */

#include "cloud_app_handler.h"
#include "cloud_client.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

/* Include app management */
#include "apps/app_loader.h"
#include "services/ocre_runtime.h"

/* Forward declarations for wasm_app_manager functions */
extern int wasm_app_upload(const char *name, const void *binary, size_t size, uint32_t version);
extern int wasm_app_start(const char *name);

LOG_MODULE_REGISTER(cloud_app, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Private Types                                                             */
/*===========================================================================*/

struct download_context {
    bool active;
    app_download_state_t state;
    char app_id[32];
    char name[32];
    uint8_t version[4];
    uint32_t total_size;
    uint32_t received;
    uint16_t expected_chunks;
    uint16_t received_chunks;
    uint8_t hash[32];
    msg_source_t source;
    
    /* Buffer for app data */
    uint8_t *buffer;
    size_t buffer_size;
    
    /* Callbacks */
    app_download_progress_cb_t progress_cb;
    app_download_complete_cb_t complete_cb;
    void *user_data;
    
    /* Options */
    bool auto_install;
    bool auto_start;
};

struct catalog_request {
    bool pending;
    app_catalog_cb_t callback;
    void *user_data;
};

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static struct {
    bool initialized;
    struct download_context downloads[APP_MAX_PENDING_DOWNLOADS];
    struct catalog_request catalog_req;
    struct k_mutex mutex;
} handler;

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static struct download_context *find_download(const char *app_id)
{
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++) {
        if (handler.downloads[i].active &&
            strcmp(handler.downloads[i].app_id, app_id) == 0) {
            return &handler.downloads[i];
        }
    }
    return NULL;
}

static struct download_context *find_free_download(void)
{
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++) {
        if (!handler.downloads[i].active) {
            return &handler.downloads[i];
        }
    }
    return NULL;
}

static void complete_download(struct download_context *ctx, bool success, const char *error)
{
    LOG_INF("Download %s: %s%s%s",
            ctx->app_id,
            success ? "SUCCESS" : "FAILED",
            error ? " - " : "",
            error ? error : "");
    
    ctx->state = success ? APP_DL_COMPLETE : APP_DL_ERROR;
    
    if (success && ctx->auto_install) {
        LOG_INF("Auto-installing app: %s", ctx->app_id);
        
        /* Use wasm_app_manager to install */
        int ret = wasm_app_upload(ctx->name, ctx->buffer, ctx->received,
                                  (ctx->version[0] << 24) | (ctx->version[1] << 16) |
                                  (ctx->version[2] << 8) | ctx->version[3]);
        
        if (ret >= 0) {
            LOG_INF("App installed: %s (container %d)", ctx->app_id, ret);
            
            if (ctx->auto_start) {
                LOG_INF("Auto-starting app: %s", ctx->name);
                wasm_app_start(ctx->name);
            }
        } else {
            LOG_ERR("Failed to install app: %d", ret);
            success = false;
            error = "Installation failed";
        }
    }
    
    /* Notify caller */
    if (ctx->complete_cb) {
        ctx->complete_cb(ctx->app_id, success, error, ctx->user_data);
    }
    
    /* Cleanup */
    if (ctx->buffer) {
        k_free(ctx->buffer);
        ctx->buffer = NULL;
    }
    ctx->active = false;
}

static int handle_app_metadata(const cloud_message_t *msg, msg_source_t source)
{
    if (msg->header.payload_len < sizeof(payload_app_metadata_t)) {
        LOG_ERR("Invalid metadata payload");
        return -EINVAL;
    }
    
    payload_app_metadata_t *meta = (payload_app_metadata_t *)msg->payload;
    
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    /* Find or create download context */
    struct download_context *ctx = find_download(meta->app_id);
    if (!ctx) {
        ctx = find_free_download();
        if (!ctx) {
            k_mutex_unlock(&handler.mutex);
            LOG_ERR("No free download slots");
            return -ENOMEM;
        }
        ctx->active = true;
        ctx->auto_install = true;  /* Default */
        ctx->auto_start = false;
    }
    
    /* Store metadata */
    strncpy(ctx->app_id, meta->app_id, sizeof(ctx->app_id) - 1);
    strncpy(ctx->name, meta->name, sizeof(ctx->name) - 1);
    memcpy(ctx->version, meta->version, 4);
    ctx->total_size = meta->size;
    ctx->expected_chunks = meta->chunk_count;
    memcpy(ctx->hash, meta->hash, 32);
    ctx->source = source;
    ctx->received = 0;
    ctx->received_chunks = 0;
    ctx->state = APP_DL_METADATA;
    
    /* Allocate buffer */
    if (ctx->buffer) {
        k_free(ctx->buffer);
    }
    ctx->buffer = k_malloc(meta->size);
    if (!ctx->buffer) {
        k_mutex_unlock(&handler.mutex);
        LOG_ERR("Failed to allocate %u bytes for app", meta->size);
        ctx->active = false;
        return -ENOMEM;
    }
    ctx->buffer_size = meta->size;
    
    LOG_INF("App download started: %s v%d.%d.%d (%u bytes, %u chunks)",
            meta->name, meta->version[0], meta->version[1], meta->version[2],
            meta->size, meta->chunk_count);
    
    ctx->state = APP_DL_RECEIVING;
    
    k_mutex_unlock(&handler.mutex);
    return 0;
}

static int handle_app_chunk(const cloud_message_t *msg, msg_source_t source)
{
    if (msg->header.payload_len < sizeof(payload_chunk_t)) {
        LOG_ERR("Invalid chunk payload");
        return -EINVAL;
    }
    
    payload_chunk_t *chunk = (payload_chunk_t *)msg->payload;
    size_t data_len = msg->header.payload_len - offsetof(payload_chunk_t, data);
    
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    /* Find active download - match by source for now */
    struct download_context *ctx = NULL;
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++) {
        if (handler.downloads[i].active &&
            handler.downloads[i].state == APP_DL_RECEIVING &&
            handler.downloads[i].source == source) {
            ctx = &handler.downloads[i];
            break;
        }
    }
    
    if (!ctx) {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("No active download for chunk");
        return -ENOENT;
    }
    
    /* Validate chunk */
    if (chunk->offset + data_len > ctx->buffer_size) {
        k_mutex_unlock(&handler.mutex);
        LOG_ERR("Chunk exceeds buffer: offset=%u, len=%zu, size=%zu",
                chunk->offset, data_len, ctx->buffer_size);
        return -EOVERFLOW;
    }
    
    /* Store chunk data */
    memcpy(ctx->buffer + chunk->offset, chunk->data, data_len);
    ctx->received += data_len;
    ctx->received_chunks++;
    
    LOG_DBG("Chunk %u/%u: offset=%u, len=%zu, total=%u/%u",
            ctx->received_chunks, ctx->expected_chunks,
            chunk->offset, data_len,
            ctx->received, ctx->total_size);
    
    /* Progress callback */
    if (ctx->progress_cb) {
        ctx->progress_cb(ctx->app_id, ctx->received, ctx->total_size, ctx->user_data);
    }
    
    k_mutex_unlock(&handler.mutex);
    return 0;
}

static int handle_app_complete(const cloud_message_t *msg, msg_source_t source)
{
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    /* Find download by source */
    struct download_context *ctx = NULL;
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++) {
        if (handler.downloads[i].active &&
            handler.downloads[i].source == source) {
            ctx = &handler.downloads[i];
            break;
        }
    }
    
    if (!ctx) {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("No active download to complete");
        return -ENOENT;
    }
    
    /* Verify we got all data */
    if (ctx->received < ctx->total_size) {
        LOG_WRN("Incomplete download: %u/%u bytes",
                ctx->received, ctx->total_size);
    }
    
    ctx->state = APP_DL_VERIFYING;
    
    /* TODO: Verify hash */
    
    ctx->state = APP_DL_INSTALLING;
    complete_download(ctx, true, NULL);
    
    k_mutex_unlock(&handler.mutex);
    return 0;
}

static int handle_app_list_response(const cloud_message_t *msg, msg_source_t source)
{
    if (!handler.catalog_req.pending || !handler.catalog_req.callback) {
        LOG_DBG("No pending catalog request");
        return 0;
    }
    
    /* Parse catalog entries */
    if (msg->header.payload_len < sizeof(uint16_t)) {
        handler.catalog_req.callback(NULL, 0, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return 0;
    }
    
    uint16_t count = *(uint16_t *)msg->payload;
    size_t expected_size = sizeof(uint16_t) + count * sizeof(payload_app_entry_t);
    
    if (msg->header.payload_len < expected_size) {
        LOG_ERR("Invalid catalog size");
        handler.catalog_req.callback(NULL, -1, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return -EINVAL;
    }
    
    /* Convert to catalog entries */
    payload_app_entry_t *entries = (payload_app_entry_t *)(msg->payload + sizeof(uint16_t));
    
    /* Allocate result array */
    app_catalog_entry_t *catalog = k_malloc(count * sizeof(app_catalog_entry_t));
    if (!catalog && count > 0) {
        handler.catalog_req.callback(NULL, -1, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return -ENOMEM;
    }
    
    for (int i = 0; i < count; i++) {
        strncpy(catalog[i].app_id, entries[i].app_id, 31);
        strncpy(catalog[i].name, entries[i].name, 31);
        catalog[i].description[0] = '\0';  /* Not in payload */
        memcpy(catalog[i].version, entries[i].version, 4);
        catalog[i].size = 0;  /* Not in payload */
        catalog[i].permissions = 0;
        catalog[i].installed = entries[i].installed;
        catalog[i].has_update = entries[i].has_update;
    }
    
    handler.catalog_req.callback(catalog, count, handler.catalog_req.user_data);
    
    k_free(catalog);
    handler.catalog_req.pending = false;
    
    return 0;
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

int cloud_app_handler_init(void)
{
    if (handler.initialized) {
        return 0;
    }
    
    memset(&handler, 0, sizeof(handler));
    k_mutex_init(&handler.mutex);
    
    /* Register with cloud client */
    cloud_client_register_handler(MSG_CAT_APP, cloud_app_handle_message);
    
    handler.initialized = true;
    LOG_INF("Cloud app handler initialized");
    
    return 0;
}

int cloud_app_handler_deinit(void)
{
    if (!handler.initialized) {
        return 0;
    }
    
    /* Cancel all downloads */
    cloud_app_cancel_download(NULL);
    
    handler.initialized = false;
    LOG_INF("Cloud app handler deinitialized");
    
    return 0;
}

int cloud_app_download(const app_download_request_t *request)
{
    if (!handler.initialized || !request || !request->app_id[0]) {
        return -EINVAL;
    }
    
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    /* Check if already downloading */
    if (find_download(request->app_id)) {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("Already downloading: %s", request->app_id);
        return -EALREADY;
    }
    
    /* Find free slot */
    struct download_context *ctx = find_free_download();
    if (!ctx) {
        k_mutex_unlock(&handler.mutex);
        LOG_ERR("No free download slots");
        return -ENOMEM;
    }
    
    /* Initialize context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->active = true;
    ctx->state = APP_DL_IDLE;
    strncpy(ctx->app_id, request->app_id, sizeof(ctx->app_id) - 1);
    ctx->progress_cb = request->progress_cb;
    ctx->complete_cb = request->complete_cb;
    ctx->user_data = request->user_data;
    ctx->auto_install = request->auto_install;
    ctx->auto_start = request->auto_start;
    
    k_mutex_unlock(&handler.mutex);
    
    /* Request app from cloud */
    LOG_INF("Requesting app: %s", request->app_id);
    return cloud_client_request_app(request->app_id);
}

int cloud_app_cancel_download(const char *app_id)
{
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++) {
        struct download_context *ctx = &handler.downloads[i];
        if (!ctx->active) {
            continue;
        }
        
        if (app_id == NULL || strcmp(ctx->app_id, app_id) == 0) {
            LOG_INF("Cancelling download: %s", ctx->app_id);
            complete_download(ctx, false, "Cancelled");
        }
    }
    
    k_mutex_unlock(&handler.mutex);
    return 0;
}

app_download_state_t cloud_app_get_download_state(const char *app_id)
{
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    struct download_context *ctx = find_download(app_id);
    app_download_state_t state = ctx ? ctx->state : APP_DL_IDLE;
    
    k_mutex_unlock(&handler.mutex);
    return state;
}

int cloud_app_get_download_progress(const char *app_id,
                                     uint32_t *received,
                                     uint32_t *total)
{
    k_mutex_lock(&handler.mutex, K_FOREVER);
    
    struct download_context *ctx = find_download(app_id);
    if (!ctx) {
        k_mutex_unlock(&handler.mutex);
        return -ENOENT;
    }
    
    if (received) *received = ctx->received;
    if (total) *total = ctx->total_size;
    
    k_mutex_unlock(&handler.mutex);
    return 0;
}

int cloud_app_check_updates(void)
{
    return cloud_client_check_app_updates();
}

int cloud_app_update(const char *app_id,
                      app_download_complete_cb_t complete_cb,
                      void *user_data)
{
    app_download_request_t req = {
        .progress_cb = NULL,
        .complete_cb = complete_cb,
        .user_data = user_data,
        .auto_install = true,
        .auto_start = false
    };
    
    if (app_id) {
        strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);
        return cloud_app_download(&req);
    }
    
    /* Update all - would need to iterate installed apps */
    /* TODO: Implement bulk update */
    return -ENOTSUP;
}

int cloud_app_request_catalog(app_catalog_cb_t callback, void *user_data)
{
    if (!handler.initialized || !callback) {
        return -EINVAL;
    }
    
    handler.catalog_req.pending = true;
    handler.catalog_req.callback = callback;
    handler.catalog_req.user_data = user_data;
    
    return cloud_client_request_app_list();
}

int cloud_app_handle_message(const cloud_message_t *msg, msg_source_t source)
{
    if (!handler.initialized) {
        return -EINVAL;
    }
    
    switch (msg->header.type) {
    case MSG_TYPE_APP_METADATA:
        return handle_app_metadata(msg, source);
        
    case MSG_TYPE_APP_CHUNK:
        return handle_app_chunk(msg, source);
        
    case MSG_TYPE_APP_COMPLETE:
        return handle_app_complete(msg, source);
        
    case MSG_TYPE_APP_LIST_RESPONSE:
        return handle_app_list_response(msg, source);
        
    case MSG_TYPE_APP_AVAILABLE:
        LOG_INF("App available notification from %s", cloud_msg_source_str(source));
        return 0;
        
    default:
        return 0;  /* Not handled */
    }
}
