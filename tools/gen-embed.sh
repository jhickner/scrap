#!/bin/sh
# tools/gen-embed.sh <dir> <header> <symbol> <count> > src/orchdata.c
set -e

dir=${1:?usage: gen-embed.sh <dir> <header> <symbol> <count>}
header=${2:?}
symbol=${3:?}
count=${4:?}

echo "#include \"$header\""
echo
echo "const struct orch_file ${symbol}[] = {"

n=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    printf '    {"%s",\n' "${f#"$dir"/}"
    if [ ! -s "$f" ]; then
        printf '     ""\n'
    else
        awk '{
            gsub(/\\/, "\\\\")
            gsub(/"/, "\\\"")
            printf "     \"%s\\n\"\n", $0
        }' "$f"
    fi
    printf '    },\n'
    n=$((n + 1))
done <<EOF
$(find "$dir" -type f ! -name '.*' | LC_ALL=C sort)
EOF

echo '};'
echo
echo "const int ${count} = $n;"
