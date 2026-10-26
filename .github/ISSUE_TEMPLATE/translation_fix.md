---
name: Translation fix
about: A wrong, clumsy or too long text in the menu in your language
title: "[Translation] "
labels: translation
---

<!-- Thanks! The menu texts were first machine-translated, so every fix from a native speaker helps. -->
<!-- You do not need git: paste the lines below, or attach your edited lang file (xx_yy.po). -->

## Language

The language code (as the game names it, e.g. `de_de`, `ru_ru`, `ko_kr`, `pt_br`):

## The fixes

One block per text. Copy the English line (`msgid`) and the current text (`msgstr`) from
[`lang/<code>.po`](../tree/dev/lang), then write your text:

```
msgid "Sharpen strength"
msgstr "the current text"
better: "your text"
```

Rules: keep every `%d`, `%ls`, `%hs`, `%.1f` ... in the same order, and `%%` as it is. Keep texts short: the menu
columns are narrow.

## Tested in the game? (optional)

Put your file at `bin\win_x64\lang\<code>.po` next to `dinput8.dll` and open the menu: the mod uses your file
at once. Did it fit on screen?

- [ ] Yes, I tested it in the game
- [ ] No
