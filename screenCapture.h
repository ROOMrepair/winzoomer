#pragma once

#include <handleapi.h>
#include <synchapi.h>
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include "monitorSelector.h"

struct ScreenCaptureBuffer {
  int width = 0;
  int height = 0;

  HDC memoryDC = NULL;
  HDC screenDC = NULL;
  HBITMAP bitmap = NULL;
  HGDIOBJ previousBitmap = NULL;
  unsigned char* pixels = nullptr;

  bool setup(int w, int h) {
    width = w;
    height = h;

    memoryDC = CreateCompatibleDC(NULL);
    if (memoryDC == NULL) {
      return false;
    }

    screenDC = GetDC(NULL);
    if (screenDC == NULL) {
      shutdown();
      return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    // Keep a bottom-up DIB so the existing OpenGL texture coordinates remain
    // unchanged (the first row in memory is the bottom row of the desktop).
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    //? 建一张 GDI 能使用的 HBITMAP，同时返回其指针
    bitmap = CreateDIBSection(NULL, &bitmapInfo, DIB_RGB_COLORS,
                              reinterpret_cast<void**>(&pixels), NULL, 0);
    if (bitmap == NULL || pixels == nullptr) {
      shutdown();
      return false;
    }

    //? 用于恢复原状态
    previousBitmap = SelectObject(memoryDC, bitmap);
    if (previousBitmap == NULL || previousBitmap == HGDI_ERROR) {
      previousBitmap = NULL;
      shutdown();
      return false;
    }

    return true;
  }

  /*? how to capture screen
      1. getdc -> create compatiblebitmap of this dc
      2. bitblt from compatiblebitmap
      3. 取 pixels
  */
  bool capture(int sourceX, int sourceY) {
    if (memoryDC == NULL || bitmap == NULL || pixels == nullptr) {
      return false;
    }

    if (screenDC == NULL) {
      return false;
    }

    WINBOOL copied = BitBlt(memoryDC, 0, 0, width, height, screenDC, sourceX,
                            sourceY, SRCCOPY);
    // Complete GDI writes before publishing or reading the DIB's pixels.
    WINBOOL flushed = GdiFlush();
    return copied != FALSE && flushed != FALSE;
  }

  void shutdown() {
    if (memoryDC != NULL && previousBitmap != NULL) {
      SelectObject(memoryDC, previousBitmap);
    }
    if (bitmap != NULL) {
      DeleteObject(bitmap);
    }
    if (memoryDC != NULL) {
      DeleteDC(memoryDC);
    }
    if (screenDC != NULL) {
      ReleaseDC(NULL, screenDC);
    }

    width = 0;
    height = 0;
    memoryDC = NULL;
    screenDC = NULL;
    bitmap = NULL;
    previousBitmap = NULL;
    pixels = nullptr;
  }
};

struct RealtimeCapture {
  ScreenCaptureBuffer buffers[3];
  CRITICAL_SECTION lock;
  HANDLE worker = NULL;
  HANDLE stopEvent = NULL;
  HANDLE wakeEvent = NULL;
  HANDLE initializedEvent = NULL;
  HANDLE resetSetCaptureEvent = NULL;
  MonitorSelector* selector = nullptr;

  std::atomic<bool> enabled{false};
  std::atomic<bool> isLoading{false};

  bool previousSelectedState = false;
  bool isFirstFrame = true;

  bool initialized = false;
  int ready = -1;
  int reading = -1;
  int width, height, left, top;

  RealtimeCapture() { InitializeCriticalSection(&lock); }
  ~RealtimeCapture() {
    stop();
    DeleteCriticalSection(&lock);
  }

  //! 只在 run 线程内调用
  bool loadCaptureBufferInfo() {
    if (!selector) return false;

    EnterCriticalSection(&selector->lock);
    width = selector->selected_screen_rect.Width;
    height = selector->selected_screen_rect.Height;
    left = selector->selected_screen_rect.Left;
    top = selector->selected_screen_rect.Top;
    LeaveCriticalSection(&selector->lock);

    for (auto& buffer : buffers) {
      buffer.shutdown();
      if (!buffer.setup(width, height)) {
        initialized = false;
        return false;
      }
    }

    reading = ready = -1;
    isLoading.store(false);

    EnterCriticalSection(&lock);
    if(previousSelectedState){
      enabled.store(true);
    }
    LeaveCriticalSection(&lock);

    return true;
  }

  static DWORD WINAPI run(void* context) {
    auto& self = *static_cast<RealtimeCapture*>(context);
    self.initialized = true;

    if (!self.selector) self.initialized = false;
    self.loadCaptureBufferInfo();

    SetEvent(self.initializedEvent);

    HANDLE events[] = {self.stopEvent, self.resetSetCaptureEvent,
                       self.wakeEvent};
    while (self.initialized &&
           WaitForSingleObject(self.stopEvent, 0) == WAIT_TIMEOUT) {
      // if not enable wait events
      if (!self.enabled.load()) {
        DWORD waitRes = WaitForMultipleObjects(3, events, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0) {
          break;
        }
        if (waitRes == WAIT_OBJECT_0 + 1) {
          self.reading = self.ready = -1;
          if (!self.loadCaptureBufferInfo()) {
            break;
          }
        }
        continue;
      }

      const DWORD started = GetTickCount();
      int writing = -1;
      EnterCriticalSection(&self.lock);
      //? reading 渲染线程读取中， ready 等待渲染线程读取
      for (int i = 0; i < 3; ++i) {
        if (i != self.reading && i != self.ready) {
          writing = i;
          break;
        }
      }
      LeaveCriticalSection(&self.lock);

      if (writing >= 0 && self.buffers[writing].capture(self.left, self.top)) {
        EnterCriticalSection(&self.lock);
        if (self.enabled.load()) {
          // Drop the previous ready frame, never queue stale frames.
          self.ready = writing;
        }
        LeaveCriticalSection(&self.lock);
      }

      const DWORD elapsed = GetTickCount() - started;
      const DWORD interval = static_cast<DWORD>(1000 / 60.0f);
      const DWORD delay = elapsed < interval ? interval - elapsed : 1;
      if (WaitForSingleObject(self.stopEvent, delay) != WAIT_TIMEOUT) {
        break;
      }
    }

    for (auto& buffer : self.buffers) {
      buffer.shutdown();
    }
    return 0;
  }

  bool start(MonitorSelector* s) {
    if (worker != NULL) {
      return initialized;
    }
    selector = s;
    stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    initializedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    resetSetCaptureEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (stopEvent && wakeEvent && initializedEvent && resetSetCaptureEvent) {
      worker = CreateThread(NULL, 0, run, this, 0, NULL);
    }
    if (worker == NULL) {
      stop();
      return false;
    }
    WaitForSingleObject(initializedEvent, INFINITE);
    if (!initialized) {
      stop();
      return false;
    }
    return true;
  }

  void setEnabled(bool value) {
    EnterCriticalSection(&lock);
    previousSelectedState = enabled.exchange(value);
    ready = -1;
    LeaveCriticalSection(&lock);
    if (value && wakeEvent) {
      SetEvent(wakeEvent);
    }
  }

  void setLoadEvent() {
    if (resetSetCaptureEvent) {
      isLoading.store(true);
      SetEvent(resetSetCaptureEvent);
    }
  }

  int acquireLatest() {
    EnterCriticalSection(&lock);
    const int result = ready;
    if (result >= 0) {
      reading = result;
      ready = -1;
    }
    LeaveCriticalSection(&lock);
    return result;
  }

  void release() {
    EnterCriticalSection(&lock);
    reading = -1;
    LeaveCriticalSection(&lock);
  }

  void stop() {
    if (worker) {
      SetEvent(stopEvent);
      WaitForSingleObject(worker, INFINITE);
      CloseHandle(worker);
      worker = NULL;
    }
    if (stopEvent) CloseHandle(stopEvent);
    if (wakeEvent) CloseHandle(wakeEvent);
    if (initializedEvent) CloseHandle(initializedEvent);
    if (resetSetCaptureEvent) CloseHandle(resetSetCaptureEvent);
    stopEvent = wakeEvent = initializedEvent = resetSetCaptureEvent = NULL;
    enabled.store(false);
    initialized = false;
    ready = reading = -1;
  }
};
