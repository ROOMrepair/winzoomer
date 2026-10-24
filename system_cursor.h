#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

// Own only the visibility change made by this process. No cursor resources or
// ShowCursor display counters are modified. Call from the UI thread only.
class SystemCursorVisibility {
 public:
  SystemCursorVisibility() = default;
  SystemCursorVisibility(const SystemCursorVisibility&) = delete;
  SystemCursorVisibility& operator=(const SystemCursorVisibility&) = delete;
  ~SystemCursorVisibility() { shutdown(); }

  bool initialize() {
    if (initialized_) return true;
    if (!loadRuntime()) return false;
    if (!initialize_()) return reportFailure("MagInitialize");
    initialized_ = true;
    return true;
  }

  bool setHidden(bool hidden) {
    if (!initialized_) {
      if (!hidden) return true;
      if (!initialize()) return false;
    }
    if (hidden_ == hidden) return true;
    if (!showCursor_(hidden ? FALSE : TRUE)) {
      return reportFailure("MagShowSystemCursor");
    }
    hidden_ = hidden;
    return true;
  }

  // Recovery runs in another process, which did not make the original hide
  // call. Unlike setHidden(false), it must explicitly show the global cursor.
  bool restoreGlobal() {
    if (!initialize()) return false;
    if (!showCursor_(TRUE)) return reportFailure("MagShowSystemCursor(restore)");
    hidden_ = false;
    return true;
  }

  bool shutdown() {
    if (initialized_) {
      // Restore before releasing the runtime, and retain ownership on failure
      // so the next cleanup path can retry instead of forgetting to show it.
      if (!setHidden(false)) return false;
      if (!uninitialize_()) return reportFailure("MagUninitialize");
      initialized_ = false;
    }
    unloadRuntime();
    return true;
  }

 private:
  using InitializeFunction = BOOL(WINAPI*)();
  using ShowCursorFunction = BOOL(WINAPI*)(BOOL);

  bool loadRuntime() {
    if (library_) return true;
    // Some MinGW distributions ship neither the function declarations nor the
    // import library. Resolve the Windows DLL directly, from System32 only.
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    constexpr wchar_t suffix[] = L"\\Magnification.dll";
    if (length == 0 ||
        length + sizeof(suffix) / sizeof(wchar_t) > MAX_PATH) {
      return reportFailure("GetSystemDirectoryW");
    }
    for (size_t i = 0; i < sizeof(suffix) / sizeof(wchar_t); ++i) {
      path[length + i] = suffix[i];
    }
    library_ = LoadLibraryW(path);
    if (!library_) return reportFailure("LoadLibraryW(Magnification.dll)");
    initialize_ = resolve<InitializeFunction>("MagInitialize");
    showCursor_ = resolve<ShowCursorFunction>("MagShowSystemCursor");
    uninitialize_ = resolve<InitializeFunction>("MagUninitialize");
    if (!initialize_ || !showCursor_ || !uninitialize_) {
      reportFailure("GetProcAddress(Magnification.dll)");
      unloadRuntime();
      return false;
    }
    return true;
  }

  template <typename Function>
  Function resolve(const char* name) {
    const FARPROC address = GetProcAddress(library_, name);
    Function function = nullptr;
    // Win32 function pointers share the FARPROC representation. Copy the
    // address without a cast between incompatible C++ function signatures.
    static_assert(sizeof(function) == sizeof(address), "Win32 function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
  }

  void unloadRuntime() {
    if (library_) FreeLibrary(library_);
    library_ = NULL;
    initialize_ = uninitialize_ = nullptr;
    showCursor_ = nullptr;
  }

  static bool reportFailure(const char* operation) {
    const DWORD error = GetLastError();
    char message[160]{};
    std::snprintf(message, sizeof(message), "winzoomer: %s failed (error %lu)\n",
                  operation, static_cast<unsigned long>(error));
    OutputDebugStringA(message);
    return false;
  }

  bool initialized_ = false;
  bool hidden_ = false;
  HMODULE library_ = NULL;
  InitializeFunction initialize_ = nullptr;
  InitializeFunction uninitialize_ = nullptr;
  ShowCursorFunction showCursor_ = nullptr;
};
