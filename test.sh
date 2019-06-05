#!/bin/sh

Xephyr :10 -screen 1152x864 &
xpid=$!

sleep 0.1

export DISPLAY=:10
./build/makron &
xclock &
xeyes &
xload &
xterm &
wait $!

kill $xpid

