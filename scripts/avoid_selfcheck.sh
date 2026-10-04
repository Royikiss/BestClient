#!/usr/bin/env bash
# BestClient - Avoid (Gores bot) module self check.
#
# Run this after touching anything related to the Avoid module:
#     ./scripts/avoid_selfcheck.sh
#
# The module reproduces the Avoid subsystem of the reference client, see
# docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md. Every check below pins down one part of that
# contract: the parameter set, the wiring of every parameter, the localized menu and the
# decision engine itself.

set -e

cd "$(dirname "$0")/.."

if [ ! -f build/build.ninja ]; then
	echo "error: build/ is not configured; run cmake first" >&2
	exit 1
fi

echo "[1/7] building"
ninja -C build DDNet

echo "[2/7] bc_avoid_* parameter contract (name, default, min, max)"
python3 - <<'EOF'
import re

CONTRACT = [
    ("bc_avoid_enabled", 0, 0, 1),
    ("bc_avoid_agent", 0, 0, 4),
    ("bc_avoid_afk_protection", 0, 0, 1),
    ("bc_avoid_afk_time", 5, 5, 300),
    ("bc_avoid_player_prediction", 1, 0, 1),
    ("bc_avoid_draw_path", 1, 0, 1),
    ("bc_avoid_draw_track_point", 0, 0, 1),
    ("bc_avoid_draw_aimbot", 0, 0, 1),
    ("bc_avoid_legit_direction_weight", 170, 1, 1000),
    ("bc_avoid_legit_lifespan_weight", 160, 1, 1000),
    ("bc_avoid_legit_hook_weight", 260, 1, 1000),
    ("bc_avoid_legit_exploration", 4, 1, 1000),
    ("bc_avoid_legit_iterations", 100, 1, 1000),
    ("bc_avoid_legit_check_ticks", 6, 1, 50),
    ("bc_avoid_legit_direction", 1, 0, 1),
    ("bc_avoid_legit_hook", 1, 0, 1),
    ("bc_avoid_legit_teles", 0, 0, 1),
    ("bc_avoid_legit_death", 0, 0, 1),
    ("bc_avoid_legit_unfreeze", 0, 0, 1),
    ("bc_avoid_legit_unfreeze_ticks", 5, 1, 30),
    ("bc_avoid_blatant_check_ticks", 26, 1, 50),
    ("bc_avoid_kick_in_ticks", 26, 1, 50),
    ("bc_avoid_blatant_direction", 1, 0, 1),
    ("bc_avoid_blatant_hook", 1, 0, 1),
    ("bc_avoid_blatant_teles", 0, 0, 1),
    ("bc_avoid_blatant_death", 0, 0, 1),
    ("bc_avoid_blatant_unfreeze", 0, 0, 1),
    ("bc_avoid_blatant_unfreeze_ticks", 26, 0, 30),
    ("bc_avoid_nsif", 1, 0, 1),
    ("bc_avoid_track_point", 0, 0, 1),
    ("bc_avoid_safe_aim_tracking", 0, 0, 1),
    ("bc_avoid_auto_drag", 0, 0, 1),
    ("bc_avoid_aimbot", 0, 0, 1),
    ("bc_avoid_aimbot_fov", 90, 10, 360),
    ("bc_avoid_aimbot_segments", 5, 1, 64),
    ("bc_avoid_auto_aim", 0, 0, 1),
    ("bc_avoid_aim_assist", 1, 0, 1),
    ("bc_avoid_fent_quality", 0, 0, 2),
    ("bc_avoid_fent_advanced", 0, 0, 1),
    ("bc_avoid_fent_ticks", 1000, 1000, 10000),
    ("bc_avoid_fent_tweaker_actions", 50, 50, 5000),
    ("bc_avoid_fent_tweaker_ticks", 1, 1, 30),
    ("bc_avoid_fent_tweaker_dosage", 1, 1, 500),
    ("bc_avoid_fent_light_tile", 0, 0, 1),
    ("bc_avoid_fent_light_tile_radius", 1, 0, 20),
    ("bc_avoid_pilot_mode", 0, 0, 2),
    ("bc_avoid_pilot_population", 2048, 128, 8192),
    ("bc_avoid_pilot_depth", 17, 5, 50),
    ("bc_avoid_pilot_top_k", 10, 1, 100),
    ("bc_avoid_pilot_sequence", 5, 1, 20),
]

text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
found = {}
for match in re.finditer(r"MACRO_CONFIG_INT\((\w+), (bc_avoid\w*), (-?\d+), (-?\d+), (-?\d+),", text):
    found[match.group(2)] = (int(match.group(3)), int(match.group(4)), int(match.group(5)))

expected = {name: (default, minimum, maximum) for name, default, minimum, maximum in CONTRACT}
assert found == expected, (
    "parameter set drifted\n  only in header: %s\n  only in contract: %s\n  differing values: %s"
    % (sorted(set(found) - set(expected)), sorted(set(expected) - set(found)),
       {k: (found[k], expected[k]) for k in set(found) & set(expected) if found[k] != expected[k]})
)
print("  ok:", len(found), "parameters, every default/min/max matches the reference")
EOF

echo "[3/7] every parameter is actually wired"
python3 - <<'EOF'
import re, subprocess

header = "src/engine/shared/config_variables_bestclient.h"
declared = [match[1] for match in
            re.findall(r"MACRO_CONFIG_INT\((\w+), (bc_avoid\w*),", open(header, encoding="utf-8").read())]

# Scan everything except the declaration file itself.
files = subprocess.run(
    ["grep", "-rl", "--include=*.cpp", "--include=*.h", "bc_avoid", "src/"],
    capture_output=True, text=True).stdout.split()
files = [f for f in files if f != header]
text = "".join(open(f, encoding="utf-8").read() for f in files)

dead = []
for name in declared:
    member = "m_" + "".join(part.capitalize() for part in name.split("_"))
    if name not in text and member not in text:
        dead.append(name)
assert not dead, "these parameters are never read by the code: %s" % dead

# The menu has to expose every parameter that the engine reads; the reverse is what makes a
# parameter "unreachable". The master switch and the agent selection are owned by the component
# (`Avoid.SetEnabled` / `Avoid.SetAgent`), everything else needs a widget of its own.
menu = open("src/game/client/components/bestclient/menus_avoid.cpp", encoding="utf-8").read()
OWNED_BY_COMPONENT = {
    "bc_avoid_enabled": "Avoid.SetEnabled(",
    "bc_avoid_agent": "Avoid.SetAgent(",
}
missing_ui = []
for name in declared:
    member = "m_" + "".join(part.capitalize() for part in name.split("_"))
    if name in OWNED_BY_COMPONENT:
        assert OWNED_BY_COMPONENT[name] in menu, "%s is not reachable from the menu" % name
        continue
    if member not in menu:
        missing_ui.append(name)
assert not missing_ui, "these parameters have no widget in the Avoid menu: %s" % missing_ui

# The restore-default tables must only name real parameters and must cover every agent.
tables = re.findall(r"const char \*const (g_a\w+DefaultParams)\[\] = \{(.*?)\};", menu, re.S)
assert len(tables) == 6, "expected a defaults table per agent plus the shared one, found %d" % len(tables)
total = 0
for table_name, body in tables:
    names = re.findall(r'"(bc_avoid_[a-z_]+)"', body)
    assert names, table_name + " is empty"
    assert len(names) == len(set(names)), "duplicate entry inside " + table_name
    for name in names:
        assert name in declared, "unknown parameter in %s: %s" % (table_name, name)
    total += len(names)
print("  ok:", len(declared), "parameters wired,", len(tables), "defaults tables,", total, "entries")
EOF

echo "[4/7] localization follows the client language"
python3 - <<'EOF'
import re

SOURCES = [
    "src/game/client/components/bestclient/avoid.cpp",
    "src/game/client/components/bestclient/avoid_engine.cpp",
    "src/game/client/components/bestclient/menus_avoid.cpp",
    "src/game/client/components/bestclient/menus_tas.cpp",
    "src/game/client/components/hud_layout.cpp",
]
keys = []
for path in SOURCES:
    text = open(path, encoding="utf-8").read()
    for match in re.finditer(r'BcLocalize\(("(?:[^"\\]|\\.)*")', text):
        key = match.group(1)[1:-1]
        if key not in keys:
            keys.append(key)
assert len(keys) > 120, "the Avoid UI should have a substantial number of localized labels"

def load(path):
    """Mirror of CLocalizationDatabase::Load, so a malformed file is caught here."""
    lines = open(path, encoding="utf-8").read().split("\n")
    strings = {}
    errors = []
    index = 0
    number = 0
    while index < len(lines):
        line = lines[index]; index += 1; number += 1
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            assert line.endswith("]"), "%s:%d malformed context %r" % (path, number, line)
            context = line[1:-1]
            assert index < len(lines), "%s:%d unexpected end of file" % (path, number)
            line = lines[index]; index += 1; number += 1
        else:
            context = ""
        origin = line
        assert index < len(lines), "%s:%d unexpected end of file after %r" % (path, number, origin)
        replacement = lines[index]; index += 1; number += 1
        if not replacement.startswith("== "):
            errors.append("%s:%d malformed replacement %r for %r" % (path, number, replacement, origin))
            continue
        strings[(origin, context)] = replacement[3:]
    return strings, errors

for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    strings, errors = load(path)
    assert not errors, "\n".join(errors)
    # BcLocalize() looks the key up in the BestClient context and only then in the default one,
    # so every label of this module must exist under [BestClient].
    missing = [key for key in keys if (key, "BestClient") not in strings]
    assert not missing, "%s is missing %d Avoid labels, e.g. %s" % (path, len(missing), missing[:5])
print("  ok:", len(keys), "labels in 2 languages")
EOF

echo "[5/7] no placeholder copy left in the module"
python3 - <<'EOF'
import re, subprocess

FORBIDDEN = [
    "not implemented", "Not implemented", "尚未实现", "Planned", "planned",
    "is missing", "arrives with the agent", "reserved for upcoming",
]
# Only user visible text counts: every BcLocalize() literal of the module, plus the language
# entries that carry an Avoid label. Comments are allowed to use these words.
def localized_literals(path):
    text = open(path, encoding="utf-8").read()
    return [match.group(1)[1:-1] for match in re.finditer(r'BcLocalize\(("(?:[^"\\]|\\.)*")', text)]

files = subprocess.run(
    ["grep", "-rl", "--include=*.cpp", "--include=*.h", "-e", "Avoid", "src/game/client/components/bestclient/"],
    capture_output=True, text=True).stdout.split()
files = [f for f in files if "avoid" in f or "menus_tas" in f]
for path in files:
    for label in localized_literals(path):
        for word in FORBIDDEN:
            assert word not in label, "%s still shows the placeholder %r" % (path, label)

# The localization files must not ship labels for features that no longer exist either.
menu = open("src/game/client/components/bestclient/menus_avoid.cpp", encoding="utf-8").read()
for word in FORBIDDEN:
    assert word not in menu, "menus_avoid.cpp still contains %r" % word

# UI contract. `CUi::DoLabel` does NOT wrap on its own: without a maximum width it shrinks the
# string down to the 5 px floor, which makes every paragraph in this page unreadable. The hint
# helpers have to pass it, and the panel hints have to reserve the measured height.
assert "SLabelProperties Props;" in menu and "Props.m_MaxWidth = Rect.w;" in menu, \
    "AvoidHint stopped wrapping, long copy will be shrunk to the font size floor"
assert "AvoidHintBottom(" in menu, "the panels no longer reserve the height their hints need"
assert "TextBoundingBox(" in menu, "the wrapped hint height is no longer measured"
# Dead helpers or branches that would silently reappear.
assert "AVOID_TEXT_VALUE" not in menu
assert "SIMULATION_SAFE_CONSTANT" not in menu, \
    "no agent reports the 9999 sentinel as its survival ticks, so that branch cannot run"

for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for key in localized_literals("src/game/client/components/bestclient/menus_avoid.cpp") + \
               localized_literals("src/game/client/components/bestclient/avoid.cpp"):
        for word in FORBIDDEN:
            assert ("%s\n== " % key) not in text or word not in key, "unreachable: %r" % key
print("  ok")
EOF

echo "[6/7] engine contract"
python3 - <<'EOF'
engine = open("src/game/client/components/bestclient/avoid_engine.cpp", encoding="utf-8").read()
header = open("src/game/client/components/bestclient/avoid_engine.h", encoding="utf-8").read()
component = open("src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read()

# Simulator (reference spec 4.1)
assert "int SimulateCandidate(" in engine and "CopyWorldClean(" in engine
assert "SIMULATION_SAFE_CONSTANT = 9999" in header
# Basic (reference spec 5): fixed 6 ticks, {0, -1, 1}, first safe candidate wins.
assert "BASIC_CHECK_TICKS = 6" in header
assert "s_aCandidateDirs[3] = {0, -1, 1}" in engine
# Legit (reference spec 7): the asymmetric heuristic and the UCT term.
assert "constexpr float WEIGHT_SCALE = 0.01f;" in engine
assert "std::abs(DirDiff - 2.0f)" in engine and "std::abs(HookDiff - 1.0f)" in engine
assert "3.402823466e+38" in engine
assert "s_aDirs[3] = {-1, 0, 1}" in engine
# Blatant (reference spec 6): kick-in hysteresis, NSIF, direction x hook candidates.
assert "Set.m_KickInTicks" in engine and "m_SavedSafeSequence" in engine
assert "s_aHooks[2] = {0, 1}" in engine
# Fentbot (reference spec 8): the velocity/flow dot product weight and the preset table.
assert "FENT_FLOW_WEIGHT = 1750.0f" in engine
assert "Set.m_FentActions = 88;" in engine
assert "Set.m_FentActions = 160;" in engine
assert "Set.m_FentActions = 1000;" in engine
assert "Set.m_FentDosage = 300;" in engine
# Pilot (reference spec 9)
assert "m_PilotPopulation" in engine and "m_PilotTopK" in engine and "m_PilotSequence" in engine

# All five agents exist and are reachable through the dispatcher.
for agent in ("CBasicAgent", "CLegitAgent", "CBlatantAgent", "CFentbotAgent", "CPilotAgent"):
    assert "AvoidInput %s::GetAction(" % agent in engine, agent + " is missing"
    assert "new Avoid::%s(" % agent in component, agent + " is not instantiated"

# The input hook must rewrite the outgoing input only when an agent intervened, hand the input
# back explicitly when it stops driving, and ask for the packet itself instead of forcing the
# sampler to send on every frame.
assert "EInputResult ApplyInput(" in open("src/game/client/components/bestclient/avoid.h", encoding="utf-8").read()
assert "*pInput = m_LastOverride;" in component, "the override is no longer applied"
assert "FinishInput(" in component, "the driving -> yielded handover is missing"
assert "m_Controls.m_aInputData" not in component, "avoid must not write the raw key state buffer"
assert "WantsEveryTickInput" not in component and "WantsEveryTickInput" not in \
    open("src/game/client/components/controls.cpp", encoding="utf-8").read()
client = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
assert "m_Avoid.ApplyInput(&Input) != CAvoid::INPUT_IDLE" in client, "the avoid hook moved or disappeared"
assert "if(m_Tas.IsRecordingActive() && !m_FastPractice.Enabled())" in client
print("  ok")
EOF

echo "[6b/7] HUD module wiring and render order"
python3 - <<'EOF'
assert "MODULE_AVOID," in open("src/game/client/components/hud_layout.h", encoding="utf-8").read()
layout = open("src/game/client/components/hud_layout.cpp", encoding="utf-8").read()
assert '"avoid",' in layout and '"Avoid",' in layout and "case MODULE_AVOID:" in layout
editor = open("src/game/client/components/bestclient/hud_editor.cpp", encoding="utf-8").read()
assert "MODULE_AVOID" in editor and "m_Avoid.RenderPreview()" in editor

client = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
foreground = client.index("\t\t\t\t\t      &m_MapLayersForeground,")
hud = client.index("\t\t\t\t\t      &m_Hud,")
tas = client.index("\t\t\t\t\t      &m_Tas, // bestclient")
avoid = client.index("\t\t\t\t\t      &m_Avoid, // bestclient")
assert foreground < hud < tas < avoid, "HUD components must render after the map layers"
print("  ok: foreground < Hud < Tas < Avoid")
EOF

echo "[7/7] testrunner test suite"
ninja -C build testrunner
./build/testrunner

echo "all checks passed"
