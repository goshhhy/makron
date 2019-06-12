#!/bin/sh

Xephyr :10 -screen 1152x864 &
xpid=$!

sleep 0.1

export DISPLAY=:10
./build/makron &
cinnabar &
barpid=$!
sleep 0.25
xterm &
termpid=$!
sleep 0.25
xclock &
sleep 0.25
xeyes &
sleep 0.25
xload &
wait $termpid

kill $barpid
kill $xpid

