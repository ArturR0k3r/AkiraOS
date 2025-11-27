/**
 * @file message_bus.h
 * @brief Inter-Process Communication Message Bus
 * 
 * Asynchronous message passing between WASM apps and system services.
 * Supports pub/sub patterns and point-to-point messaging.
 */

#ifndef AKIRA_MESSAGE_BUS_H
#define AKIRA_MESSAGE_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum message payload size
 */
#define MSG_MAX_PAYLOAD_SIZE    256

/**
 * @brief Maximum topic name length
 */
#define MSG_MAX_TOPIC_LEN       32

/**
 * @brief Maximum subscribers per topic
 */
#define MSG_MAX_SUBSCRIBERS     16

/**
 * @brief Message priority levels
 */
typedef enum {
	MSG_PRIORITY_LOW     = 0,
	MSG_PRIORITY_NORMAL  = 1,
	MSG_PRIORITY_HIGH    = 2,
	MSG_PRIORITY_URGENT  = 3
} msg_priority_t;

/**
 * @brief Message delivery mode
 */
typedef enum {
	MSG_DELIVER_ASYNC = 0,    // Queue for later delivery
	MSG_DELIVER_SYNC  = 1,    // Block until delivered
	MSG_DELIVER_FIRE_FORGET = 2  // No delivery confirmation
} msg_delivery_t;

/**
 * @brief Message header
 */
struct msg_header {
	uint32_t msg_id;           // Unique message ID
	uint32_t sender_id;        // Sender app/service ID
	uint32_t recipient_id;     // Recipient ID (0 = broadcast)
	char topic[MSG_MAX_TOPIC_LEN];  // Topic string
	msg_priority_t priority;
	uint32_t timestamp;        // System uptime when sent
	uint16_t payload_len;
	uint8_t flags;
};

/**
 * @brief Complete message structure
 */
struct akira_message {
	struct msg_header header;
	uint8_t payload[MSG_MAX_PAYLOAD_SIZE];
};

/**
 * @brief Message handler callback
 */
typedef void (*msg_handler_t)(const struct akira_message *msg, void *user_data);

/**
 * @brief Subscriber information
 */
struct msg_subscriber {
	uint32_t id;
	msg_handler_t handler;
	void *user_data;
	char topic_filter[MSG_MAX_TOPIC_LEN];  // Supports wildcards
	msg_priority_t min_priority;
};

/**
 * @brief Initialize message bus
 * @return 0 on success
 */
int msg_bus_init(void);

/**
 * @brief Subscribe to topic
 * @param topic Topic pattern (supports * wildcard)
 * @param handler Callback function
 * @param user_data User context
 * @return Subscriber ID or negative error
 */
int msg_bus_subscribe(const char *topic, msg_handler_t handler, void *user_data);

/**
 * @brief Unsubscribe from topic
 * @param subscriber_id ID returned from subscribe
 * @return 0 on success
 */
int msg_bus_unsubscribe(int subscriber_id);

/**
 * @brief Publish message to topic
 * @param topic Target topic
 * @param payload Message data
 * @param len Payload length
 * @param priority Message priority
 * @return Message ID or negative error
 */
int msg_bus_publish(const char *topic, const void *payload, size_t len,
                    msg_priority_t priority);

/**
 * @brief Send point-to-point message
 * @param recipient_id Target recipient
 * @param payload Message data
 * @param len Payload length
 * @param delivery Delivery mode
 * @return Message ID or negative error
 */
int msg_bus_send(uint32_t recipient_id, const void *payload, size_t len,
                 msg_delivery_t delivery);

/**
 * @brief Wait for reply to message
 * @param msg_id Original message ID
 * @param reply Output buffer for reply
 * @param timeout Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int msg_bus_wait_reply(int msg_id, struct akira_message *reply, k_timeout_t timeout);

/**
 * @brief Reply to received message
 * @param original Original message
 * @param payload Reply data
 * @param len Reply length
 * @return 0 on success
 */
int msg_bus_reply(const struct akira_message *original, const void *payload, size_t len);

/**
 * @brief Process pending messages (called from main loop)
 * @return Number of messages processed
 */
int msg_bus_process(void);

/**
 * @brief Get message bus statistics
 * @param sent Output for messages sent
 * @param received Output for messages received
 * @param dropped Output for messages dropped
 */
void msg_bus_stats(uint32_t *sent, uint32_t *received, uint32_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MESSAGE_BUS_H */
