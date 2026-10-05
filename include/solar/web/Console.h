#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>

#include "quanta/Embed.h"

// The Console API (https://console.spec.whatwg.org/): log, info, warn, error, debug, assert, count, time, group,
// table, dir, trace and the rest. Values are described with the engine's inspection, as a developer console prints
// them; where the text goes is the program's: a ConsoleSink.
namespace solar::web {

// How serious a message is, which a developer console shows as its colour and its filter.
enum class ConsoleLevel { Info, Debug, Warn, Error };

// One call of Printer: the standard's log level (`method`: "log", "assert", "count", "timeEnd", "group" and the rest,
// named as the function that made it), how serious that is, the text it comes to, with the indentation of the groups
// it is in and possibly on several lines, and the values it was given, which are good for the call only.
struct ConsoleMessage {
  std::string method;
  ConsoleLevel level = ConsoleLevel::Info;
  std::string text;
  std::span<const Quanta::Value> args;
};

// Where console output goes.
class ConsoleSink {
 public:
  virtual ~ConsoleSink() = default;
  virtual void Message(Quanta::Context& ctx, const ConsoleMessage& message) = 0;
  virtual void Clear() {}
};

// Output on stdout (log, info, debug) and stderr (warn, error, trace), which is the sink a realm has when it is not
// given another.
ConsoleSink* DefaultConsoleSink();

// How a host object (a DOM node, say) is printed, which the engine cannot see into; nothing for one it does not know.
using HostObjectFormatter = std::function<std::optional<std::string>(Quanta::Context& ctx, const Quanta::Value& object)>;
void SetHostObjectFormatter(HostObjectFormatter formatter);

// A value as console.log prints it: `depth` levels of objects deep (negative for no limit).
std::string InspectValue(Quanta::Context& ctx, const Quanta::Value& value, int depth = 2);

// Gives the realm a `console`. The realm is made with RealmOptions::installConsole off, so that Quanta's is not there.
void InstallConsoleApi(Quanta::Embed::Realm& realm, ConsoleSink* sink = nullptr);
void InstallConsoleApi(Quanta::Embed::Runtime& runtime, ConsoleSink* sink = nullptr);

}  // namespace solar::web
