#pragma once

#include <X11/Xlib.h>

#include <optional>
#include <string>
#include <vector>

namespace chiux::ui {

enum class MenuId {
  Apple = 0,
  File,
  Edit,
  View,
  Window,
};

class MenuBar {
public:
  MenuBar(Display* display, Window root, unsigned int width, unsigned int height);
  ~MenuBar();

  MenuBar(const MenuBar&) = delete;
  MenuBar& operator=(const MenuBar&) = delete;

  Window window() const { return window_; }
  void resize(unsigned int width);
  void set_active(std::optional<MenuId> menu);
  void set_application_title(std::string title);
  std::optional<MenuId> hit_test(int x, int y) const;
  void draw();

private:
  struct MenuLabel {
    MenuId id;
    std::string text;
    int x;
    int width;
  };

  void layout_labels() const;

  Display* display_ = nullptr;
  Window window_ = 0;
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  GC gc_ = nullptr;
  std::optional<MenuId> active_menu_;
  std::string application_title_ = "chiux";
  mutable std::vector<MenuLabel> labels_;
};

}
