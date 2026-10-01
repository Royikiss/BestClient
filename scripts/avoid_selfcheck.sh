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
        "The sensing radius is how far ahead the agent may notice a hazard, in half tiles (12 = 6 tiles). Lower it to react later, raise it to react earlier; the scan itself costs almost nothing."]
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
EOF

echo "[6/6] decision engine wiring"
python3 - <<'EOF'
# --- the shared simulator and the agents ------------------------------------------------
t = open("src/game/client/components/bestclient/avoid_engine.cpp", encoding="utf-8").read()
assert "int SimulateFixed(" in t, "forward simulator is missing"
assert "SInputPlan CPlanner::Plan(" in t, "the Legit planner is missing"
# Sensor reuse: the engine has to ask the stage 1 probe rules, never a private copy.
assert "IsRelevantHazard(Set, ClassifyPoint(pCollision, m_Sim.m_Core.m_Pos))" in t, \
    "the simulator must reuse ClassifyPoint()/IsRelevantHazard()"
assert "m_Core.m_Tuning = Src.m_Tuning" in t, "the simulator must use the map tuning, not constants"
# Candidate space: direction x jump x hook, with the player's own values first.
assert "MAX_ROOT_ACTIONS" in t and "const int aJumps[2]" in t and "int aHooks[3]" in t, \
    "the Legit candidate space is not direction x jump x hook"
# Search: UCT with a deterministic local PRNG, a quality-driven iteration count and a budget guard.
assert "m_Prng.Seed(" in t, "the search must use a local, seeded PRNG"
assert "rand()" not in t and "srand" not in t, "no global RNG in the decision path"
assert "MeanScore(" in t and "RankingScore(" in t, "the UCT value functions are missing"
assert "SEARCH_BUDGET_MS" in t, "the search has no wall clock guard"
# Player prediction: a private world carrying the snapshots.
assert "m_aShadows[NumShadows] = Snapshot.m_Core" in t and "m_World.m_apCharacters[m_aShadows[" in t, \
    "the predicted players are not injected into the clone world"
assert "Env.m_PredictPlayers" in t, "bc_avoid_player_prediction is not wired into the simulator"
# Unfreeze lookahead and the movement state gate.
assert "m_UnfreezeTicks" in t, "bc_avoid_unfreeze_ticks is not wired into the engine"
# Jump policy: a rope problem is never solved by hopping, and the jump waits for the critical
# moment. Both live in UpdateAvailability()/JumpEngaged().
assert "JUMP_URGENCY_TICKS" in t and "void CPlanner::UpdateAvailability(" in t, \
    "the jump is not held back until the critical moment any more"
assert "bool CPlanner::JumpEngaged(" in t and "HOOK_GRABBED" in t, \
    "jumps are no longer suppressed while the hook is engaged"
# The sensing reach is an ellipse derived from the map tuning, not a circle.
assert "float SensingVerticalFactor(" in t and "SENSING_VERTICAL_FALLBACK" in t, \
    "the sensing reach is not the tuning derived ellipse any more"
assert "TileBoxDelta(Pos, Tx, Ty)" in t, "the sensor does not use the per axis tile box distance"
# The sensing radius is a radius in tiles with half tile steps, measured to the hazard box.
assert "const float RadiusX = std::clamp(Set.m_SensingRadius, 0.5f, 16.0f);" in t, \
    "the sensing radius is not applied as a fractional distance to the hazard box"
assert "int ClassifyMovement(" in t, "the movement state gate is missing"
assert "ClassifyMovement(Ctx.m_Core, FlyHammerState(Ctx)" in open(
    "src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read(), \
    "the client component does not apply the movement state gate"

# --- config compatibility ----------------------------------------------------------------
# `bc_avoid_sensing_radius` changed from tiles to half tiles; the migration has to stay, together
# with the version bump that makes it run exactly once.
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
t = open("src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read()
assert "int CAvoid::SimulateInput(" in t, "forward simulator wrapper is missing"
assert "CAvoid::SInputPlan CAvoid::EvaluateBestPlan(" in t, "decision engine is missing"
# The sensing radius has to gate the engine, otherwise the setting only moves the HUD readout.
assert "!Ctx.m_Threat.m_HasNearest" in t, "the sensing radius gate is missing from the decision engine"
assert "m_Controls.m_aInputData" not in t.replace("`m_Controls.m_aInputData`", ""), "avoid must never write the raw key state buffer"
assert "STAGE 2 IMPLEMENTATION SLOT" not in t, "the stage 2 slot should be filled in now"
# Basic must still only rewrite the direction: the Legit agent is the one that touches jump/hook,
# and it lives in avoid_engine.cpp. Reads of the fields (the log line) are fine, writes are not.
for forbidden in (".m_Hook = ", ".m_Jump = ", ".m_Fire = "):
    assert forbidden not in t, f"avoid.cpp must not write {forbidden.strip()}"
assert "Plan.m_Input.m_Direction = BestDir" in t, "Basic must only change m_Direction"
print("  ok")
EOF

echo "[6b/6] simulator fidelity + sensing radius + Legit agent tests"
ninja -C build testrunner
./build/testrunner --gtest_filter='CAvoidSimulatorTest.*:CAvoidSensingRadiusTest.*:CAvoidLegitTest.*'

echo "all checks passed"
