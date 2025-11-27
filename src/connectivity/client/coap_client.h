/**
 * @file coap_client.h
 * @brief CoAP Client for AkiraOS
 *
 * Provides CoAP client functionality for connecting to constrained 
 * application protocol servers (IoT cloud, LwM2M, etc.)
 */

#ifndef AKIRA_COAP_CLIENT_H
#define AKIRA_COAP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define COAP_CLIENT_MAX_URL_LEN     256
#define COAP_CLIENT_MAX_TOKEN_LEN   8
#define COAP_CLIENT_MAX_OPTIONS     16
#define COAP_CLIENT_MAX_PAYLOAD     1024

/*===========================================================================*/
/* Types                                                                     */
/*===========================================================================*/

/** CoAP methods */
typedef enum {
    COAP_METHOD_GET    = 1,
    COAP_METHOD_POST   = 2,
    COAP_METHOD_PUT    = 3,
    COAP_METHOD_DELETE = 4
} coap_method_t;

/** CoAP response codes */
typedef enum {
    /* Success 2.xx */
    COAP_CODE_CREATED       = 65,   /* 2.01 */
    COAP_CODE_DELETED       = 66,   /* 2.02 */
    COAP_CODE_VALID         = 67,   /* 2.03 */
    COAP_CODE_CHANGED       = 68,   /* 2.04 */
    COAP_CODE_CONTENT       = 69,   /* 2.05 */
    
    /* Client Error 4.xx */
    COAP_CODE_BAD_REQUEST   = 128,  /* 4.00 */
    COAP_CODE_UNAUTHORIZED  = 129,  /* 4.01 */
    COAP_CODE_FORBIDDEN     = 131,  /* 4.03 */
    COAP_CODE_NOT_FOUND     = 132,  /* 4.04 */
    COAP_CODE_NOT_ALLOWED   = 133,  /* 4.05 */
    
    /* Server Error 5.xx */
    COAP_CODE_INTERNAL_ERR  = 160,  /* 5.00 */
    COAP_CODE_NOT_IMPL      = 161,  /* 5.01 */
    COAP_CODE_UNAVAILABLE   = 163   /* 5.03 */
} coap_code_t;

/** CoAP content formats */
typedef enum {
    COAP_FORMAT_TEXT_PLAIN    = 0,
    COAP_FORMAT_LINK_FORMAT   = 40,
    COAP_FORMAT_XML           = 41,
    COAP_FORMAT_OCTET_STREAM  = 42,
    COAP_FORMAT_EXI           = 47,
    COAP_FORMAT_JSON          = 50,
    COAP_FORMAT_CBOR          = 60,
    COAP_FORMAT_SENML_JSON    = 110,
    COAP_FORMAT_SENML_CBOR    = 112
} coap_content_format_t;

/** CoAP message types */
typedef enum {
    COAP_TYPE_CON = 0,  /**< Confirmable */
    COAP_TYPE_NON = 1,  /**< Non-confirmable */
    COAP_TYPE_ACK = 2,  /**< Acknowledgement */
    COAP_TYPE_RST = 3   /**< Reset */
} coap_type_t;

/** CoAP request configuration */
typedef struct {
    const char *url;              /**< CoAP URL (coap:// or coaps://) */
    coap_method_t method;         /**< Request method */
    coap_type_t type;             /**< Message type (CON/NON) */
    coap_content_format_t format; /**< Content format */
    const uint8_t *payload;       /**< Request payload */
    size_t payload_len;           /**< Payload length */
    uint32_t timeout_ms;          /**< Request timeout */
} coap_request_t;

/** CoAP response */
typedef struct {
    coap_code_t code;             /**< Response code */
    coap_content_format_t format; /**< Content format */
    const uint8_t *payload;       /**< Response payload */
    size_t payload_len;           /**< Payload length */
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
} coap_response_t;

/** CoAP observe callback */
typedef void (*coap_observe_cb_t)(const coap_response_t *response, void *user_data);

/** CoAP observe handle */
typedef int coap_observe_handle_t;

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize CoAP client
 * @return 0 on success
 */
int coap_client_init(void);

/**
 * @brief Deinitialize CoAP client
 * @return 0 on success
 */
int coap_client_deinit(void);

/*===========================================================================*/
/* Request/Response                                                          */
/*===========================================================================*/

/**
 * @brief Send CoAP request and wait for response
 * @param request Request configuration
 * @param response Response buffer (output)
 * @return 0 on success
 */
int coap_client_request(const coap_request_t *request, coap_response_t *response);

/**
 * @brief Send CoAP GET request
 * @param url CoAP URL
 * @param response Response buffer
 * @return 0 on success
 */
int coap_client_get(const char *url, coap_response_t *response);

/**
 * @brief Send CoAP POST request
 * @param url CoAP URL
 * @param payload Request payload
 * @param payload_len Payload length
 * @param format Content format
 * @param response Response buffer
 * @return 0 on success
 */
int coap_client_post(const char *url, const uint8_t *payload, size_t payload_len,
                      coap_content_format_t format, coap_response_t *response);

/**
 * @brief Send CoAP PUT request
 * @param url CoAP URL
 * @param payload Request payload
 * @param payload_len Payload length
 * @param format Content format
 * @param response Response buffer
 * @return 0 on success
 */
int coap_client_put(const char *url, const uint8_t *payload, size_t payload_len,
                     coap_content_format_t format, coap_response_t *response);

/**
 * @brief Send CoAP DELETE request
 * @param url CoAP URL
 * @param response Response buffer
 * @return 0 on success
 */
int coap_client_delete(const char *url, coap_response_t *response);

/*===========================================================================*/
/* Observe (Push Notifications)                                              */
/*===========================================================================*/

/**
 * @brief Start observing a resource
 * @param url Resource URL
 * @param callback Notification callback
 * @param user_data User data for callback
 * @return Observe handle (>= 0) or negative error
 */
coap_observe_handle_t coap_client_observe(const char *url, 
                                           coap_observe_cb_t callback,
                                           void *user_data);

/**
 * @brief Stop observing a resource
 * @param handle Observe handle
 * @return 0 on success
 */
int coap_client_observe_stop(coap_observe_handle_t handle);

/*===========================================================================*/
/* Block Transfer                                                            */
/*===========================================================================*/

/**
 * @brief Download large resource using block transfer
 * @param url Resource URL
 * @param buffer Output buffer
 * @param buffer_len Buffer length
 * @param received_len Actual received length (output)
 * @return 0 on success
 */
int coap_client_download(const char *url, uint8_t *buffer, size_t buffer_len,
                          size_t *received_len);

/**
 * @brief Upload large resource using block transfer
 * @param url Resource URL
 * @param data Data to upload
 * @param data_len Data length
 * @param format Content format
 * @return 0 on success
 */
int coap_client_upload(const char *url, const uint8_t *data, size_t data_len,
                        coap_content_format_t format);

/*===========================================================================*/
/* Utility                                                                   */
/*===========================================================================*/

/**
 * @brief Free response payload memory
 * @param response Response to free
 */
void coap_client_free_response(coap_response_t *response);

/**
 * @brief Get string for response code
 * @param code Response code
 * @return Code string
 */
const char *coap_code_to_str(coap_code_t code);

/**
 * @brief Set default DTLS credentials for CoAPS
 * @param psk Pre-shared key
 * @param psk_len PSK length
 * @param psk_id PSK identity
 * @return 0 on success
 */
int coap_client_set_psk(const uint8_t *psk, size_t psk_len, const char *psk_id);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_COAP_CLIENT_H */
