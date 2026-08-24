#!/bin/sh
# tools/gen-defaults.sh <dir> > src/boarddefaults.c
set -e

dir=${1:?usage: gen-defaults.sh <dir>}

echo '#include "boarddefaults.h"'
echo
echo 'const struct board_default board_defaults[] = {'

n=0
for f in "$dir"/*/*.md; do
    printf '    {"%s",\n' "${f#"$dir"/}"
    awk '{
        gsub(/\\/, "\\\\")
        gsub(/"/, "\\\"")
        printf "     \"%s\\n\"\n", $0
    }' "$f"
    printf '    },\n'
    n=$((n + 1))
done

echo '};'
echo
echo "const int board_defaults_n = $n;"
