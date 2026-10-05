#include "solar/web/ErrorReporting.h"

#include <cstdio>

namespace solar::web {

namespace qe = Quanta::Embed;

namespace {

ExceptionReporter& Reporter() {
  static ExceptionReporter reporter;
  return reporter;
}

}  // namespace

void SetExceptionReporter(ExceptionReporter reporter) { Reporter() = std::move(reporter); }

void ReportException(Quanta::Context& ctx) {
  if (!qe::HasException(ctx)) return;
  const Quanta::Value exception = ctx.get_exception();
  ctx.clear_exception();
  if (Reporter()) {
    Reporter()(ctx, exception);
    return;
  }
  const qe::ErrorInfo info = qe::InspectError(ctx, exception);
  std::fprintf(stderr, "Uncaught %s%s%s\n", info.name.c_str(), info.name.empty() ? "" : ": ", info.message.c_str());
}

}  // namespace solar::web
