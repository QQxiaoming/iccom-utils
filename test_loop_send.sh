#!/bin/sh

i=0
while true
do
    if [ "$i" -ge 4294967296 ];
    then
        let i=0
    fi
    ./iccom_send 0123#$(printf "%08x" $i)
    let i+=1
#    sleep 1
done
