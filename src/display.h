#ifndef DISPLAY_H
#define DISPLAY_H

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize the display and enable its backlight.
     *
     * @return 0 on success, negative error code otherwise.
     */
    int display_init(void);

    /**
     * @brief Turn the display backlight on or off.
     *
     * @param state True to turn on, false to turn off.
     */
    void display_backlight_set(bool state);

    /**
     * @brief Render a simple gradient test pattern to the display.
     */
    void display_test_pattern(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_H
