#pragma once

#include "config/config.hpp"
#include "ui/desktop.hpp"
#include "ui/menubar.hpp"
#include "ui/popup_menu.hpp"
#include "ui/theme.hpp"
#include "wm/client.hpp"
#include "x11/connection.hpp"

#include <filesystem>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chiux::wm {

struct PreferenceSwatch {
  std::string label;
  std::string token;
  unsigned long pixel = 0;
};

class WindowManager {
public:
  WindowManager(x11::Connection& connection, config::Config config, std::filesystem::path config_path);
  ~WindowManager();

  WindowManager(const WindowManager&) = delete;
  WindowManager& operator=(const WindowManager&) = delete;

  void run();

private:
  enum class DragMode { Idle, Move, Resize };

  void claim_wm_selection();
  void init_atoms();
  void init_desktop();
  void init_keybindings();
  void refresh_keybindings();
  void init_theme_from_config();
  void scan_existing_windows();
  void manage_window(Window window);
  void unmanage_window(Window window, bool withdraw);
  void iconify_client(Client& client);
  void deiconify_client(Client& client);
  void update_client_list();
  void sync_desktop_icons();
  void save_config();
  void update_desktop_properties();
  void switch_to_desktop(unsigned int desktop);
  void move_focused_to_desktop(unsigned int desktop);
  void cycle_desktop(int delta);
  void open_execute_window();
  void open_file_manager_window();
  void open_applications_window();
  void open_preferences_window();
  void open_about_window();
  void open_home_window();
  void refresh_file_manager();
  void ensure_file_manager_selection_visible(const Client& client);
  void draw_internal_content(Client& client);
  void ensure_internal_backing(Client& client);
  void release_internal_backing(Client& client);
  void present_internal_content(Client& client);
  void draw_execute_window(Client& client);
  void draw_file_manager_window(Client& client);
  void draw_applications_window(Client& client);
  void draw_preferences_window(Client& client);
  void draw_about_window(Client& client);
  void draw_home_window(Client& client);
  bool load_about_icon();
  void handle_internal_button_press(Client& client, const XButtonEvent& event);
  void handle_internal_button_release(Client& client, const XButtonEvent& event);
  void handle_internal_motion_notify(Client& client, const XMotionEvent& event);
  void handle_internal_key_press(Client& client, const XKeyEvent& event);
  void run_launcher_command(const std::string& command);
  void apply_background_color(const std::string& color_token);
  void set_wm_state(Client& client, long state);
  void fetch_title(Client& client);
  bool client_supports_delete(Window window) const;
  bool client_supports_protocol(Window window, Atom protocol) const;
  Client& frame_client(Window window);
  Client* find_client(Window window);
  Client* find_client_by_frame(Window frame);
  void focus_client(Client* client);
  void configure_client(Client& client, int x, int y, unsigned int width, unsigned int height);
  void move_client(Client& client, int dx, int dy);
  void resize_client(Client& client, int width, int height);
  void toggle_zoom_client(Client& client);
  void set_client_active(Client& client, bool active);
  void update_dynamic_icon_colors();
  void update_client_shadow(Client& client);
  void set_client_desktop_visible(Client& client, bool visible);
  void reflow_iconified_clients(unsigned int desktop);
  std::vector<Window> collect_transient_group(Window window) const;
  void restack_managed_windows();
  void raise_managed_window(Window window);
  void lower_managed_window(Window window);
  void draw_frame(Client& client);
  void grab_pointer_for_drag(Window window);
  void ungrab_pointer_for_drag();
  void define_cursor(Window window);
  int clamp_client_y(int y) const;
  void ensure_desktop_below_clients();
  void show_menu(ui::MenuId menu, int x, int y);
  void hide_menu();
  void handle_menu_action(const std::string& action);
  void handle_desktop_click(int x, int y);
  void handle_desktop_drag(int x, int y);
  void handle_desktop_release(const XButtonEvent& event);
  void handle_desktop_double_click(std::size_t index);
  void handle_map_request(const XMapRequestEvent& event);
  void handle_configure_request(const XConfigureRequestEvent& event);
  void handle_unmap_notify(const XUnmapEvent& event);
  void handle_destroy_notify(const XDestroyWindowEvent& event);
  void handle_button_press(const XButtonEvent& event);
  void handle_motion_notify(const XMotionEvent& event);
  void handle_button_release(const XButtonEvent& event);
  void handle_key_press(const XKeyEvent& event);
  void handle_enter_notify(const XCrossingEvent& event);
  void handle_focus_in(const XFocusChangeEvent& event);
  void handle_client_message(const XClientMessageEvent& event);
  void handle_property_notify(const XPropertyEvent& event);
  void handle_expose(const XExposeEvent& event);
  void handle_selection_clear(const XSelectionClearEvent& event);
  bool is_wm_protocol(Atom atom, Atom protocol) const;
  void set_active_window(Window window);
  void reap_children();
  void handle_idle();
  std::optional<std::size_t> hit_test_application_icon(const Client& client, int x, int y) const;
  void persist_applications();
  void add_application_entry();
  void update_selected_application();
  void remove_selected_application();
  void clear_application_form(bool clear_selection);
  void load_selected_application_into_form();
  void cycle_application_style(int delta);

  x11::Connection& connection_;
  config::Config config_;
  std::filesystem::path config_path_;
  Display* display_ = nullptr;
  Window root_ = 0;
  int screen_ = 0;
  Cursor arrow_cursor_ = None;
  ui::Theme theme_{};
  Atom wm_protocols_ = 0;
  Atom wm_delete_window_ = 0;
  Atom wm_state_ = 0;
  Atom wm_take_focus_ = 0;
  Atom net_supported_ = 0;
  Atom net_client_list_ = 0;
  Atom net_active_window_ = 0;
  Atom net_wm_name_ = 0;
  Atom net_wm_state_ = 0;
  Atom net_wm_window_type_ = 0;
  Atom net_wm_desktop_ = 0;
  Atom net_current_desktop_ = 0;
  Atom net_number_of_desktops_ = 0;
  Atom net_supporting_wm_check_ = 0;
  Atom utf8_string_ = 0;
  Window wm_check_window_ = 0;
  static constexpr unsigned int kDesktopCount = 4;
  unsigned int current_desktop_ = 0;
  std::array<std::string, kDesktopCount> desktop_names_{{"Desktop 1", "Desktop 2", "Desktop 3", "Desktop 4"}};
  Window execute_window_ = 0;
  Window file_manager_window_ = 0;
  Window applications_window_ = 0;
  Window preferences_window_ = 0;
  Window about_window_ = 0;
  Window home_window_ = 0;
  std::filesystem::path file_manager_path_;
  std::vector<std::filesystem::directory_entry> file_manager_entries_;
  std::size_t file_manager_selected_ = 0;
  std::size_t file_manager_scroll_ = 0;
  std::string execute_buffer_;
  std::string application_name_buffer_;
  std::string application_command_buffer_;
  unsigned int application_style_index_ = 0;
  std::unordered_map<Window, Client> clients_;
  std::vector<Window> stacking_order_;
  std::unique_ptr<ui::PopupMenu> popup_menu_;
  std::optional<ui::MenuId> active_menu_;
  Client* focused_ = nullptr;
  bool running_ = true;
  DragMode drag_mode_ = DragMode::Idle;
  Client* drag_client_ = nullptr;
  int drag_start_x_ = 0;
  int drag_start_y_ = 0;
  int drag_origin_x_ = 0;
  int drag_origin_y_ = 0;
  unsigned int drag_origin_w_ = 0;
  unsigned int drag_origin_h_ = 0;
  std::unique_ptr<ui::MenuBar> menubar_;
  std::unique_ptr<ui::Desktop> desktop_;
  std::optional<std::size_t> drag_icon_index_;
  bool drag_icon_moved_ = false;
  int drag_icon_offset_x_ = 0;
  int drag_icon_offset_y_ = 0;
  int drag_icon_start_x_ = 0;
  int drag_icon_start_y_ = 0;
  enum class ApplicationsField { Name, Command };
  ApplicationsField applications_active_field_ = ApplicationsField::Name;
  bool applications_details_collapsed_ = true;
  std::optional<std::size_t> applications_selected_;
  std::optional<std::size_t> applications_drag_index_;
  bool applications_drag_moved_ = false;
  int applications_drag_offset_x_ = 0;
  int applications_drag_offset_y_ = 0;
  int applications_drag_start_x_ = 0;
  int applications_drag_start_y_ = 0;
  unsigned int next_icon_order_ = 0;
  std::vector<PreferenceSwatch> preferences_palette_;
  unsigned long icon_text_pixel_ = 0x000000;
  unsigned long icon_shadow_pixel_ = 0x686868;
  std::filesystem::path about_icon_path_;
  bool about_icon_loaded_ = false;
  unsigned int about_icon_width_ = 0;
  unsigned int about_icon_height_ = 0;
  std::vector<unsigned long> about_icon_pixels_;
};

}
