#!/usr/bin/env python3
"""Compile the real portable parser and repository ImGui hash with ASan/UBSan."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
pack_text = (ROOT / "OptiScaler/translations/optional/pt-BR.lang").read_text(encoding="utf-8")
for line in pack_text.splitlines():
    if line and not line.startswith("#"):
        assert all(ord(char) <= 255 for char in line.split("\t")[1]), "pack needs glyphs outside default Latin range"
with tempfile.TemporaryDirectory(prefix="nr-localization-") as temp:
    binary = Path(temp) / "cases"
    label_pattern = re.compile(r'Localization::Label\(((?:"(?:\\.|[^"\\])*"\s*)+)\)')
    labels = []
    for relative in ("OptiScaler/dlssnr/DlssNr_Menu.cpp", "OptiScaler/menu/menu_common.cpp"):
        labels.extend(label_pattern.findall((ROOT / relative).read_text()))
    assert len(labels) >= 60, "ZERO/LOW COVERAGE: expected actual production menu label call sites"

    # The redesigned menu translates in a few central places instead of at each call site: every
    # section and card title (SectionTitle, ScopedCard, SeparatorWithHelpMarker) and every card name in
    # the Custom tab picker go through Label; the sidebar's tab and group names, and the picker's tab
    # headings, through Tr. Their literals are collected here so a title added upstream cannot stay in
    # English unnoticed.
    menu_source = (ROOT / "OptiScaler/menu/menu_common.cpp").read_text()
    literal = r'("(?:\\.|[^"\\])*")'
    central_labels = []
    for pattern in (r'\bSectionTitle\(' + literal, r'\bScopedCard card \{ ' + literal, r'\bScopedCard card\(' + literal,
                    r'\bSeparatorWithHelpMarker\(\s*' + literal):
        central_labels.extend(re.findall(pattern, menu_source))
    menu_boxes = re.findall(r'\{ "[a-z_0-9]+", ' + literal + r', ' + literal, menu_source)
    menu_tabs = re.findall(r'\{ ("(?:MAIN|SYSTEM|ADVANCED)"), ' + literal, menu_source)
    assert len(central_labels) >= 30 and len(menu_boxes) >= 30 and len(menu_tabs) >= 8, \
        "ZERO/LOW COVERAGE: the menu's central title, card and tab tables were not found"
    labels.extend(central_labels + [name for _, name in menu_boxes])
    central_text = [tab for tab, _ in menu_boxes] + [part for pair in menu_tabs for part in pair]

    # Drift guard for display text: every literal the two menus send through Tr, or through the NR
    # menu's HelpMarker (which calls Tr), has an entry. Labels are checked in cases.cpp.
    def c_text(literals):
        escapes = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}
        return re.sub(r"\\(.)", lambda m: escapes[m.group(1)], "".join(re.findall(r'"((?:\\.|[^"\\])*)"', literals)))
    pack_keys = set()
    for line in pack_text.splitlines():
        if line and not line.startswith("#"):
            pack_keys.add(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r", "\\": "\\"}[m.group(1)],
                                 line.split("\t")[0]))
    text_pattern = re.compile(r'(?:Localization::Tr|\bHelpMarker)\(((?:"(?:\\.|[^"\\])*"\s*)+)\)')
    missing = []
    for relative in ("OptiScaler/dlssnr/DlssNr_Menu.cpp", "OptiScaler/menu/menu_common.cpp"):
        for literals in text_pattern.findall((ROOT / relative).read_text()):
            text = c_text(literals)
            if re.search(r"[A-Za-z]{2}", text) and text not in pack_keys:
                missing.append(f"{relative}: {text[:80]!r}")
    for literals in central_text:
        if c_text(literals) not in pack_keys:
            missing.append(f"OptiScaler/menu/menu_common.cpp (tab or group name): {c_text(literals)[:80]!r}")
    assert not missing, "pt-BR pack lacks display text:\n" + "\n".join(missing)
    (Path(temp) / "menu-labels.h").write_text(
        "static const char* menuLabels[] = {\n" + ",\n".join(labels) + "\n};\n")
    subprocess.run([
        "g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I", temp, "-I", str(ROOT / "OptiScaler"), "-I", str(ROOT / "OptiScaler/include/imgui"),
        str(HERE / "cases.cpp"), str(ROOT / "OptiScaler/include/imgui/imgui.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(ROOT / "OptiScaler/translations/optional/pt-BR.lang")], check=True)

with tempfile.TemporaryDirectory(prefix="nr-localization-runtime-") as temp:
    directory = Path(temp)
    binary = directory / "runtime"
    subprocess.run([
        "g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(HERE / "stubs"), "-I", str(ROOT / "OptiScaler"),
        str(HERE / "runtime.cpp"), str(ROOT / "OptiScaler/misc/Localization.cpp"), "-o", str(binary),
    ], check=True)
    for scenario in ("absent", "inactive", "invalid", "active"):
        case = directory / scenario
        case.mkdir()
        if scenario == "inactive":
            optional = case / "translations/optional"
            optional.mkdir(parents=True)
            (optional / "pt-BR.lang").write_bytes((ROOT / "OptiScaler/translations/optional/pt-BR.lang").read_bytes())
        if scenario == "invalid":
            (case / "OptiScaler.lang").write_text("Save Settings\tSalvar configurações\nunsafe\t%n\n")
        if scenario == "active":
            (case / "OptiScaler.lang").write_bytes((ROOT / "OptiScaler/translations/optional/pt-BR.lang").read_bytes())
        subprocess.run([str(binary), scenario], cwd=case, check=True)
