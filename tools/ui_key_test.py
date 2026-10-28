# -*- coding: utf-8 -*-
"""真实按键回归：向运行中的窗口投递 A / D / ← / →，截图比较（仅开发用）。"""
import ctypes, subprocess, sys, time
from PIL import ImageGrab

EXE = r"C:/Users/12654/WorkBuddy/EcgRpeak/build/EcgRpeakDetector.exe"
BIN = r"C:/Users/12654/Desktop/code/Ecgtest1/Ecgtest/202512081044406110R_前15%_真正“负P波及激变”.bin"
TITLE = "ECG R 峰检测 · Pan-Tompkins 算法"
u = ctypes.windll.user32

proc = subprocess.Popen([EXE, "--open", BIN, "--fs", "500"])
time.sleep(5)

hwnd = u.FindWindowW(None, TITLE)
if not hwnd:
    print("找不到窗口")
    proc.terminate()
    sys.exit(1)
u.ShowWindow(hwnd, 9)
u.SetForegroundWindow(hwnd)
time.sleep(1.0)

img = ImageGrab.grab()
img.save(r"C:/Users/12654/WorkBuddy/EcgRpeak/build/key_before.png")

def key(vk, times=1):
    for _ in range(times):
        u.PostMessageW(hwnd, 0x0100, vk, 0)
        u.PostMessageW(hwnd, 0x0101, vk, 0)
        time.sleep(0.35)

seq = sys.argv[1] if len(sys.argv) > 1 else "a,a,right,right"
tag = sys.argv[2] if len(sys.argv) > 2 else "after"
VK = {"a": 0x41, "d": 0x44, "left": 0x25, "right": 0x27}
for item in seq.split(","):
    key(VK[item.strip()])
time.sleep(0.6)
img2 = ImageGrab.grab()
img2.save(r"C:/Users/12654/WorkBuddy/EcgRpeak/build/key_%s.png" % tag)

proc.terminate()
print("done", img.size)
