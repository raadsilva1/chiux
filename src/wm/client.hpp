#pragma once

#include <X11/Xlib.h>

#include <string>

namespace chiux::wm {

enum class InternalKind {
  Unset,
  Execute,
  Browser,
  Preferences,
  About,
  Home,
};

struct Client {
  Window window = 0;
  Window frame = 0;
  Window shadow = 0;
  bool mapped = false;
  bool iconic = false;
  bool focused = false;
  bool desktop_hidden = false;
  unsigned int unmap_suppression = 0;
  unsigned int icon_width = 140;
  unsigned int icon_height = 50;
  unsigned int icon_order = 0;
  int x = 0;
  int y = 0;
  int icon_x = 0;
  int icon_y = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int min_width = 80;
  unsigned int min_height = 40;
  bool fixed_size = false;
  bool zoomed = false;
  int restore_x = 0;
  int restore_y = 0;
  unsigned int restore_width = 0;
  unsigned int restore_height = 0;
  bool has_restore_geometry = false;
  Window transient_for = 0;
  bool transient = false;
  unsigned int desktop = 0;
  int border = 1;
  std::string title = "chiux";
  InternalKind internal_kind = InternalKind::Unset;
  Pixmap content_backing = 0;
  unsigned int content_width = 0;
  unsigned int content_height = 0;
};

}
