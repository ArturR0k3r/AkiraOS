/**
 * @file main.c
 * @brief AkiraOS reference firmware entry point
 *
 * The boot sequence is akira_start() in the akira-os module (src/akira_os.c),
 * so product firmware can reuse it from its own main().
 */

#include <akira.h>

int main(void)
{
    return akira_start();
}
