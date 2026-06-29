#!/usr/bin/env python3
"""Generate logos/gametank.svg for ES-DE.

ES-DE renders theme logo SVGs with nanosvg, which does NOT render <text> — so the
wordmark is drawn as pixel-block <rect>s (paths-only). White fill lets the theme
recolor it via ${systemLogoColor}. Run:  python3 gen-logo.py > logos/gametank.svg
"""

# 5x7 block glyphs for the letters in the wordmark.
GLYPHS = {
    'G': ["01110", "10001", "10000", "10011", "10001", "10001", "01110"],
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'M': ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'N': ["10001", "11001", "10101", "10101", "10011", "10001", "10001"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
}
TEXT = "GAMETANK"
CW = CH = 14       # cell (pixel) size
PAD = 30


def text_width(t):
    x = 0
    for ch in t:
        x += CW * 3 if ch == ' ' else CW * 5 + CW
    return x - CW  # drop trailing gap


def letter(ch, ox, oy):
    out = []
    for r, row in enumerate(GLYPHS[ch]):
        for c, v in enumerate(row):
            if v == '1':
                out.append(f'<rect x="{ox + c * CW}" y="{oy + r * CH}" '
                           f'width="{CW}" height="{CH}"/>')
    return out


def main():
    w = text_width(TEXT) + PAD * 2
    h = 7 * CH + PAD * 2
    rects, x = [], PAD
    for ch in TEXT:
        if ch == ' ':
            x += CW * 3
            continue
        rects += letter(ch, x, PAD)
        x += CW * 5 + CW
    print('<?xml version="1.0" encoding="utf-8"?>')
    print(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}">')
    print('<g fill="#ffffff">')
    print("\n".join(rects))
    print('</g>')
    print('</svg>')


if __name__ == '__main__':
    main()
