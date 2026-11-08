/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "Session.h"

namespace solar::gui {

// The window (the browser process): it has the screen, the keyboard and the pointer. What the user does is told to the page the window shows, the pictures that page makes are shown, and what it
// asks of the window (a title, the clipboard, full screen, the pointer held, sound) is done; the pages' processes are the session's (it starts the first for `address`, and another where a
// navigation goes to another site). Returns the exit code of the page that was shown last, when the program is to end.
int RunWindow(SDL_Window* window, SDL_Renderer* renderer, BrowserServices& services, Launcher launcher, bool allowFiles, const std::string& address);

}  // namespace solar::gui
