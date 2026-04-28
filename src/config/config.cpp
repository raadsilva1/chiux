#include "config/config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <sstream>
#include <unistd.h>
#include <stdexcept>
#include <unordered_set>
#include <string_view>

namespace chiux::config {

namespace {

std::string trim(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(), value.end());
  return value;
}

std::vector<std::string> split(std::string_view value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : value) {
    if (c == delimiter) {
      parts.push_back(trim(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(trim(current));
  return parts;
}

template <typename T, typename KeyFn>
void dedupe_preserve_order(std::vector<T>& items, KeyFn key_fn) {
  std::unordered_set<std::string> seen;
  std::vector<T> unique;
  unique.reserve(items.size());
  for (const auto& item : items) {
    const std::string key = key_fn(item);
    if (seen.insert(key).second) {
      unique.push_back(item);
    }
  }
  items = std::move(unique);
}

template <typename T>
void move_entry_after_label(std::vector<T>& items, const std::string& label, const std::string& after_label) {
  const auto entry_it = std::find_if(items.begin(), items.end(), [&](const T& item) {
    return item.label == label;
  });
  const auto after_it = std::find_if(items.begin(), items.end(), [&](const T& item) {
    return item.label == after_label;
  });
  if (entry_it == items.end() || after_it == items.end()) {
    return;
  }
  if (entry_it == after_it + 1) {
    return;
  }
  T entry = *entry_it;
  const std::size_t target_index = static_cast<std::size_t>(std::distance(items.begin(), after_it)) + 1u;
  items.erase(entry_it);
  const std::size_t insert_index = std::min(target_index, items.size());
  items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(entry));
}

template <typename T>
void move_entry_before_label(std::vector<T>& items, const std::string& label, const std::string& before_label) {
  const auto entry_it = std::find_if(items.begin(), items.end(), [&](const T& item) {
    return item.label == label;
  });
  const auto before_it = std::find_if(items.begin(), items.end(), [&](const T& item) {
    return item.label == before_label;
  });
  if (entry_it == items.end() || before_it == items.end() || entry_it == before_it) {
    return;
  }
  if (entry_it + 1 == before_it) {
    return;
  }
  T entry = *entry_it;
  const std::size_t target_index = static_cast<std::size_t>(std::distance(items.begin(), before_it));
  items.erase(entry_it);
  const std::size_t insert_index = std::min(target_index, items.size());
  items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(entry));
}

std::string find_in_path(const char* name) {
  const char* path = std::getenv("PATH");
  if (!path || !*path) {
    return {};
  }
  std::string current;
  auto check_candidate = [&](const std::string& dir) -> std::string {
    if (dir.empty()) {
      return {};
    }
    std::filesystem::path candidate = std::filesystem::path(dir) / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
    return {};
  };
  for (char c : std::string(path)) {
    if (c == ':') {
      if (auto found = check_candidate(current); !found.empty()) {
        return found;
      }
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  return check_candidate(current);
}

std::string find_companion_executable(const char* name) {
  std::array<char, PATH_MAX> buffer{};
  const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len <= 0) {
    return {};
  }
  buffer[static_cast<std::size_t>(len)] = '\0';
  std::filesystem::path exe_path(buffer.data());
  if (!exe_path.has_parent_path()) {
    return {};
  }
  const std::filesystem::path candidate = exe_path.parent_path() / name;
  if (access(candidate.c_str(), X_OK) == 0) {
    return candidate.string();
  }
  return {};
}

std::string find_recursive_executable(const char* name) {
  std::vector<std::filesystem::path> roots;
  if (const char* home = std::getenv("HOME"); home && *home) {
    roots.emplace_back(std::filesystem::path(home) / ".local");
    roots.emplace_back(std::filesystem::path(home) / ".projects");
  }
  roots.emplace_back(std::filesystem::current_path());
  roots.emplace_back("/usr/local");
  roots.emplace_back("/opt");

  std::unordered_set<std::string> visited;
  constexpr int kMaxDepth = 4;
  for (const auto& root : roots) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec)) {
      continue;
    }
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it.depth() > kMaxDepth) {
        it.disable_recursion_pending();
        continue;
      }
      const auto& entry = *it;
      std::error_code status_ec;
      if (!entry.is_regular_file(status_ec) && !entry.is_symlink(status_ec)) {
        continue;
      }
      if (entry.path().filename() != name) {
        continue;
      }
      std::error_code access_ec;
      if (access(entry.path().c_str(), X_OK) != 0) {
        continue;
      }
      const std::string path = entry.path().string();
      if (visited.insert(path).second) {
        return path;
      }
    }
  }
  return {};
}

std::string resolve_executable(const std::string& command) {
  if (command.empty()) {
    return command;
  }
  const std::filesystem::path path(command);
  if (path.is_absolute() || command.find('/') != std::string::npos) {
    if (access(command.c_str(), X_OK) == 0) {
      return std::filesystem::weakly_canonical(path).string();
    }
    const std::string fallback_name = path.filename().string();
    if (!fallback_name.empty() && fallback_name != command) {
      if (const std::string companion = find_companion_executable(fallback_name.c_str()); !companion.empty()) {
        return companion;
      }
      if (const std::string found = find_in_path(fallback_name.c_str()); !found.empty()) {
        return found;
      }
      if (const std::string recursive = find_recursive_executable(fallback_name.c_str()); !recursive.empty()) {
        return recursive;
      }
    }
    return command;
  }

  if (const std::string companion = find_companion_executable(command.c_str()); !companion.empty()) {
    return companion;
  }
  if (const std::string found = find_in_path(command.c_str()); !found.empty()) {
    return found;
  }
  if (const std::string recursive = find_recursive_executable(command.c_str()); !recursive.empty()) {
    return recursive;
  }
  return command;
}

bool path_is_valid_executable(const std::string& command) {
  if (command.empty()) {
    return false;
  }
  return access(command.c_str(), X_OK) == 0;
}

std::string resolve_primary_terminal() {
  if (const std::string resolved = resolve_executable("chiux-te"); !resolved.empty() && resolved != "chiux-te") {
    return resolved;
  }
  if (const std::string terminator = resolve_executable("terminator"); !terminator.empty() && terminator != "terminator") {
    return terminator;
  }
  return "terminator";
}

std::string resolve_modern_terminal() {
  if (const std::string resolved = resolve_executable("chiux-te-2"); !resolved.empty() && resolved != "chiux-te-2") {
    return resolved;
  }
  return "chiux-te-2";
}

bool has_launcher_entry(const std::vector<Launcher>& launchers, const std::string& label) {
  return std::any_of(launchers.begin(), launchers.end(), [&](const Launcher& launcher) {
    return launcher.label == label;
  });
}

bool has_icon_entry(const std::vector<DesktopIcon>& icons, const std::string& label) {
  return std::any_of(icons.begin(), icons.end(), [&](const DesktopIcon& icon) {
    return icon.label == label;
  });
}

unsigned long parse_color(const std::string& value, unsigned long fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoul(value, nullptr, 0);
  } catch (...) {
    return fallback;
  }
}

unsigned int parse_uint(const std::string& value, unsigned int fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<unsigned int>(std::stoul(value, nullptr, 0));
  } catch (...) {
    return fallback;
  }
}

bool parse_bool(const std::string& value, bool fallback) {
  const std::string lowered = [&]() {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
  }();
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
    return false;
  }
  return fallback;
}

std::string canonical_color_token(std::string value) {
  if (value.empty()) {
    return value;
  }
  if (value.front() == '#') {
    for (char& ch : value) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
  }
  return value;
}

unsigned long parse_rgb_fallback(const std::string& value, unsigned long fallback) {
  if (value.empty()) {
    return fallback;
  }
  std::string token = value;
  if (!token.empty() && token.front() == '#') {
    token.erase(token.begin());
  } else if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) {
    token.erase(0, 2);
  }
  if (token.size() != 6) {
    return fallback;
  }
  try {
    return std::stoul(token, nullptr, 16) & 0xFFFFFFu;
  } catch (...) {
    return fallback;
  }
}

std::string color_token_from_rgb(unsigned long value) {
  std::ostringstream out;
  out << '#'
      << std::uppercase
      << std::hex
      << std::setw(6)
      << std::setfill('0')
      << static_cast<unsigned long>(value & 0xFFFFFFu);
  return out.str();
}

bool is_legacy_terminal_name(const std::string& command) {
  const std::string base = std::filesystem::path(command).filename().string();
  return base == "terminator" || base == "xterm";
}

bool is_home_icon_command(const std::string& label, const std::string& command) {
  std::string normalized_label;
  normalized_label.reserve(label.size());
  for (char ch : label) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      normalized_label.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }
  if (normalized_label != "home") {
    return false;
  }
  return command.find("xdg-open") != std::string::npos &&
         (command.find("$HOME") != std::string::npos || command.find('~') != std::string::npos);
}

Config defaults() {
  Config config;
  config.terminal_command = resolve_primary_terminal();
  config.terminal_feel = 0;
  const std::string modern_terminal_command = resolve_modern_terminal();
  config.launchers = {
      {"Terminal", config.terminal_command},
      {"Modern Terminal", modern_terminal_command},
      {"Files", config.file_browser_command},
      {"Config", config.terminal_command + " -x sh -lc 'exec ${EDITOR:-vi} \"$HOME/.config/chiux/config.ini\"'"},
  };
  config.desktop_icons = {
      {"Terminal", config.terminal_command, 0, 0},
      {"Modern Terminal", modern_terminal_command, 0, 0},
      {"Files", config.file_browser_command, 0, 0},
      {"Home", "chiux:open-home-info", 0, 0},
      {"Applications", "chiux:open-applications", 0, 0},
      {"Trash", "xdg-open \"$HOME/.local/share/Trash\"", 0, 0},
  };
  return config;
}

}  // namespace

std::filesystem::path default_config_path() {
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    return std::filesystem::path(".chiux.ini");
  }
  return std::filesystem::path(home) / ".config" / "chiux" / "config.ini";
}

Config Config::load_or_default(const std::filesystem::path& path) {
  Config config = defaults();
  config.needs_save = !std::filesystem::exists(path);
  std::ifstream input(path);
  if (!input) {
    if (config.resolve_terminal_binary && !path_is_valid_executable(config.terminal_command)) {
      config.terminal_command = resolve_executable("chiux-te");
      config.needs_save = true;
    }
    return config;
  }

  config.launchers.clear();
  config.desktop_icons.clear();
  config.applications.clear();
  bool background_color_specified = false;
  bool resolve_terminal_binary_specified = false;
  bool terminal_command_specified = false;
  bool terminal_feel_specified = false;

  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));
    if (key == "border_width") {
      config.border_width = parse_uint(value, config.border_width);
    } else if (key == "title_height") {
      config.title_height = parse_uint(value, config.title_height);
    } else if (key == "menu_height") {
      config.menu_height = parse_uint(value, config.menu_height);
    } else if (key == "grow_box_size") {
      config.grow_box_size = parse_uint(value, config.grow_box_size);
    } else if (key == "background_color") {
      config.background_color = canonical_color_token(value);
      background_color_specified = true;
    } else if (key == "background_pixel") {
      config.background_pixel = parse_color(value, config.background_pixel);
    } else if (key == "frame_active_pixel") {
      config.frame_active_pixel = parse_color(value, config.frame_active_pixel);
    } else if (key == "frame_inactive_pixel") {
      config.frame_inactive_pixel = parse_color(value, config.frame_inactive_pixel);
    } else if (key == "title_active_pixel") {
      config.title_active_pixel = parse_color(value, config.title_active_pixel);
    } else if (key == "title_inactive_pixel") {
      config.title_inactive_pixel = parse_color(value, config.title_inactive_pixel);
    } else if (key == "title_text_pixel") {
      config.title_text_pixel = parse_color(value, config.title_text_pixel);
    } else if (key == "menu_pixel") {
      config.menu_pixel = parse_color(value, config.menu_pixel);
    } else if (key == "menu_text_pixel") {
      config.menu_text_pixel = parse_color(value, config.menu_text_pixel);
    } else if (key == "resolve_terminal_binary") {
      config.resolve_terminal_binary = parse_bool(value, config.resolve_terminal_binary);
      resolve_terminal_binary_specified = true;
    } else if (key == "terminal_command") {
      config.terminal_command = value;
      terminal_command_specified = true;
    } else if (key == "terminal_feel") {
      config.terminal_feel = parse_uint(value, config.terminal_feel);
      if (config.terminal_feel >= kTerminalFeelThemeCount) {
        config.terminal_feel = 0;
      }
      terminal_feel_specified = true;
    } else if (key == "file_browser_command") {
      config.file_browser_command = value;
    } else if (key == "launcher") {
      auto parts = split(value, '|');
      if (parts.size() >= 2) {
        config.launchers.push_back({parts[0], parts[1]});
      }
    } else if (key == "icon") {
      auto parts = split(value, '|');
      if (parts.size() >= 4) {
        config.desktop_icons.push_back({parts[0], parts[1], static_cast<int>(parse_uint(parts[2], 0)), static_cast<int>(parse_uint(parts[3], 0))});
      }
    } else if (key == "application") {
      auto parts = split(value, '|');
      if (parts.size() >= 4) {
        config.applications.push_back({
            parts[0],
            parts[1],
            static_cast<int>(parse_uint(parts[2], 0)),
            static_cast<int>(parse_uint(parts[3], 0)),
            parts.size() >= 5 ? parse_uint(parts[4], 0) : 0u});
      }
    }
  }

  if (config.file_browser_command.empty() || config.file_browser_command == "xdg-open ~") {
    config.file_browser_command = defaults().file_browser_command;
    config.needs_save = true;
  }
  if (!terminal_command_specified || config.terminal_command.empty() ||
      config.terminal_command == "xterm" || config.terminal_command == "terminator" ||
      is_legacy_terminal_name(config.terminal_command)) {
    config.terminal_command = defaults().terminal_command;
    config.needs_save = true;
  }
  if (!resolve_terminal_binary_specified) {
    config.resolve_terminal_binary = defaults().resolve_terminal_binary;
  }
  if (!terminal_feel_specified) {
    config.terminal_feel = 0;
  }
  for (auto& launcher : config.launchers) {
    if (launcher.command == "xterm" || launcher.command == "terminator") {
      launcher.command = config.terminal_command;
      config.needs_save = true;
    }
    if (launcher.label == "Modern Terminal") {
      const std::string modern_terminal = resolve_modern_terminal();
      if (!modern_terminal.empty() && launcher.command != modern_terminal) {
        launcher.command = modern_terminal;
        config.needs_save = true;
      }
    }
  }
  for (auto& icon : config.desktop_icons) {
    if (icon.command == "xterm" || icon.command == "terminator") {
      icon.command = config.terminal_command;
      config.needs_save = true;
    }
    if (is_home_icon_command(icon.label, icon.command)) {
      icon.command = "chiux:open-home-info";
      config.needs_save = true;
    }
    if (icon.command == "xdg-open ~") {
      icon.command = config.file_browser_command;
      config.needs_save = true;
    }
    if (icon.label == "Applications" && icon.command != "chiux:open-applications") {
      icon.command = "chiux:open-applications";
      config.needs_save = true;
    }
    if (icon.label == "Modern Terminal") {
      const std::string modern_terminal = resolve_modern_terminal();
      if (!modern_terminal.empty() && icon.command != modern_terminal) {
        icon.command = modern_terminal;
        config.needs_save = true;
      }
    }
  }

  if (config.launchers.empty()) {
    config.launchers = defaults().launchers;
  } else if (!has_launcher_entry(config.launchers, "Modern Terminal")) {
    const auto terminal_it = std::find_if(config.launchers.begin(), config.launchers.end(), [](const Launcher& launcher) {
      return launcher.label == "Terminal";
    });
    if (terminal_it != config.launchers.end()) {
      config.launchers.insert(terminal_it + 1, {"Modern Terminal", resolve_modern_terminal()});
    } else {
      config.launchers.push_back({"Modern Terminal", resolve_modern_terminal()});
    }
    config.needs_save = true;
  }
  if (config.desktop_icons.empty()) {
    config.desktop_icons = defaults().desktop_icons;
  } else {
    if (!has_icon_entry(config.desktop_icons, "Modern Terminal")) {
      const auto terminal_it = std::find_if(config.desktop_icons.begin(), config.desktop_icons.end(), [](const DesktopIcon& icon) {
        return icon.label == "Terminal";
      });
      if (terminal_it != config.desktop_icons.end()) {
        config.desktop_icons.insert(terminal_it + 1, {"Modern Terminal", resolve_modern_terminal(), 0, 0});
      } else {
        config.desktop_icons.push_back({"Modern Terminal", resolve_modern_terminal(), 0, 0});
      }
      config.needs_save = true;
    }
    if (!has_icon_entry(config.desktop_icons, "Applications")) {
      const auto trash_it = std::find_if(config.desktop_icons.begin(), config.desktop_icons.end(), [](const DesktopIcon& icon) {
        return icon.label == "Trash";
      });
      if (trash_it != config.desktop_icons.end()) {
        config.desktop_icons.insert(trash_it, {"Applications", "chiux:open-applications", 0, 0});
      } else {
        config.desktop_icons.push_back({"Applications", "chiux:open-applications", 0, 0});
      }
      config.needs_save = true;
    }
  }

  if (!background_color_specified) {
    config.background_color = color_token_from_rgb(config.background_pixel);
  }
  config.background_color = canonical_color_token(config.background_color);
  if (!config.background_color.empty() && config.background_color.front() == '#') {
    config.background_pixel = parse_rgb_fallback(config.background_color, config.background_pixel);
  }

  if (config.resolve_terminal_binary) {
    const std::string resolved_terminal = resolve_executable(config.terminal_command);
    if (resolved_terminal != config.terminal_command) {
      config.terminal_command = resolved_terminal;
      config.needs_save = true;
    }
    if (is_legacy_terminal_name(config.terminal_command)) {
      const std::string preferred = resolve_executable("chiux-te");
      if (!preferred.empty() && preferred != "chiux-te" && preferred != config.terminal_command) {
        config.terminal_command = preferred;
        config.needs_save = true;
      }
    }
    for (auto& launcher : config.launchers) {
      if (is_legacy_terminal_name(launcher.command) || launcher.command == "chiux-te") {
        if (launcher.command != config.terminal_command) {
          launcher.command = config.terminal_command;
          config.needs_save = true;
        }
      } else if (launcher.label == "Modern Terminal") {
        const std::string modern_terminal = resolve_modern_terminal();
        if (!modern_terminal.empty() && launcher.command != modern_terminal) {
          launcher.command = modern_terminal;
          config.needs_save = true;
        }
      } else {
        const std::string resolved = resolve_executable(launcher.command);
        if (resolved != launcher.command) {
          launcher.command = resolved;
          config.needs_save = true;
        }
      }
    }
    for (auto& icon : config.desktop_icons) {
      if (is_legacy_terminal_name(icon.command) || icon.command == "chiux-te") {
        if (icon.command != config.terminal_command) {
          icon.command = config.terminal_command;
          config.needs_save = true;
        }
      } else if (icon.label == "Modern Terminal") {
        const std::string modern_terminal = resolve_modern_terminal();
        if (!modern_terminal.empty() && icon.command != modern_terminal) {
          icon.command = modern_terminal;
          config.needs_save = true;
        }
      } else {
        const std::string resolved = resolve_executable(icon.command);
        if (resolved != icon.command) {
          icon.command = resolved;
          config.needs_save = true;
        }
      }
    }
  }

  dedupe_preserve_order(config.launchers, [](const Launcher& launcher) {
    return launcher.label + "\x1f" + launcher.command;
  });
  dedupe_preserve_order(config.desktop_icons, [](const DesktopIcon& icon) {
    return icon.label + "\x1f" + icon.command;
  });
  dedupe_preserve_order(config.applications, [](const ApplicationEntry& entry) {
    return entry.label + "\x1f" + entry.command;
  });
  move_entry_after_label(config.launchers, "Modern Terminal", "Terminal");
  move_entry_after_label(config.desktop_icons, "Modern Terminal", "Terminal");
  move_entry_before_label(config.desktop_icons, "Applications", "Trash");

  return config;
}

void Config::save(const std::filesystem::path& path) const {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open chiux config for writing");
  }

  output << "# chiux config\n";
  output << "border_width=" << border_width << '\n';
  output << "title_height=" << title_height << '\n';
  output << "menu_height=" << menu_height << '\n';
  output << "grow_box_size=" << grow_box_size << '\n';
  output << "background_color=" << background_color << '\n';
  output << "background_pixel=" << background_pixel << '\n';
  output << "frame_active_pixel=" << frame_active_pixel << '\n';
  output << "frame_inactive_pixel=" << frame_inactive_pixel << '\n';
  output << "title_active_pixel=" << title_active_pixel << '\n';
  output << "title_inactive_pixel=" << title_inactive_pixel << '\n';
  output << "title_text_pixel=" << title_text_pixel << '\n';
  output << "menu_pixel=" << menu_pixel << '\n';
  output << "menu_text_pixel=" << menu_text_pixel << '\n';
  output << "resolve_terminal_binary=" << (resolve_terminal_binary ? 1 : 0) << '\n';
  output << "terminal_command=" << terminal_command << '\n';
  output << "terminal_feel=" << terminal_feel << '\n';
  output << "file_browser_command=" << file_browser_command << '\n';
  for (const auto& launcher : launchers) {
    output << "launcher=" << launcher.label << '|' << launcher.command << '\n';
  }
  for (const auto& icon : desktop_icons) {
    output << "icon=" << icon.label << '|' << icon.command << '|' << icon.x << '|' << icon.y << '\n';
  }
  for (const auto& entry : applications) {
    output << "application=" << entry.label << '|' << entry.command << '|' << entry.x << '|' << entry.y << '|' << entry.style << '\n';
  }
}

}
