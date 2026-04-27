#include "ui/menubar.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <X11/Xutil.h>

namespace chiux::ui {

namespace {

std::string shorten_label(const std::string& text, std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }
  return text.substr(0, max_chars - 3) + "...";
}

}

MenuBar::MenuBar(Display* display, Window root, unsigned int width, unsigned int height)
  : display_(display), width_(width), height_(height) {
  XSetWindowAttributes attrs{};
  attrs.override_redirect = True;
  attrs.background_pixel = Theme{}.menu_pixel;
  attrs.event_mask = ExposureMask | ButtonPressMask;
  window_ = XCreateWindow(display_, root, 0, 0, width_, height_, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
  XMapRaised(display_, window_);
  gc_ = XCreateGC(display_, window_, 0, nullptr);
  XSetForeground(display_, gc_, Theme{}.menu_text_pixel);
  XSetBackground(display_, gc_, Theme{}.menu_pixel);
  labels_ = {
      {MenuId::Apple, "", 0, 48},
      {MenuId::File, "File", 0, 40},
      {MenuId::Edit, "Edit", 0, 40},
      {MenuId::View, "View", 0, 40},
      {MenuId::Window, "Window", 0, 56},
  };
  draw();
}

MenuBar::~MenuBar() {
  if (gc_) {
    XFreeGC(display_, gc_);
  }
  if (window_) {
    XDestroyWindow(display_, window_);
  }
}

void MenuBar::resize(unsigned int width) {
  width_ = width;
  XResizeWindow(display_, window_, width_, height_);
  draw();
}

void MenuBar::set_active(std::optional<MenuId> menu) {
  active_menu_ = menu;
  draw();
}

void MenuBar::set_application_title(std::string title) {
  application_title_ = std::move(title);
  draw();
}

std::optional<MenuId> MenuBar::hit_test(int x, int y) const {
  layout_labels();
  if (y < 0 || y > static_cast<int>(height_)) {
    return std::nullopt;
  }
  for (const auto& label : labels_) {
    if (x >= label.x && x <= label.x + label.width) {
      return label.id;
    }
  }
  return std::nullopt;
}

void MenuBar::layout_labels() const {
  int x = 8;
  const std::string title = shorten_label(application_title_, 24);
  labels_[0].text = title;
  labels_[0].width = std::max(48, static_cast<int>(title.size()) * 6 + 12);
  labels_[0].x = x;
  x += labels_[0].width + 18;
  for (std::size_t i = 1; i < labels_.size(); ++i) {
    labels_[i].x = x;
    labels_[i].width = std::max(40, static_cast<int>(labels_[i].text.size()) * 6 + 12);
    x += labels_[i].width + 8;
  }
}

void MenuBar::draw() {
  layout_labels();
  XClearWindow(display_, window_);
  XSetForeground(display_, gc_, Theme{}.shadow_pixel);
  XFillRectangle(display_, window_, gc_, 0, static_cast<int>(height_ - 1), width_, 1);
  for (const auto& label : labels_) {
    if (active_menu_ && *active_menu_ == label.id) {
      XSetForeground(display_, gc_, Theme{}.menu_pixel);
      XFillRectangle(display_, window_, gc_, label.x - 4, 2, static_cast<unsigned int>(label.width + 8), height_ - 4);
      XSetForeground(display_, gc_, Theme{}.shadow_pixel);
      XDrawLine(display_, window_, gc_, label.x - 4, static_cast<int>(height_ - 3), label.x + label.width + 3, static_cast<int>(height_ - 3));
    } else {
      XSetForeground(display_, gc_, Theme{}.menu_text_pixel);
    }
    const int len = static_cast<int>(label.text.size());
    XSetForeground(display_, gc_, Theme{}.menu_title_shadow_pixel);
    XDrawString(display_, window_, gc_, label.x + 1, 15, label.text.c_str(), len);
    XSetForeground(display_, gc_, Theme{}.menu_text_pixel);
    XDrawString(display_, window_, gc_, label.x, 14, label.text.c_str(), len);
  }
}

}
