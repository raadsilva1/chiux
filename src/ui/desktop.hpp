#pragma once

#include <X11/Xlib.h>

#include <optional>
#include <string>
#include <vector>

namespace chiux::ui {

struct DesktopIcon {
  std::string label;
  std::string command;
  int x = 0;
  int y = 0;
  unsigned int width = 56;
  unsigned int height = 42;
  bool selected = false;
};

class Desktop {
public:
  Desktop(Display* display, Window root, unsigned int width, unsigned int height, unsigned int top_offset);
  ~Desktop();

  Desktop(const Desktop&) = delete;
  Desktop& operator=(const Desktop&) = delete;

  Window window() const { return window_; }
  void resize(unsigned int width, unsigned int height);
  void set_icons(std::vector<DesktopIcon> icons);
  void set_icon_colors(unsigned long text_pixel, unsigned long shadow_pixel);
  const std::vector<DesktopIcon>& icons() const { return icons_; }
  std::vector<DesktopIcon>& icons() { return icons_; }
  void set_background_pixel(unsigned long pixel);
  std::optional<std::size_t> hit_test(int x, int y) const;
  void select_icon(std::optional<std::size_t> index);
  void move_icon(std::size_t index, int x, int y);
  void arrange_icons();
  void draw();

private:
  void draw_icon(Drawable target, const DesktopIcon& icon);

  Display* display_ = nullptr;
  Window window_ = 0;
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int top_offset_ = 0;
  unsigned long background_pixel_ = 0;
  unsigned long icon_text_pixel_ = 0;
  unsigned long icon_shadow_pixel_ = 0;
  GC gc_ = nullptr;
  Pixmap backing_ = 0;
  std::vector<DesktopIcon> icons_;
  std::optional<std::size_t> selected_icon_;
};

}
