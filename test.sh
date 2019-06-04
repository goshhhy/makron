#!/bin/sh

Xephyr :10 -screen 1152x864 &

sleep 0.1

export DISPLAY=:10
makron &
xclock &
xterm