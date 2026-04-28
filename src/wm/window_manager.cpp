#include "wm/window_manager.hpp"

#include "util/process.hpp"
#include "util/log.hpp"

#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h>
#include <png.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <pwd.h>
#include <sys/select.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <errno.h>
#include <memory>
#include <unistd.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>
#include <stdexcept>
#include <chrono>
#include <ctime>
#include <limits.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

namespace chiux::wm {

namespace {

int last_x_error = 0;

int x_error_handler(Display* display, XErrorEvent* event) {
  char text[256] = {};
  XGetErrorText(display, event->error_code, text, sizeof(text));
  chiux::log::error(std::string("X11 error: ") + text);
  last_x_error = event->error_code;
  return 0;
}

bool is_top_level_candidate(const XWindowAttributes& attrs) {
  return attrs.map_state != IsUnmapped && attrs.override_redirect == False;
}

std::filesystem::path home_directory() {
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::filesystem::path(home);
  }
  return std::filesystem::current_path();
}

std::string wrap_terminal_command(const std::string& terminal, const std::string& command) {
  const std::string terminal_name = std::filesystem::path(terminal).filename().string();
  if (terminal_name == "chiux-te") {
    return terminal + " -e '" + command + "'";
  }
  if (terminal_name == "chiux-te-2") {
    return terminal + " -e '" + command + "'";
  }
  if (terminal_name == "terminator") {
    return terminal + " -x sh -lc '" + command + "'";
  }
  return terminal + " -e sh -lc '" + command + "'";
}

std::string human_bytes(unsigned long long value) {
  static const char* const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double size = static_cast<double>(value);
  std::size_t unit = 0;
  while (size >= 1024.0 && unit + 1 < std::size(units)) {
    size /= 1024.0;
    ++unit;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << size << ' ' << units[unit];
  return out.str();
}

std::string format_duration(long long seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  const long long days = seconds / 86400;
  seconds %= 86400;
  const long long hours = seconds / 3600;
  seconds %= 3600;
  const long long minutes = seconds / 60;
  seconds %= 60;
  std::ostringstream out;
  if (days > 0) {
    out << days << "d ";
  }
  if (hours > 0 || days > 0) {
    out << hours << "h ";
  }
  out << minutes << "m " << seconds << 's';
  return out.str();
}

std::string format_time_point(const std::chrono::system_clock::time_point& tp) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&raw, &tm);
  char buffer[128] = {};
  if (std::strftime(buffer, sizeof(buffer), "%a %b %d %Y  %H:%M", &tm) == 0) {
    return {};
  }
  return buffer;
}

struct DiskSnapshot {
  std::string label;
  std::string path;
  std::string summary;
};

DiskSnapshot sample_disk(const std::string& label, const std::filesystem::path& path) {
  DiskSnapshot snapshot{label, path.string(), "unavailable"};
  struct statvfs stats {};
  if (statvfs(path.c_str(), &stats) != 0) {
    return snapshot;
  }
  const unsigned long long total = static_cast<unsigned long long>(stats.f_blocks) * static_cast<unsigned long long>(stats.f_frsize);
  const unsigned long long free = static_cast<unsigned long long>(stats.f_bfree) * static_cast<unsigned long long>(stats.f_frsize);
  const unsigned long long used = total > free ? total - free : 0;
  const double percent = total > 0 ? (static_cast<double>(used) * 100.0 / static_cast<double>(total)) : 0.0;
  std::ostringstream out;
  out << human_bytes(used) << " used / " << human_bytes(total) << " total (" << std::fixed << std::setprecision(0) << percent << "%)";
  snapshot.summary = out.str();
  return snapshot;
}

std::string shorten_label(const std::string& text, std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }
  return text.substr(0, max_chars - 3) + "...";
}

std::string normalize_menu_key(std::string text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }
  return out;
}

bool is_generic_launcher_label(const std::string& label) {
  const std::string key = normalize_menu_key(label);
  return key == "terminal" || key == "modernterminal" || key == "newterminal" || key == "filebrowser" || key == "filemanager" || key == "files" || key == "config" || key == "openconfig";
}

struct ApplicationsLayout {
  int inset = 16;
  int title_y = 0;
  int form_x = 0;
  int form_y = 0;
  unsigned int form_w = 0;
  unsigned int form_h = 0;
  int header_y = 0;
  unsigned int header_h = 28;
  int toggle_x = 0;
  int toggle_y = 0;
  unsigned int toggle_w = 18;
  unsigned int toggle_h = 18;
  int field_inset = 0;
  int name_x = 0;
  int name_y = 0;
  unsigned int name_w = 0;
  unsigned int field_h = 24;
  int command_x = 0;
  int command_y = 0;
  unsigned int command_w = 0;
  int button_y = 0;
  unsigned int button_w = 72;
  unsigned int button_h = 22;
  int open_x = 0;
  int add_x = 0;
  int update_x = 0;
  int remove_x = 0;
  int clear_x = 0;
  int style_label_y = 0;
  int style_row_y = 0;
  int style_x = 0;
  unsigned int style_chip_w = 58;
  unsigned int style_chip_h = 18;
  int panel_x = 0;
  int panel_y = 0;
  unsigned int panel_w = 0;
  unsigned int panel_h = 0;
  int footer_y = 0;
};

constexpr unsigned int kApplicationStyleCount = 4;

ApplicationsLayout applications_layout_for(unsigned int width, unsigned int height, bool collapsed) {
  ApplicationsLayout layout;
  layout.title_y = 30;
  layout.form_x = layout.inset;
  layout.form_y = 52;
  layout.form_w = width > static_cast<unsigned int>(layout.inset * 2) ? width - static_cast<unsigned int>(layout.inset * 2) : 1u;
  layout.header_y = layout.form_y + 6;
  layout.toggle_x = layout.form_x + static_cast<int>(layout.form_w) - 30;
  layout.toggle_y = layout.header_y + 4;
  layout.field_inset = 16;
  layout.name_x = layout.form_x + layout.field_inset;
  layout.name_y = layout.header_y + static_cast<int>(layout.header_h) + 18;
  layout.name_w = layout.form_w > static_cast<unsigned int>(layout.field_inset * 2) ? layout.form_w - static_cast<unsigned int>(layout.field_inset * 2) : 120u;
  layout.command_x = layout.name_x;
  layout.command_y = layout.name_y + static_cast<int>(layout.field_h) + 26;
  layout.command_w = layout.name_w;
  const unsigned int button_gap = 8;
  const unsigned int button_count = 5;
  const unsigned int button_area_w = layout.form_w > static_cast<unsigned int>(layout.field_inset * 2) ? layout.form_w - static_cast<unsigned int>(layout.field_inset * 2) : 1u;
  const unsigned int ideal_button_w = button_area_w > button_gap * (button_count - 1) ? (button_area_w - button_gap * (button_count - 1)) / button_count : 56u;
  layout.button_w = std::clamp(ideal_button_w, 56u, 84u);
  layout.button_y = layout.command_y + static_cast<int>(layout.field_h) + 22;
  layout.open_x = layout.form_x + layout.field_inset;
  layout.add_x = layout.open_x + static_cast<int>(layout.button_w + button_gap);
  layout.update_x = layout.add_x + static_cast<int>(layout.button_w + button_gap);
  layout.remove_x = layout.update_x + static_cast<int>(layout.button_w + button_gap);
  layout.clear_x = layout.remove_x + static_cast<int>(layout.button_w + button_gap);
  layout.style_label_y = layout.button_y + static_cast<int>(layout.button_h) + 20;
  layout.style_row_y = layout.style_label_y + 10;
  layout.style_x = layout.form_x + layout.field_inset;
  const unsigned int chip_gap = 6;
  const unsigned int chip_count = kApplicationStyleCount;
  const unsigned int chip_area_w = layout.form_w > static_cast<unsigned int>(layout.field_inset * 2) ? layout.form_w - static_cast<unsigned int>(layout.field_inset * 2) : 1u;
  const unsigned int ideal_chip_w = chip_area_w > chip_gap * (chip_count - 1) ? (chip_area_w - chip_gap * (chip_count - 1)) / chip_count : 52u;
  layout.style_chip_w = std::clamp(ideal_chip_w, 52u, 88u);
  if (collapsed) {
    layout.form_h = layout.header_h + 14;
  } else {
    layout.form_h = static_cast<unsigned int>((layout.style_row_y + static_cast<int>(layout.style_chip_h) + 18) - layout.form_y);
  }
  layout.panel_x = layout.inset;
  layout.panel_y = layout.form_y + static_cast<int>(layout.form_h) + 36;
  layout.panel_w = width > static_cast<unsigned int>(layout.inset * 2) ? width - static_cast<unsigned int>(layout.inset * 2) : 1u;
  layout.panel_h = height > static_cast<unsigned int>(layout.panel_y + 40) ? height - static_cast<unsigned int>(layout.panel_y + 40) : 1u;
  layout.footer_y = static_cast<int>(height) - 14;
  return layout;
}

const char* application_style_name(unsigned int style) {
  switch (style % kApplicationStyleCount) {
    case 0: return "Document";
    case 1: return "Tool";
    case 2: return "Folder";
    default: return "Monitor";
  }
}

constexpr int kApplicationsIconWidth = 88;
constexpr int kApplicationsIconHeight = 72;
XFontStruct* load_about_title_font(Display* display) {
  static XFontStruct* font = nullptr;
  static bool attempted = false;
  if (attempted) {
    return font;
  }
  attempted = true;
  static const char* candidates[] = {
      "10x20",
      "9x15bold",
      "9x15",
  };
  for (const char* candidate : candidates) {
    font = XLoadQueryFont(display, candidate);
    if (font) {
      break;
    }
  }
  return font;
}

XFontStruct* load_about_body_font(Display* display) {
  static XFontStruct* font = nullptr;
  static bool attempted = false;
  if (attempted) {
    return font;
  }
  attempted = true;
  static const char* candidates[] = {
      "9x15",
      "fixed",
  };
  for (const char* candidate : candidates) {
    font = XLoadQueryFont(display, candidate);
    if (font) {
      break;
    }
  }
  return font;
}

int text_width(XFontStruct* font, const std::string& text) {
  if (!font || text.empty()) {
    return 0;
  }
  return XTextWidth(font, text.c_str(), static_cast<int>(text.size()));
}

std::vector<std::string> wrap_text_to_width(XFontStruct* font, const std::string& text, int max_width) {
  if (text.empty()) {
    return {};
  }
  if (!font || max_width <= 0) {
    return {text};
  }

  std::istringstream words(text);
  std::vector<std::string> lines;
  std::string word;
  std::string line;
  while (words >> word) {
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || text_width(font, candidate) <= max_width) {
      line = candidate;
      continue;
    }
    lines.push_back(line);
    line = word;
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  if (lines.empty()) {
    lines.push_back(text);
  }
  return lines;
}

std::filesystem::path executable_directory() {
  std::array<char, PATH_MAX> buffer{};
  const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len > 0) {
    buffer[static_cast<std::size_t>(len)] = '\0';
    std::filesystem::path path(buffer.data());
    if (path.has_parent_path()) {
      return path.parent_path();
    }
  }
  return std::filesystem::current_path();
}

std::string resolve_modern_terminal_command() {
  const std::filesystem::path exe_dir = executable_directory();
  const std::vector<std::filesystem::path> candidates = {
      exe_dir / "chiux-te-2",
      exe_dir / "../bin/chiux-te-2",
      "/usr/local/bin/chiux-te-2",
      "/usr/bin/chiux-te-2",
  };
  for (const auto& candidate : candidates) {
    if (::access(candidate.c_str(), X_OK) == 0) {
      return std::filesystem::weakly_canonical(candidate).string();
    }
  }
  return "chiux-te-2";
}

std::optional<std::filesystem::path> locate_resource_path(const std::filesystem::path& relative) {
  std::vector<std::filesystem::path> candidates;
  if (const char* data_dir = std::getenv("CHIUX_DATA_DIR"); data_dir && *data_dir) {
    candidates.emplace_back(data_dir);
  }
  const std::filesystem::path exe_dir = executable_directory();
  candidates.push_back(exe_dir / ".." / "res");
  candidates.push_back(exe_dir / ".." / "share" / "chiux");
  candidates.push_back(exe_dir / ".." / "share");
  candidates.push_back(exe_dir);
  candidates.push_back(std::filesystem::current_path());
  for (const auto& base : candidates) {
    std::error_code ec;
    const std::filesystem::path candidate = (base / relative).lexically_normal();
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

unsigned long scale_to_mask(unsigned int value, unsigned long mask) {
  if (mask == 0) {
    return 0;
  }
  unsigned int shift = 0;
  while (((mask >> shift) & 1u) == 0u && shift < sizeof(unsigned long) * 8u) {
    ++shift;
  }
  unsigned long scaled_mask = mask >> shift;
  unsigned long max_value = scaled_mask;
  if (max_value == 0) {
    return 0;
  }
  return ((static_cast<unsigned long>(value) * max_value + 127u) / 255u) << shift;
}

unsigned long rgba_to_pixel(const XVisualInfo& visual_info, unsigned int r, unsigned int g, unsigned int b) {
  return scale_to_mask(r, visual_info.red_mask) |
         scale_to_mask(g, visual_info.green_mask) |
         scale_to_mask(b, visual_info.blue_mask);
}

struct PngImage {
  unsigned int width = 0;
  unsigned int height = 0;
  std::vector<unsigned char> pixels;
};

std::optional<PngImage> load_png_image(const std::filesystem::path& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    return std::nullopt;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) {
    std::fclose(fp);
    return std::nullopt;
  }
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    std::fclose(fp);
    return std::nullopt;
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return std::nullopt;
  }

  png_init_io(png, fp);
  png_read_info(png, info);

  png_uint_32 width = png_get_image_width(png, info);
  png_uint_32 height = png_get_image_height(png, info);
  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth = png_get_bit_depth(png, info);

  if (bit_depth == 16) {
    png_set_strip_16(png);
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
  std::vector<png_bytep> rows(static_cast<std::size_t>(height));
  for (unsigned int y = 0; y < height; ++y) {
    rows[y] = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
  }
  png_read_image(png, rows.data());
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(fp);

  PngImage image{};
  image.width = static_cast<unsigned int>(width);
  image.height = static_cast<unsigned int>(height);
  image.pixels = std::move(pixels);
  return image;
}

unsigned long rgb_fallback_from_token(const std::string& token, unsigned long fallback) {
  if (token.empty()) {
    return fallback;
  }
  std::string value = token;
  if (!value.empty() && value.front() == '#') {
    value.erase(value.begin());
  } else if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
    value.erase(0, 2);
  }
  if (value.size() != 6) {
    return fallback;
  }
  try {
    return std::stoul(value, nullptr, 16) & 0xFFFFFFu;
  } catch (...) {
    return fallback;
  }
}

unsigned long resolve_color_pixel(Display* display, const std::string& token, unsigned long fallback) {
  if (!display || token.empty()) {
    return fallback;
  }
  XColor color{};
  if (XParseColor(display, DefaultColormap(display, DefaultScreen(display)), token.c_str(), &color) && XAllocColor(display, DefaultColormap(display, DefaultScreen(display)), &color)) {
    return color.pixel;
  }
  return rgb_fallback_from_token(token, fallback);
}

struct Rgb {
  unsigned int r = 0;
  unsigned int g = 0;
  unsigned int b = 0;
};

Rgb parse_rgb_token(const std::string& token, unsigned long fallback) {
  const unsigned long packed = rgb_fallback_from_token(token, fallback);
  return {
      static_cast<unsigned int>((packed >> 16) & 0xFFu),
      static_cast<unsigned int>((packed >> 8) & 0xFFu),
      static_cast<unsigned int>(packed & 0xFFu),
  };
}

double linear_channel(unsigned int value) {
  const double normalized = static_cast<double>(value) / 255.0;
  if (normalized <= 0.03928) {
    return normalized / 12.92;
  }
  return std::pow((normalized + 0.055) / 1.055, 2.4);
}

double relative_luminance(const Rgb& rgb) {
  return 0.2126 * linear_channel(rgb.r) + 0.7152 * linear_channel(rgb.g) + 0.0722 * linear_channel(rgb.b);
}

double contrast_ratio(const Rgb& a, const Rgb& b) {
  const double la = relative_luminance(a);
  const double lb = relative_luminance(b);
  const double lighter = std::max(la, lb);
  const double darker = std::min(la, lb);
  return (lighter + 0.05) / (darker + 0.05);
}

Rgb mix_rgb(const Rgb& a, const Rgb& b, double weight_b) {
  const double weight_a = 1.0 - weight_b;
  return {
      static_cast<unsigned int>(std::clamp(a.r * weight_a + b.r * weight_b, 0.0, 255.0)),
      static_cast<unsigned int>(std::clamp(a.g * weight_a + b.g * weight_b, 0.0, 255.0)),
      static_cast<unsigned int>(std::clamp(a.b * weight_a + b.b * weight_b, 0.0, 255.0)),
  };
}

unsigned long rgb_to_pixel(const Rgb& rgb) {
  return (static_cast<unsigned long>(rgb.r) << 16) |
         (static_cast<unsigned long>(rgb.g) << 8) |
         static_cast<unsigned long>(rgb.b);
}

struct ContrastPalette {
  unsigned long text_pixel = 0;
  unsigned long shadow_pixel = 0;
};

ContrastPalette derive_icon_palette(const std::string& background_token, unsigned long fallback_pixel) {
  const Rgb background = parse_rgb_token(background_token, fallback_pixel);
  const Rgb black{0, 0, 0};
  const Rgb white{255, 255, 255};
  const bool use_dark_text = contrast_ratio(background, black) >= contrast_ratio(background, white);
  const Rgb text = use_dark_text ? black : white;
  const Rgb shadow = use_dark_text
      ? mix_rgb(background, black, 0.55)
      : mix_rgb(background, white, 0.55);
  return {rgb_to_pixel(text), rgb_to_pixel(shadow)};
}

std::string color_token_from_rgb(unsigned int r, unsigned int g, unsigned int b) {
  std::ostringstream out;
  out << '#'
      << std::uppercase
      << std::hex
      << std::setw(2)
      << std::setfill('0')
      << (r & 0xFFu)
      << std::setw(2)
      << (g & 0xFFu)
      << std::setw(2)
      << (b & 0xFFu);
  return out.str();
}

std::array<unsigned int, 3> hsv_to_rgb(double h, double s, double v) {
  const double c = v * s;
  const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
  const double m = v - c;
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  if (h < 60.0) {
    r = c; g = x; b = 0.0;
  } else if (h < 120.0) {
    r = x; g = c; b = 0.0;
  } else if (h < 180.0) {
    r = 0.0; g = c; b = x;
  } else if (h < 240.0) {
    r = 0.0; g = x; b = c;
  } else if (h < 300.0) {
    r = x; g = 0.0; b = c;
  } else {
    r = c; g = 0.0; b = x;
  }
  return {
      static_cast<unsigned int>(std::clamp((r + m) * 255.0, 0.0, 255.0)),
      static_cast<unsigned int>(std::clamp((g + m) * 255.0, 0.0, 255.0)),
      static_cast<unsigned int>(std::clamp((b + m) * 255.0, 0.0, 255.0)),
  };
}

std::vector<PreferenceSwatch> build_preferences_palette(Display* display) {
  std::vector<PreferenceSwatch> palette;
  palette.reserve(144);
  for (unsigned int row = 0; row < 12; ++row) {
    for (unsigned int col = 0; col < 12; ++col) {
      const double hue = static_cast<double>(col) * 30.0 + static_cast<double>(row % 3) * 3.0;
      const double saturation = std::clamp(0.42 + static_cast<double>(row) * 0.045, 0.42, 0.95);
      const double value = std::clamp(0.28 + static_cast<double>(11 - row) * 0.055 + static_cast<double>(col % 4) * 0.015, 0.20, 0.96);
      const auto rgb = hsv_to_rgb(std::fmod(hue, 360.0), saturation, value);
      const std::string token = color_token_from_rgb(rgb[0], rgb[1], rgb[2]);
      PreferenceSwatch color{};
      color.label = token;
      color.token = token;
      color.pixel = resolve_color_pixel(display, token, rgb_fallback_from_token(token, 0xC0C0C0u));
      palette.push_back(std::move(color));
    }
  }
  return palette;
}

}

int WindowManager::clamp_client_y(int y) const {
  return std::max(y, static_cast<int>(theme_.menu_height));
}

void WindowManager::ensure_desktop_below_clients() {
  if (desktop_) {
    XLowerWindow(display_, desktop_->window());
  }
}

void WindowManager::restack_managed_windows() {
  ensure_desktop_below_clients();
  std::vector<Window> windows;
  windows.reserve(clients_.size() * 2);
  for (auto order_it = stacking_order_.rbegin(); order_it != stacking_order_.rend(); ++order_it) {
    const Window window = *order_it;
    const auto client_it = clients_.find(window);
    if (client_it == clients_.end()) {
      continue;
    }
    const Client& client = client_it->second;
    if (client.frame) {
      windows.push_back(client.frame);
    }
    if (client.shadow) {
      windows.push_back(client.shadow);
    }
  }
  if (!windows.empty()) {
    XRestackWindows(display_, windows.data(), static_cast<int>(windows.size()));
  }
}

std::vector<Window> WindowManager::collect_transient_group(Window window) const {
  std::vector<Window> ordered;
  if (!clients_.contains(window)) {
    return ordered;
  }

  std::unordered_set<Window> seen;
  std::vector<Window> pending{window};
  seen.insert(window);

  while (!pending.empty()) {
    const Window current = pending.back();
    pending.pop_back();
    for (const auto& [candidate_window, client] : clients_) {
      if (client.transient_for == current && !seen.contains(candidate_window)) {
        seen.insert(candidate_window);
        pending.push_back(candidate_window);
      }
    }
  }

  for (const Window stacked : stacking_order_) {
    if (seen.contains(stacked)) {
      ordered.push_back(stacked);
    }
  }
  return ordered;
}

void WindowManager::raise_managed_window(Window window) {
  Client* client = find_client(window);
  if (!client) {
    return;
  }
  const std::vector<Window> group = collect_transient_group(client->window);
  if (group.empty()) {
    return;
  }
  for (const Window group_window : group) {
    const auto order_it = std::find(stacking_order_.begin(), stacking_order_.end(), group_window);
    if (order_it != stacking_order_.end()) {
      stacking_order_.erase(order_it);
    }
  }
  stacking_order_.insert(stacking_order_.end(), group.begin(), group.end());
  restack_managed_windows();
  ensure_desktop_below_clients();
  if (!client->iconic) {
    set_client_active(*client, true);
  }
}

void WindowManager::lower_managed_window(Window window) {
  Client* client = find_client(window);
  if (!client) {
    return;
  }
  const std::vector<Window> group = collect_transient_group(client->window);
  if (group.empty()) {
    return;
  }
  for (const Window group_window : group) {
    const auto order_it = std::find(stacking_order_.begin(), stacking_order_.end(), group_window);
    if (order_it != stacking_order_.end()) {
      stacking_order_.erase(order_it);
    }
  }
  stacking_order_.insert(stacking_order_.begin(), group.rbegin(), group.rend());
  restack_managed_windows();
  ensure_desktop_below_clients();
  if (client->focused) {
    set_client_active(*client, true);
  }
}

WindowManager::WindowManager(x11::Connection& connection, config::Config config, std::filesystem::path config_path)
  : connection_(connection),
    config_(std::move(config)),
    config_path_(std::move(config_path)),
    display_(connection.display()),
    root_(connection.root()),
    screen_(connection.screen()) {
  XSetErrorHandler(x_error_handler);
  arrow_cursor_ = XCreateFontCursor(display_, XC_left_ptr);
  init_atoms();
  init_theme_from_config();
  claim_wm_selection();
  last_x_error = 0;
  XSelectInput(display_, root_, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | PointerMotionMask | KeyPressMask | EnterWindowMask | PropertyChangeMask);
  XSync(display_, False);
  if (last_x_error == BadAccess) {
    throw std::runtime_error("another window manager owns the root redirect mask");
  }
  init_desktop();
  init_keybindings();
  scan_existing_windows();
}

WindowManager::~WindowManager() {
  save_config();
  if (arrow_cursor_ != None) {
    XFreeCursor(display_, arrow_cursor_);
  }
  desktop_.reset();
  menubar_.reset();
  popup_menu_.reset();
  if (wm_check_window_) {
    XDestroyWindow(display_, wm_check_window_);
  }
}

void WindowManager::init_atoms() {
  wm_protocols_ = connection_.atom("WM_PROTOCOLS");
  wm_delete_window_ = connection_.atom("WM_DELETE_WINDOW");
  wm_state_ = connection_.atom("WM_STATE");
  wm_take_focus_ = connection_.atom("WM_TAKE_FOCUS");
  net_supported_ = connection_.atom("_NET_SUPPORTED");
  net_client_list_ = connection_.atom("_NET_CLIENT_LIST");
  net_active_window_ = connection_.atom("_NET_ACTIVE_WINDOW");
  net_wm_name_ = connection_.atom("_NET_WM_NAME");
  net_wm_state_ = connection_.atom("_NET_WM_STATE");
  net_wm_window_type_ = connection_.atom("_NET_WM_WINDOW_TYPE");
  net_wm_desktop_ = connection_.atom("_NET_WM_DESKTOP");
  net_current_desktop_ = connection_.atom("_NET_CURRENT_DESKTOP");
  net_number_of_desktops_ = connection_.atom("_NET_NUMBER_OF_DESKTOPS");
  net_supporting_wm_check_ = connection_.atom("_NET_SUPPORTING_WM_CHECK");
  utf8_string_ = connection_.atom("UTF8_STRING");
}

void WindowManager::init_theme_from_config() {
  theme_.border_width = std::max(1u, config_.border_width);
  theme_.title_height = std::max(1u, config_.title_height);
  theme_.menu_height = std::max(1u, config_.menu_height);
  theme_.grow_box_size = std::max(10u, config_.grow_box_size);
  theme_.desktop_top = theme_.menu_height;
  theme_.background_pixel = resolve_color_pixel(display_, config_.background_color, config_.background_pixel);
  theme_.frame_active_pixel = config_.frame_active_pixel;
  theme_.frame_inactive_pixel = config_.frame_inactive_pixel;
  theme_.title_active_pixel = config_.title_active_pixel;
  theme_.title_inactive_pixel = config_.title_inactive_pixel;
  theme_.title_text_pixel = config_.title_text_pixel;
  theme_.menu_pixel = config_.menu_pixel;
  theme_.menu_text_pixel = config_.menu_text_pixel;
  update_dynamic_icon_colors();
}

void WindowManager::update_dynamic_icon_colors() {
  const ContrastPalette palette = derive_icon_palette(config_.background_color, theme_.background_pixel);
  icon_text_pixel_ = palette.text_pixel;
  icon_shadow_pixel_ = palette.shadow_pixel;
  if (desktop_) {
    desktop_->set_icon_colors(icon_text_pixel_, icon_shadow_pixel_);
  }
  for (auto& [window, client] : clients_) {
    (void)window;
    if (client.iconic) {
      draw_frame(client);
    }
  }
}

void WindowManager::claim_wm_selection() {
  const std::string selection_name = "WM_S" + std::to_string(screen_);
  const Atom selection = connection_.atom(selection_name.c_str());
  const Window owner = XGetSelectionOwner(display_, selection);
  if (owner != None) {
    throw std::runtime_error("another window manager is already running");
  }

  wm_check_window_ = XCreateSimpleWindow(display_, root_, 0, 0, 1, 1, 0, 0, 0);
  XSetSelectionOwner(display_, selection, wm_check_window_, CurrentTime);
  if (XGetSelectionOwner(display_, selection) != wm_check_window_) {
    throw std::runtime_error("failed to claim WM selection");
  }

  const std::array<Atom, 6> supported{
      net_client_list_,
      net_active_window_,
      net_wm_name_,
      net_wm_state_,
      net_wm_window_type_,
      net_wm_desktop_,
  };
  XChangeProperty(display_, root_, net_supported_, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(supported.data()),
                  static_cast<int>(supported.size()));

  XChangeProperty(display_, wm_check_window_, net_supporting_wm_check_, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&wm_check_window_), 1);
  XChangeProperty(display_, root_, net_supporting_wm_check_, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&wm_check_window_), 1);
  update_desktop_properties();
}

void WindowManager::init_desktop() {
  const unsigned int width = static_cast<unsigned int>(DisplayWidth(display_, screen_));
  const unsigned int height = static_cast<unsigned int>(DisplayHeight(display_, screen_));
  XSetWindowBackground(display_, root_, theme_.background_pixel);
  XClearWindow(display_, root_);
  menubar_ = std::make_unique<ui::MenuBar>(display_, root_, width, theme_.menu_height);
  desktop_ = std::make_unique<ui::Desktop>(display_, root_, width, height - theme_.menu_height, theme_.menu_height);
  desktop_->set_background_pixel(theme_.background_pixel);
  popup_menu_ = std::make_unique<ui::PopupMenu>(display_, root_);
  update_dynamic_icon_colors();
  define_cursor(root_);
  define_cursor(menubar_->window());
  define_cursor(desktop_->window());
  define_cursor(popup_menu_->window());
  sync_desktop_icons();
  switch_to_desktop(0);
  ensure_desktop_below_clients();
}

void WindowManager::init_keybindings() {
  refresh_keybindings();
}

void WindowManager::refresh_keybindings() {
  XUngrabKey(display_, AnyKey, AnyModifier, root_);
  const KeyCode left = XKeysymToKeycode(display_, XK_Left);
  const KeyCode right = XKeysymToKeycode(display_, XK_Right);
  const std::array<unsigned int, 4> modifiers{
      Mod1Mask,
      Mod1Mask | LockMask,
      Mod1Mask | Mod2Mask,
      Mod1Mask | Mod2Mask | LockMask,
  };
  for (const unsigned int modifier : modifiers) {
    XGrabKey(display_, left, modifier, root_, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display_, right, modifier, root_, True, GrabModeAsync, GrabModeAsync);
  }
  XSync(display_, False);
}

void WindowManager::open_execute_window() {
  if (execute_window_ != 0) {
  if (Client* client = find_client(execute_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      XGrabKeyboard(display_, client->window, False, GrabModeAsync, GrabModeAsync, CurrentTime);
      return;
    }
    execute_window_ = 0;
    return;
  }

  const unsigned int width = 420;
  const unsigned int height = 128;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2);
  execute_buffer_.clear();
  execute_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, execute_window_, "Execute");
  manage_window(execute_window_);
  if (Client* client = find_client(execute_window_)) {
    client->internal_kind = InternalKind::Execute;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    XGrabKeyboard(display_, client->window, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_internal_content(*client);
  }
}

void WindowManager::refresh_file_manager() {
  if (file_manager_path_.empty()) {
    file_manager_path_ = home_directory();
  }
  std::error_code ec;
  if (!std::filesystem::exists(file_manager_path_, ec)) {
    file_manager_path_ = home_directory();
  }
  file_manager_entries_.clear();
  std::vector<std::filesystem::directory_entry> directories;
  std::vector<std::filesystem::directory_entry> files;
  for (std::filesystem::directory_iterator it(file_manager_path_, ec), end; !ec && it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    if (it->is_directory(ec)) {
      directories.push_back(*it);
    } else {
      files.push_back(*it);
    }
  }
  auto by_name = [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b) {
    return a.path().filename().string() < b.path().filename().string();
  };
  std::sort(directories.begin(), directories.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  file_manager_entries_.reserve(directories.size() + files.size());
  file_manager_entries_.insert(file_manager_entries_.end(), directories.begin(), directories.end());
  file_manager_entries_.insert(file_manager_entries_.end(), files.begin(), files.end());
  if (file_manager_selected_ > file_manager_entries_.size()) {
    file_manager_selected_ = 0;
  }
  if (file_manager_scroll_ > file_manager_entries_.size()) {
    file_manager_scroll_ = 0;
  }
}

void WindowManager::ensure_file_manager_selection_visible(const Client& client) {
  const int list_y = 68;
  const int row_h = 20;
  const int available_rows = client.height > static_cast<unsigned int>(list_y) ? static_cast<int>((client.height - list_y - 12) / row_h) : 0;
  const std::size_t visible_entries = available_rows > 1 ? static_cast<std::size_t>(available_rows - 1) : 0;
  if (visible_entries == 0 || file_manager_entries_.empty() || file_manager_selected_ == 0) {
    file_manager_scroll_ = 0;
    return;
  }
  const std::size_t entry_index = file_manager_selected_ - 1;
  if (entry_index < file_manager_scroll_) {
    file_manager_scroll_ = entry_index;
  } else if (entry_index >= file_manager_scroll_ + visible_entries) {
    file_manager_scroll_ = entry_index - visible_entries + 1;
  }
  const std::size_t max_scroll = file_manager_entries_.size() > visible_entries ? file_manager_entries_.size() - visible_entries : 0;
  if (file_manager_scroll_ > max_scroll) {
    file_manager_scroll_ = max_scroll;
  }
}

void WindowManager::open_file_manager_window() {
  if (file_manager_window_ != 0) {
    if (Client* client = find_client(file_manager_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      return;
    }
    file_manager_window_ = 0;
    return;
  }

  const unsigned int width = 560;
  const unsigned int height = 340;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2 + 18);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2 + 18);
  file_manager_path_ = home_directory();
  file_manager_selected_ = 0;
  refresh_file_manager();
  file_manager_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, file_manager_window_, "Files");
  manage_window(file_manager_window_);
  if (Client* client = find_client(file_manager_window_)) {
    client->internal_kind = InternalKind::Browser;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    draw_internal_content(*client);
  }
}

void WindowManager::open_applications_window() {
  if (applications_window_ != 0) {
    if (Client* client = find_client(applications_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      return;
    }
    applications_window_ = 0;
  }

  const unsigned int width = static_cast<unsigned int>(std::max(520, DisplayWidth(display_, screen_) - 80));
  const unsigned int height = 620;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2 + 8);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2 + 8);
  applications_details_collapsed_ = true;
  applications_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, applications_window_, "Applications");
  manage_window(applications_window_);
  if (Client* client = find_client(applications_window_)) {
    client->internal_kind = InternalKind::Applications;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    draw_internal_content(*client);
  }
}

void WindowManager::open_preferences_window() {
  if (preferences_window_ != 0) {
    if (Client* client = find_client(preferences_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      return;
    }
    preferences_window_ = 0;
  }

  if (preferences_palette_.empty()) {
    preferences_palette_ = build_preferences_palette(display_);
  }

  const unsigned int width = 792;
  const unsigned int height = 620;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2 + 12);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2 + 12);
  preferences_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, preferences_window_, "Preferences");
  manage_window(preferences_window_);
  if (Client* client = find_client(preferences_window_)) {
    client->internal_kind = InternalKind::Preferences;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    draw_internal_content(*client);
  }
}

void WindowManager::open_about_window() {
  if (about_window_ != 0) {
    if (Client* client = find_client(about_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      return;
    }
    about_window_ = 0;
  }

  const unsigned int width = 520;
  const unsigned int height = 308;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2);
  about_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, about_window_, "About chiux");
  manage_window(about_window_);
  if (Client* client = find_client(about_window_)) {
    client->internal_kind = InternalKind::About;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    draw_internal_content(*client);
  }
}

void WindowManager::open_home_window() {
  if (home_window_ != 0) {
    if (Client* client = find_client(home_window_)) {
      if (client->iconic) {
        deiconify_client(*client);
      }
      focus_client(client);
      draw_internal_content(*client);
      return;
    }
    home_window_ = 0;
  }

  const unsigned int width = 720;
  const unsigned int height = 440;
  const int x = std::max(0, (DisplayWidth(display_, screen_) - static_cast<int>(width)) / 2);
  const int y = std::max(24, (DisplayHeight(display_, screen_) - static_cast<int>(height)) / 2);
  home_window_ = XCreateSimpleWindow(display_, root_, x, y, width, height, 0, theme_.frame_active_pixel, theme_.menu_pixel);
  XStoreName(display_, home_window_, "Home");
  manage_window(home_window_);
  if (Client* client = find_client(home_window_)) {
    client->internal_kind = InternalKind::Home;
    XMapWindow(display_, client->shadow);
    XMapWindow(display_, client->frame);
    XMapWindow(display_, client->window);
    std::array<Window, 2> restack{client->frame, client->shadow};
    XRestackWindows(display_, restack.data(), 2);
    focus_client(client);
    draw_internal_content(*client);
  }
}

void WindowManager::run_launcher_command(const std::string& command) {
  if (command.rfind("chiux:", 0) == 0) {
    const std::string action = command.substr(6);
    if (action == "open-file-manager") {
      open_file_manager_window();
    } else if (action == "open-applications") {
      open_applications_window();
    } else if (action == "open-execute") {
      open_execute_window();
    } else if (action == "open-preferences") {
      open_preferences_window();
    } else if (action == "open-home-info") {
      open_home_window();
    } else {
      handle_menu_action(action);
    }
    return;
  }
  util::spawn_command(command);
}

void WindowManager::apply_background_color(const std::string& color_token) {
  if (color_token.empty()) {
    return;
  }
  const std::string canonical = color_token.front() == '#'
      ? [&color_token]() {
          std::string value = color_token;
          for (char& ch : value) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
          }
          return value;
        }()
      : color_token;
  config_.background_color = canonical;
  config_.background_pixel = rgb_fallback_from_token(canonical, config_.background_pixel);
  theme_.background_pixel = resolve_color_pixel(display_, canonical, config_.background_pixel);
  XSetWindowBackground(display_, root_, theme_.background_pixel);
  XClearWindow(display_, root_);
  if (desktop_) {
    desktop_->set_background_pixel(theme_.background_pixel);
  }
  update_dynamic_icon_colors();
  try {
    config_.save(config_path_);
  } catch (const std::exception& e) {
    chiux::log::warn(std::string("config save failed: ") + e.what());
  }
}

void WindowManager::draw_internal_content(Client& client) {
  ensure_internal_backing(client);
  if (client.internal_kind == InternalKind::Execute) {
    draw_execute_window(client);
  } else if (client.internal_kind == InternalKind::Browser) {
    draw_file_manager_window(client);
  } else if (client.internal_kind == InternalKind::Applications) {
    draw_applications_window(client);
  } else if (client.internal_kind == InternalKind::Preferences) {
    draw_preferences_window(client);
  } else if (client.internal_kind == InternalKind::About) {
    draw_about_window(client);
  } else if (client.internal_kind == InternalKind::Home) {
    draw_home_window(client);
  }
  if (client.content_backing) {
    GC gc = XCreateGC(display_, client.window, 0, nullptr);
    XCopyArea(display_, client.content_backing, client.window, gc, 0, 0,
              client.content_width, client.content_height, 0, 0);
    XFreeGC(display_, gc);
  }
}

void WindowManager::ensure_internal_backing(Client& client) {
  if (client.internal_kind == InternalKind::Unset) {
    return;
  }

  const unsigned int width = std::max(1u, client.width);
  const unsigned int height = std::max(1u, client.height);
  if (client.content_backing && client.content_width == width && client.content_height == height) {
    return;
  }

  if (client.content_backing) {
    XFreePixmap(display_, client.content_backing);
    client.content_backing = 0;
  }

  client.content_backing = XCreatePixmap(display_, client.window, width, height,
                                         static_cast<unsigned int>(DefaultDepth(display_, screen_)));
  client.content_width = width;
  client.content_height = height;
}

void WindowManager::release_internal_backing(Client& client) {
  if (client.content_backing) {
    XFreePixmap(display_, client.content_backing);
    client.content_backing = 0;
  }
  client.content_width = 0;
  client.content_height = 0;
}

void WindowManager::present_internal_content(Client& client) {
  if (client.internal_kind == InternalKind::Unset) {
    return;
  }
  if (!client.content_backing || client.content_width != client.width || client.content_height != client.height) {
    draw_internal_content(client);
    return;
  }
  GC gc = XCreateGC(display_, client.window, 0, nullptr);
  XCopyArea(display_, client.content_backing, client.window, gc, 0, 0,
            client.content_width, client.content_height, 0, 0);
  XFreeGC(display_, gc);
}

void WindowManager::draw_execute_window(Client& client) {
  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  XSetForeground(display_, gc, theme_.frame_inactive_pixel);
  XDrawString(display_, target, gc, 12, 18, "Execute command", 15);
  XDrawString(display_, target, gc, 12, 32, "Enter to run, Esc to clear", 26);

  const int box_x = 12;
  const int box_y = 48;
  const unsigned int box_w = draw_width > 24 ? draw_width - 24 : draw_width;
  const unsigned int box_h = 28;
  XSetForeground(display_, gc, theme_.title_text_pixel);
  XFillRectangle(display_, target, gc, box_x, box_y, box_w, box_h);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawLine(display_, target, gc, box_x, box_y, box_x + static_cast<int>(box_w), box_y);
  XDrawLine(display_, target, gc, box_x, box_y, box_x, box_y + static_cast<int>(box_h));
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawLine(display_, target, gc, box_x, box_y + static_cast<int>(box_h), box_x + static_cast<int>(box_w), box_y + static_cast<int>(box_h));
  XDrawLine(display_, target, gc, box_x + static_cast<int>(box_w), box_y, box_x + static_cast<int>(box_w), box_y + static_cast<int>(box_h));
  XDrawString(display_, target, gc, box_x + 8, box_y + 18, execute_buffer_.c_str(), static_cast<int>(execute_buffer_.size()));

  const int cursor_x = box_x + 8 + static_cast<int>(execute_buffer_.size()) * 6;
  XDrawLine(display_, target, gc, cursor_x, box_y + 8, cursor_x, box_y + 20);
  XFreeGC(display_, gc);
}

void WindowManager::draw_file_manager_window(Client& client) {
  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  const std::string path_text = file_manager_path_.empty() ? std::string("/") : file_manager_path_.string();
  XSetForeground(display_, gc, theme_.frame_inactive_pixel);
  XDrawString(display_, target, gc, 12, 18, "Files", 5);
  XDrawString(display_, target, gc, 12, 32, path_text.c_str(), static_cast<int>(path_text.size()));

  const int up_x = 12;
  const int up_y = 42;
  const unsigned int up_w = 40;
  const unsigned int up_h = 18;
  XSetForeground(display_, gc, theme_.title_text_pixel);
  XFillRectangle(display_, target, gc, up_x, up_y, up_w, up_h);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, up_x, up_y, up_w, up_h);
  XDrawString(display_, target, gc, up_x + 10, up_y + 13, "Up", 2);

  const int list_x = 12;
  const int list_y = 68;
  const int row_h = 20;
  const int max_rows = static_cast<int>((draw_height > static_cast<unsigned int>(list_y)) ? (draw_height - list_y - 12) / row_h : 0);
  const std::size_t available_rows = max_rows > 0 ? static_cast<std::size_t>(max_rows) : 0;
  const std::size_t visible_entries = available_rows > 0 ? available_rows - 1 : 0;
  const std::size_t remaining_entries = file_manager_entries_.size() > file_manager_scroll_ ? file_manager_entries_.size() - file_manager_scroll_ : 0;
  const std::size_t entry_rows = std::min<std::size_t>(visible_entries, remaining_entries);
  const std::size_t visible_count = std::min<std::size_t>(available_rows, entry_rows + 1);
  for (std::size_t i = 0; i < visible_count; ++i) {
    const int top = list_y + static_cast<int>(i * row_h);
    const bool selected = (i == 0) ? file_manager_selected_ == 0 : file_manager_selected_ == file_manager_scroll_ + i;
    if (selected) {
      XSetForeground(display_, gc, theme_.frame_active_pixel);
      XFillRectangle(display_, target, gc, list_x, top, draw_width > 24 ? draw_width - 24 : draw_width, row_h - 2);
      XSetForeground(display_, gc, theme_.title_text_pixel);
    } else {
      XSetForeground(display_, gc, theme_.menu_text_pixel);
    }
    std::string label = (i == 0) ? std::string("[..]") : file_manager_entries_[file_manager_scroll_ + i - 1].path().filename().string();
    if (i != 0 && file_manager_scroll_ + i - 1 < file_manager_entries_.size() && file_manager_entries_[file_manager_scroll_ + i - 1].is_directory()) {
      label += "/";
    }
    XDrawString(display_, target, gc, list_x + 8, top + 14, label.c_str(), static_cast<int>(label.size()));
  }
  XFreeGC(display_, gc);
}

void WindowManager::draw_applications_window(Client& client) {
  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  const ApplicationsLayout layout = applications_layout_for(draw_width, draw_height, applications_details_collapsed_);
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, 1, static_cast<int>(draw_height) - 2, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XDrawLine(display_, target, gc, static_cast<int>(draw_width) - 2, 1, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, 0, 0, draw_width - 1, draw_height - 1);

  const std::string title = "Applications";
  XFontStruct* title_font = load_about_title_font(display_);
  XFontStruct* body_font = load_about_body_font(display_);
  if (title_font) {
    XSetFont(display_, gc, title_font->fid);
  }
  const int title_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(title_font, title) / 2);
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawString(display_, target, gc, title_x, layout.title_y, title.c_str(), static_cast<int>(title.size()));
  if (body_font) {
    XSetFont(display_, gc, body_font->fid);
  }

  const auto draw_panel = [&](int x, int y, unsigned int w, unsigned int h) {
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, x, y, w, h);
    XSetForeground(display_, gc, theme_.menu_pixel);
    XFillRectangle(display_, target, gc, x + 1, y + 1, w > 2 ? w - 2 : 1u, h > 2 ? h - 2 : 1u);
  };

  draw_panel(layout.form_x, layout.form_y, layout.form_w, layout.form_h);
  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, layout.form_x + 1, layout.header_y + static_cast<int>(layout.header_h), layout.form_x + static_cast<int>(layout.form_w) - 2, layout.header_y + static_cast<int>(layout.header_h));
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawString(display_, target, gc, layout.form_x + 14, layout.header_y + 16, "Application Details", 19);
  XSetForeground(display_, gc, theme_.title_text_pixel);
  XFillRectangle(display_, target, gc, layout.toggle_x, layout.toggle_y, layout.toggle_w, layout.toggle_h);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, layout.toggle_x, layout.toggle_y, layout.toggle_w, layout.toggle_h);
  const char* toggle_text = applications_details_collapsed_ ? "+" : "-";
  const int toggle_text_width = body_font ? text_width(body_font, toggle_text) : 6;
  const int toggle_text_x = layout.toggle_x + std::max(4, (static_cast<int>(layout.toggle_w) - toggle_text_width) / 2);
  const int toggle_text_y = layout.toggle_y + static_cast<int>(layout.toggle_h / 2) + (body_font ? (body_font->ascent - body_font->descent) / 2 : 4);
  XDrawString(display_, target, gc, toggle_text_x, toggle_text_y, toggle_text, 1);

  auto draw_field = [&](int x, int y, unsigned int w, const std::string& label, const std::string& value, bool active) {
    XSetForeground(display_, gc, theme_.frame_inactive_pixel);
    XDrawString(display_, target, gc, x, y - 4, label.c_str(), static_cast<int>(label.size()));
    XSetForeground(display_, gc, theme_.title_text_pixel);
    XFillRectangle(display_, target, gc, x, y, w, layout.field_h);
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, x, y, w, layout.field_h);
    if (active) {
      XSetForeground(display_, gc, theme_.shadow_pixel);
      XDrawRectangle(display_, target, gc, x + 1, y + 1, w > 2 ? w - 2 : 1u, layout.field_h > 2 ? layout.field_h - 2 : 1u);
    }
    const std::string shown = shorten_label(value, w > 18 ? (w - 12) / 6 : 1);
    XSetForeground(display_, gc, theme_.menu_text_pixel);
    XDrawString(display_, target, gc, x + 6, y + 16, shown.c_str(), static_cast<int>(shown.size()));
    if (active) {
      const int shown_width = body_font ? text_width(body_font, shown) : static_cast<int>(shown.size()) * 6;
      const int cursor_x = x + 6 + shown_width;
      XDrawLine(display_, target, gc, cursor_x, y + 6, cursor_x, y + 18);
    }
  };

  if (!applications_details_collapsed_) {
    draw_field(layout.name_x, layout.name_y, layout.name_w, "Name", application_name_buffer_, applications_active_field_ == ApplicationsField::Name);
    draw_field(layout.command_x, layout.command_y, layout.command_w, "Path", application_command_buffer_, applications_active_field_ == ApplicationsField::Command);
  }

  auto draw_button = [&](int x, const char* text, bool enabled) {
    XSetForeground(display_, gc, enabled ? theme_.menu_pixel : theme_.title_text_pixel);
    XFillRectangle(display_, target, gc, x, layout.button_y, layout.button_w, layout.button_h);
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, x, layout.button_y, layout.button_w, layout.button_h);
    const int label_width = body_font ? text_width(body_font, text) : static_cast<int>(std::strlen(text)) * 6;
    const int text_x = x + std::max(4, (static_cast<int>(layout.button_w) - label_width) / 2);
    const int text_y = layout.button_y + static_cast<int>(layout.button_h / 2) + (body_font ? (body_font->ascent - body_font->descent) / 2 : 4);
    XSetForeground(display_, gc, enabled ? theme_.menu_text_pixel : theme_.frame_inactive_pixel);
    XDrawString(display_, target, gc, text_x, text_y, text, static_cast<int>(std::strlen(text)));
  };

  const bool has_selection = applications_selected_ && *applications_selected_ < config_.applications.size();
  if (!applications_details_collapsed_) {
    draw_button(layout.open_x, "Open", has_selection);
    draw_button(layout.add_x, "Add", !application_name_buffer_.empty() && !application_command_buffer_.empty());
    draw_button(layout.update_x, "Update", has_selection);
    draw_button(layout.remove_x, "Remove", has_selection);
    draw_button(layout.clear_x, "Clear", has_selection || !application_name_buffer_.empty() || !application_command_buffer_.empty());

    XSetForeground(display_, gc, theme_.frame_inactive_pixel);
    XDrawString(display_, target, gc, layout.style_x, layout.style_label_y, "Icon Style", 10);
    for (unsigned int style = 0; style < kApplicationStyleCount; ++style) {
      const int chip_x = layout.style_x + static_cast<int>(style) * static_cast<int>(layout.style_chip_w + 6);
      const bool selected_style = application_style_index_ == style;
      XSetForeground(display_, gc, selected_style ? theme_.menu_pixel : theme_.title_text_pixel);
      XFillRectangle(display_, target, gc, chip_x, layout.style_row_y, layout.style_chip_w, layout.style_chip_h);
      XSetForeground(display_, gc, theme_.frame_active_pixel);
      XDrawRectangle(display_, target, gc, chip_x, layout.style_row_y, layout.style_chip_w, layout.style_chip_h);
      XSetForeground(display_, gc, selected_style ? theme_.menu_text_pixel : theme_.frame_inactive_pixel);
      const char* style_name = application_style_name(style);
      const int label_width = body_font ? text_width(body_font, style_name) : static_cast<int>(std::strlen(style_name)) * 6;
      const int text_x = chip_x + std::max(4, (static_cast<int>(layout.style_chip_w) - label_width) / 2);
      const int text_y = layout.style_row_y + static_cast<int>(layout.style_chip_h / 2) + (body_font ? (body_font->ascent - body_font->descent) / 2 : 4);
      XDrawString(display_, target, gc, text_x, text_y, style_name, static_cast<int>(std::strlen(style_name)));
    }
  }

  draw_panel(layout.panel_x, layout.panel_y, layout.panel_w, layout.panel_h);
  XSetForeground(display_, gc, theme_.frame_inactive_pixel);
  XDrawString(display_, target, gc, layout.panel_x + 12, layout.panel_y - 6, "Finder Grid", 11);

  if (config_.applications.empty()) {
    XSetForeground(display_, gc, theme_.frame_inactive_pixel);
    const std::string empty = "No applications yet. Add one above, then drag icons freely in the finder grid.";
    XDrawString(display_, target, gc, layout.panel_x + 18, layout.panel_y + 24, empty.c_str(), static_cast<int>(empty.size()));
  }

  for (std::size_t index = 0; index < config_.applications.size(); ++index) {
    const auto& entry = config_.applications[index];
    const int icon_x = layout.panel_x + entry.x;
    const int icon_y = layout.panel_y + entry.y;
    if (icon_x + kApplicationsIconWidth < layout.panel_x || icon_y + kApplicationsIconHeight < layout.panel_y ||
        icon_x > layout.panel_x + static_cast<int>(layout.panel_w) || icon_y > layout.panel_y + static_cast<int>(layout.panel_h)) {
      continue;
    }
    const bool selected = applications_selected_ && *applications_selected_ == index;
    if (selected) {
      XSetForeground(display_, gc, theme_.frame_active_pixel);
      XFillRectangle(display_, target, gc, icon_x - 3, icon_y - 3, kApplicationsIconWidth + 6, kApplicationsIconHeight + 16);
      XSetForeground(display_, gc, theme_.menu_pixel);
      XFillRectangle(display_, target, gc, icon_x - 2, icon_y - 2, kApplicationsIconWidth + 4, kApplicationsIconHeight + 14);
    }
    const unsigned int style = entry.style % kApplicationStyleCount;
    XSetForeground(display_, gc, theme_.shadow_pixel);
    XFillRectangle(display_, target, gc, icon_x + 7, icon_y + 7, 42, 28);
    XSetForeground(display_, gc, theme_.menu_pixel);
    XFillRectangle(display_, target, gc, icon_x + 4, icon_y + 4, 42, 28);
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, icon_x + 4, icon_y + 4, 42, 28);
    if (style == 0) {
      XFillRectangle(display_, target, gc, icon_x + 10, icon_y + 10, 28, 3);
      XFillRectangle(display_, target, gc, icon_x + 10, icon_y + 17, 24, 3);
      XFillRectangle(display_, target, gc, icon_x + 10, icon_y + 24, 20, 3);
    } else if (style == 1) {
      XDrawRectangle(display_, target, gc, icon_x + 11, icon_y + 10, 20, 14);
      XFillRectangle(display_, target, gc, icon_x + 33, icon_y + 12, 7, 10);
      XDrawLine(display_, target, gc, icon_x + 31, icon_y + 12, icon_x + 33, icon_y + 12);
      XDrawLine(display_, target, gc, icon_x + 31, icon_y + 22, icon_x + 33, icon_y + 22);
    } else if (style == 2) {
      XFillRectangle(display_, target, gc, icon_x + 9, icon_y + 12, 28, 14);
      XFillRectangle(display_, target, gc, icon_x + 12, icon_y + 9, 12, 4);
      XDrawRectangle(display_, target, gc, icon_x + 9, icon_y + 12, 28, 14);
    } else {
      XDrawRectangle(display_, target, gc, icon_x + 10, icon_y + 10, 26, 16);
      XFillRectangle(display_, target, gc, icon_x + 18, icon_y + 28, 10, 2);
      XDrawLine(display_, target, gc, icon_x + 23, icon_y + 26, icon_x + 23, icon_y + 28);
    }
    const std::string label = shorten_label(entry.label, 13);
    const std::string path_line = shorten_label(entry.command, 14);
    XSetForeground(display_, gc, theme_.menu_text_pixel);
    XDrawString(display_, target, gc, icon_x, icon_y + 47, label.c_str(), static_cast<int>(label.size()));
    XSetForeground(display_, gc, theme_.frame_inactive_pixel);
    XDrawString(display_, target, gc, icon_x, icon_y + 61, path_line.c_str(), static_cast<int>(path_line.size()));
  }

  const std::string footer = has_selection
      ? "Selected: " + config_.applications[*applications_selected_].label + "   Open, update, remove, or drag it."
      : "Select an icon to edit it. Drag to reposition. Open launches the selected application.";
  XSetForeground(display_, gc, theme_.frame_inactive_pixel);
  XDrawString(display_, target, gc, layout.inset, layout.footer_y, footer.c_str(), static_cast<int>(footer.size()));

  XFreeGC(display_, gc);
}

std::optional<std::size_t> WindowManager::hit_test_application_icon(const Client& client, int x, int y) const {
  const ApplicationsLayout layout = applications_layout_for(client.width, client.height, applications_details_collapsed_);
  for (std::size_t index = 0; index < config_.applications.size(); ++index) {
    const auto& entry = config_.applications[index];
    const int icon_x = layout.panel_x + entry.x;
    const int icon_y = layout.panel_y + entry.y;
    if (x >= icon_x && x <= icon_x + kApplicationsIconWidth &&
        y >= icon_y && y <= icon_y + kApplicationsIconHeight) {
      return index;
    }
  }
  return std::nullopt;
}

void WindowManager::persist_applications() {
  save_config();
}

void WindowManager::add_application_entry() {
  auto trim = [](std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(), value.end());
    return value;
  };
  const std::string name = trim(application_name_buffer_);
  const std::string command = trim(application_command_buffer_);
  if (name.empty() || command.empty()) {
    return;
  }
  if (std::any_of(config_.applications.begin(), config_.applications.end(), [&](const config::ApplicationEntry& entry) {
        return entry.label == name && entry.command == command;
      })) {
    applications_selected_ = static_cast<std::size_t>(std::distance(config_.applications.begin(),
        std::find_if(config_.applications.begin(), config_.applications.end(), [&](const config::ApplicationEntry& entry) {
          return entry.label == name && entry.command == command;
        })));
    load_selected_application_into_form();
    persist_applications();
    return;
  }

  int place_x = 18;
  int place_y = 18;
  if (Client* client = applications_window_ != 0 ? find_client(applications_window_) : nullptr) {
    const ApplicationsLayout layout = applications_layout_for(client->width, client->height, applications_details_collapsed_);
    const int stride_x = 96;
    const int stride_y = 84;
    const int columns = std::max(1, static_cast<int>(layout.panel_w) / stride_x);
    const int slot = static_cast<int>(config_.applications.size());
    place_x = 18 + (slot % columns) * stride_x;
    place_y = 18 + (slot / columns) * stride_y;
  }

  config_.applications.push_back({name, command, place_x, place_y});
  config_.applications.back().style = application_style_index_ % kApplicationStyleCount;
  applications_selected_ = config_.applications.size() - 1;
  load_selected_application_into_form();
  persist_applications();
}

void WindowManager::update_selected_application() {
  if (!applications_selected_ || *applications_selected_ >= config_.applications.size()) {
    return;
  }
  auto trim = [](std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(), value.end());
    return value;
  };
  const std::string name = trim(application_name_buffer_);
  const std::string command = trim(application_command_buffer_);
  if (name.empty() || command.empty()) {
    return;
  }
  auto& entry = config_.applications[*applications_selected_];
  entry.label = name;
  entry.command = command;
  entry.style = application_style_index_ % kApplicationStyleCount;
  persist_applications();
}

void WindowManager::remove_selected_application() {
  if (!applications_selected_ || *applications_selected_ >= config_.applications.size()) {
    return;
  }
  config_.applications.erase(config_.applications.begin() + static_cast<std::ptrdiff_t>(*applications_selected_));
  clear_application_form(true);
  persist_applications();
}

void WindowManager::clear_application_form(bool clear_selection) {
  application_name_buffer_.clear();
  application_command_buffer_.clear();
  application_style_index_ = 0;
  applications_active_field_ = ApplicationsField::Name;
  if (clear_selection) {
    applications_selected_.reset();
  }
}

void WindowManager::load_selected_application_into_form() {
  if (!applications_selected_ || *applications_selected_ >= config_.applications.size()) {
    return;
  }
  const auto& entry = config_.applications[*applications_selected_];
  application_name_buffer_ = entry.label;
  application_command_buffer_ = entry.command;
  application_style_index_ = entry.style % kApplicationStyleCount;
  applications_active_field_ = ApplicationsField::Name;
}

void WindowManager::cycle_application_style(int delta) {
  const int count = static_cast<int>(kApplicationStyleCount);
  int next = static_cast<int>(application_style_index_) + delta;
  while (next < 0) {
    next += count;
  }
  application_style_index_ = static_cast<unsigned int>(next % count);
}

void WindowManager::draw_preferences_window(Client& client) {
  if (preferences_palette_.empty()) {
    preferences_palette_ = build_preferences_palette(display_);
  }

  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  XSetForeground(display_, gc, theme_.frame_inactive_pixel);
  XDrawString(display_, target, gc, 12, 18, "Preferences", 11);
  const std::string subtitle = "Click a swatch to change the desktop background immediately.";
  XDrawString(display_, target, gc, 12, 34, subtitle.c_str(), static_cast<int>(subtitle.size()));

  const int grid_x = 12;
  const int grid_y = 52;
  const int cell_w = 64;
  const int cell_h = 46;
  const int label_h = 14;
  const int columns = 12;
  const std::string current_color = config_.background_color.empty() ? std::string() : config_.background_color;

  for (std::size_t index = 0; index < preferences_palette_.size(); ++index) {
    const auto& swatch = preferences_palette_[index];
    const int col = static_cast<int>(index % columns);
    const int row = static_cast<int>(index / columns);
    const int x = grid_x + col * cell_w;
    const int y = grid_y + row * cell_h;
    const bool selected = !current_color.empty() && current_color == swatch.token;
    if (selected) {
      XSetForeground(display_, gc, theme_.frame_active_pixel);
      XFillRectangle(display_, target, gc, x - 2, y - 2, static_cast<unsigned int>(cell_w - 6), static_cast<unsigned int>(cell_h - 4));
    }
    XSetForeground(display_, gc, swatch.pixel);
    XFillRectangle(display_, target, gc, x, y, static_cast<unsigned int>(cell_w - 8), static_cast<unsigned int>(cell_h - label_h - 4));
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, x, y, static_cast<unsigned int>(cell_w - 8), static_cast<unsigned int>(cell_h - label_h - 4));
    const int text_y = y + cell_h - 7;
    XSetForeground(display_, gc, theme_.menu_text_pixel);
    XDrawString(display_, target, gc, x + 2, text_y, swatch.token.c_str(), static_cast<int>(swatch.token.size()));
  }
  XFreeGC(display_, gc);
}

bool WindowManager::load_about_icon() {
  if (about_icon_loaded_) {
    return !about_icon_pixels_.empty();
  }
  about_icon_loaded_ = true;

  const std::filesystem::path path = locate_resource_path("res/chimera.png").value_or(std::filesystem::path("res/chimera.png"));
  about_icon_path_ = path;
  const std::optional<PngImage> image = load_png_image(path);
  if (!image || image->pixels.empty()) {
    chiux::log::warn(std::string("about icon not found: ") + path.string());
    return false;
  }

  about_icon_width_ = image->width;
  about_icon_height_ = image->height;
  about_icon_pixels_.assign(static_cast<std::size_t>(about_icon_width_) * static_cast<std::size_t>(about_icon_height_), 0);
  for (unsigned int y = 0; y < about_icon_height_; ++y) {
    for (unsigned int x = 0; x < about_icon_width_; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * about_icon_width_ + x) * 4u;
      const unsigned int sr = image->pixels[index + 0];
      const unsigned int sg = image->pixels[index + 1];
      const unsigned int sb = image->pixels[index + 2];
      const unsigned int sa = image->pixels[index + 3];
      const unsigned int br = static_cast<unsigned int>((theme_.menu_pixel >> 16) & 0xFFu);
      const unsigned int bg = static_cast<unsigned int>((theme_.menu_pixel >> 8) & 0xFFu);
      const unsigned int bb = static_cast<unsigned int>(theme_.menu_pixel & 0xFFu);
      const unsigned int out_r = static_cast<unsigned int>((sr * sa + br * (255u - sa)) / 255u);
      const unsigned int out_g = static_cast<unsigned int>((sg * sa + bg * (255u - sa)) / 255u);
      const unsigned int out_b = static_cast<unsigned int>((sb * sa + bb * (255u - sa)) / 255u);
      XVisualInfo visual_info{};
      visual_info.red_mask = DefaultVisual(display_, screen_)->red_mask;
      visual_info.green_mask = DefaultVisual(display_, screen_)->green_mask;
      visual_info.blue_mask = DefaultVisual(display_, screen_)->blue_mask;
      const unsigned long pixel = rgba_to_pixel(visual_info, out_r, out_g, out_b);
      about_icon_pixels_[static_cast<std::size_t>(y) * about_icon_width_ + x] = pixel;
    }
  }
  return true;
}

void WindowManager::draw_about_window(Client& client) {
  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, 1, static_cast<int>(draw_height) - 2, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XDrawLine(display_, target, gc, static_cast<int>(draw_width) - 2, 1, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, 0, 0, draw_width - 1, draw_height - 1);

  const std::string title = "chiux";
  const std::string subtitle = std::string("Version ") + CHIUX_VERSION;
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const std::tm* tm = std::localtime(&now);
  const int year = tm ? (1900 + tm->tm_year) : 2026;
  const std::string year_text = std::to_string(year);

  XFontStruct* title_font = load_about_title_font(display_);
  XFontStruct* body_font = load_about_body_font(display_);
  if (title_font) {
    XSetFont(display_, gc, title_font->fid);
  }
  const int title_y = 36;
  const int title_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(title_font, title) / 2);
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawString(display_, target, gc, title_x, title_y, title.c_str(), static_cast<int>(title.size()));

  if (body_font) {
    XSetFont(display_, gc, body_font->fid);
  }
  const int body_line_height = body_font ? (body_font->ascent + body_font->descent + 4) : 16;
  const int version_y = 64;
  const int year_y = version_y + body_line_height;
  const int version_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(body_font, subtitle) / 2);
  const int year_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(body_font, year_text) / 2);
  XDrawString(display_, target, gc, version_x, version_y, subtitle.c_str(), static_cast<int>(subtitle.size()));
  XDrawString(display_, target, gc, year_x, year_y, year_text.c_str(), static_cast<int>(year_text.size()));

  const int divider_y = year_y + body_line_height + 10;
  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, 20, divider_y, static_cast<int>(draw_width) - 21, divider_y);
  XDrawLine(display_, target, gc, 20, divider_y + 1, static_cast<int>(draw_width) - 21, divider_y + 1);

  const int icon_box_x = 24;
  const int icon_box_y = divider_y + 16;
  const unsigned int icon_box_w = 156;
  const unsigned int icon_box_h = draw_height > static_cast<unsigned int>(icon_box_y + 22) ? draw_height - static_cast<unsigned int>(icon_box_y) - 18 : 120;
  const int text_box_x = icon_box_x + static_cast<int>(icon_box_w) + 22;
  const unsigned int text_box_w = draw_width > static_cast<unsigned int>(text_box_x) + 24 ? draw_width - static_cast<unsigned int>(text_box_x) - 24 : 120;
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, icon_box_x, icon_box_y, icon_box_w, icon_box_h);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, icon_box_x + 1, icon_box_y + 1, icon_box_w - 1, icon_box_h - 1);

  if (load_about_icon() && about_icon_width_ > 0 && about_icon_height_ > 0 && !about_icon_pixels_.empty()) {
    const unsigned int max_icon_w = icon_box_w > 24 ? icon_box_w - 24 : icon_box_w;
    const unsigned int max_icon_h = icon_box_h > 24 ? icon_box_h - 24 : icon_box_h;
    const double scale = std::min(1.0, std::min(static_cast<double>(max_icon_w) / static_cast<double>(about_icon_width_),
                                                static_cast<double>(max_icon_h) / static_cast<double>(about_icon_height_)));
    const unsigned int target_w = std::max(1u, static_cast<unsigned int>(std::lround(static_cast<double>(about_icon_width_) * scale)));
    const unsigned int target_h = std::max(1u, static_cast<unsigned int>(std::lround(static_cast<double>(about_icon_height_) * scale)));
    const int icon_x = icon_box_x + std::max(0, (static_cast<int>(icon_box_w) - static_cast<int>(target_w)) / 2);
    const int icon_y = icon_box_y + std::max(0, (static_cast<int>(icon_box_h) - static_cast<int>(target_h)) / 2);
    Visual* visual = DefaultVisual(display_, screen_);
    XImage* image = XCreateImage(display_, visual, static_cast<unsigned int>(DefaultDepth(display_, screen_)), ZPixmap, 0, nullptr,
                                 target_w, target_h, 32, 0);
    if (image) {
      image->data = static_cast<char*>(std::malloc(static_cast<std::size_t>(image->bytes_per_line) * target_h));
      if (image->data) {
        std::memset(image->data, 0, static_cast<std::size_t>(image->bytes_per_line) * target_h);
        for (unsigned int y = 0; y < target_h; ++y) {
          for (unsigned int x = 0; x < target_w; ++x) {
            const unsigned int src_x = std::min(about_icon_width_ - 1u, static_cast<unsigned int>(std::lround(static_cast<double>(x) / scale)));
            const unsigned int src_y = std::min(about_icon_height_ - 1u, static_cast<unsigned int>(std::lround(static_cast<double>(y) / scale)));
            const unsigned long pixel = about_icon_pixels_[static_cast<std::size_t>(src_y) * about_icon_width_ + src_x];
            XPutPixel(image, static_cast<int>(x), static_cast<int>(y), pixel);
          }
        }
        XPutImage(display_, target, gc, image, 0, 0, icon_x, icon_y, target_w, target_h);
      }
      image->data = nullptr;
      XDestroyImage(image);
    } else {
      XSetForeground(display_, gc, theme_.frame_active_pixel);
      XDrawString(display_, target, gc, icon_box_x + 24, icon_box_y + static_cast<int>(icon_box_h) / 2, "chimera.png", 11);
    }
  }

  const std::string license_header = "BSD 2-Clause Summary";
  const std::string license_summary =
      "Redistribution and use in source and binary forms, with or without modification, are permitted "
      "provided that the copyright notice, conditions, and disclaimer are kept.";
  const int summary_title_y = icon_box_y + 20;
  const int summary_text_y = summary_title_y + body_line_height + 8;
  const auto lines = wrap_text_to_width(body_font, license_summary, static_cast<int>(text_box_w));
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  if (body_font) {
    XSetFont(display_, gc, body_font->fid);
  }
  XDrawString(display_, target, gc, text_box_x, summary_title_y, license_header.c_str(), static_cast<int>(license_header.size()));
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const int y = summary_text_y + static_cast<int>(i) * body_line_height;
    XDrawString(display_, target, gc, text_box_x, y, lines[i].c_str(), static_cast<int>(lines[i].size()));
  }
  XFreeGC(display_, gc);
}

void WindowManager::draw_home_window(Client& client) {
  Drawable target = client.content_backing ? client.content_backing : client.window;
  const unsigned int draw_width = client.content_backing ? client.content_width : client.width;
  const unsigned int draw_height = client.content_backing ? client.content_height : client.height;
  GC gc = XCreateGC(display_, target, 0, nullptr);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, 0, 0, draw_width, draw_height);

  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, 1, static_cast<int>(draw_height) - 2, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XDrawLine(display_, target, gc, static_cast<int>(draw_width) - 2, 1, static_cast<int>(draw_width) - 2, static_cast<int>(draw_height) - 2);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, 0, 0, draw_width - 1, draw_height - 1);

  const std::string title = "Home";
  const std::string subtitle = "System overview";
  XFontStruct* title_font = load_about_title_font(display_);
  XFontStruct* body_font = load_about_body_font(display_);
  if (title_font) {
    XSetFont(display_, gc, title_font->fid);
  }
  const int title_y = 36;
  const int title_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(title_font, title) / 2);
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawString(display_, target, gc, title_x, title_y, title.c_str(), static_cast<int>(title.size()));

  if (body_font) {
    XSetFont(display_, gc, body_font->fid);
  }
  const int body_line_height = body_font ? (body_font->ascent + body_font->descent + 4) : 16;
  const int subtitle_y = 58;
  const int subtitle_x = std::max(0, static_cast<int>(draw_width / 2) - text_width(body_font, subtitle) / 2);
  XDrawString(display_, target, gc, subtitle_x, subtitle_y, subtitle.c_str(), static_cast<int>(subtitle.size()));

  char host_buffer[256] = {};
  if (gethostname(host_buffer, sizeof(host_buffer) - 1) != 0) {
    std::snprintf(host_buffer, sizeof(host_buffer), "localhost");
  }
  const std::string host = host_buffer;

  std::string user = "unknown";
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_name) {
    user = pw->pw_name;
  }

  struct utsname uname_info {};
  std::string kernel = "unknown";
  if (uname(&uname_info) == 0) {
    kernel = std::string(uname_info.sysname) + " " + uname_info.release + " " + uname_info.machine;
  }

  std::string uptime = "unavailable";
  struct sysinfo info {};
  if (sysinfo(&info) == 0) {
    uptime = format_duration(static_cast<long long>(info.uptime));
  }

  const auto now_text = format_time_point(std::chrono::system_clock::now());
  const std::string home_path = home_directory().string();
  const DiskSnapshot root_disk = sample_disk("Root", "/");
  const DiskSnapshot home_disk = sample_disk("Home", home_directory());

  const int panel_top = 84;
  const int panel_bottom = static_cast<int>(draw_height) - 24;
  const int panel_height = std::max(1, panel_bottom - panel_top);
  const int left_x = 24;
  const int left_w = 208;
  const int right_x = left_x + left_w + 20;
  const int right_w = std::max(1, static_cast<int>(draw_width) - right_x - 24);
  const int left_inner = left_w - 2;
  const int right_inner = right_w - 2;

  auto draw_panel = [&](int x, int y, int w, int h) {
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, target, gc, x, y, static_cast<unsigned int>(w), static_cast<unsigned int>(h));
    XSetForeground(display_, gc, theme_.menu_pixel);
    XFillRectangle(display_, target, gc, x + 1, y + 1, static_cast<unsigned int>(std::max(1, w - 1)), static_cast<unsigned int>(std::max(1, h - 1)));
  };

  draw_panel(left_x, panel_top, left_inner, panel_height);
  draw_panel(right_x, panel_top, right_inner, panel_height);

  const int house_x = left_x + 34;
  const int house_y = panel_top + 28;
  const int house_w = 136;
  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, house_x + 18, house_y + 4, house_x + house_w / 2, house_y - 18);
  XDrawLine(display_, target, gc, house_x + house_w / 2, house_y - 18, house_x + house_w - 18, house_y + 4);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawLine(display_, target, gc, house_x + 12, house_y + 6, house_x + house_w / 2, house_y - 12);
  XDrawLine(display_, target, gc, house_x + house_w / 2, house_y - 12, house_x + house_w - 12, house_y + 6);
  XFillRectangle(display_, target, gc, house_x + 30, house_y + 20, 76, 56);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, target, gc, house_x + 33, house_y + 23, 70, 50);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, target, gc, house_x + 30, house_y + 20, 76, 56);
  XFillRectangle(display_, target, gc, house_x + 61, house_y + 44, 12, 32);
  XDrawRectangle(display_, target, gc, house_x + 61, house_y + 44, 12, 32);
  XDrawLine(display_, target, gc, house_x + 28, house_y + 76, house_x + 108, house_y + 76);
  XDrawLine(display_, target, gc, house_x + 28, house_y + 76, house_x + 20, house_y + 82);
  XDrawLine(display_, target, gc, house_x + 108, house_y + 76, house_x + 116, house_y + 82);
  XDrawLine(display_, target, gc, house_x + 20, house_y + 82, house_x + 116, house_y + 82);

  if (body_font) {
    XSetFont(display_, gc, body_font->fid);
  }
  const std::string identity = user + "@" + host;
  const int identity_x = left_x + std::max(0, (left_inner - text_width(body_font, identity)) / 2);
  const int identity_y = panel_top + 162;
  XSetForeground(display_, gc, theme_.menu_text_pixel);
  XDrawString(display_, target, gc, identity_x, identity_y, identity.c_str(), static_cast<int>(identity.size()));
  const int home_path_y = identity_y + body_line_height;
  const int home_path_x = left_x + 12;
  std::string home_path_line = home_path;
  if (home_path_line.size() > 24) {
    home_path_line = "..." + home_path_line.substr(home_path_line.size() - 21);
  }
  XDrawString(display_, target, gc, home_path_x, home_path_y, home_path_line.c_str(), static_cast<int>(home_path_line.size()));

  const int section_x = right_x + 18;
  int section_y = panel_top + 28;
  const auto draw_label_value = [&](const std::string& label, const std::string& value) {
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawString(display_, target, gc, section_x, section_y, label.c_str(), static_cast<int>(label.size()));
    section_y += body_line_height;
    XSetForeground(display_, gc, theme_.menu_text_pixel);
    XDrawString(display_, target, gc, section_x + 18, section_y, value.c_str(), static_cast<int>(value.size()));
    section_y += body_line_height + 4;
  };

  draw_label_value("User", identity);
  draw_label_value("Date", now_text);
  draw_label_value("Kernel", kernel);
  draw_label_value("Uptime", uptime);
  draw_label_value("Disk /", root_disk.summary);
  draw_label_value("Disk Home", home_disk.summary);

  XSetForeground(display_, gc, theme_.shadow_pixel);
  XDrawLine(display_, target, gc, section_x, panel_top + panel_height - 40, right_x + right_inner - 20, panel_top + panel_height - 40);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  const std::string footer = "chiux integrated system view";
  XDrawString(display_, target, gc, section_x, panel_top + panel_height - 18, footer.c_str(), static_cast<int>(footer.size()));

  XFreeGC(display_, gc);
}

void WindowManager::handle_internal_button_press(Client& client, const XButtonEvent& event) {
  focus_client(&client);
  if (client.internal_kind == InternalKind::Applications) {
    const ApplicationsLayout layout = applications_layout_for(client.width, client.height, applications_details_collapsed_);
    const auto inside = [](int px, int py, int x, int y, unsigned int w, unsigned int h) {
      return px >= x && px <= x + static_cast<int>(w) && py >= y && py <= y + static_cast<int>(h);
    };
    if (inside(event.x, event.y, layout.toggle_x, layout.toggle_y, layout.toggle_w, layout.toggle_h)) {
      applications_details_collapsed_ = !applications_details_collapsed_;
      draw_internal_content(client);
      return;
    }
    if (!applications_details_collapsed_) {
      if (inside(event.x, event.y, layout.name_x, layout.name_y, layout.name_w, layout.field_h)) {
        applications_active_field_ = ApplicationsField::Name;
        draw_internal_content(client);
        return;
      }
      if (inside(event.x, event.y, layout.command_x, layout.command_y, layout.command_w, layout.field_h)) {
        applications_active_field_ = ApplicationsField::Command;
        draw_internal_content(client);
        return;
      }
      if (inside(event.x, event.y, layout.open_x, layout.button_y, layout.button_w, layout.button_h)) {
        if (applications_selected_ && *applications_selected_ < config_.applications.size()) {
          run_launcher_command(config_.applications[*applications_selected_].command);
        }
        return;
      }
      if (inside(event.x, event.y, layout.add_x, layout.button_y, layout.button_w, layout.button_h)) {
        add_application_entry();
        draw_internal_content(client);
        return;
      }
      if (inside(event.x, event.y, layout.update_x, layout.button_y, layout.button_w, layout.button_h)) {
        update_selected_application();
        draw_internal_content(client);
        return;
      }
      if (inside(event.x, event.y, layout.remove_x, layout.button_y, layout.button_w, layout.button_h)) {
        remove_selected_application();
        draw_internal_content(client);
        return;
      }
      if (inside(event.x, event.y, layout.clear_x, layout.button_y, layout.button_w, layout.button_h)) {
        clear_application_form(true);
        draw_internal_content(client);
        return;
      }
      for (unsigned int style = 0; style < kApplicationStyleCount; ++style) {
        const int chip_x = layout.style_x + static_cast<int>(style) * static_cast<int>(layout.style_chip_w + 6);
        if (inside(event.x, event.y, chip_x, layout.style_row_y, layout.style_chip_w, layout.style_chip_h)) {
          application_style_index_ = style;
          draw_internal_content(client);
          return;
        }
      }
    }
    const auto hit = hit_test_application_icon(client, event.x, event.y);
    if (!hit) {
      clear_application_form(true);
      applications_drag_index_.reset();
      draw_internal_content(client);
      return;
    }
    applications_selected_ = hit;
    load_selected_application_into_form();
    applications_drag_index_ = hit;
    applications_drag_moved_ = false;
    applications_drag_start_x_ = event.x;
    applications_drag_start_y_ = event.y;
    applications_drag_offset_x_ = event.x - (layout.panel_x + config_.applications[*hit].x);
    applications_drag_offset_y_ = event.y - (layout.panel_y + config_.applications[*hit].y);
    grab_pointer_for_drag(client.window);
    draw_internal_content(client);
    return;
  }
  if (client.internal_kind != InternalKind::Browser) {
    if (client.internal_kind == InternalKind::Preferences) {
      if (preferences_palette_.empty()) {
        preferences_palette_ = build_preferences_palette(display_);
      }
      const int grid_x = 12;
      const int grid_y = 52;
      const int cell_w = 64;
      const int cell_h = 46;
      const int columns = 12;
      if (event.x >= grid_x && event.y >= grid_y) {
        const int col = (event.x - grid_x) / cell_w;
        const int row = (event.y - grid_y) / cell_h;
        if (col >= 0 && col < columns && row >= 0) {
          const std::size_t index = static_cast<std::size_t>(row * columns + col);
          if (index < preferences_palette_.size()) {
            apply_background_color(preferences_palette_[index].token);
            draw_internal_content(client);
          }
        }
      }
    }
    return;
  }
  if (event.y >= 42 && event.y <= 60 && event.x >= 12 && event.x <= 52) {
    if (file_manager_path_.has_parent_path()) {
      file_manager_path_ = file_manager_path_.parent_path();
      file_manager_selected_ = 0;
      file_manager_scroll_ = 0;
      refresh_file_manager();
      draw_internal_content(client);
    }
    return;
  }
  const int row_h = 20;
  const int list_y = 68;
  if (event.y >= list_y) {
    const std::size_t index = static_cast<std::size_t>((event.y - list_y) / row_h);
    if (index == 0 || (index > 0 && file_manager_scroll_ + index - 1 < file_manager_entries_.size())) {
      file_manager_selected_ = (index == 0) ? 0 : file_manager_scroll_ + index;
      ensure_file_manager_selection_visible(client);
      draw_internal_content(client);
    }
  }
}

void WindowManager::handle_internal_button_release(Client& client, const XButtonEvent&) {
  if (client.internal_kind == InternalKind::Applications) {
    if (applications_drag_index_) {
      if (applications_drag_moved_) {
        persist_applications();
      } else if (*applications_drag_index_ < config_.applications.size()) {
        run_launcher_command(config_.applications[*applications_drag_index_].command);
      }
    }
    applications_drag_index_.reset();
    applications_drag_moved_ = false;
    ungrab_pointer_for_drag();
    return;
  }
  if (client.internal_kind != InternalKind::Browser) {
    return;
  }
  if (file_manager_selected_ == 0) {
    if (file_manager_path_.has_parent_path()) {
      file_manager_path_ = file_manager_path_.parent_path();
      file_manager_scroll_ = 0;
      refresh_file_manager();
      draw_internal_content(client);
    }
    return;
  }
  const std::size_t entry_index = file_manager_selected_ - 1;
  if (entry_index >= file_manager_entries_.size()) {
    return;
  }
  const auto& entry = file_manager_entries_[entry_index];
  std::error_code ec;
  if (entry.is_directory(ec)) {
    file_manager_path_ = entry.path();
    file_manager_selected_ = 0;
    file_manager_scroll_ = 0;
    refresh_file_manager();
    draw_internal_content(client);
  } else {
    util::spawn_command(std::string("xdg-open ") + "\"" + entry.path().string() + "\"");
  }
}

void WindowManager::handle_internal_motion_notify(Client& client, const XMotionEvent& event) {
  if (client.internal_kind == InternalKind::Applications) {
    if (!applications_drag_index_ || *applications_drag_index_ >= config_.applications.size()) {
      return;
    }
    if (!applications_drag_moved_) {
      const int dx = std::abs(event.x - applications_drag_start_x_);
      const int dy = std::abs(event.y - applications_drag_start_y_);
      if (dx < 4 && dy < 4) {
        return;
      }
      applications_drag_moved_ = true;
    }
    const ApplicationsLayout layout = applications_layout_for(client.width, client.height, applications_details_collapsed_);
    auto& entry = config_.applications[*applications_drag_index_];
    const int max_x = std::max(0, static_cast<int>(layout.panel_w) - kApplicationsIconWidth - 8);
    const int max_y = std::max(0, static_cast<int>(layout.panel_h) - kApplicationsIconHeight - 8);
    entry.x = std::clamp(event.x - layout.panel_x - applications_drag_offset_x_, 8, max_x);
    entry.y = std::clamp(event.y - layout.panel_y - applications_drag_offset_y_, 8, max_y);
    draw_internal_content(client);
    return;
  }
  if (client.internal_kind != InternalKind::Browser) {
    return;
  }
  const int row_h = 20;
  const int list_y = 68;
  if (event.y >= list_y) {
    const std::size_t index = static_cast<std::size_t>((event.y - list_y) / row_h);
    if (index == 0 || file_manager_scroll_ + index - 1 < file_manager_entries_.size()) {
      file_manager_selected_ = (index == 0) ? 0 : file_manager_scroll_ + index;
      ensure_file_manager_selection_visible(client);
      draw_internal_content(client);
    }
  }
}

void WindowManager::handle_internal_key_press(Client& client, const XKeyEvent& event) {
  if (client.internal_kind == InternalKind::Execute) {
    char text[32] = {};
    KeySym sym = NoSymbol;
    const int len = XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text) - 1, &sym, nullptr);
    if (sym == XK_Return || sym == XK_KP_Enter) {
      if (!execute_buffer_.empty()) {
        const std::string command = execute_buffer_;
        execute_buffer_.clear();
        run_launcher_command(command);
        if (execute_window_ != 0) {
          const Window launcher_window = execute_window_;
          execute_window_ = 0;
          unmanage_window(launcher_window, false);
        }
        return;
      }
    } else if (sym == XK_BackSpace) {
      if (!execute_buffer_.empty()) {
        execute_buffer_.pop_back();
      }
    } else if (sym == XK_Escape) {
      execute_buffer_.clear();
    } else if (len > 0) {
      for (int i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isprint(ch) != 0) {
          execute_buffer_.push_back(static_cast<char>(ch));
        }
      }
    }
    draw_internal_content(client);
  } else if (client.internal_kind == InternalKind::Browser) {
    const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    if (sym == XK_Return || sym == XK_KP_Enter) {
      if (file_manager_selected_ == 0) {
        if (file_manager_path_.has_parent_path()) {
          file_manager_path_ = file_manager_path_.parent_path();
          file_manager_scroll_ = 0;
          refresh_file_manager();
          draw_internal_content(client);
        }
      } else {
        const std::size_t entry_index = file_manager_selected_ - 1;
        if (entry_index < file_manager_entries_.size()) {
          const auto& entry = file_manager_entries_[entry_index];
          std::error_code ec;
          if (entry.is_directory(ec)) {
            file_manager_path_ = entry.path();
            file_manager_selected_ = 0;
            file_manager_scroll_ = 0;
            refresh_file_manager();
            draw_internal_content(client);
          } else {
            util::spawn_command(std::string("xdg-open ") + "\"" + entry.path().string() + "\"");
          }
        }
      }
    } else if (sym == XK_BackSpace) {
      if (file_manager_path_.has_parent_path()) {
        file_manager_path_ = file_manager_path_.parent_path();
        file_manager_selected_ = 0;
        refresh_file_manager();
        draw_internal_content(client);
      }
    } else if (sym == XK_Up) {
      if (file_manager_selected_ > 0) {
        --file_manager_selected_;
        if (file_manager_selected_ < file_manager_scroll_ + 1 && file_manager_scroll_ > 0) {
          --file_manager_scroll_;
        }
        ensure_file_manager_selection_visible(client);
        draw_internal_content(client);
      }
    } else if (sym == XK_Down) {
      const std::size_t max_index = file_manager_entries_.size();
      if (file_manager_selected_ < max_index) {
        ++file_manager_selected_;
        ensure_file_manager_selection_visible(client);
        draw_internal_content(client);
      }
    }
  } else if (client.internal_kind == InternalKind::Applications) {
    if (applications_details_collapsed_) {
      const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
      if (sym == XK_Delete) {
        remove_selected_application();
      } else if (sym == XK_Escape) {
        clear_application_form(true);
      } else if ((sym == XK_Return || sym == XK_KP_Enter) && applications_selected_ && *applications_selected_ < config_.applications.size()) {
        run_launcher_command(config_.applications[*applications_selected_].command);
      }
      draw_internal_content(client);
      return;
    }
    char text[64] = {};
    KeySym sym = NoSymbol;
    const int len = XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text) - 1, &sym, nullptr);
    std::string& active_buffer = applications_active_field_ == ApplicationsField::Name ? application_name_buffer_ : application_command_buffer_;
    if (sym == XK_Return || sym == XK_KP_Enter) {
      if (applications_selected_) {
        update_selected_application();
      } else {
        add_application_entry();
      }
    } else if (sym == XK_BackSpace) {
      if (!active_buffer.empty()) {
        active_buffer.pop_back();
      }
    } else if (sym == XK_Tab) {
      applications_active_field_ = applications_active_field_ == ApplicationsField::Name
          ? ApplicationsField::Command
          : ApplicationsField::Name;
    } else if (sym == XK_Left) {
      cycle_application_style(-1);
    } else if (sym == XK_Right) {
      cycle_application_style(1);
    } else if (sym == XK_Delete) {
      remove_selected_application();
    } else if (sym == XK_Up) {
      if (applications_selected_ && *applications_selected_ > 0) {
        applications_selected_ = *applications_selected_ - 1;
        load_selected_application_into_form();
      }
    } else if (sym == XK_Down) {
      if (applications_selected_ && *applications_selected_ + 1 < config_.applications.size()) {
        applications_selected_ = *applications_selected_ + 1;
        load_selected_application_into_form();
      }
    } else if (sym == XK_Escape) {
      if (!application_name_buffer_.empty() || !application_command_buffer_.empty()) {
        clear_application_form(false);
      } else {
        XDestroyWindow(display_, client.frame);
        return;
      }
    } else if (len > 0) {
      for (int i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isprint(ch) != 0) {
          active_buffer.push_back(static_cast<char>(ch));
        }
      }
    }
    draw_internal_content(client);
  } else if (client.internal_kind == InternalKind::Preferences) {
    const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    if (sym == XK_Escape) {
      XDestroyWindow(display_, client.frame);
    }
  } else if (client.internal_kind == InternalKind::About) {
    const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    if (sym == XK_Escape) {
      XDestroyWindow(display_, client.frame);
    }
  } else if (client.internal_kind == InternalKind::Home) {
    const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    if (sym == XK_Escape) {
      XDestroyWindow(display_, client.frame);
    }
  }
}

void WindowManager::sync_desktop_icons() {
  if (!desktop_) {
    return;
  }
  std::vector<ui::DesktopIcon> icons;
  icons.reserve(config_.desktop_icons.size());
  for (const auto& icon : config_.desktop_icons) {
    icons.push_back({icon.label, icon.command, icon.x, icon.y});
  }
  desktop_->set_icons(std::move(icons));
}

void WindowManager::update_desktop_properties() {
  const unsigned long count = kDesktopCount;
  const unsigned long current = current_desktop_;
  XChangeProperty(display_, root_, net_number_of_desktops_, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&count), 1);
  XChangeProperty(display_, root_, net_current_desktop_, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&current), 1);
}

void WindowManager::scan_existing_windows() {
  Window root_return = 0;
  Window parent_return = 0;
  Window* children = nullptr;
  unsigned int child_count = 0;
  if (!XQueryTree(display_, root_, &root_return, &parent_return, &children, &child_count)) {
    return;
  }
  for (unsigned int i = 0; i < child_count; ++i) {
    XWindowAttributes attrs{};
    if (XGetWindowAttributes(display_, children[i], &attrs) && is_top_level_candidate(attrs)) {
      manage_window(children[i]);
    }
  }
  if (children) {
    XFree(children);
  }
}

Client* WindowManager::find_client(Window window) {
  auto it = clients_.find(window);
  if (it != clients_.end()) {
    return &it->second;
  }
  for (auto& [key, client] : clients_) {
    if (client.frame == window) {
      return &client;
    }
  }
  return nullptr;
}

Client* WindowManager::find_client_by_frame(Window frame) {
  for (auto& [window, client] : clients_) {
    (void)window;
    if (client.frame == frame) {
      return &client;
    }
  }
  return nullptr;
}

Client& WindowManager::frame_client(Window window) {
  return clients_.at(window);
}

void WindowManager::manage_window(Window window) {
  if (clients_.contains(window)) {
    return;
  }

  XWindowAttributes attrs{};
  if (!XGetWindowAttributes(display_, window, &attrs) || attrs.override_redirect) {
    return;
  }

  Client client{};
  client.window = window;
  client.x = attrs.x;
  client.y = clamp_client_y(attrs.y);
  client.width = static_cast<unsigned int>(std::max(1, attrs.width));
  client.height = static_cast<unsigned int>(std::max(1, attrs.height));
  XSizeHints size_hints{};
  long supplied = 0;
  if (XGetWMNormalHints(display_, window, &size_hints, &supplied)) {
    if ((size_hints.flags & PMinSize) != 0) {
      client.min_width = static_cast<unsigned int>(std::max<long>(1L, size_hints.min_width));
      client.min_height = static_cast<unsigned int>(std::max<long>(1L, size_hints.min_height));
    }
    if ((size_hints.flags & PMaxSize) != 0 && size_hints.max_width == size_hints.min_width && size_hints.max_height == size_hints.min_height) {
      client.fixed_size = true;
      client.min_width = static_cast<unsigned int>(std::max<long>(1L, size_hints.min_width));
      client.min_height = static_cast<unsigned int>(std::max<long>(1L, size_hints.min_height));
    }
  }
  client.width = std::max(client.width, client.min_width);
  client.height = std::max(client.height, client.min_height);
  client.desktop = current_desktop_;
  fetch_title(client);
  Window transient_for = None;
  if (XGetTransientForHint(display_, window, &transient_for) && transient_for != None) {
    client.transient = true;
    client.transient_for = transient_for;
    if (Client* parent = find_client(transient_for)) {
      client.desktop = parent->desktop;
    }
  }

  const unsigned int frame_height = theme_.title_height + client.height + theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_width = client.width + 2 * theme_.border_width + theme_.grow_box_size;

  XSetWindowAttributes shadow_attrs{};
  shadow_attrs.override_redirect = True;
  shadow_attrs.background_pixel = theme_.shadow_pixel;
  client.shadow = XCreateWindow(display_, root_,
                                client.x + static_cast<int>(theme_.shadow_offset),
                                client.y + static_cast<int>(theme_.shadow_offset),
                                frame_width, frame_height, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel, &shadow_attrs);

  XSetWindowAttributes frame_attrs{};
  frame_attrs.background_pixel = theme_.frame_inactive_pixel;
  frame_attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | SubstructureRedirectMask | SubstructureNotifyMask | EnterWindowMask;
  client.frame = XCreateWindow(display_, root_, client.x, client.y, frame_width, frame_height, theme_.border_width,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask, &frame_attrs);
  define_cursor(client.frame);
  define_cursor(window);

  XSelectInput(display_, window, PropertyChangeMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask);
  XAddToSaveSet(display_, window);
  XReparentWindow(display_, window, client.frame, static_cast<int>(theme_.border_width), static_cast<int>(theme_.title_height));
  XResizeWindow(display_, window, client.width, client.height);

  const bool mapped = attrs.map_state != IsUnmapped;
  client.mapped = mapped;
  clients_.emplace(window, std::move(client));
  Client& stored = clients_.at(window);
  stacking_order_.push_back(window);
  if (window == execute_window_) {
    stored.internal_kind = InternalKind::Execute;
  } else if (window == file_manager_window_) {
    stored.internal_kind = InternalKind::Browser;
  } else if (window == applications_window_) {
    stored.internal_kind = InternalKind::Applications;
  } else if (window == preferences_window_) {
    stored.internal_kind = InternalKind::Preferences;
  } else if (window == about_window_) {
    stored.internal_kind = InternalKind::About;
  } else if (window == home_window_) {
    stored.internal_kind = InternalKind::Home;
  }
  if (stored.internal_kind != InternalKind::Unset) {
    XSelectInput(display_, window, PropertyChangeMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask);
  }
  XChangeProperty(display_, stored.window, net_wm_desktop_, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&stored.desktop), 1);
  if (mapped) {
    XMapWindow(display_, stored.shadow);
    XMapWindow(display_, stored.frame);
    XMapWindow(display_, stored.window);
    std::array<Window, 2> restack{stored.frame, stored.shadow};
    XRestackWindows(display_, restack.data(), 2);
  }
  set_wm_state(stored, 1);
  update_client_list();
  draw_frame(stored);
  if (stored.desktop != current_desktop_) {
    set_client_desktop_visible(stored, false);
  }
  restack_managed_windows();
}

void WindowManager::unmanage_window(Window window, bool withdraw) {
  auto it = clients_.find(window);
  if (it == clients_.end()) {
    return;
  }
  Client client = it->second;
  if (client.internal_kind == InternalKind::Execute) {
    XUngrabKeyboard(display_, CurrentTime);
  }
  if (withdraw) {
    XReparentWindow(display_, client.window, root_, client.x, client.y);
    XUnmapWindow(display_, client.window);
  }
  XRemoveFromSaveSet(display_, client.window);
  if (client.shadow) {
    XDestroyWindow(display_, client.shadow);
  }
  if (client.frame) {
    XDestroyWindow(display_, client.frame);
  }
  release_internal_backing(client);
  if (focused_ == &it->second) {
    focused_ = nullptr;
    if (menubar_) {
      menubar_->set_application_title("chiux");
    }
    if (desktop_) {
      XSetInputFocus(display_, desktop_->window(), RevertToPointerRoot, CurrentTime);
    } else {
      XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
    }
  }
  if (client.window == execute_window_) {
    execute_window_ = 0;
    execute_buffer_.clear();
  }
  if (client.window == file_manager_window_) {
    file_manager_window_ = 0;
    file_manager_entries_.clear();
    file_manager_selected_ = 0;
  }
  if (client.window == applications_window_) {
    applications_window_ = 0;
    application_name_buffer_.clear();
    application_command_buffer_.clear();
    applications_selected_.reset();
    applications_drag_index_.reset();
    applications_drag_moved_ = false;
    applications_active_field_ = ApplicationsField::Name;
  }
  if (client.window == preferences_window_) {
    preferences_window_ = 0;
  }
  if (client.window == about_window_) {
    about_window_ = 0;
  }
  if (client.window == home_window_) {
    home_window_ = 0;
  }
  const auto order_it = std::find(stacking_order_.begin(), stacking_order_.end(), client.window);
  if (order_it != stacking_order_.end()) {
    stacking_order_.erase(order_it);
  }
  clients_.erase(it);
  update_client_list();
  refresh_keybindings();
  restack_managed_windows();
}

void WindowManager::configure_client(Client& client, int x, int y, unsigned int width, unsigned int height) {
  client.x = x;
  client.y = clamp_client_y(y);
  client.width = std::max(client.min_width, std::max(1u, width));
  client.height = std::max(client.min_height, std::max(1u, height));

  const unsigned int frame_width = client.width + 2 * theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_height = client.height + theme_.title_height + theme_.border_width + theme_.grow_box_size;
  update_client_shadow(client);
  XMoveResizeWindow(display_, client.frame, client.x, client.y, frame_width, frame_height);
  XMoveResizeWindow(display_, client.window, static_cast<int>(theme_.border_width), static_cast<int>(theme_.title_height), client.width, client.height);
  if (client.desktop_hidden && !client.iconic) {
    set_client_desktop_visible(client, false);
  }
  draw_frame(client);
  if (client.internal_kind != InternalKind::Unset) {
    draw_internal_content(client);
  }
}

void WindowManager::move_client(Client& client, int dx, int dy) {
  client.x += dx;
  client.y = clamp_client_y(client.y + dy);
  update_client_shadow(client);
  XMoveWindow(display_, client.frame, client.x, client.y);
  if (client.desktop_hidden && !client.iconic) {
    set_client_desktop_visible(client, false);
  }
}

void WindowManager::resize_client(Client& client, int width, int height) {
  client.width = std::max(client.min_width, static_cast<unsigned int>(std::max(1, width)));
  client.height = std::max(client.min_height, static_cast<unsigned int>(std::max(1, height)));
  const unsigned int frame_width = client.width + 2 * theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_height = client.height + theme_.title_height + theme_.border_width + theme_.grow_box_size;
  update_client_shadow(client);
  XResizeWindow(display_, client.frame, frame_width, frame_height);
  XResizeWindow(display_, client.window, client.width, client.height);
  if (client.desktop_hidden && !client.iconic) {
    set_client_desktop_visible(client, false);
  }
  draw_frame(client);
  if (client.internal_kind != InternalKind::Unset) {
    draw_internal_content(client);
  }
}

void WindowManager::toggle_zoom_client(Client& client) {
  if (client.fixed_size) {
    return;
  }
  if (client.zoomed) {
    if (client.has_restore_geometry) {
      configure_client(client, client.restore_x, client.restore_y, client.restore_width, client.restore_height);
    }
    client.zoomed = false;
    return;
  }
  client.restore_x = client.x;
  client.restore_y = client.y;
  client.restore_width = client.width;
  client.restore_height = client.height;
  client.has_restore_geometry = true;
  const unsigned int work_width = static_cast<unsigned int>(DisplayWidth(display_, screen_));
  const unsigned int work_height = static_cast<unsigned int>(DisplayHeight(display_, screen_)) - theme_.menu_height;
  configure_client(client, 0, static_cast<int>(theme_.menu_height), std::max(client.min_width, work_width - 18u), std::max(client.min_height, work_height - 28u));
  client.zoomed = true;
}

void WindowManager::set_client_active(Client& client, bool active) {
  client.focused = active;
  XSetWindowBorder(display_, client.frame, active ? theme_.frame_active_pixel : theme_.frame_inactive_pixel);
  draw_frame(client);
}

void WindowManager::update_client_shadow(Client& client) {
  if (client.shadow == 0) {
    return;
  }
  const unsigned int frame_width = client.iconic ? client.icon_width : client.width + 2 * theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_height = client.iconic ? client.icon_height : client.height + theme_.title_height + theme_.border_width + theme_.grow_box_size;
  XMoveResizeWindow(display_, client.shadow,
                    (client.iconic ? client.icon_x : client.x) + static_cast<int>(theme_.shadow_offset),
                    (client.iconic ? client.icon_y : client.y) + static_cast<int>(theme_.shadow_offset),
                    frame_width, frame_height);
}

void WindowManager::reflow_iconified_clients(unsigned int desktop) {
  if (!desktop_) {
    return;
  }

  std::vector<Client*> iconic_clients;
  iconic_clients.reserve(clients_.size());
  for (auto& [window, client] : clients_) {
    (void)window;
    if (client.desktop == desktop && client.iconic) {
      iconic_clients.push_back(&client);
    }
  }

  std::sort(iconic_clients.begin(), iconic_clients.end(), [](const Client* lhs, const Client* rhs) {
    return lhs->icon_order < rhs->icon_order;
  });

  const int screen_width = DisplayWidth(display_, screen_);
  const int base_y = std::max(8, DisplayHeight(display_, screen_) - static_cast<int>(theme_.menu_height) - 10 - 50);
  const int row_step = 60;
  const int left_margin = 18;
  const int right_margin = 18;
  const int max_x = std::max(left_margin, screen_width - right_margin - 140);

  int x = left_margin;
  int y = base_y;
  for (Client* client : iconic_clients) {
    if (x > max_x) {
      x = left_margin;
      y -= row_step;
    }
    client->icon_x = x;
    client->icon_y = y;
    set_client_desktop_visible(*client, true);
    x += static_cast<int>(client->icon_width) + 10;
  }
  restack_managed_windows();
}

void WindowManager::set_client_desktop_visible(Client& client, bool visible) {
  client.desktop_hidden = !visible;
  if (client.iconic) {
    const int target_x = visible ? client.icon_x : -20000 - static_cast<int>(client.icon_width);
    const int target_y = visible ? client.icon_y : -20000 - static_cast<int>(client.icon_height);
    XMoveResizeWindow(display_, client.shadow,
                      target_x + static_cast<int>(theme_.shadow_offset),
                      target_y + static_cast<int>(theme_.shadow_offset),
                      client.icon_width, client.icon_height);
    XMoveResizeWindow(display_, client.frame, target_x, target_y, client.icon_width, client.icon_height);
    if (visible) {
      XMapWindow(display_, client.shadow);
      XMapWindow(display_, client.frame);
      XUnmapWindow(display_, client.window);
      std::array<Window, 2> restack{client.frame, client.shadow};
      XRestackWindows(display_, restack.data(), 2);
      draw_frame(client);
    }
    return;
  }
  if (visible) {
    XMoveWindow(display_, client.shadow,
                client.x + static_cast<int>(theme_.shadow_offset),
                client.y + static_cast<int>(theme_.shadow_offset));
    XMoveWindow(display_, client.frame, client.x, client.y);
    std::array<Window, 2> restack{client.frame, client.shadow};
    XRestackWindows(display_, restack.data(), 2);
  } else {
    const int hidden_x = -20000 - static_cast<int>(client.width);
    const int hidden_y = -20000 - static_cast<int>(client.height);
    XMoveWindow(display_, client.shadow,
                hidden_x + static_cast<int>(theme_.shadow_offset),
                hidden_y + static_cast<int>(theme_.shadow_offset));
    XMoveWindow(display_, client.frame, hidden_x, hidden_y);
  }
}

void WindowManager::draw_frame(Client& client) {
  GC gc = XCreateGC(display_, client.frame, 0, nullptr);
  if (client.iconic) {
    const unsigned int frame_width = client.icon_width;
    const unsigned int frame_height = client.icon_height;
    XSetForeground(display_, gc, icon_shadow_pixel_);
    XFillRectangle(display_, client.frame, gc, 1, 1, frame_width - 1, frame_height - 1);
    XSetForeground(display_, gc, theme_.background_pixel);
    XFillRectangle(display_, client.frame, gc, 0, 0, frame_width - 2, frame_height - 2);
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, client.frame, gc, 0, 0, frame_width - 1, frame_height - 1);
    XDrawLine(display_, client.frame, gc, 1, 1, static_cast<int>(frame_width) - 2, 1);
    XDrawLine(display_, client.frame, gc, 1, 1, 1, static_cast<int>(frame_height) - 2);
    XSetForeground(display_, gc, theme_.menu_pixel);
    XFillRectangle(display_, client.frame, gc, 10, 13, 18, 12);
    XSetForeground(display_, gc, theme_.frame_active_pixel);
    XDrawRectangle(display_, client.frame, gc, 10, 13, 18, 12);
    XDrawLine(display_, client.frame, gc, 12, 15, 26, 15);
    XDrawLine(display_, client.frame, gc, 12, 19, 26, 19);
    const std::string title = shorten_label(client.title.empty() ? std::string("chiux") : client.title, 18);
    const int title_x = std::max(8, static_cast<int>(frame_width / 2) - static_cast<int>(title.size() * 3));
    XSetForeground(display_, gc, icon_shadow_pixel_);
    XDrawString(display_, client.frame, gc, title_x + 1, 42, title.c_str(), static_cast<int>(title.size()));
    XSetForeground(display_, gc, icon_text_pixel_);
    XDrawString(display_, client.frame, gc, title_x, 41, title.c_str(), static_cast<int>(title.size()));
    XFreeGC(display_, gc);
    return;
  }
  const unsigned long bg = client.focused ? theme_.title_active_pixel : theme_.title_inactive_pixel;
  const unsigned int frame_width = client.width + 2 * theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_height = client.height + theme_.title_height + theme_.border_width + theme_.grow_box_size;
  XSetForeground(display_, gc, theme_.shadow_pixel);
  XFillRectangle(display_, client.frame, gc, 2, 2, frame_width - 2, theme_.title_height + 1);
  XFillRectangle(display_, client.frame, gc, 2, 2, 2, frame_height - 2);
  XFillRectangle(display_, client.frame, gc, 2, static_cast<int>(frame_height) - 2, frame_width - 2, 2);

  XSetForeground(display_, gc, bg);
  XFillRectangle(display_, client.frame, gc, 0, 0, frame_width, theme_.title_height);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, client.frame, gc, 0, 0, frame_width - 1, theme_.title_height - 1);

  const unsigned int close_size = theme_.button_size - 2;
  const int close_x = 3;
  const int close_y = 3;
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, client.frame, gc, close_x, close_y, close_size, close_size);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, client.frame, gc, close_x, close_y, close_size, close_size);
  XDrawLine(display_, client.frame, gc, close_x + 3, close_y + 3, close_x + 8, close_y + 8);
  XDrawLine(display_, client.frame, gc, close_x + 8, close_y + 3, close_x + 3, close_y + 8);

  const unsigned int zoom_size = theme_.button_size - 2;
  const int zoom_x = static_cast<int>(frame_width) - static_cast<int>(zoom_size) - 4;
  const int zoom_y = 3;
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, client.frame, gc, zoom_x, zoom_y, zoom_size, zoom_size);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, client.frame, gc, zoom_x, zoom_y, zoom_size, zoom_size);
  XDrawRectangle(display_, client.frame, gc, zoom_x + 3, zoom_y + 3, 6, 6);

  const std::string title = client.title.empty() ? std::string("chiux") : client.title;
  const int title_x = std::max(20, static_cast<int>(frame_width / 2) - static_cast<int>(title.size() * 3));
  XSetForeground(display_, gc, client.focused ? theme_.title_text_pixel : theme_.menu_text_pixel);
  XDrawString(display_, client.frame, gc, title_x, 12, title.c_str(), static_cast<int>(title.size()));

  const unsigned int grow_size = theme_.grow_box_size;
  const int grow_x = static_cast<int>(frame_width) - static_cast<int>(grow_size);
  const int grow_y = static_cast<int>(frame_height) - static_cast<int>(grow_size);
  XSetForeground(display_, gc, theme_.menu_pixel);
  XFillRectangle(display_, client.frame, gc, grow_x, grow_y, grow_size, grow_size);
  XSetForeground(display_, gc, theme_.frame_active_pixel);
  XDrawRectangle(display_, client.frame, gc, grow_x, grow_y, grow_size, grow_size);
  XDrawLine(display_, client.frame, gc, grow_x + 3, grow_y + static_cast<int>(grow_size) - 4, grow_x + static_cast<int>(grow_size) - 4, grow_y + 3);
  XDrawLine(display_, client.frame, gc, grow_x + 6, grow_y + static_cast<int>(grow_size) - 4, grow_x + static_cast<int>(grow_size) - 4, grow_y + 6);
  XFreeGC(display_, gc);
}

void WindowManager::grab_pointer_for_drag(Window window) {
  XGrabPointer(display_, window, False, ButtonMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

void WindowManager::ungrab_pointer_for_drag() {
  XUngrabPointer(display_, CurrentTime);
}

void WindowManager::define_cursor(Window window) {
  if (window != None && arrow_cursor_ != None) {
    XDefineCursor(display_, window, arrow_cursor_);
  }
}

bool WindowManager::is_wm_protocol(Atom atom, Atom protocol) const {
  return atom == protocol;
}

void WindowManager::focus_client(Client* client) {
  if (!client) {
    return;
  }
  if (focused_ && focused_ != client) {
    set_client_active(*focused_, false);
  }
  focused_ = client;
  set_client_active(*client, true);
  if (client_supports_protocol(client->window, wm_take_focus_)) {
    XEvent message{};
    message.xclient.type = ClientMessage;
    message.xclient.window = client->window;
    message.xclient.message_type = wm_protocols_;
    message.xclient.format = 32;
    message.xclient.data.l[0] = static_cast<long>(wm_take_focus_);
    message.xclient.data.l[1] = CurrentTime;
    XSendEvent(display_, client->window, False, NoEventMask, &message);
  }
  XSetInputFocus(display_, client->window, RevertToPointerRoot, CurrentTime);
  set_active_window(client->window);
  raise_managed_window(client->window);
  if (menubar_) {
    menubar_->set_application_title(client->title);
  }
}

void WindowManager::set_active_window(Window window) {
  XChangeProperty(display_, root_, net_active_window_, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&window), 1);
}

void WindowManager::update_client_list() {
  std::vector<Window> windows;
  windows.reserve(clients_.size());
  for (const auto& [window, client] : clients_) {
    (void)client;
    windows.push_back(window);
  }
  if (windows.empty()) {
    XDeleteProperty(display_, root_, net_client_list_);
    return;
  }
  XChangeProperty(display_, root_, net_client_list_, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(windows.data()),
                  static_cast<int>(windows.size()));
}

void WindowManager::save_config() {
  if (!desktop_) {
    return;
  }
  try {
    const auto on_disk = config::Config::load_or_default(config_path_);
    config_.resolve_terminal_binary = on_disk.resolve_terminal_binary;
    config_.terminal_command = on_disk.terminal_command;
    config_.terminal_feel = on_disk.terminal_feel;
    config_.file_browser_command = on_disk.file_browser_command;
    config_.launchers = on_disk.launchers;
  } catch (const std::exception& ex) {
    chiux::log::warn(std::string("failed to refresh chiux config before save: ") + ex.what());
  }
  std::vector<config::DesktopIcon> icons;
  icons.reserve(desktop_->icons().size());
  std::unordered_set<std::string> seen;
  for (const auto& icon : desktop_->icons()) {
    const std::string key = icon.label + "\x1f" + icon.command;
    if (!seen.insert(key).second) {
      continue;
    }
    icons.push_back({icon.label, icon.command, icon.x, icon.y});
  }
  config_.desktop_icons = std::move(icons);
  {
    std::unordered_set<std::string> seen_apps;
    std::vector<config::ApplicationEntry> unique_apps;
    unique_apps.reserve(config_.applications.size());
    for (const auto& entry : config_.applications) {
      const std::string key = entry.label + "\x1f" + entry.command;
      if (!seen_apps.insert(key).second) {
        continue;
      }
      unique_apps.push_back(entry);
    }
    config_.applications = std::move(unique_apps);
  }
  try {
    config_.save(config_path_);
  } catch (const std::exception& e) {
    chiux::log::warn(std::string("config save failed: ") + e.what());
  }
}

void WindowManager::switch_to_desktop(unsigned int desktop) {
  if (desktop >= kDesktopCount) {
    return;
  }
  current_desktop_ = desktop;
  update_desktop_properties();
  for (auto& [window, client] : clients_) {
    (void)window;
    const bool show = client.desktop == current_desktop_;
    set_client_desktop_visible(client, show);
  }
  restack_managed_windows();
  reflow_iconified_clients(current_desktop_);
  if (menubar_) {
    std::string title = "chiux - ";
    title += desktop_names_[current_desktop_];
    if (focused_) {
      title = focused_->title + " - " + desktop_names_[current_desktop_];
    }
    menubar_->set_application_title(title);
  }
}

void WindowManager::move_focused_to_desktop(unsigned int desktop) {
  if (!focused_ || desktop >= kDesktopCount) {
    return;
  }
  focused_->desktop = desktop;
  XChangeProperty(display_, focused_->window, net_wm_desktop_, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&desktop), 1);
  if (desktop != current_desktop_) {
    set_client_desktop_visible(*focused_, false);
    focused_ = nullptr;
    switch_to_desktop(current_desktop_);
  }
}

void WindowManager::cycle_desktop(int delta) {
  const int next = static_cast<int>(current_desktop_) + delta;
  const int normalized = (next % static_cast<int>(kDesktopCount) + static_cast<int>(kDesktopCount)) % static_cast<int>(kDesktopCount);
  switch_to_desktop(static_cast<unsigned int>(normalized));
}

void WindowManager::set_wm_state(Client& client, long state) {
  long data[2] = {state, None};
  XChangeProperty(display_, client.window, wm_state_, wm_state_, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(data), 2);
}

void WindowManager::fetch_title(Client& client) {
  client.title = "chiux";

  Atom actual_type = None;
  int actual_format = 0;
  unsigned long item_count = 0;
  unsigned long bytes_after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(display_, client.window, net_wm_name_, 0, 2048, False, AnyPropertyType,
                         &actual_type, &actual_format, &item_count, &bytes_after, &data) == Success && data && item_count > 0) {
    client.title.assign(reinterpret_cast<char*>(data), static_cast<std::size_t>(item_count));
    XFree(data);
    if (!client.title.empty()) {
      return;
    }
  } else if (data) {
    XFree(data);
  }

  char* name = nullptr;
  if (XFetchName(display_, client.window, &name) && name) {
    client.title = name;
    XFree(name);
  }
}

bool WindowManager::client_supports_delete(Window window) const {
  return client_supports_protocol(window, wm_delete_window_);
}

bool WindowManager::client_supports_protocol(Window window, Atom protocol) const {
  Atom* protocols = nullptr;
  int count = 0;
  if (!XGetWMProtocols(display_, window, &protocols, &count) || !protocols) {
    return false;
  }
  bool supports = false;
  for (int i = 0; i < count; ++i) {
    if (protocols[i] == protocol) {
      supports = true;
      break;
    }
  }
  XFree(protocols);
  return supports;
}

void WindowManager::iconify_client(Client& client) {
  if (client.iconic) {
    return;
  }
  client.restore_x = client.x;
  client.restore_y = client.y;
  client.restore_width = client.width;
  client.restore_height = client.height;
  client.has_restore_geometry = true;
  client.icon_order = next_icon_order_++;
  client.iconic = true;
  client.focused = false;
  client.zoomed = false;
  client.unmap_suppression = 1;
  client.mapped = false;
  set_wm_state(client, 3);
  XSetWindowBackground(display_, client.frame, theme_.background_pixel);
  XClearWindow(display_, client.frame);
  XUnmapWindow(display_, client.window);
  reflow_iconified_clients(client.desktop);
  if (focused_ == &client) {
    focused_ = nullptr;
  }
}

void WindowManager::deiconify_client(Client& client) {
  if (!client.iconic) {
    return;
  }
  client.iconic = false;
  client.unmap_suppression = 0;
  client.mapped = true;
  set_wm_state(client, 1);
  if (client.has_restore_geometry) {
    configure_client(client, client.restore_x, client.restore_y, client.restore_width, client.restore_height);
  }
  XSetWindowBackground(display_, client.frame, theme_.frame_inactive_pixel);
  XClearWindow(display_, client.frame);
  XMapWindow(display_, client.shadow);
  XMapWindow(display_, client.frame);
  XMapWindow(display_, client.window);
  std::array<Window, 2> restack{client.frame, client.shadow};
  XRestackWindows(display_, restack.data(), 2);
  ensure_desktop_below_clients();
  reflow_iconified_clients(client.desktop);
  focus_client(&client);
}

void WindowManager::show_menu(ui::MenuId menu, int x, int y) {
  if (!popup_menu_) {
    return;
  }
  active_menu_ = menu;
  if (menubar_) {
    menubar_->set_active(menu);
  }
  std::vector<ui::PopupMenuItem> items;
  switch (menu) {
    case ui::MenuId::Apple:
      items = {
          {"About chiux", "about"},
          {"Applications", "open-applications"},
          {"Preferences", "open-preferences"},
          {"Terminal", "launch-terminal"},
          {"File Browser", "launch-browser"},
          {"Desktop 1", "switch-desktop:0"},
          {"Desktop 2", "switch-desktop:1"},
          {"Desktop 3", "switch-desktop:2"},
          {"Desktop 4", "switch-desktop:3"},
      };
      break;
    case ui::MenuId::File:
      items = {
          {"Terminal", "launch-terminal"},
          {"Modern Terminal", "launch-modern-terminal"},
          {"File Manager", "launch-browser"},
          {"Execute", "open-execute"},
          {"Open Config", "open-config"},
          {"Save Desktop Layout", "save-layout"},
          {"Arrange Icons", "arrange-icons"},
          {"Quit", "quit"},
      };
      {
        std::unordered_set<std::string> seen;
        for (const auto& item : items) {
          seen.insert(normalize_menu_key(item.label) + "\x1f" + normalize_menu_key(item.action));
        }
        for (const auto& launcher : config_.launchers) {
          if (is_generic_launcher_label(launcher.label)) {
            continue;
          }
          const std::string action = std::string("spawn:") + launcher.command;
          const std::string key = normalize_menu_key(launcher.label) + "\x1f" + normalize_menu_key(action);
          if (!seen.insert(key).second) {
            continue;
          }
          items.push_back({launcher.label, action});
        }
      }
      break;
    case ui::MenuId::Edit:
      items = {
          {"Refresh Desktop", "refresh-desktop"},
          {"Desktop 1", "switch-desktop:0"},
          {"Desktop 2", "switch-desktop:1"},
          {"Desktop 3", "switch-desktop:2"},
          {"Desktop 4", "switch-desktop:3"},
      };
      break;
    case ui::MenuId::View:
      items = {
          {"Show Desktop", "show-desktop"},
          {"Next Desktop", "next-desktop"},
          {"Previous Desktop", "previous-desktop"},
          {"Raise Window", "raise-window"},
          {"Lower Window", "lower-window"},
      };
      break;
    case ui::MenuId::Window:
      items = {
          {"Iconify Window", "iconify-window"},
          {"Restore Iconified Windows", "restore-iconified-windows"},
          {"Close Window", "close-window"},
          {"Zoom Window", "zoom-window"},
          {"Send to Desktop 1", "move-window:0"},
          {"Send to Desktop 2", "move-window:1"},
          {"Send to Desktop 3", "move-window:2"},
          {"Send to Desktop 4", "move-window:3"},
      };
      break;
  }
  popup_menu_->show(x, y, std::move(items));
}

void WindowManager::hide_menu() {
  if (popup_menu_) {
    popup_menu_->hide();
  }
  active_menu_.reset();
  if (menubar_) {
    menubar_->set_active(std::nullopt);
  }
}

void WindowManager::handle_menu_action(const std::string& action) {
  if (action == "about") {
    open_about_window();
  } else if (action == "open-applications") {
    open_applications_window();
  } else if (action == "open-preferences") {
    open_preferences_window();
  } else if (action == "launch-terminal") {
    run_launcher_command(config_.terminal_command);
  } else if (action == "launch-modern-terminal") {
    run_launcher_command(resolve_modern_terminal_command());
  } else if (action == "launch-browser") {
    open_file_manager_window();
  } else if (action == "open-file-manager") {
    open_file_manager_window();
  } else if (action == "open-applications-window") {
    open_applications_window();
  } else if (action == "open-execute") {
    open_execute_window();
  } else if (action == "open-config") {
    run_launcher_command(wrap_terminal_command(config_.terminal_command, std::string("exec ${EDITOR:-vi} \"") + config_path_.string() + "\""));
  } else if (action == "quit") {
    running_ = false;
  } else if (action == "refresh-desktop") {
    sync_desktop_icons();
  } else if (action == "save-layout") {
    save_config();
  } else if (action == "arrange-icons") {
    if (desktop_) {
      desktop_->arrange_icons();
      save_config();
    }
  } else if (action == "show-desktop") {
    for (auto& [window, client] : clients_) {
      (void)window;
      if (client.desktop == current_desktop_) {
        set_client_desktop_visible(client, false);
      }
    }
    restack_managed_windows();
    if (focused_) {
      focused_ = nullptr;
    }
    if (menubar_) {
      menubar_->set_application_title(std::string("chiux - ") + desktop_names_[current_desktop_]);
    }
  } else if (action == "next-desktop") {
    cycle_desktop(1);
  } else if (action == "previous-desktop") {
    cycle_desktop(-1);
  } else if (action.rfind("switch-desktop:", 0) == 0) {
    switch_to_desktop(static_cast<unsigned int>(std::stoul(action.substr(15))));
  } else if (action == "raise-window") {
    if (focused_) {
      raise_managed_window(focused_->window);
    }
  } else if (action == "lower-window") {
    if (focused_) {
      lower_managed_window(focused_->window);
    }
  } else if (action == "iconify-window") {
    if (focused_) {
      iconify_client(*focused_);
    }
  } else if (action == "restore-iconified-windows") {
    for (auto& [window, client] : clients_) {
      (void)window;
      if (client.iconic && client.desktop == current_desktop_) {
        deiconify_client(client);
      }
    }
  } else if (action == "zoom-window") {
    if (focused_) {
      toggle_zoom_client(*focused_);
    }
  } else if (action.rfind("move-window:", 0) == 0) {
    if (focused_) {
      move_focused_to_desktop(static_cast<unsigned int>(std::stoul(action.substr(12))));
    }
  } else if (action == "close-window") {
    if (focused_) {
      if (client_supports_delete(focused_->window)) {
        XEvent message{};
        message.xclient.type = ClientMessage;
        message.xclient.window = focused_->window;
        message.xclient.message_type = wm_protocols_;
        message.xclient.format = 32;
        message.xclient.data.l[0] = static_cast<long>(wm_delete_window_);
        message.xclient.data.l[1] = CurrentTime;
        XSendEvent(display_, focused_->window, False, NoEventMask, &message);
      } else {
        XDestroyWindow(display_, focused_->window);
      }
    }
  } else if (action.rfind("spawn:", 0) == 0) {
    run_launcher_command(action.substr(6));
  }
}

void WindowManager::handle_desktop_click(int x, int y) {
  if (!desktop_) {
    return;
  }
  const auto hit = desktop_->hit_test(x, y);
  if (!hit) {
    desktop_->select_icon(std::nullopt);
    drag_icon_index_.reset();
    return;
  }
  desktop_->select_icon(hit);
  drag_icon_index_ = hit;
  drag_icon_moved_ = false;
  drag_icon_start_x_ = x;
  drag_icon_start_y_ = y;
  drag_icon_offset_x_ = x - desktop_->icons()[*hit].x;
  drag_icon_offset_y_ = y - desktop_->icons()[*hit].y;
  grab_pointer_for_drag(desktop_->window());
}

void WindowManager::handle_desktop_drag(int x, int y) {
  if (!desktop_ || !drag_icon_index_) {
    return;
  }
  if (!drag_icon_moved_) {
    const int dx = std::abs(x - drag_icon_start_x_);
    const int dy = std::abs(y - drag_icon_start_y_);
    if (dx < 4 && dy < 4) {
      return;
    }
    drag_icon_moved_ = true;
  }
  const int new_x = std::max(8, x - drag_icon_offset_x_);
  const int new_y = std::max(8, y - drag_icon_offset_y_);
  desktop_->move_icon(*drag_icon_index_, new_x, new_y);
}

void WindowManager::handle_desktop_release(const XButtonEvent& event) {
  (void)event;
  if (drag_icon_index_ && desktop_) {
    if (!drag_icon_moved_) {
      if (*drag_icon_index_ < desktop_->icons().size()) {
        run_launcher_command(desktop_->icons()[*drag_icon_index_].command);
      }
    } else {
      save_config();
    }
  }
  drag_icon_index_.reset();
  drag_icon_moved_ = false;
  ungrab_pointer_for_drag();
}

void WindowManager::handle_desktop_double_click(std::size_t index) {
  if (!desktop_ || index >= desktop_->icons().size()) {
    return;
  }
  run_launcher_command(desktop_->icons()[index].command);
}

void WindowManager::handle_map_request(const XMapRequestEvent& event) {
  Client* client = find_client(event.window);
  if (!client) {
    manage_window(event.window);
    client = find_client(event.window);
  }
  if (client) {
    if (client->desktop != current_desktop_) {
      switch_to_desktop(client->desktop);
    }
    if (client->iconic) {
      deiconify_client(*client);
    } else {
      client->mapped = true;
      XMapWindow(display_, client->frame);
      XMapWindow(display_, client->window);
      focus_client(client);
    }
    update_client_list();
  }
}

void WindowManager::handle_configure_request(const XConfigureRequestEvent& event) {
  Client* client = find_client(event.window);
  if (client) {
    const int x = (event.value_mask & CWX) ? event.x : client->x;
    const int y = (event.value_mask & CWY) ? clamp_client_y(event.y) : client->y;
    const unsigned int width = (event.value_mask & CWWidth) ? static_cast<unsigned int>(event.width) : client->width;
    const unsigned int height = (event.value_mask & CWHeight) ? static_cast<unsigned int>(event.height) : client->height;
    configure_client(*client, x, y, width, height);
  } else {
    XWindowChanges changes{};
    changes.x = event.x;
    changes.y = event.y;
    changes.width = event.width;
    changes.height = event.height;
    changes.border_width = event.border_width;
    changes.sibling = event.above;
    changes.stack_mode = event.detail;
    XConfigureWindow(display_, event.window, static_cast<unsigned int>(event.value_mask), &changes);
  }
}

void WindowManager::handle_unmap_notify(const XUnmapEvent& event) {
  Client* client = find_client(event.window);
  if (!client) {
    return;
  }
  if (client->unmap_suppression > 0) {
    --client->unmap_suppression;
    return;
  }
  if (client->iconic || client->desktop_hidden) {
    client->mapped = false;
    return;
  }
  client->mapped = false;
}

void WindowManager::handle_destroy_notify(const XDestroyWindowEvent& event) {
  unmanage_window(event.window, false);
  refresh_keybindings();
}

void WindowManager::handle_button_press(const XButtonEvent& event) {
  if (popup_menu_ && popup_menu_->visible() && event.window != popup_menu_->window()) {
    hide_menu();
  }

  if (popup_menu_ && popup_menu_->visible() && event.window == popup_menu_->window()) {
    popup_menu_->set_active_index(popup_menu_->hit_test(event.x, event.y));
    return;
  }

  if (menubar_ && event.window == menubar_->window()) {
    const auto menu = menubar_->hit_test(event.x, event.y);
    if (menu) {
      show_menu(*menu, event.x_root, event.y_root + 18);
    }
    return;
  }

  if (desktop_ && event.window == desktop_->window()) {
    handle_desktop_click(event.x, event.y);
    return;
  }

  Client* client = find_client(event.window);
  if (!client) {
    client = find_client(event.subwindow);
  }
  if (!client) {
    return;
  }

  if (event.window == client->window && client->internal_kind != InternalKind::Unset) {
    handle_internal_button_press(*client, event);
    return;
  }

  if (client->iconic && event.window == client->frame && event.button == Button1) {
    deiconify_client(*client);
    return;
  }

  focus_client(client);
  const unsigned int frame_width = client->width + 2 * theme_.border_width + theme_.grow_box_size;
  const unsigned int frame_height = client->height + theme_.title_height + theme_.border_width + theme_.grow_box_size;
  const bool in_title = event.y < static_cast<int>(theme_.title_height);
  const bool in_grow_box = event.x >= static_cast<int>(frame_width - theme_.grow_box_size) &&
                           event.y >= static_cast<int>(frame_height - theme_.grow_box_size);
  if (event.button == Button1 && event.window == client->frame) {
    if (client->iconic) {
      deiconify_client(*client);
      return;
    }
    if (in_title) {
      if (event.x < static_cast<int>(theme_.button_size)) {
        if (client_supports_delete(client->window)) {
          XEvent message{};
          message.xclient.type = ClientMessage;
          message.xclient.window = client->window;
          message.xclient.message_type = wm_protocols_;
          message.xclient.format = 32;
          message.xclient.data.l[0] = static_cast<long>(wm_delete_window_);
          message.xclient.data.l[1] = CurrentTime;
          XSendEvent(display_, client->window, False, NoEventMask, &message);
        } else {
          XDestroyWindow(display_, client->window);
        }
        drag_mode_ = DragMode::Idle;
        drag_client_ = nullptr;
        return;
      } else if (event.x > static_cast<int>(frame_width - theme_.button_size - 4)) {
        toggle_zoom_client(*client);
        return;
      } else {
        drag_mode_ = DragMode::Move;
      }
    } else if (in_grow_box) {
      drag_mode_ = DragMode::Resize;
    }
  } else if (event.button == Button3 && event.window == client->frame) {
    drag_mode_ = DragMode::Resize;
  }
  drag_client_ = client;
  drag_start_x_ = event.x_root;
  drag_start_y_ = event.y_root;
  drag_origin_x_ = client->x;
  drag_origin_y_ = client->y;
  drag_origin_w_ = client->width;
  drag_origin_h_ = client->height;
  if (drag_mode_ != DragMode::Idle) {
    grab_pointer_for_drag(client->frame);
  }

}

void WindowManager::handle_motion_notify(const XMotionEvent& event) {
  if (popup_menu_ && popup_menu_->visible() && event.window == popup_menu_->window()) {
    popup_menu_->set_active_index(popup_menu_->hit_test(event.x, event.y));
    return;
  }
  if (drag_icon_index_) {
    handle_desktop_drag(event.x, event.y);
    return;
  }
  if (Client* client = find_client(event.window)) {
    if (event.window == client->window && client->internal_kind != InternalKind::Unset) {
      handle_internal_motion_notify(*client, event);
      return;
    }
  }
  if (!drag_client_ || drag_mode_ == DragMode::Idle) {
    return;
  }
  const int dx = event.x_root - drag_start_x_;
  const int dy = event.y_root - drag_start_y_;
  if (drag_mode_ == DragMode::Move) {
    const int target_x = drag_origin_x_ + dx;
    const int target_y = clamp_client_y(drag_origin_y_ + dy);
    move_client(*drag_client_, target_x - drag_client_->x, target_y - drag_client_->y);
  } else if (drag_mode_ == DragMode::Resize) {
    const int width = std::max(static_cast<int>(drag_client_->min_width), static_cast<int>(drag_origin_w_) + dx);
    const int height = std::max(static_cast<int>(drag_client_->min_height), static_cast<int>(drag_origin_h_) + dy);
    resize_client(*drag_client_, width, height);
  }
}

void WindowManager::handle_button_release(const XButtonEvent& event) {
  if (popup_menu_ && popup_menu_->visible() && event.window == popup_menu_->window()) {
    const auto index = popup_menu_->hit_test(event.x, event.y);
    if (index && *index < popup_menu_->items().size()) {
      const std::string action = popup_menu_->items()[*index].action;
      hide_menu();
      handle_menu_action(action);
    } else {
      hide_menu();
    }
    return;
  }
  if (popup_menu_ && popup_menu_->visible()) {
    hide_menu();
    return;
  }

  drag_mode_ = DragMode::Idle;
  drag_client_ = nullptr;
  if (Client* client = find_client(event.window)) {
    if (event.window == client->window && client->internal_kind != InternalKind::Unset) {
      handle_internal_button_release(*client, event);
      return;
    }
  }
  if (drag_icon_index_) {
    handle_desktop_release(event);
    return;
  }
  ungrab_pointer_for_drag();
}

void WindowManager::handle_key_press(const XKeyEvent& event) {
  const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
  if ((event.state & Mod1Mask) != 0 && sym == XK_Left) {
    cycle_desktop(-1);
    return;
  }
  if ((event.state & Mod1Mask) != 0 && sym == XK_Right) {
    cycle_desktop(1);
    return;
  }
  if (focused_ && focused_->internal_kind != InternalKind::Unset) {
    handle_internal_key_press(*focused_, event);
    return;
  }
  if (sym == XK_Escape) {
    running_ = false;
  }
}

void WindowManager::handle_enter_notify(const XCrossingEvent& event) {
  if (event.mode != NotifyNormal || event.detail == NotifyInferior) {
    return;
  }
  Client* client = find_client(event.window);
  if (!client) {
    client = find_client(event.subwindow);
  }
  if (!client || client->iconic || client->desktop_hidden) {
    return;
  }
  focus_client(client);
}

void WindowManager::handle_focus_in(const XFocusChangeEvent& event) {
  if (event.mode != NotifyNormal) {
    return;
  }
  Client* client = find_client(event.window);
  if (!client || client->iconic || client->desktop_hidden) {
    return;
  }
  if (focused_ && focused_ != client) {
    set_client_active(*focused_, false);
  }
  focused_ = client;
  set_client_active(*client, true);
  if (menubar_) {
    menubar_->set_application_title(client->title);
  }
}

void WindowManager::handle_client_message(const XClientMessageEvent& event) {
  if (event.message_type == wm_protocols_) {
    const Atom protocol = static_cast<Atom>(event.data.l[0]);
    if (protocol == wm_delete_window_) {
      XDestroyWindow(display_, event.window);
    } else if (protocol == wm_take_focus_) {
      XSetInputFocus(display_, event.window, RevertToPointerRoot, CurrentTime);
    }
  }
}

void WindowManager::handle_property_notify(const XPropertyEvent& event) {
  Client* client = find_client(event.window);
  if (!client) {
    return;
  }
  if (event.atom == XA_WM_NAME || event.atom == net_wm_name_) {
    fetch_title(*client);
    draw_frame(*client);
    if (focused_ == client && menubar_) {
      menubar_->set_application_title(client->title);
    }
  }
}

void WindowManager::handle_expose(const XExposeEvent& event) {
  if (menubar_ && event.window == menubar_->window()) {
    menubar_->draw();
  } else if (desktop_ && event.window == desktop_->window()) {
    desktop_->draw();
  } else if (popup_menu_ && event.window == popup_menu_->window()) {
    popup_menu_->draw();
  } else if (Client* client = find_client(event.window)) {
    if (event.window == client->window && client->internal_kind != InternalKind::Unset) {
      present_internal_content(*client);
    } else {
      draw_frame(*client);
    }
  }
}

void WindowManager::handle_selection_clear(const XSelectionClearEvent&) {
  running_ = false;
}

void WindowManager::reap_children() {
  util::reap_children();
}

void WindowManager::handle_idle() {
  reap_children();
}

void WindowManager::run() {
  const int xfd = ConnectionNumber(display_);
  while (running_) {
    while (XPending(display_) > 0) {
      XEvent event{};
      XNextEvent(display_, &event);
      switch (event.type) {
        case MapRequest: handle_map_request(event.xmaprequest); break;
        case ConfigureRequest: handle_configure_request(event.xconfigurerequest); break;
        case UnmapNotify: handle_unmap_notify(event.xunmap); break;
        case DestroyNotify: handle_destroy_notify(event.xdestroywindow); break;
        case ButtonPress: handle_button_press(event.xbutton); break;
        case MotionNotify: handle_motion_notify(event.xmotion); break;
        case ButtonRelease: handle_button_release(event.xbutton); break;
        case KeyPress: handle_key_press(event.xkey); break;
        case EnterNotify: handle_enter_notify(event.xcrossing); break;
        case FocusIn: handle_focus_in(event.xfocus); break;
        case ClientMessage: handle_client_message(event.xclient); break;
        case PropertyNotify: handle_property_notify(event.xproperty); break;
        case Expose: handle_expose(event.xexpose); break;
        case SelectionClear: handle_selection_clear(event.xselectionclear); break;
        default: break;
      }
    }
    handle_idle();
    if (!running_) {
      break;
    }
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(xfd, &readfds);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    const int result = select(xfd + 1, &readfds, nullptr, nullptr, &timeout);
    if (result < 0 && errno != EINTR) {
      throw std::runtime_error("select failed in event loop");
    }
  }
}

}
