#pragma once

#include <X11/Xlib.h>

namespace chiux::ui {

struct Theme {
  unsigned int border_width = 1;
  unsigned int title_height = 18;
  unsigned int button_size = 14;
  unsigned int grow_box_size = 14;
  unsigned int menu_height = 20;
  unsigned int desktop_top = 20;
  unsigned int shadow_offset = 4;
  unsigned long background_pixel = 0xC8C8C8;
  unsigned long frame_active_pixel = 0x000000;
  unsigned long frame_inactive_pixel = 0x808080;
  unsigned long shadow_pixel = 0x686868;
  unsigned long menu_title_shadow_pixel = 0xC2C2C2;
  unsigned long title_active_pixel = 0x000000;
  unsigned long title_inactive_pixel = 0xD8D8D8;
  unsigned long title_text_pixel = 0xFFFFFF;
  unsigned long menu_pixel = 0xD8D8D8;
  unsigned long menu_text_pixel = 0x000000;
};

}
