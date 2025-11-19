
if (hwnd == dialog) {
  // BeginPaint(hwnd, &ps);
  // EndPaint(hwnd, &ps);
  auto x = mouse_pos.x;
  auto y = mouse_pos.y;
  auto r = GetRValue(color);
  auto g = GetGValue(color);
  auto b = GetBValue(color);

  HDC hdc = BeginPaint(hwnd, &ps);

  HDC memDC = CreateCompatibleDC(hdc);
  HBITMAP bmp =
      CreateCompatibleBitmap(hdc, ps.rcPaint.right, ps.rcPaint.bottom);

  HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

  FillRect(memDC, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

  StringCchPrintf(buf, BUF_SIZE, _T("x=%ld y=%ld clicked:%c\n"), x, y, clicked);
  DrawTextEx(memDC, buf, -1, &tip_rc, DT_LEFT | DT_TOP, NULL);

  StringCchPrintf(buf, BUF_SIZE, _T("winsize x=%d y=%d w=%d h=%d\n"),
                  virtualLeft, virtualTop, virtualWidth, virtualHeight);
  DrawTextEx(memDC, buf, -1, &tip_rc_2, DT_LEFT | DT_TOP, NULL);

  StringCchPrintf(buf, BUF_SIZE, _T("flashlight:%d fldelta:%.2f\n"),
                  flashLight.isEnabled, flashLight.deltaRadius);
  DrawTextEx(memDC, buf, -1, &tip_rc_3, DT_LEFT | DT_TOP, NULL);

  StringCchPrintf(
      buf, BUF_SIZE, _T("camera scale:%.2f dscale:%.2f x:%.2f y:%.2f\n"),
      camera.scale, camera.deltaScale, camera.position.x, camera.position.y);
  DrawTextEx(memDC, buf, -1, &tip_rc_4, DT_LEFT | DT_TOP, NULL);

  StringCchPrintf(buf, BUF_SIZE, _T("RGB:(%d,%d,%d)\n"), r, g, b);
  DrawTextEx(memDC, buf, -1, &rcRGB, DT_LEFT | DT_TOP, NULL);

  StringCchPrintf(buf, 128, _T("HEX: #%02X%02X%02X"), r, g, b);
  DrawText(memDC, buf, -1, &rcHEX, DT_LEFT | DT_TOP);

  double h, s, v;
  RGBtoHSV(r, g, b, h, s, v);
  StringCchPrintf(buf, 128, _T("HSV: (%.0f°, %.0f%%, %.0f%%)"), h, s * 100,
                  v * 100);
  DrawText(memDC, buf, -1, &rcHSV, DT_LEFT | DT_TOP);

  HBRUSH hBrush = CreateSolidBrush(RGB(r, g, b));
  FillRect(memDC, &rcColor, hBrush);
  DeleteBrush(hBrush);

  BitBlt(hdc, 0, 0, ps.rcPaint.right, ps.rcPaint.bottom, memDC, 0, 0, SRCCOPY);

  SelectObject(memDC, oldBmp);
  DeleteObject(bmp);
  DeleteDC(memDC);

  EndPaint(hwnd, &ps);
}
