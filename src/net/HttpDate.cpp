#include "solar/net/HttpDate.h"

#include <string>

namespace solar::net {

namespace {

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

std::string Lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c;
  return out;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int64_t year, int month, int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

}  // namespace

std::optional<std::chrono::system_clock::time_point> ParseHttpDate(std::string_view text) {
  std::optional<int> hour, minute, second, day, month, year;

  const auto isDelimiter = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u == 0x09 || (u >= 0x20 && u <= 0x2F) || (u >= 0x3B && u <= 0x40) || (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
  };
  // Up to `limit` digits from the start of `token`, and how many there were.
  const auto leadingNumber = [](std::string_view token, size_t limit, int& value) {
    size_t used = 0;
    value = 0;
    while (used < token.size() && used < limit && IsAsciiDigit(token[used])) value = value * 10 + (token[used++] - '0');
    return used;
  };

  static const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && isDelimiter(text[i])) ++i;
    const size_t start = i;
    while (i < text.size() && !isDelimiter(text[i])) ++i;
    const std::string_view token = text.substr(start, i - start);
    if (token.empty()) continue;

    if (!hour) {
      // hh:mm:ss, each of one or two digits, with anything after the last
      int h, m, s;
      size_t used = leadingNumber(token, 2, h);
      if (used > 0 && used < token.size() && token[used] == ':') {
        std::string_view rest = token.substr(used + 1);
        const size_t usedM = leadingNumber(rest, 2, m);
        if (usedM > 0 && usedM < rest.size() && rest[usedM] == ':') {
          rest.remove_prefix(usedM + 1);
          const size_t usedS = leadingNumber(rest, 2, s);
          if (usedS > 0 && (usedS == rest.size() || !IsAsciiDigit(rest[usedS]))) {
            hour = h;
            minute = m;
            second = s;
            continue;
          }
        }
      }
    }
    if (!day) {
      int d;
      const size_t used = leadingNumber(token, 2, d);
      if (used > 0 && (used == token.size() || !IsAsciiDigit(token[used]))) {
        day = d;
        continue;
      }
    }
    if (!month && token.size() >= 3) {
      const std::string lead = Lowered(token.substr(0, 3));
      bool found = false;
      for (int m = 0; m < 12 && !found; ++m) {
        if (lead == kMonths[m]) {
          month = m + 1;
          found = true;
        }
      }
      if (found) continue;
    }
    if (!year) {
      int y;
      const size_t used = leadingNumber(token, 4, y);
      if (used >= 2 && (used == token.size() || !IsAsciiDigit(token[used]))) {
        year = y;
        continue;
      }
    }
  }

  if (!hour || !day || !month || !year) return std::nullopt;
  if (*year >= 70 && *year <= 99) *year += 1900;
  else if (*year >= 0 && *year <= 69) *year += 2000;
  if (*day < 1 || *day > 31 || *year < 1601 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;

  const int64_t seconds = DaysFromCivil(*year, *month, *day) * 86400 + *hour * 3600 + *minute * 60 + *second;
  return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

}  // namespace solar::net
