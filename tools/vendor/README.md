# 日历资源来源 / Calendar resource provenance

| 输入 / Input | 固定来源 / Pinned source | 许可 / License |
| --- | --- | --- |
| `lunar-javascript/lunar.cjs` | npm `lunar-javascript@1.7.7`, upstream `6tail/lunar-javascript`; SHA-256 `9750324bfe1aa63c146f8c72b1143df924466c11c8a5277d7d9225c541a18aaa` | [MIT](lunar-javascript/LICENSE), Copyright (c) 2018 6tail |
| `libre-bodoni/digits.ttf` | Libre Bodoni 2.005, Google Fonts `ofl/librebodoni/LibreBodoni[wght].ttf`, 2026-11-10取得；固定wght=700后仅保留0–9，保留字体内部版权；subset SHA-256 `63c030ddb31c5e9c5dd32d5793a12d494f4ab5ddd456d7bf99a7130b06a57f94` | [SIL OFL 1.1](libre-bodoni/OFL.txt), The Libre Bodoni Project Authors |

两份输入已随仓库保存；重生成不依赖在线最新版。`generate_calendar_data.cjs` 与 `generate_calendar_digits.py` 分别生成固件只读表和数字位图。完整OFL字体不是固件运行时依赖；子集与生成字形继续受OFL约束。
Both inputs are vendored for offline reproducibility. The generators produce firmware tables and numeral bitmaps. The complete font is not a runtime dependency; the subset and generated glyphs retain OFL terms.

## 英文断词模式 / English hyphenation patterns

- [hyph-en-us.tex](hyphen-en-us/hyph-en-us.tex)：`hyphenation/tex-hyphen` [固定提交](https://github.com/hyphenation/tex-hyphen/blob/dce23b4eb2df218ec6893b851562316d7cf37355/hyph-utf8/tex/generic/hyph-utf8/patterns/tex/hyph-en-us.tex)，SHA-256 `f4ffcd96c5cbc886bdad23f95dcae8edc3cd3620eae62f7946eceda97c4e68f8`。未改动原始规则与许可声明。/ Pinned upstream American English patterns and exceptions, with unmodified rules and notices.
- 根目录运行 `python3 tools/gen_book_hyphen_en.py` 生成只读表；`--check` 校验可复现性，随 `book` 宿主回归执行。无须联网或安装断词库。/ Regenerate the read-only table from the repository root; the `book` suite runs `--check` offline without a hyphenation library.
