/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#ifndef SECTION_H__
#define SECTION_H__

#define __noinit(name) __attribute__((section(".noinit." #name))) name
#define __memimage(name) __attribute__((section(".memimage." #name))) name

#endif
