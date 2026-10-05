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
    ("bc_avoid_tile_editor_enable", 0, 0, 1),
    ("bc_avoid_tile_editor_type", 0, 0, 1),
    ("bc_avoid_tile_editor_clear", 0, 0, 1),
    ("bc_avoid_tile_editor_auto_tunnel", 0, 0, 1),
    ("bc_avoid_tile_editor_auto_tunnel_width", 2, 0, 10),
    ("bc_avoid_tile_editor_auto_finish", 0, 0, 1),
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
component_header = open("src/game/client/components/bestclient/avoid.h", encoding="utf-8").read()
decision = open("src/game/client/components/bestclient/avoid_decision.h", encoding="utf-8").read()
editor = open("src/game/client/components/bestclient/avoid_tile_editor.cpp", encoding="utf-8").read()
editor_header = open("src/game/client/components/bestclient/avoid_tile_editor.h", encoding="utf-8").read()

# Simulator (reference spec 4.1)
assert "int SimulateCandidate(" in engine and "CopyWorldClean(" in engine
assert "SIMULATION_SAFE_CONSTANT = 9999" in header
# The death layer is a tile index, not a bit mask: `Tile & TILE_DEATH` also matches tile 3
# (nohook), 11 (unfreeze), 34 (finish) and would report death on a finish tile.
assert "== TILE_DEATH" in engine, "the death tile is no longer compared, it is masked"
assert "& TILE_DEATH" not in engine, "TILE_DEATH is a tile index, masking it invents hazards"
# The freeze hazard is the three flags of the tee, like the reference; the engine's own
# m_IsInFreeze bookkeeping is broader (it is also set for death tiles and deep frozen tees).
assert "pChar->m_FreezeTime > 0 || pChar->m_FrozenLastTick || pCore->m_DeepFrozen" in engine
assert "m_Core.m_IsInFreeze || pCore->m_DeepFrozen" not in engine
# Basic (reference spec 5): fixed 6 ticks, {0, -1, 1}, first safe candidate wins.
assert "BASIC_CHECK_TICKS = 6" in header
assert "s_aCandidateDirs[3] = {0, -1, 1}" in engine
# Legit (reference spec 7): the asymmetric heuristic and the UCT term.
assert "constexpr float WEIGHT_SCALE = 0.01f;" in decision
assert "std::abs(DirDiff - 2.0f)" in decision and "std::abs(HookDiff - 1.0f)" in decision
assert "3.402823466e+38" in engine
assert "s_aDirs[3] = {-1, 0, 1}" in decision
# Legit final decision (reference spec 7.3, 0x1403392cb): exploitation + heuristic with a zero
# exploration term, and never the most visited child. That rule is what stops the agent from
# walking off on its own while the player's input is safe.
assert "SelectLegitRootChild(" in engine, "the Legit root selection left the reference rule"
assert "MostVisits" not in engine and "m_Visits > MostVisits" not in engine, \
    "the Legit agent is back to picking the most visited child, which walks left when visits tie"
assert "Exploitation + Heuristic" in decision
assert "if(Child.m_Visits == 0)" in decision, "unvisited root children must be skipped"

# ---------------------------------------------------------------------------------------------
# v5.0 pre-activation pipeline (reference spec 5) and the two arbitration layers (spec 7.3, 8.3).
# These are the mechanisms the 70 points of "human feel at full frame rate" hang on: without them
# the client wakes a 100 round MCTS on every safe frame and the Legit agent overrides the player
# for nothing.
# ---------------------------------------------------------------------------------------------
# 10 tick probe: fixed window, `cmp eax, 0x7` threshold, and the sentinel that skips the agents.
assert "PROBE_CHECK_TICKS = 10" in decision, "the probe window left the reference constant"
assert "PROBE_SAFE_TICKS = 7" in decision, "the probe threshold left `cmp eax, 0x7`"
assert "ProbeIsSafe(" in decision and "SECTOR_SCAN_TICKS = 21" in decision
assert "LEGIT_ARBITRATION_TICKS = 26" in decision, "the arbitration horizon left `mov edi, 0x1a`"
assert "AUTO_DRAG_MAX_DIST = 380.0f" in decision and "AUTO_DRAG_MIN_DIST = 16.0f" in decision
assert "IsBlacklistedGametype(" in decision, "the gamemode blacklist logic is gone"

# The engine owns both pipeline entry points and the dispatcher has to call them in reference order.
assert "int RunLightweightProbe(" in header and "int RunLightweightProbe(" in engine
assert "bool RunSectorScan(" in header and "bool RunSectorScan(" in engine
assert "ProbeIsSafe(Survival) ? SIMULATION_SAFE_CONSTANT : Survival" in engine, \
    "the probe no longer turns >= 7 survived ticks into the fully safe sentinel"
assert "RunLightweightProbe(pWorld, pInput, Ctx.m_Settings)" in component, "the dispatcher does not probe"
assert "Avoid::RunSectorScan(GameClient(), pWorld" in component, "stage 2 is not wired"
# The probe deliberately gates only the three avoid agents: Fentbot and Pilot are sliced planners
# whose search only advances while they are called, so probing them would leave them without a plan.
assert "AgentId != AGENT_FENTBOT && AgentId != AGENT_PILOT" in component, \
    "the probe gate no longer excludes the two planners"
# Order: gates -> probe -> sector scan -> agent.
probe_at = component.index("RunLightweightProbe(pWorld, pInput, Ctx.m_Settings)")
scan_at = component.index("Avoid::RunSectorScan(GameClient(), pWorld")
agent_at = component.index("pAgent->GetAction(AgentCtx, pWorld)")
assert probe_at < scan_at < agent_at, "the pipeline stages are out of reference order"

# The five gates, in reference order, and the dispatcher asking for them before anything else.
for gate in ("PRE_GAMEMODE", "PRE_INACTIVE", "PRE_FROZEN", "PRE_AFK"):
    assert gate in component_header, gate + " left the pre-activation enum"
assert "EPreActivation PreActivation() const;" in component_header
assert "IsGamemodeBlacklisted()" in component and "IsPlayerInactive()" in component and \
       "IsCharacterFrozen()" in component and "IsAfk()" in component and "UpdateAfkTimer(" in component
assert component.index("if(IsGamemodeBlacklisted())") < component.index("if(IsPlayerInactive())") < \
       component.index("if(IsCharacterFrozen())") < component.index("if(IsAfk())"), \
    "the environment gates are no longer in reference order"
assert "m_FreezeTime > 0 || pChar->m_FrozenLastTick" in component, "gate 3 lost the freeze flags"
assert "GAMESTATEFLAG_PAUSED" in component and "TEAM_SPECTATORS" in component, "gate 2 lost a case"
# AFK is a per-tick gate now, not a persisted switch-off: the reference blocks the tick and the bot
# resumes by itself as soon as the player touches the controls again.
assert "SetEnabled(false)" not in component, "AFK protection switches the bot off again"
assert "m_LastActiveTime" in component and "m_LastPlayerInput" in component

# Legit post hoc arbitration (spec 8.3, 0x140338a0c - 0x140338a68): both paths over a fixed 26
# ticks, and the override only when the candidate survives at least one tick longer.
assert "ArbitrationGain(CandidateSurvival, HumanSurvival, LEGIT_ARBITRATION_TICKS)" in engine, \
    "the 26 tick gain arbitration is gone"
assert "ArbitrationAllowsOverride(CandidateSurvival, HumanSurvival, LEGIT_ARBITRATION_TICKS)" in engine
assert "post hoc gain below 1 tick" in engine, "the rejection path of the arbitration is gone"
assert "CandidateSurvival" in engine and "HumanSurvival" in engine
# The older, hook-only arbitration and the extra preservation guards must not come back: they let
# the agent keep an input that the 26 tick arbitration had already rejected.
assert "PlayerSurv" not in engine and "BestSurvHook0" not in engine, \
    "the Legit hook guard / Blatant preservation guards are back"

# ---------------------------------------------------------------------------------------------
# v5.1 airborne rescue mechanisms (reference spec 5.7 - 5.9) and the 12 branch Cartesian action
# space (spec 7.2). Without these three the agent watches a player who never releases the hook
# swing into the freeze, can never spend an air jump on its own, and cannot grab a ceiling that the
# crosshair is not pointing at - the exact three gaps this version closes.
# ---------------------------------------------------------------------------------------------
# The engine owns the world probes and the dispatcher-side wiring.
for signature in ("bool CanUseAirJump(", "bool CheckPreemptiveHookRelease(", "bool CheckHeadroomClearance(",
                  "bool TryEmergencyAirJump(", "bool TryEmergencyWallCeilingHook("):
    assert signature in header, signature + " left the engine header"
    assert signature in engine, signature + " left the engine"
# 5.7: two lookaheads that differ in the hook bit only, and the hook is stolen only when that wins.
assert "PreemptiveHookReleaseWins(" in decision, "the snatch rule left the decision header"
assert "if(pChar->Core()->m_HookState != HOOK_GRABBED && CurrentInput.m_Hook == 0)" in engine, \
    "the snatch no longer skips a tee that is neither hanging nor holding the key"
assert "const int KeepSurvival = SimulateCandidate(pClient, pWorld, CandKeep, CheckTicks, Flags);" in engine
assert "const int ReleaseSurvival = SimulateCandidate(pClient, pWorld, CandRelease, CheckTicks, Flags);" in engine
assert "pOutInput->m_Hook = 0;" in engine, "the forced release no longer clears the hook key"
# 5.8: air jump capability, the 48 px headroom probe and the gain threshold.
assert "!(pChar->Core()->m_Jumped & 2) && !pChar->IsGrounded()" in engine, \
    "the air jump capability check lost the m_Jumped bit that marks a spent air jump"
assert "HeadroomAllowsAirJump(" in decision and "SHeadroomProbe" in decision
assert "CheckHeadroomClearance(pWorld->Collision(), pChar->Core()->m_Pos, AIR_JUMP_HEADROOM)" in engine
assert "AIR_JUMP_HEADROOM = 48.0f" in decision and "AIR_JUMP_MIN_CLEARANCE = 32.0f" in decision
assert "AIR_JUMP_MIN_GAIN_TICKS = 8" in decision, "the air jump gain threshold left the reference value"
assert "BestScore > Baseline && BestScore >= AIR_JUMP_MIN_GAIN_TICKS" in engine
# 5.9: the five ray fan, the reach and the anchor filter.
assert "EmergencyRadarDirs()" in decision and "EMERGENCY_RADAR_RAYS = 5" in decision
assert "HOOK_MAX_DISTANCE = 380.0f" in decision and "RadarTargetIsHookable(" in decision
assert "pEscapeDirs[i] * HOOK_MAX_DISTANCE" in engine, "the radar no longer uses the hook reach"
assert "RADAR_MIN_SURVIVAL_TICKS = 10" in decision and "BestSurvival > RADAR_MIN_SURVIVAL_TICKS" in engine
assert "Cand.m_Hook = 1; // force the hook out, whatever the player is pressing" in engine
# The reference writes `Tile & (TILE_DEATH | TILE_FREEZE)`, a mask over 2 | 9 = 11, which also
# matches tile 1 (solid) and 11 (unfreeze). The rules have to compare tile *indices*.
assert "Tile == TILE_DEATH || IsFreezingTile(Tile)" in decision
assert "& (TILE_DEATH | TILE_FREEZE)" not in decision and "& (TILE_FREEZE | TILE_DEATH | TILE_NOHOOK)" not in decision
# 7.2: the Blatant action space is the full Cartesian product, jump axis included.
assert "s_aDirs[3] = {0, -1, 1}" in decision, "the Blatant search lost the reference direction axis"
assert "s_aJumps[2] = {0, 1}" in decision and "const int JumpCount = CanAirJump ? 2 : 1;" in decision
assert "CanAirJump" in engine and \
       "BuildBlatantCandidates(Ctx.m_Input, Set.m_BlatantDirection, Set.m_BlatantHook, CanAirJump, &vActions)" in engine, \
    "the Blatant greedy search is back to a jump-less action space"
assert "const bool CanAirJump = CanUseAirJump(pWorld, m_pClient->m_Snap.m_LocalClientId);" in engine
# 8.3: the Legit tree opens the same air jump axis, and mounts the snatch before its arbitration.
assert "BuildLegitCandidates(pCurr->m_Action, Set.m_LegitDirection, Set.m_LegitHook, CanAirJump, &vCandidates)" in engine, \
    "the Legit MCTS no longer expands air jump nodes"
legit_at = engine.index("AvoidInput CLegitAgent::GetAction(")
legit_snatch_at = engine.index("CheckPreemptiveHookRelease(m_pClient, pWorld, Ctx.m_Input, &SnatchInput, CheckTicks, Flags)", legit_at)
legit_arbitration_at = engine.index("ArbitrationGain(CandidateSurvival, HumanSurvival, LEGIT_ARBITRATION_TICKS)", legit_at)
assert legit_at < legit_snatch_at < legit_arbitration_at, \
    "the Legit snatch no longer runs between the search and the 26 tick arbitration"

# Blatant cascade (spec 7.6): kick-in hysteresis, preemptive hook release, auto drag, emergency air
# jump, upper hemisphere radar, unfreeze escape, concurrent greedy search, NSIF, best effort - in
# exactly that order. The eight levels are what makes the tier aggressive; a level that moved below
# another one is a level that no longer runs when the tee is already dying.
assert "Set.m_KickInTicks" in engine and "m_SavedSafeSequence" in engine
assert "s_aHooks[2] = {0, 1}" in decision
assert "Set.m_AutoDrag" in engine and "auto drag a teammate" in engine, "auto drag (spec 7.3) is gone"
assert "FindNearestUnfreezeTile" in engine and "escape to an unfreeze tile" in engine, \
    "the unfreeze escape (spec 7.4) is gone"
assert "Set.m_Aimbot" in engine and "BestSurvivalAim" in engine and "BestNearAim" in engine
kick_at = engine.index("const int KickScore = SimulateCandidate")
snatch_at = engine.index("// --- 2. Preemptive hook release")
drag_at = engine.index("// --- 3. Auto drag")
airjump_at = engine.index("// --- 4. Emergency air jump")
radar_at = engine.index("// --- 5. Emergency upper hemisphere radar")
unfreeze_at = engine.index("// --- 6. Unfreeze escape")
greedy_at = engine.index("// --- 7. Greedy search")
nsif_at = engine.index("// --- 8. NSIF")
assert kick_at < snatch_at < drag_at < airjump_at < radar_at < unfreeze_at < greedy_at < nsif_at, \
    "the Blatant cascade lost its order"
assert "CheckPreemptiveHookRelease(m_pClient, pWorld, Ctx.m_Input, &SnatchInput, CheckTicks, Flags)" in engine, \
    "the Blatant cascade no longer steals a hook that is about to swing the tee into the freeze"
assert "TryEmergencyAirJump(m_pClient, pWorld, Ctx.m_Input, &JumpInput, CheckTicks, Flags)" in engine, \
    "the Blatant cascade no longer spends the air jump on its own"
assert "TryEmergencyWallCeilingHook(m_pClient, pWorld, Ctx.m_Input, &WallHookInput, CheckTicks, Flags)" in engine, \
    "the Blatant cascade no longer reaches for the ceiling above the tee"
# The candidate ring of one greedy round is simulated concurrently (0x14032eb50).
assert "SimulateBranchesParallel(" in engine and "std::async(std::launch::async" in engine, \
    "the greedy search is no longer concurrent"
assert "BuildBlatantCandidates(Ctx.m_Input" in engine
# NSIF keeps a single step instead of spending it, so a doomed fall still gets the best input.
assert "(Set.m_Nsif || Set.m_TrackPoint) && !m_SavedSafeSequence.empty()" in engine, \
    "NSIF no longer honours the track point switch"
# Fentbot (reference spec 8): the velocity/flow dot product weight and the preset table.
assert "FENT_FLOW_WEIGHT = 1750.0f" in engine
assert "Set.m_FentActions = 88;" in engine
assert "Set.m_FentActions = 160;" in engine
assert "Set.m_FentActions = 1000;" in engine
assert "Set.m_FentDosage = 300;" in engine
# Pilot (reference spec 10)
assert "m_PilotPopulation" in engine and "m_PilotTopK" in engine and "m_PilotSequence" in engine

# Tile editor (reference spec 9): both planners read the same instance, and the grid it defines is
# what the flow field is built from.
assert "class CTileEditor" in editor_header and "Marker(" not in editor_header
assert "void CNavigator::Rebuild(CCollision *pCollision, bool LightTile, int LightRadius, const CTileEditor *pEditor)" in engine
assert "m_pEditor->IsTunnel(X, Y)" in engine and "m_pEditor->IsFinish(X, Y)" in engine, \
    "the navigator ignores the tile editor"
assert "EditorRevision()" in engine and "m_Nav.EditorRevision() != EditorRevision" in engine, \
    "an edit no longer invalidates the flow field"
assert "Avoid::CTileEditor m_TileEditor" in open("src/game/client/components/bestclient/avoid.h", encoding="utf-8").read()
assert "Ctx.m_pTileEditor = &m_TileEditor;" in component, "the editor is not handed to the agents"
assert "m_TileEditor.AutoFinish(" in component and "m_TileEditor.AutoTunnels(" in component
assert "m_TileEditor.Interact(" in component, "the in world tile editor is gone"
for action in ("bc_avoid_tile_editor_clear", "bc_avoid_tile_editor_auto_finish", "bc_avoid_tile_editor_auto_tunnel"):
    member = "m_" + "".join(part.capitalize() for part in action.split("_"))
    assert member in component, action + " is declared but never consumed"
assert "IsTunnel" in editor_header and "IsFinish" in editor_header
assert "AutoTunnels" in editor and "AutoFinish" in editor and "Interact" in editor and "ClearAll" in editor

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
