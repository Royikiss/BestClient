/* Copyright © 2026 BestProject Team */
#include "avoid_engine.h"

#include <base/math.h>
#include <base/mem.h>
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
#include <queue>
#include <vector>

namespace Avoid
{
	// -----------------------------------------------------------------------------------------
	// Sensing layer helpers
	// -----------------------------------------------------------------------------------------

	int ClassifyTile(int Tile)
	{
		switch(Tile)
		{
		case TILE_DEATH:
			return HAZ_DEATH;
		case TILE_FREEZE:
			return HAZ_FREEZE;
		case TILE_DFREEZE:
			return HAZ_DEEP;
		case TILE_LFREEZE:
			return HAZ_LIVE;
		case TILE_UNFREEZE:
			return HAZ_UNFREEZE;
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
		if(Flags & HAZ_SELF)
			return true;
		return (Flags & HazardMask(Set)) != 0;
	}

	int ClassifyPoint(CCollision *pCollision, vec2 Pos)
	{
		int Flags = HAZ_NONE;
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return Flags;

		const int Index = pCollision->GetPureMapIndex(Pos);
		if(Index >= 0)
		{
			Flags |= ClassifyTile(pCollision->GetTileIndex(Index));
			Flags |= ClassifyTile(pCollision->GetFrontTileIndex(Index));
			Flags |= ClassifyTile(pCollision->GetSwitchType(Index));
		}

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

		Threat.m_Flags = ClassifyPoint(pCollision, Core.m_Pos);
		if(Core.m_IsInFreeze || Core.m_DeepFrozen || Core.m_LiveFrozen)
			Threat.m_Flags |= HAZ_SELF;
		Threat.m_OnHazard = IsRelevantHazard(Set, Threat.m_Flags);

		const float ReachX = std::max(1.0f, Set.m_SensingRadius) * TILE_SIZE;
		const float Factor = SensingVerticalFactor(Core, Set);
		const float ReachY = std::max(1.0f, ReachX * Factor);

		const int MinTileX = std::max(0, (int)std::floor((Core.m_Pos.x - ReachX) / TILE_SIZE));
		const int MaxTileX = std::min(pCollision->GetWidth() - 1, (int)std::ceil((Core.m_Pos.x + ReachX) / TILE_SIZE));
		const int MinTileY = std::max(0, (int)std::floor((Core.m_Pos.y - ReachY) / TILE_SIZE));
		const int MaxTileY = std::min(pCollision->GetHeight() - 1, (int)std::ceil((Core.m_Pos.y + ReachY) / TILE_SIZE));

		float BestDistPx = 1e9f;
		for(int Ty = MinTileY; Ty <= MaxTileY; ++Ty)
		{
			for(int Tx = MinTileX; Tx <= MaxTileX; ++Tx)
			{
				Threat.m_SensedTiles++;
				const int TileIndex = Tx + Ty * pCollision->GetWidth();
				int TileFlags = HAZ_NONE;
				TileFlags |= ClassifyTile(pCollision->GetTileIndex(TileIndex));
				TileFlags |= ClassifyTile(pCollision->GetFrontTileIndex(TileIndex));
				TileFlags |= ClassifyTile(pCollision->GetSwitchType(TileIndex));
				if(pCollision->GetCollisionAt(Tx * TILE_SIZE + 16.0f, Ty * TILE_SIZE + 16.0f) == TILE_DEATH)
					TileFlags |= HAZ_DEATH;

				if(!IsRelevantHazard(Set, TileFlags))
					continue;

				const vec2 Delta = TileBoxDelta(Core.m_Pos, Tx, Ty);
				const float EllipseDist = std::sqrt(Delta.x * Delta.x + (Delta.y / Factor) * (Delta.y / Factor));
				if(EllipseDist > ReachX)
					continue;

				Threat.m_HazardTiles++;
				const float RealDist = length(Delta);
				if(RealDist < BestDistPx)
				{
					BestDistPx = RealDist;
					Threat.m_NearestPos = vec2(Tx * TILE_SIZE + 16.0f, Ty * TILE_SIZE + 16.0f);
					Threat.m_NearestDistPx = RealDist;
					Threat.m_HasNearest = true;
				}
			}
		}

		return Threat;
	}

	// -----------------------------------------------------------------------------------------
	// Math & aim helpers
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

	float AimAngleDeg(vec2 Dir)
	{
		const vec2 Unit = normalize(Dir);
		if(length(Unit) <= 0.0001f)
			return 0.0f;
		return std::atan2(Unit.y, Unit.x) * (180.0f / pi);
	}

	bool IsHookable(CCollision *pCollision, vec2 From, vec2 Dir, float HookLength, vec2 *pOutPos, float *pOutDist)
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
			if(pOutDist)
				*pOutDist = distance(From, Hit);
			return true;
		}
		return false;
	}

	// -----------------------------------------------------------------------------------------
	// BLAgent Base class
	// -----------------------------------------------------------------------------------------

	CGameWorld *BLAgent::GetBaseWorld() const
	{
		if(!m_pClient)
			return nullptr;
		if(m_pClient->m_FastPractice.Active())
			return &m_pClient->m_FastPractice.PracticeWorld();
		if(m_pClient->m_PredictedWorld.GetCharacterById(m_pClient->m_Snap.m_LocalClientId))
			return &m_pClient->m_PredictedWorld;
		if(m_pClient->m_GameWorld.GetCharacterById(m_pClient->m_Snap.m_LocalClientId))
			return &m_pClient->m_GameWorld;
		return &m_pClient->m_PredictedWorld;
	}

	// -----------------------------------------------------------------------------------------
	// Forward simulator (KRX spec 4.1 func_0x00014036a8d0)
	// -----------------------------------------------------------------------------------------

	int SimulateCandidate(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput &CandidateInput,
		int CheckTicks,
		bool PredictPlayers,
		bool AvoidTeles,
		bool AvoidDeath,
		bool AvoidFreeze,
		bool AvoidUnfreeze,
		int UnfreezeTicks)
	{
		if(!pClient || !pBaseWorld || CheckTicks <= 0)
			return SIMULATION_SAFE_CONSTANT;

		// 1. Create cloned world on stack/local heap
		CGameWorld ClonedWorld;
		pBaseWorld->CopyWorldClean(&ClonedWorld);
		ClonedWorld.m_WorldConfig.m_PredictEvents = false;

		const int LocalClientId = pClient->m_Snap.m_LocalClientId;
		CCharacter *pChar = ClonedWorld.GetCharacterById(LocalClientId);
		if(!pChar)
			return SIMULATION_SAFE_CONSTANT;

		CCollision *pCollision = ClonedWorld.Collision();
		if(!pCollision)
			return SIMULATION_SAFE_CONSTANT;

		// 2. Step forward simulation
		for(int Tick = 0; Tick < CheckTicks; ++Tick)
		{
			// Inject input candidate
			pChar->OnDirectInput(&CandidateInput);
			pChar->OnPredictedInput(&CandidateInput);

			// Step 1 physical frame (50Hz)
			ClonedWorld.Tick();

			// Character may have died or been removed
			pChar = ClonedWorld.GetCharacterById(LocalClientId);
			if(!pChar)
				return Tick;

			// 3. Hazard checks
			// A. Freeze tiles (Normal, Deep, Live)
			if(AvoidFreeze && (pChar->m_FreezeTime > 0 || pChar->m_FrozenLastTick || pChar->Core()->m_IsInFreeze))
				return Tick;

			// B. Death tiles
			if(AvoidDeath)
			{
				const vec2 Pos = pChar->Core()->m_Pos;
				const float Rad = pChar->GetProximityRadius() / 3.0f;
				if((pCollision->GetCollisionAt(Pos.x, Pos.y) & TILE_DEATH) ||
					(pCollision->GetCollisionAt(Pos.x + Rad, Pos.y + Rad) & TILE_DEATH) ||
					(pCollision->GetCollisionAt(Pos.x - Rad, Pos.y + Rad) & TILE_DEATH) ||
					(pCollision->GetCollisionAt(Pos.x + Rad, Pos.y - Rad) & TILE_DEATH) ||
					(pCollision->GetCollisionAt(Pos.x - Rad, Pos.y - Rad) & TILE_DEATH))
				{
					return Tick;
				}
			}

			// C. Teleport tiles
			if(AvoidTeles)
			{
				const vec2 Pos = pChar->Core()->m_Pos;
				const int MapIndex = pCollision->GetPureMapIndex(Pos);
				if(pCollision->IsTeleport(MapIndex) || pCollision->IsEvilTeleport(MapIndex) ||
					pCollision->IsCheckTeleport(MapIndex) || pCollision->IsCheckEvilTeleport(MapIndex) ||
					pCollision->IsTeleCheckpoint(MapIndex))
				{
					return Tick;
				}
			}

			// D. Unfreeze tiles (when AvoidUnfreeze is requested)
			if(AvoidUnfreeze && Tick < UnfreezeTicks)
			{
				const vec2 Pos = pChar->Core()->m_Pos;
				const int Tile = pCollision->GetCollisionAt(Pos.x, Pos.y);
				if(Tile == TILE_UNFREEZE)
					return Tick;
			}

			// E. Character collisions
			if(!PredictPlayers)
			{
				pChar->Core()->m_Colliding = 0;
			}
		}

		return SIMULATION_SAFE_CONSTANT;
	}

	// -----------------------------------------------------------------------------------------
	// 1. Basic Agent (KRX spec 5)
	// -----------------------------------------------------------------------------------------

	AvoidInput CBasicAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
	{
		AvoidInput Result;
		Result.m_Input = *pCurrentInput;
		Result.m_Active = 0;

		CGameWorld *pWorld = GetBaseWorld();
		if(!pWorld)
		{
			str_copy(Result.m_aReason, "No world data");
			return Result;
		}

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Result.m_aReason, "No character");
			return Result;
		}

		if(pLocal->m_FreezeTime > 0 || pLocal->m_FrozenLastTick || pLocal->Core()->m_IsInFreeze)
		{
			str_copy(Result.m_aReason, "Frozen, agent idle");
			return Result;
		}

		const int CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
		const int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		Result.m_SurvivalTicks = (CurrentSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CurrentSafety;

		// 1. Check if current action is already safe
		if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Result.m_aReason, "Player input safe");
			return Result;
		}

		// 2. Discrete 3-direction candidates
		const int Directions[3] = {-1, 0, 1};
		int BestScore = -1;
		int BestDir = pCurrentInput->m_Direction;

		for(int Dir : Directions)
		{
			CNetObj_PlayerInput Candidate = *pCurrentInput;
			Candidate.m_Direction = Dir;

			const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks,
				g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
				g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

			if(Score > BestScore || (Score == BestScore && Dir == pCurrentInput->m_Direction))
			{
				BestScore = Score;
				BestDir = Dir;
			}
		}

		if(BestScore > CurrentSafety)
		{
			Result.m_Input.m_Direction = BestDir;
			Result.m_Active = 1;
			Result.m_SurvivalTicks = (BestScore == SIMULATION_SAFE_CONSTANT) ? CheckTicks : BestScore;
			if(BestDir == 0)
				str_copy(Result.m_aReason, "Brake before hazard");
			else if(BestDir == -1)
				str_copy(Result.m_aReason, "Steer left before hazard");
			else
				str_copy(Result.m_aReason, "Steer right before hazard");
		}
		else
		{
			str_copy(Result.m_aReason, "No safer plan found");
		}

		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// 2. Blatant Agent (KRX spec 6)
	// -----------------------------------------------------------------------------------------

	void CBlatantAgent::OnReset()
	{
		m_TrackPointValid = false;
		m_TrackPointPos = vec2(0.0f, 0.0f);
		m_TrackPointDir = vec2(0.0f, 0.0f);
		m_SavedSafeSequence.clear();
	}

	void CBlatantAgent::OnRender()
	{
		// Render visuals if active
	}

	AvoidInput CBlatantAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
	{
		AvoidInput Result;
		Result.m_Input = *pCurrentInput;
		Result.m_Active = 0;

		CGameWorld *pWorld = GetBaseWorld();
		if(!pWorld)
		{
			str_copy(Result.m_aReason, "No world data");
			return Result;
		}

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Result.m_aReason, "No character");
			return Result;
		}

		if(pLocal->m_FreezeTime > 0 || pLocal->m_FrozenLastTick || pLocal->Core()->m_IsInFreeze)
		{
			str_copy(Result.m_aReason, "Frozen, agent idle");
			return Result;
		}

		const int CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
		const int KickInTicks = std::clamp(g_Config.m_BcAvoidKickInTicks, 0, 50);

		// Record Track Point from player's own aim
		const vec2 Pos = pLocal->Core()->m_Pos;
		const float HookLength = pLocal->Core()->m_Tuning.m_HookLength;
		const vec2 PlayerAim = AimDirection(pCurrentInput->m_TargetX, pCurrentInput->m_TargetY);
		vec2 HitPos;
		if(IsHookable(pWorld->Collision(), Pos, PlayerAim, HookLength, &HitPos))
		{
			m_TrackPointValid = true;
			m_TrackPointPos = HitPos;
			m_TrackPointDir = PlayerAim;
		}

		// 1. Check current safety
		const int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		Result.m_SurvivalTicks = (CurrentSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CurrentSafety;

		// Hysteresis kick-in check
		if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Result.m_aReason, "Player input safe");
			return Result;
		}
		if(KickInTicks > 0 && CurrentSafety >= KickInTicks)
		{
			str_copy(Result.m_aReason, "Still time before hazard");
			return Result;
		}

		// 2. Candidate action space
		std::vector<int> aDirs;
		if(g_Config.m_BcAvoidDirectionAssist)
		{
			aDirs.push_back(pCurrentInput->m_Direction);
			for(int d : {-1, 0, 1})
				if(d != pCurrentInput->m_Direction)
					aDirs.push_back(d);
		}
		else
		{
			aDirs.push_back(pCurrentInput->m_Direction);
		}

		std::vector<int> aHooks;
		if(g_Config.m_BcAvoidHookAssist)
		{
			aHooks.push_back(pCurrentInput->m_Hook);
			const int OtherHook = (pCurrentInput->m_Hook == 0) ? 1 : 0;
			aHooks.push_back(OtherHook);
		}
		else
		{
			aHooks.push_back(pCurrentInput->m_Hook);
		}

		// Candidate aims
		std::vector<vec2> aAims;
		aAims.push_back(PlayerAim);

		// Track point candidate with Safe Aim Tracking
		if(g_Config.m_BcAvoidTrackPoint && m_TrackPointValid)
		{
			const vec2 DirToTrack = normalize(m_TrackPointPos - Pos);
			if(length(DirToTrack) > 0.0001f)
			{
				bool TrackSafe = true;
				if(g_Config.m_BcAvoidSafeAimTracking)
				{
					CNetObj_PlayerInput TestInput = *pCurrentInput;
					AimTargetsFrom(DirToTrack, &TestInput.m_TargetX, &TestInput.m_TargetY);
					TestInput.m_Hook = 1;
					const int SimRes = SimulateCandidate(m_pClient, pWorld, TestInput, CheckTicks,
						g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
						g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);
					TrackSafe = (SimRes == SIMULATION_SAFE_CONSTANT || SimRes > CurrentSafety);
				}
				if(TrackSafe)
					aAims.push_back(DirToTrack);
			}
		}

		// Auto drag candidate
		if(g_Config.m_BcAvoidAutoDrag && g_Config.m_BcAvoidPlayerPrediction)
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
				const float D = distance(Pos, pOther->Core()->m_Pos);
				if(D <= BestDist)
				{
					BestDist = D;
					BestTee = i;
				}
			}
			if(BestTee >= 0)
			{
				CCharacter *pOther = pWorld->GetCharacterById(BestTee);
				const vec2 DirToTee = normalize(pOther->Core()->m_Pos - Pos);
				if(length(DirToTee) > 0.0001f)
					aAims.push_back(DirToTee);
			}
		}

		// Internal aimbot scan
		if(g_Config.m_BcAvoidAimbot)
		{
			const int BaseSeg = std::clamp(g_Config.m_BcAvoidAimbotSegments, 4, 128);
			const int Segments = std::clamp((BaseSeg * std::clamp(g_Config.m_BcAvoidQuality, 1, 200)) / 24, 4, 128);
			const float FovRad = (float)std::clamp(g_Config.m_BcAvoidAimbotFov, 10, 180) * (pi / 180.0f);
			const float BaseAngle = std::atan2(PlayerAim.y, PlayerAim.x);
			for(int s = 0; s < Segments; ++s)
			{
				const float Offset = -FovRad * 0.5f + FovRad * ((float)s / (float)(Segments - 1));
				const float Angle = BaseAngle + Offset;
				const vec2 ScanDir = vec2(std::cos(Angle), std::sin(Angle));
				vec2 ScanHit;
				if(IsHookable(pWorld->Collision(), Pos, ScanDir, HookLength, &ScanHit))
				{
					aAims.push_back(ScanDir);
				}
			}
		}

		// 3. Greedy search with priority weighting
		double BestEvaluation = -1e9;
		int BestSurvivalTicks = -1;
		CNetObj_PlayerInput BestInput = *pCurrentInput;
		bool FoundSafe = false;

		for(const auto &Aim : aAims)
		{
			int TargetX = 0, TargetY = -1;
			AimTargetsFrom(Aim, &TargetX, &TargetY);

			for(int Dir : aDirs)
			{
				for(int Hook : aHooks)
				{
					CNetObj_PlayerInput Candidate = *pCurrentInput;
					Candidate.m_Direction = Dir;
					Candidate.m_Hook = Hook;
					Candidate.m_TargetX = TargetX;
					Candidate.m_TargetY = TargetY;

					const int Score = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks,
						g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
						g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

					const int Survival = (Score == SIMULATION_SAFE_CONSTANT) ? CheckTicks : Score;
					double Evaluation = (double)Survival * (g_Config.m_BcAvoidLifeWeight * 0.01);
					if(Dir == pCurrentInput->m_Direction)
						Evaluation += (double)g_Config.m_BcAvoidDirectionWeight * 0.01;
					if(Hook == pCurrentInput->m_Hook)
						Evaluation += (double)g_Config.m_BcAvoidHookWeight * 0.01;

					// Aimbot Mode 1 (Aim assist): prioritize keeping cursor close to player's aim
					if(g_Config.m_BcAvoidAimbot && g_Config.m_BcAvoidAimbotMode == 1 && (Score == SIMULATION_SAFE_CONSTANT || Score > CurrentSafety))
					{
						float AngleDiff = std::abs(AimAngleDeg(Aim) - AimAngleDeg(PlayerAim));
						if(AngleDiff > 180.0f)
							AngleDiff = 360.0f - AngleDiff;
						Evaluation -= (double)AngleDiff * 0.005;
					}

					// Randomness noise
					if(g_Config.m_BcAvoidRandomness > 0)
					{
						Evaluation += ((rand() % (g_Config.m_BcAvoidRandomness + 1)) * 0.001);
					}

					if(Evaluation > BestEvaluation)
					{
						BestEvaluation = Evaluation;
						BestSurvivalTicks = Survival;
						BestInput = Candidate;
						if(Score == SIMULATION_SAFE_CONSTANT)
							FoundSafe = true;
					}
				}
			}
		}

		// 4. NSIF handling
		bool UsedFallback = false;
		if(FoundSafe)
		{
			m_SavedSafeSequence.clear();
			m_SavedSafeSequence.push_back(BestInput);
		}
		else if(g_Config.m_BcAvoidNsif && !m_SavedSafeSequence.empty())
		{
			BestInput = m_SavedSafeSequence.front();
			BestSurvivalTicks = CheckTicks;
			UsedFallback = true;
		}
		else if(BestSurvivalTicks > CurrentSafety)
		{
			m_SavedSafeSequence.clear();
			m_SavedSafeSequence.push_back(BestInput);
		}

		if(BestSurvivalTicks > CurrentSafety || UsedFallback)
		{
			Result.m_Input = BestInput;
			Result.m_Active = 1;
			Result.m_SurvivalTicks = BestSurvivalTicks;
			Result.m_UsedFallback = UsedFallback;

			if(UsedFallback)
			{
				str_copy(Result.m_aReason, "NSIF: using saved safe sequence");
			}
			else if(BestInput.m_TargetX != pCurrentInput->m_TargetX || BestInput.m_TargetY != pCurrentInput->m_TargetY)
			{
				if(BestInput.m_Hook != 0)
					str_copy(Result.m_aReason, "Aim hook at safe surface");
				else
					str_copy(Result.m_aReason, "Aim crosshair to safety");
			}
			else if(BestInput.m_Hook != pCurrentInput->m_Hook)
			{
				str_copy(Result.m_aReason, (BestInput.m_Hook ? "Hook to anchor" : "Release hook"));
			}
			else if(BestInput.m_Direction != pCurrentInput->m_Direction)
			{
				str_copy(Result.m_aReason, (BestInput.m_Direction == 0 ? "Brake before hazard" : "Steer away from hazard"));
			}
			else
			{
				str_copy(Result.m_aReason, "Intervention safe");
			}
		}
		else
		{
			str_copy(Result.m_aReason, "No safe plan found");
		}

		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// 3. Legit Agent (KRX spec 7)
	// -----------------------------------------------------------------------------------------

	AvoidInput CLegitAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
	{
		AvoidInput Result;
		Result.m_Input = *pCurrentInput;
		Result.m_Active = 0;

		CGameWorld *pWorld = GetBaseWorld();
		if(!pWorld)
		{
			str_copy(Result.m_aReason, "No world data");
			return Result;
		}

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Result.m_aReason, "No character");
			return Result;
		}

		if(pLocal->m_FreezeTime > 0 || pLocal->m_FrozenLastTick || pLocal->Core()->m_IsInFreeze)
		{
			str_copy(Result.m_aReason, "Frozen, agent idle");
			return Result;
		}

		const int CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
		const int KickInTicks = std::clamp(g_Config.m_BcAvoidKickInTicks, 0, 50);

		// 1. Fast path: check current input
		const int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		Result.m_SurvivalTicks = (CurrentSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CurrentSafety;

		if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Result.m_aReason, "Player input safe");
			return Result;
		}
		if(KickInTicks > 0 && CurrentSafety >= KickInTicks)
		{
			str_copy(Result.m_aReason, "Still time before hazard");
			return Result;
		}

		// 2. MCTS Node definition
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
				for(auto *pChild : m_vChildren)
					delete pChild;
			}
		};

		MCTSNode *pRoot = new MCTSNode();
		pRoot->m_Action = *pCurrentInput;

		const int Iterations = std::clamp(g_Config.m_BcAvoidQuality, 5, 200);
		const double ExplorationC = (double)std::clamp(g_Config.m_BcAvoidRandomness, 1, 1000) * 0.01;
		const double WeightDir = (double)g_Config.m_BcAvoidDirectionWeight;
		const double WeightLife = (double)g_Config.m_BcAvoidLifeWeight;
		const double WeightHook = (double)g_Config.m_BcAvoidHookWeight;

		for(int iter = 0; iter < Iterations; ++iter)
		{
			// Selection
			MCTSNode *pCurr = pRoot;
			while(!pCurr->m_vChildren.empty())
			{
				MCTSNode *pBestChild = nullptr;
				double BestScore = -1e9;
				for(MCTSNode *pChild : pCurr->m_vChildren)
				{
					double Uct = 0.0;
					if(pChild->m_Visits == 0)
					{
						Uct = 1e5;
					}
					else
					{
						const double Exploitation = pChild->m_TotalValue / (double)pChild->m_Visits;
						const double Exploration = ExplorationC * std::sqrt(std::log((double)pCurr->m_Visits) / (double)pChild->m_Visits);

						// Heuristic (KRX spec 7.2)
						const double DirDiff = (double)std::abs(pChild->m_Action.m_Direction - pCurrentInput->m_Direction);
						const double DirPenalty = -DirDiff * WeightDir * 0.001;

						const double HookDiff = (double)std::abs(pChild->m_Action.m_Hook - pCurrentInput->m_Hook);
						const double HookPenalty = -HookDiff * WeightHook * 0.001;

						const double LifeReward = (double)pChild->m_LifespanTicks * WeightLife * 0.001;

						const double Heuristic = DirPenalty + HookPenalty + LifeReward;
						Uct = Exploitation + Exploration + Heuristic;
					}

					if(Uct > BestScore)
					{
						BestScore = Uct;
						pBestChild = pChild;
					}
				}
				pCurr = pBestChild ? pBestChild : pCurr->m_vChildren[0];
			}

			// Expansion
			if(!pCurr->m_IsTerminal && pCurr->m_Visits > 0)
			{
				std::vector<int> aDirs = g_Config.m_BcAvoidDirectionAssist ? std::vector<int>{-1, 0, 1} : std::vector<int>{pCurrentInput->m_Direction};
				std::vector<int> aHooks = g_Config.m_BcAvoidHookAssist ? std::vector<int>{0, 1} : std::vector<int>{pCurrentInput->m_Hook};

				for(int d : aDirs)
				{
					for(int h : aHooks)
					{
						MCTSNode *pNewChild = new MCTSNode();
						pNewChild->m_pParent = pCurr;
						pNewChild->m_Action = pCurr->m_Action;
						pNewChild->m_Action.m_Direction = d;
						pNewChild->m_Action.m_Hook = h;
						pCurr->m_vChildren.push_back(pNewChild);
					}
				}
				if(!pCurr->m_vChildren.empty())
					pCurr = pCurr->m_vChildren[rand() % pCurr->m_vChildren.size()];
			}

			// Simulation / Rollout
			const int Survival = SimulateCandidate(m_pClient, pWorld, pCurr->m_Action, CheckTicks,
				g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
				g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

			const double Reward = (Survival == SIMULATION_SAFE_CONSTANT) ? 1.0 : ((double)Survival / (double)CheckTicks);
			pCurr->m_LifespanTicks = (Survival == SIMULATION_SAFE_CONSTANT) ? CheckTicks : Survival;
			if(Survival < CheckTicks)
				pCurr->m_IsTerminal = true;

			// Backpropagation
			while(pCurr)
			{
				pCurr->m_Visits++;
				pCurr->m_TotalValue += Reward;
				pCurr = pCurr->m_pParent;
			}
		}

		// Choose child with most visits
		MCTSNode *pSelected = nullptr;
		int MostVisits = -1;
		for(MCTSNode *pChild : pRoot->m_vChildren)
		{
			if(pChild->m_Visits > MostVisits)
			{
				MostVisits = pChild->m_Visits;
				pSelected = pChild;
			}
		}

		// NSIF fallback
		if((!pSelected || pSelected->m_LifespanTicks <= CurrentSafety) && g_Config.m_BcAvoidNsif)
		{
			int BestSurvival = -1;
			for(MCTSNode *pChild : pRoot->m_vChildren)
			{
				if(pChild->m_LifespanTicks > BestSurvival)
				{
					BestSurvival = pChild->m_LifespanTicks;
					pSelected = pChild;
				}
			}
		}

		if(pSelected && pSelected->m_LifespanTicks > CurrentSafety)
		{
			Result.m_Input = pSelected->m_Action;
			Result.m_Active = 1;
			Result.m_SurvivalTicks = pSelected->m_LifespanTicks;
			Result.m_UsedFallback = (pSelected->m_LifespanTicks < CheckTicks);

			if(Result.m_UsedFallback)
				str_copy(Result.m_aReason, "NSIF: longest survival action");
			else if(Result.m_Input.m_Hook != pCurrentInput->m_Hook && Result.m_Input.m_Direction != pCurrentInput->m_Direction)
				str_copy(Result.m_aReason, "Release hook, steer to safety");
			else if(Result.m_Input.m_Hook != pCurrentInput->m_Hook)
				str_copy(Result.m_aReason, (Result.m_Input.m_Hook ? "Hook to safety" : "Release hook"));
			else if(Result.m_Input.m_Direction != pCurrentInput->m_Direction)
				str_copy(Result.m_aReason, (Result.m_Input.m_Direction == 0 ? "Brake before hazard" : "Steer to safety"));
			else
				str_copy(Result.m_aReason, "Subtle assist");
		}
		else
		{
			str_copy(Result.m_aReason, "No safer plan found");
		}

		delete pRoot;
		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// 4. Fentbot Agent (KRX spec 8)
	// -----------------------------------------------------------------------------------------

	void CFentbotAgent::OnReset()
	{
		m_FlowFieldCalculated = false;
		m_vFlowField.clear();
	}

	void CFentbotAgent::OnRender()
	{
	}

	void CFentbotAgent::CalculateFlowField(CCollision *pCollision)
	{
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return;

		const int Width = pCollision->GetWidth();
		const int Height = pCollision->GetHeight();
		const int TotalTiles = Width * Height;

		m_LastMapWidth = Width;
		m_LastMapHeight = Height;
		m_vFlowField.assign(TotalTiles, vec2(0.0f, 0.0f));

		std::vector<int> aDist(TotalTiles, 1e8);
		std::queue<int> Q;

		// Seed safe destination tiles (unfreeze, finish)
		for(int y = 0; y < Height; ++y)
		{
			for(int x = 0; x < Width; ++x)
			{
				const int Idx = x + y * Width;
				const int Tile = pCollision->GetTileIndex(Idx);
				if(Tile == TILE_UNFREEZE)
				{
					aDist[Idx] = 0;
					Q.push(Idx);
				}
			}
		}

		if(Q.empty())
		{
			m_FlowFieldCalculated = true;
			return;
		}

		const int dx[4] = {-1, 1, 0, 0};
		const int dy[4] = {0, 0, -1, 1};

		while(!Q.empty())
		{
			const int Curr = Q.front();
			Q.pop();
			const int Cx = Curr % Width;
			const int Cy = Curr / Width;

			for(int k = 0; k < 4; ++k)
			{
				const int Nx = Cx + dx[k];
				const int Ny = Cy + dy[k];
				if(Nx >= 0 && Nx < Width && Ny >= 0 && Ny < Height)
				{
					const int NextIdx = Nx + Ny * Width;
					const int Tile = pCollision->GetTileIndex(NextIdx);
					if(Tile == TILE_DEATH || Tile == TILE_FREEZE || Tile == TILE_DFREEZE)
						continue;

					if(aDist[NextIdx] > aDist[Curr] + 1)
					{
						aDist[NextIdx] = aDist[Curr] + 1;
						Q.push(NextIdx);
					}
				}
			}
		}

		// Compute gradient flow vector
		for(int y = 1; y < Height - 1; ++y)
		{
			for(int x = 1; x < Width - 1; ++x)
			{
				const int Idx = x + y * Width;
				const float GradX = (float)(aDist[(x - 1) + y * Width] - aDist[(x + 1) + y * Width]);
				const float GradY = (float)(aDist[x + (y - 1) * Width] - aDist[x + (y + 1) * Width]);
				vec2 Dir = vec2(GradX, GradY);
				if(length(Dir) > 0.0001f)
					m_vFlowField[Idx] = normalize(Dir);
			}
		}

		m_FlowFieldCalculated = true;
	}

	AvoidInput CFentbotAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
	{
		AvoidInput Result;
		Result.m_Input = *pCurrentInput;
		Result.m_Active = 0;

		CGameWorld *pWorld = GetBaseWorld();
		if(!pWorld)
		{
			str_copy(Result.m_aReason, "No world data");
			return Result;
		}

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Result.m_aReason, "No character");
			return Result;
		}

		if(pLocal->m_FreezeTime > 0 || pLocal->m_FrozenLastTick || pLocal->Core()->m_IsInFreeze)
		{
			str_copy(Result.m_aReason, "Frozen, agent idle");
			return Result;
		}

		CCollision *pCollision = pWorld->Collision();
		if(!m_FlowFieldCalculated || m_LastMapWidth != pCollision->GetWidth() || m_LastMapHeight != pCollision->GetHeight())
		{
			CalculateFlowField(pCollision);
		}

		const int CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
		const int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		Result.m_SurvivalTicks = (CurrentSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CurrentSafety;

		const vec2 Pos = pLocal->Core()->m_Pos;
		const int TileX = std::clamp((int)(Pos.x / TILE_SIZE), 0, m_LastMapWidth - 1);
		const int TileY = std::clamp((int)(Pos.y / TILE_SIZE), 0, m_LastMapHeight - 1);
		const int TileIdx = TileX + TileY * m_LastMapWidth;

		vec2 FlowDir = vec2(0.0f, 0.0f);
		if(TileIdx >= 0 && TileIdx < (int)m_vFlowField.size())
			FlowDir = m_vFlowField[TileIdx];

		int DesiredDir = pCurrentInput->m_Direction;
		if(FlowDir.x > 0.3f)
			DesiredDir = 1;
		else if(FlowDir.x < -0.3f)
			DesiredDir = -1;

		CNetObj_PlayerInput Candidate = *pCurrentInput;
		Candidate.m_Direction = DesiredDir;
		if(FlowDir.y < -0.3f)
			Candidate.m_Jump = 1;

		const int CandidateSafety = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		if(CandidateSafety > CurrentSafety || (CurrentSafety < CheckTicks && CandidateSafety == SIMULATION_SAFE_CONSTANT))
		{
			Result.m_Input = Candidate;
			Result.m_Active = 1;
			Result.m_SurvivalTicks = (CandidateSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CandidateSafety;
			str_copy(Result.m_aReason, "Flow field navigation");
		}
		else if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Result.m_aReason, "Player input safe");
		}
		else
		{
			str_copy(Result.m_aReason, "Calculating path");
		}

		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// 5. Pilot Agent (docs/avoid/pilotbot.md)
	// -----------------------------------------------------------------------------------------

	void CPilotAgent::OnReset()
	{
	}

	void CPilotAgent::OnRender()
	{
	}

	AvoidInput CPilotAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
	{
		AvoidInput Result;
		Result.m_Input = *pCurrentInput;
		Result.m_Active = 0;

		CGameWorld *pWorld = GetBaseWorld();
		if(!pWorld)
		{
			str_copy(Result.m_aReason, "No world data");
			return Result;
		}

		CCharacter *pLocal = pWorld->GetCharacterById(m_pClient->m_Snap.m_LocalClientId);
		if(!pLocal)
		{
			str_copy(Result.m_aReason, "No character");
			return Result;
		}

		if(pLocal->m_FreezeTime > 0 || pLocal->m_FrozenLastTick || pLocal->Core()->m_IsInFreeze)
		{
			str_copy(Result.m_aReason, "Frozen, agent idle");
			return Result;
		}

		const int CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
		const int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		Result.m_SurvivalTicks = (CurrentSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CurrentSafety;

		// Pilot target based on aim / crosshair
		const vec2 Pos = pLocal->Core()->m_Pos;
		const vec2 MouseAim = AimDirection(pCurrentInput->m_TargetX, pCurrentInput->m_TargetY);

		int TargetDir = 0;
		if(MouseAim.x > 0.2f)
			TargetDir = 1;
		else if(MouseAim.x < -0.2f)
			TargetDir = -1;

		CNetObj_PlayerInput Candidate = *pCurrentInput;
		Candidate.m_Direction = TargetDir;
		if(MouseAim.y < -0.5f)
			Candidate.m_Jump = 1;

		const int CandidateSafety = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks,
			g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
			g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);

		if(CandidateSafety > CurrentSafety || (CurrentSafety < CheckTicks && CandidateSafety == SIMULATION_SAFE_CONSTANT))
		{
			Result.m_Input = Candidate;
			Result.m_Active = 1;
			Result.m_SurvivalTicks = (CandidateSafety == SIMULATION_SAFE_CONSTANT) ? CheckTicks : CandidateSafety;
			str_copy(Result.m_aReason, "Pilot navigation");
		}
		else if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
		{
			str_copy(Result.m_aReason, "Player input safe");
		}
		else
		{
			// Try stop or counter steer
			Candidate.m_Direction = (pCurrentInput->m_Direction == 1) ? -1 : (pCurrentInput->m_Direction == -1 ? 1 : 0);
			const int CounterSafety = SimulateCandidate(m_pClient, pWorld, Candidate, CheckTicks,
				g_Config.m_BcAvoidPlayerPrediction, g_Config.m_BcAvoidTileTele, g_Config.m_BcAvoidTileDeath,
				g_Config.m_BcAvoidTileFreeze, g_Config.m_BcAvoidTileUnfreeze, g_Config.m_BcAvoidUnfreezeTicks);
			if(CounterSafety > CurrentSafety)
			{
				Result.m_Input = Candidate;
				Result.m_Active = 1;
				Result.m_SurvivalTicks = CounterSafety;
				str_copy(Result.m_aReason, "Pilot counter steer");
			}
			else
			{
				str_copy(Result.m_aReason, "Pilot searching safe route");
			}
		}

		return Result;
	}

} // namespace Avoid
