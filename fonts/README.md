# Fonts

Sources of the bitmap fonts on the display. `tools/fontgen.py` turns them
into `main/fonts_data.cpp`; the firmware only carries the characters
listed there (ASCII, Latin-1, a few typographic characters).

| File | Font | License |
|---|---|---|
| `helvB10.bdf` … `helvB24.bdf`, `helvR12.bdf` | Adobe Helvetica bitmap fonts from the X11 distribution, ISO 10646 version by Markus Kuhn, taken from [u8g2](https://github.com/olikraus/u8g2/tree/master/tools/font/bdf) | Adobe / DEC X11 license, see the copyright notice in each file |
| `liberation-sans-bold-80-digits.bdf` | Digits of Liberation Sans Bold, rasterised at 80 px by `tools/ttf2bdf.py` | SIL Open Font License 1.1, see [LICENSE.liberation](LICENSE.liberation) |

Regenerate after changing the font list in `tools/fontgen.py`:

```sh
python3 tools/fontgen.py
```

The big digits were made with (needs Pillow):

```sh
uv run --with pillow tools/ttf2bdf.py \
    /usr/share/fonts/liberation/LiberationSans-Bold.ttf 80 "0123456789+" \
    fonts/liberation-sans-bold-80-digits.bdf
```
