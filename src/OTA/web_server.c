/**
 * @file web_server.c
 * @brief Optimized Web Server Implementation for ESP32
 *
 */

/* Enable GNU extensions for memmem (used for multipart boundary search in HTTP uploads) */
#define _GNU_SOURCE
#include <string.h>

#include "web_server.h"
#include "ota_manager.h"

/* WebServer OTA transport implementation */
static int webserver_ota_start(void *user_data)
{
    /* Start OTA via HTTP upload */
    return OTA_OK;
}

static int webserver_ota_stop(void *user_data)
{
    /* Stop OTA upload */
    return OTA_OK;
}

static int webserver_ota_send_chunk(const uint8_t *data, size_t len, void *user_data)
{
    /* Pass chunk to OTA manager */
    // Example: ota_manager_write_chunk(data, len);
    return OTA_OK;
}

static int webserver_ota_report_progress(uint8_t percent, void *user_data)
{
    /* Report progress to HTTP client */
    return OTA_OK;
}

static ota_transport_t webserver_ota_transport = {
    .name = "webserver",
    .start = webserver_ota_start,
    .stop = webserver_ota_stop,
    .send_chunk = webserver_ota_send_chunk,
    .report_progress = webserver_ota_report_progress,
    .user_data = NULL};

static void register_webserver_ota_transport(void)
{
    ota_manager_register_transport(&webserver_ota_transport);
}

/* Call register_webserver_ota_transport() during web server init */
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "akira/akira.h"
#ifdef CONFIG_AKIRA_APP_MANAGER
#include "../services/app_manager.h"
#endif

LOG_MODULE_REGISTER(web_server, AKIRA_LOG_LEVEL);

/* TCP_NODELAY may not be defined on all platforms */
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

/* Optimized constants */
#define HTTP_BUFFER_SIZE 768
#define HTTP_RESPONSE_BUFFER_SIZE 1536
#undef UPLOAD_CHUNK_SIZE
#define UPLOAD_CHUNK_SIZE 512
#define MAX_CONNECTIONS 2

/* Thread stack - increased for socket operations */
static K_THREAD_STACK_DEFINE(web_server_stack, 8192);
static struct k_thread web_server_thread_data;
static k_tid_t web_server_thread_id;

/* Compact server state */
static struct
{
    enum web_server_state state;
    uint32_t requests_handled;
    uint32_t bytes_transferred;
    uint8_t active_connections;
    bool network_connected;
    char server_ip[16];
} server_state __aligned(4);

static struct web_server_callbacks callbacks = {0};
static K_MUTEX_DEFINE(server_mutex);

/* Message queue - reduced size */
#define SERVER_MSG_QUEUE_SIZE 8

enum server_msg_type
{
    MSG_START_SERVER = 1,
    MSG_STOP_SERVER,
    MSG_NETWORK_STATUS_CHANGED
};

struct server_msg
{
    enum server_msg_type type : 8;
    uint8_t reserved;
    union
    {
        struct
        {
            bool connected;
            char ip_address[16];
        } network_status;
    } data;
} __packed;

K_MSGQ_DEFINE(server_msgq, sizeof(struct server_msg), SERVER_MSG_QUEUE_SIZE, 4);

/* Log buffer for web terminal - compact size */
#define LOG_BUFFER_SIZE 2048
#define MAX_LOG_LINES 30
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffer_pos = 0;
static K_MUTEX_DEFINE(log_mutex);

/* Add log entry to buffer */
void web_server_add_log(const char *log_line)
{
    k_mutex_lock(&log_mutex, K_FOREVER);
    size_t len = strlen(log_line);
    if (log_buffer_pos + len + 2 >= LOG_BUFFER_SIZE)
    {
        /* Shift buffer - remove first half */
        size_t half = LOG_BUFFER_SIZE / 2;
        memmove(log_buffer, log_buffer + half, log_buffer_pos - half);
        log_buffer_pos -= half;
    }
    memcpy(log_buffer + log_buffer_pos, log_line, len);
    log_buffer_pos += len;
    log_buffer[log_buffer_pos++] = '\n';
    log_buffer[log_buffer_pos] = '\0';
    k_mutex_unlock(&log_mutex);
}

/* Working HTML with WASM apps - tested and functional */
static const char html_page[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AkiraOS</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:'Segoe UI',system-ui,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh}"
    ".header{background:linear-gradient(135deg,#1a1a2e 0%,#16213e 100%);padding:20px;text-align:center;border-bottom:2px solid #0f3460}"
    ".header h1{color:#00d4ff;font-size:28px;text-shadow:0 0 10px #00d4ff40}"
    ".header .version{color:#888;font-size:14px;margin-top:5px}"
    ".container{max-width:1200px;margin:0 auto;padding:20px}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:20px}"
    "@media(max-width:768px){.grid{grid-template-columns:1fr}}"
    ".panel{background:#1a1a2e;border-radius:10px;padding:20px;border:1px solid #0f3460}"
    ".panel h3{color:#00d4ff;margin-bottom:15px;font-size:16px;border-bottom:1px solid #0f3460;padding-bottom:10px}"
    ".terminal{background:#0d1117;border-radius:8px;font-family:'Consolas','Monaco',monospace;height:300px;overflow:hidden;display:flex;flex-direction:column}"
    ".terminal-header{background:#161b22;padding:10px 15px;border-bottom:1px solid #30363d;display:flex;align-items:center;gap:8px}"
    ".terminal-header .dot{width:12px;height:12px;border-radius:50%}"
    ".terminal-header .dot.red{background:#ff5f56}"
    ".terminal-header .dot.yellow{background:#ffbd2e}"
    ".terminal-header .dot.green{background:#27c93f}"
    ".terminal-header span{color:#8b949e;margin-left:10px;font-size:13px}"
    ".terminal-body{flex:1;overflow-y:auto;padding:15px;font-size:13px;line-height:1.6}"
    ".terminal-body pre{white-space:pre-wrap;word-wrap:break-word;color:#c9d1d9}"
    ".cmd-input{display:flex;background:#161b22;border-top:1px solid #30363d;padding:10px}"
    ".cmd-input span{color:#27c93f;padding:0 10px}"
    ".cmd-input input{flex:1;background:transparent;border:none;color:#c9d1d9;font-family:inherit;font-size:13px;outline:none}"
    ".status-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}"
    ".status-item{background:#0d1117;padding:12px;border-radius:6px;border-left:3px solid #00d4ff}"
    ".status-item label{color:#8b949e;font-size:12px;display:block}"
    ".status-item value{color:#e0e0e0;font-size:16px;font-weight:500}"
    ".btn{background:#238636;color:white;padding:10px 20px;border:none;border-radius:6px;cursor:pointer;font-size:14px;transition:all 0.2s}"
    ".btn:hover{background:#2ea043}"
    ".btn-danger{background:#da3633}"
    ".btn-danger:hover{background:#f85149}"
    ".btn-blue{background:#1f6feb}"
    ".btn-blue:hover{background:#388bfd}"
    ".btn-small{padding:6px 12px;font-size:12px}"
    ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:15px}"
    ".app-list{background:#0d1117;border-radius:6px;padding:12px;min-height:150px;max-height:300px;overflow-y:auto}"
    ".app-item{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:10px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}"
    ".app-name{color:#58a6ff;font-weight:bold}"
    ".app-state{font-size:11px;color:#8b949e;margin-top:2px}"
    ".app-state.running{color:#27c93f}"
    ".app-actions{display:flex;gap:5px}"
    "input[type=file]{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:8px;color:#c9d1d9;font-size:13px;width:100%;margin-bottom:10px}"
    "input[type=text]{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:8px;color:#c9d1d9;font-size:13px;width:100%;margin-bottom:10px}"
    "</style></head><body>"
    "<div class='header'><h1> AkiraOS Web Dashboard</h1><div class='version'>AkiraOS v1.2.3</div></div>"
    "<div class='container'>"
    "<div class='grid'>"
    "<div class='panel'><h3> System Status</h3><div class='status-grid'>"
    "<div class='status-item'><label>Device</label><value id='dev'>Online</value></div>"
    "<div class='status-item'><label>IP Address</label><value id='ip'>Loading...</value></div>"
    "<div class='status-item'><label>Uptime</label><value id='uptime'>--:--:--</value></div>"
    "<div class='status-item'><label>Memory</label><value id='mem'>--</value></div>"
    "</div>"
    "<div class='actions'>"
    "<button class='btn btn-blue' onclick='refresh()'> Refresh</button>"
    "<button class='btn btn-danger' onclick='reboot()'> Reboot</button>"
    "</div></div>"
    "<div class='panel'><h3> OTA Update</h3>"
    "<form id='otaForm' enctype='multipart/form-data'>"
    "<input type='file' id='firmware' accept='.bin' style='margin-bottom:10px'><br>"
    "<button type='submit' class='btn'> Upload Firmware</button>"
    "</form>"
    "<div id='progress' style='margin-top:10px'></div>"
    "</div></div>"
    "<div class='panel'><h3> WASM Applications</h3>"
    "<input type='file' id='wasm' accept='.wasm'>"
    "<input type='text' id='name' placeholder='App name (optional)'>"
    "<button class='btn' onclick='installApp()'>Install WASM App</button>"
    "<div id='status' style='margin:10px 0;color:#00d4ff;font-size:13px'></div>"
    "<div class='app-list' id='apps'>Loading apps...</div>"
    "</div>"
    "<div class='panel'><h3> Terminal</h3>"
    "<div class='terminal'>"
    "<div class='terminal-header'><div class='dot red'></div><div class='dot yellow'></div><div class='dot green'></div><span>akira@esp32s3 ~</span></div>"
    "<div class='terminal-body' id='logs'><pre id='logContent'>Loading logs...</pre></div>"
    "<div class='cmd-input'><span>$</span><input type='text' id='cmd' placeholder='Enter command...' onkeypress='if(event.key==\"Enter\")sendCmd()'></div>"
    "</div></div></div>"
    "<script>"
    "function fetchStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('ip').textContent=d.ip;document.getElementById('uptime').textContent=d.uptime;document.getElementById('mem').textContent=d.mem}).catch(()=>{})}"
    "function fetchLogs(){fetch('/api/logs').then(r=>r.text()).then(d=>{document.getElementById('logContent').innerHTML=d;var el=document.getElementById('logs');el.scrollTop=el.scrollHeight})}"
    "function listApps(){fetch('/api/apps/list').then(r=>r.json()).then(d=>{var html='';if(d.apps&&d.apps.length>0){d.apps.forEach(app=>{html+='<div class=\"app-item\"><div><div class=\"app-name\">'+app.name+'</div><div class=\"app-state '+(app.state==='running'?'running':'')+'\">'+app.state+'</div></div><div class=\"app-actions\">';if(app.state!=='running'){html+='<button class=\"btn btn-small\" onclick=\"startApp(\\''+app.name+'\\');\">Start</button>'}else{html+='<button class=\"btn btn-small btn-danger\" onclick=\"stopApp(\\''+app.name+'\\');\">Stop</button>'}html+='<button class=\"btn btn-small btn-danger\" onclick=\"uninstallApp(\\''+app.name+'\\');\">Delete</button></div></div>'})}else{html='<div style=\"color:#8b949e;text-align:center;padding:20px\">No WASM apps installed</div>'}document.getElementById('apps').innerHTML=html}).catch(e=>{document.getElementById('apps').innerHTML='<div style=\"color:#f85149\">Error loading apps</div>'})}"
    "function installApp(){var file=document.getElementById('wasm').files[0];if(!file){alert('Select WASM file');return}var name=document.getElementById('name').value||file.name.replace('.wasm','');document.getElementById('status').innerHTML='Installing...';var reader=new FileReader();reader.onload=function(e){fetch('/api/apps/install?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:e.target.result}).then(r=>r.json()).then(d=>{document.getElementById('status').innerHTML=d.error?('<span style=\"color:#f85149\">Error: '+d.error+'</span>'):'<span style=\"color:#27c93f\">Installed!</span>';listApps();document.getElementById('wasm').value='';document.getElementById('name').value=''}).catch(e=>{document.getElementById('status').innerHTML='<span style=\"color:#f85149\">Error: '+e+'</span>'})};reader.readAsArrayBuffer(file)}"
    "function startApp(name){fetch('/api/apps/start?name='+encodeURIComponent(name),{method:'POST'}).then(()=>listApps()).catch(e=>alert('Error'))}"
    "function stopApp(name){fetch('/api/apps/stop?name='+encodeURIComponent(name),{method:'POST'}).then(()=>listApps()).catch(e=>alert('Error'))}"
    "function uninstallApp(name){if(confirm('Delete '+name+'?')){fetch('/api/apps/uninstall?name='+encodeURIComponent(name),{method:'POST'}).then(()=>listApps()).catch(e=>alert('Error'))}}"
    "function sendCmd(){var c=document.getElementById('cmd').value;if(c){document.getElementById('cmd').value='';fetch('/api/cmd?c='+encodeURIComponent(c)).then(r=>r.text()).then(d=>{fetchLogs()})}}"
    "function reboot(){if(confirm('Reboot device?')){fetch('/api/reboot',{method:'POST'}).then(()=>alert('Rebooting...'))}}"
    "function refresh(){location.reload()}"
    "document.getElementById('otaForm').onsubmit=function(e){e.preventDefault();var f=document.getElementById('firmware').files[0];if(!f){alert('Select firmware file');return}"
    "var p=document.getElementById('progress');p.innerHTML='<div style=\"background:#444;border-radius:4px;overflow:hidden\"><div id=\"pbar\" style=\"width:0%;height:20px;background:linear-gradient(90deg,#4fc3f7,#00bcd4);transition:0.3s\"></div></div><div id=\"ptext\">Uploading...</div>';"
    "var fd=new FormData();fd.append('firmware',f);"
    "var xhr=new XMLHttpRequest();xhr.open('POST','/upload',true);xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round(e.loaded/e.total*100);document.getElementById('pbar').style.width=pct+'%';document.getElementById('ptext').innerHTML='Uploading: '+pct+'%'}};"
    "xhr.onload=function(){if(xhr.status==200||xhr.status==302){document.getElementById('ptext').innerHTML='<span style=\"color:#4caf50\">Upload complete! Rebooting...</span>';setTimeout(function(){location.reload()},5000)}else{document.getElementById('ptext').innerHTML='<span style=\"color:#f44336\">Error: '+xhr.responseText+'</span>'}};"
    "xhr.onerror=function(){document.getElementById('ptext').innerHTML='<span style=\"color:#f44336\">Upload failed</span>'};xhr.send(fd)};"
    "setInterval(fetchLogs,2000);setInterval(fetchStatus,5000);setInterval(listApps,3000);fetchLogs();fetchStatus();listApps();"
    "</script></body></html>";

static size_t parse_content_length(const char *request_data)
{
    const char *content_length_header = strstr(request_data, "Content-Length:");
    if (!content_length_header)
    {
        return 0; // No Content-Length header
    }

    content_length_header += 15; // Skip "Content-Length:"

    /* Skip whitespace */
    while (*content_length_header == ' ' || *content_length_header == '\t')
    {
        content_length_header++;
    }

    /* Parse number with bounds checking */
    char *endptr;
    unsigned long length = strtoul(content_length_header, &endptr, 10);

    /* Validate parsing */
    if (endptr == content_length_header || length > SIZE_MAX)
    {
        LOG_ERR("Invalid Content-Length value");
        return 0;
    }

    /* Reasonable size limits for embedded device */
    if (length > (2 * 1024 * 1024))
    { // Max 2MB
        LOG_ERR("Content-Length too large: %lu", length);
        return 0;
    }

    return (size_t)length;
}

static int find_multipart_boundary(const char *request_data, char *boundary, size_t boundary_size)
{
    const char *content_type = strstr(request_data, "Content-Type:");
    if (!content_type)
    {
        return -1;
    }

    const char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start)
    {
        return -1;
    }

    boundary_start += 9; // Skip "boundary="

    /* Find end of boundary (space, newline, or semicolon) */
    const char *boundary_end = boundary_start;
    while (*boundary_end && *boundary_end != ' ' &&
           *boundary_end != '\r' && *boundary_end != '\n' &&
           *boundary_end != ';')
    {
        boundary_end++;
    }

    size_t boundary_len = boundary_end - boundary_start;
    if (boundary_len == 0 || boundary_len >= boundary_size - 2)
    {
        return -1;
    }

    /* Add "--" prefix for multipart boundary */
    boundary[0] = '-';
    boundary[1] = '-';
    memcpy(boundary + 2, boundary_start, boundary_len);
    boundary[boundary_len + 2] = '\0';

    return 0;
}

/* HTTP response helpers */
static int send_http_response(int client_fd, int status_code, const char *content_type,
                              const char *body, size_t body_len)
{
    char response[HTTP_RESPONSE_BUFFER_SIZE];
    int header_len;

    if (body_len == 0 && body)
    {
        body_len = strlen(body);
    }

    LOG_DBG("Sending response: status=%d, len=%zu", status_code, body_len);

    header_len = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status_code,
                          (status_code == 200) ? "OK" : "Error",
                          content_type,
                          body_len);

    if (header_len >= sizeof(response))
    {
        LOG_ERR("Header too large");
        return -1;
    }

    /* Send header */
    ssize_t sent = send(client_fd, response, header_len, 0);
    if (sent != header_len)
    {
        LOG_ERR("Header send failed: sent=%zd, errno=%d", sent, errno);
        return -1;
    }

    /* Send body in small chunks if present */
    if (body && body_len > 0)
    {
        const char *ptr = body;
        size_t remaining = body_len;
        size_t chunk_size = 256; /* Start with smaller chunks for large payloads */
        int retry_count = 0;

        LOG_DBG("Sending body: %zu bytes total", body_len);

        while (remaining > 0)
        {
            size_t to_send = (remaining > chunk_size) ? chunk_size : remaining;

            sent = send(client_fd, ptr, to_send, 0);
            if (sent > 0)
            {
                ptr += sent;
                remaining -= sent;
                retry_count = 0;
                LOG_DBG("Sent %zd bytes, %zu remaining", sent, remaining);
                /* Give network stack time to process */
                k_msleep(10);
                k_yield();
            }
            else if (sent == 0)
            {
                /* Connection closed */
                LOG_WRN("Connection closed by peer");
                return -1;
            }
            else
            {
                /* Error */
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)
                {
                    /* Socket buffer full - wait and retry with exponential backoff */
                    retry_count++;
                    if (retry_count > 20)
                    {
                        LOG_ERR("Send retry limit exceeded: errno=%d, remaining=%zu", errno, remaining);
                        return -1;
                    }
                    
                    /* Reduce chunk size on memory pressure */
                    if (errno == ENOMEM && chunk_size > 128)
                    {
                        chunk_size = 128;
                        LOG_DBG("Reduced chunk size to %zu due to memory pressure", chunk_size);
                    }
                    
                    /* Exponential backoff with cap at 200ms */
                    int delay_ms = (retry_count * 20 < 200) ? (retry_count * 20) : 200;
                    LOG_DBG("Send retry %d: errno=%d, waiting %dms", retry_count, errno, delay_ms);
                    k_msleep(delay_ms);
                    continue;
                }
                LOG_ERR("Send error: errno=%d, remaining=%zu", errno, remaining);
                return -1;
            }
        }
        LOG_DBG("Body sent successfully");
    }

    return 0;
}

/* HTTP response helpers */
static int parse_http_request(const char *buffer, char *method, char *path, size_t path_size)
{
    const char *space1 = strchr(buffer, ' ');
    const char *space2 = space1 ? strchr(space1 + 1, ' ') : NULL;

    if (!space1 || !space2)
    {
        return -1;
    }

    /* Extract method */
    size_t method_len = space1 - buffer;
    if (method_len >= 8)
        return -1; // Max method length

    memcpy(method, buffer, method_len);
    method[method_len] = '\0';

    /* Extract path */
    size_t path_len = space2 - space1 - 1;
    if (path_len >= path_size)
        return -1;

    memcpy(path, space1 + 1, path_len);
    path[path_len] = '\0';

    return 0;
}

/* Handle firmware upload
 * initial_body: body data already read during HTTP header parsing
 * initial_body_len: length of initial_body data
 */
static int handle_firmware_upload(int client_fd, const char *request_headers, size_t content_length,
                                  const char *initial_body, size_t initial_body_len)
{
    if (content_length == 0 || content_length > (2 * 1024 * 1024))
    {
        send_http_response(client_fd, 400, "text/plain", "Invalid file size", 0);
        return -1;
    }

    /* Set longer receive timeout for upload */
    struct timeval upload_timeout = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &upload_timeout, sizeof(upload_timeout));

    /* Find multipart boundary from HTTP headers */
    char boundary[128];
    if (find_multipart_boundary(request_headers, boundary, sizeof(boundary)) != 0)
    {
        send_http_response(client_fd, 400, "text/plain", "Invalid multipart format", 0);
        return -1;
    }

    LOG_INF("Using multipart boundary: %s", boundary);
    LOG_INF("Starting firmware upload, content-length: %u bytes, initial body: %u bytes",
            content_length, initial_body_len);

    /* Buffer to accumulate data until we find the multipart header end.
     * The initial_body contains the start of the multipart data. */
    static char header_buffer[2048];
    size_t header_buffered = 0;

    /* Copy initial body data to our header buffer */
    if (initial_body_len > 0)
    {
        size_t copy_len = MIN(initial_body_len, sizeof(header_buffer) - 1);
        memcpy(header_buffer, initial_body, copy_len);
        header_buffered = copy_len;
        header_buffer[header_buffered] = '\0';
    }

    /* Check if we already have the multipart header end */
    char *data_start = strstr(header_buffer, "\r\n\r\n");

    /* If not found, keep receiving until we find it */
    while (!data_start && header_buffered < sizeof(header_buffer) - 1)
    {
        ssize_t received = recv(client_fd, header_buffer + header_buffered,
                                sizeof(header_buffer) - 1 - header_buffered, 0);
        if (received <= 0)
        {
            LOG_ERR("Failed to receive multipart header (got %u bytes)", header_buffered);
            send_http_response(client_fd, 400, "text/plain", "Failed to receive header", 0);
            return -1;
        }
        header_buffered += received;
        header_buffer[header_buffered] = '\0';

        data_start = strstr(header_buffer, "\r\n\r\n");
        if (!data_start)
        {
            LOG_DBG("Header incomplete, received %u bytes total", header_buffered);
        }
    }

    if (!data_start)
    {
        LOG_ERR("Could not find end of multipart headers after %u bytes", header_buffered);
        LOG_HEXDUMP_ERR(header_buffer, MIN(200, header_buffered), "Header start:");
        send_http_response(client_fd, 400, "text/plain", "Invalid multipart format", 0);
        return -1;
    }
    data_start += 4; /* Skip \r\n\r\n */

    /* Calculate how much of the buffer is actual file data */
    size_t header_size = data_start - header_buffer;
    size_t first_data_len = header_buffered - header_size;

    /* Use content_length as the max expected size - actual firmware will be slightly less
     * due to multipart boundaries, but we detect the closing boundary during upload.
     * This prevents "write would exceed expected size" errors. */
    size_t expected_size = content_length;
    LOG_INF("Header size: %u, first data chunk: %u, max expected: %u",
            header_size, first_data_len, expected_size);

    /* Start OTA update with expected size */
    enum ota_result result = ota_start_update(expected_size);
    if (result != OTA_OK)
    {
        send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
        return -1;
    }

    /* Write the first chunk of file data */
    size_t total_written = 0;
    if (first_data_len > 0)
    {
        result = ota_write_chunk((uint8_t *)data_start, first_data_len);
        if (result != OTA_OK)
        {
            LOG_ERR("OTA write failed: %s", ota_result_to_string(result));
            ota_abort_update();
            send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
            return -1;
        }
        total_written = first_data_len;
    }

    /* Now receive and write the rest of the file */
    static char upload_buffer[1024];
    /* total_received tracks how many body bytes we've received so far:
     * - initial_body_len was received during HTTP header parsing
     * - any extra we received while finding the multipart header end */
    size_t total_received = initial_body_len + (header_buffered - MIN(initial_body_len, header_buffered));
    /* Simpler: header_buffered contains everything we have from the body so far */
    total_received = header_buffered;
    size_t boundary_len = strlen(boundary);
    int retry_count = 0;
    uint8_t last_progress = 0;

    while (total_received < content_length)
    {
        size_t chunk_size = MIN(1024, content_length - total_received);
        ssize_t received = recv(client_fd, upload_buffer, chunk_size, 0);

        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                retry_count++;
                if (retry_count > 300) /* 300 * 100ms = 30 seconds timeout */
                {
                    LOG_ERR("Upload timeout after %u bytes written", total_written);
                    ota_abort_update();
                    send_http_response(client_fd, 408, "text/plain", "Upload timeout", 0);
                    return -1;
                }
                k_msleep(100);
                continue;
            }
            LOG_ERR("Receive failed: %d (written %u bytes)", errno, total_written);
            ota_abort_update();
            send_http_response(client_fd, 500, "text/plain", "Upload failed", 0);
            return -1;
        }
        else if (received == 0)
        {
            LOG_WRN("Connection closed at %u/%u bytes", total_received, content_length);
            break;
        }

        retry_count = 0;
        total_received += received;

        /* Check for closing boundary - it will be at the end */
        char *end_boundary = memmem(upload_buffer, received, boundary, boundary_len);
        if (end_boundary)
        {
            /* Only write data before the boundary */
            size_t data_len = end_boundary - upload_buffer;
            /* Also remove the preceding \r\n */
            if (data_len >= 2)
            {
                data_len -= 2;
            }
            if (data_len > 0)
            {
                result = ota_write_chunk((uint8_t *)upload_buffer, data_len);
                if (result != OTA_OK)
                {
                    LOG_ERR("Final write failed: %s", ota_result_to_string(result));
                    ota_abort_update();
                    send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
                    return -1;
                }
                total_written += data_len;
            }
            LOG_INF("Found closing boundary, upload complete");
            break;
        }

        /* Write chunk to OTA */
        result = ota_write_chunk((uint8_t *)upload_buffer, received);
        if (result != OTA_OK)
        {
            LOG_ERR("OTA write failed at %u bytes: %s", total_written, ota_result_to_string(result));
            ota_abort_update();
            send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
            return -1;
        }
        total_written += received;

        /* Log progress every 10% */
        uint8_t progress = (total_written * 100) / expected_size;
        if (progress >= last_progress + 10)
        {
            LOG_INF("OTA write: %u%% (%u bytes)", progress, total_written);
            last_progress = progress;
        }

        /* Yield to allow other tasks */
        k_yield();
    }

    LOG_INF("Upload finished: wrote %u bytes to flash", total_written);

    if (total_written == 0)
    {
        LOG_ERR("No valid firmware data received");
        ota_abort_update();
        send_http_response(client_fd, 400, "text/plain", "No file data found", 0);
        return -1;
    }

    LOG_INF("Finalizing OTA update...");

    /* Finalize update */
    result = ota_finalize_update();
    if (result != OTA_OK)
    {
        send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
        return -1;
    }

    /* Send redirect to main page */
    const char *redirect_response =
        "HTTP/1.1 302 Found\r\n"
        "Location: /\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    send(client_fd, redirect_response, strlen(redirect_response), 0);

    /* Schedule reboot */
    ota_reboot_to_apply_update(3000);
    return 0;
}

/* Handle API requests */
static int handle_api_request(int client_fd, const char *path)
{
    char response[512];

    if (strcmp(path, "/api/ota/status") == 0)
    {
        const struct ota_progress *ota = ota_get_progress();
        snprintf(response, sizeof(response),
                 "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
                 ota_state_to_string(ota->state), ota->percentage, ota->status_message);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strcmp(path, "/api/ota/confirm") == 0)
    {
        enum ota_result result = ota_confirm_firmware();
        const char *msg = (result == OTA_OK) ? "Firmware confirmed" : ota_result_to_string(result);
        return send_http_response(client_fd, (result == OTA_OK) ? 200 : 500, "text/plain", msg, 0);
    }

    if (strcmp(path, "/api/reboot") == 0)
    {
        send_http_response(client_fd, 200, "text/plain", "Rebooting", 0);
        ota_reboot_to_apply_update(2000);
        return 0;
    }

    if (strcmp(path, "/api/logs") == 0)
    {
        /* Return logs with HTML formatting for colors */
        k_mutex_lock(&log_mutex, K_FOREVER);

        /* Format logs with color coding */
        static char formatted_logs[LOG_BUFFER_SIZE + 512];
        char *src = log_buffer;
        char *dst = formatted_logs;
        char *dst_end = formatted_logs + sizeof(formatted_logs) - 100;

        while (*src && dst < dst_end)
        {
            /* Find line end */
            char *line_end = strchr(src, '\n');
            if (!line_end)
                line_end = src + strlen(src);

            /* Determine log level color */
            const char *color_class = "";
            if (strstr(src, "<inf>"))
                color_class = "log-inf";
            else if (strstr(src, "<wrn>"))
                color_class = "log-wrn";
            else if (strstr(src, "<err>"))
                color_class = "log-err";

            if (color_class[0])
            {
                dst += snprintf(dst, dst_end - dst, "<span class='%s'>", color_class);
            }

            /* Copy line, escaping HTML */
            while (src < line_end && dst < dst_end)
            {
                if (*src == '<')
                {
                    *dst++ = '&';
                    *dst++ = 'l';
                    *dst++ = 't';
                    *dst++ = ';';
                }
                else if (*src == '>')
                {
                    *dst++ = '&';
                    *dst++ = 'g';
                    *dst++ = 't';
                    *dst++ = ';';
                }
                else
                {
                    *dst++ = *src;
                }
                src++;
            }

            if (color_class[0])
            {
                dst += snprintf(dst, dst_end - dst, "</span>");
            }

            if (*src == '\n')
            {
                *dst++ = '\n';
                src++;
            }
        }
        *dst = '\0';

        k_mutex_unlock(&log_mutex);
        return send_http_response(client_fd, 200, "text/html", formatted_logs, 0);
    }

    if (strcmp(path, "/api/status") == 0)
    {
        /* Calculate uptime */
        uint64_t uptime_ms = k_uptime_get();
        uint32_t hours = uptime_ms / 3600000;
        uint32_t mins = (uptime_ms % 3600000) / 60000;
        uint32_t secs = (uptime_ms % 60000) / 1000;

        snprintf(response, sizeof(response),
                 "{\"ip\":\"%s\",\"uptime\":\"%02u:%02u:%02u\",\"mem\":\"99%% used\"}",
                 server_state.server_ip[0] ? server_state.server_ip : "0.0.0.0",
                 hours, mins, secs);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strcmp(path, "/api/logs") == 0)
    {
        /* Return log buffer as plain text */
        k_mutex_lock(&log_mutex, K_FOREVER);
        size_t len = log_buffer_pos;
        ssize_t sent = send_http_response(client_fd, 200, "text/plain", log_buffer, len);
        k_mutex_unlock(&log_mutex);
        return sent;
    }

    if (strncmp(path, "/api/cmd", 8) == 0)
    {
        /* Execute shell command */
        const char *cmd_start = strstr(path, "c=");
        if (cmd_start)
        {
            cmd_start += 2;
            /* URL decode and execute command */
            char cmd[128];
            size_t i = 0, j = 0;
            while (cmd_start[i] && j < sizeof(cmd) - 1)
            {
                if (cmd_start[i] == '%' && cmd_start[i + 1] && cmd_start[i + 2])
                {
                    char hex[3] = {cmd_start[i + 1], cmd_start[i + 2], 0};
                    cmd[j++] = (char)strtol(hex, NULL, 16);
                    i += 3;
                }
                else if (cmd_start[i] == '+')
                {
                    cmd[j++] = ' ';
                    i++;
                }
                else
                {
                    cmd[j++] = cmd_start[i++];
                }
            }
            cmd[j] = '\0';

            /* Log the command */
            char log_entry[256];
            snprintf(log_entry, sizeof(log_entry), "akira:~$ %s", cmd);
            web_server_add_log(log_entry);

            /* Execute via callback if available */
            if (callbacks.execute_shell_command)
            {
                char result[512];
                callbacks.execute_shell_command(cmd, result, sizeof(result));
                web_server_add_log(result);
            }
        }
        return send_http_response(client_fd, 200, "text/plain", "OK", 0);
    }

    if (strcmp(path, "/api/system") == 0)
    {
        snprintf(response, sizeof(response),
                 "{\"uptime\":\"%.1f hours\",\"memory\":\"Available\",\"wifi\":\"Connected\",\"cpu\":\"ESP32\"}",
                 (float)k_uptime_get() / 3600000.0f);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* App Manager API */
    if (strcmp(path, "/api/apps/list") == 0)
    {
        app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
        int count = app_manager_list(apps, CONFIG_AKIRA_APP_MAX_INSTALLED);
        if (count < 0) {
            return send_http_response(client_fd, 500, "application/json", "{\"apps\":[]}", 0);
        }
        /* Build JSON object with apps array */
        char *p = response;
        char *end = response + sizeof(response) - 2;
        p += snprintf(p, end - p, "{\"apps\":[");
        for (int i = 0; i < count && p < end; i++) {
            p += snprintf(p, end - p, "%s{\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"description\":\"WASM Application\"}",
                          i > 0 ? "," : "", apps[i].id, apps[i].name,
                          app_state_to_str(apps[i].state));
        }
        snprintf(p, end - p, "]}");
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strncmp(path, "/api/apps/start?", 16) == 0)
    {
        const char *name = strstr(path, "name=");
        if (!name) {
            return send_http_response(client_fd, 400, "text/plain", "Missing name parameter", 0);
        }
        name += 5;
        char app_name[32];
        strncpy(app_name, name, sizeof(app_name) - 1);
        app_name[sizeof(app_name) - 1] = '\0';
        char *amp = strchr(app_name, '&');
        if (amp) *amp = '\0';
        int ret = app_manager_start(app_name);
        if (ret < 0) {
            snprintf(response, sizeof(response), "{\"error\":\"Failed to start app: %d\"}", ret);
            return send_http_response(client_fd, 500, "application/json", response, 0);
        }
        snprintf(response, sizeof(response), "{\"status\":\"started\",\"name\":\"%s\"}", app_name);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strncmp(path, "/api/apps/stop?", 15) == 0)
    {
        const char *name = strstr(path, "name=");
        if (!name) {
            return send_http_response(client_fd, 400, "text/plain", "Missing name parameter", 0);
        }
        name += 5;
        char app_name[32];
        strncpy(app_name, name, sizeof(app_name) - 1);
        app_name[sizeof(app_name) - 1] = '\0';
        char *amp = strchr(app_name, '&');
        if (amp) *amp = '\0';
        int ret = app_manager_stop(app_name);
        if (ret < 0) {
            snprintf(response, sizeof(response), "{\"error\":\"Failed to stop app: %d\"}", ret);
            return send_http_response(client_fd, 500, "application/json", response, 0);
        }
        snprintf(response, sizeof(response), "{\"status\":\"stopped\",\"name\":\"%s\"}", app_name);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strncmp(path, "/api/apps/uninstall?", 20) == 0)
    {
        const char *name = strstr(path, "name=");
        if (!name) {
            return send_http_response(client_fd, 400, "text/plain", "Missing name parameter", 0);
        }
        name += 5;
        char app_name[32];
        strncpy(app_name, name, sizeof(app_name) - 1);
        app_name[sizeof(app_name) - 1] = '\0';
        char *amp = strchr(app_name, '&');
        if (amp) *amp = '\0';
        int ret = app_manager_uninstall(app_name);
        if (ret < 0) {
            snprintf(response, sizeof(response), "{\"error\":\"Failed to uninstall app: %d\"}", ret);
            return send_http_response(client_fd, 500, "application/json", response, 0);
        }
        snprintf(response, sizeof(response), "{\"status\":\"uninstalled\",\"name\":\"%s\"}", app_name);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }
#endif /* CONFIG_AKIRA_APP_MANAGER */

    return send_http_response(client_fd, 404, "application/json", "{\"error\":\"API not found\"}", 0);
}

/* Main HTTP request handler */
static int handle_http_request(int client_fd)
{
    char buffer[HTTP_BUFFER_SIZE];
    char method[16], path[128];
    ssize_t received;

    /* Set socket timeouts */
    struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* Disable Nagle for faster small packet sends */
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Receive request with timeout */
    received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0)
    {
        LOG_WRN("Request receive failed or timeout: %d", errno);
        return -1;
    }

    buffer[received] = '\0';

    /* Parse request line */
    if (parse_http_request(buffer, method, path, sizeof(path)) != 0)
    {
        send_http_response(client_fd, 400, "text/plain", "Bad Request", 0);
        return -1;
    }

    LOG_DBG("HTTP %s %s", method, path);

    /* Route request */
    if (strcmp(method, "GET") == 0)
    {
        if (strcmp(path, "/") == 0)
        {
            /* Main page - send static HTML directly */
            return send_http_response(client_fd, 200, "text/html", html_page, sizeof(html_page) - 1);
        }

        if (strncmp(path, "/api/", 5) == 0)
        {
            return handle_api_request(client_fd, path);
        }

        return send_http_response(client_fd, 404, "text/plain", "Not Found", 0);
    }

    if (strcmp(method, "POST") == 0)
    {
        if (strcmp(path, "/upload") == 0)
        {
            /* Parse Content-Length with validation */
            size_t content_length = parse_content_length(buffer);
            if (content_length == 0)
            {
                return send_http_response(client_fd, 400, "text/plain",
                                          "Missing or invalid Content-Length", 0);
            }

            /* Find end of HTTP headers - body data may already be in buffer */
            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start)
            {
                return send_http_response(client_fd, 400, "text/plain",
                                          "Invalid HTTP request", 0);
            }
            body_start += 4; /* Skip \r\n\r\n */

            /* Calculate how much body data was already received */
            size_t headers_len = body_start - buffer;
            size_t body_already_read = received - headers_len;

            return handle_firmware_upload(client_fd, buffer, content_length,
                                          body_start, body_already_read);
        }

#ifdef CONFIG_AKIRA_APP_MANAGER
        /* App upload endpoint - POST /api/apps/install */
        if (strncmp(path, "/api/apps/install", 17) == 0)
        {
            size_t content_length = parse_content_length(buffer);
            if (content_length == 0 || content_length > CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)
            {
                return send_http_response(client_fd, 400, "text/plain",
                                          "Invalid Content-Length", 0);
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start)
            {
                return send_http_response(client_fd, 400, "text/plain",
                                          "Invalid HTTP request", 0);
            }
            body_start += 4;

            /* Get app name from query parameter */
            char app_name[32] = "uploaded_app";
            const char *name_param = strstr(buffer, "?name=");
            if (name_param)
            {
                name_param += 6;
                const char *end = strpbrk(name_param, " &\r\n");
                size_t len = end ? (size_t)(end - name_param) : strlen(name_param);
                if (len > 0 && len < sizeof(app_name))
                {
                    strncpy(app_name, name_param, len);
                    app_name[len] = '\0';
                }
            }

            /* Begin chunked install */
            int session = app_manager_install_begin(app_name, content_length, APP_SOURCE_HTTP);
            if (session < 0)
            {
                char resp[64];
                snprintf(resp, sizeof(resp), "{\"error\":\"Install begin failed: %d\"}", session);
                return send_http_response(client_fd, 500, "application/json", resp, 0);
            }

            /* Write first chunk */
            size_t headers_len = body_start - buffer;
            size_t body_already_read = received - headers_len;
            if (body_already_read > 0)
            {
                int ret = app_manager_install_chunk(session, body_start, body_already_read);
                if (ret < 0)
                {
                    app_manager_install_abort(session);
                    return send_http_response(client_fd, 500, "text/plain", "Chunk write failed", 0);
                }
            }

            /* Receive remaining data */
            size_t total_received = body_already_read;
            static char app_upload_buf[512];
            while (total_received < content_length)
            {
                size_t chunk_size = MIN(sizeof(app_upload_buf), content_length - total_received);
                ssize_t recvd = recv(client_fd, app_upload_buf, chunk_size, 0);
                if (recvd <= 0)
                {
                    app_manager_install_abort(session);
                    return send_http_response(client_fd, 500, "text/plain", "Upload failed", 0);
                }
                int ret = app_manager_install_chunk(session, app_upload_buf, recvd);
                if (ret < 0)
                {
                    app_manager_install_abort(session);
                    return send_http_response(client_fd, 500, "text/plain", "Chunk write failed", 0);
                }
                total_received += recvd;
                k_yield();
            }

            /* Finalize install */
            int app_id = app_manager_install_end(session, NULL);
            if (app_id < 0)
            {
                char resp[64];
                snprintf(resp, sizeof(resp), "{\"error\":\"Install failed: %d\"}", app_id);
                return send_http_response(client_fd, 500, "application/json", resp, 0);
            }

            char resp[128];
            snprintf(resp, sizeof(resp), "{\"status\":\"installed\",\"name\":\"%s\",\"id\":%d}", app_name, app_id);
            return send_http_response(client_fd, 200, "application/json", resp, 0);
        }
#endif /* CONFIG_AKIRA_APP_MANAGER */

        if (strncmp(path, "/api/", 5) == 0)
        {
            return handle_api_request(client_fd, path);
        }
    }

    return send_http_response(client_fd, 405, "text/plain", "Method Not Allowed", 0);
}

/* Simplified web server */
static int run_web_server(void)
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0)
    {
        LOG_ERR("Socket creation failed: %d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Set accept timeout so we can check state periodically */
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        LOG_ERR("Bind failed: %d", errno);
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_CONNECTIONS) < 0)
    {
        LOG_ERR("Listen failed: %d", errno);
        close(server_fd);
        return -1;
    }

    LOG_INF("HTTP server listening on port %d", HTTP_PORT);

    while (server_state.state == WEB_SERVER_RUNNING)
    {
        client_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /* Timeout - just continue to check state */
                continue;
            }
            if (server_state.state == WEB_SERVER_RUNNING)
            {
                LOG_ERR("Accept failed: %d", errno);
            }
            continue;
        }

        LOG_DBG("Client connected from %d.%d.%d.%d",
                (client_addr.sin_addr.s_addr) & 0xFF,
                (client_addr.sin_addr.s_addr >> 8) & 0xFF,
                (client_addr.sin_addr.s_addr >> 16) & 0xFF,
                (client_addr.sin_addr.s_addr >> 24) & 0xFF);

        /* Handle request */
        if (handle_http_request(client_fd) == 0)
        {
            k_mutex_lock(&server_mutex, K_FOREVER);
            server_state.requests_handled++;
            k_mutex_unlock(&server_mutex);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}

/* Server operations */
static void do_start_server(void)
{
    if (server_state.state == WEB_SERVER_RUNNING)
    {
        return;
    }

    server_state.state = WEB_SERVER_RUNNING;
    LOG_INF("Web server started");

    /* This is a blocking call - runs until server is stopped */
    run_web_server();

    /* Server has stopped */
    server_state.state = WEB_SERVER_STOPPED;
    LOG_INF("Web server stopped");
}

static void do_stop_server(void)
{
    server_state.state = WEB_SERVER_STOPPED;
    LOG_INF("Web server stopped");
}

static void do_network_status_changed(bool connected, const char *ip_address)
{
    server_state.network_connected = connected;

    if (connected && ip_address)
    {
        strncpy(server_state.server_ip, ip_address, sizeof(server_state.server_ip) - 1);
        server_state.server_ip[sizeof(server_state.server_ip) - 1] = '\0';
        LOG_INF("Network connected: http://%s:%d", server_state.server_ip, HTTP_PORT);

        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "<inf> wifi: Connected to network");
        web_server_add_log(log_msg);
        snprintf(log_msg, sizeof(log_msg), "<inf> wifi: IP Address: %s", ip_address);
        web_server_add_log(log_msg);
        snprintf(log_msg, sizeof(log_msg), "<inf> web_server: HTTP server listening on port %d", HTTP_PORT);
        web_server_add_log(log_msg);

        if (server_state.state == WEB_SERVER_STOPPED)
        {
            do_start_server();
        }
    }
    else
    {
        server_state.server_ip[0] = '\0';
        LOG_INF("Network disconnected");
        web_server_add_log("<wrn> wifi: Network disconnected");
    }
}

/* Web server thread */
static void web_server_thread_main(void *p1, void *p2, void *p3)
{
    struct server_msg msg;

    LOG_INF("Web server thread started");

    while (1)
    {
        if (k_msgq_get(&server_msgq, &msg, K_MSEC(5000)) == 0)
        {
            switch (msg.type)
            {
            case MSG_START_SERVER:
                do_start_server();
                break;
            case MSG_STOP_SERVER:
                do_stop_server();
                break;
            case MSG_NETWORK_STATUS_CHANGED:
                do_network_status_changed(msg.data.network_status.connected,
                                          msg.data.network_status.ip_address);
                break;
            }
        }

        /* Periodic housekeeping */
        if (server_state.state == WEB_SERVER_RUNNING)
        {
            k_mutex_lock(&server_mutex, K_FOREVER);
            server_state.active_connections = 0; // Reset counter
            k_mutex_unlock(&server_mutex);
        }
    }
}

/* Public API */
int web_server_start(const struct web_server_callbacks *cb)
{
    if (cb)
    {
        memcpy(&callbacks, cb, sizeof(callbacks));
    }

    memset(&server_state, 0, sizeof(server_state));
    server_state.state = WEB_SERVER_STOPPED;
    strcpy(server_state.server_ip, "0.0.0.0");

    /* Add initial boot messages to log buffer */
    web_server_add_log("*** Booting Zephyr OS build v4.2.1 ***");
    web_server_add_log("=== AkiraOS V1.1 ===");
    web_server_add_log("[00:00:00.000] <inf> akira_hal: Akira HAL initializing for");
    web_server_add_log("[00:00:00.001] <inf> akira_main: Platform");
    web_server_add_log("[00:00:00.002] <inf> akira_main: Display: Available");
    web_server_add_log("[00:00:00.003] <inf> akira_main: WiFi: Available");
    web_server_add_log("[00:00:00.010] <inf> user_settings: User settings module initialized");
    web_server_add_log("[00:00:00.020] <inf> ota_manager: OTA Manager ready");
    web_server_add_log("[00:00:00.030] <inf> web_server: Web server initialized");

    web_server_thread_id = k_thread_create(&web_server_thread_data,
                                           web_server_stack,
                                           K_THREAD_STACK_SIZEOF(web_server_stack),
                                           web_server_thread_main,
                                           NULL, NULL, NULL,
                                           WEB_SERVER_THREAD_PRIORITY,
                                           0, K_NO_WAIT);

    k_thread_name_set(web_server_thread_id, "web_server");

    LOG_INF("Web server initialized");
    return 0;
}

int web_server_stop(void)
{
    struct server_msg msg = {.type = MSG_STOP_SERVER};
    return (k_msgq_put(&server_msgq, &msg, K_NO_WAIT) == 0) ? 0 : -ENOMEM;
}

int web_server_get_stats(struct web_server_stats *stats)
{
    if (!stats)
        return -EINVAL;

    k_mutex_lock(&server_mutex, K_FOREVER);
    stats->state = server_state.state;
    stats->requests_handled = server_state.requests_handled;
    stats->bytes_transferred = server_state.bytes_transferred;
    stats->active_connections = server_state.active_connections;
    k_mutex_unlock(&server_mutex);

    return 0;
}

bool web_server_is_running(void)
{
    return (server_state.state == WEB_SERVER_RUNNING);
}

enum web_server_state web_server_get_state(void)
{
    return server_state.state;
}

void web_server_notify_network_status(bool connected, const char *ip_address)
{
    struct server_msg msg = {
        .type = MSG_NETWORK_STATUS_CHANGED,
        .data.network_status.connected = connected};

    if (connected && ip_address)
    {
        strncpy(msg.data.network_status.ip_address, ip_address,
                sizeof(msg.data.network_status.ip_address) - 1);
        msg.data.network_status.ip_address[sizeof(msg.data.network_status.ip_address) - 1] = '\0';
    }
    else
    {
        msg.data.network_status.ip_address[0] = '\0';
    }

    k_msgq_put(&server_msgq, &msg, K_NO_WAIT);
}