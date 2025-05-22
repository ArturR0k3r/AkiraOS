#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

int display_init(void);
void display_test_pattern(void);
void display_backlight_set(bool state);

#endif /* DISPLAY_H */