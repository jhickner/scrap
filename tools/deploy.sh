#!/usr/bin/env bash
# Build and install scrap on another machine: sync the checkout, then run
# make install there, which restarts every running scrap on the new binary.
set -euo pipefail
host=${1:?usage: tools/deploy.sh HOST [DIR]}
dir=${2:-src/scrap}
cd "$(dirname "$0")/.."

rsync -az --delete --exclude /build --exclude /scrap --exclude /map --exclude /.claude \
    ./ "$host:$dir/"
ssh "$host" "export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH; cd $dir && make -j4 install >/dev/null 2>make.log || { cat make.log >&2; exit 1; }; ~/.local/bin/scrap -V"
