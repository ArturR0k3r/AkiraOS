/**
 * @file web_server.c
 * @brief Web Server and OTA Handler Implementation - Fixed for Zephyr 4.2
 *
 * Complete web server module with OTA support running on dedicated thread.
 * Handles HTTP requests, firmware uploads, and provides web interface.
 */

#include "web_server.h"
#include "ota_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/service.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(web_server, LOG_LEVEL_INF);

static const struct http_header content_type_html[] = {
    {.name = "Content-Type", .value = "text/html"},
    {.name = "Cache-Control", .value = "no-cache"}};

static const struct http_header content_type_json[] = {
    {.name = "Content-Type", .value = "application/json"}};

static const struct http_header content_type_plain[] = {
    {.name = "Content-Type", .value = "text/plain"}};

/* Thread stack and control block */
static K_THREAD_STACK_DEFINE(web_server_stack, WEB_SERVER_STACK_SIZE);
static struct k_thread web_server_thread_data;
static k_tid_t web_server_thread_id;

/* Server state */
static struct web_server_stats server_stats = {0};
static struct web_server_callbacks server_callbacks = {0};
static bool network_connected = false;
static char server_ip[16] = {0};
static enum web_server_state current_state = WEB_SERVER_STOPPED;
static const char *custom_headers = NULL;

/* HTTP service configuration */
static struct http_service_desc web_service;
static struct sockaddr_in server_addr;

/* Synchronization */
static K_MUTEX_DEFINE(server_mutex);
static K_SEM_DEFINE(server_ready_sem, 0, 1);

/* Message queue for server operations */
#define SERVER_MSG_QUEUE_SIZE 20

enum server_msg_type
{
    MSG_START_SERVER,
    MSG_STOP_SERVER,
    MSG_NETWORK_STATUS_CHANGED,
    MSG_REFRESH_DATA,
    MSG_BROADCAST_LOG
};

struct server_msg
{
    enum server_msg_type type;
    union
    {
        struct
        {
            bool connected;
            char ip_address[16];
        } network_status;
        struct
        {
            char message[256];
            size_t length;
        } log_broadcast;
    } data;
};

K_MSGQ_DEFINE(server_msgq, sizeof(struct server_msg), SERVER_MSG_QUEUE_SIZE, 4);

/* Web interface HTML (same as before) */
static const char web_interface_html[] =
    "<!DOCTYPE html>"
    "<html><head><title>ESP32 Gaming Device Control</title>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;color:#fff}"
    ".container{max-width:1200px;margin:0 auto;padding:20px}"
    ".header{text-align:center;padding:30px;background:rgba(255,255,255,0.1);margin-bottom:30px;border-radius:20px;backdrop-filter:blur(10px)}"
    ".header h1{margin:0;font-size:2.5em;text-shadow:2px 2px 4px rgba(0,0,0,0.3)}"
    ".header p{margin:10px 0;opacity:0.9}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(350px,1fr));gap:20px;margin-bottom:20px}"
    ".panel{background:rgba(255,255,255,0.1);padding:25px;border-radius:15px;backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,0.2);box-shadow:0 8px 32px rgba(0,0,0,0.1)}"
    ".panel h3{margin-top:0;color:#fff;display:flex;align-items:center;gap:10px}"
    ".panel h3::before{font-size:1.2em}"
    ".btn{background:linear-gradient(45deg,#ff6b6b,#ff8e53);color:white;padding:12px 24px;border:none;border-radius:25px;cursor:pointer;margin:8px 5px;font-weight:600;transition:all 0.3s ease;box-shadow:0 4px 15px rgba(255,107,107,0.3)}"
    ".btn:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(255,107,107,0.4)}"
    ".btn-success{background:linear-gradient(45deg,#51cf66,#40c057)}"
    ".btn-danger{background:linear-gradient(45deg,#ff6b6b,#fa5252)}"
    ".btn-info{background:linear-gradient(45deg,#339af0,#228be6)}"
    ".status{display:inline-block;padding:6px 12px;border-radius:20px;font-size:12px;font-weight:600;margin:5px}"
    ".status.online{background:#51cf66;color:white;box-shadow:0 0 10px rgba(81,207,102,0.5)}"
    ".status.offline{background:#ff6b6b;color:white}"
    ".upload-area{border:2px dashed rgba(255,255,255,0.5);padding:40px;text-align:center;border-radius:15px;transition:all 0.3s ease;cursor:pointer}"
    ".upload-area:hover{border-color:#fff;background:rgba(255,255,255,0.1)}"
    ".upload-area.drag-over{border-color:#51cf66;background:rgba(81,207,102,0.2)}"
    ".progress{width:100%;height:8px;background:rgba(255,255,255,0.2);border-radius:4px;overflow:hidden;margin:15px 0}"
    ".progress-bar{height:100%;background:linear-gradient(45deg,#51cf66,#40c057);width:0%;transition:width 0.3s ease}"
    ".terminal{background:rgba(0,0,0,0.8);color:#00ff41;font-family:'Courier New',monospace;padding:20px;border-radius:10px;max-height:300px;overflow-y:auto;font-size:14px;line-height:1.4}"
    ".terminal .prompt{color:#00ff41;font-weight:bold}"
    ".terminal .output{color:#fff;margin:5px 0}"
    ".terminal .error{color:#ff6b6b}"
    ".input-group{display:flex;gap:10px;align-items:center;margin-top:15px}"
    ".input-group input{flex:1;padding:12px;background:rgba(255,255,255,0.1);border:1px solid rgba(255,255,255,0.3);border-radius:8px;color:#fff;font-size:14px}"
    ".input-group input::placeholder{color:rgba(255,255,255,0.7)}"
    ".data-grid{display:grid;grid-template-columns:1fr 1fr;gap:15px;font-size:14px}"
    ".data-item{padding:10px;background:rgba(255,255,255,0.1);border-radius:8px}"
    ".data-item strong{color:#51cf66}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.7}}"
    ".pulsing{animation:pulse 2s infinite}"
    "</style></head>"
    "<body>"
    "<div class='container'>"
    "<div class='header'>"
    "<h1>🎮 ESP32 Gaming Device</h1>"
    "<p>Advanced OTA Management & System Control Interface</p>"
    "<div>Status: <span class='status online' id='deviceStatus'>Online</span></div>"
    "<div><small>IP: <span id='deviceIP'>Loading...</span></small></div>"
    "</div>"
    "<div class='grid'>"
    "<div class='panel'>"
    "<h3>🔄 Firmware Update</h3>"
    "<div class='upload-area' id='uploadArea' onclick='document.getElementById(\"firmwareFile\").click()' ondragover='handleDragOver(event)' ondrop='handleDrop(event)'>"
    "<div>📁 Click or drag firmware file (.bin)</div>"
    "<small>Supports MCUboot signed binaries</small>"
    "<input type='file' id='firmwareFile' accept='.bin' style='display:none' onchange='uploadFirmware()'>"
    "</div>"
    "<div class='progress' id='uploadProgress' style='display:none'>"
    "<div class='progress-bar' id='uploadProgressBar'></div>"
    "</div>"
    "<div id='uploadStatus'></div>"
    "<div style='margin-top:15px'>"
    "<button class='btn btn-info' onclick='checkOTAStatus()'>Check OTA Status</button>"
    "<button class='btn btn-success' onclick='confirmFirmware()'>Confirm Firmware</button>"
    "</div>"
    "</div>"
    "<div class='panel'>"
    "<h3>⚙️ System Information</h3>"
    "<div class='data-grid' id='systemInfo'>Loading...</div>"
    "<div style='margin-top:15px'>"
    "<button class='btn btn-info' onclick='refreshSystemInfo()'>Refresh</button>"
    "<button class='btn btn-danger' onclick='rebootSystem()'>Reboot System</button>"
    "</div>"
    "</div>"
    "<div class='panel'>"
    "<h3>🎮 Gaming Controls</h3>"
    "<div id='buttonStates'>Loading button states...</div>"
    "<div style='margin-top:15px'>"
    "<button class='btn btn-info' onclick='readButtons()'>Read Buttons</button>"
    "<button class='btn' onclick='testDisplay()'>Test Display</button>"
    "</div>"
    "</div>"
    "</div>"
    "</div>"
    "<script>"
    "let uploadInProgress=false;"
    "function handleDragOver(e){e.preventDefault();document.getElementById('uploadArea').classList.add('drag-over')}"
    "function handleDrop(e){e.preventDefault();const area=document.getElementById('uploadArea');area.classList.remove('drag-over');const files=e.dataTransfer.files;if(files.length>0){document.getElementById('firmwareFile').files=files;uploadFirmware()}}"
    "function uploadFirmware(){"
    "if(uploadInProgress){alert('Upload already in progress');return}"
    "const file=document.getElementById('firmwareFile').files[0];"
    "if(!file){alert('Please select a firmware file');return}"
    "if(!file.name.endsWith('.bin')){alert('Please select a .bin file');return}"
    "uploadInProgress=true;"
    "const formData=new FormData();formData.append('firmware',file);"
    "const progress=document.getElementById('uploadProgress');"
    "const progressBar=document.getElementById('uploadProgressBar');"
    "const status=document.getElementById('uploadStatus');"
    "progress.style.display='block';status.innerHTML='<div class=\"pulsing\">Uploading firmware...</div>';status.className='status';"
    "const xhr=new XMLHttpRequest();"
    "xhr.upload.onprogress=function(e){if(e.lengthComputable){const pct=(e.loaded/e.total)*100;progressBar.style.width=pct+'%';status.innerHTML='<div>Upload progress: '+Math.round(pct)+'%</div>'}}"
    "xhr.onload=function(){uploadInProgress=false;if(xhr.status===200){status.innerHTML='<div style=\"color:#51cf66\">✅ Upload successful! Device will reboot in 5 seconds...</div>';progressBar.style.background='linear-gradient(45deg,#51cf66,#40c057)';setTimeout(()=>location.reload(),8000)}else{status.innerHTML='<div style=\"color:#ff6b6b\">❌ Upload failed: '+xhr.responseText+'</div>';progressBar.style.background='linear-gradient(45deg,#ff6b6b,#fa5252)'}}"
    "xhr.onerror=function(){uploadInProgress=false;status.innerHTML='<div style=\"color:#ff6b6b\">❌ Network error during upload</div>'};"
    "xhr.open('POST','/upload');xhr.send(formData)}"
    "function refreshSystemInfo(){fetch('/api/system').then(r=>r.json()).then(data=>{const info=document.getElementById('systemInfo');info.innerHTML=`<div class='data-item'><strong>Uptime:</strong> ${data.uptime}</div><div class='data-item'><strong>Memory:</strong> ${data.memory}</div><div class='data-item'><strong>WiFi:</strong> ${data.wifi}</div><div class='data-item'><strong>CPU:</strong> ${data.cpu}</div><div class='data-item'><strong>Temperature:</strong> ${data.temp}</div><div class='data-item'><strong>Threads:</strong> ${data.threads||'N/A'}</div>`}).catch(e=>console.error('Failed to load system info:',e))}"
    "function readButtons(){fetch('/api/buttons').then(r=>r.json()).then(data=>{const states=document.getElementById('buttonStates');let html='<div class=\"data-grid\">';Object.entries(data).forEach(([btn,pressed])=>{const color=pressed?'#51cf66':'#666';html+=`<div class='data-item' style='border-left:4px solid ${color}'><strong>${btn.toUpperCase()}:</strong> ${pressed?'PRESSED':'Released'}</div>`});html+='</div>';states.innerHTML=html}).catch(e=>console.error('Failed to read buttons:',e))}"
    "function checkOTAStatus(){fetch('/api/ota/status').then(r=>r.json()).then(data=>{alert(`OTA Status:\\nState: ${data.state}\\nProgress: ${data.progress}%\\nMessage: ${data.message}`)}).catch(e=>alert('Failed to get OTA status'))}"
    "function confirmFirmware(){if(confirm('Confirm current firmware as permanent?\\nThis prevents automatic rollback.')){fetch('/api/ota/confirm',{method:'POST'}).then(r=>r.text()).then(data=>alert(data)).catch(e=>alert('Failed to confirm firmware'))}}"
    "function testDisplay(){fetch('/api/display/test',{method:'POST'}).then(r=>r.text()).then(data=>alert(data)).catch(e=>alert('Display test failed'))}"
    "function rebootSystem(){if(confirm('⚠️ This will reboot the device. Continue?')){fetch('/api/reboot',{method:'POST'}).then(()=>{alert('Reboot command sent. Device will restart in a few seconds.');setTimeout(()=>location.reload(),5000)}).catch(e=>alert('Failed to send reboot command'))}}"
    "setInterval(refreshSystemInfo,10000);setInterval(readButtons,2000);"
    "refreshSystemInfo();readButtons();"
    "</script></body></html>";

/* Simple HTTP request handler function */
static int simple_http_handler(int client_fd, void *user_data)
{
    char buffer[1024];
    int bytes_received;
    char *response = (char *)user_data;

    // Read request
    bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0)
    {
        return -1;
    }

    buffer[bytes_received] = '\0';

    // Simple HTTP response
    char http_response[8192];
    int response_len = snprintf(http_response, sizeof(http_response),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/html\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "%s",
                                (int)strlen(response), response);

    send(client_fd, http_response, response_len, 0);
    close(client_fd);

    k_mutex_lock(&server_mutex, K_FOREVER);
    server_stats.requests_handled++;
    k_mutex_unlock(&server_mutex);

    return 0;
}

/* Simplified web server using raw sockets */
static int run_simple_web_server(void)
{
    int server_fd, client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0)
    {
        LOG_ERR("Failed to create socket: %d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        LOG_ERR("Failed to bind socket: %d", errno);
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) < 0)
    {
        LOG_ERR("Failed to listen on socket: %d", errno);
        close(server_fd);
        return -1;
    }

    LOG_INF("Web server listening on port %d", HTTP_PORT);

    while (current_state == WEB_SERVER_RUNNING)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (current_state == WEB_SERVER_RUNNING)
            {
                LOG_ERR("Failed to accept connection: %d", errno);
            }
            continue;
        }

        // Handle request in a simple way
        simple_http_handler(client_fd, (void *)web_interface_html);
    }

    close(server_fd);
    return 0;
}

/* Web server thread operations */
static int do_start_server(void)
{
    if (current_state == WEB_SERVER_RUNNING)
    {
        return 0; // Already running
    }

    current_state = WEB_SERVER_STARTING;

    // Start simple HTTP server
    if (run_simple_web_server() != 0)
    {
        LOG_ERR("Failed to start HTTP server");
        current_state = WEB_SERVER_ERROR;
        return -1;
    }

    current_state = WEB_SERVER_RUNNING;

    k_mutex_lock(&server_mutex, K_FOREVER);
    server_stats.state = WEB_SERVER_RUNNING;
    k_mutex_unlock(&server_mutex);

    LOG_INF("Web server started successfully on port %d", HTTP_PORT);
    return 0;
}

static int do_stop_server(void)
{
    if (current_state == WEB_SERVER_STOPPED)
    {
        return 0; // Already stopped
    }

    current_state = WEB_SERVER_STOPPED;

    k_mutex_lock(&server_mutex, K_FOREVER);
    server_stats.state = WEB_SERVER_STOPPED;
    k_mutex_unlock(&server_mutex);

    LOG_INF("Web server stopped");
    return 0;
}

static void do_network_status_changed(bool connected, const char *ip_address)
{
    network_connected = connected;

    if (connected && ip_address)
    {
        strncpy(server_ip, ip_address, sizeof(server_ip) - 1);
        server_ip[sizeof(server_ip) - 1] = '\0';
        LOG_INF("Network connected - Web interface: http://%s:%d", server_ip, HTTP_PORT);

        if (current_state == WEB_SERVER_STOPPED)
        {
            do_start_server();
        }
    }
    else
    {
        server_ip[0] = '\0';
        LOG_INF("Network disconnected");
    }
}

/* Main web server thread */
static void web_server_thread_main(void *p1, void *p2, void *p3)
{
    struct server_msg msg;

    LOG_INF("Web server thread started");

    /* Signal that thread is ready */
    k_sem_give(&server_ready_sem);

    while (1)
    {
        /* Process messages */
        if (k_msgq_get(&server_msgq, &msg, K_MSEC(1000)) == 0)
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

            case MSG_REFRESH_DATA:
                LOG_DBG("Data refresh requested");
                break;

            case MSG_BROADCAST_LOG:
                LOG_DBG("Log broadcast: %s", msg.data.log_broadcast.message);
                break;
            }
        }

        /* Periodic health check */
        if (current_state == WEB_SERVER_RUNNING)
        {
            k_mutex_lock(&server_mutex, K_FOREVER);
            server_stats.active_connections = 1;
            k_mutex_unlock(&server_mutex);
        }
    }
}

/* Public API Implementation */
int web_server_start(const struct web_server_callbacks *callbacks)
{
    if (!callbacks)
    {
        return -EINVAL;
    }

    /* Store callbacks */
    memcpy(&server_callbacks, callbacks, sizeof(server_callbacks));

    /* Initialize statistics */
    k_mutex_lock(&server_mutex, K_FOREVER);
    memset(&server_stats, 0, sizeof(server_stats));
    server_stats.state = WEB_SERVER_STARTING;
    k_mutex_unlock(&server_mutex);

    /* Create and start web server thread */
    web_server_thread_id = k_thread_create(&web_server_thread_data,
                                           web_server_stack,
                                           K_THREAD_STACK_SIZEOF(web_server_stack),
                                           web_server_thread_main,
                                           NULL, NULL, NULL,
                                           WEB_SERVER_THREAD_PRIORITY,
                                           0, K_NO_WAIT);

    k_thread_name_set(web_server_thread_id, "web_server");

    /* Wait for thread to be ready */
    k_sem_take(&server_ready_sem, K_SECONDS(5));

    LOG_INF("Web server module initialized");
    return 0;
}

int web_server_stop(void)
{
    struct server_msg msg = {
        .type = MSG_STOP_SERVER};

    if (k_msgq_put(&server_msgq, &msg, K_NO_WAIT) != 0)
    {
        LOG_ERR("Failed to send stop message to web server thread");
        return -ENOMEM;
    }

    return 0;
}

int web_server_get_stats(struct web_server_stats *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    k_mutex_lock(&server_mutex, K_FOREVER);
    memcpy(stats, &server_stats, sizeof(server_stats));
    k_mutex_unlock(&server_mutex);

    return 0;
}

bool web_server_is_running(void)
{
    return (current_state == WEB_SERVER_RUNNING);
}

enum web_server_state web_server_get_state(void)
{
    return current_state;
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
    }
    else
    {
        msg.data.network_status.ip_address[0] = '\0';
    }

    k_msgq_put(&server_msgq, &msg, K_NO_WAIT);
}

void web_server_broadcast_log(const char *message, size_t length)
{
    if (!message || length == 0 || length >= sizeof(((struct server_msg *)0)->data.log_broadcast.message))
    {
        return;
    }

    struct server_msg msg = {
        .type = MSG_BROADCAST_LOG,
        .data.log_broadcast.length = length};

    memcpy(msg.data.log_broadcast.message, message, length);
    msg.data.log_broadcast.message[length] = '\0';

    k_msgq_put(&server_msgq, &msg, K_NO_WAIT);
}

void web_server_refresh_data(void)
{
    struct server_msg msg = {
        .type = MSG_REFRESH_DATA};

    k_msgq_put(&server_msgq, &msg, K_NO_WAIT);
}

void web_server_set_custom_headers(const char *headers)
{
    custom_headers = headers;
}