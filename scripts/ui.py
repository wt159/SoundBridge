#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SoundBridge GUI 观察/操作小工具（零第三方依赖，仅 Windows）。

为什么存在：本机安全策略禁止 `Add-Type`，PowerShell 侧的 UIAutomation / .NET 互操作
全部不可用，因此截图与点击只能通过 ctypes 直连 Win32。也正因如此，这里刻意不引入
Pillow / pyautogui / mss —— 换台机器、进容器都能直接跑。

子命令
------
  shot   截取窗口（或整屏）存成 PNG
  map    打印窗口内容的亮度字符图，用来量控件坐标
  where  查询某个屏幕坐标落在哪个窗口，或打印窗口的位置信息
  click  点击屏幕绝对坐标，或窗口客户区内的相对坐标

窗口寻址
--------
  --window "SoundBridge"   先按标题精确匹配，失败后退化为不区分大小写的子串匹配
  --pid 1234               按进程号定位（标题重复时更稳）

典型用法
--------
  # 1. 截一张，看界面长什么样
  python scripts/ui.py shot --window SoundBridge --out shot.png

  # 2. 不知道按钮在哪：打字符图，按 step 像素一格读出坐标
  python scripts/ui.py map --window SoundBridge --step 10

  # 3. 用「窗口客户区坐标」点击：工具自己加窗口偏移，窗口被拖动也不受影响
  python scripts/ui.py click --window SoundBridge --client 207,654

  # 4. 先看会点到谁，不真点
  python scripts/ui.py click --window SoundBridge --client 207,654 --dry-run

坐标约定
--------
  `--client X,Y` 是**客户区坐标**（不含标题栏与边框），原点在客户区左上角，与 Qt 里
  控件的 geometry() 同一套坐标系。绝对坐标直接写 `X Y`。
  两者都支持 `--dry-run`，此时只打印命中窗口不动作。

退出码
------
  0 成功 / 2 参数错误 / 3 找不到窗口 / 4 截图失败 / 5 坐标落在客户区之外
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time
import zlib

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

SRCCOPY = 0x00CC0020
PW_RENDERFULLCONTENT = 0x00000002
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
GA_ROOT = 2

RAMP = " .:-=+*#%@"


class UiError(Exception):
    """带退出码的失败，避免到处 if/raise SystemExit。"""

    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


# --------------------------------------------------------------------------
# Win32 基础设施
# --------------------------------------------------------------------------


def set_dpi_aware():
    """必须在任何取窗口矩形/截屏动作之前调用，否则拿到的是被虚拟化的坐标。"""
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # per-monitor v2
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass


def window_title(hwnd):
    length = user32.GetWindowTextLengthW(hwnd)
    if length <= 0:
        return ""
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def window_pid(hwnd):
    pid = wt.DWORD(0)
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


SW_RESTORE = 9
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001


def activate_window(hwnd):
    """把窗口提到前台。后台窗口被别的窗口盖住时，屏幕坐标点击会打到上层窗口上。

    注意：从非前台进程调用 SetForegroundWindow 常常被系统忽略，返回 False 也属正常；
    这种情况请用 --post 走消息投递。
    """
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    time.sleep(0.12)
    return user32.GetForegroundWindow() == hwnd


def post_click(hwnd, client_x, client_y, double=False):
    """把鼠标消息直接投给窗口：不依赖前台，也不怕被遮挡。"""
    lparam = (client_y << 16) | (client_x & 0xFFFF)
    rounds = 2 if double else 1
    for index in range(rounds):
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam)
        time.sleep(0.03)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam)
        if index + 1 < rounds:
            time.sleep(0.06)


def process_image_name(pid):
    """取进程可执行文件名（含扩展名）。用于区分同名但不同来源的窗口。"""
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ""
    try:
        size = wt.DWORD(260)
        buf = ctypes.create_unicode_buffer(260)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return os.path.basename(buf.value)
    finally:
        kernel32.CloseHandle(handle)
    return ""


def _window_score(hwnd, title):
    """标题只能定位到「名字」，同名窗口（例如资源管理器里同名的文件夹）需要靠进程名与窗口类区分。"""
    score = 0
    stem = os.path.splitext(process_image_name(window_pid(hwnd)))[0].lower()
    wanted = title.replace(" ", "").lower()
    if stem and stem == wanted:
        score += 100
    elif stem and (wanted in stem or stem in wanted):
        score += 60
    if window_class(hwnd).startswith("Qt"):
        score += 40
    return score


def _visible_windows():
    found = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def callback(hwnd, _):
        if user32.IsWindowVisible(hwnd) and window_title(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proto(callback), 0)
    return found


def find_window(title=None, pid=None):
    """按标题/进程号找顶层窗口；标题支持精确优先、子串兜底。"""
    candidates = _visible_windows()

    if pid is not None:
        matched = [h for h in candidates if window_pid(h) == int(pid)]
        if not matched:
            raise UiError("找不到 pid=%s 的可见顶层窗口" % pid, 3)
        return matched[0]

    if not title:
        raise UiError("需要 --window 或 --pid 之一", 2)

    exact = [h for h in candidates if window_title(h) == title]
    pool = exact or [h for h in candidates if title.lower() in window_title(h).lower()]
    if pool:
        return max(pool, key=lambda h: _window_score(h, title))

    raise UiError("找不到标题匹配 %r 的窗口，可见窗口有：%s"
                  % (title, ", ".join(sorted({window_title(h) for h in candidates})[:12])), 3)


def window_rect(hwnd):
    rect = wt.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise UiError("GetWindowRect 失败", 4)
    return rect.left, rect.top, rect.right, rect.bottom


def client_rect(hwnd):
    """返回客户区在屏幕坐标系里的 left/top/width/height。"""
    rc = wt.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rc)):
        raise UiError("GetClientRect 失败", 4)
    origin = wt.POINT(0, 0)
    if not user32.ClientToScreen(hwnd, ctypes.byref(origin)):
        raise UiError("ClientToScreen 失败", 4)
    return origin.x, origin.y, rc.right - rc.left, rc.bottom - rc.top


def describe(hwnd):
    left, top, right, bottom = window_rect(hwnd)
    cx, cy, cw, ch = client_rect(hwnd)
    return ("hwnd=0x%X pid=%d class=%s title=%r window=(%d,%d) %dx%d "
            "client_origin=(%d,%d) client=%dx%d"
            % (hwnd, window_pid(hwnd), window_class(hwnd), window_title(hwnd),
               left, top, right - left, bottom - top, cx, cy, cw, ch))


def _grab(hdc_source, src_x, src_y, width, height):
    """从某个 DC 抠一块出来，返回 top-down 的 24bpp RGB 字节串。"""
    hdc_mem = gdi32.CreateCompatibleDC(hdc_source)
    hbitmap = gdi32.CreateCompatibleBitmap(hdc_source, width, height)
    try:
        gdi32.SelectObject(hdc_mem, hbitmap)
        if not gdi32.BitBlt(hdc_mem, 0, 0, width, height, hdc_source, src_x, src_y, SRCCOPY):
            raise UiError("BitBlt 失败", 4)
        return _dib(hdc_mem, hbitmap, width, height)
    finally:
        gdi32.DeleteObject(hbitmap)
        gdi32.DeleteDC(hdc_mem)


def _dib(hdc_mem, hbitmap, width, height):
    # 负数高度 = 让 GetDIBits 直接按 top-down 输出，省一次整幅翻转。
    header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 24, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(width * height * 3)
    got = gdi32.GetDIBits(hdc_mem, hbitmap, 0, height, buf, header, 0)
    if got != height:
        raise UiError("GetDIBits 只拿到 %s/%s 行" % (got, height), 4)
    return buf.raw


def capture_window(hwnd):
    """优先 PrintWindow（被遮挡/在后台也能截），失败再退回屏幕抠图。"""
    left, top, right, bottom = window_rect(hwnd)
    width, height = right - left, bottom - top
    if width <= 0 or height <= 0:
        raise UiError("窗口尺寸无效：%dx%d（可能已最小化）" % (width, height), 4)

    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbitmap = gdi32.CreateCompatibleBitmap(hdc_screen, width, height)
    try:
        gdi32.SelectObject(hdc_mem, hbitmap)
        if user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT):
            try:
                return width, height, _dib(hdc_mem, hbitmap, width, height), "PrintWindow"
            except UiError:
                pass  # 某些窗口（如资源管理器）会「成功但空位图」，退回屏幕抠图
    finally:
        gdi32.DeleteObject(hbitmap)
        gdi32.DeleteDC(hdc_mem)

    return width, height, _grab(hdc_screen, left, top, width, height), "BitBlt"


def capture_screen():
    width = user32.GetSystemMetrics(0)
    height = user32.GetSystemMetrics(1)
    hdc_screen = user32.GetDC(0)
    return width, height, _grab(hdc_screen, 0, 0, width, height), "screen"


# --------------------------------------------------------------------------
# PNG 输出（stdlib 手写，避免引入 Pillow）
# --------------------------------------------------------------------------


def write_png(path, width, height, rgb):
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0
        raw += rgb[y * stride:(y + 1) * stride]
    compressed = zlib.compress(bytes(raw), 6)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", ihdr))
        handle.write(chunk(b"IDAT", compressed))
        handle.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# 子命令
# --------------------------------------------------------------------------


def cmd_shot(args):
    if args.all:
        width, height, rgb, via = capture_screen()
    else:
        hwnd = find_window(args.window, args.pid)
        print(describe(hwnd))
        width, height, rgb, via = capture_window(hwnd)

    out = args.out or time.strftime("ui_shot_%Y%m%d_%H%M%S.png")
    out = os.path.abspath(out)
    write_png(out, width, height, rgb)
    print("截图 %dx%d via %s -> %s" % (width, height, via, out))
    return 0


def cmd_map(args):
    hwnd = find_window(args.window, args.pid)
    print(describe(hwnd))
    width, height, rgb, via = capture_window(hwnd)
    stride = width * 3

    x0, y0, x1, y1 = 0, 0, width, height
    if args.region:
        parts = [int(v) for v in args.region.split(",")]
        if len(parts) != 4:
            raise UiError("--region 需要 x,y,w,h", 2)
        x0 = max(0, parts[0])
        y0 = max(0, parts[1])
        x1 = min(width, x0 + parts[2])
        y1 = min(height, y0 + parts[3])

    step = max(2, args.step)
    print("via %s, step=%d, region=(%d,%d)-(%d,%d)" % (via, step, x0, y0, x1, y1))
    ruler = [" "] * ((x1 - x0) // step + 1)
    for x in range(x0, x1, step * 10):
        label = str(x)
        at = (x - x0) // step
        for i, ch in enumerate(label):
            if at + i < len(ruler):
                ruler[at + i] = ch
    print("      x:" + "".join(ruler))

    for gy in range(y0, y1, step):
        cells = []
        for gx in range(x0, x1, step):
            total = 0
            count = 0
            for y in range(gy, min(gy + step, y1), 2):
                base = y * stride
                for x in range(gx, min(gx + step, x1), 2):
                    p = base + x * 3
                    b, g, r = rgb[p], rgb[p + 1], rgb[p + 2]
                    total += (r * 299 + g * 587 + b * 114) // 1000
                    count += 1
            lum = total // max(count, 1)
            cells.append(RAMP[min(lum * len(RAMP) // 256, len(RAMP) - 1)])
        print("y=%4d |%s|" % (gy, "".join(cells)))
    return 0


def cmd_where(args):
    print("screen: %dx%d" % (user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)))
    cursor = wt.POINT()
    user32.GetCursorPos(ctypes.byref(cursor))
    print("cursor: %d,%d" % (cursor.x, cursor.y))

    if args.coords:
        x, y = args.coords
        hwnd = user32.WindowFromPoint(wt.POINT(x, y))
        if not hwnd:
            print("(%d,%d) 处没有窗口" % (x, y))
            return 0
        root = user32.GetAncestor(hwnd, GA_ROOT) or hwnd
        print("(%d,%d) -> %s" % (x, y, describe(root)))

    if args.window or args.pid:
        hwnd = find_window(args.window, args.pid)
        print("目标窗口: %s" % describe(hwnd))
    return 0


def cmd_click(args):
    if args.client:
        if not (args.window or args.pid):
            raise UiError("--client 需要配合 --window 或 --pid 使用", 2)
        if len(args.client) != 2:
            raise UiError("--client 需要 X,Y", 2)
        hwnd = find_window(args.window, args.pid)
        cx, cy, cw, ch = client_rect(hwnd)
        rel_x, rel_y = args.client
        if not (0 <= rel_x < cw and 0 <= rel_y < ch):
            raise UiError("客户区坐标 (%d,%d) 超出客户区 %dx%d，放弃点击"
                          % (rel_x, rel_y, cw, ch), 5)
        x, y = cx + rel_x, cy + rel_y
        target_hwnd = hwnd
        print("目标窗口: %s" % describe(hwnd))
        print("客户区 (%d,%d) + 原点 (%d,%d) -> 屏幕 (%d,%d)" % (rel_x, rel_y, cx, cy, x, y))
    else:
        if not args.coords or len(args.coords) != 2:
            raise UiError("需要绝对坐标 X Y，或 --window ... --client X,Y", 2)
        x, y = args.coords
        hwnd = user32.WindowFromPoint(wt.POINT(x, y))
        target_hwnd = user32.GetAncestor(hwnd, GA_ROOT) if hwnd else 0
        print("屏幕 (%d,%d) 命中: %s"
              % (x, y, describe(target_hwnd) if target_hwnd else "无窗口"))

    if args.dry_run:
        print("--dry-run：未执行点击")
        return 0

    if args.post:
        if not args.client:
            raise UiError("--post 需要 --window ... --client X,Y（消息坐标是客户区坐标）", 2)
        post_click(target_hwnd, args.client[0], args.client[1], args.double)
        print("已投递点击事件 客户区(%d,%d)%s"
              % (args.client[0], args.client[1], "（双击）" if args.double else ""))
        return 0

    if not args.no_activate and target_hwnd:
        if activate_window(target_hwnd):
            print("窗口已激活到前台")
        else:
            print("警告: 没能把窗口提到前台，若点击无反应请改用 --post")

    user32.SetCursorPos(x, y)
    time.sleep(0.05)
    bursts = 2 if args.double else 1
    for index in range(bursts):
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.03)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        if index + 1 < bursts:
            time.sleep(0.06)
    print("已点击 (%d,%d)%s" % (x, y, "（双击）" if args.double else ""))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="ui.py",
        description="SoundBridge GUI 截图/量坐标/点击工具（零第三方依赖）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="提示：不知道按钮坐标时先跑 `map`，按字符图读出像素位置。")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_window_opts(p):
        p.add_argument("--window", "-w", help="窗口标题（精确优先，子串兜底）")
        p.add_argument("--pid", type=int, help="按进程号定位窗口")

    p_shot = sub.add_parser("shot", help="截图存 PNG")
    add_window_opts(p_shot)
    p_shot.add_argument("--out", "-o", help="输出文件，默认 ui_shot_<时间戳>.png")
    p_shot.add_argument("--all", action="store_true", help="截整个主屏而不是某个窗口")
    p_shot.set_defaults(func=cmd_shot)

    p_map = sub.add_parser("map", help="打印亮度字符图，用来量控件坐标")
    add_window_opts(p_map)
    p_map.add_argument("--step", type=int, default=10, help="每格像素数，默认 10")
    p_map.add_argument("--region", help="只看一块区域：x,y,w,h")
    p_map.set_defaults(func=cmd_map)

    p_where = sub.add_parser("where", help="查询坐标命中哪个窗口 / 打印窗口位置")
    add_window_opts(p_where)
    p_where.add_argument("coords", nargs="*", type=int, help="屏幕坐标 X Y（可选）")
    p_where.set_defaults(func=cmd_where)

    p_click = sub.add_parser("click", help="点击绝对坐标或客户区相对坐标")
    add_window_opts(p_click)
    p_click.add_argument("coords", nargs="*", type=int, help="屏幕绝对坐标 X Y")
    p_click.add_argument("--client", type=lambda s: [int(v) for v in s.split(",")],
                         help="客户区相对坐标 X,Y（配合 --window）")
    p_click.add_argument("--dry-run", action="store_true", help="只报命中窗口，不点击")
    p_click.add_argument("--double", action="store_true", help="双击")
    p_click.add_argument("--no-activate", action="store_true",
                         help="点击前不把窗口提到前台（默认会提，否则后台窗口的点击会被上层窗口截走）")
    p_click.add_argument("--post", action="store_true",
                         help="用消息投递代替真实鼠标（窗口被遮挡或抢不到前台时用）")
    p_click.set_defaults(func=cmd_click)

    return parser


def main(argv=None):
    set_dpi_aware()
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except UiError as exc:
        print("错误: %s" % exc, file=sys.stderr)
        return exc.code


if __name__ == "__main__":
    sys.exit(main())
