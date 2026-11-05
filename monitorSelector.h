#pragma once

#include <cstddef>
#include <mutex>
#include <vector>
#include <windows.h>
#include <winuser.h>

struct ScreenRect {
  int Left;
  int Top;
  int Width;
  int Height;
};

struct MonitorSelector {
  MonitorSelector();
  ~MonitorSelector();

  bool InitMonitorSelector(int left, int top, int w, int h);
  bool GetMonitorPreview();
  bool SelectMonitor(size_t to_selected);

  ScreenRect virtual_screen_rect{};
  ScreenRect selected_screen_rect{};

  std::vector<HMONITOR> monitors;
  std::vector<MONITORINFOEXW> monitor_infos;


  // selected_monitor < 0 || selected_monitor >= size use full screen
  size_t selected_monitor = -1;

  CRITICAL_SECTION lock;
};
