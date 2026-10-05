#include "solar/web/Console.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

HostObjectFormatter& HostFormatter() {
  static HostObjectFormatter formatter;
  return formatter;
}

constexpr size_t kMaxArrayEntries = 100;
constexpr size_t kBreakLength = 80;

// The characters of UTF-8 text, which is how wide it is for what is lined up.
size_t DisplayWidth(const std::string& text) {
  size_t width = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++width;
  }
  return width;
}

bool IsIdentifier(const std::string& key) {
  if (key.empty()) return false;
  for (size_t i = 0; i < key.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(key[i]);
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
    if (!(letter || (i > 0 && c >= '0' && c <= '9'))) return false;
  }
  return true;
}

// A string as it is written in source, in the quotes that need the least escaping.
std::string Quote(const std::string& text) {
  char quote = '\'';
  if (text.find('\'') != std::string::npos) {
    if (text.find('"') == std::string::npos) quote = '"';
    else if (text.find('`') == std::string::npos && text.find("${") == std::string::npos) quote = '`';
  }
  std::string out(1, quote);
  for (unsigned char c : text) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\v': out += "\\v"; break;
      case '\\': out += "\\\\"; break;
      default:
        if (c == static_cast<unsigned char>(quote)) {
          out += '\\';
          out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7F) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\x%02X", c);
          out += buffer;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += quote;
  return out;
}

std::string IsoDate(double ms) {
  if (std::isnan(ms)) return "Invalid Date";
  const double days = std::floor(ms / 86400000.0);
  double rest = ms - days * 86400000.0;
  // Civil date from days since the epoch.
  long long z = static_cast<long long>(days) + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const long long doe = z - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long year = yoe + era * 400;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long long mp = (5 * doy + 2) / 153;
  const long long day = doy - (153 * mp + 2) / 5 + 1;
  const long long month = mp < 10 ? mp + 3 : mp - 9;
  if (month <= 2) ++year;
  const int hours = static_cast<int>(rest / 3600000.0);
  rest -= hours * 3600000.0;
  const int minutes = static_cast<int>(rest / 60000.0);
  rest -= minutes * 60000.0;
  const int seconds = static_cast<int>(rest / 1000.0);
  const int millis = static_cast<int>(rest - seconds * 1000.0);
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%04lld-%02lld-%02lldT%02d:%02d:%02d.%03dZ", year, month, day, hours, minutes, seconds, millis);
  return buffer;
}

// ---- Describing a value ----

class Inspector {
 public:
  Inspector(Context& ctx, int depth) : ctx_(ctx), depth_(depth) {}

  std::string Format(const Value& value, int level) {
    if (!qe::IsObject(value)) return Primitive(value, true);
    return Object(value, level);
  }

  static std::string PrimitiveText(Context& ctx, const Value& value, bool quoteStrings) {
    Inspector inspector(ctx, 0);
    return inspector.Primitive(value, quoteStrings);
  }

 private:
  std::string Primitive(const Value& value, bool quoteStrings) {
    if (value.is_undefined()) return "undefined";
    if (value.is_null()) return "null";
    if (value.is_boolean()) return value.to_boolean() ? "true" : "false";
    if (value.is_string()) {
      const std::string text = qe::ToWtf8(ctx_, value);
      return quoteStrings ? Quote(text) : text;
    }
    if (value.is_number()) {
      const double number = value.as_number();
      if (number == 0 && std::signbit(number)) return "-0";
      return qe::ToWtf8(ctx_, value);
    }
    if (value.is_bigint()) return qe::ToWtf8(ctx_, value) + "n";
    if (value.is_symbol()) return value.to_string();
    return "";
  }

  std::string KeyText(const Value& key) {
    if (key.is_symbol()) return "[" + key.to_string() + "]";
    const std::string text = qe::ToWtf8(ctx_, key);
    return IsIdentifier(text) ? text : Quote(text);
  }

  // The entries of an object, as `key: value` text.
  void Properties(const Value& object, int level, std::vector<std::string>& out, const std::set<std::string>& skip, bool skipIndices) {
    qe::PropertyList list = qe::InspectProperties(ctx_, object);
    for (const qe::PropertyInfo& property : list) {
      if (!property.enumerable) continue;
      if (!property.key.is_symbol()) {
        const std::string name = qe::ToWtf8(ctx_, property.key);
        if (skip.count(name)) continue;
        if (skipIndices && !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
      }
      std::string text = KeyText(property.key) + ": ";
      if (property.has_getter || property.has_setter) {
        text += property.has_getter && property.has_setter ? "[Getter/Setter]" : property.has_getter ? "[Getter]" : "[Setter]";
      } else {
        text += Format(property.value, level + 1);
      }
      out.push_back(std::move(text));
    }
  }

  std::string Reduce(const std::string& prefix, const std::string& open, const std::string& close, const std::vector<std::string>& entries, int level) {
    if (entries.empty()) return prefix + open + close;
    size_t total = entries.size();
    bool newline = false;
    for (const std::string& entry : entries) {
      total += DisplayWidth(entry);
      if (entry.find('\n') != std::string::npos) newline = true;
    }
    const size_t start = entries.size() + static_cast<size_t>(level) * 2 + open.size() + prefix.size() + 10;
    if (!newline && total + start <= kBreakLength) {
      std::string line = prefix + open + " ";
      for (size_t i = 0; i < entries.size(); ++i) line += (i ? ", " : "") + entries[i];
      return line + " " + close;
    }
    const std::string indentation(static_cast<size_t>(level) * 2, ' ');
    std::string text = prefix + open + "\n";
    for (size_t i = 0; i < entries.size(); ++i) {
      text += indentation + "  " + entries[i] + (i + 1 < entries.size() ? ",\n" : "\n");
    }
    return text + indentation + close;
  }

  std::string Object(const Value& value, int level) {
    qe::ObjectInfo info = qe::Inspect(ctx_, value);
    if (info.kind == qe::ObjectKind::Proxy) {
      if (info.proxy_revoked) return "<Revoked Proxy>";
      return Format(info.proxy_target, level);
    }
    if (info.kind == qe::ObjectKind::Host) {
      if (HostFormatter()) {
        if (auto text = HostFormatter()(ctx_, value)) return *text;
      }
    }
    if (std::find(stack_.begin(), stack_.end(), info.id) != stack_.end()) {
      auto found = refs_.find(info.id);
      if (found == refs_.end()) found = refs_.emplace(info.id, static_cast<int>(refs_.size()) + 1).first;
      return "[Circular *" + std::to_string(found->second) + "]";
    }
    const bool deep = depth_ >= 0 && level > depth_;
    const std::string className = info.class_name.empty() ? "Object" : info.class_name;
    const bool nullPrototype = info.prototype.is_null();

    // What it is called when it cannot be looked into any further.
    const auto label = [&]() -> std::string {
      switch (info.kind) {
        case qe::ObjectKind::Array: return "[Array]";
        case qe::ObjectKind::Map: case qe::ObjectKind::Set: return "[" + className + "]";
        default: return nullPrototype ? "[Object: null prototype]" : "[" + className + "]";
      }
    };

    stack_.push_back(info.id);
    std::string result;
    std::vector<std::string> entries;
    switch (info.kind) {
      case qe::ObjectKind::Array:
      case qe::ObjectKind::Arguments: {
        if (deep) { result = label(); break; }
        size_t holes = 0;
        const size_t shown = std::min(info.size, kMaxArrayEntries);
        const auto flushHoles = [&] {
          if (holes) entries.push_back("<" + std::to_string(holes) + " empty item" + (holes > 1 ? "s" : "") + ">");
          holes = 0;
        };
        for (size_t i = 0; i < info.size && entries.size() < shown; ++i) {
          if (!qe::HasProperty(ctx_, value, qe::FromWtf8(ctx_, std::to_string(i)))) {
            ++holes;
            continue;
          }
          flushHoles();
          Value item = qe::GetIndex(ctx_, value, static_cast<uint32_t>(i));
          if (qe::HasException(ctx_)) ctx_.clear_exception();
          entries.push_back(Format(item, level + 1));
        }
        flushHoles();
        if (info.size > kMaxArrayEntries && entries.size() >= shown) entries.push_back("... " + std::to_string(info.size - kMaxArrayEntries) + " more item" + (info.size - kMaxArrayEntries > 1 ? "s" : ""));
        Properties(value, level, entries, {"length"}, true);
        std::string prefix;
        if (info.kind == qe::ObjectKind::Arguments) prefix = "[Arguments] ";
        else if (className != "Array" && className != "Object") prefix = className + "(" + std::to_string(info.size) + ") ";
        result = Reduce(prefix, "[", "]", entries, level);
        break;
      }
      case qe::ObjectKind::TypedArray: {
        if (deep) { result = "[" + className + "]"; break; }
        const size_t shown = std::min(info.size, kMaxArrayEntries);
        for (size_t i = 0; i < shown; ++i) entries.push_back(Format(qe::GetIndex(ctx_, value, static_cast<uint32_t>(i)), level + 1));
        if (info.size > shown) entries.push_back("... " + std::to_string(info.size - shown) + " more item" + (info.size - shown > 1 ? "s" : ""));
        result = Reduce(className + "(" + std::to_string(info.size) + ") ", "[", "]", entries, level);
        break;
      }
      case qe::ObjectKind::ArrayBuffer:
      case qe::ObjectKind::SharedArrayBuffer: {
        std::string contents = "<";
        const auto bytesOptional = qe::BytesOf(value);
        const std::span<const uint8_t> bytes = bytesOptional ? *bytesOptional : std::span<const uint8_t>();
        for (size_t i = 0; i < bytes.size() && i < 50; ++i) {
          char buffer[4];
          std::snprintf(buffer, sizeof buffer, "%02x", bytes[i]);
          contents += (i ? " " : "") + std::string(buffer);
        }
        if (bytes.size() > 50) contents += " ... " + std::to_string(bytes.size() - 50) + " more byte" + (bytes.size() - 50 > 1 ? "s" : "");
        contents += ">";
        if (info.detached) contents = "(detached)";
        entries.push_back("[Uint8Contents]: " + contents);
        entries.push_back("byteLength: " + std::to_string(info.size));
        result = Reduce(className + " ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::Function: {
        std::string base;
        const qe::FunctionInfo& f = *info.function;
        if (f.is_class) {
          base = "[class " + (f.name.empty() ? std::string("(anonymous)") : f.name) + "]";
        } else {
          std::string kind = f.is_async && f.is_generator ? "AsyncGeneratorFunction" : f.is_async ? "AsyncFunction" : f.is_generator ? "GeneratorFunction" : "Function";
          base = "[" + kind + (f.name.empty() ? " (anonymous)" : ": " + f.name) + "]";
        }
        if (deep) { result = base; break; }
        Properties(value, level, entries, {"prototype", "length", "name"}, false);
        result = entries.empty() ? base : Reduce(base + " ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::Error: {
        const qe::ErrorInfo error = qe::InspectError(ctx_, value);
        std::string text = !error.stack.empty() ? error.stack : (error.name.empty() ? "Error" : error.name) + (error.message.empty() ? "" : ": " + error.message);
        if (!deep) Properties(value, level, entries, {"stack", "message"}, false);
        result = entries.empty() ? text : Reduce(text + " ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::Date: result = IsoDate(info.date_value); break;
      case qe::ObjectKind::RegExp: result = "/" + info.regexp_source + "/" + info.regexp_flags; break;
      case qe::ObjectKind::Promise: {
        if (deep) { result = "[Promise]"; break; }
        if (info.promise_state == qe::PromiseState::Pending) entries.push_back("<pending>");
        else if (info.promise_state == qe::PromiseState::Fulfilled) entries.push_back(Format(info.promise_result, level + 1));
        else entries.push_back("<rejected> " + Format(info.promise_result, level + 1));
        result = Reduce("Promise ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::Map: {
        if (deep) { result = label(); break; }
        qe::ValueList items = qe::MapEntries(value);
        for (size_t i = 0; i + 1 < items.size() && entries.size() < kMaxArrayEntries; i += 2) entries.push_back(Format(items[i], level + 1) + " => " + Format(items[i + 1], level + 1));
        if (info.size > kMaxArrayEntries) entries.push_back("... " + std::to_string(info.size - kMaxArrayEntries) + " more item" + (info.size - kMaxArrayEntries > 1 ? "s" : ""));
        Properties(value, level, entries, {}, false);
        result = Reduce(className + "(" + std::to_string(info.size) + ") ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::Set: {
        if (deep) { result = label(); break; }
        qe::ValueList items = qe::SetValues(value);
        for (size_t i = 0; i < items.size() && entries.size() < kMaxArrayEntries; ++i) entries.push_back(Format(items[i], level + 1));
        if (info.size > kMaxArrayEntries) entries.push_back("... " + std::to_string(info.size - kMaxArrayEntries) + " more item" + (info.size - kMaxArrayEntries > 1 ? "s" : ""));
        Properties(value, level, entries, {}, false);
        result = Reduce(className + "(" + std::to_string(info.size) + ") ", "{", "}", entries, level);
        break;
      }
      case qe::ObjectKind::WeakMap:
      case qe::ObjectKind::WeakSet: result = className + " { <items unknown> }"; break;
      case qe::ObjectKind::WeakRef: result = "WeakRef { <target unknown> }"; break;
      case qe::ObjectKind::BoxedBoolean: case qe::ObjectKind::BoxedNumber: case qe::ObjectKind::BoxedString: case qe::ObjectKind::BoxedSymbol: case qe::ObjectKind::BoxedBigInt: {
        const char* kind = info.kind == qe::ObjectKind::BoxedBoolean ? "Boolean" : info.kind == qe::ObjectKind::BoxedNumber ? "Number" : info.kind == qe::ObjectKind::BoxedString ? "String" : info.kind == qe::ObjectKind::BoxedSymbol ? "Symbol" : "BigInt";
        const std::string base = std::string("[") + kind + ": " + Primitive(info.primitive_value, true) + "]";
        Properties(value, level, entries, {}, info.kind == qe::ObjectKind::BoxedString);
        result = entries.empty() ? base : Reduce(base + " ", "{", "}", entries, level);
        break;
      }
      default: {
        if (deep) { result = label(); break; }
        Properties(value, level, entries, {}, false);
        std::string prefix;
        if (info.kind == qe::ObjectKind::ModuleNamespace) prefix = "[Module: null prototype] ";
        else if (nullPrototype) prefix = "[Object: null prototype] ";
        else if (info.kind == qe::ObjectKind::Generator || info.kind == qe::ObjectKind::AsyncGenerator || info.kind == qe::ObjectKind::Iterator) prefix = "Object [" + className + "] ";
        else if (className != "Object") prefix = className + " ";
        result = Reduce(prefix, "{", "}", entries, level);
      }
    }
    stack_.pop_back();
    if (const auto found = refs_.find(info.id); found != refs_.end()) result = "<ref *" + std::to_string(found->second) + "> " + result;
    return result;
  }

  Context& ctx_;
  int depth_;
  std::vector<uint64_t> stack_;
  std::map<uint64_t, int> refs_;
};

// ---- The console ----

struct StdSink : ConsoleSink {
  void Message(ConsoleLevel level, const std::string& text) override {
    FILE* out = level == ConsoleLevel::Warn || level == ConsoleLevel::Error || level == ConsoleLevel::Trace ? stderr : stdout;
    std::fputs(text.c_str(), out);
    std::fputc('\n', out);
  }
};

struct ConsoleState {
  ConsoleSink* sink = nullptr;
  // %String%, %parseInt% and %parseFloat%, as they were when the console was made, which Formatter calls.
  std::shared_ptr<qe::Persistent> stringFunction, parseIntFunction, parseFloatFunction;
  std::vector<std::string> groups;  // the labels of the groups that are open, only their depth matters
  std::map<std::string, uint64_t> counts;
  std::map<std::string, std::chrono::steady_clock::time_point> timers;
};

// "Formatter": the format specifiers of the first argument, then what is left, each as its own kind of text.
std::string FormatArguments(Context& ctx, ConsoleState& state, qe::Args args) {
  std::string out;
  size_t next = 0;
  if (!args.empty() && args[0].is_string()) {
    const std::string format = qe::ToWtf8(ctx, args[0]);
    next = 1;
    for (size_t i = 0; i < format.size(); ++i) {
      if (format[i] != '%' || i + 1 >= format.size()) {
        out += format[i];
        continue;
      }
      const char specifier = format[i + 1];
      if (specifier == '%') {
        out += '%';
        ++i;
        continue;
      }
      if (std::string("sdifoOc").find(specifier) == std::string::npos || next >= args.size()) {
        out += format[i];
        continue;
      }
      const Value& arg = args[next++];
      ++i;
      // The conversions are those of the standard: String(x), parseInt(x, 10), parseFloat(x).
      const auto call = [&](const std::shared_ptr<qe::Persistent>& function, Value argument, bool withRadix) {
        Value arguments[] = {argument, Value(10)};
        Value result = qe::Call(ctx, function->Get(), qe::Undefined(), qe::Args(arguments, withRadix ? 2 : 1));
        if (qe::HasException(ctx)) {
          ctx.clear_exception();
          return Value(std::nan(""));
        }
        return result;
      };
      switch (specifier) {
        case 's': {
          Value converted = call(state.stringFunction, arg, false);
          out += converted.is_string() ? qe::ToWtf8(ctx, converted) : InspectValue(ctx, arg, 0);
          break;
        }
        case 'd':
        case 'i':
          out += arg.is_symbol() ? "NaN" : Inspector::PrimitiveText(ctx, call(state.parseIntFunction, arg, true), false);
          break;
        case 'f':
          out += arg.is_symbol() ? "NaN" : Inspector::PrimitiveText(ctx, call(state.parseFloatFunction, arg, false), false);
          break;
        case 'o': out += InspectValue(ctx, arg, 4); break;
        case 'O': out += InspectValue(ctx, arg, 2); break;
        default: break;  // %c: the CSS is not for a text console
      }
    }
  }
  for (size_t i = next; i < args.size(); ++i) {
    if (!out.empty() || i > 0) out += ' ';
    out += args[i].is_string() ? qe::ToWtf8(ctx, args[i]) : InspectValue(ctx, args[i], 2);
  }
  return out;
}

void Print(ConsoleState& state, ConsoleLevel level, std::string text) {
  if (!state.groups.empty()) {
    const std::string indent(state.groups.size() * 2, ' ');
    std::string indented = indent;
    for (char c : text) {
      indented += c;
      if (c == '\n') indented += indent;
    }
    text = std::move(indented);
  }
  state.sink->Message(level, text);
}

// "Logger": nothing for no arguments, and the formatted text for the rest.
void Logger(ConsoleState& state, Context& ctx, ConsoleLevel level, qe::Args args) {
  if (args.empty()) return;
  Print(state, level, FormatArguments(ctx, state, args));
}

std::string Centered(const std::string& text, size_t width) {
  const size_t length = DisplayWidth(text);
  const size_t left = (width - length) / 2;
  return std::string(left, ' ') + text + std::string(width - length - left, ' ');
}

// console.table: the rows of an array or object as a box of cells.
bool Table(ConsoleState& state, Context& ctx, qe::Args args) {
  if (args.empty() || !qe::IsObject(args[0]) || qe::IsCallable(args[0])) return false;
  const Value& data = args[0];
  std::vector<std::string> columnsFilter;
  bool filtered = false;
  if (args.size() > 1 && qe::IsObject(args[1])) {
    filtered = true;
    const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, args[1], "length"));
    for (uint32_t i = 0; i < length; ++i) columnsFilter.push_back(qe::ToWtf8(ctx, qe::GetIndex(ctx, args[1], i)));
  }
  std::vector<std::pair<std::string, Value>> rows;  // index label and value
  const qe::ObjectInfo info = qe::Inspect(ctx, data);
  qe::ValueList keepRows;
  if (info.kind == qe::ObjectKind::Map) {
    qe::ValueList items = qe::MapEntries(data);
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
      rows.emplace_back(std::to_string(i / 2), items[i + 1]);
      keepRows.Append(items[i + 1]);
    }
  } else if (info.kind == qe::ObjectKind::Set) {
    qe::ValueList items = qe::SetValues(data);
    for (size_t i = 0; i < items.size(); ++i) {
      rows.emplace_back(std::to_string(i), items[i]);
      keepRows.Append(items[i]);
    }
  } else {
    for (const std::string& key : qe::OwnKeys(ctx, data)) {
      Value item = qe::Get(ctx, data, key);
      if (qe::HasException(ctx)) {
        ctx.clear_exception();
        continue;
      }
      keepRows.Append(item);
      rows.emplace_back(key, item);
    }
  }
  std::vector<std::string> columns;
  bool hasValues = false;
  std::vector<std::map<std::string, std::string>> cells(rows.size());
  std::vector<std::string> values(rows.size());
  for (size_t r = 0; r < rows.size(); ++r) {
    const Value& item = rows[r].second;
    if (qe::IsObject(item) && !qe::IsCallable(item)) {
      for (const std::string& key : qe::OwnKeys(ctx, item)) {
        if (filtered && std::find(columnsFilter.begin(), columnsFilter.end(), key) == columnsFilter.end()) continue;
        Value cell = qe::Get(ctx, item, key);
        if (qe::HasException(ctx)) {
          ctx.clear_exception();
          continue;
        }
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
        cells[r][key] = InspectValue(ctx, cell, 0);
      }
    } else {
      hasValues = true;
      values[r] = InspectValue(ctx, item, 0);
    }
  }
  if (filtered) columns = columnsFilter;
  std::vector<std::string> header = {"(index)"};
  header.insert(header.end(), columns.begin(), columns.end());
  if (hasValues) header.push_back("Values");
  std::vector<std::vector<std::string>> table;
  for (size_t r = 0; r < rows.size(); ++r) {
    std::vector<std::string> row = {rows[r].first};
    for (const std::string& column : columns) {
      const auto found = cells[r].find(column);
      row.push_back(found == cells[r].end() ? "" : found->second);
    }
    if (hasValues) row.push_back(values[r]);
    table.push_back(std::move(row));
  }
  std::vector<size_t> widths;
  for (const std::string& h : header) widths.push_back(DisplayWidth(h) + 2);
  for (const auto& row : table) {
    for (size_t c = 0; c < row.size(); ++c) widths[c] = std::max(widths[c], DisplayWidth(row[c]) + 2);
  }
  const auto rule = [&](const char* left, const char* middle, const char* right) {
    std::string line = left;
    for (size_t c = 0; c < widths.size(); ++c) {
      for (size_t i = 0; i < widths[c]; ++i) line += "─";
      line += c + 1 < widths.size() ? middle : right;
    }
    return line;
  };
  const auto line = [&](const std::vector<std::string>& cellsOfRow) {
    std::string text = "│";
    for (size_t c = 0; c < widths.size(); ++c) text += Centered(cellsOfRow[c], widths[c]) + "│";
    return text;
  };
  std::string out = rule("┌", "┬", "┐") + "\n" + line(header) + "\n" + rule("├", "┼", "┤") + "\n";
  for (const auto& row : table) out += line(row) + "\n";
  out += rule("└", "┴", "┘");
  Print(state, ConsoleLevel::Log, out);
  return true;
}

std::string Elapsed(std::chrono::steady_clock::time_point start) {
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  char buffer[32];
  if (ms >= 1000) std::snprintf(buffer, sizeof buffer, "%.3fs", ms / 1000);
  else std::snprintf(buffer, sizeof buffer, "%.3fms", ms);
  return buffer;
}

std::string Label(Context& ctx, qe::Args args) {
  if (args.empty() || args[0].is_undefined()) return "default";
  return qe::ToWtf8(ctx, args[0]);
}

template <typename Fn>
void Define(Context& ctx, const Value& console, const char* name, int length, Fn fn) {
  Value function = qe::NewFunction(ctx, name, length, std::move(fn));
  qe::Descriptor d;
  d.value = function;
  d.has_value = true;
  d.has_writable = d.writable = true;
  d.has_enumerable = d.enumerable = true;
  d.has_configurable = d.configurable = true;
  qe::DefineProperty(ctx, console, name, d);
}

template <typename Host>
void Install(Host& host, ConsoleSink* sink) {
  Context& ctx = host.GetContext();
  auto state = std::make_shared<ConsoleState>();
  state->sink = sink ? sink : DefaultConsoleSink();
  const Value global = qe::FromObject(ctx.get_global_object());
  state->stringFunction = std::make_shared<qe::Persistent>(ctx, qe::Get(ctx, global, "String"));
  state->parseIntFunction = std::make_shared<qe::Persistent>(ctx, qe::Get(ctx, global, "parseInt"));
  state->parseFloatFunction = std::make_shared<qe::Persistent>(ctx, qe::Get(ctx, global, "parseFloat"));
  Value console = qe::NewObject(ctx);

  const auto logAt = [state](ConsoleLevel level) {
    return [state, level](Context& c, Value, qe::Args args, Value) {
      Logger(*state, c, level, args);
      return qe::Undefined();
    };
  };
  Define(ctx, console, "log", 0, logAt(ConsoleLevel::Log));
  Define(ctx, console, "info", 0, logAt(ConsoleLevel::Info));
  Define(ctx, console, "debug", 0, logAt(ConsoleLevel::Debug));
  Define(ctx, console, "warn", 0, logAt(ConsoleLevel::Warn));
  Define(ctx, console, "error", 0, logAt(ConsoleLevel::Error));
  Define(ctx, console, "dirxml", 0, logAt(ConsoleLevel::Log));
  Define(ctx, console, "assert", 0, [state](Context& c, Value, qe::Args args, Value) {
    if (!args.empty() && args[0].to_boolean()) return qe::Undefined();
    std::vector<Value> data;
    for (size_t i = 1; i < args.size(); ++i) data.push_back(args[i]);
    if (data.empty()) {
      data.push_back(qe::FromUtf8(c, "Assertion failed"));
    } else if (data[0].is_string()) {
      data[0] = qe::FromWtf8(c, "Assertion failed: " + qe::ToWtf8(c, data[0]));
    } else {
      data.insert(data.begin(), qe::FromUtf8(c, "Assertion failed"));
    }
    Logger(*state, c, ConsoleLevel::Error, qe::Args(data.data(), data.size()));
    return qe::Undefined();
  });
  Define(ctx, console, "clear", 0, [state](Context&, Value, qe::Args, Value) {
    state->groups.clear();
    state->sink->Clear();
    return qe::Undefined();
  });
  Define(ctx, console, "dir", 0, [state](Context& c, Value, qe::Args args, Value) {
    int depth = 2;
    if (args.size() > 1 && qe::IsObject(args[1])) {
      Value d = qe::Get(c, args[1], "depth");
      if (d.is_null()) depth = -1;
      else if (d.is_number()) depth = std::isinf(d.as_number()) ? -1 : static_cast<int>(d.as_number());
    }
    Print(*state, ConsoleLevel::Log, InspectValue(c, args.empty() ? qe::Undefined() : args[0], depth));
    return qe::Undefined();
  });
  Define(ctx, console, "table", 0, [state](Context& c, Value, qe::Args args, Value) {
    if (!Table(*state, c, args)) Logger(*state, c, ConsoleLevel::Log, args);
    return qe::Undefined();
  });
  Define(ctx, console, "trace", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string message = args.empty() ? "" : FormatArguments(c, *state, args);
    // The stack is that of an error made here.
    Value errorConstructor = qe::Get(c, qe::FromObject(c.get_global_object()), "Error");
    Value error = qe::Construct(c, errorConstructor);
    const qe::ErrorInfo info = qe::InspectError(c, error);
    std::string text = "Trace" + (message.empty() ? std::string() : ": " + message);
    for (const qe::StackFrame& frame : info.frames) {
      text += "\n    at " + (frame.function.empty() ? std::string("<anonymous>") : frame.function) + " (" + frame.filename + ":" + std::to_string(frame.line) + ":" + std::to_string(frame.column) + ")";
    }
    Print(*state, ConsoleLevel::Trace, text);
    return qe::Undefined();
  });
  Define(ctx, console, "count", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string label = Label(c, args);
    Print(*state, ConsoleLevel::Log, label + ": " + std::to_string(++state->counts[label]));
    return qe::Undefined();
  });
  Define(ctx, console, "countReset", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string label = Label(c, args);
    if (!state->counts.count(label)) Print(*state, ConsoleLevel::Warn, "Count for '" + label + "' does not exist");
    else state->counts[label] = 0;
    return qe::Undefined();
  });
  const auto group = [state](Context& c, qe::Args args) {
    if (!args.empty()) Logger(*state, c, ConsoleLevel::Log, args);
    state->groups.push_back(args.empty() ? "console.group" : qe::ToWtf8(c, args[0]));
  };
  Define(ctx, console, "group", 0, [group](Context& c, Value, qe::Args args, Value) {
    group(c, args);
    return qe::Undefined();
  });
  Define(ctx, console, "groupCollapsed", 0, [group](Context& c, Value, qe::Args args, Value) {
    group(c, args);
    return qe::Undefined();
  });
  Define(ctx, console, "groupEnd", 0, [state](Context&, Value, qe::Args, Value) {
    if (!state->groups.empty()) state->groups.pop_back();
    return qe::Undefined();
  });
  Define(ctx, console, "time", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string label = Label(c, args);
    if (state->timers.count(label)) Print(*state, ConsoleLevel::Warn, "Timer '" + label + "' already exists");
    else state->timers[label] = std::chrono::steady_clock::now();
    return qe::Undefined();
  });
  Define(ctx, console, "timeLog", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string label = Label(c, args);
    const auto found = state->timers.find(label);
    if (found == state->timers.end()) {
      Print(*state, ConsoleLevel::Warn, "Timer '" + label + "' does not exist");
      return qe::Undefined();
    }
    std::string text = label + ": " + Elapsed(found->second);
    if (args.size() > 1) text += " " + FormatArguments(c, *state, args.subspan(1));
    Print(*state, ConsoleLevel::Log, text);
    return qe::Undefined();
  });
  Define(ctx, console, "timeEnd", 0, [state](Context& c, Value, qe::Args args, Value) {
    const std::string label = Label(c, args);
    const auto found = state->timers.find(label);
    if (found == state->timers.end()) {
      Print(*state, ConsoleLevel::Warn, "Timer '" + label + "' does not exist");
      return qe::Undefined();
    }
    Print(*state, ConsoleLevel::Log, label + ": " + Elapsed(found->second));
    state->timers.erase(found);
    return qe::Undefined();
  });

  qe::Set(ctx, qe::FromObject(ctx.get_global_object()), "console", console);
  // console is not enumerable on the global, and is a namespace object that says so.
  qe::Descriptor tag;
  tag.value = qe::FromUtf8(ctx, "console");
  tag.has_value = true;
  tag.has_writable = true;
  tag.has_enumerable = tag.has_configurable = true;
  tag.configurable = true;
  qe::Descriptor hidden;
  hidden.has_enumerable = true;
  hidden.enumerable = false;
  qe::DefineProperty(ctx, qe::FromObject(ctx.get_global_object()), "console", hidden);
  // Its prototype is an empty object, which has Object.prototype, as the standard has it for compatibility.
  host.Evaluate("Object.setPrototypeOf(console, Object.create(Object.prototype)); Object.defineProperty(console, Symbol.toStringTag, { value: 'console', configurable: true });", "console.js");
}

}  // namespace

ConsoleSink* DefaultConsoleSink() {
  static StdSink sink;
  return &sink;
}

void SetHostObjectFormatter(HostObjectFormatter formatter) { HostFormatter() = std::move(formatter); }

std::string InspectValue(Context& ctx, const Value& value, int depth) {
  Inspector inspector(ctx, depth);
  return inspector.Format(value, 0);
}

void InstallConsoleApi(qe::Realm& realm, ConsoleSink* sink) { Install(realm, sink); }
void InstallConsoleApi(qe::Runtime& runtime, ConsoleSink* sink) { Install(runtime, sink); }

}  // namespace solar::web
