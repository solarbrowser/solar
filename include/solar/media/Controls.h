/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace solar::media {

// Where the parts of the controls of a video are (the bar over the bottom of the picture, in CSS pixels from the left of the picture): the painter draws them there and the
// script takes the clicks there, so the measures are made in one place. What there is room for is shown, the least needed last to go: with 480 px or more all of it, from
// 320 no volume slider, from 200 no time, from 160 no full screen button, from 120 only the play button and the position.
struct ControlsSpan {
  float from = 0, to = 0;
  bool shown = false;
};
struct ControlsLayout {
  float bar = 48;  // the bar's height
  ControlsSpan play, progress, time, mute, slider, fullscreen;
  bool any = false;  // whether there is room for controls at all
};

inline ControlsLayout LayoutControls(float width, float height) {
  ControlsLayout c;
  if (width < 120 || height < 64) return c;
  c.any = true;
  c.play = {0, 48, true};
  float cursor = width;
  if (width >= 200) {
    c.fullscreen = {cursor - 44, cursor, true};
    cursor = c.fullscreen.from;
  }
  if (width >= 320) {
    c.slider = {0, 0, false};
  }
  if (width >= 480) {
    c.slider = {cursor - 72, cursor - 8, true};
    cursor = c.slider.from - 0;
  }
  if (width >= 160) {
    c.mute = {cursor - 40, cursor, true};
    cursor = c.mute.from;
  }
  if (width >= 320) {
    c.time = {cursor - 108, cursor - 8, true};
    cursor = c.time.from;
  }
  c.progress = {c.play.to + 4, cursor - 8, true};
  if (c.progress.to - c.progress.from < 24) c.progress.shown = false;
  return c;
}

}  // namespace solar::media
