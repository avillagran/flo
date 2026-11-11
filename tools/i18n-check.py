#!/usr/bin/env python3
"""Checks Flo's translation catalogs (i18n/<code>.json) against the source
strings (i18n/strings.json, from tools/i18n-extract.py).

For every language of Omarchy's registry (the list Folio's translator
follows) it reports: missing or unknown strings, placeholders (%1..%9, %n)
that differ from the English, and counted strings whose plural forms are
not exactly the CLDR categories the language uses (the rules of
fo::i18n::plural_category and Folio's translator.cpp).

    tools/i18n-check.py            # every language, a summary line each
    tools/i18n-check.py es-MX ja   # just these, with every problem
Exits 1 when any catalog checked has a problem.
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
LANGUAGES = ["en", "da", "ar", "fi", "fr", "el", "hu", "hi", "is", "ja", "ko", "es-MX", "fil", "pt", "sv", "tr",
             "vi", "ur", "bn", "ca", "si", "ta", "th", "uz", "it", "zh-CN", "pl", "lt", "ga", "nl", "no", "ru", "id",
             "de", "sw", "fa", "ms", "uk", "ro", "cs", "sr", "he", "bg", "sk", "hr", "sl", "zh-TW", "et", "lv",
             "pt-BR"]


def categories(code):
    if code in ("ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"):
        return {"other"}
    if code in ("fr", "pt-BR", "ca", "es-MX", "it", "pt"):
        return {"one", "many", "other"}
    if code in ("cs", "sk", "hr", "sr", "lt", "ro"):
        return {"one", "few", "other"}
    if code in ("pl", "ru", "uk"):
        return {"one", "few", "many", "other"}
    if code == "sl":
        return {"one", "two", "few", "other"}
    if code == "lv":
        return {"zero", "one", "other"}
    if code == "ar":
        return {"zero", "one", "two", "few", "many", "other"}
    if code == "he":
        return {"one", "two", "other"}
    if code == "ga":
        return {"one", "two", "few", "many", "other"}
    return {"one", "other"}


marker = re.compile(r"%[1-9n]")


def check(code, sources, verbose):
    path = root / "i18n" / f"{code}.json"
    problems = []
    if not path.exists():
        return [f"{code}: no catalog"]
    try:
        cat = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as e:
        return [f"{code}: not valid JSON: {e}"]
    if not isinstance(cat, dict):
        return [f"{code}: not a JSON object"]
    want = categories(code)
    for key in cat:
        if key not in sources:
            problems.append(f"{code}: unknown string {key!r}")
    for src, plural in sources.items():
        if code == "en" and not plural:
            continue  # English shows the source itself
        if src not in cat:
            problems.append(f"{code}: missing {src!r}")
            continue
        value = cat[src]
        need = sorted(set(marker.findall(src)) - ({"%n"} if plural else set()))
        if plural:
            if not isinstance(value, dict):
                problems.append(f"{code}: {src!r} needs plural forms {sorted(want)}")
                continue
            if set(value) != want:
                problems.append(f"{code}: {src!r} has forms {sorted(value)}, wants {sorted(want)}")
            for form, text in value.items():
                if not isinstance(text, str) or not text.strip():
                    problems.append(f"{code}: {src!r} form {form} is empty")
                    continue
                got = sorted(set(marker.findall(text)) - {"%n"})
                if got != need:
                    problems.append(f"{code}: {src!r} form {form} has {got}, wants {need}")
                if form == "other" and "%n" not in text:
                    problems.append(f"{code}: {src!r} form other lacks %n")
        else:
            if not isinstance(value, str) or not value.strip():
                problems.append(f"{code}: {src!r} is empty or not text")
                continue
            got = sorted(set(marker.findall(value)))
            if got != need:
                problems.append(f"{code}: {src!r} has {got}, wants {need}")
    return problems


def main(argv):
    sources = {x["source"]: x["plural"] for x in json.loads((root / "i18n" / "strings.json").read_text("utf-8"))}
    codes = argv or LANGUAGES
    bad = 0
    for code in codes:
        if code not in LANGUAGES:
            print(f"{code}: not one of Omarchy's languages")
            bad += 1
            continue
        problems = check(code, sources, bool(argv))
        if problems:
            bad += 1
            if argv:
                print("\n".join(problems))
            else:
                print(f"{code}: {len(problems)} problems (first: {problems[0]})")
        elif argv:
            print(f"{code}: ok")
    if not argv:
        print(f"{len(codes) - bad}/{len(codes)} catalogs complete")
    return 1 if bad else 0


sys.exit(main(sys.argv[1:]))
