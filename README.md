# winzoomer

https://github.com/tsoding/boomer 的一个可能的 windows 版本

## 环境&工具
- mingw-w64	此程序使用 w64devkit 2.1.0
- freetype 2.10.0	可以在makefile内直接设置禁用freetype

## 控制方式
| 功能 | 快捷键 |
| --- | --- |
|退出|<kbd>Esc</kbd>|
|手电筒|<kbd>F</kbd>|
|缩放手电筒(包括画面)|<kbd>Ctrl</kbd> + <kbd>滚轮</kbd>  |
|缩放手电筒|<kbd>Shift</kbd> + <kbd>滚轮</kbd> |
|重置|<kbd>R</kbd>|
|取色到剪切板(HEX模式)|<kbd>X</kbd>|
|取色(RGB模式)|<kbd>C</kbd>|
| 实时画面|<kbd>U</kbd>|
| 穿透模式 (danger!!) |<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>F11</kbd>|
| 穿透模式下缩放画面|<kbd>Shift</kbd> + <kbd>滚轮</kbd>|

## 系统光标恢复

如果程序导致光标消失，可尝试（我也不清楚有没有效果）

```powershell
.\build\winzoomer.exe --restore-cursor
```



