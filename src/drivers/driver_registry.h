/**
 * @file driver_registry.h
 * @brief Generic Driver Registration System
 * 
 * Allows runtime driver registration and lookup.
 * Decouples API layer from specific driver implementations.
 */

#ifndef AKIRA_DRIVER_REGISTRY_H
#define AKIRA_DRIVER_REGISTRY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Driver categories */
typedef enum {
    DRIVER_TYPE_DISPLAY,
    DRIVER_TYPE_SENSOR_IMU,
    DRIVER_TYPE_SENSOR_ENV,
    DRIVER_TYPE_SENSOR_POWER,
    DRIVER_TYPE_RF,
    DRIVER_TYPE_STORAGE,
    DRIVER_TYPE_MAX
} driver_type_t;

/* Generic driver operations */
typedef struct {
    int (*init)(void);
    int (*deinit)(void);
    int (*read)(void *buffer, size_t size);
    int (*write)(const void *buffer, size_t size);
    int (*ioctl)(uint32_t cmd, void *arg);
    void *priv_data;
} driver_ops_t;

/* Driver descriptor */
typedef struct {
    const char *name;           /* Driver name (e.g., "ili9341") */
    driver_type_t type;         /* Driver category */
    const driver_ops_t *ops;    /* Operations */
    bool initialized;
} driver_desc_t;

/**
 * @brief Initialize driver registry
 * @return 0 on success
 */
int driver_registry_init(void);

/**
 * @brief Register a driver
 * @param name Driver name (must be unique)
 * @param type Driver category
 * @param ops Driver operations
 * @return 0 on success, negative on error
 */
int driver_registry_register(const char *name, driver_type_t type, 
                             const driver_ops_t *ops);

/**
 * @brief Unregister a driver
 * @param name Driver name
 * @return 0 on success
 */
int driver_registry_unregister(const char *name);

/**
 * @brief Get driver by name
 * @param name Driver name
 * @return Driver descriptor or NULL if not found
 */
const driver_desc_t *driver_registry_get(const char *name);

/**
 * @brief Get first driver of specified type
 * @param type Driver type
 * @return Driver descriptor or NULL if not found
 */
const driver_desc_t *driver_registry_get_by_type(driver_type_t type);

/**
 * @brief List all registered drivers of a type
 * @param type Driver type (or DRIVER_TYPE_MAX for all)
 * @param names Output array of driver names
 * @param max_count Maximum entries in array
 * @return Number of drivers found
 */
int driver_registry_list(driver_type_t type, const char **names, size_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DRIVER_REGISTRY_H */
