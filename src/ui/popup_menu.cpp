#include "ui/popup_menu.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <utility>

namespace chiux::ui {

PopupMenu::PopupMenu(Display* display, Window root, unsigned int item_width, unsigned int item_height)
  : display_(display), root_(root), width_(item_width), item_height_(item_height) {
  XSetWindowAttributes attrs{};
  attrs.override_redirect = True;
  attrs.background_pixel = Theme{}.menu_pixel;
  attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask;
  window_ = XCreateWindow(display_, root_, 0, 0, width_, item_height_, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
  gc_ = XCreateGC(display_, window_, 0, nullptr);
  XSetForeground(display_, gc_, Theme{}.menu_text_pixel);
  XSetBackground(display_, gc_, Theme{}.menu_pixel);
}

PopupMenu::~PopupMenu() {
  if (gc_) {
    XFreeGC(display_, gc_);
  }
  if (window_) {
    XDestroyWindow(display_, window_);
  }
}

void PopupMenu::show(int x, int y, std::vector<PopupMenuItem> items) {
  items_ = std::move(items);
  active_index_ = items_.empty() ? std::nullopt : std::optional<std::size_t>(0);
  const unsigned int height = std::max<unsigned int>(item_height_, static_cast<unsigned int>(items_.size()) * item_height_);
  const int screen = DefaultScreen(display_);
  const int screen_width = DisplayWidth(display_, screen);
  const int screen_height = DisplayHeight(display_, screen);
  const int clamped_x = std::max(0, std::min(x, screen_width - static_cast<int>(width_)));
  const int clamped_y = std::max(0, std::min(y, screen_height - static_cast<int>(height)));
  XMoveResizeWindow(display_, window_, clamped_x, clamped_y, width_, height);
  XMapRaised(display_, window_);
  XGrabPointer(display_, window_, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  visible_ = true;
  draw();
}

void PopupMenu::hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  active_index_.reset();
  XUngrabPointer(display_, CurrentTime);
  XUnmapWindow(display_, window_);
  items_.clear();
}

void PopupMenu::set_active_index(std::optional<std::size_t> index) {
  if (!visible_) {
    return;
  }
  if (index && *index >= items_.size()) {
    index.reset();
  }
  if (active_index_ == index) {
    return;
  }
  active_index_ = index;
  draw();
}

std::optional<std::size_t> PopupMenu::hit_test(int x, int y) const {
  if (!visible_ || x < 0 || y < 0) {
    return std::nullopt;
  }
  const unsigned int index = static_cast<unsigned int>(y / static_cast<int>(item_height_));
  if (index >= items_.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(index);
}

void PopupMenu::draw() {
  if (!visible_) {
    return;
  }
  XClearWindow(display_, window_);
  XSetForeground(display_, gc_, Theme{}.menu_pixel);
  XFillRectangle(display_, window_, gc_, 0, 0, width_, static_cast<unsigned int>(items_.size()) * item_height_);
  XSetForeground(display_, gc_, Theme{}.shadow_pixel);
  XDrawLine(display_, window_, gc_, 0, 0, static_cast<int>(width_) - 1, 0);
  XDrawLine(display_, window_, gc_, static_cast<int>(width_) - 1, 0, static_cast<int>(width_) - 1, static_cast<int>(items_.size()) * static_cast<int>(item_height_) - 1);
  XDrawLine(display_, window_, gc_, 0, static_cast<int>(items_.size()) * static_cast<int>(item_height_) - 1, static_cast<int>(width_) - 1, static_cast<int>(items_.size()) * static_cast<int>(item_height_) - 1);
  for (std::size_t i = 0; i < items_.size(); ++i) {
    const int top = static_cast<int>(i * item_height_);
    if (active_index_ && *active_index_ == i) {
      XSetForeground(display_, gc_, Theme{}.shadow_pixel);
      XDrawRectangle(display_, window_, gc_, 1, top + 1, width_ - 3, item_height_ - 3);
    }
    XSetForeground(display_, gc_, Theme{}.menu_text_pixel);
    XDrawString(display_, window_, gc_, 10, top + 14, items_[i].label.c_str(), static_cast<int>(items_[i].label.size()));
    XSetForeground(display_, gc_, Theme{}.shadow_pixel);
    XDrawRectangle(display_, window_, gc_, 0, top, width_ - 1, item_height_ - 1);
  }
}

}
