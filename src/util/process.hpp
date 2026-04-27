#pragma once

#include <string>

namespace chiux::util {

int spawn_command(const std::string& command);
void reap_children();

}

