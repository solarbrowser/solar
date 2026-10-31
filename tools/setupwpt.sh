#!/bin/sh
# Checks out tests/wpt (the web-platform-tests submodule) with only the directories the tests use, and without the file contents of the rest (the whole of WPT is several GB).
# Usage: tools/setupwpt.sh   (from the repository root; a plain `git submodule update --init tests/wpt` also works, but it takes everything)
set -e
git submodule update --init --filter=blob:none tests/wpt
git -C tests/wpt sparse-checkout set --no-cone /common/ /console/ /css/ /custom-elements/ /dom/ /encoding/ /fetch/ /FileAPI/ /fonts/ /html/ /import-maps/ \
  /intersection-observer/ /resize-observer/ /resources/ /shadow-dom/ /streams/ /url/ /wai-aria/ /web-animations/ /webidl/ /webmessaging/ /xhr/ /images/ /media/
