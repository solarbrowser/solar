/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Web Animations, the frame: animations are updated, and their events sent, before the callbacks of an animation frame run.
(function () {
  'use strict';
  const S = globalThis.__solarAnim;
  const hook = globalThis.__solarFrameHook;
  delete globalThis.__solarFrameHook;
  if (hook) S.requestFrame = hook((now) => S.frame(now), () => S.endFrame(), () => S.wantsFrame());
  // The members of an interface are enumerable.
  for (const name of ['AnimationTimeline', 'DocumentTimeline', 'Animation', 'AnimationEffect', 'KeyframeEffect', 'AnimationPlaybackEvent']) {
    const proto = globalThis[name].prototype;
    for (const key of Reflect.ownKeys(proto)) {
      if (key === 'constructor' || typeof key !== 'string') continue;
      const descriptor = Reflect.getOwnPropertyDescriptor(proto, key);
      Reflect.defineProperty(proto, key, { ...descriptor, enumerable: true });
    }
  }
  delete globalThis.__solarAnim;
})();
