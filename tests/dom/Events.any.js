const document = new Text().ownerDocument;

function tree() {
  const root = document.createElement('div');
  const middle = document.createElement('p');
  const leaf = document.createElement('b');
  root.append(middle);
  middle.append(leaf);
  return { root, middle, leaf };
}

test(() => {
  const { root, middle, leaf } = tree();
  const log = [];
  const record = (name, capture) => (e) => log.push(name + (capture ? '(c)' : '') + ':' + e.eventPhase + ':' + (e.currentTarget === {root, middle, leaf}[name]));
  for (const [name, node] of Object.entries({ root, middle, leaf })) {
    node.addEventListener('x', record(name, true), true);
    node.addEventListener('x', record(name, false), false);
  }
  const event = new Event('x', { bubbles: true });
  assert_true(leaf.dispatchEvent(event));
  assert_array_equals(log, ['root(c):1:true', 'middle(c):1:true', 'leaf(c):2:true', 'leaf:2:true', 'middle:3:true', 'root:3:true']);
  assert_equals(event.target, leaf);
  assert_equals(event.currentTarget, null);
  assert_equals(event.eventPhase, Event.NONE);
}, 'an event goes down the tree and back up');

test(() => {
  const { root, middle, leaf } = tree();
  const log = [];
  root.addEventListener('x', () => log.push('root'));
  leaf.addEventListener('x', () => log.push('leaf'));
  leaf.dispatchEvent(new Event('x'));
  assert_array_equals(log, ['leaf'], 'an event that does not bubble stays at its target');
  root.addEventListener('x', () => log.push('root capture'), true);
  leaf.dispatchEvent(new Event('x'));
  assert_array_equals(log, ['leaf', 'root capture', 'leaf'], 'but still goes down');
}, 'bubbles: false');

test(() => {
  const { root, middle, leaf } = tree();
  const log = [];
  root.addEventListener('x', () => log.push('root'));
  middle.addEventListener('x', (e) => { log.push('middle'); e.stopPropagation(); });
  middle.addEventListener('x', () => log.push('middle again'));
  leaf.dispatchEvent(new Event('x', { bubbles: true }));
  assert_array_equals(log, ['middle', 'middle again'], 'stopPropagation finishes the target');
  log.length = 0;
  middle.addEventListener('y', (e) => { log.push('first'); e.stopImmediatePropagation(); });
  middle.addEventListener('y', () => log.push('second'));
  leaf.dispatchEvent(new Event('y', { bubbles: true }));
  assert_array_equals(log, ['first']);
  log.length = 0;
  root.addEventListener('z', (e) => { log.push('capture'); e.stopPropagation(); }, true);
  leaf.addEventListener('z', () => log.push('leaf'));
  leaf.dispatchEvent(new Event('z', { bubbles: true }));
  assert_array_equals(log, ['capture'], 'stopped while going down');
}, 'stopping propagation');

test(() => {
  const { root, middle, leaf } = tree();
  let path;
  middle.addEventListener('x', (e) => { path = e.composedPath(); });
  const event = new Event('x', { bubbles: true });
  leaf.dispatchEvent(event);
  assert_array_equals(path, [leaf, middle, root]);
  assert_array_equals(event.composedPath(), [], 'empty when it is done');
}, 'composedPath');

test(() => {
  const { root, middle, leaf } = tree();
  const log = [];
  root.addEventListener('x', () => log.push('root'));
  const detached = document.createElement('i');
  middle.append(detached);
  detached.addEventListener('x', () => { log.push('detached'); middle.removeChild(leaf); });
  leaf.addEventListener('x', () => log.push('leaf'));
  detached.dispatchEvent(new Event('x', { bubbles: true }));
  assert_array_equals(log, ['detached', 'root'], 'the path is fixed when the event starts');
  const fragment = document.createDocumentFragment();
  const inFragment = document.createElement('u');
  fragment.append(inFragment);
  let reached = false;
  fragment.addEventListener('x', () => { reached = true; });
  inFragment.dispatchEvent(new Event('x', { bubbles: true }));
  assert_true(reached, 'a fragment is on the path');
  const cancelable = new Event('c', { cancelable: true });
  leaf.addEventListener('c', (e) => e.preventDefault());
  assert_false(leaf.dispatchEvent(cancelable));
}, 'the path, fragments and cancelation');
