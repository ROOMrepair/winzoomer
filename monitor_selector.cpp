#include "monitor_selector.h"
#include <handleapi.h>

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto& monitors = *reinterpret_cast<std::vector<HMONITOR>*>(data);

  monitors.push_back(monitor);
  return TRUE;
}

MonitorSelector::MonitorSelector() { InitializeCriticalSection(&lock); }
MonitorSelector::~MonitorSelector() {
  // HMONITOR values are borrowed identifiers, not CloseHandle resources.
  DeleteCriticalSection(&lock);
}

bool MonitorSelector::InitMonitorSelector(int left, int top, int w, int h) {
  virtual_screen_rect.Left = left;
  virtual_screen_rect.Top = top;
  virtual_screen_rect.Width = w;
  virtual_screen_rect.Height = h;

  if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                           reinterpret_cast<LPARAM>(&monitors)))
    return false;

  if (monitors.empty()) return false;
  for (auto& m : monitors) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);

    if (!GetMonitorInfoW(m, reinterpret_cast<MONITORINFO*>(&info))) {
      return false;
    }
    monitor_infos.push_back(info);
  }

  selected_screen_rect = virtual_screen_rect;
  selected_monitor = monitors.size();

  return true;
}

bool MonitorSelector::GetMonitorPreview() { return false; }

bool MonitorSelector::SelectMonitor(size_t to_selected) {
  EnterCriticalSection(&lock);
  if (to_selected >= monitors.size()) {
    selected_monitor = monitors.size();
    selected_screen_rect.Width = virtual_screen_rect.Width;
    selected_screen_rect.Height = virtual_screen_rect.Height;
    selected_screen_rect.Left = virtual_screen_rect.Left;
    selected_screen_rect.Top = virtual_screen_rect.Top;
  } else {
    selected_monitor = to_selected;
    auto& info = monitor_infos[selected_monitor];
    selected_screen_rect.Width = info.rcMonitor.right - info.rcMonitor.left;
    selected_screen_rect.Height = info.rcMonitor.bottom - info.rcMonitor.top;
    selected_screen_rect.Left = info.rcMonitor.left;
    selected_screen_rect.Top = info.rcMonitor.top;
  }
  LeaveCriticalSection(&lock);
  return true;
}
