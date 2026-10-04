/* Copyright © 2026 BestProject Team */
#include "avoid_engine.h"

#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/config.h>

#include <game/collision.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <game/client/components/bestclient/fast_practice.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <algorithm>
#include <cmath>

namespace Avoid
{
	namespace
	{
		// Velocity/flow dot product weight of the reference fitness function (0x14054a6b8).
		constexpr float FENT_FLOW_WEIGHT = 1750.0f;

		// A tee counts as crossing a light freeze tile only while it still moves; below this speed
		// it is parked in the freeze and the tile has to count as a hazard again (px per tick).
		constexpr float LIGHT_FREEZE_MIN_SPEED = 1.5f;

		// Hard safety net for the Legit search. The reference simply drops frames when the MCTS
		// budget is set absurdly high; a shippable client must not stall, so the search yields and
		// reports the best plan it has. With the reference defaults this never triggers.
		constexpr double LEGIT_DEADLINE_MS = 8.0;

		// Simulated character ticks a planner may spend per rendered tick before yielding.
		constexpr int PLANNER_STEPS_PER_TICK = 600;

		// Tiles the navigator may classify or flood per tick while building its grid.
		constexpr int NAV_WORK_PER_TICK = 20000;

		// How long a finished Fentbot search round waits before starting the next one.
		constexpr int SEARCH_COOLDOWN_TICKS = 10;

		// Lookahead used by the closed loop safety net of the two planners.
		constexpr int PLAN_GUARD_TICKS = 6;

		bool IsDeathTile(CCollision *pCollision, int Index)
		{
			return pCollision->GetTileIndex(Index) == TILE_DEATH ||
			       pCollision->GetFrontTileIndex(Index) == TILE_DEATH ||
			       pCollision->GetSwitchType(Index) == TILE_DEATH;
		}

		bool IsFreezeTile(CCollision *pCollision, int Index)
		{
			const int aTiles[] = {pCollision->GetTileIndex(Index), pCollision->GetFrontTileIndex(Index), pCollision->GetSwitchType(Index)};
			for(const int Tile : aTiles)
			{
				if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE)
					return true;
			}
			return false;
		}

		bool IsDeepFreezeTile(CCollision *pCollision, int Index)
		{
			const int aTiles[] = {pCollision->GetTileIndex(Index), pCollision->GetFrontTileIndex(Index), pCollision->GetSwitchType(Index)};
			for(const int Tile : aTiles)
			{
				if(Tile == TILE_DFREEZE)
					return true;
			}
			return false;
		}

		bool IsUnfreezeTile(CCollision *pCollision, int Index)
		{
			return pCollision->GetTileIndex(Index) == TILE_UNFREEZE || pCollision->GetFrontTileIndex(Index) == TILE_UNFREEZE;
		}

		bool IsFinishTile(CCollision *pCollision, int Index)
		{
			return pCollision->GetTileIndex(Index) == TILE_FINISH || pCollision->GetFrontTileIndex(Index) == TILE_FINISH;
		}

		bool IsTeleportTile(CCollision *pCollision, int Index)
		{
			return pCollision->IsTeleport(Index) || pCollision->IsEvilTeleport(Index) ||
			       pCollision->IsCheckTeleport(Index) || pCollision->IsCheckEvilTeleport(Index) ||
			       pCollision->IsTeleCheckpoint(Index);
		}

		// Reference spec 4.1, steps 3 and 4. Shared by the blocking and the resumable simulator.
		bool TickHitHazard(CCharacter *pChar, CCollision *pCollision, const SSimFlags &Flags, int Tick)
		{
			if(!pChar || !pCollision)
				return true;

			const CCharacterCore *pCore = pChar->Core();
			const vec2 Pos = pCore->m_Pos;

			// Death comes first: a tile can be freeze on one layer and death on another, and the
			// light-tile exemption below must never hide that. Both layers are probed because both
			// of them kill, see CCharacter::HandleTiles.
			if(Flags.m_AvoidDeath)
			{
				const float Rad = pChar->GetProximityRadius() / 3.0f;
				const float aProbeX[] = {Pos.x, Pos.x + Rad, Pos.x - Rad, Pos.x + Rad, Pos.x - Rad};
				const float aProbeY[] = {Pos.y, Pos.y + Rad, Pos.y + Rad, Pos.y - Rad, Pos.y - Rad};
				for(int i = 0; i < 5; ++i)
				{
					if((pCollision->GetCollisionAt(aProbeX[i], aProbeY[i]) & TILE_DEATH) ||
						(pCollision->GetFrontCollisionAt(aProbeX[i], aProbeY[i]) & TILE_DEATH))
					{
						return true;
					}
				}
			}

			// Freeze: normal freeze, deep freeze and live freeze all count as a failure, with one
			// exception: the Fentbot may deliberately cross a freeze tile that has an unfreeze tile
			// nearby, which is exactly what its light-tile rule puts on the grid. The exception is
			// bound to a tee that is still moving through the tile, because a tee that came to rest
			// on it is frozen rather than crossing, and the search must not rank "park in the
			// freeze" as a perfect plan.
			if(Flags.m_AvoidFreeze &&
				(pChar->m_FreezeTime > 0 || pChar->m_FrozenLastTick ||
					pCore->m_IsInFreeze || pCore->m_DeepFrozen || pCore->m_LiveFrozen))
			{
				const bool Crossable = Flags.m_AllowLightFreeze && Flags.m_pNav &&
						       !pCore->m_DeepFrozen && !pCore->m_LiveFrozen &&
						       length(pCore->m_Vel) > LIGHT_FREEZE_MIN_SPEED &&
						       Flags.m_pNav->IsLightTile(Pos);
				if(!Crossable)
					return true;
			}

			if(Flags.m_AvoidTeles)
			{
				const int Index = pCollision->GetPureMapIndex(Pos);
				if(Index >= 0 && IsTeleportTile(pCollision, Index))
					return true;
			}

			if(Flags.m_AvoidUnfreeze && Tick < Flags.m_UnfreezeTicks)
			{
				const int Index = pCollision->GetPureMapIndex(Pos);
				if(Index >= 0 && IsUnfreezeTile(pCollision, Index))
					return true;
			}

			return false;
		}

		const CNetObj_PlayerInput &InputAt(const CNetObj_PlayerInput *pInputs, int NumInputs, int Tick)
		{
			if(NumInputs <= 0)
				return *pInputs;
			return pInputs[std::min(Tick, NumInputs - 1)];
		}

		// One cloned world plus the per tick bookkeeping of the forward simulator.
		class CForwardSim
		{
		public:
			CGameWorld m_World;
			CCharacter *m_pChar = nullptr;
			CCollision *m_pCollision = nullptr;
			int m_LocalClientId = -1;

			bool Begin(CGameClient *pClient, CGameWorld *pBaseWorld)
			{
				if(!pClient || !pBaseWorld)
					return false;
				// 1. Clone the world (reference: CopyWorld).
				m_World.CopyWorldClean(pBaseWorld);
				m_World.m_WorldConfig.m_PredictEvents = false;

				m_LocalClientId = pClient->m_Snap.m_LocalClientId;
				m_pChar = m_World.GetCharacterById(m_LocalClientId);
				if(!m_pChar)
					return false;
				m_pCollision = m_World.Collision();
				return m_pCollision != nullptr;
			}

			// The only thing that makes another tee solid for us is its entry in the core world.
			// Clearing those entries is what "do not predict the other players" means: they are no
			// longer in the way, so no plan has to route around them.
			void SetPredictPlayers(bool PredictPlayers)
			{
				if(PredictPlayers)
					return;
				for(int i = 0; i < MAX_CLIENTS; ++i)
				{
					if(i != m_LocalClientId)
						m_World.m_Core.m_apCharacters[i] = nullptr;
				}
			}

			// Injects one input and advances one 50 Hz physics frame.
			void Step(const CNetObj_PlayerInput &Input)
			{
				m_pChar->OnDirectInput(&Input);
				m_pChar->OnPredictedInput(&Input);
				m_World.Tick();
				m_pChar = m_World.GetCharacterById(m_LocalClientId);
			}
		};

		// Shared body of SimulatePlan: walks the plan over the lookahead window.
		int RunPlan(
			CForwardSim &Sim,
			const CNetObj_PlayerInput *pInputs,
			int NumInputs,
			int CheckTicks,
			const SSimFlags &Flags,
			const SFlowField *pFlow,
			float *pOutFlowScore,
			std::vector<vec2> *pvPath,
			vec2 *pOutEndPos)
		{
			float FlowScore = 0.0f;
			if(pvPath)
			{
				pvPath->clear();
				pvPath->push_back(Sim.m_pChar->Core()->m_Pos);
			}

			int Survived = 0;
			for(; Survived < CheckTicks; ++Survived)
			{
				Sim.Step(InputAt(pInputs, NumInputs, Survived));
				if(!Sim.m_pChar)
					break;

				if(pFlow && pFlow->m_pDir && pFlow->m_Width > 0 && pFlow->m_Height > 0)
				{
					const vec2 Vel = Sim.m_pChar->Core()->m_Vel;
					const int Index = Sim.m_pCollision->GetPureMapIndex(Sim.m_pChar->Core()->m_Pos);
					if(Index >= 0 && Index < pFlow->m_Width * pFlow->m_Height)
					{
						const vec2 Dir = pFlow->m_pDir[Index];
						FlowScore += (Vel.x * Dir.x + Vel.y * Dir.y) * pFlow->m_Scale;
					}
				}

				if(pvPath)
					pvPath->push_back(Sim.m_pChar->Core()->m_Pos);

				if(TickHitHazard(Sim.m_pChar, Sim.m_pCollision, Flags, Survived))
					break;
			}

			if(pOutFlowScore)
				*pOutFlowScore = FlowScore;
			if(pOutEndPos)
				*pOutEndPos = Sim.m_pChar ? Sim.m_pChar->Core()->m_Pos : vec2(0.0f, 0.0f);

			return Survived >= CheckTicks ? SIMULATION_SAFE_CONSTANT : Survived;
		}
	} // namespace

	void ResolveFentPreset(SSettings &Set)
	{
		// Reference 0x1403356ba: the preset overrides actions / dosage / hold ticks while the
		// advanced switch is off. Every preset plans 10000 ticks ahead.
		switch(std::clamp(Set.m_FentQuality, 0, 2))
		{
		case 1: // Mid
			Set.m_FentActions = 160;
			Set.m_FentDosage = 160;
			Set.m_FentHoldTicks = 8;
			Set.m_FentHorizon = 10000;
			break;
		case 2: // Max
			Set.m_FentActions = 1000;
			Set.m_FentDosage = 300;
			Set.m_FentHoldTicks = 8;
			Set.m_FentHorizon = 10000;
			break;
		case 0: // Low
		default:
			Set.m_FentActions = 88;
			Set.m_FentDosage = 88;
			Set.m_FentHoldTicks = 8;
			Set.m_FentHorizon = 10000;
			break;
		}

		if(Set.m_FentAdvanced)
		{
			Set.m_FentActions = std::clamp(Set.m_FentTweakerActions, 50, 5000);
			Set.m_FentDosage = std::clamp(Set.m_FentTweakerDosage, 1, 500);
			Set.m_FentHoldTicks = std::clamp(Set.m_FentTweakerTicks, 1, 30);
			Set.m_FentHorizon = std::clamp(Set.m_FentTicks, 1000, 10000);
		}
	}

	// -----------------------------------------------------------------------------------------
	// Shared helpers
	// -----------------------------------------------------------------------------------------

	vec2 AimDirection(int TargetX, int TargetY)
	{
		return normalize(vec2((float)TargetX, (float)TargetY));
	}

	bool AimTargetsFrom(vec2 Dir, int *pTargetX, int *pTargetY)
	{
		if(length(Dir) <= 0.0001f)
			return false;
		const vec2 Unit = normalize(Dir);
		if(pTargetX)
			*pTargetX = (int)std::lround(Unit.x * 256.0f);
		if(pTargetY)
			*pTargetY = (int)std::lround(Unit.y * 256.0f);
		if(pTargetX && pTargetY && *pTargetX == 0 && *pTargetY == 0)
			*pTargetY = -1;
		return true;
	}

	bool IsHookable(CCollision *pCollision, vec2 From, vec2 Dir, float HookLength, vec2 *pOutPos)
	{
		if(!pCollision || length(Dir) <= 0.0001f || HookLength <= 0.0f)
			return false;
		const vec2 Unit = normalize(Dir);
		const vec2 To = From + Unit * HookLength;
		vec2 Hit;
		const int Tile = pCollision->IntersectLineTeleHook(From, To, &Hit, nullptr);
		if(Tile != 0 && Tile != TILE_NOHOOK && Tile != TILE_TELEINHOOK)
		{
			if(pOutPos)
				*pOutPos = Hit;
			return true;
		}
		return false;
	}

	// -----------------------------------------------------------------------------------------
	// Forward simulator (reference spec 4.1)
	// -----------------------------------------------------------------------------------------

	int SimulateCandidate(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput &CandidateInput,
		int CheckTicks,
		const SSimFlags &Flags,
		std::vector<vec2> *pvPath)
	{
		return SimulatePlan(pClient, pBaseWorld, &CandidateInput, 1, CheckTicks, Flags, nullptr, nullptr, pvPath, nullptr);
	}

	int SimulatePlan(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput *pInputs,
		int NumInputs,
		int CheckTicks,
		const SSimFlags &Flags,
		const SFlowField *pFlow,
		float *pOutFlowScore,
		std::vector<vec2> *pvPath,
		vec2 *pOutEndPos)
	{
		if(!pInputs || NumInputs <= 0 || CheckTicks <= 0)
			return SIMULATION_SAFE_CONSTANT;

		CForwardSim Sim;
		if(!Sim.Begin(pClient, pBaseWorld))
			return SIMULATION_SAFE_CONSTANT;
		Sim.SetPredictPlayers(Flags.m_PredictPlayers);

		return RunPlan(Sim, pInputs, NumInputs, CheckTicks, Flags, pFlow, pOutFlowScore, pvPath, pOutEndPos);
	}

	bool CSimSession::Begin(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput *pInputs,
		int NumInputs,
		int CheckTicks,
		const SSimFlags &Flags,
		const SFlowField *pFlow)
	{
		Abort();

		if(!pClient || !pBaseWorld || !pInputs || NumInputs <= 0 || CheckTicks <= 0)
			return false;

		m_pWorld = new CGameWorld();
		m_pWorld->CopyWorldClean(pBaseWorld);
		m_pWorld->m_WorldConfig.m_PredictEvents = false;

		m_LocalClientId = pClient->m_Snap.m_LocalClientId;
		m_pChar = m_pWorld->GetCharacterById(m_LocalClientId);
		m_pCollision = m_pWorld->Collision();
		if(!m_pChar || !m_pCollision)
		{
			Abort();
			return false;
		}

		if(!Flags.m_PredictPlayers)
		{
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(i != m_LocalClientId)
					m_pWorld->m_Core.m_apCharacters[i] = nullptr;
			}
		}

		m_pInputs = pInputs;
		m_NumInputs = NumInputs;
		m_Horizon = CheckTicks;
		m_Flags = Flags;
		m_pFlow = pFlow;
		m_Tick = 0;
		m_Finished = false;
		m_Outcome = SOutcome{};
		m_Outcome.m_EndPos = m_pChar->Core()->m_Pos;
		return true;
	}

	int CSimSession::Step(int MaxSteps)
	{
		if(m_Finished || !m_pWorld || !m_pChar)
			return 0;

		int Taken = 0;
		while(Taken < MaxSteps && m_Tick < m_Horizon)
		{
			const CNetObj_PlayerInput &Input = InputAt(m_pInputs, m_NumInputs, m_Tick);
			m_pChar->OnDirectInput(&Input);
			m_pChar->OnPredictedInput(&Input);
			m_pWorld->Tick();
			Taken++;

			m_pChar = m_pWorld->GetCharacterById(m_LocalClientId);
			if(!m_pChar)
			{
				m_Outcome.m_SurvivalTicks = m_Tick;
				m_Finished = true;
				return Taken;
			}

			if(m_pFlow && m_pFlow->m_pDir && m_pFlow->m_Width > 0 && m_pFlow->m_Height > 0)
			{
				const vec2 Vel = m_pChar->Core()->m_Vel;
				const int Index = m_pCollision->GetPureMapIndex(m_pChar->Core()->m_Pos);
				if(Index >= 0 && Index < m_pFlow->m_Width * m_pFlow->m_Height)
				{
					const vec2 Dir = m_pFlow->m_pDir[Index];
					m_Outcome.m_FlowScore += (Vel.x * Dir.x + Vel.y * Dir.y) * m_pFlow->m_Scale;
				}
			}

			m_Outcome.m_EndPos = m_pChar->Core()->m_Pos;

			if(HitHazard())
			{
				m_Outcome.m_SurvivalTicks = m_Tick;
				m_Finished = true;
				return Taken;
			}

			m_Tick++;
		}

		if(m_Tick >= m_Horizon)
		{
			m_Outcome.m_SurvivalTicks = SIMULATION_SAFE_CONSTANT;
			m_Finished = true;
		}
		return Taken;
	}

	bool CSimSession::HitHazard() const
	{
		return TickHitHazard(m_pChar, m_pCollision, m_Flags, m_Tick);
	}

	void CSimSession::Abort()
	{
		delete m_pWorld;
		m_pWorld = nullptr;
		m_pChar = nullptr;
		m_pCollision = nullptr;
		m_pInputs = nullptr;
		m_NumInputs = 0;
		m_Horizon = 0;
		m_Tick = 0;
		m_Finished = true;
		m_pFlow = nullptr;
		m_Outcome = SOutcome{};
	}

	// -----------------------------------------------------------------------------------------
	// Navigable grid + flow field
	// -----------------------------------------------------------------------------------------

	void CNavigator::Reset()
	{
		m_vGrid.clear();
		m_vLightSeen.clear();
		m_vDist.clear();
		m_vFlow.clear();
		m_vQueue.clear();
		m_LightQueue.clear();
		m_Width = 0;
		m_Height = 0;
		m_QueueHead = 0;
		m_QueueTail = 0;
		m_GoalTiles = 0;
		m_Cursor = 0;
		m_Phase = PHASE_IDLE;
		m_Ready = false;
		m_Building = false;
	}

	void CNavigator::Rebuild(CCollision *pCollision, bool LightTile, int LightRadius)
	{
		Reset();
		m_LightTile = LightTile;
		m_LightRadius = std::clamp(LightRadius, 0, 20);
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return;

		m_Width = pCollision->GetWidth();
		m_Height = pCollision->GetHeight();
		const size_t Total = (size_t)m_Width * (size_t)m_Height;
		m_vGrid.assign(Total, NAV_BLOCKED);
		m_vLightSeen.assign(Total, 0);
		m_vDist.assign(Total, -1);
		m_vFlow.assign(Total, vec2(0.0f, 0.0f));
		m_Building = true;
		m_Ready = false;
		m_Phase = PHASE_CLASSIFY;
	}

	void CNavigator::ClassifyTiles(CCollision *pCollision)
	{
		const int Total = m_Width * m_Height;
		const int End = std::min(Total, m_Cursor + NAV_WORK_PER_TICK);
		for(; m_Cursor < End; ++m_Cursor)
		{
			const int X = m_Cursor % m_Width;
			const int Y = m_Cursor / m_Width;
			unsigned char Cell = NAV_BLOCKED;

			if(!pCollision->IsSolid(X, Y) && !IsDeathTile(pCollision, m_Cursor))
			{
				if(IsFinishTile(pCollision, m_Cursor))
				{
					Cell = NAV_GOAL;
					m_GoalTiles++;
				}
				else if(IsDeepFreezeTile(pCollision, m_Cursor))
				{
					Cell = NAV_BLOCKED; // deep freeze can never be crossed
				}
				else if(IsFreezeTile(pCollision, m_Cursor))
				{
					// Freeze always starts blocked. The light-tile pass below re-opens exactly the
					// ones that have an unfreeze tile within the configured radius.
					Cell = NAV_BLOCKED;
				}
				else
				{
					Cell = NAV_OPEN;
					// Unfreeze tiles are the seeds of the light-tile walk, so they are collected in
					// the same pass over the map.
					if(m_LightTile && IsUnfreezeTile(pCollision, m_Cursor))
						m_LightQueue.emplace_back(m_Cursor, 0);
				}
			}
			m_vGrid[m_Cursor] = Cell;
		}

		if(m_Cursor >= Total)
			m_Phase = PHASE_MARK_LIGHT;
	}

	bool CNavigator::StepMarkLight(CCollision *pCollision)
	{
		// The queue was seeded with every unfreeze tile while the map was classified. The walk
		// spreads through walkable tiles and turns freeze tiles into NAV_LIGHT, but only inside the
		// configured radius; freeze tiles no unfreeze tile is near enough to stay blocked.
		const int Radius = m_LightTile ? m_LightRadius : 0;
		if(Radius <= 0)
		{
			m_LightQueue.clear();
			return true;
		}

		int Work = NAV_WORK_PER_TICK;
		while(Work-- > 0 && !m_LightQueue.empty())
		{
			const auto Entry = m_LightQueue.front();
			m_LightQueue.pop_front();
			const int Index = Entry.first;
			const int Dist = Entry.second;
			if(Dist >= Radius)
				continue;

			const int X = Index % m_Width;
			const int Y = Index / m_Width;
			for(int i = 0; i < 4; ++i)
			{
				const int Nx = X + (i == 0 ? -1 : (i == 1 ? 1 : 0));
				const int Ny = Y + (i == 2 ? -1 : (i == 3 ? 1 : 0));
				if(Nx < 0 || Nx >= m_Width || Ny < 0 || Ny >= m_Height)
					continue;
				const int Next = Nx + Ny * m_Width;
				if(m_vLightSeen[Next])
					continue;
				if(m_vGrid[Next] == NAV_BLOCKED)
				{
					// Solid rock, deep freeze and death never become crossable; a plain freeze tile
					// does, which is the whole point of the light-tile rule.
					if(!IsFreezeTile(pCollision, Next) || IsDeepFreezeTile(pCollision, Next) ||
						IsDeathTile(pCollision, Next))
						continue;
					m_vGrid[Next] = NAV_LIGHT;
				}
				m_vLightSeen[Next] = 1;
				m_LightQueue.emplace_back(Next, Dist + 1);
			}
		}

		return m_LightQueue.empty();
	}

	bool CNavigator::StepFlood()
	{
		if(m_QueueHead >= m_QueueTail)
			return true;

		int Work = NAV_WORK_PER_TICK;
		while(Work-- > 0 && m_QueueHead < m_QueueTail)
		{
			const int Index = m_vQueue[(size_t)m_QueueHead++];
			const int X = Index % m_Width;
			const int Y = Index / m_Width;
			const int Dist = m_vDist[Index];
			for(int i = 0; i < 4; ++i)
			{
				const int Nx = X + (i == 0 ? -1 : (i == 1 ? 1 : 0));
				const int Ny = Y + (i == 2 ? -1 : (i == 3 ? 1 : 0));
				if(Nx < 0 || Nx >= m_Width || Ny < 0 || Ny >= m_Height)
					continue;
				const int Next = Nx + Ny * m_Width;
				if(m_vGrid[Next] == NAV_BLOCKED || m_vDist[Next] >= 0)
					continue;
				m_vDist[Next] = Dist + 1;
				m_vQueue.push_back(Next);
				m_QueueTail++;
			}
		}
		return m_QueueHead >= m_QueueTail;
	}

	bool CNavigator::BuildGradient()
	{
		const int Total = m_Width * m_Height;
		const int End = std::min(Total, m_Cursor + NAV_WORK_PER_TICK);
		for(; m_Cursor < End; ++m_Cursor)
		{
			if(m_vDist[m_Cursor] < 0)
				continue;
			const int X = m_Cursor % m_Width;
			const int Y = m_Cursor / m_Width;
			if(X <= 0 || Y <= 0 || X >= m_Width - 1 || Y >= m_Height - 1)
				continue;

			const int Self = m_vDist[m_Cursor];
			auto Neighbour = [&](int Index) { return m_vDist[Index] < 0 ? Self + 8 : m_vDist[Index]; };
			const float GradX = (float)(Neighbour(m_Cursor - 1) - Neighbour(m_Cursor + 1));
			const float GradY = (float)(Neighbour(m_Cursor - m_Width) - Neighbour(m_Cursor + m_Width));
			const vec2 Dir(GradX, GradY);
			if(length(Dir) > 0.0001f)
				m_vFlow[m_Cursor] = normalize(Dir);
		}
		return m_Cursor >= Total;
	}

	bool CNavigator::Update(CCollision *pCollision, int MaxWork)
	{
		if(m_Ready)
			return true;
		if(!m_Building || !pCollision || MaxWork <= 0)
			return false;

		switch(m_Phase)
		{
		case PHASE_CLASSIFY:
			ClassifyTiles(pCollision);
			break;
		case PHASE_MARK_LIGHT:
			if(StepMarkLight(pCollision))
			{
				// Goals: finish tiles when the map has them, unfreeze tiles otherwise.
				std::vector<int> vGoals;
				for(size_t i = 0; i < m_vGrid.size(); ++i)
				{
					if(m_vGrid[i] == NAV_GOAL)
						vGoals.push_back((int)i);
				}
				if(vGoals.empty())
				{
					for(size_t i = 0; i < m_vGrid.size(); ++i)
					{
						if(IsUnfreezeTile(pCollision, (int)i))
						{
							m_vGrid[i] = NAV_GOAL;
							vGoals.push_back((int)i);
						}
					}
					m_GoalTiles = (int)vGoals.size();
				}

				m_vQueue = vGoals;
				m_QueueHead = 0;
				m_QueueTail = (int)vGoals.size();
				for(const int Goal : vGoals)
					m_vDist[Goal] = 0;
				m_Cursor = 0;
				m_Phase = PHASE_FLOOD;
			}
			break;
		case PHASE_FLOOD:
			if(StepFlood())
			{
				m_Cursor = 0;
				m_Phase = PHASE_GRADIENT;
			}
			break;
		case PHASE_GRADIENT:
			if(BuildGradient())
			{
				m_Phase = PHASE_IDLE;
				m_Building = false;
				m_Ready = true;
			}
			break;
		case PHASE_IDLE:
		default:
			m_Building = false;
			m_Ready = true;
			break;
		}

		return m_Ready;
	}

	vec2 CNavigator::FlowAt(vec2 Pos) const
	{
		if(!m_Ready || m_Width <= 0 || m_Height <= 0)
			return vec2(0.0f, 0.0f);
		const int X = std::clamp((int)(Pos.x / 32.0f), 0, m_Width - 1);
		const int Y = std::clamp((int)(Pos.y / 32.0f), 0, m_Height - 1);
		return m_vFlow[(size_t)X + (size_t)Y * (size_t)m_Width];
	}

	bool CNavigator::IsLightTile(vec2 Pos) const
	{
		if(!m_Ready || m_Width <= 0 || m_Height <= 0)
			return false;
		const int X = std::clamp((int)(Pos.x / 32.0f), 0, m_Width - 1);
		const int Y = std::clamp((int)(Pos.y / 32.0f), 0, m_Height - 1);
		return m_vGrid[(size_t)X + (size_t)Y * (size_t)m_Width] == NAV_LIGHT;
	}

	int CNavigator::DistanceAt(vec2 Pos) const
	{
		if(!m_Ready || m_Width <= 0 || m_Height <= 0)
			return -1;
		const int X = std::clamp((int)(Pos.x / 32.0f), 0, m_Width - 1);
		const int Y = std::clamp((int)(Pos.y / 32.0f), 0, m_Height - 1);
		return m_vDist[(size_t)X + (size_t)Y * (size_t)m_Width];
	}

	// -----------------------------------------------------------------------------------------
	// BLAgent base
	// -----------------------------------------------------------------------------------------

	CGameWorld *GetActiveWorld(CGameClient *pClient)
	{
		if(!pClient)
			return nullptr;
		if(pClient->m_FastPractice.Active())
			return &pClient->m_FastPractice.PracticeWorld();
		if(pClient->m_PredictedWorld.GetCharacterById(pClient->m_Snap.m_LocalClientId))
			return &pClient->m_PredictedWorld;
		if(pClient->m_GameWorld.GetCharacterById(pClient->m_Snap.m_LocalClientId))
			return &pClient->m_GameWorld;
		return &pClient->m_PredictedWorld;
	}

	// -----------------------------------------------------------------------------------------
	// 1. Basic agent (reference spec 5)
	// -----------------------------------------------------------------------------------------

	AvoidInput CBasicAgent::GetAction(const SContext &Ctx, CGameWorld *pWorld)
	{
		AvoidInput Out;
		Out.m_Input = Ctx.m_Input;

		if(!pWorld)
		{
			str_copy(Out.m_aReason, "no world");
			return Out;
		}

		SSimFlags Flags;
		Flags.m_PredictPlayers = Ctx.m_Settings.m_PlayerPrediction;
		Flags.m_AvoidFreeze = true;
		Flags.m_AvoidDeath = true; // reference passes avoid-death, avoid-teles is false

		// 1. Test the player input over the fixed 6 tick window.
		const int Baseline = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, BASIC_CHECK_TICKS, Flags,
			Ctx.m_Settings.m_DrawPath ? &Out.m_vPath : nullptr);
		Out.m_SurvivalTicks = Baseline == SIMULATION_SAFE_CONSTANT ? BASIC_CHECK_TICKS : Baseline;
		if(Baseline == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Out.m_aReason, "player input safe");
			return Out;
		}

		// 2. Enumerate {0, -1, 1} in exactly that order, keep the first fully safe candidate.
		static const int s_aCandidateDirs[3] = {0, -1, 1};
		int BestScore = Baseline;
		int BestDir = Ctx.m_Input.m_Direction;
		for(const int Dir : s_aCandidateDirs)
		{
			CNetObj_PlayerInput Candidate = Ctx.m_Input;
			Candidate.m_Direction = Dir;
			const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, BASIC_CHECK_TICKS, Flags);
			if(Score > BestScore)
			{
				BestScore = Score;
				BestDir = Dir;
				if(Score == SIMULATION_SAFE_CONSTANT)
					break;
			}
		}

		if(BestScore > Baseline)
		{
			Out.m_Input.m_Direction = BestDir;
			Out.m_Active = 1;
			Out.m_SurvivalTicks = BestScore == SIMULATION_SAFE_CONSTANT ? BASIC_CHECK_TICKS : BestScore;
			if(BestDir == 0)
				str_copy(Out.m_aReason, "brake before hazard");
			else if(BestDir < 0)
				str_copy(Out.m_aReason, "steer left before hazard");
			else
				str_copy(Out.m_aReason, "steer right before hazard");
			if(Ctx.m_Settings.m_DrawPath)
				SimulateCandidate(m_pClient, pWorld, Out.m_Input, BASIC_CHECK_TICKS, Flags, &Out.m_vPath);
		}
		else
		{
			str_copy(Out.m_aReason, "no safer plan");
		}

		return Out;
	}

	// -----------------------------------------------------------------------------------------
	// 2. Blatant agent (reference spec 6)
	// -----------------------------------------------------------------------------------------

	void CBlatantAgent::OnReset()
	{
		m_TrackPointValid = false;
		m_TrackPointPos = vec2(0.0f, 0.0f);
		m_SavedSafeSequence.clear();
	}

	AvoidInput CBlatantAgent::GetAction(const SContext &Ctx, CGameWorld *pWorld)
	{
		const SSettings &Set = Ctx.m_Settings;
		AvoidInput Out;
		Out.m_Input = Ctx.m_Input;

		if(!pWorld)
		{
			str_copy(Out.m_aReason, "no world");
			return Out;
		}

		SSimFlags Flags;
		Flags.m_PredictPlayers = Set.m_PlayerPrediction;
		Flags.m_AvoidFreeze = true;
		Flags.m_AvoidDeath = Set.m_BlatantDeath;
		Flags.m_AvoidTeles = Set.m_BlatantTeles;
		Flags.m_AvoidUnfreeze = Set.m_BlatantUnfreeze;
		Flags.m_UnfreezeTicks = Set.m_BlatantUnfreezeTicks;

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Out.m_aReason, "no character");
			return Out;
		}

		const vec2 Pos = pLocal->Core()->m_Pos;
		const float HookLength = pLocal->Core()->m_Tuning.m_HookLength;
		const vec2 PlayerAim = AimDirection(Ctx.m_Input.m_TargetX, Ctx.m_Input.m_TargetY);

		// Track point: remember the last aim direction of the player that was hookable onto a tile.
		if(Set.m_TrackPoint)
		{
			vec2 HitPos;
			if(IsHookable(pWorld->Collision(), Pos, PlayerAim, HookLength, &HitPos))
			{
				m_TrackPointValid = true;
				m_TrackPointPos = HitPos;
			}
		}
		else
		{
			m_TrackPointValid = false;
		}
		if(m_TrackPointValid)
		{
			Out.m_TrackPoint.m_Valid = true;
			Out.m_TrackPoint.m_Pos = m_TrackPointPos;
		}

		// 1. Kick in ticks: stay out of the way while the player input is safe for that long.
		const int KickSafety = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, Set.m_KickInTicks, Flags,
			Set.m_DrawPath ? &Out.m_vPath : nullptr);
		if(KickSafety == SIMULATION_SAFE_CONSTANT)
		{
			m_SavedSafeSequence.clear();
			Out.m_SurvivalTicks = Set.m_KickInTicks;
			str_copy(Out.m_aReason, "player input safe");
			return Out;
		}

		const int CheckTicks = Set.m_BlatantCheckTicks;

		// 2. Candidate actions: direction x hook, in the reference order.
		static const int s_aDirs[3] = {0, -1, 1};
		static const int s_aHooks[2] = {0, 1};
		const int DirCount = Set.m_BlatantDirection ? 3 : 1;
		const int HookCount = Set.m_BlatantHook ? 2 : 1;

		std::vector<CNetObj_PlayerInput> vActions;
		vActions.reserve((size_t)DirCount * (size_t)HookCount);
		for(int d = 0; d < DirCount; ++d)
		{
			for(int h = 0; h < HookCount; ++h)
			{
				CNetObj_PlayerInput Act = Ctx.m_Input;
				if(Set.m_BlatantDirection)
					Act.m_Direction = s_aDirs[d];
				if(Set.m_BlatantHook)
					Act.m_Hook = s_aHooks[h];
				vActions.push_back(Act);
			}
		}

		int BestSurvival = -1;
		CNetObj_PlayerInput BestAction = Ctx.m_Input;
		bool FoundSafe = false;
		bool BestFromAimbot = false;

		for(const CNetObj_PlayerInput &Action : vActions)
		{
			const int Score = SimulateCandidate(m_pClient, pWorld, Action, CheckTicks, Flags);
			const int Survival = Score == SIMULATION_SAFE_CONSTANT ? CheckTicks : Score;
			if(Survival > BestSurvival)
			{
				BestSurvival = Survival;
				BestAction = Action;
				BestFromAimbot = false;
			}
			if(Score == SIMULATION_SAFE_CONSTANT)
				FoundSafe = true;
		}

		// 3. Internal aim layer: only active while the internal aimbot is switched on. Everything
		// below produces *candidate* aim directions; the survival search still decides which of
		// them is actually used, so an aim switch can never make the bot less safe.
		if(Set.m_Aimbot)
		{
			std::vector<vec2> vAims;

			// The track point is a candidate either way; safe aim tracking only adds the rule that
			// it must have been verified as safe first.
			if(Set.m_TrackPoint && m_TrackPointValid)
			{
				const vec2 Dir = m_TrackPointPos - Pos;
				if(length(Dir) > 0.0001f)
				{
					const vec2 DirToTrack = normalize(Dir);
					bool Usable = true;
					if(Set.m_SafeAimTracking)
					{
						CNetObj_PlayerInput Test = Ctx.m_Input;
						AimTargetsFrom(DirToTrack, &Test.m_TargetX, &Test.m_TargetY);
						Test.m_Hook = 1;
						// Both sides are measured over the same window, otherwise a tracked aim
						// that dies late would pass a comparison against a much shorter baseline.
						const int Res = SimulateCandidate(m_pClient, pWorld, Test, CheckTicks, Flags);
						const int PlayerCheck = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, CheckTicks, Flags);
						Usable = Res == SIMULATION_SAFE_CONSTANT || Res > PlayerCheck;
					}
					if(Usable)
						vAims.push_back(DirToTrack);
				}
			}

			// Auto drag always aims at the closest tee; the survival search decides whether hooking
			// it is actually a good idea.
			if(Set.m_AutoDrag && Set.m_PlayerPrediction)
			{
				int BestTee = -1;
				float BestDist = HookLength;
				for(int i = 0; i < MAX_CLIENTS; ++i)
				{
					if(i == m_pClient->m_Snap.m_LocalClientId)
						continue;
					CCharacter *pOther = pWorld->GetCharacterById(i);
					if(!pOther)
						continue;
					const float Dist = distance(Pos, pOther->Core()->m_Pos);
					if(Dist <= BestDist)
					{
						BestDist = Dist;
						BestTee = i;
					}
				}
				if(BestTee >= 0)
					vAims.push_back(normalize(pWorld->GetCharacterById(BestTee)->Core()->m_Pos - Pos));
			}

			// Track point and auto drag carry their own aim, the field of view scan below adds two
			// more picks. Everything ends up in the same candidate list.
			const int FixedAims = (int)vAims.size();

			// Field of view scan. The reference offers two ways to pick from it, and they really are
			// different choices: auto aim takes the direction that survives longest, aim assist
			// takes the *safe* direction closest to the player's own crosshair. With both switches
			// on, both picks become candidates.
			const int Segments = std::max(1, Set.m_AimbotSegments);
			const float FovRad = (float)Set.m_AimbotFov * (pi / 180.0f);
			const float BaseAngle = std::atan2(PlayerAim.y, PlayerAim.x);

			int BestSurvivalAim = -1;
			int BestSurvivalScore = -1;
			int BestNearAim = -1;
			float BestNearAngle = 1e9f;

			for(int Segment = 0; Segment < Segments; ++Segment)
			{
				const float Offset = Segments == 1 ? 0.0f : (-FovRad * 0.5f + FovRad * ((float)Segment / (float)(Segments - 1)));
				const float Angle = BaseAngle + Offset;
				const vec2 ScanDir(std::cos(Angle), std::sin(Angle));
				vec2 HitPos;
				if(!IsHookable(pWorld->Collision(), Pos, ScanDir, HookLength, &HitPos))
					continue;

				CNetObj_PlayerInput Probe = Ctx.m_Input;
				Probe.m_Hook = 1;
				AimTargetsFrom(ScanDir, &Probe.m_TargetX, &Probe.m_TargetY);
				const int ProbeScore = SimulateCandidate(m_pClient, pWorld, Probe, CheckTicks, Flags);
				const int ProbeSurvival = ProbeScore == SIMULATION_SAFE_CONSTANT ? CheckTicks : ProbeScore;

				if(ProbeSurvival > BestSurvivalScore)
				{
					BestSurvivalScore = ProbeSurvival;
					BestSurvivalAim = (int)vAims.size();
				}
				if(ProbeScore == SIMULATION_SAFE_CONSTANT)
				{
					float AngleDiff = std::abs(Offset);
					if(AngleDiff > pi)
						AngleDiff = 2.0f * pi - AngleDiff;
					if(AngleDiff < BestNearAngle)
					{
						BestNearAngle = AngleDiff;
						BestNearAim = (int)vAims.size();
					}
				}
				vAims.push_back(ScanDir);
			}

			std::vector<vec2> vSelected;
			auto AddAim = [&](int Index) {
				if(Index < 0 || Index >= (int)vAims.size())
					return;
				const vec2 &Dir = vAims[(size_t)Index];
				for(const vec2 &Existing : vSelected)
				{
					if(dot(Existing, Dir) > 0.9999f)
						return; // already selected, do not pay for the same aim twice
				}
				vSelected.push_back(Dir);
			};

			// The tracked aim point and the auto drag target are chosen by their own rule, so they
			// are candidates whenever their switch put them on the list.
			for(int i = 0; i < FixedAims; ++i)
				AddAim(i);
			if(Set.m_AutoAim)
				AddAim(BestSurvivalAim);
			if(Set.m_AimAssist)
				AddAim(BestNearAim);

			for(const vec2 &Aim : vSelected)
			{
				for(const CNetObj_PlayerInput &Action : vActions)
				{
					CNetObj_PlayerInput Candidate = Action;
					AimTargetsFrom(Aim, &Candidate.m_TargetX, &Candidate.m_TargetY);
					const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks, Flags);
					const int Survival = Score == SIMULATION_SAFE_CONSTANT ? CheckTicks : Score;
					if(Survival > BestSurvival)
					{
						BestSurvival = Survival;
						BestAction = Candidate;
						BestFromAimbot = true;
					}
					if(Score == SIMULATION_SAFE_CONSTANT)
						FoundSafe = true;
				}
			}
		}

		// 4. NSIF (reference spec 6.4): nothing is fully safe, so spend the first step of the last
		// known safe sequence. The reference *consumes* that step, which is what makes NSIF a
		// one-shot hope rather than an endless replay of the same stale input.
		bool UsedFallback = false;
		if(FoundSafe)
		{
			m_SavedSafeSequence.clear();
			m_SavedSafeSequence.push_back(BestAction);
		}
		else if(Set.m_Nsif && !m_SavedSafeSequence.empty())
		{
			BestAction = m_SavedSafeSequence.front();
			m_SavedSafeSequence.erase(m_SavedSafeSequence.begin());
			// The saved step was safe when it was stored, not necessarily now; reporting what it
			// actually does keeps the HUD honest.
			const int Score = SimulateCandidate(m_pClient, pWorld, BestAction, CheckTicks, Flags);
			BestSurvival = Score == SIMULATION_SAFE_CONSTANT ? CheckTicks : Score;
			UsedFallback = true;
		}

		// Reference spec 6.4 step 4 only applies a plan when the search produced one, i.e. when
		// some candidate survived at least one tick. If everything dies immediately the player
		// keeps their own input, exactly like the reference leaves m_Sequence empty.
		if(BestSurvival > 0 || UsedFallback)
		{
			Out.m_Input = BestAction;
			Out.m_Active = 1;
			Out.m_SurvivalTicks = BestSurvival;
			Out.m_UsedFallback = UsedFallback;
			if(BestFromAimbot)
			{
				Out.m_AimTarget.m_Valid = true;
				const vec2 Dir = AimDirection(BestAction.m_TargetX, BestAction.m_TargetY);
				Out.m_AimTarget.m_Pos = Pos + Dir * 128.0f;
			}
			if(UsedFallback)
				str_copy(Out.m_aReason, "NSIF: replay saved safe input");
			else if(BestAction.m_TargetX != Ctx.m_Input.m_TargetX || BestAction.m_TargetY != Ctx.m_Input.m_TargetY)
				str_copy(Out.m_aReason, BestAction.m_Hook ? "hook the safe aim" : "aim clear of the hazard");
			else if(BestAction.m_Hook != Ctx.m_Input.m_Hook)
				str_copy(Out.m_aReason, BestAction.m_Hook ? "hook to safety" : "release hook");
			else
				str_copy(Out.m_aReason, BestAction.m_Direction == 0 ? "brake before hazard" : "steer away from hazard");

			if(Set.m_DrawPath && !UsedFallback)
				SimulateCandidate(m_pClient, pWorld, Out.m_Input, CheckTicks, Flags, &Out.m_vPath);
		}
		else
		{
			Out.m_SurvivalTicks = KickSafety == SIMULATION_SAFE_CONSTANT ? Set.m_KickInTicks : KickSafety;
			str_copy(Out.m_aReason, "no safer plan");
		}

		return Out;
	}

	// -----------------------------------------------------------------------------------------
	// 3. Legit agent (reference spec 7)
	// -----------------------------------------------------------------------------------------

	AvoidInput CLegitAgent::GetAction(const SContext &Ctx, CGameWorld *pWorld)
	{
		const SSettings &Set = Ctx.m_Settings;
		AvoidInput Out;
		Out.m_Input = Ctx.m_Input;

		if(!pWorld)
		{
			str_copy(Out.m_aReason, "no world");
			return Out;
		}

		SSimFlags Flags;
		Flags.m_PredictPlayers = Set.m_PlayerPrediction;
		Flags.m_AvoidFreeze = true;
		Flags.m_AvoidDeath = Set.m_LegitDeath;
		Flags.m_AvoidTeles = Set.m_LegitTeles;
		Flags.m_AvoidUnfreeze = Set.m_LegitUnfreeze;
		Flags.m_UnfreezeTicks = Set.m_LegitUnfreezeTicks;

		const int CheckTicks = Set.m_LegitCheckTicks;
		const int Iterations = std::max(1, Set.m_LegitIterations);
		const double ExplorationC = (double)Set.m_LegitExploration;
		const double WeightDir = (double)Set.m_LegitDirectionWeight;
		const double WeightHook = (double)Set.m_LegitHookWeight;
		const double WeightLife = (double)Set.m_LegitLifespanWeight;
		constexpr float WEIGHT_SCALE = 0.01f;

		// Reference MCTS node (0x58 bytes in the binary).
		struct MCTSNode
		{
			MCTSNode *m_pParent = nullptr;
			std::vector<MCTSNode *> m_vChildren;
			CNetObj_PlayerInput m_Action{};
			int m_Visits = 0;
			double m_TotalValue = 0.0;
			int m_LifespanTicks = 0;
			bool m_IsTerminal = false;

			~MCTSNode()
			{
				for(MCTSNode *pChild : m_vChildren)
					delete pChild;
			}
		};

		// Reference spec 7.2: asymmetric multi objective UCT heuristic.
		auto Heuristic = [&](const CNetObj_PlayerInput &Action, int SurvivalTicks) {
			const float DirDiff = std::abs((float)Action.m_Direction - (float)Ctx.m_Input.m_Direction);
			const float DirScore = std::abs(DirDiff - 2.0f) * ((float)WeightDir * WEIGHT_SCALE);
			const float HookDiff = std::abs((float)Action.m_Hook - (float)Ctx.m_Input.m_Hook);
			const float HookScore = std::abs(HookDiff - 1.0f) * ((float)WeightHook * WEIGHT_SCALE);
			const float LifeScore = (float)SurvivalTicks * ((float)WeightLife * WEIGHT_SCALE);
			return (double)(DirScore + HookScore + LifeScore);
		};

		MCTSNode *pRoot = new MCTSNode();
		pRoot->m_Action = Ctx.m_Input;

		const int64_t Deadline = time_get() + (int64_t)(LEGIT_DEADLINE_MS * (double)time_freq() / 1000.0);
		bool DeadlineHit = false;

		for(int Iter = 0; Iter < Iterations; ++Iter)
		{
			if((Iter & 7) == 0 && time_get() >= Deadline)
			{
				DeadlineHit = true;
				break;
			}

			// 1. Selection
			MCTSNode *pCurr = pRoot;
			while(!pCurr->m_vChildren.empty())
			{
				MCTSNode *pBestChild = nullptr;
				double BestScore = -1e38;
				for(MCTSNode *pChild : pCurr->m_vChildren)
				{
					double Score;
					if(pChild->m_Visits == 0)
					{
						Score = 3.402823466e+38; // FLT_MAX, so every child is tried at least once
					}
					else
					{
						const double Exploitation = pChild->m_TotalValue / (double)pChild->m_Visits;
						const double Exploration = ExplorationC * std::sqrt(std::log((double)std::max(1, pCurr->m_Visits)) / (double)pChild->m_Visits);
						Score = Exploitation + Exploration + Heuristic(pChild->m_Action, pChild->m_LifespanTicks);
					}
					if(Score > BestScore)
					{
						BestScore = Score;
						pBestChild = pChild;
					}
				}
				if(!pBestChild)
					break;
				pCurr = pBestChild;
			}

			// 2. Expansion
			if(!pCurr->m_IsTerminal && pCurr->m_Visits > 0)
			{
				static const int s_aDirs[3] = {-1, 0, 1};
				static const int s_aHooks[2] = {0, 1};
				const int DirCount = Set.m_LegitDirection ? 3 : 1;
				const int HookCount = Set.m_LegitHook ? 2 : 1;
				for(int d = 0; d < DirCount; ++d)
				{
					for(int h = 0; h < HookCount; ++h)
					{
						MCTSNode *pNewChild = new MCTSNode();
						pNewChild->m_pParent = pCurr;
						pNewChild->m_Action = pCurr->m_Action;
						if(Set.m_LegitDirection)
							pNewChild->m_Action.m_Direction = s_aDirs[d];
						if(Set.m_LegitHook)
							pNewChild->m_Action.m_Hook = s_aHooks[h];
						pCurr->m_vChildren.push_back(pNewChild);
					}
				}
				if(!pCurr->m_vChildren.empty())
					pCurr = pCurr->m_vChildren[(size_t)(rand() % (int)pCurr->m_vChildren.size())];
			}

			// 3. Rollout
			const int Survival = SimulateCandidate(m_pClient, pWorld, pCurr->m_Action, CheckTicks, Flags);
			pCurr->m_LifespanTicks = Survival == SIMULATION_SAFE_CONSTANT ? CheckTicks : Survival;
			const double Reward = Survival == SIMULATION_SAFE_CONSTANT ? 1.0 : ((double)Survival / (double)CheckTicks);
			if(Survival < CheckTicks)
				pCurr->m_IsTerminal = true;

			// 4. Backpropagation
			for(MCTSNode *pNode = pCurr; pNode; pNode = pNode->m_pParent)
			{
				pNode->m_Visits++;
				pNode->m_TotalValue += Reward;
			}
		}

		// The most visited root child wins.
		MCTSNode *pBest = nullptr;
		int MostVisits = -1;
		for(MCTSNode *pChild : pRoot->m_vChildren)
		{
			if(pChild->m_Visits > MostVisits)
			{
				MostVisits = pChild->m_Visits;
				pBest = pChild;
			}
		}

		// Reference spec 7.3: the plan is applied whenever it differs from what the player pressed.
		if(pBest && (pBest->m_Action.m_Direction != Ctx.m_Input.m_Direction ||
			      pBest->m_Action.m_Hook != Ctx.m_Input.m_Hook))
		{
			Out.m_Input = pBest->m_Action;
			Out.m_Active = 1;
			Out.m_SurvivalTicks = pBest->m_LifespanTicks;
			if(pBest->m_Action.m_Hook != Ctx.m_Input.m_Hook && pBest->m_Action.m_Direction != Ctx.m_Input.m_Direction)
				str_copy(Out.m_aReason, "hook and steer to safety");
			else if(pBest->m_Action.m_Hook != Ctx.m_Input.m_Hook)
				str_copy(Out.m_aReason, pBest->m_Action.m_Hook ? "hook to safety" : "release hook");
			else if(pBest->m_Action.m_Direction == 0)
				str_copy(Out.m_aReason, "brake before hazard");
			else
				str_copy(Out.m_aReason, "steer to safety");
			if(Set.m_DrawPath)
				SimulateCandidate(m_pClient, pWorld, Out.m_Input, CheckTicks, Flags, &Out.m_vPath);
		}
		else
		{
			Out.m_SurvivalTicks = CheckTicks;
			str_copy(Out.m_aReason, DeadlineHit ? "search budget reached" : "player input safe");
		}

		delete pRoot;
		return Out;
	}

	// -----------------------------------------------------------------------------------------
	// 4. Fentbot agent (reference spec 8)
	// -----------------------------------------------------------------------------------------

	CFentbotAgent::~CFentbotAgent()
	{
		m_Session.Abort();
		delete m_pSnapshot;
	}

	void CFentbotAgent::OnReset()
	{
		m_Session.Abort();
		delete m_pSnapshot;
		m_pSnapshot = nullptr;
		m_SnapshotValid = false;
		m_vCandidates.clear();
		m_vFitness.clear();
		m_CandidateCount = 0;
		m_CandidateLength = 0;
		m_vPlan.clear();
		m_vPendingPlan.clear();
		m_PlanPending = false;
		m_PlanIndex = 0;
		m_BestFitness = -1e30f;
		m_CandidateIndex = 0;
		m_Generation = 0;
		m_Cooldown = 0;
		m_SessionFlow = SFlowField{};
		m_Nav.Reset();
	}

	void CFentbotAgent::SeedGeneration(const SSettings &Set, const SContext &Ctx)
	{
		const int Count = std::clamp(Set.m_FentActions, 1, 5000);
		const int Length = std::clamp(Set.m_FentHoldTicks, 1, 30);

		m_vCandidates.assign((size_t)Count * (size_t)Length, CNetObj_PlayerInput{});
		m_vFitness.assign((size_t)Count, -1e30f);
		m_CandidateCount = Count;
		m_CandidateLength = Length;
		// Every genome of this round is simulated from the same snapshot, so their fitness values
		// are comparable and the incumbent may carry over from one generation to the next.
		m_BestFitness = -1e30f;

		// The navigator's flow field is the seed: half the population walks down the gradient, the
		// rest is randomised around it so the search can also find what the heuristic misses.
		const vec2 StartFlow = m_Nav.FlowAt(m_pClient->m_LocalCharacterPos);
		const bool HasFlow = m_Nav.Ready();

		for(int i = 0; i < Count; ++i)
		{
			CNetObj_PlayerInput Base = Ctx.m_Input;
			Base.m_Jump = 0;
			Base.m_Hook = 0;
			if(HasFlow && (i % 2) == 0)
			{
				Base.m_Direction = StartFlow.x > 0.25f ? 1 : (StartFlow.x < -0.25f ? -1 : 0);
				if(StartFlow.y < -0.25f)
					Base.m_Jump = 1;
			}
			else if((i % 4) == 1)
			{
				Base.m_Direction = (i % 3) - 1;
			}

			for(int t = 0; t < Length; ++t)
			{
				CNetObj_PlayerInput Gene = Base;
				if(i % 2 == 1 || t > 0)
				{
					const int R = rand();
					if((R & 3) == 0)
						Gene.m_Direction = (R >> 4) % 3 - 1;
					if((R & 12) == 0)
						Gene.m_Jump ^= 1;
					if((R & 48) == 0)
						Gene.m_Hook ^= 1;
				}
				m_vCandidates[(size_t)i * (size_t)Length + (size_t)t] = Gene;
			}
		}
		m_CandidateIndex = 0;
		m_Generation = 0;
	}

	void CFentbotAgent::Breed(const SSettings &Set)
	{
		// The shape of the running generation is authoritative; a settings change only takes effect
		// when a new round seeds its population.
		const int Count = m_CandidateCount;
		const int Length = m_CandidateLength;
		if(Count <= 0 || Length <= 0)
			return;
		const int Keep = std::clamp(Count / 4, 1, Count);

		std::vector<int> vOrder((size_t)Count);
		for(int i = 0; i < Count; ++i)
			vOrder[i] = i;
		std::stable_sort(vOrder.begin(), vOrder.end(), [&](int a, int b) { return m_vFitness[a] > m_vFitness[b]; });

		// Elites survive unchanged, the rest is bred from the two best parents plus a mutation.
		std::vector<CNetObj_PlayerInput> vNext((size_t)Count * (size_t)Length, CNetObj_PlayerInput{});
		for(int i = 0; i < Count; ++i)
		{
			const int ParentA = vOrder[i % Keep];
			const int ParentB = vOrder[(i * 7 + 3) % Keep];
			for(int t = 0; t < Length; ++t)
			{
				CNetObj_PlayerInput Gene;
				if(i < Keep)
				{
					Gene = m_vCandidates[(size_t)ParentA * (size_t)Length + (size_t)t];
				}
				else
				{
					const int Src = (rand() & 1) ? ParentA : ParentB;
					Gene = m_vCandidates[(size_t)Src * (size_t)Length + (size_t)t];
					const int R = rand();
					if((R % 5) == 0)
						Gene.m_Direction = (R >> 4) % 3 - 1;
					if((R % 11) == 0)
						Gene.m_Jump ^= 1;
					if((R % 13) == 0)
						Gene.m_Hook ^= 1;
				}
				vNext[(size_t)i * (size_t)Length + (size_t)t] = Gene;
			}
		}

		m_vCandidates.swap(vNext);
		m_vFitness.assign((size_t)Count, -1e30f);
		m_CandidateIndex = 0;
		m_Generation++;
		(void)Set;
	}

	AvoidInput CFentbotAgent::GetAction(const SContext &Ctx, CGameWorld *pWorld)
	{
		const SSettings &Set = Ctx.m_Settings;
		AvoidInput Out;
		Out.m_Input = Ctx.m_Input;

		if(!pWorld)
		{
			str_copy(Out.m_aReason, "no world");
			return Out;
		}

		CCollision *pCollision = pWorld->Collision();
		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pCollision || !pLocal)
		{
			str_copy(Out.m_aReason, "no character");
			return Out;
		}

		const vec2 Pos = pLocal->Core()->m_Pos;

		SSimFlags Flags;
		Flags.m_PredictPlayers = Set.m_PlayerPrediction;
		Flags.m_AvoidFreeze = true;
		Flags.m_AvoidDeath = true;
		Flags.m_AvoidTeles = false;
		Flags.m_AvoidUnfreeze = false;
		// With light tiles on, the navigator decided which freeze tiles are crossable; the
		// simulator has to agree with it, otherwise no plan could ever use them.
		Flags.m_AllowLightFreeze = Set.m_FentLightTile;
		Flags.m_pNav = &m_Nav;

		// --- 1. Navigable grid and flow field (budgeted, survives across ticks). -------------
		const bool MapChanged = m_Nav.Width() != pCollision->GetWidth() || m_Nav.Height() != pCollision->GetHeight();
		const bool LightChanged = m_Nav.LightTile() != Set.m_FentLightTile ||
					  m_Nav.LightRadius() != Set.m_FentLightTileRadius;
		if(MapChanged || LightChanged || (!m_Nav.Ready() && !m_Nav.Building()))
		{
			m_Session.Abort(); // the flow field the session points into is about to be replaced
			m_Nav.Rebuild(pCollision, Set.m_FentLightTile, Set.m_FentLightTileRadius);
			m_SnapshotValid = false;
		}
		if(!m_Nav.Ready())
			m_Nav.Update(pCollision, NAV_WORK_PER_TICK);

		const int Length = std::clamp(Set.m_FentHoldTicks, 1, 30);
		const int Horizon = std::clamp(Set.m_FentHorizon, 1, 10000);

		// --- 2. Keep a plan available at all times. ------------------------------------------
		auto FallbackAction = [&]() {
			// Nothing solved yet: stay alive with a one step greedy search guided by the flow.
			AvoidInput Fallback;
			Fallback.m_Input = Ctx.m_Input;
			// Keep the stopgap cheap: it runs on every tick until the first plan exists, so it
			// only looks a handful of ticks ahead instead of the full search horizon.
			const int Guard = std::clamp(Length, PLAN_GUARD_TICKS, 10);
			int Best = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, Guard, Flags);
			const int aDirs[3] = {0, -1, 1};
			const vec2 Flow = m_Nav.FlowAt(Pos);
			// A permutation of the three direction slots with the one the flow field asks for
			// first, so the most likely candidate is simulated first and each slot is tried
			// exactly once. aDirs[0] is neutral, aDirs[1] is left and aDirs[2] is right.
			int aOrder[3] = {1, 2, 0}; // no usable flow: left, right, neutral
			if(Flow.x > 0.25f)
			{
				aOrder[0] = 2; // right first
				aOrder[1] = 0; // then neutral
				aOrder[2] = 1; // then left
			}
			else if(Flow.x < -0.25f)
			{
				aOrder[0] = 1; // left first
				aOrder[1] = 0; // then neutral
				aOrder[2] = 2; // then right
			}
			for(const int Slot : aOrder)
			{
				for(int Jump = 0; Jump < 2; ++Jump)
				{
					for(int Hook = 0; Hook < 2; ++Hook)
					{
						CNetObj_PlayerInput Candidate = Ctx.m_Input;
						Candidate.m_Direction = aDirs[Slot];
						Candidate.m_Jump = Jump;
						Candidate.m_Hook = Hook;
						if(Hook)
							AimTargetsFrom(Flow, &Candidate.m_TargetX, &Candidate.m_TargetY);
						const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, Guard, Flags);
						if(Score > Best)
						{
							Best = Score;
							Fallback.m_Input = Candidate;
							Fallback.m_Active = 1;
						}
					}
				}
			}
			Fallback.m_SurvivalTicks = Best == SIMULATION_SAFE_CONSTANT ? Guard : Best;
			str_copy(Fallback.m_aReason, Fallback.m_Active ? "survival fallback while searching" : "player input safe");
			if(Set.m_DrawPath)
				SimulateCandidate(m_pClient, pWorld, Fallback.m_Input, Guard, Flags, &Fallback.m_vPath);
			return Fallback;
		};

		if(m_Cooldown > 0)
			m_Cooldown--;

		// --- 3. Advance the background search by one budgeted slice. -------------------------
		if(m_Cooldown == 0)
		{
			if(!m_SnapshotValid)
			{
				m_Session.Abort();
				if(!m_pSnapshot)
					m_pSnapshot = new CGameWorld();
				m_pSnapshot->CopyWorldClean(pWorld);
				m_pSnapshot->m_WorldConfig.m_PredictEvents = false;
				m_SnapshotValid = true;
				SeedGeneration(Set, Ctx);
			}

			// The flow field lives in the navigator, so the session only survives as long as the
			// navigator is not rebuilt. The rebuild path above aborts the session first.
			const std::vector<vec2> &vFlow = m_Nav.Flow();
			m_SessionFlow.m_pDir = vFlow.empty() ? nullptr : vFlow.data();
			m_SessionFlow.m_Width = m_Nav.Width();
			m_SessionFlow.m_Height = m_Nav.Height();
			// Velocity is px/tick, so the reference weight is scaled down to keep the fitness in a
			// sane numeric range, exactly like the reference does with its own float constants.
			m_SessionFlow.m_Scale = FENT_FLOW_WEIGHT / 50.0f;

			const int Count = m_CandidateCount;
			const int GenomeLength = m_CandidateLength;
			int StepsLeft = PLANNER_STEPS_PER_TICK;
			while(StepsLeft > 0 && Count > 0 && GenomeLength > 0 && m_CandidateIndex < Count)
			{
				if(m_Session.Finished())
				{
					const CNetObj_PlayerInput *pGenome = &m_vCandidates[(size_t)m_CandidateIndex * (size_t)GenomeLength];
					if(!m_Session.Begin(m_pClient, m_pSnapshot, pGenome, GenomeLength, Horizon, Flags,
						   m_SessionFlow.m_pDir ? &m_SessionFlow : nullptr))
					{
						m_vFitness[m_CandidateIndex] = -1e30f;
						m_CandidateIndex++;
						continue;
					}
				}

				const int Taken = m_Session.Step(StepsLeft);
				StepsLeft -= Taken;
				if(Taken <= 0)
					break;

				if(m_Session.Finished())
				{
					const CSimSession::SOutcome &Result = m_Session.Outcome();
					const int Survival = Result.m_SurvivalTicks == SIMULATION_SAFE_CONSTANT ? Horizon : Result.m_SurvivalTicks;
					float Fitness = (float)Survival + Result.m_FlowScore;
					const int Dist = m_Nav.Ready() ? m_Nav.DistanceAt(Result.m_EndPos) : -1;
					if(Dist >= 0)
						Fitness -= (float)Dist * 2.0f;
					m_vFitness[m_CandidateIndex] = Fitness;

					// Anytime publication: the round keeps improving the plan, and the executor picks
					// every improvement up once the genome it is driving has run out. The genome is
					// staged, never written into the plan that is currently being driven.
					if(Fitness > m_BestFitness)
					{
						m_BestFitness = Fitness;
						const CNetObj_PlayerInput *pBest = &m_vCandidates[(size_t)m_CandidateIndex * (size_t)GenomeLength];
						m_vPendingPlan.assign(pBest, pBest + GenomeLength);
						m_PlanPending = true;
					}
					m_CandidateIndex++;
				}
			}

			if(Count > 0 && GenomeLength > 0 && m_CandidateIndex >= Count)
			{
				// Generation over: breed the next one, or end the round and let the search rest.
				if(m_Generation + 1 < std::max(1, Set.m_FentDosage))
					Breed(Set);
				else
				{
					m_SnapshotValid = false;
					m_Cooldown = SEARCH_COOLDOWN_TICKS;
				}
			}
		}

		// --- 4. Execute the plan, with a closed loop guard. ----------------------------------
		if(m_vPlan.empty())
			return FallbackAction();

		// A staged genome is swapped in as a whole, and only at its own first tick. Driving the
		// tail of the old plan and then restarting the new one from its head would replay the same
		// jump or hook over and over; while nothing newer exists the last input is held instead.
		const int LastStep = (int)m_vPlan.size() - 1;
		if(m_PlanPending && m_PlanIndex >= LastStep)
		{
			m_vPlan.swap(m_vPendingPlan);
			m_vPendingPlan.clear();
			m_PlanPending = false;
			m_PlanIndex = 0;
		}

		Out.m_Input = m_vPlan[(size_t)std::clamp(m_PlanIndex, 0, (int)m_vPlan.size() - 1)];

		// Guard: a stale plan must never make things worse than the player's own input.
		const int PlanSafety = SimulateCandidate(m_pClient, pWorld, Out.m_Input, PLAN_GUARD_TICKS, Flags);
		if(PlanSafety != SIMULATION_SAFE_CONSTANT)
		{
			const int PlayerSafety = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, PLAN_GUARD_TICKS, Flags);
			if(PlanSafety <= PlayerSafety)
			{
				// The plan went stale (the world moved on). Throw it away and replan from scratch;
				// the search is nudged to start a new round on the very next tick.
				m_vPlan.clear();
				m_vPendingPlan.clear();
				m_PlanPending = false;
				m_PlanIndex = 0;
				m_SnapshotValid = false;
				m_Cooldown = 0;
				m_CandidateCount = 0;
				m_CandidateLength = 0;
				m_Session.Abort();
				return FallbackAction();
			}
		}

		if(m_PlanIndex + 1 < (int)m_vPlan.size())
			m_PlanIndex++;
		Out.m_Active = 1;
		Out.m_SurvivalTicks = PlanSafety == SIMULATION_SAFE_CONSTANT ? PLAN_GUARD_TICKS : PlanSafety;
		str_copy(Out.m_aReason, "fentbot plan");

		if(Set.m_DrawPath)
		{
			Out.m_vPath.clear();
			vec2 Walk = Pos;
			Out.m_vPath.push_back(Walk);
			for(int i = 0; i < 40; ++i)
			{
				const vec2 Flow = m_Nav.FlowAt(Walk);
				if(length(Flow) < 0.001f)
					break;
				Walk += Flow * 24.0f;
				Out.m_vPath.push_back(Walk);
			}
		}

		return Out;
	}

	// -----------------------------------------------------------------------------------------
	// 5. Pilot agent (reference spec 9)
	// -----------------------------------------------------------------------------------------

	CPilotAgent::~CPilotAgent()
	{
		m_Session.Abort();
		delete m_pSnapshot;
	}

	int CPilotAgent::NextRand()
	{
		// Small deterministic LCG so that one decision is reproducible and testable.
		m_Rng = m_Rng * 1103515245 + 12345;
		return (int)((unsigned)m_Rng >> 16) & 0x7fff;
	}

	void CPilotAgent::OnReset()
	{
		m_Session.Abort();
		delete m_pSnapshot;
		m_pSnapshot = nullptr;
		m_SnapshotValid = false;
		m_vPopulation.clear();
		m_vFitness.clear();
		m_vNext.clear();
		m_IndividualCount = 0;
		m_IndividualDepth = 0;
		m_vPlan.clear();
		m_PlanIndex = 0;
		m_PlanTick = 0;
		m_PlanEpoch = 0;
		m_HeldEpoch = 0;
		m_Individual = 0;
		m_Rng = 0x1f123bb5;
		m_TargetValid = false;
		m_SessionFlow = SFlowField{};
		m_Nav.Reset();
	}

	void CPilotAgent::SeedPopulation(const SSettings &Set, const SContext &Ctx)
	{
		const int Count = std::clamp(Set.m_PilotPopulation, 1, 8192);
		const int Depth = std::clamp(Set.m_PilotDepth, 1, 50);

		// A fresh population is only built when there is none yet or when the shape changed. It is
		// deliberately not rebuilt between generations: the offspring that Breed() produced is the
		// whole point of the search, and re-seeding here would throw it away and re-evaluate the
		// parents instead. A new world snapshot is handled separately, by resetting the fitness.
		m_vPopulation.assign((size_t)Count * (size_t)Depth, CNetObj_PlayerInput{});
		m_vFitness.assign((size_t)Count, -1e30f);
		m_IndividualCount = Count;
		m_IndividualDepth = Depth;

		const vec2 Flow = m_Nav.Ready() ? m_Nav.FlowAt(m_pClient->m_LocalCharacterPos) : vec2(0.0f, 0.0f);
		vec2 ToTarget(0.0f, 0.0f);
		if(m_TargetValid)
			ToTarget = m_Target - m_pClient->m_LocalCharacterPos;

		for(int i = 0; i < Count; ++i)
		{
			CNetObj_PlayerInput Base = Ctx.m_Input;
			// A quarter follows the flow field, a quarter heads for the mode target, the rest is
			// random so that the search can find routes neither heuristic sees.
			if(i % 4 == 0)
			{
				Base.m_Direction = Flow.x > 0.25f ? 1 : (Flow.x < -0.25f ? -1 : 0);
				Base.m_Jump = Flow.y < -0.25f ? 1 : 0;
			}
			else if(i % 4 == 1 && m_TargetValid)
			{
				Base.m_Direction = ToTarget.x > 8.0f ? 1 : (ToTarget.x < -8.0f ? -1 : 0);
				Base.m_Jump = ToTarget.y < -8.0f ? 1 : 0;
			}

			for(int t = 0; t < Depth; ++t)
			{
				CNetObj_PlayerInput Gene = Base;
				if(i % 4 >= 2 || t > 0)
				{
					const int R = NextRand();
					if((R % 4) == 0)
						Gene.m_Direction = (R / 4) % 3 - 1;
					if((R % 9) == 0)
						Gene.m_Jump ^= 1;
					if((R % 17) == 0)
						Gene.m_Hook ^= 1;
				}
				m_vPopulation[(size_t)i * (size_t)Depth + (size_t)t] = Gene;
			}
		}
		m_Individual = 0;
	}

	void CPilotAgent::Breed(const SSettings &Set)
	{
		// The shape of the running generation is authoritative; a settings change only takes effect
		// when a new round seeds its population.
		const int Count = m_IndividualCount;
		const int Depth = m_IndividualDepth;
		if(Count <= 0 || Depth <= 0)
			return;
		const int TopK = std::clamp(Set.m_PilotTopK, 1, std::min(100, Count));

		std::vector<int> vOrder((size_t)Count);
		for(int i = 0; i < Count; ++i)
			vOrder[i] = i;
		std::stable_sort(vOrder.begin(), vOrder.end(), [&](int a, int b) { return m_vFitness[a] > m_vFitness[b]; });

		m_vNext.assign((size_t)Count * (size_t)Depth, CNetObj_PlayerInput{});

		// Elitism: the Top-K individuals survive unchanged.
		for(int i = 0; i < TopK; ++i)
		{
			for(int t = 0; t < Depth; ++t)
				m_vNext[(size_t)i * (size_t)Depth + (size_t)t] = m_vPopulation[(size_t)vOrder[i] * (size_t)Depth + (size_t)t];
		}

		for(int i = TopK; i < Count; ++i)
		{
			const int ParentA = vOrder[NextRand() % TopK];
			const int ParentB = vOrder[NextRand() % TopK];
			for(int t = 0; t < Depth; ++t)
			{
				// Uniform crossover: every gene is taken from either parent.
				const int Src = (NextRand() & 1) ? ParentA : ParentB;
				CNetObj_PlayerInput Gene = m_vPopulation[(size_t)Src * (size_t)Depth + (size_t)t];
				const int R = NextRand();
				if((R % 7) == 0)
					Gene.m_Direction = (R / 7) % 3 - 1;
				if((R % 23) == 0)
					Gene.m_Jump ^= 1;
				if((R % 29) == 0)
					Gene.m_Hook ^= 1;
				m_vNext[(size_t)i * (size_t)Depth + (size_t)t] = Gene;
			}
		}

		m_vPopulation.swap(m_vNext);
		m_vFitness.assign((size_t)Count, -1e30f);
		m_Individual = 0;
	}

	AvoidInput CPilotAgent::GetAction(const SContext &Ctx, CGameWorld *pWorld)
	{
		const SSettings &Set = Ctx.m_Settings;
		AvoidInput Out;
		Out.m_Input = Ctx.m_Input;

		if(!pWorld)
		{
			str_copy(Out.m_aReason, "no world");
			return Out;
		}

		CCollision *pCollision = pWorld->Collision();
		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pCollision || !pLocal)
		{
			str_copy(Out.m_aReason, "no character");
			return Out;
		}

		const vec2 Pos = pLocal->Core()->m_Pos;
		const int Sequence = std::max(1, Set.m_PilotSequence);

		SSimFlags Flags;
		Flags.m_PredictPlayers = Set.m_PlayerPrediction;
		Flags.m_AvoidFreeze = true;
		Flags.m_AvoidDeath = true;
		Flags.m_AvoidTeles = false;
		Flags.m_AvoidUnfreeze = false;

		// --- 1. Navigable grid. ---------------------------------------------------------------
		// Pilot has no light-tile switch of its own, so its grid never treats freeze as crossable.
		const bool MapChanged = m_Nav.Width() != pCollision->GetWidth() || m_Nav.Height() != pCollision->GetHeight();
		if(MapChanged || m_Nav.LightTile() || (!m_Nav.Ready() && !m_Nav.Building()))
		{
			m_Session.Abort();
			m_Nav.Rebuild(pCollision, false, 0);
			m_SnapshotValid = false;
		}
		if(!m_Nav.Ready())
			m_Nav.Update(pCollision, NAV_WORK_PER_TICK);

		// --- 2. Mode target. ------------------------------------------------------------------
		m_TargetValid = false;
		switch(Set.m_PilotMode)
		{
		case 1: // follow cursor
			m_Target = m_pClient->m_Controls.m_aMousePos[g_Config.m_ClDummy];
			m_TargetValid = true;
			break;
		case 2: // follow player
		{
			int BestTee = -1;
			float BestDist = 1e9f;
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(i == m_pClient->m_Snap.m_LocalClientId)
					continue;
				CCharacter *pOther = pWorld->GetCharacterById(i);
				if(!pOther)
					continue;
				const float Dist = distance(Pos, pOther->Core()->m_Pos);
				if(Dist < BestDist)
				{
					BestDist = Dist;
					BestTee = i;
				}
			}
			if(BestTee >= 0)
			{
				m_Target = pWorld->GetCharacterById(BestTee)->Core()->m_Pos;
				m_TargetValid = true;
			}
			break;
		}
		case 0: // autonomous: follow the navigator towards the goal tiles
		default:
			if(m_Nav.Ready() && m_Nav.GoalTiles() > 0)
			{
				m_Target = Pos + m_Nav.FlowAt(Pos) * 256.0f;
				m_TargetValid = true;
			}
			break;
		}
		if(m_TargetValid)
		{
			Out.m_AimTarget.m_Valid = true;
			Out.m_AimTarget.m_Pos = m_Target;
		}

		// --- 3. Background evolution. ---------------------------------------------------------
		{
			const bool ShapeChanged = m_IndividualCount <= 0 || m_IndividualDepth <= 0 ||
						  m_IndividualCount != std::clamp(Set.m_PilotPopulation, 1, 8192) ||
						  m_IndividualDepth != std::clamp(Set.m_PilotDepth, 1, 50) ||
						  m_vPopulation.size() != (size_t)m_IndividualCount * (size_t)m_IndividualDepth;
			if(!m_SnapshotValid || ShapeChanged)
			{
				m_Session.Abort();
				if(!m_pSnapshot)
					m_pSnapshot = new CGameWorld();
				m_pSnapshot->CopyWorldClean(pWorld);
				m_pSnapshot->m_WorldConfig.m_PredictEvents = false;
				m_SnapshotValid = true;
				if(ShapeChanged || m_vPopulation.empty())
				{
					SeedPopulation(Set, Ctx);
				}
				else
				{
					// Same population, new world: the fitness of every individual is stale, so the
					// generation is evaluated again from the current state.
					m_vFitness.assign((size_t)m_IndividualCount, -1e30f);
					m_Individual = 0;
				}
			}

			// The flow field lives in the navigator, which is never rebuilt while a session runs.
			const std::vector<vec2> &vFlow = m_Nav.Flow();
			m_SessionFlow.m_pDir = vFlow.empty() ? nullptr : vFlow.data();
			m_SessionFlow.m_Width = m_Nav.Width();
			m_SessionFlow.m_Height = m_Nav.Height();
			m_SessionFlow.m_Scale = 1.0f;

			const int GenomeDepth = m_IndividualDepth;
			int StepsLeft = PLANNER_STEPS_PER_TICK;
			while(StepsLeft > 0 && m_Individual < m_IndividualCount)
			{
				if(m_Session.Finished())
				{
					const CNetObj_PlayerInput *pGenome = &m_vPopulation[(size_t)m_Individual * (size_t)GenomeDepth];
					if(!m_Session.Begin(m_pClient, m_pSnapshot, pGenome, GenomeDepth, GenomeDepth, Flags,
						   m_SessionFlow.m_pDir ? &m_SessionFlow : nullptr))
					{
						m_vFitness[m_Individual] = -1e30f;
						m_Individual++;
						continue;
					}
				}

				const int Taken = m_Session.Step(StepsLeft);
				StepsLeft -= Taken;
				if(Taken <= 0)
					break;

				if(m_Session.Finished())
				{
					const CSimSession::SOutcome &Result = m_Session.Outcome();
					const int Survival = Result.m_SurvivalTicks == SIMULATION_SAFE_CONSTANT ? GenomeDepth : Result.m_SurvivalTicks;
					float Fitness = (float)Survival * 100.0f + Result.m_FlowScore * 4.0f;
					if(m_TargetValid)
						Fitness -= distance(Result.m_EndPos, m_Target);
					const int Dist = m_Nav.Ready() ? m_Nav.DistanceAt(Result.m_EndPos) : -1;
					if(Dist >= 0)
						Fitness -= (float)Dist;
					m_vFitness[m_Individual] = Fitness;
					m_Individual++;
				}
			}

			if(m_IndividualCount > 0 && m_Individual >= m_IndividualCount)
			{
				Breed(Set);
				m_PlanEpoch++;
				// The world moved on while the generation was simulated; re-seed from it so the
				// search tracks the live situation instead of a stale snapshot.
				m_SnapshotValid = false;
			}
		}

		// --- 4. Execute the plan, adopting the newest generation every sequence ticks. --------
		// `sequence_length` is the control horizon of the reference client: after that many ticks
		// the newest plan is adopted and driven from its first tick. Between two completed
		// generations the current plan keeps running forward and its last input is held, so a
		// manoeuvre is never restarted.
		const bool NewerGeneration = m_PlanEpoch != m_HeldEpoch;
		// m_PlanIndex saturates at the last step, so "exhausted" has to be tested as "the next
		// step would be past the end".
		const bool WantPlan = m_vPlan.empty() ||
				      (NewerGeneration && (m_PlanTick >= Sequence || m_PlanIndex + 1 >= (int)m_vPlan.size()));
		if(WantPlan && m_IndividualCount > 0 && m_IndividualDepth > 0 &&
			m_vPopulation.size() == (size_t)m_IndividualCount * (size_t)m_IndividualDepth &&
			m_vFitness.size() == (size_t)m_IndividualCount)
		{
			int BestIdx = 0;
			for(int i = 1; i < m_IndividualCount; ++i)
				if(m_vFitness[i] > m_vFitness[BestIdx])
					BestIdx = i;
			m_vPlan.assign(m_vPopulation.begin() + (size_t)BestIdx * (size_t)m_IndividualDepth,
				m_vPopulation.begin() + (size_t)(BestIdx + 1) * (size_t)m_IndividualDepth);
			m_PlanIndex = 0;
			m_PlanTick = 0;
			m_HeldEpoch = m_PlanEpoch;
		}

		if(!m_vPlan.empty())
		{
			const int Step = std::clamp(m_PlanIndex, 0, (int)m_vPlan.size() - 1);
			Out.m_Input = m_vPlan[(size_t)Step];

			// Guard: never let a stale plan be worse than what the player pressed. The comparison
			// is over the same window on both sides.
			const int PlanSafety = SimulateCandidate(m_pClient, pWorld, Out.m_Input, PLAN_GUARD_TICKS, Flags);
			const int PlayerSafety = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, PLAN_GUARD_TICKS, Flags);
			const bool PlanUsable = PlanSafety == SIMULATION_SAFE_CONSTANT || PlanSafety > PlayerSafety;
			if(PlanUsable)
			{
				Out.m_Active = 1;
				Out.m_SurvivalTicks = PlanSafety == SIMULATION_SAFE_CONSTANT ? PLAN_GUARD_TICKS : PlanSafety;
				if(m_PlanIndex + 1 < (int)m_vPlan.size())
					m_PlanIndex++;
				m_PlanTick++;
				str_copy(Out.m_aReason, "pilot plan");
			}
			else
			{
				m_vPlan.clear();
				m_PlanIndex = 0;
				m_PlanTick = 0;
				m_HeldEpoch = m_PlanEpoch;
			}
		}

		if(!Out.m_Active)
		{
			// Nothing usable planned yet: stay alive with a one step greedy search.
			const int Baseline = SimulateCandidate(m_pClient, pWorld, Ctx.m_Input, PLAN_GUARD_TICKS, Flags);
			int Best = Baseline;
			static const int s_aDirs[3] = {0, -1, 1};
			for(const int Dir : s_aDirs)
			{
				CNetObj_PlayerInput Candidate = Ctx.m_Input;
				Candidate.m_Direction = Dir;
				const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, PLAN_GUARD_TICKS, Flags);
				if(Score > Best)
				{
					Best = Score;
					Out.m_Input = Candidate;
					Out.m_Active = 1;
				}
			}
			Out.m_SurvivalTicks = Best == SIMULATION_SAFE_CONSTANT ? PLAN_GUARD_TICKS : Best;
			str_copy(Out.m_aReason, Out.m_Active ? "survival fallback while evolving" : "player input safe");
		}

		if(Set.m_DrawPath && Out.m_Active)
		{
			Out.m_vPath.clear();
			vec2 Walk = Pos;
			Out.m_vPath.push_back(Walk);
			for(int i = 0; i < 40; ++i)
			{
				const vec2 Flow = m_Nav.FlowAt(Walk);
				if(length(Flow) < 0.001f)
					break;
				Walk += Flow * 24.0f;
				Out.m_vPath.push_back(Walk);
			}
		}

		return Out;
	}

} // namespace Avoid
