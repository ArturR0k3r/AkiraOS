/*
 * ESP32 POSIX Timer Compatibility Shim
 *
 * Provides missing sigevent struct members for ESP32 platforms
 * to enable POSIX timer functionality with Zephyr 4.3.0
 */

#include <zephyr/kernel.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#if defined(CONFIG_SOC_SERIES_ESP32) || defined(CONFIG_SOC_SERIES_ESP32S2) ||   \
    defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32C3) || \
    defined(CONFIG_SOC_SERIES_ESP32C6)

/*
 * ESP32 platforms have incomplete sigevent struct definition.
 * We extend it here to provide the missing members needed by Zephyr POSIX timers.
 */

struct sigevent_ext
{
    int sigev_notify;
    int sigev_signo;
    union sigval sigev_value;
    /* Extended members for ESP32 compatibility */
    void (*sigev_notify_function)(union sigval);
    pthread_attr_t *sigev_notify_attributes;
};

/* Weak symbol overrides for timer functions that use these members */
__attribute__((weak)) int timer_create_esp32_compat(clockid_t clockid,
                                                    struct sigevent *evp,
                                                    timer_t *timerid);

#endif /* ESP32 platforms */
