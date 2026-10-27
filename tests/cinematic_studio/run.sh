#!/bin/sh
# Builds and runs the Cinematic Studio core tests (no engine needed).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined -o test_core test_core.cpp ../../code/cgame/cs_core.cpp ../../code/cgame/cs_json.cpp
./test_core
