// What the user has chosen: defaults, what is taken and what is refused, what is kept in the file, and what the address bar makes of what is typed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "solar/gui/Chrome.h"
#include "solar/gui/Settings.h"

using namespace solar::gui;
namespace fs = std::filesystem;

namespace {
int total = 0, failed = 0;
void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}
}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "solar-settings-test";
  fs::remove_all(root);
  const std::string file = (root / "settings.conf").string();
  setenv("SOLAR_SETTINGS_FILE", file.c_str(), 1);
  unsetenv("SOLAR_SYSTEM_TITLE");

  Settings& s = Settings::Instance();
  Check("the defaults", s.Get("theme") == "system" && s.Get("search") == "duckduckgo" && s.Get("start").empty() && s.Get("titlebar") == "app");
  Check("the start address is a new tab's page when none is chosen", s.StartAddress() == kNewTabAddress);
  Check("the system's bar is not wanted by default", !s.SystemTitleBar());

  Check("a theme is taken", s.Set("theme", "dark") && s.Get("theme") == "dark");
  Check("one that is none is not", !s.Set("theme", "purple") && s.Get("theme") == "dark");
  Check("a key that is none is not", !s.Set("nothing", "x") && s.Get("nothing").empty());
  Check("the tabs are along the top by default, and may be at the side", s.Get("tabs") == "top" && s.Set("tabs", "side") && s.Get("tabs") == "side" && s.Set("tabs", "sidebar") && s.Get("tabs") == "sidebar" && s.Set("tabs", "side") && !s.Set("tabs", "bottom") && s.Get("tabs") == "side");
  Check("a search engine is taken", s.Set("search", "google") && s.SearchAddress() == "https://www.google.com/search?q=");
  Check("an engine that is none is not", !s.Set("search", "evil") && s.Get("search") == "google");
  Check("a start page is made an address", s.Set("start", " example.org/x ") && s.Get("start") == "https://example.org/x" && s.StartAddress() == "https://example.org/x");
  Check("one that is no address is not (a script)", !s.Set("start", "javascript:alert(1)") && s.Get("start") == "https://example.org/x");
  Check("one with a line end is not", !s.Set("start", "https://a.test/\nsearch=evil") && s.Get("search") == "google");
  Check("the empty one is the new tab's page", s.Set("start", "") && s.StartAddress() == kNewTabAddress);
  Check("the system's bar is chosen", s.Set("titlebar", "system") && s.SystemTitleBar());
  setenv("SOLAR_SYSTEM_TITLE", "0", 1);
  Check("and the environment may say no", !s.SystemTitleBar());
  unsetenv("SOLAR_SYSTEM_TITLE");

  std::ifstream in(file);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Check("it is in the file, a line each", text.find("theme=dark\n") != std::string::npos && text.find("search=google\n") != std::string::npos && text.find("titlebar=system\n") != std::string::npos, text);
  Check("no temporary file is left", !fs::exists(file + ".tmp"));
  const std::string json = s.Json();
  Check("the page is told the settings and the engines", json.find("\"theme\":\"dark\"") != std::string::npos && json.find("\"id\":\"brave\"") != std::string::npos, json);

  const auto engine = s.SearchAddress();
  Check("words are a search", AddressFromInput("solar browser", engine) == "https://www.google.com/search?q=solar+browser");
  Check("a host is an address", AddressFromInput("example.com/a b", engine) != "" && AddressFromInput("example.com", engine) == "https://example.com");
  Check("an address has its scheme", AddressFromInput("http://a.test/", engine) == "http://a.test/" && AddressFromInput("localhost:8080/x", engine) == "http://localhost:8080/x" && AddressFromInput("192.168.1.1", engine) == "http://192.168.1.1");
  Check("the browser's own pages are addresses", AddressFromInput("solar://settings/", engine) == "solar://settings/");
  Check("the two layouts of the interface are interface addresses and nothing else is", IsInterfaceAddress(kChromeAddress) && IsInterfaceAddress(kSideTabsAddress) && IsInterfaceAddress(kSidebarAddress) && !IsInterfaceAddress(kSettingsAddress) && !IsInterfaceAddress(kNewTabAddress) && !IsInterfaceAddress("https://example.com/"));
  Check("what is not safe in a query is escaped", AddressFromInput("a&b=c", engine) == "https://www.google.com/search?q=a%26b%3Dc", AddressFromInput("a&b=c", engine));

  fs::remove_all(root);
  std::printf("settings: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
