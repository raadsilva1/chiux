#pragma once

#include <X11/Xlib.h>

#include <string>

namespace chiux::x11 {

class Connection {
public:
  Connection();
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  Display* display() const { return display_; }
  int screen() const { return screen_; }
  Window root() const { return root_; }

  Atom atom(const char* name);
  void sync();

private:
  Display* display_ = nullptr;
  int screen_ = 0;
  Window root_ = 0;
};

}

