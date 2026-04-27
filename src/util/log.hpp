#pragma once

#include <cstdio>
#include <string_view>

namespace chiux::log {

void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);

}

