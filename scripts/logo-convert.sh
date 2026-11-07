#!/bin/sh

if [ -z "$1" ]; then
    echo "logo-convert.sh: a quick image magick command wrapper" >&2
    echo "Usage: $0 <input-image>" >&2
    exit 1
fi

INPUT="$1"
OUTPUT="${INPUT%.*}.bmp"

magick "$INPUT" -resize 186x208\! -units PixelsPerCentimeter -density 96x96 -type TrueColor "bmp3:$OUTPUT"
