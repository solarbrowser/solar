#pragma once

#include "quanta/Embed.h"

// Errors and rejected promises, as a page learns of them: the window's error event (and window.onerror) for an
// exception nothing caught, unhandledrejection and rejectionhandled for a promise (https://html.spec.whatwg.org/#
// report-the-exception, #unhandled-promise-rejections).
namespace solar::html {

// Fires the error event at the window of the realm `ctx` is of, for an exception with where it came from. True when
// a handler cancelled it (window.onerror returned true, or preventDefault); otherwise it is printed.
bool ReportError(Quanta::Context& ctx, const Quanta::Value& exception, const Quanta::Embed::ErrorInfo& info);

// What the program gives its Isolate with SetUncaughtExceptionHandler and SetPromiseRejectionHandler, so that what
// timers, jobs and microtasks throw, and promises nobody handles, reach the page of the realm they happened in.
Quanta::Embed::UncaughtExceptionHandler MakeUncaughtExceptionHandler();
Quanta::Embed::PromiseRejectionHandler MakeRejectionHandler();

// The window script's natives: __solarRegisterReporters(reportError, reportRejection).
void DefineErrorNatives(Quanta::Context& ctx);

}  // namespace solar::html
