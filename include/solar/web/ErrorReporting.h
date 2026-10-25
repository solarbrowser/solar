/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>

#include "quanta/Embed.h"

namespace solar::web {

// "Report the exception" (https://html.spec.whatwg.org/#report-the-exception): what is done with an exception that
// no script was left to catch, such as one an event listener threw. The Web API that gives a window its error event
// sets the reporter; without one the exception is printed.
using ExceptionReporter = std::function<void(Quanta::Context& ctx, const Quanta::Value& exception)>;
void SetExceptionReporter(ExceptionReporter reporter);

// Reports the exception that is pending in `ctx` and clears it.
void ReportException(Quanta::Context& ctx);

}  // namespace solar::web
