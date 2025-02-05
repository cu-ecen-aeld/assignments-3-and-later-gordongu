#!/bin/bash

# Check if arguments exist
if [ -z "$1" ] || [ -z "$2" ]; then
    echo "Error: missing arguments"
    exit 1
fi

# Check if directory exists
if ! [ -d "$1" ]; then
    echo "Error: missing directory"
    exit 1
fi

filesdir=$1
searchstr=$2

# Get number of files
files=$(find "$filesdir" -type f -follow | wc -l)
# Get number of matching lines
lines=$(grep "$searchstr" $(find "$filesdir" -type f -follow) | wc -l )

echo "The number of files are $files and the number of matching lines are $lines"
