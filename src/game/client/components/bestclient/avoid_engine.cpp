/* Copyright © 2026 BestProject Team */
#include "avoid_engine.h"

#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <game/collision.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace Avoid
{
	namespace
	{
		// `bc_avoid_jump_weight` does not exist: jump fidelity is not a slider, it is an internal
		// preference, so a jump costs a fixed amount of "deviation" against the player's own input.
		// It sits between "no change" (0) and the biggest slider value (200), which is what keeps
		// the agent from hopping in place when a jump buys nothing.
		constexpr int JUMP_DEVIATION_WEIGHT = 60;

		// The whole decision must stay inside the 1.5 ms tick budget of the delivery document
		// (6.4). The search checks the clock before every iteration, so even `quality = 200` stops
		// in time; the remaining head room is what the sensing scan of the caller spends.
		constexpr float SEARCH_BUDGET_MS = 1.0f;

		// A jump is a last resort, not a reflex:
		//  * while the hook is engaged the answer has to come from the rope (release, steer, brake),
		//    never from hopping - a jump there reads as a glitch and fights the swing the player is
		//    flying;
		//  * otherwise the jump waits until the player's own input is this close to dying, so the
		//    agent only leaves the ground at the critical moment instead of two tiles early (the
		//    player still has ~0.12 s to react on their own and no intervention happens at all).
		constexpr int JUMP_URGENCY_TICKS = 6;

		float ElapsedMs(int64_t StartTime)
		{
			return (float)((time_get() - StartTime) * 1000.0 / (double)time_freq());
		}
	} // namespace

	// -----------------------------------------------------------------------------------------
	// Hazard sensing
	// -----------------------------------------------------------------------------------------

	int ClassifyTile(int Tile)
	{
		switch(Tile)
		{
		case TILE_DEATH: return HAZ_DEATH;
		case TILE_FREEZE: return HAZ_FREEZE;
		case TILE_DFREEZE: return HAZ_DEEP;
		case TILE_LFREEZE: return HAZ_LIVE;
		case TILE_UNFREEZE: return HAZ_UNFREEZE;
		case TILE_TELEIN:
		case TILE_TELEOUT:
		case TILE_TELECHECK:
		case TILE_TELECHECKIN:
		case TILE_TELECHECKOUT:
		case TILE_TELEINEVIL:
		case TILE_TELECHECKINEVIL:
		case TILE_TELEINWEAPON:
		case TILE_TELEINHOOK:
			return HAZ_TELE;
		default:
			return HAZ_NONE;
		}
	}

	vec2 TileBoxDelta(vec2 Pos, int TileX, int TileY)
	{
		const float Left = TileX * TILE_SIZE;
		const float Top = TileY * TILE_SIZE;
		return vec2(
			std::max(0.0f, std::max(Left - Pos.x, Pos.x - (Left + TILE_SIZE))),
			std::max(0.0f, std::max(Top - Pos.y, Pos.y - (Top + TILE_SIZE))));
	}

	float DistanceToTileBox(vec2 Pos, int TileX, int TileY)
	{
		return length(TileBoxDelta(Pos, TileX, TileY));
	}

	// How much shorter the vertical sensing reach is than the horizontal one on this map: over one
	// lookahead window the tee can cover `ground_control_speed * ticks` sideways, but only
	// `0.5 * gravity * ticks^2` downwards, and gravity wins the moment the tee leaves the ground.
	float SensingVerticalFactor(const CCharacterCore &Core, const SSettings &Set)
	{
		const float Ticks = (float)std::clamp(Set.m_CheckTicks, 2, MAX_SIM_TICKS);
		const float Horizontal = Core.m_Tuning.m_GroundControlSpeed * Ticks;
		const float Vertical = 0.5f * Core.m_Tuning.m_Gravity * Ticks * Ticks;
		if(Horizontal <= 0.0f)
			return SENSING_VERTICAL_FALLBACK;
		return std::clamp(Vertical / Horizontal, 0.25f, 1.0f);
	}

	int HazardMask(const SSettings &Set)
	{
		int Mask = HAZ_NONE;
		if(Set.m_TileDeath)
			Mask |= HAZ_DEATH;
		if(Set.m_TileFreeze)
			Mask |= HAZ_FREEZE | HAZ_DEEP | HAZ_LIVE;
		if(Set.m_TileUnfreeze)
			Mask |= HAZ_UNFREEZE;
		if(Set.m_TileTele)
			Mask |= HAZ_TELE;
		return Mask;
	}

	bool IsRelevantHazard(const SSettings &Set, int Flags)
	{
		// Being frozen right now is a state, not a tile: it always matters.
		if(Flags & HAZ_SELF)
			return true;
		return (Flags & HazardMask(Set)) != 0;
	}

	int ClassifyPoint(CCollision *pCollision, vec2 Pos)
	{
		int Flags = HAZ_NONE;
		// The tile helpers index straight into the collision arrays, so refuse to probe while no
		// map is loaded (`GetPureMapIndex` would clamp against an empty grid, and `GetTileIndex`
		// would dereference a null tile array).
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return Flags;

		// Centre probe: freeze family, unfreeze, teleport and death switches.
		// Mirrors CCharacter::HandleTiles().
		const int Index = pCollision->GetPureMapIndex(Pos);
		if(Index >= 0)
		{
			Flags |= ClassifyTile(pCollision->GetTileIndex(Index));
			Flags |= ClassifyTile(pCollision->GetFrontTileIndex(Index));
			Flags |= ClassifyTile(pCollision->GetSwitchType(Index));
		}

		// Corner probe: death tiles, mirrors CCharacter::HandleSkippableTiles().
		const float R = HAZARD_CORNER_PROBE;
		for(int Corner = 0; Corner < 4; ++Corner)
		{
			const float Px = Pos.x + ((Corner & 1) ? R : -R);
			const float Py = Pos.y + ((Corner & 2) ? R : -R);
			if(pCollision->GetCollisionAt(Px, Py) == TILE_DEATH ||
				pCollision->GetFrontCollisionAt(Px, Py) == TILE_DEATH)
			{
				Flags |= HAZ_DEATH;
				break;
			}
			const int CornerIndex = pCollision->GetPureMapIndex(vec2(Px, Py));
			if(CornerIndex >= 0 && pCollision->GetSwitchType(CornerIndex) == TILE_DEATH)
			{
				Flags |= HAZ_DEATH;
				break;
			}
		}

		return Flags;
	}

	SThreat ScanThreat(CCollision *pCollision, const SSettings &Set, const CCharacterCore &Core)
	{
		SThreat Threat;
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return Threat;

		const vec2 Pos = Core.m_Pos;

		if(Core.m_FreezeEnd != 0 || Core.m_DeepFrozen || Core.m_LiveFrozen)
			Threat.m_Flags |= HAZ_SELF;

		Threat.m_Flags |= ClassifyPoint(pCollision, Pos);

		// `bc_avoid_sensing_radius` is a radius in tiles with half tile steps, measured from the
		// tee to the *box* of a hazard tile. It is an ellipse, not a circle: horizontal reach is
		// the slider value, vertical reach is what gravity allows in the same time (see
		// SensingVerticalFactor()), so a pit two tiles below the tee is not treated like a wall two
		// tiles to the side. At 0.5 the agent can only react when it is already standing at the
		// edge.
		const float RadiusX = std::clamp(Set.m_SensingRadius, 0.5f, 16.0f);
		const float RadiusY = std::max(0.5f, RadiusX * SensingVerticalFactor(Core, Set));
		const float ReachX = RadiusX * TILE_SIZE;
		const float ReachY = RadiusY * TILE_SIZE;
		const int ScanX = (int)std::ceil(RadiusX);
		const int ScanY = (int)std::ceil(RadiusY);
		const int CenterX = (int)std::floor(Pos.x / TILE_SIZE);
		const int CenterY = (int)std::floor(Pos.y / TILE_SIZE);

		for(int Ty = CenterY - ScanY; Ty <= CenterY + ScanY; ++Ty)
		{
			for(int Tx = CenterX - ScanX; Tx <= CenterX + ScanX; ++Tx)
			{
				Threat.m_SensedTiles++;

				const vec2 TileCenter((Tx + 0.5f) * TILE_SIZE, (Ty + 0.5f) * TILE_SIZE);
				const int Flags = ClassifyPoint(pCollision, TileCenter);
				if(Flags == HAZ_NONE || !IsRelevantHazard(Set, Flags))
					continue;

				// Ellipse test: 1.0 sits exactly on the reach. Everything the tee could not get to
				// in time is dropped here, so the decision engine never even argues about it.
				const vec2 Delta = TileBoxDelta(Pos, Tx, Ty);
				const float Normalised = std::sqrt(
					(Delta.x / ReachX) * (Delta.x / ReachX) +
					(Delta.y / ReachY) * (Delta.y / ReachY));
				if(Normalised > 1.0f)
					continue;

				// The reported "nearest" stays the plain pixel distance to the tile box, which is
				// what the HUD prints and the overlay draws a line to.
				const float Dist = length(Delta);
				Threat.m_HazardTiles++;

				if(!Threat.m_HasNearest || Dist < Threat.m_NearestDistPx)
				{
					Threat.m_HasNearest = true;
					Threat.m_NearestDistPx = Dist;
					Threat.m_NearestPos = TileCenter;
				}
			}
		}

		Threat.m_OnHazard = IsRelevantHazard(Set, Threat.m_Flags);
		return Threat;
	}

	int ClassifyMovement(const CCharacterCore &Core, bool FlyHammer)
	{
		int Flags = MOVE_NORMAL;
		if(Core.m_Jetpack)
			Flags |= MOVE_JETPACK;
		if(Core.HookedPlayer() != -1)
			Flags |= MOVE_HOOKED_PLAYER;
		if(FlyHammer)
			Flags |= MOVE_FLY_HAMMER;
		return Flags;
	}

	// -----------------------------------------------------------------------------------------
	// Forward simulation
	// -----------------------------------------------------------------------------------------

	void SSimState::Init(const SContext &Ctx, const SEnvironment &Env)
	{
		const CCharacterCore &Src = Ctx.m_Core;
		m_pCollision = Env.m_pCollision;

		// A world of our own, so that the predicted players are the only companions of the clone.
		// Never the live world: TickDeferred() writes into the cores it finds there (hook drag),
		// and that must not reach the client's own prediction.
		m_World.m_pPrng = nullptr;
		for(auto &pCharacter : m_World.m_apCharacters)
			pCharacter = nullptr;
		m_NumShadows = 0;
		m_Deferred = false;

		m_Core.Reset();

		// Collect the predicted players first: with none of them in reach the clone has no reason
		// to carry a world of its own, and the simulation then stays bit-for-bit the stage 2 one
		// even though `bc_avoid_player_prediction` is on.
		int NumShadows = 0;
		if(Env.m_PredictPlayers && Env.m_pTeams && Env.m_pCollision)
		{
			for(int i = 0; i < Env.m_NumPlayers && NumShadows < MAX_SHADOW_PLAYERS; i++)
			{
				const SPlayerSnapshot &Snapshot = Env.m_aPlayers[i];
				if(Snapshot.m_Id < 0 || Snapshot.m_Id >= MAX_CLIENTS || Snapshot.m_Id == Env.m_LocalId)
					continue;

				m_aShadows[NumShadows] = Snapshot.m_Core;
				m_aShadows[NumShadows].m_Id = Snapshot.m_Id;
				NumShadows++;
			}
		}

		if(NumShadows > 0)
		{
			m_Core.SetCoreWorld(&m_World, Env.m_pCollision, Env.m_pTeams);
			// The clone takes the seat of the controlled tee, so `CanCollide()` / `CanKeepHook()`
			// answer for the real team and the switch (door) tiles of the real map stay correct.
			m_Core.m_Id = (Env.m_LocalId >= 0 && Env.m_LocalId < MAX_CLIENTS) ? Env.m_LocalId : -1;
			if(m_Core.m_Id >= 0)
				m_World.m_apCharacters[m_Core.m_Id] = &m_Core;

			// The switch (door) state of the real map has to survive into the private world,
			// otherwise the clone would walk through doors the player cannot pass. Assigning into
			// the member vector keeps its capacity, so this does not allocate after the first call.
			if(Env.m_pWorld)
				m_World.m_vSwitchers = Env.m_pWorld->m_vSwitchers;

			for(int i = 0; i < NumShadows; i++)
				m_World.m_apCharacters[m_aShadows[i].m_Id] = &m_aShadows[i];
			m_NumShadows = NumShadows;
			m_Deferred = true;
		}
		else
		{
			// Stage 2 configuration: no world of our own, no deferred tick. `m_Id = 0` keeps the
			// switch callback on a neutral team without touching the team logic.
			m_Core.SetCoreWorld(Env.m_pWorld, Env.m_pCollision, Env.m_pTeams);
			m_Core.m_Id = 0;
		}

		// Carry over everything the tick loop reads (the stage 2 carry-over list).
		m_Core.m_Pos = Src.m_Pos;
		m_Core.m_Vel = Src.m_Vel;
		m_Core.m_HookPos = Src.m_HookPos;
		m_Core.m_HookDir = Src.m_HookDir;
		m_Core.m_HookTeleBase = Src.m_HookTeleBase;
		m_Core.m_HookTick = Src.m_HookTick;
		m_Core.m_HookState = Src.m_HookState;
		m_Core.m_NewHook = Src.m_NewHook;
		m_Core.m_Jumped = Src.m_Jumped;
		m_Core.m_JumpedTotal = Src.m_JumpedTotal;
		m_Core.m_Jumps = Src.m_Jumps;
		m_Core.m_Direction = Src.m_Direction;
		m_Core.m_Angle = Src.m_Angle;
		m_Core.m_TriggeredEvents = Src.m_TriggeredEvents;
		m_Core.m_FreezeStart = Src.m_FreezeStart;
		m_Core.m_FreezeEnd = Src.m_FreezeEnd;
		m_Core.m_IsInFreeze = Src.m_IsInFreeze;
		m_Core.m_DeepFrozen = Src.m_DeepFrozen;
		m_Core.m_LiveFrozen = Src.m_LiveFrozen;
		m_Core.m_CollisionDisabled = Src.m_CollisionDisabled;
		m_Core.m_Solo = Src.m_Solo;
		m_Core.m_Super = Src.m_Super;
		m_Core.m_Invincible = Src.m_Invincible;
		m_Core.m_Jetpack = Src.m_Jetpack;
		m_Core.m_EndlessHook = Src.m_EndlessHook;
		m_Core.m_EndlessJump = Src.m_EndlessJump;
		m_Core.m_HookHitDisabled = Src.m_HookHitDisabled;
		// Map tuning, never hard-coded constants: on a `tune` map the physics is driven entirely
		// by this struct, and guessing gravity / control accel here would drift the prediction.
		m_Core.m_Tuning = Src.m_Tuning;
	}

	void SSimState::Step(const CNetObj_PlayerInput &Input)
	{
		// Predicted players are moved by their snapshot velocity. They are obstacles for the clone;
		// their own physics (gravity, walls, hooks) is not simulated, which is the documented
		// deviation of the player prediction (see the delivery document, 6.8).
		for(int i = 0; i < m_NumShadows; i++)
			m_aShadows[i].m_Pos += m_aShadows[i].m_Vel;

		m_Core.m_Input = Input;
		m_Core.Tick(true, m_Deferred);
		m_Core.Move();
	}

	int SimulateFixed(const SContext &Ctx, const CNetObj_PlayerInput &Input, int MaxTicks,
		const SEnvironment *pEnv, vec2 *pOutPos, vec2 *pOutVel)
	{
		const int Depth = std::clamp(MaxTicks, 0, MAX_SIM_TICKS);
		if(Depth <= 0)
			return 0;

		// The clone needs a collision context. Without one the agent has nothing to reason about,
		// so this mirrors the "no map data" bail-out of the caller instead of guessing.
		if(!pEnv || !pEnv->m_pCollision)
			return 0;

		// A planner instance owns the shared simulation buffers; SimulateFixed() only needs the
		// same stepping recipe, so it runs its own stack clone (exactly like stage 2 did).
		const SEnvironment &Env = *pEnv;
		SSimState Sim;
		Sim.Init(Ctx, Env);

		int Safe = 0;
		while(Safe < Depth)
		{
			Sim.Step(Input);

			// Exactly the stage 1 probe rules, so "the sensor says dangerous" and "the physics says
			// dangerous" can never drift apart.
			if(IsRelevantHazard(Ctx.m_Settings, ClassifyPoint(Env.m_pCollision, Sim.m_Core.m_Pos)))
				break;
			Safe++;
		}

		if(pOutPos)
			*pOutPos = Sim.m_Core.m_Pos;
		if(pOutVel)
			*pOutVel = Sim.m_Core.m_Vel;
		return Safe;
	}

	// -----------------------------------------------------------------------------------------
	// Legit agent: UCT search over direction x jump x hook
	// -----------------------------------------------------------------------------------------

	void CPlanner::BuildRoots(const SContext &Ctx)
	{
		const SSettings &Set = Ctx.m_Settings;
		const CNetObj_PlayerInput &Player = Ctx.m_Input;

		const int PlayerDirection = std::clamp(Player.m_Direction, -1, 1);
		const int PlayerJump = Player.m_Jump ? 1 : 0;
		const int PlayerHook = Player.m_Hook ? 1 : 0;

		// The player's own value always comes first: it is the reference plan, and every tie of the
		// search resolves to the earliest candidate, which is what makes "hands off" the default.
		int aDirections[3] = {PlayerDirection, 0, 0};
		int NumDirections = 1;
		if(Set.m_DirectionAssist)
		{
			for(int Direction = -1; Direction <= 1; Direction++)
			{
				if(Direction != PlayerDirection)
					aDirections[NumDirections++] = Direction;
			}
		}

		const int aJumps[2] = {PlayerJump, 1 - PlayerJump};

		int aHooks[3] = {PlayerHook, 0, 0};
		int NumHooks = 1;
		if(Set.m_HookAssist)
		{
			for(int Hook = 0; Hook <= 1; Hook++)
			{
				if(Hook != PlayerHook)
					aHooks[NumHooks++] = Hook;
			}
		}

		m_NumRoots = 0;
		for(int d = 0; d < NumDirections; d++)
		{
			for(int Jump : aJumps)
			{
				for(int h = 0; h < NumHooks; h++)
				{
					if(m_NumRoots >= MAX_ROOT_ACTIONS)
						return;

					const int Direction = aDirections[d];
					const int Hook = aHooks[h];
					SRoot &Root = m_aRoots[m_NumRoots++];
					Root = SRoot{};
					Root.m_Direction = Direction;
					Root.m_Jump = Jump;
					Root.m_Hook = Hook;
					Root.m_Input = Player;
					Root.m_Input.m_Direction = Direction;
					Root.m_Input.m_Jump = Jump;
					Root.m_Input.m_Hook = Hook;
					// How much of the player's own intent this action throws away. Survival is the
					// hard objective; this only decides between plans that survive equally long
					// (and, with `bc_avoid_life_weight`, how much survival a change is worth).
					Root.m_Deviation = Set.m_DirectionWeight * std::abs(Direction - PlayerDirection) +
							   Set.m_HookWeight * (Hook != PlayerHook ? 1 : 0) +
							   JUMP_DEVIATION_WEIGHT * (Jump != PlayerJump ? 1 : 0);
				}
			}
		}
	}

	bool CPlanner::JumpEngaged(const SContext &Ctx) const
	{
		// "The rope is the thing to solve": the player is holding the hook button (which is what
		// keeps a grabbed hook grabbed) or the clone is still flying/attached.
		if(Ctx.m_Input.m_Hook != 0)
			return true;
		return Ctx.m_Core.m_HookState == HOOK_GRABBED || Ctx.m_Core.m_HookState == HOOK_FLYING;
	}

	void CPlanner::UpdateAvailability(const SContext &Ctx, int PlayerSafe)
	{
		const int PlayerJump = Ctx.m_Input.m_Jump ? 1 : 0;
		const bool Hooked = JumpEngaged(Ctx);
		const bool Critical = PlayerSafe <= JUMP_URGENCY_TICKS;

		for(int i = 0; i < m_NumRoots; i++)
		{
			SRoot &Root = m_aRoots[i];
			// Only actions that *add* a jump are held back; keeping the player's own jump value (or
			// releasing it) never is.
			const bool AddsJump = Root.m_Jump != 0 && PlayerJump == 0;
			Root.m_Available = !AddsJump || (!Hooked && Critical);
		}
	}

	int CPlanner::Rollout(int RootIndex, bool Canonical, int Horizon, const SSettings &Set, CCollision *pCollision)
	{
		// Every rollout starts from the state of the decision tick again.
		m_Sim.m_Core = m_InitCore;
		for(int i = 0; i < m_Sim.m_NumShadows; i++)
			m_Sim.m_aShadows[i] = m_aInitShadows[i];

		const SRoot &Root = m_aRoots[RootIndex];
		CNetObj_PlayerInput Input = Root.m_Input;

		// A fixed candidate sequence, drawn before it is executed and never re-planned from the
		// simulation state. `bc_avoid_randomness` is how often the sequence leaves the root action;
		// at 0 the rollout is bit-for-bit the canonical one, which is what keeps the agent
		// deterministic for players who do not want the search to wander.
		const float DeviationChance = Set.m_Randomness / 400.0f;
		const unsigned DeviationThreshold = (unsigned)(DeviationChance * 1000.0f);

		for(int Tick = 0; Tick < Horizon; Tick++)
		{
			// Tick 0 is always the root action: the plan that gets committed has to be the action
			// whose survival was measured.
			if(!Canonical && Tick > 0 && DeviationThreshold > 0 && (m_Prng.RandomBits() % 1000) < DeviationThreshold)
				Input = m_aRoots[m_Prng.RandomBits() % (unsigned)m_NumRoots].m_Input;

			m_Sim.Step(Input);

			// Early pruning: once the clone touches a relevant hazard the sequence is dead and the
			// rest of the lookahead is not worth simulating.
			if(IsRelevantHazard(Set, ClassifyPoint(pCollision, m_Sim.m_Core.m_Pos)))
				return Tick;
		}
		return Horizon;
	}

	float CPlanner::MeanScore(const SRoot &Root, const SSettings &Set) const
	{
		const float Mean = Root.m_Visits > 0 ? (float)Root.m_SafeSum / (float)Root.m_Visits : 0.0f;
		return (float)Set.m_LifeWeight * Mean - (float)Root.m_Deviation;
	}

	float CPlanner::RankingScore(const SRoot &Root, const SSettings &Set) const
	{
		// Survival dominates, exactly like in Basic (`bc_avoid_life_weight` is what makes the trade
		// explicit). The deviation term is what makes the three sliders change the decision.
		return (float)Set.m_LifeWeight * (float)Root.m_CanonicalSafe - (float)Root.m_Deviation;
	}

	int CPlanner::SelectRoot(int TotalVisits, float Exploration, const SSettings &Set) const
	{
		// Progressive widening: until every action was tried once, the search sweeps them in the
		// order they were generated. That is what turns `quality` into "how much of the candidate
		// space was actually looked at": below the number of candidates the agent is fast and dumb.
		for(int i = 0; i < m_NumRoots; i++)
		{
			if(m_aRoots[i].m_Available && m_aRoots[i].m_Visits == 0)
				return i;
		}

		int Best = -1;
		float BestUct = 0.0f;
		const float LogN = std::log((float)TotalVisits + 1.0f);
		for(int i = 0; i < m_NumRoots; i++)
		{
			const SRoot &Root = m_aRoots[i];
			if(!Root.m_Available)
				continue;
			const float Exploit = MeanScore(Root, Set);
			const float Explore = Exploration * std::sqrt(LogN / (float)Root.m_Visits);
			const float Uct = Exploit + Explore;
			if(Best < 0 || Uct > BestUct)
			{
				Best = i;
				BestUct = Uct;
			}
		}
		return Best;
	}

	int CPlanner::FindBest(const SSettings &Set, int PlayerSafe) const
	{
		int Best = -1;
		float BestScore = 0.0f;
		for(int i = 0; i < m_NumRoots; i++)
		{
			const SRoot &Root = m_aRoots[i];
			// Only actions whose own (fixed sequence) continuation carries the tee at least as far
			// as the player's input does are allowed to win: the search may never make things
			// worse, and an exploratory rescue may never be sold as the action's own survival.
			if(!Root.m_Available || Root.m_Visits == 0 || Root.m_CanonicalSafe < PlayerSafe)
				continue;

			const float Score = RankingScore(Root, Set);
			if(Best < 0 || Score > BestScore + 0.0001f ||
				(std::fabs(Score - BestScore) <= 0.0001f &&
					(Root.m_CanonicalSafe > m_aRoots[Best].m_CanonicalSafe ||
						(Root.m_CanonicalSafe == m_aRoots[Best].m_CanonicalSafe && Root.m_Deviation < m_aRoots[Best].m_Deviation))))
			{
				Best = i;
				BestScore = Score;
			}
		}
		return Best;
	}

	void CPlanner::DescribePlan(SInputPlan &Plan, const SContext &Ctx, bool SafeEnough, const SRoot &Best) const
	{
		const int PlayerDirection = std::clamp(Ctx.m_Input.m_Direction, -1, 1);
		const int PlayerJump = Ctx.m_Input.m_Jump ? 1 : 0;
		const int PlayerHook = Ctx.m_Input.m_Hook ? 1 : 0;

		const bool HookChanged = Best.m_Hook != PlayerHook;
		const bool JumpChanged = Best.m_Jump != PlayerJump;
		const bool DirectionChanged = Best.m_Direction != PlayerDirection;

		const char *pDirection = "";
		if(DirectionChanged)
		{
			if(Best.m_Direction == 0)
				pDirection = ", brake";
			else if(Best.m_Direction < 0)
				pDirection = ", steer left";
			else
				pDirection = ", steer right";
		}

		// The key is assembled in English and only then translated, so every combination the agent
		// can actually produce has exactly one localised phrase.
		char aKey[96];
		if(HookChanged && Best.m_Hook == 0 && JumpChanged)
			str_copy(aKey, "release hook and jump before hazard");
		else if(HookChanged)
			str_format(aKey, sizeof(aKey), "%s%s before hazard", Best.m_Hook == 0 ? "release hook" : "press hook", pDirection);
		else if(JumpChanged)
			str_format(aKey, sizeof(aKey), "jump%s before hazard", pDirection);
		else
			str_format(aKey, sizeof(aKey), "%s before hazard", Best.m_Direction == 0 ? "brake" : (Best.m_Direction < 0 ? "steer left" : "steer right"));

		if(SafeEnough)
			str_copy(Plan.m_aReason, BcLocalize(aKey));
		else
			str_format(Plan.m_aReason, sizeof(Plan.m_aReason), "NSIF: %s", BcLocalize(aKey));
	}

	SInputPlan CPlanner::Plan(const SContext &Ctx, const SEnvironment &Env)
	{
		SInputPlan Plan;
		Plan.m_Input = Ctx.m_Input;

		const int64_t StartTime = time_get();
		auto Finish = [&](const char *pReason) {
			str_copy(Plan.m_aReason, pReason);
			Plan.m_CostMs = ElapsedMs(StartTime);
			return Plan;
		};

		CCollision *pCollision = Env.m_pCollision;
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return Finish(BcLocalize("no map data"));

		const SSettings &Set = Ctx.m_Settings;

		// The two "hands off" movement states the engine can see on its own. The third one
		// (hammer fly / deep fly) needs client state and is filtered by the caller, which merges
		// both into one gate before any agent runs.
		const int Movement = ClassifyMovement(Ctx.m_Core, false);
		if(Movement & MOVE_JETPACK)
			return Finish(BcLocalize("jetpack, hands off"));
		if(Movement & MOVE_HOOKED_PLAYER)
			return Finish(BcLocalize("hooked to a player, hands off"));

		// Lookahead. Unfreeze tiles are not lethal, so they get their own, usually much shorter
		// window (`bc_avoid_unfreeze_ticks`): the agent stops being paranoid around them.
		int Horizon = std::clamp(Set.m_CheckTicks, 2, MAX_SIM_TICKS);
		if(Set.m_TileUnfreeze && Ctx.m_Threat.m_HasNearest &&
			(ClassifyPoint(pCollision, Ctx.m_Threat.m_NearestPos) & HAZ_UNFREEZE))
		{
			Horizon = std::clamp(Set.m_UnfreezeTicks, 2, MAX_SIM_TICKS);
		}
		Plan.m_ScannedTicks = Horizon;

		// --- simulation state of this decision ------------------------------------------------
		m_Sim.Init(Ctx, Env);
		m_InitCore = m_Sim.m_Core;
		for(int i = 0; i < m_Sim.m_NumShadows; i++)
			m_aInitShadows[i] = m_Sim.m_aShadows[i];

		BuildRoots(Ctx);
		if(m_NumRoots <= 0)
			return Finish(BcLocalize("nothing evaluated"));

		uint64_t aSeed[2] = {(uint64_t)(uint32_t)Ctx.m_Tick + 0x9E3779B97F4A7C15ull, 0xBF58476D1CE4E5B9ull};
		m_Prng.Seed(aSeed);

		const int KickIn = std::clamp(Set.m_KickInTicks, 0, MAX_SIM_TICKS);
		const int Iterations = std::clamp(Set.m_Quality, 1, MAX_ITERATIONS);
		const float Exploration = Set.m_Randomness / 100.0f;

		// --- iteration 0: the player's own input ----------------------------------------------
		// It is the baseline every other action has to beat, and it doubles as the stage 2 fast
		// path: a safe input is never touched, and an input that still survives `kick_in_ticks`
		// is not worth a search yet.
		m_aRoots[0].m_Visits = 1;
		m_aRoots[0].m_CanonicalSafe = Rollout(0, true, Horizon, Set, pCollision);
		m_aRoots[0].m_BestSafe = m_aRoots[0].m_CanonicalSafe;
		const int PlayerSafe = m_aRoots[0].m_CanonicalSafe;
		m_aRoots[0].m_SafeSum = PlayerSafe;
		Plan.m_Candidates = 1;
		Plan.m_SafeTicks = PlayerSafe;
		Plan.m_Score = RankingScore(m_aRoots[0], Set);

		if(PlayerSafe >= Horizon)
			return Finish(BcLocalize("player input safe"));
		// `kick_in_ticks = 0` means "do not wait at all" (the agent intervenes as early as its
		// sensing reach allows). Without the guard the comparison would always be true and the
		// slider's lowest setting would silently mean "off", which is the opposite of what the
		// label says.
		if(KickIn > 0 && PlayerSafe >= KickIn)
			return Finish(BcLocalize("still time before the hazard"));

		// Which actions are actually allowed now: the jump is a last resort and waits for the
		// critical moment, and it never fires while the rope is the thing to solve.
		UpdateAvailability(Ctx, PlayerSafe);
		const bool JumpHeldBack = std::any_of(m_aRoots, m_aRoots + m_NumRoots,
			[](const SRoot &Root) { return !Root.m_Available; });

		// --- the search ------------------------------------------------------------------------
		int TotalVisits = 1;
		for(int Iteration = 1; Iteration < Iterations; Iteration++)
		{
			// `quality` may ask for 200 iterations, the tick budget stays 1.5 ms: the search stops
			// as soon as it has spent its share and reports what it found so far.
			if(ElapsedMs(StartTime) >= SEARCH_BUDGET_MS)
				break;

			const int RootIndex = SelectRoot(TotalVisits, Exploration, Set);
			if(RootIndex < 0)
				break;

			SRoot &Root = m_aRoots[RootIndex];
			const bool Canonical = Root.m_Visits == 0;
			const int Safe = Rollout(RootIndex, Canonical, Horizon, Set, pCollision);
			if(Canonical)
				Root.m_CanonicalSafe = Safe;
			Root.m_BestSafe = std::max(Root.m_BestSafe, Safe);
			Root.m_SafeSum += Safe;
			Root.m_Visits++;
			TotalVisits++;
			Plan.m_Candidates++;
		}

		// --- decision --------------------------------------------------------------------------
		const int BestIndex = FindBest(Set, PlayerSafe);
		if(BestIndex <= 0 || m_aRoots[BestIndex].m_CanonicalSafe <= PlayerSafe)
		{
			// Say why the agent is visibly doing nothing while the tee is walking into trouble: the
			// only action left would have been a jump, and the jump is held back on purpose.
			if(JumpHeldBack)
				return Finish(BcLocalize("no safer plan (jump is a last resort)"));
			return Finish(BcLocalize("no safer plan"));
		}

		const SRoot &Best = m_aRoots[BestIndex];
		const bool SafeEnough = Best.m_CanonicalSafe >= Horizon;
		if(!SafeEnough && !Set.m_Nsif)
			return Finish(BcLocalize("no safer plan"));

		Plan.m_Override = true;
		Plan.m_UsedFallback = !SafeEnough;
		Plan.m_Input = Best.m_Input;
		Plan.m_SafeTicks = Best.m_CanonicalSafe;
		Plan.m_Score = MeanScore(Best, Set);
		DescribePlan(Plan, Ctx, SafeEnough, Best);
		return Finish(Plan.m_aReason);
	}
} // namespace Avoid
