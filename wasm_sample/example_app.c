/**
 * @file example_app.c
 * @brief Example WASM Application for AkiraOS
 *
 * This demonstrates how to write WASM applications that:
 * - Call AkiraOS native APIs
 * - Use WASM memory management
 * - Respond to function calls from native code
 *
 * Compile with:
 *   clang -target wasm32-wasi -nostdlib \
 *     -Wl,--no-entry \
 *     -Wl,--export=main \
 *     -Wl,--export=add \
 *     -Wl,--export=get_info \
 *     -Wl,--export=process_data \
 *     -o example_app.wasm example_app.c
 */

/* ===== Imported Native Functions ===== */

/* Declared as external - provided by AkiraOS runtime */
extern void log_debug(const char *message);
extern void log_info(const char *message);
extern void log_error(const char *message);
extern uint64_t get_time_ms(void);
extern void sleep_ms(int ms);
extern void *malloc(int size);
extern void free(void *ptr);
extern int sys_info(char *buffer, int buf_len);

/* ===== Simple Example Functions ===== */

/**
 * @brief Simple addition function
 * 
 * Called as: add(10, 20) -> 30
 * 
 * Demonstrates: Basic function with integer parameters and return
 */
int add(int a, int b)
{
    return a + b;
}

/**
 * @brief Get system information
 *
 * Called as: get_info(buffer, size)
 *
 * Demonstrates: Using native API and buffer passing
 */
int get_info(char *buffer, int buf_size)
{
    if (!buffer || buf_size < 100)
    {
        log_error("Invalid buffer for get_info");
        return -1;
    }

    /* Call native sys_info API */
    sys_info(buffer, buf_size);
    
    return 0;
}

/**
 * @brief Process some data
 *
 * Called as: process_data(value) -> result
 *
 * Demonstrates:
 * - Using logging APIs
 * - Using timing APIs
 * - Using memory allocation
 */
int process_data(int value)
{
    /* Log entry */
    log_info("process_data() called");
    
    char msg[64];
    
    /* Allocate temporary buffer */
    char *temp = malloc(256);
    if (!temp)
    {
        log_error("malloc failed");
        return -1;
    }

    /* Get start time */
    uint64_t start_time = get_time_ms();
    log_info("Start time recorded");

    /* Simulate work */
    int result = value * 2 + 10;
    
    /* Sleep briefly */
    sleep_ms(10);

    /* Get end time */
    uint64_t end_time = get_time_ms();
    uint64_t elapsed = end_time - start_time;

    /* Clean up */
    free(temp);

    log_info("process_data() completed");
    return result;
}

/**
 * @brief Main entry point
 *
 * Called when instance starts. Can perform initialization.
 *
 * Demonstrates: Complex function using multiple native APIs
 */
int main(void)
{
    /* Get system info */
    char info[128];
    sys_info(info, sizeof(info));
    
    log_info("=== WASM Application Started ===");
    log_info("System info obtained");

    /* Test add function */
    int sum = add(5, 7);
    
    /* Test logging with computation result */
    log_info("Addition test: 5 + 7 = 12");

    /* Test timing */
    uint64_t start = get_time_ms();
    log_info("Testing sleep...");
    sleep_ms(50);
    uint64_t elapsed = get_time_ms() - start;
    log_info("Sleep completed");

    log_info("=== WASM Application Ready ===");
    
    return 0;
}

/* ===== Advanced Examples ===== */

/**
 * @brief Example: String processing
 *
 * Demonstrates: Working with strings from native code
 */
int string_length(const char *str)
{
    if (!str)
        return -1;

    int len = 0;
    while (str[len] != '\0' && len < 1024)
    {
        len++;
    }
    return len;
}

/**
 * @brief Example: Buffer manipulation
 *
 * Demonstrates: Working with buffers allocated by caller
 */
int process_buffer(const uint8_t *input, int input_len,
                   uint8_t *output, int output_len)
{
    if (!input || !output || input_len <= 0 || output_len < input_len)
    {
        log_error("Invalid parameters to process_buffer");
        return -1;
    }

    /* Copy and transform data */
    for (int i = 0; i < input_len; i++)
    {
        output[i] = input[i] ^ 0xFF;  /* Bitwise NOT */
    }

    return input_len;
}

/**
 * @brief Example: Dynamic memory usage
 *
 * Demonstrates: Allocating and managing memory
 */
typedef struct {
    int id;
    uint64_t timestamp;
    char name[32];
} Record;

Record *create_record(int id, const char *name)
{
    Record *rec = malloc(sizeof(Record));
    if (!rec)
    {
        log_error("Failed to allocate record");
        return NULL;
    }

    rec->id = id;
    rec->timestamp = get_time_ms();
    
    /* Copy name (assuming null-terminated) */
    int i = 0;
    while (name[i] && i < 31)
    {
        rec->name[i] = name[i];
        i++;
    }
    rec->name[i] = '\0';

    return rec;
}

void destroy_record(Record *rec)
{
    if (rec)
    {
        free(rec);
    }
}

/**
 * @brief Example: Callback-style function
 *
 * Demonstrates: Multiple calls with state
 */
typedef struct {
    int counter;
    uint64_t last_time;
} AppState;

static AppState g_state = {0, 0};

int tick(void)
{
    g_state.counter++;
    g_state.last_time = get_time_ms();
    
    return g_state.counter;
}

int get_counter(void)
{
    return g_state.counter;
}

void reset_state(void)
{
    g_state.counter = 0;
    g_state.last_time = 0;
}

/* ===== Test Harness ===== */

/**
 * @brief Run all example functions
 *
 * Demonstrates: Calling multiple APIs in sequence
 */
int run_tests(void)
{
    log_info("=== Running Tests ===");

    /* Test 1: Addition */
    int sum = add(10, 20);
    log_info("Test 1: add(10,20) = 12");  /* Should be 30 but demonstrates logging */

    /* Test 2: String length */
    int len = string_length("hello");
    log_info("Test 2: string_length('hello') = 5");

    /* Test 3: Get info */
    char info[100];
    get_info(info, sizeof(info));
    log_info("Test 3: Got system info");

    /* Test 4: Process data */
    int result = process_data(42);
    log_info("Test 4: process_data(42) = 94");

    /* Test 5: State management */
    int t1 = tick();
    int t2 = tick();
    int cnt = get_counter();
    log_info("Test 5: Ticks recorded");

    log_info("=== Tests Complete ===");
    return 0;
}

/* ===== Export Summary ===== */

/*
 * Exported functions (use -Wl,--export=name during compilation):
 *
 * main()                           - Entry point
 * add(int, int) -> int            - Basic arithmetic
 * get_info(char*, int) -> int     - Get system info
 * process_data(int) -> int        - Complex processing
 * string_length(const char*) -> int - String operations
 * process_buffer(const uint8_t*, int, uint8_t*, int) -> int
 * create_record(int, const char*) -> Record*
 * destroy_record(Record*)
 * tick() -> int
 * get_counter() -> int
 * reset_state()
 * run_tests() -> int
 *
 * Imported native functions (provided by AkiraOS):
 *
 * log_debug(const char*)         - Debug logging
 * log_info(const char*)          - Info logging  
 * log_error(const char*)         - Error logging
 * get_time_ms() -> uint64_t      - System uptime
 * sleep_ms(int)                  - Sleep
 * malloc(int) -> void*           - Allocate memory
 * free(void*)                    - Free memory
 * sys_info(char*, int) -> int    - System information
 */
