#!/usr/bin/env python3
"""settings_check.py (issue #26) -- every setting the menu / a hotkey changes survives a game restart.

  python scripts/settings_check.py      fails (exit 1) when one of the checks below finds a gap (CI)

Save (SaveSettings), Reset (ResetSettings) and the startup read (LoadConfig) in src/inject.cpp are three hand-kept lists of
dlaa.ini keys. Issue #26: the See-through depth row set its "explicit" mark and Reset removed `mv_fwd_depth`, but Save never
wrote it, so the choice was gone after a restart. The checks:

  1. every key Reset removes is written by Save (or saves itself at once: SELF_SAVED below)
  2. every key Save writes is removed by Reset (Reset to defaults really resets it)
  3. every key Save writes is read back by LoadConfig
  4. every "explicit" mark a menu row / hotkey sets (g_xxxExplicit = true, g_profExplicit[PK_X] = true outside LoadConfig)
     is looked at by Save -- the mark means "the user's choice", Save must write it
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC = os.path.join(ROOT, "src", "inject.cpp")

# keys Reset removes that Save does not write, because their row writes dlaa.ini itself at once
SELF_SAVED = {
    "mode": "CycleMode (the Mode row / End) writes it at once",
}

_FUNC = re.compile(r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w:<>\s\*&]*?\b(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{\s*(?://.*)?$")


def function_body(lines, name):
    """The lines of the top-level function `name` (its definition line .. the closing brace at column 0)."""
    for i, ln in enumerate(lines):
        m = _FUNC.match(ln)
        if m and m.group(1) == name:
            for j in range(i + 1, len(lines)):
                if lines[j].startswith("}"):
                    return i, lines[i:j + 1]
    sys.exit("settings_check: function %s not found in src/inject.cpp" % name)


def enclosing_function(lines, idx):
    for i in range(idx, -1, -1):
        m = _FUNC.match(lines[i])
        if m:
            return m.group(1)
    return ""


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def main():
    with open(SRC, encoding="utf-8") as f:
        lines = f.read().split("\n")

    _, save = function_body(lines, "SaveSettings")
    _, reset = function_body(lines, "ResetSettings")
    _, load = function_body(lines, "LoadConfig")
    save_txt, reset_txt, load_txt = (strip_comments("\n".join(b)) for b in (save, reset, load))

    saved = set(re.findall(r'\badd\("(\w+)"\)', save_txt))
    m = re.search(r"kKeys\[\]\s*=\s*\{(.*?)\};", reset_txt, re.S)
    if not m:
        sys.exit("settings_check: ResetSettings has no kKeys[] list")
    reset_keys = set(re.findall(r'"(\w+)"', m.group(1)))
    loaded = set(re.findall(r'(?:strcmp|_stricmp)\(\s*key\s*,\s*"(\w+)"\s*\)', load_txt))
    if not saved or not reset_keys or not loaded:
        sys.exit("settings_check: could not read the key lists (saved %d, reset %d, loaded %d)"
                 % (len(saved), len(reset_keys), len(loaded)))

    errors = []
    for k in sorted(reset_keys - saved - set(SELF_SAVED)):
        errors.append("1. Reset removes `%s` but Save never writes it: the user's choice is lost at the next start "
                      "(add it to SaveSettings, or to SELF_SAVED here when its row writes dlaa.ini itself)" % k)
    for k in sorted(set(SELF_SAVED) - reset_keys):
        errors.append("1. SELF_SAVED lists `%s`, but Reset does not remove it (stale entry?)" % k)
    for k in sorted(saved - reset_keys):
        errors.append("2. Save writes `%s` but Reset does not remove it: Reset to defaults keeps it (add it to kKeys[])" % k)
    for k in sorted(saved - loaded):
        errors.append("3. Save writes `%s` but LoadConfig never reads it" % k)

    # 4. the explicit marks: a statement that sets one (alone or in a chain) to true
    marks = {}
    for i, ln in enumerate(lines):
        code = ln.split("//", 1)[0]
        if not re.search(r"=\s*true\s*;", code):
            continue
        for mk in re.findall(r"(g_\w+Explicit(?:\[\w+\])?)\s*=", code):
            fn = enclosing_function(lines, i)
            if fn != "LoadConfig":
                marks.setdefault(mk, set()).add("%s (line %d)" % (fn, i + 1))
    for mk in sorted(marks):
        if mk not in save_txt:
            errors.append("4. %s is set by %s, but SaveSettings never looks at it: the change is not saved"
                          % (mk, ", ".join(sorted(marks[mk]))))

    if errors:
        print("settings_check: %d problem(s) in src/inject.cpp" % len(errors))
        for e in errors:
            print("  " + e)
        return 1
    print("settings_check: OK -- Save writes %d keys, Reset removes %d, LoadConfig reads them back; %d explicit marks "
          "all saved" % (len(saved), len(reset_keys), len(marks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
