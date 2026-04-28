#include "util/log.hpp"
#include "config/config.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <deque>
#include <filesystem>
#include <cwchar>
#include <optional>
#include <pty.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace chiux::te {

namespace {

struct Cell {
  std::string text;
  unsigned long fg = 0;
  unsigned long bg = 0;
  bool bold = false;
  bool inverse = false;
  bool continuation = false;
};

enum class ParseState {
  Ground,
  Escape,
  Charset,
  Csi,
  Osc,
};

struct CompletionItem {
  std::string label;
  std::string insert_text;
  std::string source;
};

std::string join_args(char* const* argv, int start, int argc) {
  std::string result;
  for (int i = start; i < argc; ++i) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += argv[i];
  }
  return result;
}

XFontStruct* load_font(Display* display, unsigned int* ascent, unsigned int* descent, unsigned int* cell_w, unsigned int* cell_h) {
  static XFontStruct* font = nullptr;
  static bool attempted = false;
  if (attempted) {
    if (font && ascent && descent && cell_w && cell_h) {
      *ascent = static_cast<unsigned int>(std::max(1, font->ascent));
      *descent = static_cast<unsigned int>(std::max(1, font->descent));
      *cell_w = static_cast<unsigned int>(std::max(1, static_cast<int>(font->max_bounds.width)));
      *cell_h = static_cast<unsigned int>(std::max(1, font->ascent + font->descent));
    }
    return font;
  }
  attempted = true;
  static const char* candidates[] = {
      "-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso10646-1",
      "-misc-fixed-medium-r-normal--14-130-75-75-c-80-iso10646-1",
      "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso10646-1",
      "fixed",
  };
  for (const char* candidate : candidates) {
    font = XLoadQueryFont(display, candidate);
    if (font) {
      break;
    }
  }
  if (font && ascent && descent && cell_w && cell_h) {
    *ascent = static_cast<unsigned int>(std::max(1, font->ascent));
    *descent = static_cast<unsigned int>(std::max(1, font->descent));
    *cell_w = static_cast<unsigned int>(std::max(1, static_cast<int>(font->max_bounds.width)));
    *cell_h = static_cast<unsigned int>(std::max(1, font->ascent + font->descent));
  }
  return font;
}

unsigned int utf8_expected_length(unsigned char lead) {
  if ((lead & 0x80u) == 0u) {
    return 1;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    return 2;
  }
  if ((lead & 0xF0u) == 0xE0u) {
    return 3;
  }
  if ((lead & 0xF8u) == 0xF0u) {
    return 4;
  }
  return 0;
}

bool utf8_decode(const std::string& bytes, char32_t& codepoint) {
  if (bytes.empty()) {
    return false;
  }
  const unsigned char lead = static_cast<unsigned char>(bytes[0]);
  const unsigned int expected = utf8_expected_length(lead);
  if (expected == 0 || bytes.size() != expected) {
    return false;
  }
  auto is_continuation = [](unsigned char ch) { return (ch & 0xC0u) == 0x80u; };
  for (std::size_t i = 1; i < bytes.size(); ++i) {
    if (!is_continuation(static_cast<unsigned char>(bytes[i]))) {
      return false;
    }
  }
  if (expected == 1) {
    codepoint = lead;
    return true;
  }
  if (expected == 2) {
    const char32_t cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(bytes[1]) & 0x3Fu);
    if (cp < 0x80) {
      return false;
    }
    codepoint = cp;
    return true;
  }
  if (expected == 3) {
    const char32_t cp = ((lead & 0x0Fu) << 12) |
                        ((static_cast<unsigned char>(bytes[1]) & 0x3Fu) << 6) |
                        (static_cast<unsigned char>(bytes[2]) & 0x3Fu);
    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    codepoint = cp;
    return true;
  }
  const char32_t cp = ((lead & 0x07u) << 18) |
                      ((static_cast<unsigned char>(bytes[1]) & 0x3Fu) << 12) |
                      ((static_cast<unsigned char>(bytes[2]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(bytes[3]) & 0x3Fu);
  if (cp < 0x10000 || cp > 0x10FFFF) {
    return false;
  }
  codepoint = cp;
  return true;
}

std::string utf8_from_codepoint(char32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7Fu) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else if (codepoint <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  }
  return out;
}

unsigned int utf8_display_width(char32_t codepoint) {
  const int width = wcwidth(static_cast<wchar_t>(codepoint));
  if (width < 0) {
    return 1;
  }
  return static_cast<unsigned int>(width);
}

struct Rgb {
  unsigned int r = 0;
  unsigned int g = 0;
  unsigned int b = 0;
};

struct FeelTheme {
  std::string name;
  Rgb background;
  Rgb foreground;
  Rgb cursor;
  Rgb bezel_light;
  Rgb bezel_shadow;
  Rgb bezel_edge;
  Rgb chrome;
  Rgb chrome_text;
  Rgb chrome_shadow;
  Rgb popup_fill;
  Rgb popup_text;
  Rgb popup_selected;
  Rgb popup_selected_text;
};

constexpr unsigned int kFeelFamilyCount = 9;
constexpr unsigned int kFeelVariantsPerFamily = 12;
constexpr std::size_t kFeelThemeCount = config::kTerminalFeelThemeCount;
constexpr unsigned int kChromeHeight = 22;

Rgb clamp_rgb(int r, int g, int b) {
  return {
      static_cast<unsigned int>(std::clamp(r, 0, 255)),
      static_cast<unsigned int>(std::clamp(g, 0, 255)),
      static_cast<unsigned int>(std::clamp(b, 0, 255)),
  };
}

Rgb adjust_rgb(const Rgb& rgb, int delta) {
  return clamp_rgb(static_cast<int>(rgb.r) + delta,
                   static_cast<int>(rgb.g) + delta,
                   static_cast<int>(rgb.b) + delta);
}

Rgb mix_rgb(const Rgb& lhs, const Rgb& rhs, double ratio) {
  const double clamped = std::clamp(ratio, 0.0, 1.0);
  return clamp_rgb(
      static_cast<int>(std::lround(lhs.r * (1.0 - clamped) + rhs.r * clamped)),
      static_cast<int>(std::lround(lhs.g * (1.0 - clamped) + rhs.g * clamped)),
      static_cast<int>(std::lround(lhs.b * (1.0 - clamped) + rhs.b * clamped)));
}

unsigned int rgb_luma(const Rgb& rgb) {
  return static_cast<unsigned int>((rgb.r * 299u + rgb.g * 587u + rgb.b * 114u) / 1000u);
}

Rgb contrast_rgb(const Rgb& rgb) {
  return rgb_luma(rgb) >= 145u ? Rgb{32, 32, 32} : Rgb{240, 240, 240};
}

std::string feel_family_name(unsigned int family) {
  static const std::array<const char*, kFeelFamilyCount> families = {
      "Classic Gray",
      "Slate",
      "Parchment",
      "Moss",
      "Harbor",
      "Graphite",
      "Rosewood",
      "Sandstone",
      "Indigo",
  };
  return families[family % families.size()];
}

std::string feel_variant_name(unsigned int variant) {
  static const std::array<const char*, kFeelVariantsPerFamily> variants = {
      "Pale",
      "Light",
      "Soft",
      "Classic",
      "Muted",
      "Mid",
      "Deep",
      "Shade",
      "Night",
      "Ink",
      "Dusk",
      "Noir",
  };
  return variants[variant % variants.size()];
}

std::string theme_name_for_index(unsigned int index) {
  const unsigned int family = index / kFeelVariantsPerFamily;
  const unsigned int variant = index % kFeelVariantsPerFamily;
  return feel_family_name(family) + " " + feel_variant_name(variant);
}

std::string feel_family_label(unsigned int index) {
  const unsigned int family = index / kFeelVariantsPerFamily;
  return feel_family_name(family);
}

FeelTheme feel_theme_for_index(unsigned int index) {
  static const std::array<Rgb, kFeelFamilyCount> family_backgrounds = {
      Rgb{192, 192, 192},
      Rgb{184, 190, 198},
      Rgb{224, 218, 204},
      Rgb{186, 196, 176},
      Rgb{184, 196, 210},
      Rgb{170, 172, 180},
      Rgb{210, 196, 196},
      Rgb{224, 214, 188},
      Rgb{194, 198, 216},
  };
  static const std::array<Rgb, kFeelFamilyCount> family_accents = {
      Rgb{108, 108, 108},
      Rgb{84, 104, 132},
      Rgb{156, 136, 108},
      Rgb{116, 136, 88},
      Rgb{84, 120, 156},
      Rgb{104, 110, 120},
      Rgb{144, 96, 104},
      Rgb{164, 140, 100},
      Rgb{96, 104, 144},
  };
  static const std::array<int, kFeelVariantsPerFamily> variants = {
      -30, -24, -18, -12, -8, -4, 4, 8, 12, 18, 24, 30,
  };

  const unsigned int family = (index / kFeelVariantsPerFamily) % family_backgrounds.size();
  const unsigned int variant = index % kFeelVariantsPerFamily;
  const Rgb base_bg = family_backgrounds[family];
  const Rgb accent = family_accents[family];
  const int delta = variants[variant] + static_cast<int>(family) * 2 - 8;
  const Rgb bg = adjust_rgb(base_bg, delta);
  const Rgb fg = contrast_rgb(bg);
  const Rgb chrome = mix_rgb(bg, adjust_rgb(bg, rgb_luma(bg) > 140u ? -8 : 8), 0.55);
  const Rgb selected = mix_rgb(bg, accent, 0.58);
  return {
      theme_name_for_index(index),
      bg,
      fg,
      accent,
      adjust_rgb(bg, 28),
      adjust_rgb(bg, -36),
      adjust_rgb(bg, -18),
      chrome,
      fg,
      adjust_rgb(bg, -62),
      adjust_rgb(bg, 20),
      fg,
      selected,
      contrast_rgb(selected),
  };
}

unsigned int clamp_feel_index(unsigned int index) {
  return std::min<unsigned int>(index, static_cast<unsigned int>(kFeelThemeCount - 1));
}

std::string special_graphics_glyph(unsigned char ch) {
  switch (ch) {
    case 'j': return utf8_from_codepoint(0x2518);
    case 'k': return utf8_from_codepoint(0x2510);
    case 'l': return utf8_from_codepoint(0x250c);
    case 'm': return utf8_from_codepoint(0x2514);
    case 'n': return utf8_from_codepoint(0x253c);
    case 'q': return utf8_from_codepoint(0x2500);
    case 't': return utf8_from_codepoint(0x251c);
    case 'u': return utf8_from_codepoint(0x2524);
    case 'v': return utf8_from_codepoint(0x2534);
    case 'w': return utf8_from_codepoint(0x252c);
    case 'x': return utf8_from_codepoint(0x2502);
    case 'o': return utf8_from_codepoint(0x25cf);
    case '~': return utf8_from_codepoint(0x25c6);
    case '`': return utf8_from_codepoint(0x25c6);
    case 'a': return utf8_from_codepoint(0x2592);
    case 'f': return utf8_from_codepoint(0x00b0);
    case 'g': return utf8_from_codepoint(0x00b1);
    case 'h': return utf8_from_codepoint(0x2424);
    case 'i': return utf8_from_codepoint(0x00b6);
    case '0': return utf8_from_codepoint(0x2588);
    case '1': return utf8_from_codepoint(0x2589);
    case '2': return utf8_from_codepoint(0x258a);
    case '3': return utf8_from_codepoint(0x258b);
    case '4': return utf8_from_codepoint(0x258c);
    case '5': return utf8_from_codepoint(0x258d);
    case '6': return utf8_from_codepoint(0x258e);
    case '7': return utf8_from_codepoint(0x258f);
    case '8': return utf8_from_codepoint(0x25ae);
    case '9': return utf8_from_codepoint(0x25af);
    default: return {};
  }
}

unsigned long rgb_to_pixel(const Rgb& rgb, unsigned long red_mask, unsigned long green_mask, unsigned long blue_mask) {
  auto scale = [](unsigned int value, unsigned long mask) -> unsigned long {
    if (mask == 0) {
      return 0;
    }
    unsigned int shift = 0;
    while (((mask >> shift) & 1u) == 0u && shift < sizeof(unsigned long) * 8u) {
      ++shift;
    }
    const unsigned long scaled_mask = mask >> shift;
    const unsigned long max_value = scaled_mask;
    return ((static_cast<unsigned long>(value) * max_value + 127u) / 255u) << shift;
  };
  return scale(rgb.r, red_mask) | scale(rgb.g, green_mask) | scale(rgb.b, blue_mask);
}

Rgb xterm_256_rgb(unsigned int index) {
  if (index < 16) {
    static const Rgb table[] = {
        {0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0},
        {0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
        {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
        {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    };
    return table[index];
  }
  if (index >= 232) {
    const unsigned int gray = 8u + (index - 232u) * 10u;
    return {gray, gray, gray};
  }
  index -= 16u;
  const unsigned int r = index / 36u;
  const unsigned int g = (index % 36u) / 6u;
  const unsigned int b = index % 6u;
  auto level = [](unsigned int component) {
    static const unsigned int steps[] = {0, 95, 135, 175, 215, 255};
    return steps[component % 6u];
  };
  return {level(r), level(g), level(b)};
}

unsigned long xterm_256_pixel(Display* display, int screen, unsigned int index) {
  XVisualInfo vi{};
  vi.red_mask = DefaultVisual(display, screen)->red_mask;
  vi.green_mask = DefaultVisual(display, screen)->green_mask;
  vi.blue_mask = DefaultVisual(display, screen)->blue_mask;
  const Rgb rgb = xterm_256_rgb(index % 256u);
  return rgb_to_pixel(rgb, vi.red_mask, vi.green_mask, vi.blue_mask);
}

  unsigned long rgb_pixel(Display* display, int screen, unsigned int r, unsigned int g, unsigned int b) {
    XVisualInfo vi{};
    vi.red_mask = DefaultVisual(display, screen)->red_mask;
    vi.green_mask = DefaultVisual(display, screen)->green_mask;
    vi.blue_mask = DefaultVisual(display, screen)->blue_mask;
    return rgb_to_pixel({r, g, b}, vi.red_mask, vi.green_mask, vi.blue_mask);
  }

std::string trim_copy(const std::string& text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(start, end - start);
}

std::string home_directory() {
  if (const char* home = std::getenv("HOME"); home && *home) {
    return home;
  }
  return {};
}

std::string expand_tilde(const std::string& path) {
  if (path.empty() || path[0] != '~') {
    return path;
  }
  const std::string home = home_directory();
  if (home.empty()) {
    return path;
  }
  if (path.size() == 1) {
    return home;
  }
  if (path[1] == '/') {
    return home + path.substr(1);
  }
  return path;
}

bool is_path_like(const std::string& token) {
  return token.find('/') != std::string::npos || (!token.empty() && (token[0] == '.' || token[0] == '~'));
}

std::string shell_escape_text(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 2);
  for (const char ch : text) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '/' || ch == '.' || ch == '-' || ch == '_' || ch == '~' || ch == '+') {
      out.push_back(ch);
    } else {
      out.push_back('\\');
      out.push_back(ch);
    }
  }
  return out;
}

std::string path_directory_part(const std::string& token) {
  if (token.empty()) {
    return ".";
  }
  const auto pos = token.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  if (pos == 0) {
    return "/";
  }
  return token.substr(0, pos);
}

std::string path_filename_prefix(const std::string& token) {
  const auto pos = token.find_last_of('/');
  if (pos == std::string::npos) {
    return token;
  }
  return token.substr(pos + 1);
}

bool is_executable_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return false;
  }
  return access(path.c_str(), X_OK) == 0;
}

std::vector<CompletionItem> collect_path_completions(const std::string& token, std::size_t limit) {
  std::vector<CompletionItem> items;
  const std::string expanded_token = expand_tilde(token);
  const std::filesystem::path dir = std::filesystem::path(expand_tilde(path_directory_part(expanded_token)));
  const std::string prefix = path_filename_prefix(expanded_token);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
    return items;
  }

  std::unordered_set<std::string> seen;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (!prefix.empty() && name.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string insert_text = name;
    if (entry.is_directory(ec)) {
      insert_text.push_back('/');
    }
    const std::string full_text = (dir == std::filesystem::path(".") ? std::filesystem::path(name) : dir / insert_text).string();
    if (!seen.insert(full_text).second) {
      continue;
    }
    items.push_back({full_text, shell_escape_text(full_text), "path"});
    if (items.size() >= limit) {
      break;
    }
  }
  std::sort(items.begin(), items.end(), [](const CompletionItem& lhs, const CompletionItem& rhs) {
    return lhs.label < rhs.label;
  });
  return items;
}

std::vector<CompletionItem> collect_command_completions(const std::string& token, std::size_t limit) {
  std::vector<CompletionItem> items;
  if (token.empty() || is_path_like(token)) {
    return items;
  }

  std::unordered_set<std::string> seen;
  const char* path_env = std::getenv("PATH");
  if (!path_env) {
    return items;
  }
  std::stringstream stream(path_env);
  std::string directory;
  while (std::getline(stream, directory, ':')) {
    if (directory.empty()) {
      directory = ".";
    }
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name.rfind(token, 0) != 0) {
        continue;
      }
      if (!is_executable_file(entry.path())) {
        continue;
      }
      if (!seen.insert(name).second) {
        continue;
      }
      items.push_back({name, name, "cmd"});
      if (items.size() >= limit) {
        break;
      }
    }
    if (items.size() >= limit) {
      break;
    }
  }
  std::sort(items.begin(), items.end(), [](const CompletionItem& lhs, const CompletionItem& rhs) {
    if (lhs.label.size() != rhs.label.size()) {
      return lhs.label.size() < rhs.label.size();
    }
    return lhs.label < rhs.label;
  });
  return items;
}

}  // namespace

class TerminalApp {
public:
  TerminalApp(int argc, char** argv) {
    parse_args(argc, argv);
    open_display();
    load_configuration();
    load_resources();
    create_window();
    spawn_shell();
    redraw();
  }

  ~TerminalApp() {
    terminate_child();
    if (master_fd_ >= 0) {
      close(master_fd_);
    }
    if (backing_) {
      XFreePixmap(display_, backing_);
    }
    if (font_) {
      XFreeFont(display_, font_);
    }
    if (gc_) {
      XFreeGC(display_, gc_);
    }
    if (window_) {
      XDestroyWindow(display_, window_);
    }
    if (display_) {
      XCloseDisplay(display_);
    }
  }

  int run() {
    const int xfd = ConnectionNumber(display_);
    while (running_) {
      if (reap_child_if_done()) {
        break;
      }

      while (XPending(display_) > 0) {
        XEvent event{};
        XNextEvent(display_, &event);
        switch (event.type) {
          case Expose: handle_expose(event.xexpose); break;
          case ConfigureNotify: handle_configure(event.xconfigure); break;
          case KeyPress: handle_key_press(event.xkey); break;
          case ButtonPress: handle_button_press(event.xbutton); break;
          case ButtonRelease: handle_button_release(event.xbutton); break;
          case MotionNotify: handle_motion_notify(event.xmotion); break;
          case FocusIn: handle_focus_in(); break;
          case FocusOut: handle_focus_out(); break;
          case SelectionRequest: handle_selection_request(event.xselectionrequest); break;
          case SelectionClear: handle_selection_clear(event.xselectionclear); break;
          case SelectionNotify: handle_selection_notify(event.xselection); break;
          case ClientMessage: handle_client_message(event.xclient); break;
          case DestroyNotify: running_ = false; break;
          default: break;
        }
      }

      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(xfd, &readfds);
      FD_SET(master_fd_, &readfds);
      const int maxfd = std::max(xfd, master_fd_);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 150000;
      const int ready = select(maxfd + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(std::string("chiux-te select failed: ") + std::strerror(errno));
      }
      if (ready > 0) {
        if (FD_ISSET(master_fd_, &readfds)) {
          read_pty();
        }
        if (FD_ISSET(xfd, &readfds)) {
          while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);
            switch (event.type) {
              case Expose: handle_expose(event.xexpose); break;
              case ConfigureNotify: handle_configure(event.xconfigure); break;
              case KeyPress: handle_key_press(event.xkey); break;
              case ButtonPress: handle_button_press(event.xbutton); break;
              case ButtonRelease: handle_button_release(event.xbutton); break;
              case MotionNotify: handle_motion_notify(event.xmotion); break;
              case FocusIn: handle_focus_in(); break;
              case FocusOut: handle_focus_out(); break;
              case SelectionRequest: handle_selection_request(event.xselectionrequest); break;
              case SelectionClear: handle_selection_clear(event.xselectionclear); break;
              case SelectionNotify: handle_selection_notify(event.xselection); break;
              case ClientMessage: handle_client_message(event.xclient); break;
              case DestroyNotify: running_ = false; break;
              default: break;
            }
          }
        }
      }
      tick_cursor_blink();
    }
    return 0;
  }

private:
  void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "-e" || arg == "--execute") {
        exec_command_ = join_args(argv, i + 1, argc);
        break;
      }
      if (arg == "-h" || arg == "--help") {
        throw std::runtime_error("usage: chiux-te [-e command]");
      }
    }
  }

  void open_display() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      throw std::runtime_error("failed to open X display");
    }
    screen_ = DefaultScreen(display_);
  }

  void load_configuration() {
    config_path_ = chiux::config::default_config_path();
    config_ = chiux::config::Config::load_or_default(config_path_);
    feel_index_ = clamp_feel_index(config_.terminal_feel);
    feel_theme_ = feel_theme_for_index(feel_index_);
    theme_name_ = feel_theme_.name;
  }

  void apply_theme_to_pixels(const FeelTheme& theme) {
    background_pixel_ = rgb_pixel(display_, screen_, theme.background.r, theme.background.g, theme.background.b);
    foreground_pixel_ = rgb_pixel(display_, screen_, theme.foreground.r, theme.foreground.g, theme.foreground.b);
    cursor_pixel_ = rgb_pixel(display_, screen_, theme.cursor.r, theme.cursor.g, theme.cursor.b);
    bezel_light_pixel_ = rgb_pixel(display_, screen_, theme.bezel_light.r, theme.bezel_light.g, theme.bezel_light.b);
    bezel_shadow_pixel_ = rgb_pixel(display_, screen_, theme.bezel_shadow.r, theme.bezel_shadow.g, theme.bezel_shadow.b);
    bezel_edge_pixel_ = rgb_pixel(display_, screen_, theme.bezel_edge.r, theme.bezel_edge.g, theme.bezel_edge.b);
    chrome_pixel_ = rgb_pixel(display_, screen_, theme.chrome.r, theme.chrome.g, theme.chrome.b);
    chrome_text_pixel_ = rgb_pixel(display_, screen_, theme.chrome_text.r, theme.chrome_text.g, theme.chrome_text.b);
    chrome_shadow_pixel_ = rgb_pixel(display_, screen_, theme.chrome_shadow.r, theme.chrome_shadow.g, theme.chrome_shadow.b);
    popup_fill_pixel_ = rgb_pixel(display_, screen_, theme.popup_fill.r, theme.popup_fill.g, theme.popup_fill.b);
    popup_text_pixel_ = rgb_pixel(display_, screen_, theme.popup_text.r, theme.popup_text.g, theme.popup_text.b);
    popup_selected_pixel_ = rgb_pixel(display_, screen_, theme.popup_selected.r, theme.popup_selected.g, theme.popup_selected.b);
    popup_selected_text_pixel_ = rgb_pixel(display_, screen_, theme.popup_selected_text.r, theme.popup_selected_text.g, theme.popup_selected_text.b);
    current_fg_pixel_ = foreground_pixel_;
    current_bg_pixel_ = background_pixel_;
  }

  void load_resources() {
    font_ = load_font(display_, &ascent_, &descent_, &cell_w_, &cell_h_);
    if (!font_) {
      cell_w_ = 8;
      cell_h_ = 16;
      ascent_ = 12;
      descent_ = 4;
    } else {
      if (cell_w_ > 8 || cell_h_ > 16 || ascent_ > 12) {
        chiux::log::warn("chiux-te font metrics too large; forcing fixed cell size");
        cell_w_ = 8;
        cell_h_ = 16;
        ascent_ = 12;
        descent_ = 4;
      }
    }

    palette_[0] = rgb_pixel(display_, screen_, 32, 32, 32);
    palette_[1] = rgb_pixel(display_, screen_, 138, 58, 58);
    palette_[2] = rgb_pixel(display_, screen_, 58, 106, 58);
    palette_[3] = rgb_pixel(display_, screen_, 138, 99, 58);
    palette_[4] = rgb_pixel(display_, screen_, 58, 79, 138);
    palette_[5] = rgb_pixel(display_, screen_, 106, 74, 106);
    palette_[6] = rgb_pixel(display_, screen_, 58, 106, 106);
    palette_[7] = rgb_pixel(display_, screen_, 240, 240, 240);
    palette_[8] = rgb_pixel(display_, screen_, 118, 118, 118);
    palette_[9] = rgb_pixel(display_, screen_, 200, 108, 108);
    palette_[10] = rgb_pixel(display_, screen_, 108, 176, 108);
    palette_[11] = rgb_pixel(display_, screen_, 208, 176, 108);
    palette_[12] = rgb_pixel(display_, screen_, 108, 134, 208);
    palette_[13] = rgb_pixel(display_, screen_, 176, 122, 176);
    palette_[14] = rgb_pixel(display_, screen_, 108, 176, 176);
    palette_[15] = rgb_pixel(display_, screen_, 255, 255, 255);
    apply_theme_to_pixels(feel_theme_);
    for (unsigned int i = 0; i < 256; ++i) {
      color_table_[i] = xterm_256_pixel(display_, screen_, i);
    }
  }

  void create_window() {
    const unsigned int width = 800;
    const unsigned int height = 600;
    XSetWindowAttributes attrs{};
    attrs.background_pixel = background_pixel_;
    attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | FocusChangeMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask;
    window_ = XCreateWindow(display_, RootWindow(display_, screen_), 0, 0, width, height, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWEventMask, &attrs);
    XStoreName(display_, window_, "chiux-te");
    XClassHint class_hint{};
    class_hint.res_name = const_cast<char*>("chiux-te");
    class_hint.res_class = const_cast<char*>("ChiuxTe");
    XSetClassHint(display_, window_, &class_hint);

    wm_protocols_ = XInternAtom(display_, "WM_PROTOCOLS", False);
    wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    clipboard_atom_ = XInternAtom(display_, "CLIPBOARD", False);
    utf8_string_atom_ = XInternAtom(display_, "UTF8_STRING", False);
    targets_atom_ = XInternAtom(display_, "TARGETS", False);
    text_atom_ = XInternAtom(display_, "TEXT", False);
    string_atom_ = XA_STRING;
    paste_property_ = XInternAtom(display_, "_CHIUX_PASTE", False);
    XSetWMProtocols(display_, window_, &wm_delete_window_, 1);

    gc_ = XCreateGC(display_, window_, 0, nullptr);
    if (font_) {
      XSetFont(display_, gc_, font_->fid);
    }
    XSetForeground(display_, gc_, foreground_pixel_);
    XSetBackground(display_, gc_, background_pixel_);
    XMapWindow(display_, window_);
    resize(width, height);
  }

  void resize(unsigned int width, unsigned int height) {
    const unsigned int previous_cols = cols_;
    const unsigned int previous_rows = rows_;
    width_ = width;
    height_ = height;
    const unsigned int content_height = height_ > chrome_height_ ? height_ - chrome_height_ : 1u;
    cols_ = std::max(1u, width_ / cell_w_);
    rows_ = std::max(1u, content_height / cell_h_);
    std::vector<Cell> next_cells(static_cast<std::size_t>(cols_ * rows_));
    const unsigned int copy_cols = std::min(cols_, old_cols_);
    const unsigned int copy_rows = std::min(rows_, old_rows_);
    for (unsigned int row = 0; row < copy_rows; ++row) {
      for (unsigned int col = 0; col < copy_cols; ++col) {
        next_cells[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row) * old_cols_ + col];
      }
    }
    cells_.swap(next_cells);
    old_cols_ = cols_;
    old_rows_ = rows_;
    history_rows_.clear();
    scrollback_offset_ = 0;
    cursor_x_ = std::min(cursor_x_, static_cast<int>(cols_ - 1));
    cursor_y_ = std::min(cursor_y_, static_cast<int>(rows_ - 1));
    if (alternate_screen_state_.active) {
      resize_saved_main_screen(previous_cols, previous_rows);
    }
    reset_scroll_region();
    recreate_backing();
    notify_child_winsize();
    redraw();
  }

  void recreate_backing() {
    if (backing_) {
      XFreePixmap(display_, backing_);
      backing_ = 0;
    }
    backing_ = XCreatePixmap(display_, window_, width_, height_, static_cast<unsigned int>(DefaultDepth(display_, screen_)));
  }

  void notify_child_winsize() {
    if (master_fd_ < 0) {
      return;
    }
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols_);
    ws.ws_row = static_cast<unsigned short>(rows_);
    ws.ws_xpixel = static_cast<unsigned short>(width_);
    ws.ws_ypixel = static_cast<unsigned short>(height_ > chrome_height_ ? height_ - chrome_height_ : height_);
    ioctl(master_fd_, TIOCSWINSZ, &ws);
  }

  void spawn_shell() {
    std::array<char, 64> slave_name{};
    master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd_ < 0) {
      throw std::runtime_error(std::string("posix_openpt failed: ") + std::strerror(errno));
    }
    if (grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
      throw std::runtime_error(std::string("pty setup failed: ") + std::strerror(errno));
    }
    char* name = ptsname(master_fd_);
    if (!name) {
      throw std::runtime_error(std::string("ptsname failed: ") + std::strerror(errno));
    }
    std::snprintf(slave_name.data(), slave_name.size(), "%s", name);

    const pid_t pid = fork();
    if (pid < 0) {
      throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
      if (master_fd_ >= 0) {
        close(master_fd_);
      }
      setsid();
      const int slave_fd = open(slave_name.data(), O_RDWR);
      if (slave_fd < 0) {
        _exit(127);
      }
      ioctl(slave_fd, TIOCSCTTY, 0);
      winsize ws{};
      ws.ws_col = static_cast<unsigned short>(cols_);
      ws.ws_row = static_cast<unsigned short>(rows_);
      ws.ws_xpixel = static_cast<unsigned short>(width_);
      ws.ws_ypixel = static_cast<unsigned short>(height_ > chrome_height_ ? height_ - chrome_height_ : height_);
      ioctl(slave_fd, TIOCSWINSZ, &ws);
      dup2(slave_fd, STDIN_FILENO);
      dup2(slave_fd, STDOUT_FILENO);
      dup2(slave_fd, STDERR_FILENO);
      if (slave_fd > STDERR_FILENO) {
        close(slave_fd);
      }
      setenv("TERM", "xterm-256color", 1);
      setenv("COLORTERM", "truecolor", 1);
      setenv("TERM_PROGRAM", "chiux-te", 1);
      if (!exec_command_.empty()) {
        execl("/bin/bash", "bash", "-lc", exec_command_.c_str(), static_cast<char*>(nullptr));
      } else {
        execl("/bin/bash", "bash", "-i", static_cast<char*>(nullptr));
      }
      _exit(127);
    }

    child_pid_ = pid;
    const int flags = fcntl(master_fd_, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    notify_child_winsize();
  }

  void terminate_child() {
    if (child_pid_ > 0) {
      kill(child_pid_, SIGHUP);
      kill(child_pid_, SIGTERM);
      int status = 0;
      waitpid(child_pid_, &status, WNOHANG);
      child_pid_ = -1;
    }
  }

  bool reap_child_if_done() {
    if (child_pid_ <= 0) {
      return false;
    }
    int status = 0;
    const pid_t pid = waitpid(child_pid_, &status, WNOHANG);
    if (pid == 0) {
      return false;
    }
    if (pid > 0) {
      child_pid_ = -1;
      if (should_respawn_interactive_shell(status)) {
        respawn_interactive_shell();
        return false;
      }
      running_ = false;
      return true;
    }
    if (errno != ECHILD) {
      chiux::log::warn(std::string("chiux-te waitpid failed: ") + std::strerror(errno));
    }
    child_pid_ = -1;
    running_ = false;
    return true;
  }

  void send_bytes(const char* data, std::size_t length) {
    if (master_fd_ < 0 || length == 0) {
      return;
    }
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(length)) {
      const ssize_t n = write(master_fd_, data + written, length - static_cast<std::size_t>(written));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      written += n;
    }
  }

  void read_pty() {
    char buffer[4096];
    while (true) {
      const ssize_t n = read(master_fd_, buffer, sizeof(buffer));
      if (n > 0) {
        for (ssize_t i = 0; i < n; ++i) {
          process_byte(static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]));
        }
        cursor_visible_ = true;
        next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);
        redraw();
        continue;
      }
      if (n == 0) {
        break;
      }
      break;
    }
  }

  bool should_respawn_interactive_shell(int status) const {
    if (!exec_command_.empty()) {
      return false;
    }
    if (WIFSIGNALED(status)) {
      return true;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
  }

  void respawn_interactive_shell() {
    if (master_fd_ >= 0) {
      close(master_fd_);
      master_fd_ = -1;
    }
    history_rows_.clear();
    scrollback_offset_ = 0;
    alternate_screen_active_ = false;
    alternate_screen_state_ = AlternateScreenState{};
    clear_completion_state();
    editor_.text.clear();
    editor_.cursor = 0;
    reset_screen();
    spawn_shell();
    redraw();
  }

  void process_byte(unsigned char ch) {
    switch (state_) {
      case ParseState::Ground:
        if (ch == 0x1B) {
          state_ = ParseState::Escape;
        } else if (ch == 0x0E) {
          use_g1_charset_ = true;
        } else if (ch == 0x0F) {
          use_g1_charset_ = false;
        } else if (ch == '\r') {
          cursor_x_ = 0;
        } else if (ch == '\n') {
          line_feed();
        } else if (ch == '\b' || ch == 0x7f) {
          erase_previous_glyph();
        } else if (ch == '\t') {
          const int next_tab = ((cursor_x_ / 8) + 1) * 8;
          while (cursor_x_ < next_tab) {
            put_glyph(" ", 1);
          }
        } else if (ch >= 32) {
          consume_printable_byte(ch);
        }
        break;
      case ParseState::Escape:
        if (ch == '[') {
          state_ = ParseState::Csi;
          csi_params_.clear();
          csi_current_ = 0;
          csi_have_value_ = false;
          csi_private_ = false;
        } else if (ch == '=') {
          application_keypad_mode_ = true;
          state_ = ParseState::Ground;
        } else if (ch == '>') {
          application_keypad_mode_ = false;
          state_ = ParseState::Ground;
        } else if (ch == '(') {
          state_ = ParseState::Charset;
          charset_target_g1_ = false;
        } else if (ch == ')') {
          state_ = ParseState::Charset;
          charset_target_g1_ = true;
        } else if (ch == ']') {
          state_ = ParseState::Osc;
          osc_buffer_.clear();
        } else if (ch == 'c') {
          reset_screen();
          state_ = ParseState::Ground;
        } else if (ch == '7') {
          save_cursor();
          state_ = ParseState::Ground;
        } else if (ch == '8') {
          restore_cursor();
          state_ = ParseState::Ground;
        } else if (ch == 'D' || ch == 'E') {
          line_feed();
          state_ = ParseState::Ground;
        } else if (ch == 'M') {
          reverse_index();
          state_ = ParseState::Ground;
        } else {
          state_ = ParseState::Ground;
        }
        break;
      case ParseState::Charset:
        if (ch == '0' || ch == '2') {
          if (charset_target_g1_) {
            g1_special_graphics_ = true;
          } else {
            g0_special_graphics_ = true;
          }
        } else {
          if (charset_target_g1_) {
            g1_special_graphics_ = false;
          } else {
            g0_special_graphics_ = false;
          }
        }
        state_ = ParseState::Ground;
        break;
      case ParseState::Csi:
        if (ch == '?') {
          csi_private_ = true;
        } else if (ch >= '0' && ch <= '9') {
          csi_current_ = csi_current_ * 10 + (ch - '0');
          csi_have_value_ = true;
        } else if (ch == ';') {
          csi_params_.push_back(csi_have_value_ ? csi_current_ : -1);
          csi_current_ = 0;
          csi_have_value_ = false;
        } else {
          if (csi_have_value_ || csi_params_.empty()) {
            csi_params_.push_back(csi_have_value_ ? csi_current_ : -1);
          }
          apply_csi(ch);
          state_ = ParseState::Ground;
        }
        break;
      case ParseState::Osc:
        if (ch == 0x07) {
          apply_osc();
          state_ = ParseState::Ground;
        } else if (ch == 0x1B) {
          osc_seen_escape_ = true;
        } else if (osc_seen_escape_ && ch == '\\') {
          apply_osc();
          state_ = ParseState::Ground;
          osc_seen_escape_ = false;
        } else {
          if (osc_seen_escape_) {
            osc_buffer_.push_back('\x1b');
            osc_seen_escape_ = false;
          }
          osc_buffer_.push_back(static_cast<char>(ch));
        }
        break;
    }
  }

  void apply_csi(unsigned char final) {
    auto param_or = [&](std::size_t index, int default_value) {
      if (index >= csi_params_.size() || csi_params_[index] < 0) {
        return default_value;
      }
      return csi_params_[index];
    };

    if (csi_private_ && (final == 'h' || final == 'l')) {
      const bool enable = final == 'h';
      for (int raw : csi_params_) {
        const int value = raw < 0 ? 0 : raw;
        if (value == 47 || value == 1047 || value == 1049) {
          set_alternate_screen(enable);
        } else if (value == 1) {
          application_cursor_keys_ = enable;
        } else if (value == 6) {
          origin_mode_ = enable;
        } else if (value == 2004) {
          bracketed_paste_mode_ = enable;
        } else if (value == 1000) {
          mouse_reporting_ = enable;
        } else if (value == 1002 || value == 1003) {
          mouse_reporting_ = enable;
          mouse_motion_reporting_ = enable;
        } else if (value == 1006) {
          mouse_sgr_mode_ = enable;
        } else if (value == 1015) {
          mouse_sgr_mode_ = enable;
        }
      }
      csi_private_ = false;
      return;
    }

    if (csi_private_) {
      csi_private_ = false;
    }

    switch (final) {
      case 'A':
        cursor_y_ = clamp_cursor_row(cursor_y_ - param_or(0, 1));
        break;
      case 'B':
        cursor_y_ = clamp_cursor_row(cursor_y_ + param_or(0, 1));
        break;
      case 'C':
        cursor_x_ = std::min(static_cast<int>(cols_) - 1, cursor_x_ + param_or(0, 1));
        break;
      case 'D':
        cursor_x_ = std::max(0, cursor_x_ - param_or(0, 1));
        break;
      case 'H':
      case 'f':
        cursor_y_ = clamp_cursor_row(param_or(0, 1) - 1);
        cursor_x_ = std::clamp(param_or(1, 1) - 1, 0, static_cast<int>(cols_) - 1);
        break;
      case 'd':
        cursor_y_ = clamp_cursor_row(param_or(0, 1) - 1);
        break;
      case 'a':
        cursor_x_ = std::min(static_cast<int>(cols_) - 1, cursor_x_ + param_or(0, 1));
        break;
      case 'e':
        cursor_y_ = clamp_cursor_row(cursor_y_ + param_or(0, 1));
        break;
      case 'J': {
        const int mode = param_or(0, 0);
        if (mode == 2) {
          reset_screen();
        } else if (mode == 0) {
          clear_from_cursor();
        }
        break;
      }
      case 'K': {
        const int mode = param_or(0, 0);
        if (mode == 2) {
          clear_line_all();
        } else {
          clear_line_from_cursor();
        }
        break;
      }
      case 'G':
        cursor_x_ = std::clamp(param_or(0, 1) - 1, 0, static_cast<int>(cols_) - 1);
        break;
      case 'r': {
        const int top = param_or(0, 1) - 1;
        const int bottom = param_or(1, static_cast<int>(rows_)) - 1;
        set_scroll_region(top, bottom);
        break;
      }
      case 'L':
        insert_lines(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'M':
        delete_lines(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'P':
        delete_chars(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case '@':
        insert_chars(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'X':
        erase_chars(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'S':
        scroll_region_up(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'T':
        scroll_region_down(static_cast<unsigned int>(std::max(1, param_or(0, 1))));
        break;
      case 'm':
        apply_sgr();
        break;
      case 's':
        save_cursor();
        break;
      case 'u':
        restore_cursor();
        break;
      default:
        break;
    }
  }

  void apply_sgr() {
    if (csi_params_.empty() || (csi_params_.size() == 1 && csi_params_[0] < 0)) {
      current_fg_pixel_ = foreground_pixel_;
      current_bg_pixel_ = background_pixel_;
      current_bold_ = false;
      current_inverse_ = false;
      return;
    }
    for (std::size_t i = 0; i < csi_params_.size(); ++i) {
      const int value = csi_params_[i] < 0 ? 0 : csi_params_[i];
      if (value == 0) {
        current_fg_pixel_ = foreground_pixel_;
        current_bg_pixel_ = background_pixel_;
        current_bold_ = false;
        current_inverse_ = false;
      } else if (value == 1) {
        current_bold_ = true;
      } else if (value == 22) {
        current_bold_ = false;
      } else if (value == 7) {
        current_inverse_ = true;
      } else if (value == 27) {
        current_inverse_ = false;
      } else if (value == 39) {
        current_fg_pixel_ = foreground_pixel_;
      } else if (value == 49) {
        current_bg_pixel_ = background_pixel_;
      } else if (value >= 30 && value <= 37) {
        const std::size_t index = static_cast<std::size_t>(value - 30 + ((current_bold_ && value < 38) ? 8 : 0));
        current_fg_pixel_ = palette_[index % 16];
      } else if (value >= 40 && value <= 47) {
        current_bg_pixel_ = palette_[static_cast<std::size_t>(value - 40)];
      } else if (value >= 90 && value <= 97) {
        current_fg_pixel_ = palette_[static_cast<std::size_t>(value - 90 + 8)];
      } else if (value >= 100 && value <= 107) {
        current_bg_pixel_ = palette_[static_cast<std::size_t>(value - 100 + 8)];
      } else if (value == 38 || value == 48) {
        const bool is_fg = value == 38;
        if (i + 1 < csi_params_.size()) {
          const int mode = csi_params_[i + 1] < 0 ? 0 : csi_params_[i + 1];
          if (mode == 5 && i + 2 < csi_params_.size()) {
            const unsigned long pixel = color_table_[static_cast<std::size_t>(std::clamp(csi_params_[i + 2], 0, 255))];
            if (is_fg) {
              current_fg_pixel_ = pixel;
            } else {
              current_bg_pixel_ = pixel;
            }
            i += 2;
          } else if (mode == 2 && i + 4 < csi_params_.size()) {
            const unsigned int r = static_cast<unsigned int>(std::clamp(csi_params_[i + 2], 0, 255));
            const unsigned int g = static_cast<unsigned int>(std::clamp(csi_params_[i + 3], 0, 255));
            const unsigned int b = static_cast<unsigned int>(std::clamp(csi_params_[i + 4], 0, 255));
            const unsigned long pixel = rgb_pixel(display_, screen_, r, g, b);
            if (is_fg) {
              current_fg_pixel_ = pixel;
            } else {
              current_bg_pixel_ = pixel;
            }
            i += 4;
          }
        }
      }
    }
  }

  void save_cursor() {
    saved_x_ = cursor_x_;
    saved_y_ = cursor_y_;
    saved_fg_pixel_ = current_fg_pixel_;
    saved_bg_pixel_ = current_bg_pixel_;
    saved_bold_ = current_bold_;
    saved_inverse_ = current_inverse_;
    has_saved_ = true;
  }

  void restore_cursor() {
    if (!has_saved_) {
      return;
    }
    cursor_x_ = saved_x_;
    cursor_y_ = saved_y_;
    current_fg_pixel_ = saved_fg_pixel_;
    current_bg_pixel_ = saved_bg_pixel_;
    current_bold_ = saved_bold_;
    current_inverse_ = saved_inverse_;
  }

  void apply_osc() {
    if (osc_buffer_.empty()) {
      return;
    }
    const auto separator = osc_buffer_.find(';');
    if (separator == std::string::npos) {
      osc_buffer_.clear();
      return;
    }
    const int command = std::atoi(osc_buffer_.substr(0, separator).c_str());
    const std::string value = osc_buffer_.substr(separator + 1);
    if (command == 0 || command == 2) {
      XStoreName(display_, window_, value.c_str());
    }
    osc_buffer_.clear();
  }

  void set_alternate_screen(bool enable) {
    if (enable == alternate_screen_active_) {
      return;
    }
    if (enable) {
      clear_completion_state();
      editor_.text.clear();
      editor_.cursor = 0;
      alternate_screen_state_.cells = cells_;
      alternate_screen_state_.cursor_x = cursor_x_;
      alternate_screen_state_.cursor_y = cursor_y_;
      alternate_screen_state_.current_fg_pixel = current_fg_pixel_;
      alternate_screen_state_.current_bg_pixel = current_bg_pixel_;
      alternate_screen_state_.current_bold = current_bold_;
      alternate_screen_state_.current_inverse = current_inverse_;
      alternate_screen_state_.cols = cols_;
      alternate_screen_state_.rows = rows_;
      alternate_screen_state_.scroll_region_top = scroll_region_top_;
      alternate_screen_state_.scroll_region_bottom = scroll_region_bottom_;
      alternate_screen_state_.origin_mode = origin_mode_;
      alternate_screen_state_.history_rows = history_rows_;
      alternate_screen_state_.scrollback_offset = scrollback_offset_;
      alternate_screen_state_.g0_special_graphics = g0_special_graphics_;
      alternate_screen_state_.g1_special_graphics = g1_special_graphics_;
      alternate_screen_state_.use_g1_charset = use_g1_charset_;
      alternate_screen_state_.active = true;
      history_rows_.clear();
      scrollback_offset_ = 0;
      for (auto& cell : cells_) {
        cell = Cell{};
      }
      cursor_x_ = 0;
      cursor_y_ = 0;
      reset_scroll_region();
    } else if (alternate_screen_state_.active) {
      std::vector<Cell> restored(static_cast<std::size_t>(cols_ * rows_));
      if (alternate_screen_state_.cols > 0 && alternate_screen_state_.rows > 0 && !alternate_screen_state_.cells.empty()) {
        const unsigned int copy_cols = std::min(cols_, alternate_screen_state_.cols);
        const unsigned int copy_rows = std::min(rows_, alternate_screen_state_.rows);
        for (unsigned int row = 0; row < copy_rows; ++row) {
          for (unsigned int col = 0; col < copy_cols; ++col) {
            restored[static_cast<std::size_t>(row) * cols_ + col] =
                alternate_screen_state_.cells[static_cast<std::size_t>(row) * alternate_screen_state_.cols + col];
          }
        }
      }
      cells_.swap(restored);
      cursor_x_ = alternate_screen_state_.cursor_x;
      cursor_y_ = alternate_screen_state_.cursor_y;
      current_fg_pixel_ = alternate_screen_state_.current_fg_pixel;
      current_bg_pixel_ = alternate_screen_state_.current_bg_pixel;
      current_bold_ = alternate_screen_state_.current_bold;
      current_inverse_ = alternate_screen_state_.current_inverse;
      scroll_region_top_ = alternate_screen_state_.scroll_region_top;
      scroll_region_bottom_ = alternate_screen_state_.scroll_region_bottom;
      origin_mode_ = alternate_screen_state_.origin_mode;
      history_rows_ = alternate_screen_state_.history_rows;
      scrollback_offset_ = alternate_screen_state_.scrollback_offset;
      g0_special_graphics_ = alternate_screen_state_.g0_special_graphics;
      g1_special_graphics_ = alternate_screen_state_.g1_special_graphics;
      use_g1_charset_ = alternate_screen_state_.use_g1_charset;
      alternate_screen_state_.active = false;
      clear_completion_state();
      editor_.text.clear();
      editor_.cursor = 0;
    }
    alternate_screen_active_ = enable;
    redraw();
  }

  void consume_printable_byte(unsigned char ch) {
    const bool special_charset = (use_g1_charset_ ? g1_special_graphics_ : g0_special_graphics_);
    if (ch < 0x80u && special_charset) {
      const std::string glyph = special_graphics_glyph(ch);
      if (!glyph.empty()) {
        put_glyph(glyph, 1);
        return;
      }
    }

    if (ch < 0x80u) {
      if (!utf8_pending_.empty()) {
        utf8_pending_.clear();
        utf8_expected_ = 0;
      }
      put_glyph(std::string(1, static_cast<char>(ch)), 1);
      return;
    }

    if (utf8_pending_.empty()) {
      utf8_expected_ = utf8_expected_length(ch);
      if (utf8_expected_ == 0) {
        return;
      }
    }

    utf8_pending_.push_back(static_cast<char>(ch));
    if (utf8_pending_.size() < utf8_expected_) {
      return;
    }

    char32_t codepoint = 0;
    if (utf8_decode(utf8_pending_, codepoint)) {
      const unsigned int width = utf8_display_width(codepoint);
      const std::string glyph = utf8_from_codepoint(codepoint);
      if (width == 0) {
        append_to_previous_cell(glyph);
      } else {
        put_glyph(glyph, width);
      }
    }
    utf8_pending_.clear();
    utf8_expected_ = 0;
  }

  void put_glyph(const std::string& glyph, unsigned int width) {
    if (glyph.empty()) {
      return;
    }
    if (width == 0) {
      width = 1;
    }
    if (cursor_x_ >= static_cast<int>(cols_)) {
      line_feed();
      cursor_x_ = 0;
    }
    if (width > 1 && cursor_x_ >= static_cast<int>(cols_) - 1) {
      line_feed();
      cursor_x_ = 0;
    }
    const std::size_t index = static_cast<std::size_t>(cursor_y_) * cols_ + static_cast<std::size_t>(cursor_x_);
    if (index < cells_.size()) {
      auto& cell = cells_[index];
      cell = Cell{};
      cell.text = glyph;
      cell.fg = current_fg_pixel_;
      cell.bg = current_bg_pixel_;
      cell.bold = current_bold_;
      cell.inverse = current_inverse_;
      cell.continuation = false;
      if (width > 1) {
        const std::size_t next_index = index + 1;
        if (next_index < cells_.size()) {
          cells_[next_index] = Cell{};
          cells_[next_index].fg = current_fg_pixel_;
          cells_[next_index].bg = current_bg_pixel_;
          cells_[next_index].bold = current_bold_;
          cells_[next_index].inverse = current_inverse_;
          cells_[next_index].continuation = true;
        }
      }
    }
    cursor_x_ += static_cast<int>(width);
    if (cursor_x_ >= static_cast<int>(cols_)) {
      line_feed();
      cursor_x_ = 0;
    }
  }

  void append_to_previous_cell(const std::string& glyph) {
    if (glyph.empty() || cursor_x_ <= 0 || cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    const std::size_t index = static_cast<std::size_t>(cursor_y_) * cols_ + static_cast<std::size_t>(cursor_x_ - 1);
    if (index < cells_.size()) {
      cells_[index].text += glyph;
    }
  }

  void erase_previous_glyph() {
    if (cursor_x_ <= 0) {
      return;
    }
    --cursor_x_;
    if (cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    std::size_t index = static_cast<std::size_t>(cursor_y_) * cols_ + static_cast<std::size_t>(cursor_x_);
    while (index < cells_.size() && cells_[index].continuation) {
      if (cursor_x_ == 0) {
        break;
      }
      --cursor_x_;
      index = static_cast<std::size_t>(cursor_y_) * cols_ + static_cast<std::size_t>(cursor_x_);
    }
    if (index < cells_.size()) {
      const bool spans_next = !cells_[index].text.empty() && index + 1 < cells_.size() && cells_[index + 1].continuation;
      cells_[index] = Cell{};
      if (spans_next) {
        cells_[index + 1] = Cell{};
      }
    }
  }

  void line_feed() {
    if (cursor_y_ < scroll_region_bottom_) {
      ++cursor_y_;
    } else {
      scroll_region_up(1);
      cursor_y_ = scroll_region_bottom_;
    }
  }

  void reverse_index() {
    if (cursor_y_ > scroll_region_top_) {
      --cursor_y_;
    } else {
      scroll_region_down(1);
      cursor_y_ = scroll_region_top_;
    }
  }

  void scroll_up() {
    if (rows_ == 0 || cols_ == 0) {
      return;
    }
    history_rows_.push_back(snapshot_row(0));
    if (history_rows_.size() > history_limit_) {
      history_rows_.pop_front();
    }
    if (scrollback_offset_ > 0) {
      ++scrollback_offset_;
    }
    for (unsigned int row = 1; row < rows_; ++row) {
      for (unsigned int col = 0; col < cols_; ++col) {
        cells_[static_cast<std::size_t>(row - 1) * cols_ + col] = cells_[static_cast<std::size_t>(row) * cols_ + col];
      }
    }
    clear_row(rows_ - 1);
  }

  void scroll_down() {
    if (rows_ == 0 || cols_ == 0) {
      return;
    }
    for (unsigned int row = rows_ - 1; row > 0; --row) {
      for (unsigned int col = 0; col < cols_; ++col) {
        cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row - 1) * cols_ + col];
      }
    }
    clear_row(0);
  }

  void clear_row(unsigned int row) {
    if (row >= rows_) {
      return;
    }
    for (unsigned int col = 0; col < cols_; ++col) {
      auto& cell = cells_[static_cast<std::size_t>(row) * cols_ + col];
      cell = Cell{};
    }
  }

  void clear_line_from_cursor() {
    if (cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    for (unsigned int col = static_cast<unsigned int>(std::max(0, cursor_x_)); col < cols_; ++col) {
      cells_[static_cast<std::size_t>(cursor_y_) * cols_ + col] = Cell{};
    }
  }

  void clear_line_all() {
    if (cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    clear_row(static_cast<unsigned int>(cursor_y_));
  }

  void clear_from_cursor() {
    clear_line_from_cursor();
    for (unsigned int row = static_cast<unsigned int>(std::max(0, cursor_y_ + 1)); row <= static_cast<unsigned int>(scroll_region_bottom_); ++row) {
      clear_row(row);
    }
  }

  void resize_saved_main_screen(unsigned int old_cols, unsigned int old_rows) {
    if (!alternate_screen_state_.active) {
      return;
    }
    std::vector<Cell> resized(static_cast<std::size_t>(cols_ * rows_));
    if (old_cols > 0 && old_rows > 0 && !alternate_screen_state_.cells.empty()) {
      const unsigned int copy_cols = std::min(cols_, old_cols);
      const unsigned int copy_rows = std::min(rows_, old_rows);
      for (unsigned int row = 0; row < copy_rows; ++row) {
        for (unsigned int col = 0; col < copy_cols; ++col) {
          resized[static_cast<std::size_t>(row) * cols_ + col] =
              alternate_screen_state_.cells[static_cast<std::size_t>(row) * old_cols + col];
        }
      }
    }
    alternate_screen_state_.cells.swap(resized);
    alternate_screen_state_.cols = cols_;
    alternate_screen_state_.rows = rows_;
    alternate_screen_state_.cursor_x = std::clamp(alternate_screen_state_.cursor_x, 0, static_cast<int>(cols_) - 1);
    alternate_screen_state_.cursor_y = std::clamp(alternate_screen_state_.cursor_y, 0, static_cast<int>(rows_) - 1);
    alternate_screen_state_.scroll_region_top = 0;
    alternate_screen_state_.scroll_region_bottom = static_cast<int>(rows_ ? rows_ - 1 : 0);
    alternate_screen_state_.origin_mode = false;
  }

  void reset_scroll_region() {
    scroll_region_top_ = 0;
    scroll_region_bottom_ = static_cast<int>(rows_ ? rows_ - 1 : 0);
    origin_mode_ = false;
  }

  int clamp_cursor_row(int row) const {
    if (origin_mode_) {
      const int top = scroll_region_top_;
      const int bottom = scroll_region_bottom_;
      return std::clamp(top + row, top, bottom);
    }
    return std::clamp(row, 0, static_cast<int>(rows_) - 1);
  }

  void set_scroll_region(int top, int bottom) {
    if (rows_ == 0) {
      return;
    }
    const int max_row = static_cast<int>(rows_) - 1;
    if (top < 0) {
      top = 0;
    }
    if (bottom < 0 || bottom > max_row) {
      bottom = max_row;
    }
    if (top >= bottom) {
      reset_scroll_region();
      return;
    }
    scroll_region_top_ = top;
    scroll_region_bottom_ = bottom;
    if (cursor_y_ < scroll_region_top_ || cursor_y_ > scroll_region_bottom_) {
      cursor_y_ = scroll_region_top_;
    }
    cursor_x_ = std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1);
  }

  void scroll_region_up(unsigned int lines) {
    if (rows_ == 0 || cols_ == 0 || lines == 0) {
      return;
    }
    const unsigned int top = static_cast<unsigned int>(std::clamp(scroll_region_top_, 0, static_cast<int>(rows_) - 1));
    const unsigned int bottom = static_cast<unsigned int>(std::clamp(scroll_region_bottom_, 0, static_cast<int>(rows_) - 1));
    const unsigned int region_height = bottom >= top ? bottom - top + 1u : 0u;
    if (region_height == 0) {
      return;
    }
    const unsigned int amount = std::min(lines, region_height);
    if (top == 0 && bottom + 1u == rows_) {
      history_rows_.push_back(snapshot_row(0));
      if (history_rows_.size() > history_limit_) {
        history_rows_.pop_front();
      }
      if (scrollback_offset_ > 0) {
        ++scrollback_offset_;
      }
    }
    for (unsigned int line = 0; line < amount; ++line) {
      for (unsigned int row = top; row < bottom; ++row) {
        for (unsigned int col = 0; col < cols_; ++col) {
          cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row + 1) * cols_ + col];
        }
      }
      clear_row(bottom);
    }
  }

  void scroll_region_down(unsigned int lines) {
    if (rows_ == 0 || cols_ == 0 || lines == 0) {
      return;
    }
    const unsigned int top = static_cast<unsigned int>(std::clamp(scroll_region_top_, 0, static_cast<int>(rows_) - 1));
    const unsigned int bottom = static_cast<unsigned int>(std::clamp(scroll_region_bottom_, 0, static_cast<int>(rows_) - 1));
    const unsigned int region_height = bottom >= top ? bottom - top + 1u : 0u;
    if (region_height == 0) {
      return;
    }
    const unsigned int amount = std::min(lines, region_height);
    for (unsigned int line = 0; line < amount; ++line) {
      for (unsigned int row = bottom; row > top; --row) {
        for (unsigned int col = 0; col < cols_; ++col) {
          cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row - 1) * cols_ + col];
        }
      }
      clear_row(top);
    }
  }

  void insert_lines(unsigned int lines) {
    if (rows_ == 0 || cols_ == 0 || cursor_y_ < scroll_region_top_ || cursor_y_ > scroll_region_bottom_) {
      return;
    }
    const unsigned int top = static_cast<unsigned int>(cursor_y_);
    const unsigned int bottom = static_cast<unsigned int>(std::clamp(scroll_region_bottom_, 0, static_cast<int>(rows_) - 1));
    const unsigned int amount = std::min(lines, bottom - top + 1u);
    for (unsigned int line = 0; line < amount; ++line) {
      for (unsigned int row = bottom; row > top; --row) {
        for (unsigned int col = 0; col < cols_; ++col) {
          cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row - 1) * cols_ + col];
        }
      }
      clear_row(top);
    }
  }

  void delete_lines(unsigned int lines) {
    if (rows_ == 0 || cols_ == 0 || cursor_y_ < scroll_region_top_ || cursor_y_ > scroll_region_bottom_) {
      return;
    }
    const unsigned int top = static_cast<unsigned int>(cursor_y_);
    const unsigned int bottom = static_cast<unsigned int>(std::clamp(scroll_region_bottom_, 0, static_cast<int>(rows_) - 1));
    const unsigned int amount = std::min(lines, bottom - top + 1u);
    for (unsigned int line = 0; line < amount; ++line) {
      for (unsigned int row = top; row < bottom; ++row) {
        for (unsigned int col = 0; col < cols_; ++col) {
          cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row + 1) * cols_ + col];
        }
      }
      clear_row(bottom);
    }
  }

  void insert_chars(unsigned int chars) {
    if (rows_ == 0 || cols_ == 0 || cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    const unsigned int row = static_cast<unsigned int>(cursor_y_);
    const unsigned int start = static_cast<unsigned int>(std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1));
    const unsigned int amount = std::min(chars, cols_ - start);
    if (amount == 0) {
      return;
    }
    for (int col = static_cast<int>(cols_) - 1; col >= static_cast<int>(start + amount); --col) {
      cells_[static_cast<std::size_t>(row) * cols_ + static_cast<std::size_t>(col)] =
          cells_[static_cast<std::size_t>(row) * cols_ + static_cast<std::size_t>(col - static_cast<int>(amount))];
    }
    for (unsigned int col = 0; col < amount; ++col) {
      cells_[static_cast<std::size_t>(row) * cols_ + start + col] = Cell{};
    }
  }

  void delete_chars(unsigned int chars) {
    if (rows_ == 0 || cols_ == 0 || cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    const unsigned int row = static_cast<unsigned int>(cursor_y_);
    const unsigned int start = static_cast<unsigned int>(std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1));
    const unsigned int amount = std::min(chars, cols_ - start);
    if (amount == 0) {
      return;
    }
    for (unsigned int col = start; col + amount < cols_; ++col) {
      cells_[static_cast<std::size_t>(row) * cols_ + col] = cells_[static_cast<std::size_t>(row) * cols_ + col + amount];
    }
    for (unsigned int col = cols_ - amount; col < cols_; ++col) {
      cells_[static_cast<std::size_t>(row) * cols_ + col] = Cell{};
    }
  }

  void erase_chars(unsigned int chars) {
    if (rows_ == 0 || cols_ == 0 || cursor_y_ < 0 || cursor_y_ >= static_cast<int>(rows_)) {
      return;
    }
    const unsigned int row = static_cast<unsigned int>(cursor_y_);
    const unsigned int start = static_cast<unsigned int>(std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1));
    const unsigned int amount = std::min(chars, cols_ - start);
    for (unsigned int col = 0; col < amount; ++col) {
      cells_[static_cast<std::size_t>(row) * cols_ + start + col] = Cell{};
    }
  }

  std::vector<Cell> snapshot_row(unsigned int row) const {
    std::vector<Cell> snapshot;
    if (row >= rows_) {
      return snapshot;
    }
    snapshot.reserve(cols_);
    for (unsigned int col = 0; col < cols_; ++col) {
      snapshot.push_back(cells_[static_cast<std::size_t>(row) * cols_ + col]);
    }
    return snapshot;
  }

  void scroll_scrollback(int delta) {
    if (delta == 0) {
      return;
    }
    const std::size_t history_count = history_rows_.size();
    if (delta > 0) {
      scrollback_offset_ = std::min<std::size_t>(history_count, scrollback_offset_ + static_cast<std::size_t>(delta));
    } else {
      const std::size_t amount = static_cast<std::size_t>(-delta);
      scrollback_offset_ = scrollback_offset_ > amount ? scrollback_offset_ - amount : 0;
    }
    redraw();
  }

  struct SelectionRange {
    bool active = false;
    std::size_t start_row = 0;
    std::size_t start_col = 0;
    std::size_t end_row = 0;
    std::size_t end_col = 0;
  };

  std::vector<Cell> visible_row(unsigned int row) const {
    std::vector<Cell> out(cols_);
    const std::size_t history_count = history_rows_.size();
    const std::size_t offset = std::min(scrollback_offset_, history_count);
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(history_count) - static_cast<std::ptrdiff_t>(offset);
    const std::ptrdiff_t source_index = start + static_cast<std::ptrdiff_t>(row);
    if (source_index >= 0 && static_cast<std::size_t>(source_index) < history_count) {
      const auto& source = history_rows_[static_cast<std::size_t>(source_index)];
      for (unsigned int col = 0; col < cols_ && col < source.size(); ++col) {
        out[col] = source[col];
      }
      return out;
    }
    const std::ptrdiff_t current_row = source_index - static_cast<std::ptrdiff_t>(history_count);
    if (current_row >= 0 && current_row < static_cast<std::ptrdiff_t>(rows_)) {
      for (unsigned int col = 0; col < cols_; ++col) {
        out[col] = cells_[static_cast<std::size_t>(current_row) * cols_ + col];
      }
    }
    return out;
  }

  std::string visible_row_text(unsigned int row) const {
    std::string text;
    if (row >= rows_) {
      return text;
    }
    const auto cells = visible_row(row);
    text.reserve(cols_);
    for (const auto& cell : cells) {
      if (cell.continuation) {
        continue;
      }
      if (cell.text.empty()) {
        text.push_back(' ');
      } else {
        text += cell.text;
      }
    }
    while (!text.empty() && text.back() == ' ') {
      text.pop_back();
    }
    return text;
  }

  std::string selection_text_for_range(const SelectionRange& range) const {
    if (!range.active || rows_ == 0 || cols_ == 0) {
      return {};
    }
    const bool forward = (range.start_row < range.end_row) ||
                         (range.start_row == range.end_row && range.start_col <= range.end_col);
    const std::size_t first_row = forward ? range.start_row : range.end_row;
    const std::size_t first_col = forward ? range.start_col : range.end_col;
    const std::size_t last_row = forward ? range.end_row : range.start_row;
    const std::size_t last_col = forward ? range.end_col : range.start_col;

    std::string out;
    for (std::size_t row = first_row; row <= last_row && row < rows_; ++row) {
      const auto cells = visible_row(static_cast<unsigned int>(row));
      const std::size_t start_col = (row == first_row) ? first_col : 0;
      const std::size_t end_col = (row == last_row) ? last_col : (cols_ > 0 ? cols_ - 1 : 0);
      for (std::size_t col = start_col; col <= end_col && col < cells.size(); ++col) {
        if (cells[col].continuation) {
          continue;
        }
        if (cells[col].text.empty()) {
          out.push_back(' ');
        } else {
          out += cells[col].text;
        }
      }
      if (row != last_row) {
        out.push_back('\n');
      }
    }
    return out;
  }

  void clear_selection() {
    selection_.active = false;
    selecting_ = false;
    selection_text_.clear();
    XSetSelectionOwner(display_, XA_PRIMARY, None, CurrentTime);
    if (clipboard_atom_ != None) {
      XSetSelectionOwner(display_, clipboard_atom_, None, CurrentTime);
    }
  }

  void finalize_selection() {
    if (!selecting_) {
      return;
    }
    selecting_ = false;
    if (selection_.start_row == selection_.end_row && selection_.start_col == selection_.end_col) {
      selection_.active = false;
      selection_text_.clear();
      redraw();
      return;
    }
    selection_.active = true;
    selection_text_ = selection_text_for_range(selection_);
    if (!selection_text_.empty()) {
      XSetSelectionOwner(display_, XA_PRIMARY, window_, CurrentTime);
      if (clipboard_atom_ != None) {
        XSetSelectionOwner(display_, clipboard_atom_, window_, CurrentTime);
      }
    }
    redraw();
  }

  void update_selection(int x, int y) {
    if (!selecting_) {
      return;
    }
    const int content_y = std::max(0, y - static_cast<int>(chrome_height_));
    const std::size_t max_col = cols_ > 0 ? static_cast<std::size_t>(cols_ - 1) : 0u;
    const std::size_t max_row = rows_ > 0 ? static_cast<std::size_t>(rows_ - 1) : 0u;
    selection_.end_col = std::clamp(static_cast<std::size_t>(x / static_cast<int>(cell_w_)), std::size_t{0}, max_col);
    selection_.end_row = std::clamp(static_cast<std::size_t>(content_y / static_cast<int>(cell_h_)), std::size_t{0}, max_row);
    redraw();
  }

  void request_primary_paste() {
    if (XGetSelectionOwner(display_, XA_PRIMARY) == window_ && !selection_text_.empty()) {
      if (bracketed_paste_mode_) {
        send_bytes("\x1b[200~", 6);
      }
      send_bytes(selection_text_.data(), selection_text_.size());
      if (bracketed_paste_mode_) {
        send_bytes("\x1b[201~", 6);
      }
      return;
    }
    XConvertSelection(display_, XA_PRIMARY, utf8_string_atom_, paste_property_, window_, CurrentTime);
  }

  struct LineEditor {
    std::string text;
    std::size_t cursor = 0;
  };

  std::size_t current_line_start() const {
    return editor_.cursor;
  }

  std::string current_line_prefix() const {
    if (editor_.cursor > editor_.text.size()) {
      return editor_.text;
    }
    return editor_.text.substr(0, editor_.cursor);
  }

  std::string current_token_prefix() const {
    const std::string line = current_line_prefix();
    const auto pos = line.find_last_of(" \t");
    if (pos == std::string::npos) {
      return line;
    }
    return line.substr(pos + 1);
  }

  std::size_t current_token_start() const {
    const std::string line = current_line_prefix();
    const auto pos = line.find_last_of(" \t");
    if (pos == std::string::npos) {
      return 0;
    }
    return pos + 1;
  }

  void clear_completion_state() {
    completion_items_.clear();
    completion_index_ = 0;
    completion_popup_open_ = false;
    completion_prefix_.clear();
    completion_token_start_ = 0;
    history_hint_.clear();
  }

  std::string history_suggestion_for_prefix(const std::string& prefix) const {
    const std::string needle = trim_copy(prefix);
    if (needle.empty()) {
      return {};
    }
    for (auto it = command_history_.rbegin(); it != command_history_.rend(); ++it) {
      if (it->rfind(needle, 0) == 0 && *it != needle) {
        return *it;
      }
    }
    return {};
  }

  void refresh_completion_state() {
    clear_completion_state();
    if (scrollback_offset_ != 0 || editor_.cursor != editor_.text.size()) {
      return;
    }

    completion_prefix_ = current_token_prefix();
    completion_token_start_ = current_token_start();
    if (completion_prefix_.empty()) {
      history_hint_ = history_suggestion_for_prefix(current_line_prefix());
      return;
    }

    std::vector<CompletionItem> items;
    std::unordered_set<std::string> seen;
    auto append_unique = [&](const std::vector<CompletionItem>& source_items) {
      for (const auto& item : source_items) {
        if (seen.insert(item.label).second) {
          items.push_back(item);
        }
        if (items.size() >= 8) {
          return;
        }
      }
    };

    if (is_path_like(completion_prefix_)) {
      append_unique(collect_path_completions(completion_prefix_, 8));
    } else {
      append_unique(collect_command_completions(completion_prefix_, 8));
      if (items.size() < 8) {
        append_unique(collect_path_completions(completion_prefix_, 8 - items.size()));
      }
    }

    completion_items_ = std::move(items);
    completion_popup_open_ = !completion_items_.empty();
    history_hint_ = history_suggestion_for_prefix(current_line_prefix());
  }

  void insert_text_at_cursor(const std::string& text) {
    if (text.empty()) {
      return;
    }
    editor_.text.insert(editor_.cursor, text);
    editor_.cursor += text.size();
    send_bytes(text.data(), text.size());
    refresh_completion_state();
    redraw();
  }

  void erase_previous_character() {
    if (editor_.cursor == 0 || editor_.text.empty()) {
      return;
    }
    editor_.text.erase(editor_.cursor - 1, 1);
    --editor_.cursor;
    send_bytes("\x7f", 1);
    refresh_completion_state();
    redraw();
  }

  void erase_next_character() {
    if (editor_.cursor >= editor_.text.size()) {
      return;
    }
    editor_.text.erase(editor_.cursor, 1);
    send_bytes("\x1b[3~", 4);
    refresh_completion_state();
    redraw();
  }

  void move_cursor_left() {
    if (editor_.cursor == 0) {
      return;
    }
    --editor_.cursor;
    send_bytes("\x1b[D", 3);
    refresh_completion_state();
    redraw();
  }

  void move_cursor_right() {
    if (editor_.cursor >= editor_.text.size()) {
      return;
    }
    ++editor_.cursor;
    send_bytes("\x1b[C", 3);
    refresh_completion_state();
    redraw();
  }

  void move_cursor_home() {
    const std::size_t amount = editor_.cursor;
    if (amount == 0) {
      return;
    }
    editor_.cursor = 0;
    send_bytes("\x1b[H", 3);
    refresh_completion_state();
    redraw();
  }

  void move_cursor_end() {
    if (editor_.cursor == editor_.text.size()) {
      return;
    }
    editor_.cursor = editor_.text.size();
    send_bytes("\x1b[F", 3);
    refresh_completion_state();
    redraw();
  }

  void commit_current_line_history() {
    const std::string line = trim_copy(editor_.text);
    if (line.empty()) {
      return;
    }
    if (command_history_.empty() || command_history_.back() != line) {
      command_history_.push_back(line);
      if (command_history_.size() > history_limit_) {
        command_history_.pop_front();
      }
    }
  }

  void accept_completion() {
    if (!completion_popup_open_ || completion_items_.empty()) {
      return;
    }
    const auto& item = completion_items_[completion_index_ % completion_items_.size()];
    if (item.insert_text.size() >= completion_prefix_.size() &&
        item.insert_text.rfind(completion_prefix_, 0) == 0) {
      const std::string suffix = item.insert_text.substr(completion_prefix_.size());
      if (!suffix.empty()) {
        editor_.text.insert(editor_.cursor, suffix);
        editor_.cursor += suffix.size();
        send_bytes(suffix.data(), suffix.size());
      }
    }
    clear_completion_state();
    redraw();
  }

  void insert_paste_data() {
    if (pasted_data_.empty()) {
      return;
    }
    if (bracketed_paste_mode_) {
      send_bytes("\x1b[200~", 6);
    }
    send_bytes(pasted_data_.data(), pasted_data_.size());
    if (bracketed_paste_mode_) {
      send_bytes("\x1b[201~", 6);
    }
    if (pasted_data_.find_first_of("\r\n") == std::string::npos) {
      editor_.text.insert(editor_.cursor, pasted_data_);
      editor_.cursor += pasted_data_.size();
      refresh_completion_state();
    } else {
      editor_.text.clear();
      editor_.cursor = 0;
      clear_completion_state();
    }
    pasted_data_.clear();
    redraw();
  }

  void send_mouse_report(int button, int x, int y, bool release, bool motion) {
    if (!mouse_reporting_) {
      return;
    }
    const int cell_x = std::clamp(x / static_cast<int>(cell_w_) + 1, 1, static_cast<int>(cols_));
    const int cell_y = std::clamp(y / static_cast<int>(cell_h_) + 1, 1, static_cast<int>(rows_));
    int code = button;
    if (motion) {
      code += 32;
    }
    if (release) {
      code = 3;
    }
    std::ostringstream out;
    out << "\x1b[<" << code << ';' << cell_x << ';' << cell_y << (release ? 'm' : 'M');
    const std::string seq = out.str();
    send_bytes(seq.data(), seq.size());
  }

  void redraw() {
    if (!display_ || !window_ || !backing_) {
      return;
    }

    const int content_y_offset = static_cast<int>(chrome_height_);

    XSetForeground(display_, gc_, background_pixel_);
    XFillRectangle(display_, backing_, gc_, 0, 0, width_, height_);
    XSetForeground(display_, gc_, chrome_pixel_);
    XFillRectangle(display_, backing_, gc_, 0, 0, width_, chrome_height_);
    XSetForeground(display_, gc_, bezel_edge_pixel_);
    XDrawRectangle(display_, backing_, gc_, 0, 0, width_ - 1, height_ - 1);
    XSetForeground(display_, gc_, chrome_shadow_pixel_);
    XDrawLine(display_, backing_, gc_, 1, static_cast<int>(chrome_height_) - 1, static_cast<int>(width_) - 2, static_cast<int>(chrome_height_) - 1);
    XSetForeground(display_, gc_, bezel_light_pixel_);
    XDrawLine(display_, backing_, gc_, 1, 1, static_cast<int>(width_) - 2, 1);
    XDrawLine(display_, backing_, gc_, 1, 1, 1, static_cast<int>(chrome_height_) - 2);
    XSetForeground(display_, gc_, bezel_shadow_pixel_);
    XDrawLine(display_, backing_, gc_, 1, static_cast<int>(height_) - 2, static_cast<int>(width_) - 2, static_cast<int>(height_) - 2);
    XDrawLine(display_, backing_, gc_, static_cast<int>(width_) - 2, 1, static_cast<int>(width_) - 2, static_cast<int>(height_) - 2);

    XSetForeground(display_, gc_, chrome_text_pixel_);
    draw_text(8, static_cast<int>(chrome_height_ > 4 ? chrome_height_ - 7 : chrome_height_ - 2), "Feel", chrome_text_pixel_);
    const std::string feel_position_text = feel_family_label(feel_index_);
    const int feel_position_x = 64;
    draw_text(feel_position_x, static_cast<int>(chrome_height_ > 4 ? chrome_height_ - 8 : chrome_height_ - 3), feel_position_text, chrome_text_pixel_);
    std::string title_text = theme_name_;
    if (title_text.size() > 22) {
      title_text = title_text.substr(0, 19) + "...";
    }
    const int title_width = text_width(title_text);
    const int title_x = std::max(64, static_cast<int>(width_) - title_width - 10);

    std::string status_text;
    if (!alternate_screen_active_ && completion_popup_open_ && !completion_items_.empty()) {
      const auto& item = completion_items_[completion_index_ % completion_items_.size()];
      status_text = "Tab: " + item.label;
    } else if (!alternate_screen_active_ && !history_hint_.empty()) {
      status_text = "Hist: " + history_hint_;
    }
    if (!status_text.empty()) {
      const int status_x = feel_position_x + text_width(feel_position_text) + 12;
      const int available = std::max(0, title_x - status_x - 12);
      if (available > 0) {
        const unsigned int max_chars = cell_w_ > 0 ? static_cast<unsigned int>(available / static_cast<int>(cell_w_)) : 0u;
        if (max_chars > 0 && status_text.size() > max_chars) {
          if (max_chars > 3) {
            status_text = status_text.substr(0, max_chars - 3) + "...";
          } else {
            status_text = status_text.substr(0, max_chars);
          }
        }
        draw_text(status_x, static_cast<int>(chrome_height_ > 4 ? chrome_height_ - 8 : chrome_height_ - 3), status_text, chrome_text_pixel_);
      }
    }

    XSetForeground(display_, gc_, chrome_shadow_pixel_);
    draw_text(title_x + 1, static_cast<int>(chrome_height_ > 4 ? chrome_height_ - 7 : chrome_height_ - 2), title_text, chrome_shadow_pixel_);
    XSetForeground(display_, gc_, chrome_text_pixel_);
    draw_text(title_x, static_cast<int>(chrome_height_ > 4 ? chrome_height_ - 8 : chrome_height_ - 3), title_text, chrome_text_pixel_);

    const std::size_t history_count = history_rows_.size();
    const std::size_t offset = std::min(scrollback_offset_, history_count);
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(history_count) - static_cast<std::ptrdiff_t>(offset);

    for (unsigned int row = 0; row < rows_; ++row) {
      const std::ptrdiff_t source_index = start + static_cast<std::ptrdiff_t>(row);
      const std::vector<Cell>* source = nullptr;
      std::size_t source_row = 0;
      std::size_t source_cols = cols_;
      if (source_index >= 0 && static_cast<std::size_t>(source_index) < history_count) {
        source = &history_rows_[static_cast<std::size_t>(source_index)];
        source_cols = source->size();
      } else {
        const std::ptrdiff_t current_row = source_index - static_cast<std::ptrdiff_t>(history_count);
        if (current_row >= 0 && current_row < static_cast<std::ptrdiff_t>(rows_)) {
          source = &cells_;
          source_row = static_cast<std::size_t>(current_row);
          source_cols = cols_;
        }
      }

      const int baseline = content_y_offset + static_cast<int>(row * cell_h_ + ascent_);
      for (unsigned int col = 0; col < cols_; ++col) {
        Cell cell{};
        if (source) {
          if (source == &cells_) {
            if (col < source_cols) {
              cell = cells_[source_row * cols_ + col];
            }
          } else if (col < source_cols) {
            cell = (*source)[col];
          }
        }
        const bool selected = selection_.active && cell_selected(row, col);
        const bool inverse = selected || cell.inverse;
        const unsigned long fg_pixel = inverse ? cell.bg : cell.fg;
        const unsigned long bg_pixel = cell.text.empty()
            ? background_pixel_
            : (inverse ? cell.fg : cell.bg);
        const int x = static_cast<int>(col * cell_w_);
        XSetForeground(display_, gc_, bg_pixel);
        XFillRectangle(display_, backing_, gc_, x, content_y_offset + static_cast<int>(row * cell_h_), cell_w_, cell_h_);
        if (!cell.continuation && !cell.text.empty()) {
          draw_text(x, baseline, cell.text, fg_pixel);
        }
      }
    }

    const int cursor_x_px = cursor_x_ * static_cast<int>(cell_w_);
    const int cursor_y_px = content_y_offset + cursor_y_ * static_cast<int>(cell_h_);
    if (scrollback_offset_ == 0 && cursor_visible_ && focused_ && cursor_x_ >= 0 && cursor_y_ >= 0) {
      const Cell& cell = cells_[static_cast<std::size_t>(std::clamp(cursor_y_, 0, static_cast<int>(rows_) - 1)) * cols_
                                     + static_cast<std::size_t>(std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1))];
      XSetForeground(display_, gc_, cursor_pixel_);
      XFillRectangle(display_, backing_, gc_, cursor_x_px, cursor_y_px, cell_w_, cell_h_);
      XSetForeground(display_, gc_, background_pixel_);
      XDrawRectangle(display_, backing_, gc_, cursor_x_px, cursor_y_px, cell_w_ - 1, cell_h_ - 1);
      if (!cell.continuation && !cell.text.empty()) {
        draw_text(cursor_x_px, cursor_y_px + static_cast<int>(ascent_), cell.text, cell.bg);
      }
    }

    XCopyArea(display_, backing_, window_, gc_, 0, 0, width_, height_, 0, 0);
    XFlush(display_);
  }

  void persist_feel_index() {
    config_.terminal_feel = feel_index_;
    if (!config_path_.empty()) {
      try {
        config_.save(config_path_);
      } catch (const std::exception& ex) {
        chiux::log::warn(std::string("chiux-te failed to save feel: ") + ex.what());
      }
    }
  }

  void apply_feel_index(unsigned int index, bool persist) {
    const unsigned long old_default_fg = foreground_pixel_;
    const unsigned long old_default_bg = background_pixel_;
    feel_index_ = clamp_feel_index(index);
    feel_theme_ = feel_theme_for_index(feel_index_);
    theme_name_ = feel_theme_.name;
    apply_theme_to_pixels(feel_theme_);
    remap_default_pixels(old_default_fg, old_default_bg, foreground_pixel_, background_pixel_);
    if (persist) {
      persist_feel_index();
    }
    redraw();
  }

  bool feel_button_hit(int x, int y) const {
    return y >= 0 && y < static_cast<int>(chrome_height_) && x >= 4 && x <= 56;
  }

  void remap_default_pixels(unsigned long old_fg, unsigned long old_bg, unsigned long new_fg, unsigned long new_bg) {
    auto remap_cell = [&](Cell& cell) {
      if (cell.fg == old_fg) {
        cell.fg = new_fg;
      }
      if (cell.bg == old_bg) {
        cell.bg = new_bg;
      }
    };
    for (auto& cell : cells_) {
      remap_cell(cell);
    }
    for (auto& row : history_rows_) {
      for (auto& cell : row) {
        remap_cell(cell);
      }
    }
    for (auto& cell : alternate_screen_state_.cells) {
      remap_cell(cell);
    }
    if (current_fg_pixel_ == old_fg) {
      current_fg_pixel_ = new_fg;
    }
    if (current_bg_pixel_ == old_bg) {
      current_bg_pixel_ = new_bg;
    }
    if (saved_fg_pixel_ == old_fg) {
      saved_fg_pixel_ = new_fg;
    }
    if (saved_bg_pixel_ == old_bg) {
      saved_bg_pixel_ = new_bg;
    }
    if (alternate_screen_state_.current_fg_pixel == old_fg) {
      alternate_screen_state_.current_fg_pixel = new_fg;
    }
    if (alternate_screen_state_.current_bg_pixel == old_bg) {
      alternate_screen_state_.current_bg_pixel = new_bg;
    }
  }

  void handle_expose(const XExposeEvent&) {
    redraw();
  }

  void handle_configure(const XConfigureEvent& event) {
    if (event.width == static_cast<int>(width_) && event.height == static_cast<int>(height_)) {
      return;
    }
    resize(static_cast<unsigned int>(std::max(1, event.width)), static_cast<unsigned int>(std::max(1, event.height)));
  }

  void handle_client_message(const XClientMessageEvent& event) {
    if (event.message_type == wm_protocols_ && static_cast<Atom>(event.data.l[0]) == wm_delete_window_) {
      running_ = false;
    }
  }

  void handle_focus_in() {
    focused_ = true;
    cursor_visible_ = true;
    next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);
    redraw();
  }

  void handle_focus_out() {
    focused_ = false;
    cursor_visible_ = false;
    redraw();
  }

  void handle_button_press(const XButtonEvent& event) {
    const int content_y = std::max(0, event.y - static_cast<int>(chrome_height_));

    if (event.y < static_cast<int>(chrome_height_)) {
      if (feel_button_hit(event.x, event.y)) {
        if (event.button == Button3) {
          apply_feel_index((feel_index_ + kFeelThemeCount - 1u) % kFeelThemeCount, true);
        } else {
          apply_feel_index((feel_index_ + 1u) % kFeelThemeCount, true);
        }
        redraw();
        return;
      }
    }

    if (event.button == Button4) {
      if (mouse_reporting_) {
        send_mouse_report(64, event.x, content_y, false, false);
        return;
      }
      scroll_scrollback(3);
      return;
    }
    if (event.button == Button5) {
      if (mouse_reporting_) {
        send_mouse_report(65, event.x, content_y, false, false);
        return;
      }
      scroll_scrollback(-3);
      return;
    }
    if (mouse_reporting_ && (event.state & ShiftMask) == 0 && (event.button == Button1 || event.button == Button2 || event.button == Button3)) {
      send_mouse_report(event.button == Button1 ? 0 : event.button == Button2 ? 1 : 2, event.x, content_y, false, false);
      return;
    }
    if (event.button == Button2) {
      request_primary_paste();
      return;
    }

    clear_completion_state();

    if (event.button == Button1) {
      selecting_ = true;
      selection_.active = false;
      const std::size_t max_col = cols_ > 0 ? static_cast<std::size_t>(cols_ - 1) : 0u;
      const std::size_t max_row = rows_ > 0 ? static_cast<std::size_t>(rows_ - 1) : 0u;
      selection_.start_col = std::clamp(static_cast<std::size_t>(event.x / static_cast<int>(cell_w_)), std::size_t{0}, max_col);
      selection_.start_row = std::clamp(static_cast<std::size_t>(content_y / static_cast<int>(cell_h_)), std::size_t{0}, max_row);
      selection_.end_col = selection_.start_col;
      selection_.end_row = selection_.start_row;
      XGrabPointer(display_, window_, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
      redraw();
      return;
    }

    focused_ = true;
    cursor_visible_ = true;
    next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);
    redraw();
  }

  void handle_button_release(const XButtonEvent& event) {
    const int content_y = std::max(0, event.y - static_cast<int>(chrome_height_));
    if (mouse_reporting_ && (event.state & ShiftMask) == 0 && (event.button == Button1 || event.button == Button2 || event.button == Button3)) {
      send_mouse_report(event.button == Button1 ? 0 : event.button == Button2 ? 1 : 2, event.x, content_y, true, false);
      return;
    }
    if (event.button == Button1 && selecting_) {
      update_selection(event.x, event.y);
      finalize_selection();
      XUngrabPointer(display_, CurrentTime);
      return;
    }
    if (event.button == Button2) {
      return;
    }
  }

  void handle_motion_notify(const XMotionEvent& event) {
    if (mouse_reporting_ && (event.state & ShiftMask) == 0 && (mouse_motion_reporting_ || (event.state & (Button1Mask | Button2Mask | Button3Mask)) != 0)) {
      const int content_y = std::max(0, event.y - static_cast<int>(chrome_height_));
      int button = 0;
      if ((event.state & Button1Mask) != 0) {
        button = 0;
      } else if ((event.state & Button2Mask) != 0) {
        button = 1;
      } else if ((event.state & Button3Mask) != 0) {
        button = 2;
      } else {
        button = 3;
      }
      send_mouse_report(button, event.x, content_y, false, true);
      return;
    }
    if (selecting_) {
      update_selection(event.x, event.y);
    }
  }

  void handle_selection_request(const XSelectionRequestEvent& event) {
    XSelectionEvent response{};
    response.type = SelectionNotify;
    response.display = event.display;
    response.requestor = event.requestor;
    response.selection = event.selection;
    response.target = event.target;
    response.property = None;
    response.time = event.time;

    const bool have_text = !selection_text_.empty();
    const auto send_property = [&](Atom target, Atom property, const unsigned char* data, int format, std::size_t length) {
      XChangeProperty(display_, event.requestor, property, target, format, PropModeReplace, data, static_cast<int>(length));
      response.property = property;
    };

    if (event.target == targets_atom_) {
      Atom targets[] = {targets_atom_, utf8_string_atom_, text_atom_, XA_STRING};
      send_property(event.target, event.property == None ? event.target : event.property,
                    reinterpret_cast<const unsigned char*>(targets), 32, std::size_t{4});
    } else if ((event.target == utf8_string_atom_ || event.target == text_atom_ || event.target == XA_STRING) && have_text) {
      send_property(event.target, event.property == None ? event.target : event.property,
                    reinterpret_cast<const unsigned char*>(selection_text_.data()), 8, selection_text_.size());
    }

    XEvent reply{};
    reply.xselection = response;
    XSendEvent(display_, event.requestor, False, 0, &reply);
  }

  void handle_selection_clear(const XSelectionClearEvent&) {
    selection_.active = false;
    selection_text_.clear();
    selecting_ = false;
    redraw();
  }

  void handle_selection_notify(const XSelectionEvent& event) {
    if (event.property == None) {
      return;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display_, window_, paste_property_, 0, 4096, True, AnyPropertyType,
                           &actual_type, &actual_format, &item_count, &bytes_after, &data) == Success && data) {
      pasted_data_.assign(reinterpret_cast<char*>(data), reinterpret_cast<char*>(data) + item_count);
      XFree(data);
      insert_paste_data();
    } else if (data) {
      XFree(data);
    }
  }

  void tick_cursor_blink() {
    if (!focused_) {
      return;
    }
    if (scrollback_offset_ != 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_cursor_toggle_) {
      return;
    }
    cursor_visible_ = !cursor_visible_;
    next_cursor_toggle_ = now + std::chrono::milliseconds(525);
    redraw();
  }

  void handle_key_press(const XKeyEvent& event) {
    KeySym sym = NoSymbol;
    char buffer[64] = {};
    const int length = XLookupString(const_cast<XKeyEvent*>(&event), buffer, sizeof(buffer), &sym, nullptr);
    const bool tui_mode = alternate_screen_active_;

    if (scrollback_offset_ != 0) {
      scrollback_offset_ = 0;
      redraw();
    }

    if (!tui_mode && completion_popup_open_ && sym == XK_Escape) {
      clear_completion_state();
      redraw();
      return;
    }

    if (!tui_mode && completion_popup_open_ && sym == XK_Tab) {
      if (!completion_items_.empty()) {
        completion_index_ = (completion_index_ + 1) % completion_items_.size();
        redraw();
        return;
      }
    }

    if (!tui_mode && completion_popup_open_ && (sym == XK_Return || sym == XK_KP_Enter)) {
      accept_completion();
      commit_current_line_history();
      send_bytes("\r", 1);
      editor_.text.clear();
      editor_.cursor = 0;
      clear_completion_state();
      cursor_visible_ = true;
      next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);
      redraw();
      return;
    }

    if ((event.state & ControlMask) != 0 && (event.state & ShiftMask) != 0) {
      if (sym == XK_C || sym == XK_c) {
        if (!selection_text_.empty()) {
          XSetSelectionOwner(display_, XA_PRIMARY, window_, CurrentTime);
          if (clipboard_atom_ != None) {
            XSetSelectionOwner(display_, clipboard_atom_, window_, CurrentTime);
          }
          return;
        }
      }
      if (sym == XK_V || sym == XK_v) {
        request_primary_paste();
        return;
      }
    }

    if ((event.state & ControlMask) != 0 && (event.state & ShiftMask) == 0) {
      if (tui_mode && length > 0) {
        send_bytes(buffer, static_cast<std::size_t>(length));
        return;
      }
      switch (sym) {
        case XK_A:
        case XK_a:
          if (editor_.cursor > 0) {
            editor_.cursor = 0;
            send_bytes("\x01", 1);
            clear_completion_state();
            redraw();
            return;
          }
          break;
        case XK_E:
        case XK_e:
          if (editor_.cursor < editor_.text.size()) {
            editor_.cursor = editor_.text.size();
            send_bytes("\x05", 1);
            clear_completion_state();
            redraw();
            return;
          }
          break;
        case XK_U:
        case XK_u:
          if (editor_.cursor > 0) {
            editor_.text.erase(0, editor_.cursor);
            editor_.cursor = 0;
            send_bytes("\x15", 1);
            clear_completion_state();
            redraw();
            return;
          }
          break;
        case XK_K:
        case XK_k:
          if (editor_.cursor < editor_.text.size()) {
            editor_.text.erase(editor_.cursor);
            send_bytes("\x0b", 1);
            clear_completion_state();
            redraw();
            return;
          }
          break;
        default:
          break;
      }
    }

    cursor_visible_ = true;
    next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);

    auto send_char = [&](char ch) {
      send_bytes(&ch, 1);
    };

    auto send_cursor_sequence = [&](const char* normal, const char* application) {
      const bool application_mode = tui_mode || application_cursor_keys_ || application_keypad_mode_;
      const char* seq = application_mode ? application : normal;
      send_bytes(seq, std::strlen(seq));
    };

    const bool special_keysym =
        sym == XK_Return || sym == XK_KP_Enter ||
        sym == XK_BackSpace || sym == XK_Tab || sym == XK_Escape ||
        sym == XK_Left || sym == XK_KP_Left ||
        sym == XK_Right || sym == XK_KP_Right ||
        sym == XK_Up || sym == XK_KP_Up ||
        sym == XK_Down || sym == XK_KP_Down ||
        sym == XK_Home || sym == XK_KP_Home ||
        sym == XK_End || sym == XK_KP_End ||
        sym == XK_Delete || sym == XK_KP_Delete ||
        sym == XK_Page_Up || sym == XK_KP_Page_Up ||
        sym == XK_Page_Down || sym == XK_KP_Page_Down;

    if ((event.state & Mod1Mask) != 0 && length > 0 && !special_keysym) {
      send_char('\x1b');
      send_bytes(buffer, static_cast<std::size_t>(length));
      if (!tui_mode) {
        clear_completion_state();
        redraw();
      }
      return;
    }

    if ((event.state & ControlMask) != 0 && length > 0 && !special_keysym) {
      send_bytes(buffer, static_cast<std::size_t>(length));
      if (!tui_mode) {
        clear_completion_state();
        redraw();
      }
      return;
    }

    if (length > 0 && !special_keysym) {
      if (tui_mode) {
        send_bytes(buffer, static_cast<std::size_t>(length));
        return;
      }
      insert_text_at_cursor(std::string(buffer, static_cast<std::size_t>(length)));
      return;
    }

    switch (sym) {
      case XK_Return:
      case XK_KP_Enter:
        if (!tui_mode) {
          commit_current_line_history();
        }
        if (application_keypad_mode_ && sym == XK_KP_Enter) {
          send_bytes("\x1bOM", 3);
        } else {
          send_char('\r');
        }
        if (!tui_mode) {
          editor_.text.clear();
          editor_.cursor = 0;
          clear_completion_state();
        }
        break;
      case XK_BackSpace:
        if (tui_mode) {
          send_bytes("\b", 1);
        } else {
          erase_previous_character();
        }
        break;
      case XK_Tab:
        {
          if (tui_mode) {
            send_char('\t');
            break;
          }
          const bool was_open = completion_popup_open_;
        refresh_completion_state();
        if (completion_popup_open_ && !completion_items_.empty()) {
          if (was_open) {
            completion_index_ = (completion_index_ + 1) % completion_items_.size();
          }
          redraw();
        } else {
          send_char('\t');
        }
        }
        break;
      case XK_Escape:
        send_char('\x1b');
        if (!tui_mode) {
          clear_completion_state();
        }
        break;
      case XK_Left:
        if (!tui_mode) {
          move_cursor_left();
        }
        send_cursor_sequence("\x1b[D", "\x1bOD");
        break;
      case XK_KP_Left:
        if (!tui_mode) {
          move_cursor_left();
        }
        send_cursor_sequence("\x1b[D", "\x1bOD");
        break;
      case XK_Right:
        if (!tui_mode) {
          move_cursor_right();
        }
        send_cursor_sequence("\x1b[C", "\x1bOC");
        break;
      case XK_KP_Right:
        if (!tui_mode) {
          move_cursor_right();
        }
        send_cursor_sequence("\x1b[C", "\x1bOC");
        break;
      case XK_Up:
        if (!tui_mode) {
          clear_completion_state();
          editor_.text.clear();
          editor_.cursor = 0;
        }
        send_cursor_sequence("\x1b[A", "\x1bOA");
        break;
      case XK_KP_Up:
        if (!tui_mode) {
          clear_completion_state();
          editor_.text.clear();
          editor_.cursor = 0;
        }
        send_cursor_sequence("\x1b[A", "\x1bOA");
        break;
      case XK_Down:
        if (!tui_mode) {
          clear_completion_state();
          editor_.text.clear();
          editor_.cursor = 0;
        }
        send_cursor_sequence("\x1b[B", "\x1bOB");
        break;
      case XK_KP_Down:
        if (!tui_mode) {
          clear_completion_state();
          editor_.text.clear();
          editor_.cursor = 0;
        }
        send_cursor_sequence("\x1b[B", "\x1bOB");
        break;
      case XK_Home:
        if (!tui_mode) {
          move_cursor_home();
        }
        send_cursor_sequence("\x1b[H", "\x1bOH");
        break;
      case XK_KP_Home:
        if (!tui_mode) {
          move_cursor_home();
        }
        send_cursor_sequence("\x1b[H", "\x1bOH");
        break;
      case XK_End:
        if (!tui_mode) {
          move_cursor_end();
        }
        send_cursor_sequence("\x1b[F", "\x1bOF");
        break;
      case XK_KP_End:
        if (!tui_mode) {
          move_cursor_end();
        }
        send_cursor_sequence("\x1b[F", "\x1bOF");
        break;
      case XK_Delete:
        if (tui_mode) {
          send_bytes("\x1b[3~", 4);
        } else {
          erase_next_character();
        }
        break;
      case XK_KP_Delete:
        if (tui_mode) {
          send_bytes("\x1b[3~", 4);
        } else {
          erase_next_character();
        }
        break;
      case XK_Page_Up:
        send_bytes("\x1b[5~", 4);
        break;
      case XK_KP_Page_Up:
        send_bytes("\x1b[5~", 4);
        break;
      case XK_Page_Down:
        send_bytes("\x1b[6~", 4);
        break;
      case XK_KP_Page_Down:
        send_bytes("\x1b[6~", 4);
        break;
      default:
        break;
    }
    if (!tui_mode) {
      refresh_completion_state();
    }
  }

  void reset_screen() {
    for (auto& cell : cells_) {
      cell = Cell{};
    }
    cursor_x_ = 0;
    cursor_y_ = 0;
    current_fg_pixel_ = foreground_pixel_;
    current_bg_pixel_ = background_pixel_;
    current_bold_ = false;
    current_inverse_ = false;
    g0_special_graphics_ = false;
    g1_special_graphics_ = false;
    use_g1_charset_ = false;
    application_cursor_keys_ = false;
    application_keypad_mode_ = false;
    reset_scroll_region();
    state_ = ParseState::Ground;
    csi_params_.clear();
    csi_current_ = 0;
    csi_have_value_ = false;
    utf8_pending_.clear();
    utf8_expected_ = 0;
    osc_buffer_.clear();
    osc_seen_escape_ = false;
    redraw();
  }

  bool cell_selected(unsigned int row, unsigned int col) const {
    if (!selection_.active) {
      return false;
    }
    const std::size_t start_row = selection_.start_row;
    const std::size_t end_row = selection_.end_row;
    const std::size_t start_col = selection_.start_col;
    const std::size_t end_col = selection_.end_col;
    const std::size_t top_row = std::min(start_row, end_row);
    const std::size_t bottom_row = std::max(start_row, end_row);
    if (row < top_row || row > bottom_row) {
      return false;
    }
    if (top_row == bottom_row) {
      const std::size_t left_col = std::min(start_col, end_col);
      const std::size_t right_col = std::max(start_col, end_col);
      return col >= left_col && col <= right_col;
    }
    if (row == top_row) {
      return col >= start_col;
    }
    if (row == bottom_row) {
      return col <= end_col;
    }
    return true;
  }

  int text_width(const std::string& text) const {
    return font_ ? XTextWidth(font_, text.c_str(), static_cast<int>(text.size()))
                 : static_cast<int>(text.size()) * 6;
  }

  void draw_text(int x, int y, const std::string& text, unsigned long pixel) {
    if (text.empty()) {
      return;
    }
    XSetForeground(display_, gc_, pixel);
    if (font_) {
      XDrawString(display_, backing_, gc_, x, y, text.c_str(), static_cast<int>(text.size()));
    } else {
      XDrawString(display_, backing_, gc_, x, y, text.c_str(), static_cast<int>(text.size()));
    }
  }

  Display* display_ = nullptr;
  int screen_ = 0;
  Window window_ = 0;
  GC gc_ = 0;
  Pixmap backing_ = 0;
  XFontStruct* font_ = nullptr;
  Atom wm_delete_window_ = 0;
  Atom wm_protocols_ = 0;
  Atom clipboard_atom_ = None;
  Atom utf8_string_atom_ = None;
  Atom targets_atom_ = None;
  Atom text_atom_ = None;
  Atom string_atom_ = None;
  Atom paste_property_ = None;
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int cols_ = 0;
  unsigned int rows_ = 0;
  unsigned int old_cols_ = 0;
  unsigned int old_rows_ = 0;
  unsigned int cell_w_ = 8;
  unsigned int cell_h_ = 16;
  unsigned int ascent_ = 12;
  unsigned int descent_ = 4;
  unsigned long palette_[16]{};
  unsigned long color_table_[256]{};
  unsigned long background_pixel_ = 0;
  unsigned long foreground_pixel_ = 0;
  unsigned long cursor_pixel_ = 0;
  unsigned long bezel_light_pixel_ = 0;
  unsigned long bezel_shadow_pixel_ = 0;
  unsigned long bezel_edge_pixel_ = 0;
  std::vector<Cell> cells_;
  std::deque<std::vector<Cell>> history_rows_;
  std::deque<std::string> command_history_;
  LineEditor editor_{};
  std::vector<CompletionItem> completion_items_;
  std::size_t history_limit_ = 2000;
  std::size_t scrollback_offset_ = 0;
  std::size_t completion_index_ = 0;
  std::size_t completion_token_start_ = 0;
  std::string completion_prefix_;
  std::string history_hint_;
  bool completion_popup_open_ = false;
  SelectionRange selection_{};
  bool selecting_ = false;
  std::string selection_text_;
  std::string pasted_data_;
  int cursor_x_ = 0;
  int cursor_y_ = 0;
  ParseState state_ = ParseState::Ground;
  std::vector<int> csi_params_;
  int csi_current_ = 0;
  bool csi_have_value_ = false;
  bool csi_private_ = false;
  unsigned long current_fg_pixel_ = 0;
  unsigned long current_bg_pixel_ = 0;
  bool current_bold_ = false;
  bool current_inverse_ = false;
  int saved_x_ = 0;
  int saved_y_ = 0;
  unsigned long saved_fg_pixel_ = 0;
  unsigned long saved_bg_pixel_ = 0;
  bool saved_bold_ = false;
  bool saved_inverse_ = false;
  bool has_saved_ = false;
  bool bracketed_paste_mode_ = false;
  bool mouse_reporting_ = false;
  bool mouse_motion_reporting_ = false;
  bool mouse_sgr_mode_ = false;
  bool application_cursor_keys_ = false;
  bool application_keypad_mode_ = false;
  bool origin_mode_ = false;
  bool g0_special_graphics_ = false;
  bool g1_special_graphics_ = false;
  bool use_g1_charset_ = false;
  int scroll_region_top_ = 0;
  int scroll_region_bottom_ = 0;
  std::string utf8_pending_;
  unsigned int utf8_expected_ = 0;
  std::string osc_buffer_;
  bool osc_seen_escape_ = false;
  bool charset_target_g1_ = false;
  chiux::config::Config config_{};
  std::filesystem::path config_path_{};
  unsigned int feel_index_ = 0;
  FeelTheme feel_theme_{};
  std::string theme_name_;
  unsigned int chrome_height_ = kChromeHeight;
  unsigned long chrome_pixel_ = 0;
  unsigned long chrome_text_pixel_ = 0;
  unsigned long chrome_shadow_pixel_ = 0;
  unsigned long popup_fill_pixel_ = 0;
  unsigned long popup_text_pixel_ = 0;
  unsigned long popup_selected_pixel_ = 0;
  unsigned long popup_selected_text_pixel_ = 0;
  struct AlternateScreenState {
    bool active = false;
    std::vector<Cell> cells;
    unsigned int cols = 0;
    unsigned int rows = 0;
    std::deque<std::vector<Cell>> history_rows;
    std::size_t scrollback_offset = 0;
    int cursor_x = 0;
    int cursor_y = 0;
    unsigned long current_fg_pixel = 0;
    unsigned long current_bg_pixel = 0;
    bool current_bold = false;
    bool current_inverse = false;
    int scroll_region_top = 0;
    int scroll_region_bottom = 0;
    bool origin_mode = false;
    bool g0_special_graphics = false;
    bool g1_special_graphics = false;
    bool use_g1_charset = false;
  };
  AlternateScreenState alternate_screen_state_{};
  bool alternate_screen_active_ = false;
  bool focused_ = true;
  bool cursor_visible_ = true;
  std::chrono::steady_clock::time_point next_cursor_toggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(525);
  pid_t child_pid_ = -1;
  int master_fd_ = -1;
  bool running_ = true;
  std::string exec_command_;
};

}  // namespace chiux::te

int main(int argc, char** argv) {
  try {
    std::setlocale(LC_ALL, "");
    XSetLocaleModifiers("");
    chiux::te::TerminalApp app(argc, argv);
    return app.run();
  } catch (const std::exception& e) {
    chiux::log::error(e.what());
    return 1;
  }
}
