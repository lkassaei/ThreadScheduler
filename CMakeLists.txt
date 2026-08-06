cmake_minimum_required(VERSION 4.2)
project(ThreadHandler C)

set(CMAKE_C_STANDARD 11)

add_executable(ThreadHandler
        main.c
        Lists.c
        ThreadHandler.h)
