#!/bin/bash
g++ pwtest1.cpp -o pwt -g -Wall -I /usr/include/spa-0.2 -I /usr/include/pipewire-0.3 -D_REENTRANT -lpipewire-0.3 -lpistache
