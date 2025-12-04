/**
 * @file main.c
 * @brief Hello World - Simple AkiraOS WASM App Example
 * 
 * This is a minimal example demonstrating how to create a WASM app
 * that runs on AkiraOS using WAMR libc-builtin.
 */

/* Declare puts - WAMR libc-builtin provides this */
int puts(const char *s);

int main() 
{  
    puts("");
    puts("========================================");
    puts("    _    _    _           ___  ____    ");
    puts("   / \\  | | _(_)_ __ __ _/ _ \\/ ___|   ");
    puts("  / _ \\ | |/ / | '__/ _` | | | \\___ \\  ");
    puts(" / ___ \\|   <| | | | (_| | |_| |___) | ");
    puts("/_/   \\_\\_|\\_\\_|_|  \\__,_|\\___/|____/  ");
    puts("                                        ");
    puts("    Hello from WASM App!                ");
    puts("    Running on AkiraOS v1.2.1           ");
    puts("    Powered by WebAssembly and OCRE     ");
    puts("========================================");
    puts("");
    
    return 0;
}
