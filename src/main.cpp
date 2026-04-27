#include "config/config.hpp"
#include "util/log.hpp"
#include "wm/window_manager.hpp"
#include "x11/connection.hpp"

#include <exception>

int main() {
  try {
    chiux::x11::Connection connection;
    const auto config_path = chiux::config::default_config_path();
    auto config = chiux::config::Config::load_or_default(config_path);
    if (config.needs_save) {
      try {
        config.save(config_path);
      } catch (const std::exception& e) {
        chiux::log::warn(std::string("config auto-save failed: ") + e.what());
      }
    }
    chiux::wm::WindowManager wm(connection, config, config_path);
    wm.run();
    return 0;
  } catch (const std::exception& e) {
    chiux::log::error(e.what());
    return 1;
  }
}
