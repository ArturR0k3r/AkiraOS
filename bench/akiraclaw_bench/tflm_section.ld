/*
 * tflm_section.ld — TFLM text-section boundary symbols for B4
 *
 * Included via zephyr_linker_sources(SECTIONS ...) in CMakeLists.txt.
 * Content is inserted directly into the generated linker SECTIONS block —
 * do NOT wrap in SECTIONS {} here.
 *
 * Defines __tflm_text_start / __tflm_text_end so bench_b4_mem.c can
 * compute the TFLite Micro flash footprint at runtime.
 */
    .tflm_text : ALIGN_WITH_INPUT {
        __tflm_text_start = .;
        *libtensorflow-lite-micro.a:*(.text .text.*)
        __tflm_text_end = .;
    } > drom0_0_seg AT > FLASH
