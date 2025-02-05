#!/bin/bash

# Check if arguments exist
if [ -z "$1" ] || [ -z "$2" ]; then
    echo "Error: missing arguments"
    exit 1
fi

writefile=$1
writestr=$2

# Create directory if it doesn't exist
mkdir -p $(dirname "$writefile")
# Write to file
echo "$writestr" > "$writefile"

# Check if file was successfully created and written to
if ! [ -e "$writefile" ]; then
    echo "Error: failed to create file"
    exit 1
elif [ $(cat "$writefile") != "$writestr" ]; then
    echo "Error: failed to write to file"
    exit 1
fi
