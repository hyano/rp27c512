cmake_minimum_required(VERSION 3.13)

add_library(xmodem STATIC
    xmodem.c
)

target_include_directories(xmodem PUBLIC .)
