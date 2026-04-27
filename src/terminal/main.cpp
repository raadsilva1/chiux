#include "util/log.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <deque>
#include <filesystem>
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
  unsigned char ch = ' ';
  unsigned char fg = 7;
  unsigned char bg = 0;
  bool bold = false;
  bool inverse = false;
};

enum class ParseState {
  Ground,
  Escape,
  Csi,
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

unsigned long alloc_color(Display* display, Colormap colormap, const char* spec, unsigned long fallback) {
  XColor exact{};
  XColor screen{};
  if (XAllocNamedColor(display, colormap, spec, &screen, &exact)) {
    return screen.pixel;
  }
  return fallback;
}

XFontStruct* load_font(Display* display) {
  static XFontStruct* font = nullptr;
  static bool attempted = false;
  if (attempted) {
    return font;
  }
  attempted = true;
  static const char* candidates[] = {
      "10x20",
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

  void load_resources() {
    font_ = load_font(display_);
    if (font_) {
      cell_w_ = static_cast<unsigned int>(std::max(1, static_cast<int>(font_->max_bounds.width)));
      cell_h_ = static_cast<unsigned int>(font_->ascent + font_->descent);
      ascent_ = static_cast<unsigned int>(font_->ascent);
    } else {
      cell_w_ = 8;
      cell_h_ = 16;
      ascent_ = 12;
    }

    const Colormap colormap = DefaultColormap(display_, screen_);
    palette_[0] = alloc_color(display_, colormap, "#202020", BlackPixel(display_, screen_));
    palette_[1] = alloc_color(display_, colormap, "#8A3A3A", BlackPixel(display_, screen_));
    palette_[2] = alloc_color(display_, colormap, "#3A6A3A", BlackPixel(display_, screen_));
    palette_[3] = alloc_color(display_, colormap, "#8A633A", BlackPixel(display_, screen_));
    palette_[4] = alloc_color(display_, colormap, "#3A4F8A", BlackPixel(display_, screen_));
    palette_[5] = alloc_color(display_, colormap, "#6A4A6A", BlackPixel(display_, screen_));
    palette_[6] = alloc_color(display_, colormap, "#3A6A6A", BlackPixel(display_, screen_));
    palette_[7] = alloc_color(display_, colormap, "#F0F0F0", WhitePixel(display_, screen_));
    palette_[8] = alloc_color(display_, colormap, "#767676", BlackPixel(display_, screen_));
    palette_[9] = alloc_color(display_, colormap, "#C86C6C", BlackPixel(display_, screen_));
    palette_[10] = alloc_color(display_, colormap, "#6CB06C", BlackPixel(display_, screen_));
    palette_[11] = alloc_color(display_, colormap, "#D0B06C", BlackPixel(display_, screen_));
    palette_[12] = alloc_color(display_, colormap, "#6C86D0", BlackPixel(display_, screen_));
    palette_[13] = alloc_color(display_, colormap, "#B07AB0", BlackPixel(display_, screen_));
    palette_[14] = alloc_color(display_, colormap, "#6CB0B0", BlackPixel(display_, screen_));
    palette_[15] = alloc_color(display_, colormap, "#FFFFFF", WhitePixel(display_, screen_));
    background_pixel_ = alloc_color(display_, colormap, "#D8D8D8", WhitePixel(display_, screen_));
    foreground_pixel_ = alloc_color(display_, colormap, "#202020", BlackPixel(display_, screen_));
    cursor_pixel_ = alloc_color(display_, colormap, "#6C6C6C", BlackPixel(display_, screen_));
    bezel_light_pixel_ = alloc_color(display_, colormap, "#F7F7F7", WhitePixel(display_, screen_));
    bezel_shadow_pixel_ = alloc_color(display_, colormap, "#A0A0A0", BlackPixel(display_, screen_));
    bezel_edge_pixel_ = alloc_color(display_, colormap, "#B8B8B8", BlackPixel(display_, screen_));
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
    width_ = width;
    height_ = height;
    cols_ = std::max(1u, width_ / cell_w_);
    rows_ = std::max(1u, height_ / cell_h_);
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
    ws.ws_ypixel = static_cast<unsigned short>(height_);
    ioctl(master_fd_, TIOCSWINSZ, &ws);
    if (child_pid_ > 0) {
      kill(child_pid_, SIGWINCH);
    }
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
      setsid();
      const int slave_fd = open(slave_name.data(), O_RDWR);
      if (slave_fd < 0) {
        _exit(127);
      }
      ioctl(slave_fd, TIOCSCTTY, 0);
      dup2(slave_fd, STDIN_FILENO);
      dup2(slave_fd, STDOUT_FILENO);
      dup2(slave_fd, STDERR_FILENO);
      if (slave_fd > STDERR_FILENO) {
        close(slave_fd);
      }
      const char* term = "xterm";
      setenv("TERM", term, 1);
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
      running_ = false;
      return true;
    }
    if (errno != ECHILD) {
      chiux::log::warn(std::string("chiux-te waitpid failed: ") + std::strerror(errno));
    }
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
        running_ = false;
      }
      break;
    }
  }

  void process_byte(unsigned char ch) {
    switch (state_) {
      case ParseState::Ground:
        if (ch == 0x1B) {
          state_ = ParseState::Escape;
        } else if (ch == '\r') {
          cursor_x_ = 0;
        } else if (ch == '\n') {
          line_feed();
        } else if (ch == '\b' || ch == 0x7f) {
          if (cursor_x_ > 0) {
            --cursor_x_;
          }
        } else if (ch == '\t') {
          const int next_tab = ((cursor_x_ / 8) + 1) * 8;
          while (cursor_x_ < next_tab) {
            put_char(' ');
          }
        } else if (ch >= 32) {
          put_char(ch);
        }
        break;
      case ParseState::Escape:
        if (ch == '[') {
          state_ = ParseState::Csi;
          csi_params_.clear();
          csi_current_ = 0;
          csi_have_value_ = false;
          csi_private_ = false;
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
    }
  }

  void apply_csi(unsigned char final) {
    if (csi_private_) {
      csi_private_ = false;
      if (final == 'h' || final == 'l') {
        return;
      }
    }

    auto param_or = [&](std::size_t index, int default_value) {
      if (index >= csi_params_.size() || csi_params_[index] < 0) {
        return default_value;
      }
      return csi_params_[index];
    };

    switch (final) {
      case 'A':
        cursor_y_ = std::max(0, cursor_y_ - param_or(0, 1));
        break;
      case 'B':
        cursor_y_ = std::min(static_cast<int>(rows_) - 1, cursor_y_ + param_or(0, 1));
        break;
      case 'C':
        cursor_x_ = std::min(static_cast<int>(cols_) - 1, cursor_x_ + param_or(0, 1));
        break;
      case 'D':
        cursor_x_ = std::max(0, cursor_x_ - param_or(0, 1));
        break;
      case 'H':
      case 'f':
        cursor_y_ = std::clamp(param_or(0, 1) - 1, 0, static_cast<int>(rows_) - 1);
        cursor_x_ = std::clamp(param_or(1, 1) - 1, 0, static_cast<int>(cols_) - 1);
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
      current_fg_ = 7;
      current_bg_ = 0;
      current_bold_ = false;
      current_inverse_ = false;
      return;
    }
    for (int raw : csi_params_) {
      const int value = raw < 0 ? 0 : raw;
      if (value == 0) {
        current_fg_ = 7;
        current_bg_ = 0;
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
        current_fg_ = 7;
      } else if (value == 49) {
        current_bg_ = 0;
      } else if (value >= 30 && value <= 37) {
        current_fg_ = static_cast<unsigned char>(value - 30);
      } else if (value >= 40 && value <= 47) {
        current_bg_ = static_cast<unsigned char>(value - 40);
      } else if (value >= 90 && value <= 97) {
        current_fg_ = static_cast<unsigned char>(value - 90 + 8);
      } else if (value >= 100 && value <= 107) {
        current_bg_ = static_cast<unsigned char>(value - 100 + 8);
      }
    }
  }

  void save_cursor() {
    saved_x_ = cursor_x_;
    saved_y_ = cursor_y_;
    saved_fg_ = current_fg_;
    saved_bg_ = current_bg_;
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
    current_fg_ = saved_fg_;
    current_bg_ = saved_bg_;
    current_bold_ = saved_bold_;
    current_inverse_ = saved_inverse_;
  }

  void put_char(unsigned char ch) {
    if (cursor_x_ >= static_cast<int>(cols_)) {
      line_feed();
      cursor_x_ = 0;
    }
    const std::size_t index = static_cast<std::size_t>(cursor_y_) * cols_ + static_cast<std::size_t>(cursor_x_);
    if (index < cells_.size()) {
      cells_[index].ch = ch;
      cells_[index].fg = current_fg_;
      cells_[index].bg = current_bg_;
      cells_[index].bold = current_bold_;
      cells_[index].inverse = current_inverse_;
    }
    ++cursor_x_;
    if (cursor_x_ >= static_cast<int>(cols_)) {
      line_feed();
      cursor_x_ = 0;
    }
  }

  void line_feed() {
    ++cursor_y_;
    if (cursor_y_ >= static_cast<int>(rows_)) {
      scroll_up();
      cursor_y_ = static_cast<int>(rows_) - 1;
    }
  }

  void reverse_index() {
    if (cursor_y_ == 0) {
      scroll_down();
    } else {
      --cursor_y_;
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
    for (unsigned int row = static_cast<unsigned int>(std::max(0, cursor_y_ + 1)); row < rows_; ++row) {
      clear_row(row);
    }
  }

  unsigned long color_for(unsigned char index, bool bold) const {
    const unsigned char palette_index = bold && index < 8 ? static_cast<unsigned char>(index + 8) : index;
    return palette_[palette_index % 16];
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
      text.push_back(cell.ch == 0 ? ' ' : static_cast<char>(cell.ch));
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
        const char ch = cells[col].ch == 0 ? ' ' : static_cast<char>(cells[col].ch);
        out.push_back(ch);
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
    const std::size_t max_col = cols_ > 0 ? static_cast<std::size_t>(cols_ - 1) : 0u;
    const std::size_t max_row = rows_ > 0 ? static_cast<std::size_t>(rows_ - 1) : 0u;
    selection_.end_col = std::clamp(static_cast<std::size_t>(x / static_cast<int>(cell_w_)), std::size_t{0}, max_col);
    selection_.end_row = std::clamp(static_cast<std::size_t>(y / static_cast<int>(cell_h_)), std::size_t{0}, max_row);
    redraw();
  }

  void request_primary_paste() {
    if (XGetSelectionOwner(display_, XA_PRIMARY) == window_ && !selection_text_.empty()) {
      send_bytes(selection_text_.data(), selection_text_.size());
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
    send_bytes(pasted_data_.data(), pasted_data_.size());
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

  void redraw() {
    if (!display_ || !window_ || !backing_) {
      return;
    }

    XSetForeground(display_, gc_, background_pixel_);
    XFillRectangle(display_, backing_, gc_, 0, 0, width_, height_);
    XSetForeground(display_, gc_, bezel_light_pixel_);
    XFillRectangle(display_, backing_, gc_, 1, 1, width_ > 2 ? width_ - 2 : width_, height_ > 2 ? height_ - 2 : height_);
    XSetForeground(display_, gc_, bezel_edge_pixel_);
    XDrawRectangle(display_, backing_, gc_, 0, 0, width_ - 1, height_ - 1);
    XSetForeground(display_, gc_, bezel_light_pixel_);
    XDrawLine(display_, backing_, gc_, 1, 1, static_cast<int>(width_) - 2, 1);
    XDrawLine(display_, backing_, gc_, 1, 1, 1, static_cast<int>(height_) - 2);
    XSetForeground(display_, gc_, bezel_shadow_pixel_);
    XDrawLine(display_, backing_, gc_, 1, static_cast<int>(height_) - 2, static_cast<int>(width_) - 2, static_cast<int>(height_) - 2);
    XDrawLine(display_, backing_, gc_, static_cast<int>(width_) - 2, 1, static_cast<int>(width_) - 2, static_cast<int>(height_) - 2);

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

      const int baseline = static_cast<int>(row * cell_h_ + ascent_);
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
        const unsigned char fg_index = inverse ? cell.bg : cell.fg;
        const unsigned char bg_index = inverse ? cell.fg : cell.bg;
        const unsigned long fg_pixel = color_for(fg_index, cell.bold);
        const unsigned long bg_pixel = color_for(bg_index, false);
        const int x = static_cast<int>(col * cell_w_);
        XSetForeground(display_, gc_, bg_pixel);
        XFillRectangle(display_, backing_, gc_, x, static_cast<int>(row * cell_h_), cell_w_, cell_h_);
        if (cell.ch != ' ') {
          XSetForeground(display_, gc_, fg_pixel);
          const char text[1] = {static_cast<char>(cell.ch)};
          XDrawString(display_, backing_, gc_, x, baseline, text, 1);
        }
      }
    }

    const int cursor_x_px = cursor_x_ * static_cast<int>(cell_w_);
    const int cursor_y_px = cursor_y_ * static_cast<int>(cell_h_);
    if (scrollback_offset_ == 0 && cursor_visible_ && focused_ && cursor_x_ >= 0 && cursor_y_ >= 0) {
      const Cell& cell = cells_[static_cast<std::size_t>(std::clamp(cursor_y_, 0, static_cast<int>(rows_) - 1)) * cols_
                                     + static_cast<std::size_t>(std::clamp(cursor_x_, 0, static_cast<int>(cols_) - 1))];
      const unsigned char bg_index = cell.fg;
      XSetForeground(display_, gc_, cursor_pixel_);
      XFillRectangle(display_, backing_, gc_, cursor_x_px, cursor_y_px, cell_w_, cell_h_);
      XSetForeground(display_, gc_, background_pixel_);
      XDrawRectangle(display_, backing_, gc_, cursor_x_px, cursor_y_px, cell_w_ - 1, cell_h_ - 1);
      if (cell.ch != ' ') {
        XSetForeground(display_, gc_, color_for(bg_index, false));
        const char text[1] = {static_cast<char>(cell.ch)};
        XDrawString(display_, backing_, gc_, cursor_x_px, cursor_y_px + static_cast<int>(ascent_), text, 1);
      }
    }

    const unsigned int status_rows = (history_hint_.empty() ? 0u : 1u) + (completion_popup_open_ && !completion_items_.empty() ? static_cast<unsigned int>(std::min<std::size_t>(completion_items_.size(), 5)) : 0u);
    if (status_rows > 0 && rows_ > 0) {
      const unsigned int popup_rows = completion_popup_open_ && !completion_items_.empty()
                                          ? static_cast<unsigned int>(std::min<std::size_t>(completion_items_.size(), 5))
                                          : 0u;
      const unsigned int hint_row = rows_ - 1;
      const unsigned int popup_start_row = hint_row >= popup_rows + (history_hint_.empty() ? 0u : 1u)
                                               ? hint_row - popup_rows - (history_hint_.empty() ? 0u : 1u)
                                               : 0u;
      const int panel_x = 1;
      const int panel_w = static_cast<int>(std::max(1u, width_ - 2));
      const unsigned int panel_w_u = static_cast<unsigned int>(panel_w);
      const unsigned int inner_w_u = panel_w > 2 ? static_cast<unsigned int>(panel_w - 2) : 0u;
      const unsigned int edge_w_u = panel_w > 0 ? static_cast<unsigned int>(panel_w - 1) : 0u;

      if (popup_rows > 0) {
        const int panel_y = static_cast<int>(popup_start_row * cell_h_);
        const int panel_h = static_cast<int>(popup_rows * cell_h_);
        XSetForeground(display_, gc_, bezel_light_pixel_);
        XFillRectangle(display_, backing_, gc_, panel_x, panel_y, panel_w_u, static_cast<unsigned int>(panel_h));
        XSetForeground(display_, gc_, bezel_edge_pixel_);
        XDrawRectangle(display_, backing_, gc_, panel_x, panel_y, edge_w_u, static_cast<unsigned int>(panel_h > 0 ? panel_h - 1 : 0));
        XSetForeground(display_, gc_, bezel_shadow_pixel_);
        XDrawLine(display_, backing_, gc_, panel_x, panel_y + panel_h - 1, panel_x + panel_w - 1, panel_y + panel_h - 1);
        XDrawLine(display_, backing_, gc_, panel_x + panel_w - 1, panel_y, panel_x + panel_w - 1, panel_y + panel_h - 1);

        for (unsigned int i = 0; i < popup_rows; ++i) {
          const auto& item = completion_items_[i];
          const bool selected = i == completion_index_ % completion_items_.size();
          const int row_y = panel_y + static_cast<int>(i * cell_h_);
          const unsigned long bg = selected ? cursor_pixel_ : background_pixel_;
          const unsigned long fg = selected ? background_pixel_ : foreground_pixel_;
          XSetForeground(display_, gc_, bg);
          XFillRectangle(display_, backing_, gc_, panel_x + 1, row_y + 1, inner_w_u, cell_h_ - 1);
          XSetForeground(display_, gc_, fg);
          std::string label = item.label;
          const unsigned int max_chars = cell_w_ > 0 ? static_cast<unsigned int>(std::max(0, panel_w - 4) / static_cast<int>(cell_w_)) : 0u;
          if (max_chars > 0 && label.size() > max_chars) {
            if (max_chars > 3) {
              label = label.substr(0, max_chars - 3) + "...";
            } else {
              label = label.substr(0, max_chars);
            }
          }
          XDrawString(display_, backing_, gc_, panel_x + 6, row_y + static_cast<int>(ascent_), label.c_str(), static_cast<int>(label.size()));
        }
      }

      if (!history_hint_.empty()) {
        const int row_y = static_cast<int>(hint_row * cell_h_);
        XSetForeground(display_, gc_, bezel_light_pixel_);
        XFillRectangle(display_, backing_, gc_, panel_x, row_y, panel_w_u, cell_h_);
        XSetForeground(display_, gc_, bezel_edge_pixel_);
        XDrawRectangle(display_, backing_, gc_, panel_x, row_y, edge_w_u, cell_h_ - 1);
        XSetForeground(display_, gc_, foreground_pixel_);
        std::string hint = "history: " + history_hint_;
        const unsigned int max_chars = cell_w_ > 0 ? static_cast<unsigned int>(std::max(0, panel_w - 12) / static_cast<int>(cell_w_)) : 0u;
        if (max_chars > 0 && hint.size() > max_chars) {
          if (max_chars > 3) {
            hint = hint.substr(0, max_chars - 3) + "...";
          } else {
            hint = hint.substr(0, max_chars);
          }
        }
        XDrawString(display_, backing_, gc_, panel_x + 6, row_y + static_cast<int>(ascent_), hint.c_str(), static_cast<int>(hint.size()));
      }
    }

    XCopyArea(display_, backing_, window_, gc_, 0, 0, width_, height_, 0, 0);
    XFlush(display_);
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
    if (event.button == Button4) {
      scroll_scrollback(3);
      return;
    }
    if (event.button == Button5) {
      scroll_scrollback(-3);
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
      selection_.start_row = std::clamp(static_cast<std::size_t>(event.y / static_cast<int>(cell_h_)), std::size_t{0}, max_row);
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

    if (scrollback_offset_ != 0) {
      scrollback_offset_ = 0;
      redraw();
    }

    if (completion_popup_open_ && sym == XK_Escape) {
      clear_completion_state();
      redraw();
      return;
    }

    if (completion_popup_open_ && sym == XK_Tab) {
      if (!completion_items_.empty()) {
        completion_index_ = (completion_index_ + 1) % completion_items_.size();
        redraw();
        return;
      }
    }

    if (completion_popup_open_ && (sym == XK_Return || sym == XK_KP_Enter)) {
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

    if ((event.state & Mod1Mask) != 0 && length > 0) {
      send_char('\x1b');
      send_bytes(buffer, static_cast<std::size_t>(length));
      clear_completion_state();
      redraw();
      return;
    }

    if ((event.state & ControlMask) != 0 && length > 0) {
      send_bytes(buffer, static_cast<std::size_t>(length));
      clear_completion_state();
      redraw();
      return;
    }

    if (length > 0) {
      insert_text_at_cursor(std::string(buffer, static_cast<std::size_t>(length)));
      return;
    }

    switch (sym) {
      case XK_Return:
      case XK_KP_Enter:
        commit_current_line_history();
        send_char('\r');
        editor_.text.clear();
        editor_.cursor = 0;
        clear_completion_state();
        break;
      case XK_BackSpace:
        erase_previous_character();
        break;
      case XK_Tab:
        {
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
        clear_completion_state();
        break;
      case XK_Left:
        move_cursor_left();
        break;
      case XK_Right:
        move_cursor_right();
        break;
      case XK_Up:
        clear_completion_state();
        editor_.text.clear();
        editor_.cursor = 0;
        send_bytes("\x1b[A", 3);
        break;
      case XK_Down:
        clear_completion_state();
        editor_.text.clear();
        editor_.cursor = 0;
        send_bytes("\x1b[B", 3);
        break;
      case XK_Home:
        move_cursor_home();
        break;
      case XK_End:
        move_cursor_end();
        break;
      case XK_Delete:
        erase_next_character();
        break;
      case XK_Page_Up:
        send_bytes("\x1b[5~", 4);
        break;
      case XK_Page_Down:
        send_bytes("\x1b[6~", 4);
        break;
      default:
        break;
    }
    refresh_completion_state();
  }

  void reset_screen() {
    for (auto& cell : cells_) {
      cell = Cell{};
    }
    cursor_x_ = 0;
    cursor_y_ = 0;
    current_fg_ = 7;
    current_bg_ = 0;
    current_bold_ = false;
    current_inverse_ = false;
    state_ = ParseState::Ground;
    csi_params_.clear();
    csi_current_ = 0;
    csi_have_value_ = false;
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
  unsigned long palette_[16]{};
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
  unsigned char current_fg_ = 7;
  unsigned char current_bg_ = 0;
  bool current_bold_ = false;
  bool current_inverse_ = false;
  int saved_x_ = 0;
  int saved_y_ = 0;
  unsigned char saved_fg_ = 7;
  unsigned char saved_bg_ = 0;
  bool saved_bold_ = false;
  bool saved_inverse_ = false;
  bool has_saved_ = false;
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
    chiux::te::TerminalApp app(argc, argv);
    return app.run();
  } catch (const std::exception& e) {
    chiux::log::error(e.what());
    return 1;
  }
}
