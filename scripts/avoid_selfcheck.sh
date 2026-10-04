#!/usr/bin/env bash
# BestClient - Avoid (Gores bot) module self check.
#
# Run this after touching anything related to the Avoid module:
#     ./scripts/avoid_selfcheck.sh
#
# See docs/AVOID_TECHNICAL_DOCUMENTATION.md (appendix D) for the rationale of every step.

set -e

cd "$(dirname "$0")/.."

if [ ! -f build/build.ninja ]; then
	echo "error: build/ is not configured; run cmake first" >&2
	exit 1
fi

echo "[1/6] building"
ninja -C build DDNet

echo "[2/6] bc_avoid_* config variables"
python3 - <<'EOF'
text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
n = text.count("MACRO_CONFIG_INT(BcAvoid") + text.count("MACRO_CONFIG_STR(BcAvoid")
assert n == 33, f"expected 33 bc_avoid_* cvars, found {n}"
print("  ok:", n)
EOF

echo "[3/6] localization entries"
python3 - <<'EOF'
keys = ["Avoid", "Assist mode", "Arm Avoid agent", "Check ticks", "NSIF on no safe input",
        "Basic", "Legit", "Blatant", "Input pipeline self-test",
        "Tee", "Nearest", "Hazard / sensed", "Safe ahead", "Overrides", "Status HUD",
        # stage 2 (Basic decision engine) reasons and log strings
        "brake before hazard", "steer left before hazard", "steer right before hazard",
        "player input safe", "still time before the hazard", "frozen, agent idle",
        # stage 3 (Legit decision engine) reasons, state gate and config hint
        "release hook before hazard", "release hook, brake before hazard",
        "release hook, steer left before hazard", "release hook, steer right before hazard",
        "release hook and jump before hazard",
        "press hook before hazard", "press hook, brake before hazard",
        "press hook, steer left before hazard", "press hook, steer right before hazard",
        "jump before hazard", "jump, brake before hazard",
        "jump, steer left before hazard", "jump, steer right before hazard",
        "no safer plan", "jetpack, hands off", "hooked to a player, hands off",
        "fly hammer, hands off", "agent not implemented yet",
        "warning: kick in ticks is not below check ticks, the agent will react late",
        # v1.2.1: sensing radius in half tiles
        "Radius (half tiles)",
        "no safer plan (jump is a last resort)",
        # v1.2.4: restore-defaults button
        "Defaults", "Restored", "Avoid: parameters restored to their defaults",
        "The sensing radius is how far ahead the agent may notice a hazard, in half tiles (12 = 6 tiles). Lower it to react later, raise it to react earlier; the scan itself costs almost nothing.",
        # stage 4 (Blatant decision engine): the aim layer and its reasons
        "auto drag: hook the closest tee", "auto drag: hook the closest tee, brake",
        "auto drag: hook the closest tee, steer left", "auto drag: hook the closest tee, steer right",
        "auto drag: aim at the closest tee", "auto drag: aim at the closest tee, brake",
        "auto drag: aim at the closest tee, steer left", "auto drag: aim at the closest tee, steer right",
        "track point: keep aim and press hook", "track point: keep aim and press hook, brake",
        "track point: keep aim and press hook, steer left", "track point: keep aim and press hook, steer right",
        "track point: hold the hookable aim", "track point: hold the hookable aim, brake",
        "track point: hold the hookable aim, steer left", "track point: hold the hookable aim, steer right",
        "aim at the safest hookable spot and press hook", "aim at the safest hookable spot and press hook, brake",
        "aim at the safest hookable spot and press hook, steer left", "aim at the safest hookable spot and press hook, steer right",
        "aim at the safest hookable spot", "aim at the safest hookable spot, brake",
        "aim at the safest hookable spot, steer left", "aim at the safest hookable spot, steer right",
        "release hook and aim clear of the hazard"]
for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for k in keys:
        assert "\n%s\n== " % k in text, (path, k)
print("  ok")
EOF

echo "[4/6] HUD module wiring"
python3 - <<'EOF'
assert "MODULE_AVOID," in open("src/game/client/components/hud_layout.h", encoding="utf-8").read()
t = open("src/game/client/components/hud_layout.cpp", encoding="utf-8").read()
assert '"avoid",' in t and '"Avoid",' in t and "case MODULE_AVOID:" in t
t = open("src/game/client/components/bestclient/hud_editor.cpp", encoding="utf-8").read()
assert "MODULE_AVOID" in t and "m_Avoid.RenderPreview()" in t
print("  ok")
EOF

echo "[5/6] render order (map foreground layer must come before the HUD pass)"
python3 - <<'EOF'
t = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
fg = t.index("\t\t\t\t\t      &m_MapLayersForeground,")
hud = t.index("\t\t\t\t\t      &m_Hud,")
tas = t.index("\t\t\t\t\t      &m_Tas, // bestclient")
avo = t.index("\t\t\t\t\t      &m_Avoid, // bestclient")
assert fg < hud < tas < avo, "HUD components must render after the map layers"
print("  ok: foreground < Hud < Tas < Avoid")
EOF

echo "[5b/6] menu: restore-defaults button"
python3 - <<'EOF'
import re
t = open("src/game/client/components/bestclient/menus_avoid.cpp", encoding="utf-8").read()
assert "AvoidDefaultParams(" in t and "pConfigManager->Reset(" in t, "the restore-defaults button is missing"
cvars = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
tables = re.findall(r'const char \*const (g_a\w+DefaultParams)\[\] = \{(.*?)\};', t, re.S)
assert tables, "the defaults tables are missing"
total = 0
for table_name, body in tables:
    names = re.findall(r'"(bc_avoid_[a-z_]+)"', body)
    assert names, table_name + " is empty"
    assert len(names) == len(set(names)), "duplicate entry inside " + table_name
    for name in names:
        assert (", %s," % name) in cvars, "unknown cvar in %s: %s" % (table_name, name)
    total += len(names)
print("  ok:", len(tables), "tables,", total, "entries")

# Every agent page that exposes parameters needs a way to reach the tile switches and the sensing
# radius. Blatant was missing its `Tiles` panel from stage 1 until v1.4.1, which made those settings
# unreachable on that page - and the "restore defaults" table kept resetting them anyway.
def panels(name):
    body = re.search(r'const SAvoidPanelDef %s\[\] = \{(.*?)\};' % name, t, re.S)
    assert body, name + " is missing"
    return re.findall(r'\{PANEL_(\w+), "([^"]+)"\}', body.group(1))
for table, expected in (("g_aBasicPanels", 2), ("g_aLegitPanels", 4), ("g_aBlatantPanels", 6),
                        ("g_aFentPanels", 4), ("g_aPilotPanels", 4)):
    got = panels(table)
    assert len(got) == expected, "%s has %d panels, expected %d" % (table, len(got), expected)
    assert "TILES" in [k for k, _ in got], table + " cannot reach the tile switches"
print("  ok: every agent page reaches its Tiles panel")
EOF

echo "[6/6] decision engine wiring"
python3 - <<'EOF'
# --- KRX reproduction architecture ------------------------------------------------
t = open("src/game/client/components/bestclient/avoid_engine.cpp", encoding="utf-8").read()
assert "int SimulateCandidate(" in t, "SimulateCandidate is missing"
assert "CopyWorldClean(" in t, "CopyWorldClean is missing"
assert "AvoidInput CBasicAgent::GetAction(" in t, "Basic agent is missing"
assert "AvoidInput CBlatantAgent::GetAction(" in t, "Blatant agent is missing"
assert "AvoidInput CLegitAgent::GetAction(" in t, "Legit agent is missing"
assert "AvoidInput CFentbotAgent::GetAction(" in t, "Fentbot agent is missing"
assert "AvoidInput CPilotAgent::GetAction(" in t, "Pilot agent is missing"
assert "bool IsHookable(" in t and "IntersectLineTeleHook(" in t, \
    "the hookability probe has to use the engine's own hook ray"

# --- config compatibility ----------------------------------------------------------------
t = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
assert "bc_avoid_sensing_radius, 2, 1, 32" in t, "bc_avoid_sensing_radius is not in half tiles any more"
t = open("src/engine/client/client.cpp", encoding="utf-8").read()
assert "g_Config.m_BcAvoidSensingRadius = std::clamp(g_Config.m_BcAvoidSensingRadius * 2, 1, 32);" in t, \
    "the half tile migration of bc_avoid_sensing_radius is missing"
assert "g_Config.m_ClConfigVersion = 2;" in t, "the config version was not bumped for the migration"
t = open("src/engine/shared/config_variables.h", encoding="utf-8").read()
assert "ClConfigVersion, cl_config_version, 2," in t, \
    "a fresh config must already be at version 2, otherwise it would migrate its own default"

# --- the client component ---------------------------------------------------------------
act = open("src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read()
assert "pAgent->GetAction(" in act, "agent action dispatch is missing"
assert "m_Controls.m_aInputData" not in act.replace("`m_Controls.m_aInputData`", ""), "avoid must never write the raw key state buffer"
print("  ok")
EOF

echo "[6b/6] testrunner test suite"
ninja -C build testrunner
./build/testrunner

echo "all checks passed"

