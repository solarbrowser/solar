# Web Platform Tests

`tests/wpt` is the official [web-platform-tests](https://github.com/web-platform-tests/wpt) repository as a submodule, pinned to the commit the pages we pass were checked against.
Nothing of ours lives in it. Run `tools/setupwpt.sh` once after cloning (it checks out only the directories the tests use).

What is ours is in `tests/wpt-solar`:

- `dom-passing.txt`, `css-passing.txt`: the pages `make test-wpt` runs (each in a process of its own); all must pass.
- `harness.js`, `skip.tsv`: the harness the runner (`tests/WptTest.cpp`) loads, and the test names it skips with the reason.
- `pages/`: pages of our own (served as `/solar/...`), `fonts/`: fonts of our own.

To move to a newer WPT: `git -C tests/wpt fetch origin && git -C tests/wpt checkout <commit>`, run `make test-wpt`, fix or take out of the lists what changed upstream, and commit the new pointer.
`tools/layoutref.py` writes generated pages into `tests/wpt/css`; they are untracked files of the submodule and are not to be committed.
