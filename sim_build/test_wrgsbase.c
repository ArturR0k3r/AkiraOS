
    #include <stdio.h>
    #include <stdint.h>
    int main() {
        uint64_t value;
        asm volatile ("wrgsbase %0" : : "r"(value));
        printf("WRGSBASE instruction is available.\n");
        return 0;
    }