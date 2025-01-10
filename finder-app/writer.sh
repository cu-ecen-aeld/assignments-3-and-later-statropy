#!/bin/sh

if ! [ $# -eq 2 ]
then
    echo usage: writer.sh \<writefile\> \<writestr\>
    exit 1
fi

writefile=$1
writestr=$2

mkdir -p $( dirname "$writefile" )
echo $writestr > $writefile
