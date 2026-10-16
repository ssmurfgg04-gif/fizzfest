#!/usr/bin/env bash
set -e
cc -O3 -march=native -pthread -o server server.c
