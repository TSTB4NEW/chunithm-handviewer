# chunithm-handviewer

> v0.1.1

CHUNITHM 用的 **TASOLLER PLUS 手台输入可视化**。独立 exe + 本地网页，游戏开着也能用，直接当 OBS 浏览器源。

## 特性

- **16 列 × 上下两个感应区**，每列宽高 1:4，压力块与灯带宽度 25:3
- 格子颜色 = 台子当前真实 LED 颜色（读游戏发给驱动的 LED 帧），灯灭为暗色
- 压力用格内白色液面表示，超过阈值加内描边
- **键盘也能打**：默认 `SSSS DDDD FFFF GGGG HHHH JJJJ KKKK LLLL`，与台子数据取较高者合并
- 120 Hz 推送，读游戏内存 1000 Hz
- 两种数据来源自动切换：**游戏进程**（游戏运行中） / **WinUSB**（游戏未开）
- 页面透明、无标题栏、左上角对齐，专为叠加设计

## 快速开始

双击 `chunithm-handviewer.exe`，浏览器会打开标准模式窗口。

建议顺序：**先开游戏，再开 viewer**。

## OBS

| 项 | 值 |
|---|---|
| 来源 | 浏览器 |
| URL | `http://127.0.0.1:38085/?ui=0` |
| 宽 × 高 | 1920 × 520 |
| FPS | 30 或 60 |
| 自定义 CSS | 留空 |

`?ui=0` = OBS 模式：隐藏标题栏和齿轮，保留外框，画面从左上角开始。
其他常用参数：`anchor=bottom`（贴底）、`bg=1`（预览背景）、`num=1`（显示压力数值）、`reset=1`（清空本地设置）。

面板比例约 4:1（16 列格子 + 15 条灯带），其他宽度对应高度：1280×330、1600×415、2560×665。

## 数据来源

优先级：**游戏进程 → 共享内存 → WinUSB → HID**

1. **游戏进程**（推荐，游戏运行中）
   驱动 `tasoller_plus.dll` 独占 USB，viewer 用 `ReadProcessMemory` 只读它的输入缓冲（36 字节）和 LED 包（114 字节）。不碰设备、不抢输入。
2. **WinUSB**（游戏未开）
   直接打开 `0E8F:1231` 的 MI_00 厂商接口，自动探测数据管道（官方固件为 `0x84`）。
3. **共享内存 / HID**
   兼容 chuniio-tasoller 的 `tasoller_input` 与 CFW 固件 `1CCF:2333`。

报文约定：

```
输入: 'DBT' + 状态字节(位反转后 bit0/1=TEST/SERVICE, bit2-7=AIR) + 32 字节压力
LED : 'DL' 0x02 + 32 × RGB
```

## 配置 `handviewer.ini`

```ini
[general]
port=38085
open_browser=1

[input]
hid=1          ; CFW 固件的 HID 兜底
probe=0
vid=0x1CCF
pid=0x2333
threshold=20
keyboard=1     ; 键盘输入

[keys]
cells=S S S S D D D D F F F F G G G G H H H H J J J J K K K K L L L L
air_keys=
fn_keys=
```

按键记号：`A`–`Z`、`0`–`9`、`SPACE`、`ENTER`、`TAB`、`ESC`、`LEFT`、`F1`…`F24`，或直接写 VK 数值。
`/bind` 接口也接受同一套记号，例如 `/bind?slot=cell0&key=SPACE`。

改键不用手写 ini：网页里点设置 →「重新绑定按键」，再点要改的格子（或列表里的 AIR / FN），按下新键即可，写入 ini 立即生效。

## 构建

需要 Zig 0.13+（不需要 MSVC）：

```bat
build.cmd
```

产物在 `build\`。自检（不需要硬件）：

```bat
python tests\smoke_test.py     :: HTTP / WebSocket / 演示动画
python tests\keyboard_test.py  :: 按键记号 / /bind 接口（能注入按键时还测实时按键）
```

## 目录

```
src/main.cpp          Win32 主程序（HTTP + WebSocket + 输入调度）
src/winusb_input.h    WinUSB 读取（游戏未开时）
src/gameinput.h       读游戏进程里的驱动缓冲区（游戏运行时）
web/index.html        叠加页面（单文件，无外部依赖）
tools/probe.cpp       设备诊断工具（probe.exe --list / --read / --led-test）
dist/                 当前成品（exe + web + 配置）
```

## 排错

| 现象 | 处理 |
|---|---|
| 双击没反应 | 旧实例占端口；任务管理器结束所有 `chunithm-handviewer.exe` |
| 颜色/顺序不对 | 设置里调「LED 颜色顺序」和「反色」 |
| 台子没反应 | 看日志 `handviewer.log`：`game: attached` / `winusb: opened` / `CreateFile failed (5)` |
| 按键盘没反应 | 日志里要有 `keyboard: polling [...]`；ini 里 `keyboard=1` 且按键列表与键盘一致 |
| 画面卡顿 | 用 OBS 浏览器源，别用被游戏遮住的普通窗口 |
| 游戏里掉输入 | 先开游戏再开 viewer；viewer 检测到 `chusanApp.exe` 会主动释放设备 |

## 更新记录

- **0.1.1** — 键盘输入真正生效（此前 `keyboard` 线程从未启动）；`/bind` 支持按键记号并可从网页点格子改键；
  修正 `enter`/`tab`/`esc` 等按键名被当成数字解析的问题；修掉网页灯带样式里一段没有选择器的声明；
  版本号不再硬编码。
- **0.1.0** — 首个版本。

## 已知限制

- 只适配 TASOLLER PLUS 的 16×2 + 6 AIR 布局
- 读游戏内存需要与游戏同等权限（游戏以管理员运行时 viewer 也要）
- AIR 暂未接入 LED 颜色，只按触发显示
- LED 报文里三字节的通道顺序未在真机上逐位核对，用设置里的「LED 颜色顺序 / 反色」对齐
