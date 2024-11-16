/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"

#include "readline.h"

void readline(char *buffer, int32_t size)
{
    int32_t cursor = 0;
    buffer[cursor] = '\0';

    for (;;)
    {
        int c = getchar();

        if ((c == '\x08') || (c == '\x7f'))
        {
            if (cursor > 0)
            {
                cursor--;
                buffer[cursor] = '\0';
                printf("\x08");
                printf("\x1b[1P");
            }
        }
        else if (c == '\x0d')
        {
            printf("\n");
            break;
        }
        else if (isprint(c))
        {
            if (cursor < size - 1)
            {
                buffer[cursor] = c;
                cursor++;
                buffer[cursor] = '\0';
                printf("%c", c);
            }
        }
    }
}
