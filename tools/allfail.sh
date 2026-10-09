#!/bin/sh
# Runs one WPT page printing every failure, not the first fifteen: tools/allfail.sh tests/wpt/css/x/y.html
page="$1"
dir=$(dirname "$page")
tmp="$dir/zz-allfail-$(basename "$page")"
{ echo '<script>globalThis.__allFailures = true;</script>'; cat "$page"; } > "$tmp"
build/WptTest "$tmp" 2>&1
rm -f "$tmp"
