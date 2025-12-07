/*
 * @copyright Copyright © contributors to Project Ocre,
 * which has been established as Project Ocre a Series of LF Projects, LLC
 * SPDX-License-Identifier: Apache-2.0
 * 
 * AkiraOS Hello World WASM App - minimal no-stdlib version
 * 
 * Compiles with -nostdlib to avoid WASI imports
 * Calls putchar directly from env module (provided by WAMR libc-builtin)
 */

/* Declare external functions from env module (WAMR libc-builtin) */
extern int putchar(int c);

int main(void)
{
    putchar('H');
    putchar('e');
    putchar('l');
    putchar('l');
    putchar('o');
    putchar(' ');
    putchar('f');
    putchar('r');
    putchar('o');
    putchar('m');
    putchar(' ');
    putchar('W');
    putchar('A');
    putchar('S');
    putchar('M');
    putchar(' ');
    putchar('o');
    putchar('n');
    putchar(' ');
    putchar('A');
    putchar('k');
    putchar('i');
    putchar('r');
    putchar('a');
    putchar('O');
    putchar('S');
    putchar('!');
    putchar('\n');
    
    return 0;
}
