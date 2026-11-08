/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <SDL3/SDL.h>

#include "solar/net/Ipc.h"

namespace solar::gui {

// The window (the browser process): it has the screen, the keyboard and the pointer. What the user does is told to the page (`link`) as it is, the pictures the page makes are shown, and
// what the page asks of the window (a title, the clipboard, full screen, the pointer held) is done. Returns when the program is to end.
void RunWindow(net::ipc::Channel& link, SDL_Window* window, SDL_Renderer* renderer);

// The window's size as the page is told it.
void SendGeometry(net::ipc::Channel& link, SDL_Window* window);

}  // namespace solar::gui
