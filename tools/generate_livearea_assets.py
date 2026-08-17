#!/usr/bin/env python3
"""Generate the Vita NS Controller LiveArea artwork without external tools."""

from __future__ import annotations

import binascii
import pathlib
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSET_ROOT = ROOT / "controller_app" / "sce_sys"
LIVEAREA_ROOT = ASSET_ROOT / "livearea" / "contents"

BG = 0
PANEL = 1
BORDER = 2
BLUE = 3
GREEN = 4
TEXT = 5
MUTED = 6
DARK_BLUE = 7
DARK_GREEN = 8
SOFT_PANEL = 9

PALETTE = (
    (14, 18, 28),
    (38, 46, 60),
    (92, 106, 128),
    (26, 132, 220),
    (28, 176, 112),
    (242, 246, 255),
    (170, 183, 204),
    (15, 50, 81),
    (15, 67, 48),
    (48, 58, 76),
)


FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "N": ("10001", "11001", "11001", "10101", "10011", "10011", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
}


class Canvas:
    def __init__(self, width: int, height: int, fill: int = BG) -> None:
        self.width = width
        self.height = height
        self.pixels = bytearray([fill]) * (width * height)

    def set(self, x: int, y: int, color: int) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            self.pixels[y * self.width + x] = color

    def rect(self, x: int, y: int, width: int, height: int, color: int) -> None:
        left = max(0, x)
        top = max(0, y)
        right = min(self.width, x + width)
        bottom = min(self.height, y + height)
        if left >= right or top >= bottom:
            return
        row = bytes([color]) * (right - left)
        for py in range(top, bottom):
            start = py * self.width + left
            self.pixels[start : start + len(row)] = row

    def circle(self, cx: int, cy: int, radius: int, color: int) -> None:
        radius_sq = radius * radius
        for py in range(max(0, cy - radius), min(self.height, cy + radius + 1)):
            dy_sq = (py - cy) * (py - cy)
            for px in range(max(0, cx - radius), min(self.width, cx + radius + 1)):
                if (px - cx) * (px - cx) + dy_sq <= radius_sq:
                    self.set(px, py, color)

    def rounded_rect(
        self, x: int, y: int, width: int, height: int, radius: int, color: int
    ) -> None:
        radius = max(0, min(radius, width // 2, height // 2))
        if radius == 0:
            self.rect(x, y, width, height, color)
            return
        self.rect(x + radius, y, width - 2 * radius, height, color)
        self.rect(x, y + radius, width, height - 2 * radius, color)
        self.circle(x + radius, y + radius, radius, color)
        self.circle(x + width - radius - 1, y + radius, radius, color)
        self.circle(x + radius, y + height - radius - 1, radius, color)
        self.circle(x + width - radius - 1, y + height - radius - 1, radius, color)

    def line(
        self, x0: int, y0: int, x1: int, y1: int, width: int, color: int
    ) -> None:
        dx = x1 - x0
        dy = y1 - y0
        length_sq = dx * dx + dy * dy
        radius = width / 2.0
        radius_sq = radius * radius
        left = max(0, min(x0, x1) - width)
        right = min(self.width, max(x0, x1) + width + 1)
        top = max(0, min(y0, y1) - width)
        bottom = min(self.height, max(y0, y1) + width + 1)
        for py in range(top, bottom):
            for px in range(left, right):
                if length_sq == 0:
                    distance_sq = (px - x0) ** 2 + (py - y0) ** 2
                else:
                    dot = (px - x0) * dx + (py - y0) * dy
                    amount = max(0.0, min(1.0, dot / length_sq))
                    near_x = x0 + amount * dx
                    near_y = y0 + amount * dy
                    distance_sq = (px - near_x) ** 2 + (py - near_y) ** 2
                if distance_sq <= radius_sq:
                    self.set(px, py, color)


def text_width(text: str, scale: int) -> int:
    if not text:
        return 0
    return len(text) * 5 * scale + (len(text) - 1) * scale


def draw_text(canvas: Canvas, x: int, y: int, text: str, scale: int, color: int) -> None:
    cursor = x
    for char in text:
        if char == " ":
            cursor += 4 * scale
            continue
        glyph = FONT[char]
        for row, bits in enumerate(glyph):
            for column, bit in enumerate(bits):
                if bit == "1":
                    canvas.rect(
                        cursor + column * scale,
                        y + row * scale,
                        scale,
                        scale,
                        color,
                    )
        cursor += 6 * scale


def draw_centered_text(
    canvas: Canvas, center_x: int, y: int, text: str, scale: int, color: int
) -> None:
    draw_text(canvas, center_x - text_width(text, scale) // 2, y, text, scale, color)


def draw_controller(canvas: Canvas, x: int, y: int, width: int, height: int) -> None:
    rim = max(2, height // 28)
    outer_radius = max(8, height // 3)
    canvas.rounded_rect(x, y, width, height, outer_radius, BORDER)
    canvas.rounded_rect(
        x + rim,
        y + rim,
        width - rim * 2,
        height - rim * 2,
        max(6, outer_radius - rim),
        BG,
    )

    accent_width = max(2, width // 45)
    canvas.rounded_rect(
        x + rim + 1,
        y + height // 4,
        accent_width,
        height // 2,
        accent_width // 2,
        BLUE,
    )
    canvas.rounded_rect(
        x + width - rim - accent_width - 1,
        y + height // 4,
        accent_width,
        height // 2,
        accent_width // 2,
        GREEN,
    )

    screen_x = x + width * 35 // 100
    screen_y = y + height * 18 // 100
    screen_width = width * 30 // 100
    screen_height = height * 52 // 100
    screen_rim = max(1, rim)
    canvas.rounded_rect(
        screen_x,
        screen_y,
        screen_width,
        screen_height,
        max(2, height // 16),
        BORDER,
    )
    canvas.rounded_rect(
        screen_x + screen_rim,
        screen_y + screen_rim,
        screen_width - screen_rim * 2,
        screen_height - screen_rim * 2,
        max(1, height // 16 - screen_rim),
        PANEL,
    )
    label_scale = max(1, height // 36)
    draw_centered_text(
        canvas,
        screen_x + screen_width // 2,
        screen_y + (screen_height - 7 * label_scale) // 2,
        "NS",
        label_scale,
        TEXT,
    )

    dpad_x = x + width * 20 // 100
    dpad_y = y + height * 43 // 100
    dpad_unit = max(2, height // 14)
    canvas.rounded_rect(
        dpad_x - dpad_unit // 2,
        dpad_y - dpad_unit * 3 // 2,
        dpad_unit,
        dpad_unit * 3,
        max(1, dpad_unit // 4),
        BLUE,
    )
    canvas.rounded_rect(
        dpad_x - dpad_unit * 3 // 2,
        dpad_y - dpad_unit // 2,
        dpad_unit * 3,
        dpad_unit,
        max(1, dpad_unit // 4),
        BLUE,
    )

    buttons_x = x + width * 81 // 100
    buttons_y = y + height * 42 // 100
    button_offset = max(3, height // 9)
    button_radius = max(1, height // 28)
    canvas.circle(buttons_x, buttons_y - button_offset, button_radius, GREEN)
    canvas.circle(buttons_x, buttons_y + button_offset, button_radius, GREEN)
    canvas.circle(buttons_x - button_offset, buttons_y, button_radius, GREEN)
    canvas.circle(buttons_x + button_offset, buttons_y, button_radius, GREEN)

    stick_radius = max(2, height // 11)
    for stick_x in (x + width * 28 // 100, x + width * 72 // 100):
        stick_y = y + height * 75 // 100
        canvas.circle(stick_x, stick_y, stick_radius + rim, BORDER)
        canvas.circle(stick_x, stick_y, stick_radius, PANEL)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    checksum = binascii.crc32(kind)
    checksum = binascii.crc32(data, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)


def write_indexed_png(path: pathlib.Path, canvas: Canvas) -> None:
    palette_data = bytes(channel for color in PALETTE for channel in color)
    rows = bytearray()
    for y in range(canvas.height):
        rows.append(0)
        start = y * canvas.width
        rows.extend(canvas.pixels[start : start + canvas.width])
    ihdr = struct.pack(">IIBBBBB", canvas.width, canvas.height, 8, 3, 0, 0, 0)
    payload = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"PLTE", palette_data)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + png_chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def make_icon() -> Canvas:
    canvas = Canvas(128, 128)
    canvas.circle(17, 18, 31, DARK_BLUE)
    canvas.circle(115, 112, 39, DARK_GREEN)
    canvas.rounded_rect(5, 5, 118, 118, 23, PANEL)
    canvas.rounded_rect(9, 9, 110, 110, 20, BG)
    canvas.rounded_rect(50, 17, 28, 6, 3, BORDER)
    canvas.rounded_rect(52, 19, 11, 2, 1, BLUE)
    canvas.rounded_rect(65, 19, 11, 2, 1, GREEN)
    draw_controller(canvas, 11, 33, 106, 66)
    canvas.rounded_rect(41, 106, 46, 4, 2, SOFT_PANEL)
    canvas.rounded_rect(43, 107, 19, 2, 1, BLUE)
    canvas.rounded_rect(66, 107, 19, 2, 1, GREEN)
    return canvas


def make_startup() -> Canvas:
    canvas = Canvas(280, 158)
    canvas.circle(1, 1, 94, DARK_BLUE)
    canvas.circle(279, 158, 96, DARK_GREEN)
    canvas.rounded_rect(8, 8, 264, 142, 18, PANEL)
    canvas.rounded_rect(12, 12, 256, 134, 15, BG)
    canvas.rounded_rect(12, 30, 4, 43, 2, BLUE)
    canvas.rounded_rect(12, 78, 4, 43, 2, GREEN)
    draw_controller(canvas, 20, 44, 114, 72)

    draw_text(canvas, 153, 29, "VITA", 3, BLUE)
    draw_text(canvas, 153, 56, "NS", 6, TEXT)
    draw_text(canvas, 153, 111, "CONTROLLER", 2, MUTED)
    canvas.rounded_rect(153, 136, 113, 4, 2, SOFT_PANEL)
    canvas.rounded_rect(153, 136, 52, 4, 2, BLUE)
    canvas.rounded_rect(210, 136, 56, 4, 2, GREEN)
    return canvas


def make_background() -> Canvas:
    canvas = Canvas(840, 500)
    canvas.circle(0, 80, 300, DARK_BLUE)
    canvas.circle(840, 450, 350, DARK_GREEN)
    canvas.rounded_rect(38, 36, 764, 428, 42, PANEL)
    canvas.rounded_rect(46, 44, 748, 412, 36, BG)
    draw_controller(canvas, 136, 132, 568, 238)
    canvas.rounded_rect(257, 399, 326, 10, 5, SOFT_PANEL)
    canvas.rounded_rect(263, 402, 148, 4, 2, BLUE)
    canvas.rounded_rect(429, 402, 148, 4, 2, GREEN)
    return canvas


def main() -> None:
    write_indexed_png(ASSET_ROOT / "icon0.png", make_icon())
    write_indexed_png(LIVEAREA_ROOT / "startup.png", make_startup())
    write_indexed_png(LIVEAREA_ROOT / "bg.png", make_background())


if __name__ == "__main__":
    main()
