/**
 * @file message_bus.c
 * @brief Inter-Process Communication Message Bus Implementation
 * 
 * Provides asynchronous message passing for WASM apps and system services.
 * Uses Zephyr message queues for efficient delivery.
 */

#include "message_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(msg_bus, CONFIG_AKIRA_LOG_LEVEL);

/* Message queue configuration */
#define MSG_QUEUE_SIZE          32
#define MSG_QUEUE_ALIGN         4

/* Pending reply tracking */
#define MAX_PENDING_REPLIES     8

/* Message bus state */
static struct {
	bool initialized;
	uint32_t next_msg_id;
	uint32_t next_subscriber_id;
	
	/* Message queue */
	struct k_msgq msg_queue;
	char __aligned(MSG_QUEUE_ALIGN) msg_queue_buf[MSG_QUEUE_SIZE * sizeof(struct akira_message)];
	
	/* Subscribers */
	struct msg_subscriber subscribers[MSG_MAX_SUBSCRIBERS];
	uint32_t subscriber_count;
	struct k_mutex subscriber_mutex;
	
	/* Pending replies */
	struct {
		uint32_t msg_id;
		struct k_sem sem;
		struct akira_message *reply_buf;
		bool received;
	} pending_replies[MAX_PENDING_REPLIES];
	
	/* Statistics */
	uint32_t stats_sent;
	uint32_t stats_received;
	uint32_t stats_dropped;
} bus_state;

/**
 * @brief Check if topic matches filter (supports * wildcard)
 */
static bool topic_matches(const char *topic, const char *filter)
{
	// TODO: Implement topic matching with wildcards
	// - Exact match
	// - * matches any single level
	// - # matches any remaining levels
	
	/* Simple exact match for now */
	return strcmp(topic, filter) == 0 || strcmp(filter, "*") == 0;
}

/**
 * @brief Find free pending reply slot
 */
static int find_free_reply_slot(void)
{
	for (int i = 0; i < MAX_PENDING_REPLIES; i++) {
		if (bus_state.pending_replies[i].msg_id == 0) {
			return i;
		}
	}
	return -1;
}

int msg_bus_init(void)
{
	if (bus_state.initialized) {
		return 0;
	}
	
	LOG_INF("Initializing message bus");
	
	// TODO: Full initialization
	// 1. Initialize message queue
	// 2. Initialize subscriber mutex
	// 3. Initialize reply semaphores
	// 4. Start processing thread (optional)
	
	k_msgq_init(&bus_state.msg_queue, bus_state.msg_queue_buf,
	            sizeof(struct akira_message), MSG_QUEUE_SIZE);
	
	k_mutex_init(&bus_state.subscriber_mutex);
	
	/* Initialize reply slots */
	for (int i = 0; i < MAX_PENDING_REPLIES; i++) {
		k_sem_init(&bus_state.pending_replies[i].sem, 0, 1);
		bus_state.pending_replies[i].msg_id = 0;
	}
	
	bus_state.next_msg_id = 1;
	bus_state.next_subscriber_id = 1;
	bus_state.initialized = true;
	
	LOG_INF("Message bus initialized");
	return 0;
}

int msg_bus_subscribe(const char *topic, msg_handler_t handler, void *user_data)
{
	if (!bus_state.initialized) {
		return -ENODEV;
	}
	
	if (!topic || !handler) {
		return -EINVAL;
	}
	
	k_mutex_lock(&bus_state.subscriber_mutex, K_FOREVER);
	
	if (bus_state.subscriber_count >= MSG_MAX_SUBSCRIBERS) {
		k_mutex_unlock(&bus_state.subscriber_mutex);
		LOG_ERR("Max subscribers reached");
		return -ENOMEM;
	}
	
	// TODO: Check for duplicate subscriptions
	
	struct msg_subscriber *sub = &bus_state.subscribers[bus_state.subscriber_count];
	sub->id = bus_state.next_subscriber_id++;
	sub->handler = handler;
	sub->user_data = user_data;
	strncpy(sub->topic_filter, topic, MSG_MAX_TOPIC_LEN - 1);
	sub->topic_filter[MSG_MAX_TOPIC_LEN - 1] = '\0';
	sub->min_priority = MSG_PRIORITY_LOW;
	
	bus_state.subscriber_count++;
	
	k_mutex_unlock(&bus_state.subscriber_mutex);
	
	LOG_INF("Subscribed to topic '%s' (id=%d)", topic, sub->id);
	return sub->id;
}

int msg_bus_unsubscribe(int subscriber_id)
{
	if (!bus_state.initialized) {
		return -ENODEV;
	}
	
	k_mutex_lock(&bus_state.subscriber_mutex, K_FOREVER);
	
	// TODO: Implement unsubscribe
	// 1. Find subscriber by ID
	// 2. Remove from list (compact array)
	// 3. Update count
	
	for (uint32_t i = 0; i < bus_state.subscriber_count; i++) {
		if (bus_state.subscribers[i].id == (uint32_t)subscriber_id) {
			/* Compact array */
			for (uint32_t j = i; j < bus_state.subscriber_count - 1; j++) {
				bus_state.subscribers[j] = bus_state.subscribers[j + 1];
			}
			bus_state.subscriber_count--;
			k_mutex_unlock(&bus_state.subscriber_mutex);
			LOG_INF("Unsubscribed id=%d", subscriber_id);
			return 0;
		}
	}
	
	k_mutex_unlock(&bus_state.subscriber_mutex);
	return -ENOENT;
}

int msg_bus_publish(const char *topic, const void *payload, size_t len,
                    msg_priority_t priority)
{
	if (!bus_state.initialized) {
		return -ENODEV;
	}
	
	if (!topic || (len > 0 && !payload) || len > MSG_MAX_PAYLOAD_SIZE) {
		return -EINVAL;
	}
	
	// TODO: Implement publish
	// 1. Build message
	// 2. Queue message
	// 3. Optionally trigger immediate processing for high priority
	
	struct akira_message msg = {0};
	msg.header.msg_id = bus_state.next_msg_id++;
	msg.header.sender_id = 0;  // TODO: Get caller ID
	msg.header.recipient_id = 0;  // Broadcast
	strncpy(msg.header.topic, topic, MSG_MAX_TOPIC_LEN - 1);
	msg.header.priority = priority;
	msg.header.timestamp = k_uptime_get_32();
	msg.header.payload_len = len;
	
	if (len > 0) {
		memcpy(msg.payload, payload, len);
	}
	
	int ret = k_msgq_put(&bus_state.msg_queue, &msg, K_NO_WAIT);
	if (ret != 0) {
		bus_state.stats_dropped++;
		LOG_WRN("Message queue full, dropping message");
		return -EAGAIN;
	}
	
	bus_state.stats_sent++;
	LOG_DBG("Published to '%s' (id=%d, len=%zu)", topic, msg.header.msg_id, len);
	
	return msg.header.msg_id;
}

int msg_bus_send(uint32_t recipient_id, const void *payload, size_t len,
                 msg_delivery_t delivery)
{
	if (!bus_state.initialized) {
		return -ENODEV;
	}
	
	if ((len > 0 && !payload) || len > MSG_MAX_PAYLOAD_SIZE) {
		return -EINVAL;
	}
	
	// TODO: Implement point-to-point send
	// 1. Build message with recipient
	// 2. For SYNC, set up reply waiter
	// 3. Queue message
	// 4. For SYNC, wait for reply
	
	struct akira_message msg = {0};
	msg.header.msg_id = bus_state.next_msg_id++;
	msg.header.sender_id = 0;  // TODO: Get caller ID
	msg.header.recipient_id = recipient_id;
	msg.header.topic[0] = '\0';  // P2P, no topic
	msg.header.priority = MSG_PRIORITY_NORMAL;
	msg.header.timestamp = k_uptime_get_32();
	msg.header.payload_len = len;
	
	if (len > 0) {
		memcpy(msg.payload, payload, len);
	}
	
	int ret = k_msgq_put(&bus_state.msg_queue, &msg, K_NO_WAIT);
	if (ret != 0) {
		bus_state.stats_dropped++;
		return -EAGAIN;
	}
	
	bus_state.stats_sent++;
	
	if (delivery == MSG_DELIVER_SYNC) {
		// TODO: Wait for reply
		LOG_WRN("Synchronous delivery not fully implemented");
	}
	
	return msg.header.msg_id;
}

int msg_bus_wait_reply(int msg_id, struct akira_message *reply, k_timeout_t timeout)
{
	if (!bus_state.initialized || !reply) {
		return -EINVAL;
	}
	
	// TODO: Implement reply waiting
	// 1. Find pending reply slot for msg_id
	// 2. Wait on semaphore
	// 3. Copy reply to output buffer
	// 4. Clean up slot
	
	LOG_WRN("msg_bus_wait_reply not implemented");
	return -ENOTSUP;
}

int msg_bus_reply(const struct akira_message *original, const void *payload, size_t len)
{
	if (!bus_state.initialized || !original) {
		return -EINVAL;
	}
	
	// TODO: Implement reply
	// 1. Build reply message
	// 2. Set recipient to original sender
	// 3. Find pending reply slot
	// 4. Copy reply and signal semaphore
	
	LOG_WRN("msg_bus_reply not implemented");
	return -ENOTSUP;
}

int msg_bus_process(void)
{
	if (!bus_state.initialized) {
		return -ENODEV;
	}
	
	struct akira_message msg;
	int processed = 0;
	
	while (k_msgq_get(&bus_state.msg_queue, &msg, K_NO_WAIT) == 0) {
		bus_state.stats_received++;
		
		/* Deliver to matching subscribers */
		k_mutex_lock(&bus_state.subscriber_mutex, K_FOREVER);
		
		for (uint32_t i = 0; i < bus_state.subscriber_count; i++) {
			struct msg_subscriber *sub = &bus_state.subscribers[i];
			
			/* Check recipient for P2P messages */
			if (msg.header.recipient_id != 0 && 
			    msg.header.recipient_id != sub->id) {
				continue;
			}
			
			/* Check topic for pub/sub messages */
			if (msg.header.topic[0] != '\0' &&
			    !topic_matches(msg.header.topic, sub->topic_filter)) {
				continue;
			}
			
			/* Check priority filter */
			if (msg.header.priority < sub->min_priority) {
				continue;
			}
			
			/* Deliver message */
			if (sub->handler) {
				sub->handler(&msg, sub->user_data);
			}
		}
		
		k_mutex_unlock(&bus_state.subscriber_mutex);
		processed++;
	}
	
	return processed;
}

void msg_bus_stats(uint32_t *sent, uint32_t *received, uint32_t *dropped)
{
	if (sent) {
		*sent = bus_state.stats_sent;
	}
	if (received) {
		*received = bus_state.stats_received;
	}
	if (dropped) {
		*dropped = bus_state.stats_dropped;
	}
}
