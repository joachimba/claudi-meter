"""patch_font_lvgl9.py NEW.c OLD.c OUT.c — convert lv_font_conv 1.5.3 (LVGL 8) output to
the LVGL 9 form used in firmware/src, keeping OLD's line_height/base_line so the
extra glyph ranges don't change layout."""
import re, sys

new_path, old_path, out_path = sys.argv[1:4]
src = open(new_path).read()
old = open(old_path).read()

lh = re.search(r"\.line_height = (\d+),", old).group(1)
bl = re.search(r"\.base_line = (\d+),", old).group(1)

src = re.sub(r"#if LVGL_VERSION_MAJOR == 8\n/\*Store all the custom data of the font\*/\n"
             r"static  lv_font_fmt_txt_glyph_cache_t cache;\n#endif\n", "", src)
src = re.sub(r"#if LVGL_VERSION_MAJOR >= 8\n(static const lv_font_fmt_txt_dsc_t font_dsc = \{)\n"
             r"#else\n.*?\n#endif\n", r"\1\n", src)
src = re.sub(r",\n#if LVGL_VERSION_MAJOR == 8\n    \.cache = &cache\n#endif\n", ",\n", src)
src = re.sub(r"#if LVGL_VERSION_MAJOR >= 8\n(const lv_font_t \w+ = \{)\n#else\n.*?\n#endif\n",
             r"\1\n", src)
src = re.sub(r"#if !\(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0\)\n"
             r"    \.subpx = LV_FONT_SUBPX_NONE,\n#endif\n",
             "    .subpx = LV_FONT_SUBPX_NONE,\n    .release_glyph = NULL,\n    .kerning = 0,\n", src)
src = re.sub(r"#if LV_VERSION_CHECK\(7, 4, 0\) \|\| LVGL_VERSION_MAJOR >= 8\n(.*?\n.*?\n)#endif\n",
             r"\1", src)
src = re.sub(r"#if LV_VERSION_CHECK\(8, 2, 0\) \|\| LVGL_VERSION_MAJOR >= 9\n(    \.fallback = NULL,\n)#endif\n",
             r"\1", src)
src = re.sub(r"\.line_height = \d+,", f".line_height = {lh},", src)
src = re.sub(r"\.base_line = \d+,", f".base_line = {bl},", src)

tail = src[src.index("ALL CUSTOM DATA"):]
assert "LVGL_VERSION" not in tail and "LV_VERSION_CHECK" not in tail, "unpatched guard left"
assert ".release_glyph = NULL" in tail and ".user_data = NULL" in tail
open(out_path, "w").write(src)
print(f"{out_path}: line_height={lh} base_line={bl}")
