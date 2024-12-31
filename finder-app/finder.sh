#!/bin/bash

if ! [ $# -eq 2 ]
then
    echo usage: finder.sh \<filesdir\> \<searchstr\>
    exit 1
fi

filesdir=$1
searchstr=$2

if ! [ -d $filesdir ]
then
    echo $filesdir is not a valid directory
    exit 1
fi

echo The number of files are $( find $filesdir -type f | wc -l ) and the number of matching lines are $( grep -rI "$searchstr" $filesdir | wc -l )
