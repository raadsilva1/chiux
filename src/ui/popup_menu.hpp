#pragma once

#include <X11/Xlib.h>

#include <optional>
#include <string>
#include <vector>

namespace chiux::ui {

struct PopupMenuItem {
  std::string label;
  std::string action;
};

class PopupMenu {
public:
  PopupMenu(Display* display, Window root, unsigned int item_width = 220, unsigned int item_height = 20);
  ~PopupMenu();

  PopupMenu(const PopupMenu&) = delete;
  PopupMenu& operator=(const PopupMenu&) = delete;

  Window window() const { return window_; }
  bool visible() const { return visible_; }
  void show(int x, int y, std::vector<PopupMenuItem> items);
  void hide();
  void set_active_index(std::optional<std::size_t> index);
  std::optional<std::size_t> hit_test(int x, int y) const;
  void draw();
  const std::vector<PopupMenuItem>& items() const { return items_; }
  std::optional<std::size_t> active_index() const { return active_index_; }

private:
  Display* display_ = nullptr;
  Window root_ = 0;
  Window window_ = 0;
  GC gc_ = nullptr;
  unsigned int width_ = 0;
  unsigned int item_height_ = 0;
  std::vector<PopupMenuItem> items_;
  std::optional<std::size_t> active_index_;
  bool visible_ = false;
};

}
