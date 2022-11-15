#!/bin/sh
gcc -fPIC -shared -O1 -g -o ForceIMESupport.so ForceIMESupport.c -ldl -lX11 -Wall -Wextra -Werror && LD_PRELOAD=./ForceIMESupport.so $@
