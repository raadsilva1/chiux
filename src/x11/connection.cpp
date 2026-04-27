#include "x11/connection.hpp"

#include "util/log.hpp"

#include <X11/Xatom.h>
#include <cstdlib>
#include <stdexcept>

namespace chiux::x11 {

Connection::Connection() {
  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    throw std::runtime_error("failed to open X display");
  }
  screen_ = DefaultScreen(display_);
  root_ = RootWindow(display_, screen_);
}

Connection::~Connection() {
  if (display_) {
    XCloseDisplay(display_);
  }
}

Atom Connection::atom(const char* name) {
  return XInternAtom(display_, name, False);
}

void Connection::sync() {
  XSync(display_, False);
}

}

