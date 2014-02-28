#!/bin/sh

# mdx2wav from https://github.com/mitsuman/mdx2wav
MDX2WAV=/usr/local/bin/mdx2wav

ACTION="${1}"
case "${ACTION}" in
        rawdecode | filerawdecode)
                INPUT="${2}"
                OUTPUT="${3}"
                exec $MDX2WAV -d 0 "${INPUT}"
                ;;

        gettag)
                TAGNAME="${2}"
                INPUT="${3}"
                             case "${TAGNAME}" in
                                time) $MDX2WAV -m "${INPUT}" ;;
                                title) $MDX2WAV -t "${INPUT}" | iconv -f SHIFT_JIS -t UTF-8 ;;
                                *) exit 1 ;;
                             esac ;;

        *)
                echo "$0 filerawdecode <inputfile>" >&2
                echo "$0 gettag <tagname> <inputfile>" >&2
                exit 1 ;;
esac

