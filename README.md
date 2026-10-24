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
| 实时画面|<kbd>U</kbd>|
| 穿透模式|<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>F11</kbd>|
| 穿透模式下缩放画面|<kbd>Shift</kbd> + <kbd>滚轮</kbd>|

穿透模式下，按住左或右 Shift 向上滚动放大、向下滚动缩小，以 overlay 中绘制的鼠标位置为缩放中心。
通过 Raw Input（`RIDEV_INPUTSINK` / `WM_INPUT`）接收后台滚轮，不使用鼠标钩子、额外输入线程或鼠标注入。
这里只监听输入，不拦截：Shift + 滚轮也会传给底层应用，因此底层应用可能同时滚动或执行自己的快捷操作。
不按 Shift 的滚轮、点击和移动照常穿透。
普通模式下的 Shift + 滚轮仍按原来的手电筒逻辑处理。

## 系统光标恢复

进入穿透模式时隐藏系统光标，退出穿透模式或正常关闭程序时恢复。
首次进入前会启动同一可执行文件的后台恢复进程；它没有窗口，通过主进程句柄等待退出，平时不轮询。
如果主进程崩溃或被单独强制结束，恢复进程会恢复光标并自行退出。
恢复进程未能启动时，不会隐藏光标或进入穿透模式。

如果主进程和恢复进程被一起结束（例如结束整个进程树），可在项目目录的 PowerShell 中手动恢复：

```powershell
.\build\winzoomer.exe --restore-cursor
```

手动恢复模式不创建 overlay，运行后立即退出。

