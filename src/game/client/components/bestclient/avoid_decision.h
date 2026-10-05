/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_DECISION_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_DECISION_H

#include <base/str.h>
#include <base/vmath.h>

#include <game/mapitems.h>

#include <generated/protocol.h>

#include <cmath>
#include <vector>

/* ---------------------------------------------------------------------------------------------
 * Pure decision helpers of the Avoid agents (reference specs 5.5 - 5.9, 6.2, 6.3, 7.2, 7.3 and 8.3).
 *
 * They live in a header of their own because they are the parts of the pipeline that must be
 * exactly right and are worth testing on their own: the Legit root selection used to pick the first
 * child of its tree whenever visit counts were tied (which made the bot walk left on its own), the
 * enumeration order of the candidate sets decides every survival tie of the Blatant agent, the
 * 10 tick probe threshold decides whether a heavy search is woken at all, the v5.1 rescue rules
 * decide when a hook is stolen from the player or an air jump is spent, and the 26 tick gain
 * arbitration decides whether a search answer is allowed to touch the player's input.
 *
 * Everything here is a pure function of its arguments - no world, no client, no settings object -
 * so src/test/avoid_decision_test.cpp can pin the formulas down without a running game.
 * ------------------------------------------------------------------------------------------- */
namespace Avoid
{
	// -------------------------------------------------------------------------------------
	// Pre-activation pipeline (reference spec 5)
	// -------------------------------------------------------------------------------------

	// Reference spec 5.5, assembly 0x140312258: `mov edx, 0xa` - the cheap baseline probe looks
	// exactly this many ticks ahead, with no CVar behind it.
	constexpr int PROBE_CHECK_TICKS = 10;

	// Reference spec 5.5, assembly 0x14031226d: `cmp eax, 0x7; jge`. Surviving at least this many
	// of the 10 probe ticks means the player's own input has enough margin, so the whole heavy
	// machinery (MCTS, greedy search, aim scan) is skipped and the input passes through untouched.
	constexpr int PROBE_SAFE_TICKS = 7;

	// Reference spec 5.6, assembly `mov edx, 0x15`: every candidate crosshair of the sector scan is
	// pushed this far before it is scored.
	constexpr int SECTOR_SCAN_TICKS = 21;

	// Reference spec 8.3, assembly 0x140338a14: `mov edi, 0x1a` - the fixed horizon of the Legit
	// post hoc arbitration. It does not follow the Legit check-ticks CVar.
	constexpr int LEGIT_ARBITRATION_TICKS = 26;

	// Reference spec 7.3: the auto drag rescue only considers tees inside the maximum hook reach,
	// and ignores ones that are practically on top of the player.
	constexpr float AUTO_DRAG_MAX_DIST = 380.0f;
	constexpr float AUTO_DRAG_MIN_DIST = 16.0f;

	// Reference spec 5.9 / 5.7: the emergency upper hemisphere radar casts its rays up to the
	// maximum hook reach of the engine (the default m_HookLength tuning is the same 380 px).
	constexpr float HOOK_MAX_DISTANCE = 380.0f;

	// Reference spec 5.8: the headroom probe of the air jump. The ray is 48 px long - the impulse of
	// the air jump needs that much room to be worth anything - and anything closer than one tile
	// (32 px) is a ceiling the tee would immediately bounce off.
	constexpr float AIR_JUMP_HEADROOM = 48.0f;
	constexpr float AIR_JUMP_MIN_CLEARANCE = 32.0f;

	// Reference spec 5.8: the injected air jump has to buy at least this many ticks over doing
	// nothing before the agent spends the one air jump the tee has.
	constexpr int AIR_JUMP_MIN_GAIN_TICKS = 8;

	// Reference spec 5.9: a radar candidate only replaces the player's own input once it survives
	// strictly more than this many ticks of the check window.
	constexpr int RADAR_MIN_SURVIVAL_TICKS = 10;

	// Reference spec 5.9: the five escape rays of the upper hemisphere radar, in reference order.
	// They deliberately ignore where the player is aiming: a tee that falls into freeze while the
	// crosshair points at the floor has to be able to grab the ceiling above it.
	inline const vec2 *EmergencyRadarDirs()
	{
		static const vec2 s_aDirs[5] = {
			vec2(0.0f, -1.0f), // straight up: the ceiling
			vec2(-0.7071f, -0.7071f), // up-left, 45 degrees
			vec2(0.7071f, -0.7071f), // up-right, 45 degrees
			vec2(-1.0f, -0.2f), // the wall on the left
			vec2(1.0f, -0.2f), // the wall on the right
		};
		return s_aDirs;
	}
	constexpr int EMERGENCY_RADAR_RAYS = 5;

	// Reference spec 5.1 (0x1402f3f88 / 0x14053232c): the four game types whose rules make the
	// freeze prediction meaningless or actively harmful. The reference compares the whole game type
	// string case insensitively, so "fng" is blocked but "ddracenetwork" is not.
	inline bool IsBlacklistedGametype(const char *pGametype)
	{
		if(pGametype == nullptr || pGametype[0] == '\0')
			return false;

		static const char *const s_apBlacklist[] = {
			"fng",
			"vanilla",
			"f-ddrace",
			"blockworlds",
		};

		for(const char *pBlack : s_apBlacklist)
		{
			if(str_comp_nocase(pGametype, pBlack) == 0)
				return true;
		}
		return false;
	}

	// Reference spec 5.5: `cmp eax, 0x7; jge 0x1403125fb`. A probe that survived at least
	// PROBE_SAFE_TICKS of its window reports the fully safe sentinel, which is what makes the
	// dispatcher leave the player alone; anything below that is the raw survival count.
	inline bool ProbeIsSafe(int SurvivalTicks)
	{
		return SurvivalTicks >= PROBE_SAFE_TICKS;
	}

	// Reference spec 5.6: the sector scan walks the field of view from one edge to the other in
	// `Segments` steps, which is one candidate more than there are segments. FovRad is the full
	// opening angle, so index 0 aims at -FOV/2 and index Segments at +FOV/2.
	inline float SectorScanAngle(float BaseAngle, float FovRad, int Segments, int Index)
	{
		const int Steps = Segments < 1 ? 1 : Segments;
		const float Start = BaseAngle - FovRad * 0.5f;
		const float Step = FovRad / (float)Steps;
		return Start + (float)Index * Step;
	}

	// -------------------------------------------------------------------------------------
	// v5.1 rescue mechanisms (reference spec 5.7 - 5.9)
	//
	// These are the three rules that keep a tee alive in the air: letting go of a hook that is
	// about to swing it into the freeze, spending the air jump while there is headroom for it, and
	// grabbing the ceiling or a side wall while the crosshair points somewhere useless. The world
	// probes live in avoid_engine.cpp; only the decisions they feed are pinned down here.
	// -------------------------------------------------------------------------------------

	// TILE_FREEZE is 9 and TILE_DEATH is 2, TILE_NOHOOK is 3. The reference ORs the first two into a
	// bit mask and ANDs it onto the tile index; over 2 | 9 = 11 that mask also matches tile 1
	// (solid), 3 (nohook), 8, 10 and 11 (unfreeze) - exactly the kind of mask the simulator must not
	// use on a tile *index*. The three reads below compare indices instead.
	inline bool IsFreezingTile(int Tile)
	{
		return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
	}

	// A tile the tee must not jump into or hang from: it freezes, it kills, or it cannot be hooked.
	inline bool IsLethalOrFreezingTile(int Tile)
	{
		return Tile == TILE_DEATH || IsFreezingTile(Tile);
	}

	// Reference spec 5.9: the radar may only anchor on something that is neither lethal, nor
	// freezing, nor unhookable.
	inline bool RadarTargetIsHookable(int Tile)
	{
		return !IsLethalOrFreezingTile(Tile) && Tile != TILE_NOHOOK;
	}

	// What the headroom probe of reference spec 5.8 measured above the tee.
	struct SHeadroomProbe
	{
		bool m_Hit = false; // the 48 px ray hit something
		float m_Distance = 0.0f; // how far above the tee that something is
		int m_HitTile = 0; // collision tile at the hit
		int m_AboveTile = 0; // collision tile one tile above the tee
	};

	// Reference spec 5.8: an air jump is only allowed when the tee can actually rise. A ceiling
	// closer than one tile (32 px), a lethal/freezing ceiling, or a lethal/freezing tile directly
	// above the tee all veto it - jumping there would bounce the tee straight back into the
	// hazard it is trying to leave.
	inline bool HeadroomAllowsAirJump(const SHeadroomProbe &Probe, float MinClearance = AIR_JUMP_MIN_CLEARANCE)
	{
		if(Probe.m_Hit && Probe.m_Distance < MinClearance)
			return false;
		if(Probe.m_Hit && IsLethalOrFreezingTile(Probe.m_HitTile))
			return false;
		if(IsLethalOrFreezingTile(Probe.m_AboveTile))
			return false;
		return true;
	}

	// Reference spec 5.7, assembly 0x1403286f0 - 0x1403289e0: hold the hook and the pendulum takes
	// the tee into the freeze inside the check window, let go and the tangential speed carries it
	// across. Only the second case may steal the hook key from the player. The two survivals are
	// compared inside the window (see PreemptiveHookReleaseWins below).

	// Survived ticks of a finished lookahead, expressed inside its own horizon: the simulator
	// reports the sentinel (9999) when the whole window was survived, and both sides of the Legit
	// arbitration have to be measured on the same scale before they are subtracted.
	inline int ArbitrationSurvival(int SurvivalTicks, int Horizon)
	{
		return SurvivalTicks > Horizon ? Horizon : SurvivalTicks;
	}

	// Reference spec 8.3, assembly 0x140338a5f: `sub ebx, eax; cmp ebx, 0x1; cmovge ecx, r15d`.
	// The candidate action only replaces the player's own input when it survives at least one tick
	// longer over the arbitration horizon. Anything below that is noise and is rejected, which is
	// what keeps the bot from twitching around a safe edge.
	inline int ArbitrationGain(int CandidateSurvival, int HumanSurvival, int Horizon)
	{
		return ArbitrationSurvival(CandidateSurvival, Horizon) - ArbitrationSurvival(HumanSurvival, Horizon);
	}

	inline bool ArbitrationAllowsOverride(int CandidateSurvival, int HumanSurvival, int Horizon)
	{
		return ArbitrationGain(CandidateSurvival, HumanSurvival, Horizon) >= 1;
	}

	// Reference spec 5.7, assembly 0x1403286f0 - 0x1403289e0: hold the hook and the pendulum takes
	// the tee into the freeze inside the check window; let go and the tangential speed carries it
	// across instead. Only the second case may steal the hook key from the player. Both survivals
	// are compared inside the window, so the simulator sentinel becomes the horizon itself.
	inline bool PreemptiveHookReleaseWins(int KeepSurvival, int ReleaseSurvival, int CheckTicks)
	{
		const int Keep = ArbitrationSurvival(KeepSurvival, CheckTicks);
		const int Release = ArbitrationSurvival(ReleaseSurvival, CheckTicks);
		return Keep < CheckTicks && Release > Keep;
	}

	// Reference spec 7.2: asymmetric multi objective UCT heuristic. Matching the player's direction
	// is worth `2 * WeightDir * 0.01` and matching the hook state `WeightHook * 0.01`, so the
	// player's own action always carries the largest human-likeness bonus.
	inline float LegitHeuristicScore(
		const CNetObj_PlayerInput &Action,
		const CNetObj_PlayerInput &HumanInput,
		int SurvivalTicks,
		int WeightDir,
		int WeightHook,
		int WeightLifespan)
	{
		// Reference spec 7.2, float scale constant 0x1405300e4.
		constexpr float WEIGHT_SCALE = 0.01f;

		// 1. Direction consistency, full marks at 2.0 for "exactly what the player pressed".
		const float DirDiff = std::abs((float)Action.m_Direction - (float)HumanInput.m_Direction);
		const float DirScore = std::abs(DirDiff - 2.0f) * ((float)WeightDir * WEIGHT_SCALE);

		// 2. Hook state consistency, full marks at 1.0.
		const float HookDiff = std::abs((float)Action.m_Hook - (float)HumanInput.m_Hook);
		const float HookScore = std::abs(HookDiff - 1.0f) * ((float)WeightHook * WEIGHT_SCALE);

		// 3. Survival bonus.
		const float LifeScore = (float)SurvivalTicks * ((float)WeightLifespan * WEIGHT_SCALE);

		return DirScore + HookScore + LifeScore;
	}

	// One root child of the Legit search as the final decision sees it (reference spec 7.3).
	struct SLegitRootChild
	{
		CNetObj_PlayerInput m_Action{};
		int m_Visits = 0;
		double m_TotalValue = 0.0;
		int m_LifespanTicks = 0;
	};

	// Reference spec 7.3, `0x1403392cb`: the final decision evaluates exploitation + heuristic with
	// the exploration term set to zero and skips children that were never rolled out. Returns the
	// index of the winner inside vChildren, or -1 when the whole ring is unvisited.
	inline int SelectLegitRootChild(
		const std::vector<SLegitRootChild> &vChildren,
		const CNetObj_PlayerInput &HumanInput,
		int WeightDir,
		int WeightHook,
		int WeightLifespan)
	{
		int BestIndex = -1;
		double BestScore = -1e38;
		for(size_t i = 0; i < vChildren.size(); ++i)
		{
			const SLegitRootChild &Child = vChildren[i];
			if(Child.m_Visits == 0)
				continue;

			const double Exploitation = Child.m_TotalValue / (double)Child.m_Visits;
			const double Heuristic = (Child.m_LifespanTicks <= 0) ? 0.0 : (double)LegitHeuristicScore(Child.m_Action, HumanInput, Child.m_LifespanTicks, WeightDir, WeightHook, WeightLifespan);
			const double Score = Exploitation + Heuristic;

			bool Replace = false;
			if(Score > BestScore + 1e-7)
			{
				Replace = true;
			}
			else if(std::abs(Score - BestScore) <= 1e-7 && BestIndex >= 0)
			{
				const SLegitRootChild &CurrentBest = vChildren[(size_t)BestIndex];
				const bool CandMatchesHook = (Child.m_Action.m_Hook == HumanInput.m_Hook);
				const bool BestMatchesHook = (CurrentBest.m_Action.m_Hook == HumanInput.m_Hook);
				const bool CandMatchesDir = (Child.m_Action.m_Direction == HumanInput.m_Direction);
				const bool BestMatchesDir = (CurrentBest.m_Action.m_Direction == HumanInput.m_Direction);

				if(CandMatchesHook && !BestMatchesHook)
				{
					Replace = true;
				}
				else if(CandMatchesHook == BestMatchesHook)
				{
					if(CandMatchesDir && !BestMatchesDir)
						Replace = true;
				}
			}

			if(Replace)
			{
				BestScore = Score;
				BestIndex = (int)i;
			}
		}
		return BestIndex;
	}

	// Reference spec 8.3 step 2: the children one expansion creates, in reference order.
	//
	// The direction ring carries the jump axis: while the tee still has its air jump and headroom
	// for it (CanAirJump, decided by the caller) every direction is offered twice, once without and
	// once with m_Jump = 1. That is what lets the search spend the air jump on its own instead of
	// only when the player happens to hold the key.
	//
	// With no air jump left the parent's jump key is inherited rather than cleared: the reference
	// snippet assigns `m_Jump = j` with a single option 0 there, which would erase a held ground
	// jump from every child and leave the agent unable to jump over anything while grounded.
	inline void BuildLegitCandidates(
		const CNetObj_PlayerInput &Parent,
		bool DirectionAssist,
		bool HookAssist,
		bool CanAirJump,
		std::vector<CNetObj_PlayerInput> *pvOut)
	{
		auto Push = [pvOut](CNetObj_PlayerInput Act) {
			if(Act.m_TargetX == 0 && Act.m_TargetY == 0)
				Act.m_TargetY = -1;
			pvOut->push_back(Act);
		};

		pvOut->clear();
		static const int s_aDirs[3] = {-1, 0, 1};
		static const int s_aHooks[2] = {0, 1};

		if(DirectionAssist)
		{
			// The hook = 0 ring comes first; with hook assistance off the current hook state is
			// kept instead.
			const int BaseHook = HookAssist ? 0 : Parent.m_Hook;
			for(const int Dir : s_aDirs)
			{
				for(int Jump = 0; Jump <= (CanAirJump ? 1 : 0); ++Jump)
				{
					CNetObj_PlayerInput Act = Parent;
					Act.m_Direction = Dir;
					Act.m_Hook = BaseHook;
					if(CanAirJump)
						Act.m_Jump = Jump;
					Push(Act);
				}
			}
			if(HookAssist)
			{
				// The hook = 1 ring inherits the parent's jump state, exactly like the reference.
				for(const int Dir : s_aDirs)
				{
					CNetObj_PlayerInput Act = Parent;
					Act.m_Direction = Dir;
					Act.m_Hook = 1;
					Push(Act);
				}
			}
		}
		else if(HookAssist)
		{
			for(const int Hook : s_aHooks)
			{
				CNetObj_PlayerInput Act = Parent;
				Act.m_Hook = Hook;
				Push(Act);
			}
		}
	}

	// Reference spec 7.2 (supersedes the 0x14032e530 priority ring): the Blatant agent does not
	// walk a preference list, it enumerates the whole action space at once -
	// Dirs[3] x Hooks[2] x Jumps[2] = up to 12 branches, simulated in parallel.
	//
	// The jump axis only exists while the tee still has an air jump and is in the air (CanAirJump),
	// and without it the player's own jump key is inherited. Leaving the axis out entirely is what
	// made the agent unable to generate a self rescue out of a fall: the greedy search could only
	// pick what the player was already pressing.
	//
	// The loop order is the reference order and it decides every survival tie, because the greedy
	// search keeps the first branch that reaches the best survival count.
	inline void BuildBlatantCandidates(
		const CNetObj_PlayerInput &BaseInput,
		bool DirectionAssist,
		bool HookAssist,
		bool CanAirJump,
		std::vector<CNetObj_PlayerInput> *pvOut)
	{
		// Reference spec 7.2: Dirs[3] = {0, -1, 1}, Hooks[2] = {0, 1}, Jumps[2] = {0, 1}.
		static const int s_aDirs[3] = {0, -1, 1};
		static const int s_aHooks[2] = {0, 1};
		static const int s_aJumps[2] = {0, 1};

		const int DirCount = DirectionAssist ? 3 : 1;
		const int HookCount = HookAssist ? 2 : 1;
		const int JumpCount = CanAirJump ? 2 : 1;

		pvOut->clear();
		pvOut->reserve((size_t)DirCount * (size_t)HookCount * (size_t)JumpCount);

		for(int d = 0; d < DirCount; ++d)
		{
			for(int h = 0; h < HookCount; ++h)
			{
				for(int j = 0; j < JumpCount; ++j)
				{
					CNetObj_PlayerInput Act = BaseInput;
					if(DirectionAssist)
						Act.m_Direction = s_aDirs[d];
					if(HookAssist)
						Act.m_Hook = s_aHooks[h];
					if(CanAirJump)
						Act.m_Jump = s_aJumps[j];

					// Teeworlds protocol: (0, 0) is not a valid aim vector.
					if(Act.m_TargetX == 0 && Act.m_TargetY == 0)
						Act.m_TargetY = -1;

					pvOut->push_back(Act);
				}
			}
		}
	}

	// A candidate input that walks and aims at Target (reference spec 6.3 / 6.4). The aim vector is
	// the raw offset, exactly like the reference builds the auto drag candidate.
	inline CNetObj_PlayerInput BuildApproachInput(const CNetObj_PlayerInput &Base, vec2 From, vec2 Target, bool Hook)
	{
		CNetObj_PlayerInput Out = Base;
		const vec2 Delta = Target - From;

		Out.m_TargetX = (int)Delta.x;
		Out.m_TargetY = (int)Delta.y;
		if(Out.m_TargetX == 0 && Out.m_TargetY == 0)
			Out.m_TargetY = -1;

		if(Delta.x > 12.0f)
			Out.m_Direction = 1;
		else if(Delta.x < -12.0f)
			Out.m_Direction = -1;
		else
			Out.m_Direction = 0;

		Out.m_Hook = Hook ? 1 : 0;
		return Out;
	}
} // namespace Avoid

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_DECISION_H
