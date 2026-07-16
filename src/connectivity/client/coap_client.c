/**
 * @file coap_client.c
 * @brief CoAP Client implementation for AkiraOS
 */

#include "coap_client.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/random/random.h>

#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(coap_client, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define COAP_DEFAULT_PORT 5683
#define COAPS_DEFAULT_PORT 5684
#define COAP_MAX_RETRIES 4
#define COAP_ACK_TIMEOUT_MS 2000
#define COAP_MAX_OBSERVERS 8

/* Fallback so the file still builds when block-wise transfer is disabled. */
#ifndef CONFIG_AKIRA_COAP_BLOCK_SIZE
#define CONFIG_AKIRA_COAP_BLOCK_SIZE 256
#endif

/* Per-block scratch buffer: one full block payload plus CoAP header/option
 * overhead (token, URI-Path, Block/Size, Content-Format, payload marker). */
#define COAP_BLOCK_HDR_OVERHEAD 128
#define COAP_BLOCK_BUF_SIZE (CONFIG_AKIRA_COAP_BLOCK_SIZE + COAP_BLOCK_HDR_OVERHEAD)


/*===========================================================================*/
/* Private Types                                                             */
/*===========================================================================*/

struct observe_entry
{
    bool active;
    char url[COAP_CLIENT_MAX_URL_LEN];
    coap_observe_cb_t callback;
    void *user_data;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    int sock;
};

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static bool initialized = false;
static struct k_mutex client_mutex;
static struct observe_entry observers[COAP_MAX_OBSERVERS];
static uint16_t message_id = 0;

/* DTLS PSK credentials */
static uint8_t psk_key[64];
static size_t psk_key_len = 0;
static char psk_identity[64];

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static uint16_t get_next_message_id(void)
{
    return ++message_id;
}

static void generate_token(uint8_t *token, size_t *len)
{
    *len = 4;
    sys_rand_get(token, *len);
}

static int parse_coap_url(const char *url, char *host, size_t host_len,
                          uint16_t *port, char *path, size_t path_len,
                          bool *secure)
{
    const char *p = url;

    /* Check protocol */
    if (strncmp(p, "coaps://", 8) == 0)
    {
        *secure = true;
        *port = COAPS_DEFAULT_PORT;
        p += 8;
    }
    else if (strncmp(p, "coap://", 7) == 0)
    {
        *secure = false;
        *port = COAP_DEFAULT_PORT;
        p += 7;
    }
    else
    {
        return -EINVAL;
    }

    /* Extract host */
    const char *host_end = strchr(p, '/');
    const char *port_start = strchr(p, ':');

    size_t host_copy_len;
    if (port_start && (!host_end || port_start < host_end))
    {
        host_copy_len = port_start - p;
        if (host_copy_len >= host_len)
        {
            return -ENOMEM;
        }
        memcpy(host, p, host_copy_len);
        host[host_copy_len] = '\0';

        /* Parse port */
        *port = (uint16_t)atoi(port_start + 1);
    }
    else if (host_end)
    {
        host_copy_len = host_end - p;
        if (host_copy_len >= host_len)
        {
            return -ENOMEM;
        }
        memcpy(host, p, host_copy_len);
        host[host_copy_len] = '\0';
    }
    else
    {
        size_t url_len = strlen(p);
        if (url_len >= host_len)
        {
            return -ENOMEM;
        }
        strcpy(host, p);
    }

    /* Extract path */
    if (host_end)
    {
        size_t path_copy_len = strlen(host_end);
        if (path_copy_len >= path_len)
        {
            return -ENOMEM;
        }
        strcpy(path, host_end);
    }
    else
    {
        strcpy(path, "/");
    }

    return 0;
}

static int create_socket(const char *host, uint16_t port, bool secure,
                         struct sockaddr *addr, socklen_t *addr_len)
{
    int sock;
    int ret;

    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
    memset(addr4, 0, sizeof(struct sockaddr_in));
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    *addr_len = sizeof(struct sockaddr_in);

    /* Fast path: the host is already a literal IPv4 address. */
    ret = zsock_inet_pton(AF_INET, host, &addr4->sin_addr);
    if (ret != 1)
    {
        /* Otherwise resolve the hostname via DNS. The query is restricted
         * to IPv4 to match the AF_INET socket created below; switch
         * ai_family to AF_UNSPEC (and generalise the sockaddr handling) if
         * IPv6 support is needed. */
        struct zsock_addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_DGRAM,
        };
        struct zsock_addrinfo *res = NULL;

        ret = zsock_getaddrinfo(host, NULL, &hints, &res);
        if (ret != 0 || res == NULL)
        {
            LOG_ERR("DNS resolution failed for '%s' (ret=%d)", host, ret);
            return -EHOSTUNREACH;
        }

        /* Use the first A record and keep the port parsed from the URL. */
        memcpy(addr4, res->ai_addr, sizeof(struct sockaddr_in));
        addr4->sin_port = htons(port);
        *addr_len = sizeof(struct sockaddr_in);
        zsock_freeaddrinfo(res);
    }

    /* Create UDP socket (DTLS-wrapped when secure). */
    sock = zsock_socket(AF_INET, SOCK_DGRAM, secure ? IPPROTO_DTLS_1_2 : IPPROTO_UDP);
    if (sock < 0)
    {
        LOG_ERR("Failed to create socket: %d", errno);
        return -errno;
    }

    /* Configure DTLS if secure */
    if (secure && psk_key_len > 0)
    {
#ifdef CONFIG_NET_SOCKETS_SOCKOPT_TLS
        sec_tag_t sec_tag_list[] = {1}; /* Use tag 1 for PSK */
        ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                               sec_tag_list, sizeof(sec_tag_list));
        if (ret < 0)
        {
            LOG_WRN("Failed to set DTLS sec tag: %d", errno);
        }

        /* Provide the server name for SNI / peer verification. */
        ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                               host, strlen(host) + 1);
        if (ret < 0)
        {
            LOG_WRN("Failed to set DTLS hostname: %d", errno);
        }
#endif
    }

    /* Connect socket */
    ret = zsock_connect(sock, addr, *addr_len);
    if (ret < 0)
    {
        LOG_ERR("Failed to connect: %d", errno);
        zsock_close(sock);
        return -errno;
    }

    return sock;
}


/* Map the configured block size to the RFC 7959 enum; unsupported values
 * fall back to 256 bytes. */
static enum coap_block_size akira_coap_block_size(void)
{
    switch (CONFIG_AKIRA_COAP_BLOCK_SIZE)
    {
    case 16:
        return COAP_BLOCK_16;
    case 32:
        return COAP_BLOCK_32;
    case 64:
        return COAP_BLOCK_64;
    case 128:
        return COAP_BLOCK_128;
    case 256:
        return COAP_BLOCK_256;
    case 512:
        return COAP_BLOCK_512;
    case 1024:
        return COAP_BLOCK_1024;
    default:
        return COAP_BLOCK_256;
    }
}

/* Append each '/'-separated path segment as a Uri-Path option. strtok()
 * transparently skips a leading '/'. */
static int coap_append_uri_path(struct coap_packet *pkt, const char *path)
{
    if (!path || path[0] == '\0')
    {
        return 0;
    }

    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *segment = strtok(path_copy, "/");
    while (segment)
    {
        int ret = coap_packet_append_option(pkt, COAP_OPTION_URI_PATH,
                                            (uint8_t *)segment, strlen(segment));
        if (ret < 0)
        {
            return ret;
        }
        segment = strtok(NULL, "/");
    }

    return 0;
}

/* Parse a coap[s]:// URL, resolve it and return a connected UDP/DTLS socket.
 * On success the resource path is written to 'path'. */
static int coap_open_url_socket(const char *url, char *path, size_t path_len,
                                struct sockaddr_storage *addr, socklen_t *addr_len)
{
    char host[128];
    uint16_t port;
    bool secure;
    int ret;

    ret = parse_coap_url(url, host, sizeof(host), &port, path, path_len, &secure);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse URL: %d", ret);
        return ret;
    }

    return create_socket(host, port, secure, (struct sockaddr *)addr, addr_len);
}


static int send_coap_request(int sock, const coap_request_t *request,
                             const char *path, coap_response_t *response)
{
    uint8_t tx_buf[512 + COAP_CLIENT_MAX_PAYLOAD];
    uint8_t rx_buf[512 + COAP_CLIENT_MAX_PAYLOAD];
    struct coap_packet pkt;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    int ret;

    /* Generate token */
    generate_token(token, &token_len);

    /* Initialize CoAP packet */
    ret = coap_packet_init(&pkt, tx_buf, sizeof(tx_buf),
                           COAP_VERSION_1,
                           request->type == COAP_TYPE_CON ? COAP_TYPE_CON : COAP_TYPE_NON,
                           token_len, token,
                           request->method, get_next_message_id());
    if (ret < 0)
    {
        LOG_ERR("Failed to init CoAP packet: %d", ret);
        return ret;
    }

    /* Add URI path options */
    if (path && path[0] == '/')
    {
        path++;
    }
    if (path && strlen(path) > 0)
    {
        char path_copy[256];
        strncpy(path_copy, path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *segment = strtok(path_copy, "/");
        while (segment)
        {
            ret = coap_packet_append_option(&pkt, COAP_OPTION_URI_PATH,
                                            (uint8_t *)segment, strlen(segment));
            if (ret < 0)
            {
                LOG_ERR("Failed to add URI path: %d", ret);
                return ret;
            }
            segment = strtok(NULL, "/");
        }
    }

    /* Add content format if we have payload */
    if (request->payload && request->payload_len > 0)
    {
        uint8_t fmt_buf[2];
        size_t fmt_len = 1;
        fmt_buf[0] = request->format & 0xFF;
        if (request->format > 0xFF)
        {
            fmt_buf[0] = (request->format >> 8) & 0xFF;
            fmt_buf[1] = request->format & 0xFF;
            fmt_len = 2;
        }

        ret = coap_packet_append_option(&pkt, COAP_OPTION_CONTENT_FORMAT,
                                        fmt_buf, fmt_len);
        if (ret < 0)
        {
            LOG_ERR("Failed to add content format: %d", ret);
            return ret;
        }

        /* Add payload */
        ret = coap_packet_append_payload_marker(&pkt);
        if (ret < 0)
        {
            LOG_ERR("Failed to add payload marker: %d", ret);
            return ret;
        }

        ret = coap_packet_append_payload(&pkt, request->payload, request->payload_len);
        if (ret < 0)
        {
            LOG_ERR("Failed to add payload: %d", ret);
            return ret;
        }
    }

    /* Send request */
    ret = zsock_send(sock, pkt.data, pkt.offset, 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to send: %d", errno);
        return -errno;
    }

    LOG_DBG("Sent CoAP request, %d bytes", ret);

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec = request->timeout_ms / 1000,
        .tv_usec = (request->timeout_ms % 1000) * 1000};
    if (tv.tv_sec == 0 && tv.tv_usec == 0)
    {
        tv.tv_sec = 5; /* Default 5 second timeout */
    }
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Receive response */
    ret = zsock_recv(sock, rx_buf, sizeof(rx_buf), 0);
    if (ret < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            LOG_WRN("CoAP request timeout");
            return -ETIMEDOUT;
        }
        LOG_ERR("Failed to receive: %d", errno);
        return -errno;
    }

    LOG_DBG("Received CoAP response, %d bytes", ret);

    /* Parse response */
    struct coap_packet resp_pkt;
    ret = coap_packet_parse(&resp_pkt, rx_buf, ret, NULL, 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse response: %d", ret);
        return ret;
    }

    /* Extract response data */
    response->code = coap_header_get_code(&resp_pkt);

    /* Get token. coap_header_get_token() copies the token into the caller's
     * buffer (which must hold at least COAP_TOKEN_MAX_LEN bytes) and returns
     * its length. */
    uint8_t resp_token[COAP_CLIENT_MAX_TOKEN_LEN];
    uint8_t resp_token_len = coap_header_get_token(&resp_pkt, resp_token);
    response->token_len = resp_token_len;
    if (resp_token_len > 0 && resp_token_len <= COAP_CLIENT_MAX_TOKEN_LEN)
    {
        memcpy(response->token, resp_token, resp_token_len);
    }

    /* Get payload */
    uint16_t payload_len;
    const uint8_t *payload = coap_packet_get_payload(&resp_pkt, &payload_len);
    if (payload && payload_len > 0)
    {
        /* Allocate and copy payload */
        uint8_t *payload_copy = k_malloc(payload_len);
        if (payload_copy)
        {
            memcpy(payload_copy, payload, payload_len);
            response->payload = payload_copy;
            response->payload_len = payload_len;
        }
        else
        {
            response->payload = NULL;
            response->payload_len = 0;
        }
    }
    else
    {
        response->payload = NULL;
        response->payload_len = 0;
    }

    /* Parse the Content-Format option (RFC 7252 sec 5.10.3). A negative
     * return means the option is absent; default to text/plain to preserve
     * the previous behaviour. */
    int content_format = coap_get_option_int(&resp_pkt, COAP_OPTION_CONTENT_FORMAT);
    if (content_format >= 0)
    {
        response->format = (coap_content_format_t)content_format;
    }
    else
    {
        response->format = COAP_FORMAT_TEXT_PLAIN;
    }

    return 0;
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

int coap_client_init(void)
{
    if (initialized)
    {
        return 0;
    }

    k_mutex_init(&client_mutex);
    memset(observers, 0, sizeof(observers));
    message_id = sys_rand32_get() & 0xFFFF;

    initialized = true;
    LOG_INF("CoAP client initialized");

    return 0;
}

int coap_client_deinit(void)
{
    if (!initialized)
    {
        return 0;
    }

    /* Stop all observers */
    for (int i = 0; i < COAP_MAX_OBSERVERS; i++)
    {
        if (observers[i].active)
        {
            coap_client_observe_stop(i);
        }
    }

    initialized = false;
    LOG_INF("CoAP client deinitialized");

    return 0;
}

int coap_client_request(const coap_request_t *request, coap_response_t *response)
{
    if (!initialized || !request || !response)
    {
        return -EINVAL;
    }

    char host[128];
    uint16_t port;
    char path[256];
    bool secure;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int ret;

    /* Parse URL */
    ret = parse_coap_url(request->url, host, sizeof(host),
                         &port, path, sizeof(path), &secure);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse URL: %d", ret);
        return ret;
    }

    LOG_DBG("Request to %s:%d%s (secure=%d)", host, port, path, secure);

    k_mutex_lock(&client_mutex, K_FOREVER);

    /* Create socket */
    int sock = create_socket(host, port, secure,
                             (struct sockaddr *)&addr, &addr_len);
    if (sock < 0)
    {
        k_mutex_unlock(&client_mutex);
        return sock;
    }

    /* Send request and get response */
    ret = send_coap_request(sock, request, path, response);

    zsock_close(sock);
    k_mutex_unlock(&client_mutex);

    return ret;
}

int coap_client_get(const char *url, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_GET,
        .type = COAP_TYPE_CON,
        .format = COAP_FORMAT_TEXT_PLAIN,
        .payload = NULL,
        .payload_len = 0,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_post(const char *url, const uint8_t *payload, size_t payload_len,
                     coap_content_format_t format, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_POST,
        .type = COAP_TYPE_CON,
        .format = format,
        .payload = payload,
        .payload_len = payload_len,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_put(const char *url, const uint8_t *payload, size_t payload_len,
                    coap_content_format_t format, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_PUT,
        .type = COAP_TYPE_CON,
        .format = format,
        .payload = payload,
        .payload_len = payload_len,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_delete(const char *url, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_DELETE,
        .type = COAP_TYPE_CON,
        .format = COAP_FORMAT_TEXT_PLAIN,
        .payload = NULL,
        .payload_len = 0,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

coap_observe_handle_t coap_client_observe(const char *url,
                                          coap_observe_cb_t callback,
                                          void *user_data)
{
    if (!initialized || !url || !callback)
    {
        return -EINVAL;
    }

    k_mutex_lock(&client_mutex, K_FOREVER);

    /* Find free observer slot */
    int handle = -1;
    for (int i = 0; i < COAP_MAX_OBSERVERS; i++)
    {
        if (!observers[i].active)
        {
            handle = i;
            break;
        }
    }

    if (handle < 0)
    {
        k_mutex_unlock(&client_mutex);
        LOG_ERR("No free observer slots");
        return -ENOMEM;
    }

    /* Set up observer */
    struct observe_entry *obs = &observers[handle];
    strncpy(obs->url, url, sizeof(obs->url) - 1);
    obs->url[sizeof(obs->url) - 1] = '\0';
    obs->callback = callback;
    obs->user_data = user_data;
    generate_token(obs->token, &obs->token_len);

    /* TODO: Send observe request and start background receive thread */
    /* For now, this is a stub - real implementation would need a dedicated
     * thread or work queue to handle incoming notifications */

    obs->active = true;

    k_mutex_unlock(&client_mutex);

    LOG_INF("Started observing: %s (handle=%d)", url, handle);

    return handle;
}

int coap_client_observe_stop(coap_observe_handle_t handle)
{
    if (!initialized || handle < 0 || handle >= COAP_MAX_OBSERVERS)
    {
        return -EINVAL;
    }

    k_mutex_lock(&client_mutex, K_FOREVER);

    struct observe_entry *obs = &observers[handle];
    if (!obs->active)
    {
        k_mutex_unlock(&client_mutex);
        return -ENOENT;
    }

    /* TODO: Send cancel observe request */

    if (obs->sock >= 0)
    {
        zsock_close(obs->sock);
    }

    obs->active = false;

    k_mutex_unlock(&client_mutex);

    LOG_INF("Stopped observing (handle=%d)", handle);

    return 0;
}

int coap_client_download(const char *url, uint8_t *buffer, size_t buffer_len,
                         size_t *received_len)
{
    if (!initialized || !url || !buffer || !received_len)
    {
        return -EINVAL;
    }

    char path[256];
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    struct coap_block_context blk_ctx;
    int sock;
    int ret;

    *received_len = 0;

    k_mutex_lock(&client_mutex, K_FOREVER);

    sock = coap_open_url_socket(url, path, sizeof(path), &addr, &addr_len);
    if (sock < 0)
    {
        k_mutex_unlock(&client_mutex);
        return sock;
    }

    /* Bounded blocking receive per block. */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* One token for the whole transfer (RFC 7959). */
    generate_token(token, &token_len);
    coap_block_transfer_init(&blk_ctx, akira_coap_block_size(), 0);

    while (true)
    {
        uint8_t tx_buf[COAP_BLOCK_BUF_SIZE];
        uint8_t rx_buf[COAP_BLOCK_BUF_SIZE];
        struct coap_packet pkt;
        struct coap_packet reply;
        int rcvd;

        ret = coap_packet_init(&pkt, tx_buf, sizeof(tx_buf), COAP_VERSION_1,
                               COAP_TYPE_CON, token_len, token,
                               COAP_METHOD_GET, get_next_message_id());
        if (ret < 0)
        {
            goto out;
        }

        ret = coap_append_uri_path(&pkt, path);
        if (ret < 0)
        {
            goto out;
        }

        /* Request the current block; block number is derived from
         * blk_ctx.current. */
        ret = coap_append_block2_option(&pkt, &blk_ctx);
        if (ret < 0)
        {
            goto out;
        }

        ret = zsock_send(sock, pkt.data, pkt.offset, 0);
        if (ret < 0)
        {
            ret = -errno;
            goto out;
        }

        rcvd = zsock_recv(sock, rx_buf, sizeof(rx_buf), 0);
        if (rcvd < 0)
        {
            ret = (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
            goto out;
        }

        ret = coap_packet_parse(&reply, rx_buf, rcvd, NULL, 0);
        if (ret < 0)
        {
            goto out;
        }

        uint8_t code = coap_header_get_code(&reply);
        if (code != COAP_CODE_CONTENT)
        {
            LOG_ERR("Block2 download failed: %s", coap_code_to_str(code));
            ret = -EIO;
            goto out;
        }

        /* Pull Block2/Size2 from the reply: sets blk_ctx.current to this
         * block's byte offset and records the total size when advertised.
         * Degrades gracefully to a single response if the server ignores
         * block-wise (current stays 0). */
        ret = coap_update_from_block(&reply, &blk_ctx);
        if (ret < 0)
        {
            goto out;
        }

        uint16_t frag_len = 0;
        const uint8_t *frag = coap_packet_get_payload(&reply, &frag_len);
        size_t offset = blk_ctx.current;
        if (frag && frag_len > 0)
        {
            if (offset + frag_len > buffer_len)
            {
                LOG_ERR("Download buffer too small (have %zu bytes)", buffer_len);
                ret = -ENOMEM;
                goto out;
            }
            memcpy(buffer + offset, frag, frag_len);
            if (offset + frag_len > *received_len)
            {
                *received_len = offset + frag_len;
            }
        }

        /* Advance to the next block; returns 0 once the last block (MORE=0)
         * has been received. */
        if (coap_next_block(&reply, &blk_ctx) == 0)
        {
            ret = 0;
            break;
        }
    }

out:
    zsock_close(sock);
    k_mutex_unlock(&client_mutex);
    return ret;
}

int coap_client_upload(const char *url, const uint8_t *data, size_t data_len,
                       coap_content_format_t format)
{
    if (!initialized || !url || !data)
    {
        return -EINVAL;
    }

    /* Small payloads that fit in a single block need no block-wise transfer;
     * fall back to a plain PUT (which handles its own socket + mutex). */
    size_t block_bytes = coap_block_size_to_bytes(akira_coap_block_size());
    if (data_len <= block_bytes)
    {
        coap_response_t resp = {0};
        int ret = coap_client_put(url, data, data_len, format, &resp);
        if (ret == 0 && resp.code != COAP_CODE_CHANGED &&
            resp.code != COAP_CODE_CREATED)
        {
            ret = -EIO;
        }
        coap_client_free_response(&resp);
        return ret;
    }

    char path[256];
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    struct coap_block_context blk_ctx;
    int sock;
    int ret;
    bool first = true;

    k_mutex_lock(&client_mutex, K_FOREVER);

    sock = coap_open_url_socket(url, path, sizeof(path), &addr, &addr_len);
    if (sock < 0)
    {
        k_mutex_unlock(&client_mutex);
        return sock;
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    generate_token(token, &token_len);
    coap_block_transfer_init(&blk_ctx, akira_coap_block_size(), data_len);

    while (true)
    {
        uint8_t tx_buf[COAP_BLOCK_BUF_SIZE];
        uint8_t rx_buf[COAP_BLOCK_BUF_SIZE];
        struct coap_packet pkt;
        struct coap_packet reply;
        int rcvd;

        size_t bytes = coap_block_size_to_bytes(blk_ctx.block_size);
        size_t remaining = data_len - blk_ctx.current;
        size_t frag_len = MIN(bytes, remaining);
        bool last = (blk_ctx.current + frag_len >= data_len);

        ret = coap_packet_init(&pkt, tx_buf, sizeof(tx_buf), COAP_VERSION_1,
                               COAP_TYPE_CON, token_len, token,
                               COAP_METHOD_PUT, get_next_message_id());
        if (ret < 0)
        {
            goto out;
        }

        ret = coap_append_uri_path(&pkt, path);
        if (ret < 0)
        {
            goto out;
        }

        /* Content-Format (option 12) must precede Block1 (option 27). */
        uint8_t fmt_buf[2];
        size_t fmt_len = 1;
        fmt_buf[0] = format & 0xFF;
        if (format > 0xFF)
        {
            fmt_buf[0] = (format >> 8) & 0xFF;
            fmt_buf[1] = format & 0xFF;
            fmt_len = 2;
        }
        ret = coap_packet_append_option(&pkt, COAP_OPTION_CONTENT_FORMAT,
                                        fmt_buf, fmt_len);
        if (ret < 0)
        {
            goto out;
        }

        /* Block1 option: MORE flag and block number derived from blk_ctx. */
        ret = coap_append_block1_option(&pkt, &blk_ctx);
        if (ret < 0)
        {
            goto out;
        }

        /* Advertise the full body size once, on the first block. */
        if (first)
        {
            ret = coap_append_size1_option(&pkt, &blk_ctx);
            if (ret < 0)
            {
                goto out;
            }
            first = false;
        }

        ret = coap_packet_append_payload_marker(&pkt);
        if (ret < 0)
        {
            goto out;
        }

        ret = coap_packet_append_payload(&pkt, data + blk_ctx.current, frag_len);
        if (ret < 0)
        {
            goto out;
        }

        ret = zsock_send(sock, pkt.data, pkt.offset, 0);
        if (ret < 0)
        {
            ret = -errno;
            goto out;
        }

        rcvd = zsock_recv(sock, rx_buf, sizeof(rx_buf), 0);
        if (rcvd < 0)
        {
            ret = (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
            goto out;
        }

        ret = coap_packet_parse(&reply, rx_buf, rcvd, NULL, 0);
        if (ret < 0)
        {
            goto out;
        }

        uint8_t code = coap_header_get_code(&reply);

        if (last)
        {
            /* Final block must be acknowledged with 2.04 Changed or
             * 2.01 Created. */
            if (code != COAP_CODE_CHANGED && code != COAP_CODE_CREATED)
            {
                LOG_ERR("Block1 upload final ack unexpected: %s",
                        coap_code_to_str(code));
                ret = -EIO;
                goto out;
            }
            ret = 0;
            goto out;
        }

        /* Intermediate blocks are acknowledged with 2.31 Continue (some
         * servers reply 2.04). */
        if (code != COAP_RESPONSE_CODE_CONTINUE && code != COAP_CODE_CHANGED)
        {
            LOG_ERR("Block1 upload block rejected: %s", coap_code_to_str(code));
            ret = -EIO;
            goto out;
        }

        /* Validate the server's Block1 echo and pick up any negotiated
         * (smaller) block size. */
        ret = coap_update_from_block(&reply, &blk_ctx);
        if (ret < 0)
        {
            goto out;
        }

        blk_ctx.current += frag_len;
    }

out:
    zsock_close(sock);
    k_mutex_unlock(&client_mutex);
    return ret;
}

void coap_client_free_response(coap_response_t *response)
{
    if (response && response->payload)
    {
        k_free((void *)response->payload);
        response->payload = NULL;
        response->payload_len = 0;
    }
}

const char *coap_code_to_str(coap_code_t code)
{
    switch (code)
    {
    case COAP_CODE_CREATED:
        return "2.01 Created";
    case COAP_CODE_DELETED:
        return "2.02 Deleted";
    case COAP_CODE_VALID:
        return "2.03 Valid";
    case COAP_CODE_CHANGED:
        return "2.04 Changed";
    case COAP_CODE_CONTENT:
        return "2.05 Content";
    case COAP_CODE_BAD_REQUEST:
        return "4.00 Bad Request";
    case COAP_CODE_UNAUTHORIZED:
        return "4.01 Unauthorized";
    case COAP_CODE_FORBIDDEN:
        return "4.03 Forbidden";
    case COAP_CODE_NOT_FOUND:
        return "4.04 Not Found";
    case COAP_CODE_NOT_ALLOWED:
        return "4.05 Method Not Allowed";
    case COAP_CODE_INTERNAL_ERR:
        return "5.00 Internal Server Error";
    case COAP_CODE_NOT_IMPL:
        return "5.01 Not Implemented";
    case COAP_CODE_UNAVAILABLE:
        return "5.03 Service Unavailable";
    default:
        return "Unknown";
    }
}

int coap_client_set_psk(const uint8_t *psk, size_t psk_len, const char *psk_id)
{
    if (!psk || psk_len == 0 || !psk_id)
    {
        return -EINVAL;
    }

    if (psk_len > sizeof(psk_key))
    {
        return -ENOMEM;
    }

    memcpy(psk_key, psk, psk_len);
    psk_key_len = psk_len;
    strncpy(psk_identity, psk_id, sizeof(psk_identity) - 1);
    psk_identity[sizeof(psk_identity) - 1] = '\0';

    LOG_INF("CoAP DTLS PSK configured");

    return 0;
}
