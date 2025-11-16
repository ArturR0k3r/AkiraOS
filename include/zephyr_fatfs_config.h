/* Application FatFs config override */

/* Fix version mismatch - Zephyr expects 80386 but fatfs provides 5380 */
#ifndef FFCONF_DEF
#define FFCONF_DEF 80386
#endif

// #undef FF_VOLUMES
// #define FF_VOLUMES 2

// /* Optional: long filename support */
// #define FF_USE_LFN 2
// #define FF_MAX_LFN 255
