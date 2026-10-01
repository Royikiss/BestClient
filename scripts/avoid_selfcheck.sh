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

echo "[1/5] building"
ninja -C build DDNet

echo "[2/5] bc_avoid_* config variables"
python3 - <<'EOF'
text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
n = text.count("MACRO_CONFIG_INT(BcAvoid") + text.count("MACRO_CONFIG_STR(BcAvoid")
assert n == 33, f"expected 33 bc_avoid_* cvars, found {n}"
print("  ok:", n)
EOF

echo "[3/5] localization entries"
python3 - <<'EOF'
keys = ["Avoid", "Assist mode", "Arm Avoid agent", "Check ticks", "NSIF on no safe input",
        "Basic", "Legit", "Blatant", "Input pipeline self-test",
        "Tee", "Nearest", "Hazard / sensed", "Safe ahead", "Overrides", "Status HUD"]
for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for k in keys:
        assert "\n%s\n== " % k in text, (path, k)
print("  ok")
EOF

echo "[4/5] HUD module wiring"
python3 - <<'EOF'
assert "MODULE_AVOID," in open("src/game/client/components/hud_layout.h", encoding="utf-8").read()
t = open("src/game/client/components/hud_layout.cpp", encoding="utf-8").read()
assert '"avoid",' in t and '"Avoid",' in t and "case MODULE_AVOID:" in t
t = open("src/game/client/components/bestclient/hud_editor.cpp", encoding="utf-8").read()
assert "MODULE_AVOID" in t and "m_Avoid.RenderPreview()" in t
print("  ok")
EOF

echo "[5/5] render order (map foreground layer must come before the HUD pass)"
python3 - <<'EOF'
t = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
fg = t.index("\t\t\t\t\t      &m_MapLayersForeground,")
hud = t.index("\t\t\t\t\t      &m_Hud,")
tas = t.index("\t\t\t\t\t      &m_Tas, // bestclient")
avo = t.index("\t\t\t\t\t      &m_Avoid, // bestclient")
assert fg < hud < tas < avo, "HUD components must render after the map layers"
print("  ok: foreground < Hud < Tas < Avoid")
EOF

echo "all checks passed"
