#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace chiux::config {

struct Launcher {
  std::string label;
  std::string command;
};

struct DesktopIcon {
  std::string label;
  std::string command;
  int x = 0;
  int y = 0;
};

struct Config {
  unsigned int border_width = 1;
  unsigned int title_height = 20;
  unsigned int menu_height = 20;
  unsigned int grow_box_size = 14;
  std::string background_color = "#C0C0C0";
  unsigned long background_pixel = 0xC0C0C0;
  unsigned long frame_active_pixel = 0x000000;
  unsigned long frame_inactive_pixel = 0x808080;
  unsigned long title_active_pixel = 0x000000;
  unsigned long title_inactive_pixel = 0x404040;
  unsigned long title_text_pixel = 0xFFFFFF;
  unsigned long menu_pixel = 0xD8D8D8;
  unsigned long menu_text_pixel = 0x000000;
  bool resolve_terminal_binary = true;
  std::string terminal_command = "terminator";
  std::string file_browser_command = "chiux:open-file-manager";
  std::vector<Launcher> launchers;
  std::vector<DesktopIcon> desktop_icons;
  bool needs_save = false;

  static Config load_or_default(const std::filesystem::path& path);
  void save(const std::filesystem::path& path) const;
};

std::filesystem::path default_config_path();

}
