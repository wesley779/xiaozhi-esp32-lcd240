"""重新生成待机界面用的中文字库（main/display/idle_font_16.c）。

字符集 = main/display/lcd_display.cc 里出现的全部非 ASCII 字符 + ASCII 0x20-0x7E，
所以往屏上加新中文文案后，跑一遍本脚本就能避免"新字显示空白"。

用法：
    python tools/gen_idle_font.py
环境变量（可选）：
    NPX         指定 npx 可执行文件（默认从 PATH 找）
    FONT_TTF    指定字体文件（默认 Windows 自带的 simhei.ttf）
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "main" / "display" / "lcd_display.cc"
OUT = ROOT / "main" / "display" / "idle_font_16.c"
FONT = os.environ.get("FONT_TTF", r"C:\Windows\Fonts\simhei.ttf")
SIZE = "16"
BPP = "4"


def find_npx() -> str:
    explicit = os.environ.get("NPX")
    if explicit and Path(explicit).exists():
        return explicit
    found = shutil.which("npx")
    if found:
        return found
    # Windows 上 shutil.which 也能找到 npx.cmd，但真找不到时给出可读的报错
    raise SystemExit("找不到 npx，请安装 Node.js 或用 NPX 环境变量指定 npx.cmd 的路径")


def main() -> None:
    if not SRC.exists():
        raise SystemExit(f"找不到源文件: {SRC}")
    if not Path(FONT).exists():
        raise SystemExit(f"找不到字体: {FONT}（可用 FONT_TTF 环境变量指定）")

    text = SRC.read_text(encoding="utf-8")
    symbols = "".join(sorted({c for c in text if ord(c) > 127}))
    print(f"symbols: {len(symbols)}")

    cmd = [
        find_npx(), "--yes", "lv_font_conv@1.5.3",
        "--font", FONT,
        "--size", SIZE,
        "--bpp", BPP,
        "--format", "lvgl",
        "--no-compress",
        "--no-kerning",
        "--range", "0x20-0x7e",
        "--symbols", symbols,
        "--lv-include", "lvgl.h",
        "--lv-font-name", "idle_font_16",
        "-o", str(OUT),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(res.stdout[-3000:])
    print(res.stderr[-3000:])
    if res.returncode != 0:
        raise SystemExit(f"lv_font_conv 失败，exit={res.returncode}")
    print(f"生成完成: {OUT} ({OUT.stat().st_size} B)")


if __name__ == "__main__":
    main()
