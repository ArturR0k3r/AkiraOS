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
#include "lib/mem_helper.h"
#include <string.h>

/* Include app management */
#include <runtime/app_loader/app_loader.h>
#include <runtime/app_manager/app_manager.h>
#include <runtime/app_manager/app_cmd_bridge.h>
#include <runtime/akira_runtime.h>

LOG_MODULE_REGISTER(cloud_app, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Private Types                                                             */
/*===========================================================================*/

struct download_context
{
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

struct catalog_request
{
    bool pending;
    app_catalog_cb_t callback;
    void *user_data;
};

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static struct
{
    bool initialized;
    struct download_context downloads[APP_MAX_PENDING_DOWNLOADS];
    struct catalog_request catalog_req;
    struct k_mutex mutex;
} handler;

static struct k_work_delayable s_update_check_work;

static void update_check_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

#if CONFIG_AKIRA_CLOUD_APP_UPDATE_CHECK_INTERVAL_MS > 0
    cloud_app_check_updates();
    k_work_schedule(&s_update_check_work,
                    K_MSEC(CONFIG_AKIRA_CLOUD_APP_UPDATE_CHECK_INTERVAL_MS));
#endif
}

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static struct download_context *find_download(const char *app_id)
{
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++)
    {
        if (handler.downloads[i].active &&
            strcmp(handler.downloads[i].app_id, app_id) == 0)
        {
            return &handler.downloads[i];
        }
    }
    return NULL;
}

static struct download_context *find_free_download(void)
{
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++)
    {
        if (!handler.downloads[i].active)
        {
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

    if (success && ctx->auto_install)
    {
        LOG_INF("Auto-installing app: %s", ctx->app_id);

        /* Create manifest */
        app_manifest_t manifest = {0};
        strncpy(manifest.name, ctx->name, APP_NAME_MAX_LEN - 1);
        snprintf(manifest.version, APP_VERSION_MAX_LEN, "%u.%u.%u.%u",
                 ctx->version[0], ctx->version[1], ctx->version[2], ctx->version[3]);

        /* Use app_manager to install */
        int id = app_loader_install_memory(ctx->name, ctx->buffer, ctx->received);

        if (id >= 0)
        {
            LOG_INF("App loaded into slot %d", id);

            if (ctx->auto_start)
            {
                LOG_INF("Auto-starting app slot: %d", id);
                akira_runtime_start(id);
            }
        }
        else
        {
            LOG_ERR("Failed to load app into runtime: %d", id);
            success = false;
            error = "Installation failed";
        }
    }

    /* Notify caller */
    if (ctx->complete_cb)
    {
        ctx->complete_cb(ctx->app_id, success, error, ctx->user_data);
    }

    /* Cleanup */
    if (ctx->buffer)
    {
        akira_free_buffer(ctx->buffer);
        ctx->buffer = NULL;
    }
    ctx->active = false;
}

static int handle_app_metadata(const cloud_message_t *msg, msg_source_t source)
{
    if (msg->header.payload_len < sizeof(payload_app_metadata_t))
    {
        LOG_ERR("Invalid metadata payload");
        return -EINVAL;
    }

    payload_app_metadata_t *meta = (payload_app_metadata_t *)msg->payload;

    k_mutex_lock(&handler.mutex, K_FOREVER);

    /* Find or create download context */
    struct download_context *ctx = find_download(meta->app_id);
    if (!ctx)
    {
        ctx = find_free_download();
        if (!ctx)
        {
            k_mutex_unlock(&handler.mutex);
            LOG_ERR("No free download slots");
            return -ENOMEM;
        }
        ctx->active = true;
        ctx->auto_install = true; /* Default */
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
    if (ctx->buffer)
    {
        akira_free_buffer(ctx->buffer);
        ctx->buffer = NULL;
    }
    ctx->buffer = akira_malloc_buffer(meta->size);
    if (!ctx->buffer)
    {
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
    if (msg->header.payload_len < sizeof(payload_chunk_t))
    {
        LOG_ERR("Invalid chunk payload");
        return -EINVAL;
    }

    payload_chunk_t *chunk = (payload_chunk_t *)msg->payload;
    size_t data_len = msg->header.payload_len - offsetof(payload_chunk_t, data);

    k_mutex_lock(&handler.mutex, K_FOREVER);

    /* Find active download - match by source for now */
    struct download_context *ctx = NULL;
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++)
    {
        if (handler.downloads[i].active &&
            handler.downloads[i].state == APP_DL_RECEIVING &&
            handler.downloads[i].source == source)
        {
            ctx = &handler.downloads[i];
            break;
        }
    }

    if (!ctx)
    {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("No active download for chunk");
        return -ENOENT;
    }

    /* Validate chunk — avoid overflow in offset + data_len addition */
    if (data_len > ctx->buffer_size ||
        (size_t)chunk->offset > ctx->buffer_size - data_len)
    {
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
    if (ctx->progress_cb)
    {
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
    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++)
    {
        if (handler.downloads[i].active &&
            handler.downloads[i].source == source)
        {
            ctx = &handler.downloads[i];
            break;
        }
    }

    if (!ctx)
    {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("No active download to complete");
        return -ENOENT;
    }

    /* Verify we got all data */
    if (ctx->received < ctx->total_size)
    {
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
    if (!handler.catalog_req.pending || !handler.catalog_req.callback)
    {
        LOG_DBG("No pending catalog request");
        return 0;
    }

    /* Parse catalog entries */
    if (msg->header.payload_len < sizeof(uint16_t))
    {
        handler.catalog_req.callback(NULL, 0, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return 0;
    }

    uint16_t count = *(uint16_t *)msg->payload;
    size_t expected_size = sizeof(uint16_t) + count * sizeof(payload_app_entry_t);

    if (msg->header.payload_len < expected_size)
    {
        LOG_ERR("Invalid catalog size");
        handler.catalog_req.callback(NULL, -1, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return -EINVAL;
    }

    /* Convert to catalog entries */
    payload_app_entry_t *entries = (payload_app_entry_t *)(msg->payload + sizeof(uint16_t));

    /* Allocate result array */
    app_catalog_entry_t *catalog = akira_malloc_buffer(count * sizeof(app_catalog_entry_t));
    if (!catalog && count > 0)
    {
        handler.catalog_req.callback(NULL, -1, handler.catalog_req.user_data);
        handler.catalog_req.pending = false;
        return -ENOMEM;
    }

    for (int i = 0; i < count; i++)
    {
        strncpy(catalog[i].app_id, entries[i].app_id, 31);
        strncpy(catalog[i].name, entries[i].name, 31);
        catalog[i].description[0] = '\0'; /* Not in payload */
        memcpy(catalog[i].version, entries[i].version, 4);
        catalog[i].size = 0; /* Not in payload */
        catalog[i].permissions = 0;
        catalog[i].installed = entries[i].installed;
        catalog[i].has_update = entries[i].has_update;
    }

    handler.catalog_req.callback(catalog, count, handler.catalog_req.user_data);

    akira_free_buffer(catalog);
    handler.catalog_req.pending = false;

    return 0;
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

int cloud_app_handler_init(void)
{
    if (handler.initialized)
    {
        return 0;
    }

    memset(&handler, 0, sizeof(handler));
    k_mutex_init(&handler.mutex);

    /* Register with cloud client */
    cloud_client_register_handler(MSG_CAT_APP, cloud_app_handle_message);

#if CONFIG_AKIRA_CLOUD_APP_UPDATE_CHECK_INTERVAL_MS > 0
    k_work_init_delayable(&s_update_check_work, update_check_work_fn);
    k_work_schedule(&s_update_check_work,
                    K_MSEC(CONFIG_AKIRA_CLOUD_APP_UPDATE_CHECK_INTERVAL_MS));
#endif

    handler.initialized = true;
    LOG_INF("Cloud app handler initialized");

    return 0;
}

int cloud_app_handler_deinit(void)
{
    k_work_cancel_delayable(&s_update_check_work);

    if (!handler.initialized)
    {
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
    if (!handler.initialized || !request || !request->app_id[0])
    {
        return -EINVAL;
    }

    k_mutex_lock(&handler.mutex, K_FOREVER);

    /* Check if already downloading */
    if (find_download(request->app_id))
    {
        k_mutex_unlock(&handler.mutex);
        LOG_WRN("Already downloading: %s", request->app_id);
        return -EALREADY;
    }

    /* Find free slot */
    struct download_context *ctx = find_free_download();
    if (!ctx)
    {
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

    for (int i = 0; i < APP_MAX_PENDING_DOWNLOADS; i++)
    {
        struct download_context *ctx = &handler.downloads[i];
        if (!ctx->active)
        {
            continue;
        }

        if (app_id == NULL || strcmp(ctx->app_id, app_id) == 0)
        {
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
    if (!ctx)
    {
        k_mutex_unlock(&handler.mutex);
        return -ENOENT;
    }

    if (received)
        *received = ctx->received;
    if (total)
        *total = ctx->total_size;

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
        .auto_start = false};

    if (app_id)
    {
        strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);
        return cloud_app_download(&req);
    }

    /* Update all - would need to iterate installed apps */
    /* TODO: Implement bulk update */
    return -ENOTSUP;
}

int cloud_app_request_catalog(app_catalog_cb_t callback, void *user_data)
{
    if (!handler.initialized || !callback)
    {
        return -EINVAL;
    }

    handler.catalog_req.pending = true;
    handler.catalog_req.callback = callback;
    handler.catalog_req.user_data = user_data;

    return cloud_client_request_app_list();
}

static void send_app_cmd_reply(const char *app_id, const void *reply, size_t reply_len,
                               app_cmd_reply_status_t status, void *user_data)
{
    msg_source_t source = *(msg_source_t *)user_data;

    if (status != APP_CMD_REPLY_OK) {
        LOG_WRN("app.cmd to %s: %s", app_id,
                status == APP_CMD_REPLY_NOT_LISTENING ? "not listening" : "timeout");
        return; /* no reply sent to Hub on failure — Hub sees no MSG_TYPE_APP_CMD echo, times out itself */
    }

    uint8_t buf[sizeof(payload_app_cmd_t) + CONFIG_AKIRA_IPC_MSG_MAX_SIZE];
    payload_app_cmd_t *out = (payload_app_cmd_t *)buf;
    strncpy(out->app_id, app_id, CLOUD_APP_ID_LEN - 1);
    out->app_id[CLOUD_APP_ID_LEN - 1] = '\0';
    memcpy(out->data, reply, reply_len);

    cloud_message_t reply_msg;
    cloud_msg_init(&reply_msg.header, MSG_TYPE_APP_CMD, MSG_SOURCE_INTERNAL);
    reply_msg.header.flags |= MSG_FLAG_RESPONSE;
    reply_msg.header.payload_len = (uint32_t)(offsetof(payload_app_cmd_t, data) + reply_len);
    reply_msg.payload = buf;
    cloud_client_send(&reply_msg, source);
}

int cloud_app_handle_message(const cloud_message_t *msg, msg_source_t source)
{
    if (!handler.initialized)
    {
        return -EINVAL;
    }

    switch (msg->header.type)
    {
    case MSG_TYPE_APP_METADATA:
        return handle_app_metadata(msg, source);

    case MSG_TYPE_APP_CHUNK:
        return handle_app_chunk(msg, source);

    case MSG_TYPE_APP_COMPLETE:
        return handle_app_complete(msg, source);

    case MSG_TYPE_APP_LIST_RESPONSE:
        return handle_app_list_response(msg, source);

    case MSG_TYPE_APP_AVAILABLE:
    {
        if (msg->payload && msg->header.payload_len >= sizeof(payload_app_metadata_t)) {
            payload_app_metadata_t *meta = (payload_app_metadata_t *)msg->payload;
            char version[APP_VERSION_MAX_LEN];
            snprintf(version, sizeof(version), "%u.%u.%u",
                    meta->version[0], meta->version[1], meta->version[2]);
            app_manager_set_update_available(meta->app_id, version);
        }
        LOG_INF("App available notification from %s", cloud_msg_source_str(source));
        return 0;
    }

    case MSG_TYPE_APP_CHECK:
        return cloud_app_check_updates();

    case MSG_TYPE_APP_START:
        if (msg->payload && msg->header.payload_len >= CLOUD_APP_ID_LEN) {
            return app_manager_start((const char *)msg->payload);
        }
        return -EINVAL;

    case MSG_TYPE_APP_STOP:
        if (msg->payload && msg->header.payload_len >= CLOUD_APP_ID_LEN) {
            return app_manager_stop((const char *)msg->payload);
        }
        return -EINVAL;

    case MSG_TYPE_APP_INSTALL:
    {
        /* Same app_id-triggers-download path apps.install.begin/end already use over BLE —
         * reuse cloud_app_download() (this file) rather than duplicating install logic. */
        if (!(msg->payload && msg->header.payload_len >= CLOUD_APP_ID_LEN)) {
            return -EINVAL;
        }
        app_download_request_t req = {
            .progress_cb = NULL, .complete_cb = NULL, .user_data = NULL,
            .auto_install = true, .auto_start = false,
        };
        strncpy(req.app_id, (const char *)msg->payload, sizeof(req.app_id) - 1);
        return cloud_app_download(&req);
    }

    case MSG_TYPE_APP_UNINSTALL:
        if (msg->payload && msg->header.payload_len >= CLOUD_APP_ID_LEN) {
            return app_manager_uninstall((const char *)msg->payload);
        }
        return -EINVAL;

    case MSG_TYPE_APP_CMD:
    {
        if (msg->header.flags & MSG_FLAG_RESPONSE) {
            return 0; /* our own reply looped back — ignore, not a request */
        }
        if (!msg->payload || msg->header.payload_len < offsetof(payload_app_cmd_t, data)) {
            return -EINVAL;
        }
        payload_app_cmd_t *cmd = (payload_app_cmd_t *)msg->payload;
        size_t data_len = msg->header.payload_len - offsetof(payload_app_cmd_t, data);
        static msg_source_t s_cmd_source; /* single AkiraHub connection — one in flight */
        s_cmd_source = source;
        return app_cmd_bridge_send(cmd->app_id, cmd->data, data_len,
                                   send_app_cmd_reply, &s_cmd_source, K_SECONDS(3));
    }

    default:
        return 0; /* Not handled */
    }
}
