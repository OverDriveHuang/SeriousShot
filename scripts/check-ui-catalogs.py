#!/usr/bin/env python3
"""Check embedded SeriousShot catalogs against a fresh, cross-platform lupdate."""

import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SOURCE_LIST = ROOT / "cmake/ui_language_sources.txt"
CATALOG_DIRECTORY = ROOT / "src/ui/qt/i18n"
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".mm"}
ID_CALL = re.compile(r"\b(?:qtTrId|QT_TRID_NOOP|QT_TRID_N_NOOP)\s*\(")
PLACEHOLDER = re.compile(r"%(?:L?[1-9][0-9]*|L?n)")


def translation_sources():
    entries = [line for line in SOURCE_LIST.read_text(encoding="utf-8").splitlines()
               if line and not line.startswith("#")]
    if len(set(entries)) != len(entries):
        raise ValueError(f"Duplicate source in {SOURCE_LIST}")
    for entry in entries:
        if not (ROOT / entry).is_file():
            raise ValueError(f"Missing translation source: {entry}")
    for folder in ("src", "tools"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                relative = path.relative_to(ROOT).as_posix()
                if relative not in entries and ID_CALL.search(path.read_text(encoding="utf-8")):
                    raise ValueError(f"Translation source absent from {SOURCE_LIST.name}: {relative}")
    return [str(ROOT / entry) for entry in entries]


def extract(lupdate, output):
    result = subprocess.run(
        [lupdate, "-I", str(ROOT / "src"), *translation_sources(), "-no-obsolete",
         "-locations", "none", "-source-language", "zh_CN", "-target-language", "zh_CN",
         "-ts", str(output)],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if result.returncode or result.stderr.strip():
        raise ValueError("lupdate reported an extraction error:\n" + result.stdout + result.stderr)


def read_catalog(path):
    document = ET.parse(path).getroot()
    messages = {}
    for message in document.iter("message"):
        message_id = message.get("id")
        if not message_id:
            raise ValueError(f"{path}: source-based message without stable ID: {message.findtext('source')!r}")
        if message_id in messages:
            raise ValueError(f"{path}: duplicate ID {message_id}")
        if not message.findtext("source"):
            raise ValueError(f"{path}: missing source annotation for {message_id}")
        messages[message_id] = message
    if not messages:
        raise ValueError(f"{path}: empty catalog")
    return document, messages


def check_catalog(path, locale, active):
    document, messages = read_catalog(path)
    if document.get("language") != locale:
        raise ValueError(f"{path}: expected language={locale}")
    missing = active.keys() - messages.keys()
    extra = messages.keys() - active.keys()
    if missing or extra:
        raise ValueError(f"{path}: missing IDs {sorted(missing)}; inactive IDs {sorted(extra)}")
    for message_id, message in messages.items():
        source = message.findtext("source")
        extracted = active[message_id]
        if source != extracted.findtext("source") or message.get("numerus") != extracted.get("numerus"):
            raise ValueError(f"{path}: source or numerus changed for {message_id}; refresh with lupdate")
        translation = message.find("translation")
        if translation is None or translation.get("type"):
            raise ValueError(f"{path}: unfinished or inactive translation for {message_id}")
        forms = translation.findall("numerusform") if message.get("numerus") == "yes" else [translation]
        if not forms:
            raise ValueError(f"{path}: no plural translations for {message_id}")
        expected = Counter(PLACEHOLDER.findall(source))
        for form in forms:
            text = form.text or ""
            if not text.strip() or Counter(PLACEHOLDER.findall(text)) != expected:
                raise ValueError(f"{path}: empty translation or placeholder mismatch for {message_id}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lupdate", required=True, help="Path to the configured Qt lupdate")
    parser.add_argument("--catalog-dir", type=Path, default=CATALOG_DIRECTORY,
                        help="Alternate TS directory for isolated verification")
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix="seriousshot-lupdate-") as directory:
            output = Path(directory) / "active.ts"
            extract(args.lupdate, output)
            _, active = read_catalog(output)
            for locale in ("en", "zh_CN"):
                check_catalog(args.catalog_dir / f"seriousshot_{locale}.ts", locale, active)
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, f"{error}\n")
    print(f"SeriousShot catalogs: {len(active)} active IDs, both languages complete.")


if __name__ == "__main__":
    main()
