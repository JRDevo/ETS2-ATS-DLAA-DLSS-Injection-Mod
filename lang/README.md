# Translations of the in-game menu

The tuning menu (the `Delete` key in the game) is available in **all 43 languages of ETS2 and ATS**.

> [!WARNING]
> **Best effort — help wanted.** The translations were created by **Claude** (AI) and reviewed by **Grok**,
> **Google Gemini** and **ChatGPT**. An AI can never be as accurate as a native speaker, so some texts may still
> be wrong, clumsy or too formal. If you have a change or a suggestion, please open a new translation issue:
> it helps every player of your language.
> **[Report a translation problem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml)**

## The languages

| Code | Language | Status |
|---|---|---|
| `en_us`, `en_gb` | English | original |
| `bg_bg` | Български | AI draft |
| `ca_es` | Català | AI draft |
| `cs_cz` | Česky | AI draft |
| `da_dk` | Dansk | AI draft |
| `de_de` | Deutsch | AI draft |
| `el_gr` | Ελληνικά | AI draft |
| `es_es` | Español | AI draft |
| `es_la` | Español (Am. Latina) | AI draft |
| `et_ee` | Eesti | AI draft |
| `eu_es` | Euskara | AI draft |
| `fi_fi` | Suomi | AI draft |
| `fr_ca` | Français (Québec) | AI draft |
| `fr_fr` | Français (France) | AI draft |
| `gl_es` | Galego | AI draft |
| `hr_hr` | Hrvatski | AI draft |
| `hu_hu` | Magyar | AI draft |
| `it_it` | Italiano | AI draft |
| `ja_jp` | 日本語 | AI draft |
| `ka_ge` | ქართული | AI draft |
| `ko_kr` | 한국어 | AI draft |
| `lt_lt` | Lietuviškai | AI draft |
| `lv_lv` | Latviešu | AI draft |
| `mk_mk` | Македонски | AI draft |
| `nl_nl` | Nederlands | AI draft |
| `no_no` | Norsk (bokmål) | AI draft |
| `pl_pl` | Polski | AI draft |
| `pl_si` | Ślōnskŏ gŏdka | AI draft |
| `pt_br` | Português (Brasil) | AI draft |
| `pt_pt` | Português (Portugal) | AI draft |
| `ro_ro` | Română | AI draft |
| `ru_ru` | Русский | AI draft |
| `sk_sk` | Slovenčina | AI draft |
| `sl_sl` | Slovenščina | AI draft |
| `sr_sp` | Srpski (latinica) | AI draft |
| `sr_sr` | Српски (ћирилица) | AI draft |
| `sv_se` | Svenska | AI draft |
| `tr_tr` | Türkçe | AI draft |
| `uk_uk` | Українська | AI draft |
| `vi_vn` | Tiếng Việt | AI draft |
| `zh_cn` | 简体中文 | AI draft |
| `zh_tw` | 繁體中文 | AI draft |

A language moves from "AI draft" to "checked" once a native speaker has gone through it.

## How the language is chosen

- **Automatic (the default).** The menu uses **the game's own language**: set ETS2 / ATS to Polish and the menu
  is Polish. The mod reads it from the game, not from Windows, so it also works under Linux / Proton.
- **The `Language` row** in the menu (below `Logging`): `Left` / `Right` pick another language, `Save` keeps it.
  The first choice, `auto`, goes back to the game's language.
- **`dlaa.ini`:** `language = auto`, or a code from the table, for example `language = de_de`.
- **Key names** (`Ctrl+F9`, `Delete`) follow your **keyboard**: a German keyboard shows `Strg+F9` and `Entf`.
  Under Linux / Proton they stay in English.
- **Fonts.** Windows has the fonts for every language. **Linux / Proton:** Chinese, Japanese and Korean need
  extra fonts. Without them the menu stays English and the `Language` row says `font missing`. Install them once
  per game (227300 = ETS2, 270880 = ATS):

  ```
  protontricks 227300 cjkfonts
  protontricks 270880 cjkfonts
  ```

## What to change

Each language is one plain text file in this folder: `<code>.po` (for example [`de_de.po`](de_de.po)). Any
text editor opens it; [Poedit](https://poedit.net) makes it easier. One entry looks like this:

```
#. Setting / action name column (about 26 Latin characters)
#: src/inject.cpp
msgid "Sharpen strength"
msgstr "Schärfungsstärke"
```

- `msgid` = the English text. **Do not change it.**
- `msgstr` = the text in your language. **Change only this line.**
- The `#.` line says where the text is shown and how much room it has. The menu columns are narrow:
  keep names and values short.
- **Placeholders:** keep every `%d`, `%ls`, `%hs`, `%.1f`, `%+d` ... in the **same order**, and keep `%%`
  (that is a percent sign). If a line breaks this rule, the mod shows the English text for that line instead.
- Keep these as they are: DLAA, DLSS, VR, NVIDIA, Reflex, OpenXR, OFXR Bridge, VectorXR, fps, ms, and every
  `dlaa.ini` name (`mirror_turns`, `log_level` ...).
- An empty `msgstr ""` means "not translated yet": the menu shows the English text there.

## Test your changes in the game

1. Make a folder `lang` next to `dinput8.dll` (in `bin\win_x64\`).
2. Put your edited file there, for example `bin\win_x64\lang\de_de.po`.
3. Open the menu (`Delete`). Your file wins over the built-in texts, line by line. Each time you open the menu,
   the mod reads the file again: edit, close the menu, open it again, look.

## Send your fixes

- **The easy way: [Report a translation problem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).** Pick the
  language, say what is wrong and what it should say, and add a **screenshot** of the menu that shows the spot.
  You can also attach your edited `.po` file there. No git needed.
- **With git:** a pull request against the `dev` branch that changes your `<code>.po` file.

`python scripts/lang_tool.py check` checks a file before you send it (placeholders, file format).

## For developers

- New menu text in the code goes into `TR(L"...")`, or `TRF(L"...")` when it is a printf format.
- `python scripts/lang_tool.py extract` updates [`template.pot`](template.pot) (every English text);
  `python scripts/lang_tool.py update` adds the new texts to every `.po` file (empty = English until translated).
- The build packs all `.po` files into the DLL (`lang_tool.py pack`). Details: [`docs/DEVELOPMENT.md`](../docs/DEVELOPMENT.md),
  phase 74.
