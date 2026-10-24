#pragma once

#include "system_cursor.h"
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

class CursorRecoveryHandle {
 public:
  explicit CursorRecoveryHandle(HANDLE handle = NULL) : handle_(handle) {}
  ~CursorRecoveryHandle() { reset(); }
  CursorRecoveryHandle(const CursorRecoveryHandle&) = delete;
  CursorRecoveryHandle& operator=(const CursorRecoveryHandle&) = delete;
  HANDLE get() const { return handle_; }
  void reset(HANDLE handle = NULL) {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_;
};

// A second instance of this executable runs only the recovery command. It
// inherits a SYNCHRONIZE-only parent handle and three events, never arbitrary
// application handles. It has no overlay, OpenGL context, or mouse hooks.
class CursorRecoveryWatchdog {
 public:
  bool arm() {
    if (!start()) return false;
    // Arm before the hide call, closing the crash window between the two.
    return SetEvent(armed_.get()) != FALSE;
  }

  void disarm() {
    // Only call after a successful restore; a crash must leave this signaled.
    if (armed_.get()) ResetEvent(armed_.get());
  }

 private:
  bool start() {
    //! wait 进程句柄时 wait_timeout 表示还在运行，wait_object_0 表示结束
    //! 不同的object 表示的含义都不同
    if (process_.get() &&
        WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
      return true;
    }
    process_.reset();
    armed_.reset();
    cancel_.reset();

    //? 事件句柄属性，TRUE 表示事件可以给子进程继承
    SECURITY_ATTRIBUTES security{sizeof(security), NULL, TRUE};
    CursorRecoveryHandle ready(CreateEventW(&security, TRUE, FALSE, NULL));
    armed_.reset(CreateEventW(&security, TRUE, FALSE, NULL));
    cancel_.reset(CreateEventW(&security, TRUE, FALSE, NULL));
    HANDLE parentHandle = NULL;

    //? 在主进程内调用，GetCurrentProcess 获取主进程parentHandle
    if (!ready.get() || !armed_.get() || !cancel_.get() ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                          GetCurrentProcess(), &parentHandle, SYNCHRONIZE,
                          TRUE, 0)) {
      return false;
    }
    CursorRecoveryHandle parent(parentHandle);

    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(NULL, executable.data(),
                                             static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return false;

    wchar_t arguments[256]{};
    std::swprintf(arguments, sizeof(arguments) / sizeof(wchar_t),
                   L" --cursor-watchdog %llu %llu %llu %llu",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(parent.get())),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ready.get())),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(armed_.get())),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cancel_.get())));
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\"" + arguments;
    std::vector<wchar_t> writableCommand(command.begin(), command.end());
    writableCommand.push_back(L'\0');
    // "D:\My Projects\winzoomer.exe" --cursor-watchdog 120 124 128 132

    //? 分配拓展属性字节
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);

    //? 启动子进程的属性 lp 表示拓展属性
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data()); // LPPROC_THREAD_ATTRIBUTE_LIST
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
      return false;
    }
    HANDLE handles[] = {parent.get(), ready.get(), armed_.get(), cancel_.get()};
    PROCESS_INFORMATION child{};
    //? 向属性列表添加属性(handles)
    const BOOL configured = UpdateProcThreadAttribute(
        startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
        sizeof(handles), NULL, NULL);

    const BOOL created = configured && CreateProcessW(
        executable.data(), writableCommand.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
        &startup.StartupInfo, &child);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
      SetLastError(error);
      return false;
    }
    //? 只需要看子进程状态,关闭线程
    CloseHandle(child.hThread);

    //? 主进程监视子进程
    process_.reset(child.hProcess);
    HANDLE waiting[] = {ready.get(), process_.get()};
    //! 先等待 ready 再确认 process存在
    if (WaitForMultipleObjects(2, waiting, FALSE, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) {
      SetEvent(cancel_.get());
      process_.reset();
      armed_.reset();
      cancel_.reset();
      SetLastError(ERROR_NOT_READY);
      return false;
    }
    return true;
  }

  // Closing the parent's handles does not end recovery: the watcher retains
  // its inherited references until the parent process actually terminates.
  CursorRecoveryHandle process_;
  CursorRecoveryHandle armed_;
  CursorRecoveryHandle cancel_;
};

template <typename Cursor = SystemCursorVisibility>
int TryRunCursorRecoveryCommand(const wchar_t* command) {
  //! 独立执行 restore
  if (std::wcscmp(command, L"--restore-cursor") == 0) {
    Cursor cursor;
    return cursor.restoreGlobal() ? 0 : 1;
  }
  constexpr wchar_t prefix[] = L"--cursor-watchdog";

  //! return -1 启动 overlay
  if (std::wcsncmp(command, prefix, std::wcslen(prefix)) != 0) return -1;

  unsigned long long parentValue = 0, readyValue = 0, armedValue = 0, cancelValue = 0;
  wchar_t extra = 0;
  if (std::swscanf(command, L"--cursor-watchdog %llu %llu %llu %llu %lc",
                    &parentValue, &readyValue, &armedValue, &cancelValue,
                    &extra) != 4) {
    return 2;
  }
  CursorRecoveryHandle parent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parentValue)));
  CursorRecoveryHandle ready(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(readyValue)));
  CursorRecoveryHandle armed(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(armedValue)));
  CursorRecoveryHandle cancel(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cancelValue)));
  DWORD flags = 0;
  if (!GetHandleInformation(parent.get(), &flags) ||
      !GetHandleInformation(ready.get(), &flags) ||
      !GetHandleInformation(armed.get(), &flags) ||
      !GetHandleInformation(cancel.get(), &flags)) {
    return 2;
  }

  // Serialize generations: a rapidly restarted app must not hide the cursor
  // until the previous watcher has completed its recovery and exited.
  CursorRecoveryHandle gate(CreateMutexW(NULL, FALSE, L"Local\\Winzoomer.CursorRecovery"));
  if (!gate.get()) return 1;
  HANDLE startupWait[] = {cancel.get(), gate.get()};
  const DWORD acquired = WaitForMultipleObjects(2, startupWait, FALSE, INFINITE);
  if (acquired != WAIT_OBJECT_0 + 1 && acquired != WAIT_ABANDONED_0 + 1) return 1;
  struct GateRelease {
    HANDLE mutex;
    ~GateRelease() { ReleaseMutex(mutex); }
  } release{gate.get()};

  Cursor cursor;
  //? 通知主进程ready,或者子进程返回1 退出
  if (!cursor.initialize() || !SetEvent(ready.get())) return 1;
  HANDLE waiting[] = {parent.get(), cancel.get()};
  bool parentEnded = false;
  while (true) {
    const DWORD result = MsgWaitForMultipleObjectsEx(
        2, waiting, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0) {
      parentEnded = true;
      break;
    }
    if (result != WAIT_OBJECT_0 + 2) break;
    MSG message{};
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (parentEnded && WaitForSingleObject(armed.get(), 0) == WAIT_OBJECT_0) {
    // Transient desktop changes can reject the first call. Retry for up to 4 s.
    for (int attempt = 0; attempt < 20; ++attempt) {
      if (cursor.restoreGlobal()) return 0;
      Sleep(200);
    }
    return 1;
  }
  return parentEnded ? 0 : 1;
}
