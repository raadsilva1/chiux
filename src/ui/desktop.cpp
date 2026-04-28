#include "ui/desktop.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <utility>

namespace chiux::ui {

namespace {

constexpr int kGridX = 22;
constexpr int kGridY = 24;
constexpr unsigned int kSlotWidth = 96;
constexpr unsigned int kSlotHeight = 108;
constexpr unsigned int kLabelHeight = 28;

std::vector<std::string> wrap_label(const std::string& label, std::size_t max_chars) {
  std::vector<std::string> lines;
  std::string current;
  std::size_t last_space = std::string::npos;
  for (char c : label) {
    if (c == ' ' || c == '-') {
      last_space = current.size();
    }
    current.push_back(c);
    if (current.size() >= max_chars) {
      if (last_space != std::string::npos) {
        lines.push_back(current.substr(0, last_space));
        current = current.substr(last_space + 1);
      } else {
        lines.push_back(current);
        current.clear();
      }
      last_space = std::string::npos;
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  if (lines.empty()) {
    lines.push_back(label);
  }
  if (lines.size() > 2) {
    lines.resize(2);
  }
  return lines;
}

int snap_to_grid(int value, int origin, unsigned int slot) {
  const int offset = value - origin;
  const int snapped = static_cast<int>((offset + static_cast<int>(slot / 2)) / static_cast<int>(slot));
  return origin + snapped * static_cast<int>(slot);
}

}

Desktop::Desktop(Display* display, Window root, unsigned int width, unsigned int height, unsigned int top_offset)
  : display_(display), width_(width), height_(height), top_offset_(top_offset), background_pixel_(Theme{}.background_pixel) {
  XSetWindowAttributes attrs{};
  attrs.override_redirect = True;
  attrs.background_pixel = background_pixel_;
  attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask;
  window_ = XCreateWindow(display_, root, 0, static_cast<int>(top_offset_), width_, height_, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
  XMapWindow(display_, window_);
  gc_ = XCreateGC(display_, window_, 0, nullptr);
  label_font_ = XLoadQueryFont(display_, "fixed");
  compact_label_font_ = XLoadQueryFont(display_, "6x10");
  icon_text_pixel_ = Theme{}.menu_text_pixel;
  icon_shadow_pixel_ = Theme{}.shadow_pixel;
  if (label_font_) {
    XSetFont(display_, gc_, label_font_->fid);
  }
  XSetForeground(display_, gc_, icon_text_pixel_);
  XSetBackground(display_, gc_, background_pixel_);
  backing_ = XCreatePixmap(display_, window_, width_, height_, static_cast<unsigned int>(DefaultDepth(display_, DefaultScreen(display_))));
  draw();
}

Desktop::~Desktop() {
  if (gc_) {
    XFreeGC(display_, gc_);
  }
  if (compact_label_font_) {
    XFreeFont(display_, compact_label_font_);
  }
  if (label_font_) {
    XFreeFont(display_, label_font_);
  }
  if (backing_) {
    XFreePixmap(display_, backing_);
  }
  if (window_) {
    XDestroyWindow(display_, window_);
  }
}

void Desktop::resize(unsigned int width, unsigned int height) {
  width_ = width;
  height_ = height;
  XResizeWindow(display_, window_, width_, height_);
  if (backing_) {
    XFreePixmap(display_, backing_);
  }
  backing_ = XCreatePixmap(display_, window_, width_, height_, static_cast<unsigned int>(DefaultDepth(display_, DefaultScreen(display_))));
  draw();
}

void Desktop::set_icons(std::vector<DesktopIcon> icons) {
  icons_ = std::move(icons);
  selected_icon_.reset();
  arrange_icons();
  draw();
}

void Desktop::set_icon_colors(unsigned long text_pixel, unsigned long shadow_pixel) {
  icon_text_pixel_ = text_pixel;
  icon_shadow_pixel_ = shadow_pixel;
  if (gc_) {
    XSetForeground(display_, gc_, icon_text_pixel_);
  }
  draw();
}

void Desktop::set_background_pixel(unsigned long pixel) {
  background_pixel_ = pixel;
  if (window_) {
    XSetWindowBackground(display_, window_, background_pixel_);
  }
  if (gc_) {
    XSetBackground(display_, gc_, background_pixel_);
  }
  draw();
}

std::optional<std::size_t> Desktop::hit_test(int x, int y) const {
  for (std::size_t i = 0; i < icons_.size(); ++i) {
    const auto& icon = icons_[i];
    const int left = icon.x;
    const int top = icon.y;
    const int right = left + static_cast<int>(icon.width);
    const int bottom = top + static_cast<int>(icon.height + kLabelHeight);
    if (x >= left && x <= right && y >= top && y <= bottom) {
      return i;
    }
  }
  return std::nullopt;
}

void Desktop::select_icon(std::optional<std::size_t> index) {
  if (selected_icon_ && *selected_icon_ < icons_.size()) {
    icons_[*selected_icon_].selected = false;
  }
  selected_icon_ = index;
  if (selected_icon_ && *selected_icon_ < icons_.size()) {
    icons_[*selected_icon_].selected = true;
  }
  draw();
}

void Desktop::move_icon(std::size_t index, int x, int y) {
  if (index >= icons_.size()) {
    return;
  }
  icons_[index].x = std::max(8, snap_to_grid(x, kGridX, kSlotWidth));
  icons_[index].y = std::max(8, snap_to_grid(y, kGridY, kSlotHeight));
  draw();
}

void Desktop::arrange_icons() {
  const unsigned int columns = std::max(1u, width_ / kSlotWidth);
  for (std::size_t i = 0; i < icons_.size(); ++i) {
    const unsigned int col = static_cast<unsigned int>(i % columns);
    const unsigned int row = static_cast<unsigned int>(i / columns);
    icons_[i].x = kGridX + static_cast<int>(col * kSlotWidth);
    icons_[i].y = kGridY + static_cast<int>(row * kSlotHeight);
  }
  draw();
}

void Desktop::draw_icon(Drawable target, const DesktopIcon& icon) {
  const int body_x = icon.x;
  const int body_y = icon.y;
  const int body_w = static_cast<int>(icon.width);
  const int body_h = static_cast<int>(icon.height);
  const int label_top = body_y + body_h + 18;
  const unsigned int tab_w = static_cast<unsigned int>(std::max(8, body_w / 2));
  const unsigned int tab_h = 8;
  const bool compact_label = icon.label == "Applications";
  const auto lines = wrap_label(icon.label, compact_label ? 14 : 10);
  XFontStruct* label_font = compact_label && compact_label_font_ ? compact_label_font_ : label_font_;
  if (label_font) {
    XSetFont(display_, gc_, label_font->fid);
  }

  if (icon.selected) {
    XSetForeground(display_, gc_, Theme{}.frame_active_pixel);
    XFillRectangle(display_, target, gc_, body_x - 3, body_y - 3, icon.width + 6, icon.height + kLabelHeight + 10);
    XSetForeground(display_, gc_, Theme{}.menu_pixel);
    XFillRectangle(display_, target, gc_, body_x - 2, body_y - 2, icon.width + 4, icon.height + kLabelHeight + 8);
  }

  XSetForeground(display_, gc_, icon_shadow_pixel_);
  XFillRectangle(display_, target, gc_, body_x + 3, body_y + 3, icon.width, icon.height);
  XSetForeground(display_, gc_, icon_shadow_pixel_);
  XFillRectangle(display_, target, gc_, body_x + 2, body_y + 2, icon.width - 1, icon.height - 1);
  XSetForeground(display_, gc_, Theme{}.menu_pixel);
  XFillRectangle(display_, target, gc_, body_x, body_y, icon.width, icon.height);
  XSetForeground(display_, gc_, icon_text_pixel_);
  XDrawRectangle(display_, target, gc_, body_x, body_y, icon.width, icon.height);
  XFillRectangle(display_, target, gc_, body_x + 6, body_y - 3, tab_w, tab_h);
  XDrawRectangle(display_, target, gc_, body_x + 6, body_y - 3, tab_w, tab_h);
  XDrawLine(display_, target, gc_, body_x + 8, body_y + 10, body_x + body_w - 8, body_y + 10);
  XDrawLine(display_, target, gc_, body_x + 8, body_y + 22, body_x + body_w - 8, body_y + 22);
  for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
    const int text_width = label_font ? XTextWidth(label_font, lines[i].c_str(), static_cast<int>(lines[i].size())) : static_cast<int>(lines[i].size()) * 6;
    const int text_x = body_x + std::max(0, (body_w - text_width) / 2);
    const int line_step = compact_label ? 11 : 12;
    const int text_y = label_top + static_cast<int>(i) * line_step;
    XSetForeground(display_, gc_, icon_shadow_pixel_);
    XDrawString(display_, target, gc_, text_x + 1, text_y + 1, lines[i].c_str(), static_cast<int>(lines[i].size()));
    XSetForeground(display_, gc_, icon_text_pixel_);
    XDrawString(display_, target, gc_, text_x, text_y, lines[i].c_str(), static_cast<int>(lines[i].size()));
  }
  if (label_font_) {
    XSetFont(display_, gc_, label_font_->fid);
  }
}

void Desktop::draw() {
  if (!backing_) {
    return;
  }
  XSetForeground(display_, gc_, background_pixel_);
  XFillRectangle(display_, backing_, gc_, 0, 0, width_, height_);
  for (const auto& icon : icons_) {
    draw_icon(backing_, icon);
  }
  XCopyArea(display_, backing_, window_, gc_, 0, 0, width_, height_, 0, 0);
}

}
