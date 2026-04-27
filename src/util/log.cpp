#include "util/log.hpp"

#include <iostream>

namespace chiux::log {

static void write(const char* level, std::string_view message) {
  std::cerr << "[chiux] " << level << ": " << message << '\n';
}

void info(std::string_view message) { write("info", message); }
void warn(std::string_view message) { write("warn", message); }
void error(std::string_view message) { write("error", message); }

}

