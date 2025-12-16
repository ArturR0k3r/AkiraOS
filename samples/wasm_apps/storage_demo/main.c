/**
 * @file main.c
 * @brief Storage API Demo - File operations demonstration
 * 
 * Demonstrates:
 * - Writing and reading files
 * - Listing directory contents
 * - File size checking
 * - Data persistence
 */

#include "akira_api.h"

#define MAX_FILENAME 64
#define MAX_CONTENT 256

/**
 * @brief Write configuration file
 */
int write_config(void)
{
    const char *config_data = 
        "# App Configuration\n"
        "version=1.0.0\n"
        "theme=dark\n"
        "auto_save=true\n"
        "refresh_rate=60\n";
    
    int ret = akira_storage_write("config.txt", config_data, strlen(config_data));
    if (ret < 0) {
        akira_log("ERROR: Failed to write config.txt");
        return ret;
    }
    
    akira_log("Wrote config.txt (%d bytes)", ret);
    return 0;
}

/**
 * @brief Read and display file
 */
int read_file(const char *filename)
{
    char buffer[MAX_CONTENT];
    
    int size = akira_storage_read(filename, buffer, sizeof(buffer) - 1);
    if (size < 0) {
        akira_log("ERROR: Failed to read %s", filename);
        return size;
    }
    
    buffer[size] = '\0';
    
    akira_log("=== Content of %s ===", filename);
    akira_log("%s", buffer);
    akira_log("=== End (%d bytes) ===", size);
    
    return 0;
}

/**
 * @brief Write high score data
 */
int write_highscore(uint32_t score)
{
    char data[32];
    int len = snprintf(data, sizeof(data), "highscore=%lu\n", score);
    
    int ret = akira_storage_write("highscore.dat", data, len);
    if (ret < 0) {
        akira_log("ERROR: Failed to write highscore");
        return ret;
    }
    
    akira_log("Saved high score: %lu", score);
    return 0;
}

/**
 * @brief Read high score
 */
uint32_t read_highscore(void)
{
    char buffer[32];
    
    int size = akira_storage_read("highscore.dat", buffer, sizeof(buffer) - 1);
    if (size < 0) {
        akira_log("No high score found, using 0");
        return 0;
    }
    
    buffer[size] = '\0';
    
    uint32_t score = 0;
    if (sscanf(buffer, "highscore=%lu", &score) == 1) {
        akira_log("Loaded high score: %lu", score);
        return score;
    }
    
    return 0;
}

/**
 * @brief List all files in storage
 */
void list_all_files(void)
{
    char files[10][MAX_FILENAME];
    
    int count = akira_storage_list(files, 10, MAX_FILENAME);
    if (count < 0) {
        akira_log("ERROR: Failed to list files");
        return;
    }
    
    akira_log("=== Files in storage ===");
    for (int i = 0; i < count; i++) {
        int size = akira_storage_size(files[i]);
        akira_log("%2d: %-20s %6d bytes", i + 1, files[i], size);
    }
    akira_log("Total: %d files", count);
}

/**
 * @brief Check if file exists
 */
void check_file_exists(const char *filename)
{
    int exists = akira_storage_exists(filename);
    if (exists > 0) {
        int size = akira_storage_size(filename);
        akira_log("File '%s' exists (%d bytes)", filename, size);
    } else {
        akira_log("File '%s' does not exist", filename);
    }
}

/**
 * @brief Write user preferences
 */
int write_preferences(void)
{
    const char *prefs = 
        "{\"volume\":75,"
        "\"brightness\":90,"
        "\"wifi_auto\":true}";
    
    int ret = akira_storage_write("prefs.json", prefs, strlen(prefs));
    if (ret < 0) {
        akira_log("ERROR: Failed to write preferences");
        return ret;
    }
    
    akira_log("Saved preferences (%d bytes)", ret);
    return 0;
}

/**
 * @brief Main entry point
 */
void _start()
{
    akira_log("=================================");
    akira_log("   Storage API Demonstration");
    akira_log("=================================");
    akira_log("");
    
    /* Test 1: Write configuration file */
    akira_log("Test 1: Writing config file...");
    write_config();
    akira_system_sleep_ms(500);
    
    /* Test 2: Read config file */
    akira_log("Test 2: Reading config file...");
    read_file("config.txt");
    akira_system_sleep_ms(500);
    
    /* Test 3: Check file exists */
    akira_log("Test 3: Checking file existence...");
    check_file_exists("config.txt");
    check_file_exists("nonexistent.txt");
    akira_system_sleep_ms(500);
    
    /* Test 4: High score persistence */
    akira_log("Test 4: High score operations...");
    uint32_t old_score = read_highscore();
    uint32_t new_score = old_score + 1000;
    write_highscore(new_score);
    akira_system_sleep_ms(500);
    
    /* Test 5: Write preferences */
    akira_log("Test 5: Writing preferences...");
    write_preferences();
    akira_system_sleep_ms(500);
    
    /* Test 6: List all files */
    akira_log("Test 6: Listing all files...");
    list_all_files();
    akira_system_sleep_ms(500);
    
    /* Test 7: Delete test */
    akira_log("Test 7: Deleting config.txt...");
    int ret = akira_storage_delete("config.txt");
    if (ret == 0) {
        akira_log("Successfully deleted config.txt");
    } else {
        akira_log("ERROR: Failed to delete config.txt");
    }
    akira_system_sleep_ms(500);
    
    /* Test 8: Verify deletion */
    akira_log("Test 8: Verifying deletion...");
    check_file_exists("config.txt");
    list_all_files();
    
    akira_log("");
    akira_log("=================================");
    akira_log("   All tests completed!");
    akira_log("=================================");
    
    /* Keep app running to view results */
    akira_log("App will exit in 10 seconds...");
    akira_system_sleep_ms(10000);
}
