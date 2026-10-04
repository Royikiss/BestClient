/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H

#include <base/vmath.h>
#include <generated/protocol.h>
#include <game/gamecore.h>

#include <cstdint>
#include <vector>

class CCollision;
class CGameClient;
class CGameWorld;

namespace Avoid
{
	constexpr float TILE_SIZE = 32.0f;
	constexpr float HAZARD_CORNER_PROBE = CCharacterCore::PhysicalSize() / 3.0f;
	constexpr int MAX_SIM_TICKS = 50;
	constexpr int SIMULATION_SAFE_CONSTANT = 9999;
	constexpr float SENSING_VERTICAL_FALLBACK = 0.6f;

	// Hazard classification bits
	enum
	{
		HAZ_NONE = 0,
		HAZ_DEATH = 1 << 0,
		HAZ_FREEZE = 1 << 1,
		HAZ_DEEP = 1 << 2,
		HAZ_LIVE = 1 << 3,
		HAZ_UNFREEZE = 1 << 4,
		HAZ_TELE = 1 << 5,
		HAZ_SELF = 1 << 6,
		HAZ_ANY = HAZ_DEATH | HAZ_FREEZE | HAZ_DEEP | HAZ_LIVE | HAZ_UNFREEZE | HAZ_TELE,
	};

	enum
	{
		AGENT_BASIC = 0,
		AGENT_LEGIT = 1,
		AGENT_BLATANT = 2,
		AGENT_FENTBOT = 3,
		AGENT_PILOT = 4,
		NUM_AGENTS = 5,
	};

	struct SSettings
	{
		int m_Agent = 0;
		bool m_DirectionAssist = true;
		bool m_HookAssist = true;
		int m_CheckTicks = 26;
		int m_KickInTicks = 20;
		int m_Quality = 24;
		int m_Randomness = 30;
		int m_DirectionWeight = 100;
		int m_HookWeight = 100;
		int m_LifeWeight = 150;
		bool m_TileDeath = true;
		bool m_TileFreeze = true;
		bool m_TileUnfreeze = false;
		int m_UnfreezeTicks = 26;
		bool m_TileTele = false;
		bool m_PlayerPrediction = true;
		bool m_Nsif = true;
		bool m_AfkProtect = true;
		int m_AfkTime = 60;
		bool m_TrackPoint = false;
		bool m_SafeAimTracking = true;
		bool m_AutoDrag = false;
		bool m_Aimbot = false;
		int m_AimbotMode = 0;
		int m_AimbotSegments = 24;
		int m_AimbotFov = 90;
		bool m_SensingGate = false;
		float m_SensingRadius = 6.0f;
	};

	struct SThreat
	{
		int m_Flags = HAZ_NONE;
		vec2 m_NearestPos = vec2(0.0f, 0.0f);
		float m_NearestDistPx = 0.0f;
		bool m_HasNearest = false;
		int m_SensedTiles = 0;
		int m_HazardTiles = 0;
		bool m_OnHazard = false;
	};

	struct SContext
	{
		int m_Tick = 0;
		int m_ClientId = -1;
		bool m_Dummy = false;
		CCharacterCore m_Core{};
		CNetObj_PlayerInput m_Input{};
		bool m_PlayerInputSafe = false;
		SThreat m_Threat{};
		SSettings m_Settings{};
	};

	struct SInputPlan
	{
		bool m_Override = false;
		bool m_UsedFallback = false;
		CNetObj_PlayerInput m_Input{};
		int m_SafeTicks = 0;
		int m_InputSafeTicks = 0;
		int m_ScannedTicks = 0;
		int m_Candidates = 0;
		float m_Score = 0.0f;
		float m_CostMs = 0.0f;
		char m_aReason[64] = "";
	};

	// AvoidInput output structure (KRX spec 3.1)
	struct AvoidInput
	{
		CNetObj_PlayerInput m_Input{};
		int m_Active = 0; // 0: keep player input, 1: bot intervened
		int m_SurvivalTicks = 0;
		bool m_UsedFallback = false;
		char m_aReason[64] = "";
	};

	// Sensing helpers (shared with CAvoid HUD/visuals)
	int ClassifyTile(int Tile);
	int ClassifyPoint(CCollision *pCollision, vec2 Pos);
	float DistanceToTileBox(vec2 Pos, int TileX, int TileY);
	vec2 TileBoxDelta(vec2 Pos, int TileX, int TileY);
	float SensingVerticalFactor(const CCharacterCore &Core, const SSettings &Set);
	int HazardMask(const SSettings &Set);
	bool IsRelevantHazard(const SSettings &Set, int Flags);
	SThreat ScanThreat(CCollision *pCollision, const SSettings &Set, const CCharacterCore &Core);

	// Math & aim helpers
	vec2 AimDirection(int TargetX, int TargetY);
	bool AimTargetsFrom(vec2 Dir, int *pTargetX, int *pTargetY);
	float AimAngleDeg(vec2 Dir);
	bool IsHookable(CCollision *pCollision, vec2 From, vec2 Dir, float HookLength, vec2 *pOutPos = nullptr, float *pOutDist = nullptr);

	// Forward simulator (KRX spec 4.1)
	int SimulateCandidate(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput &CandidateInput,
		int CheckTicks,
		bool PredictPlayers = true,
		bool AvoidTeles = false,
		bool AvoidDeath = true,
		bool AvoidFreeze = true,
		bool AvoidUnfreeze = false,
		int UnfreezeTicks = 0);

	// Base Agent interface (KRX spec 3.2)
	class BLAgent
	{
	protected:
		CGameClient *m_pClient = nullptr;
		CGameWorld *GetBaseWorld() const;

	public:
		BLAgent(CGameClient *pClient) : m_pClient(pClient) {}
		virtual ~BLAgent() = default;

		virtual AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) = 0;
		virtual void OnRender() {}
		virtual void OnReset() {}
		virtual void OnUpdateCamera(vec2 &TargetPos) {}
	};

	// 1. Basic Agent (KRX spec 5)
	class CBasicAgent : public BLAgent
	{
	public:
		CBasicAgent(CGameClient *pClient) : BLAgent(pClient) {}
		AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override;
	};

	// 2. Blatant Agent (KRX spec 6)
	class CBlatantAgent : public BLAgent
	{
	private:
		bool m_TrackPointValid = false;
		vec2 m_TrackPointPos = vec2(0.0f, 0.0f);
		vec2 m_TrackPointDir = vec2(0.0f, 0.0f);
		std::vector<CNetObj_PlayerInput> m_SavedSafeSequence;

	public:
		CBlatantAgent(CGameClient *pClient) : BLAgent(pClient) {}
		void OnReset() override;
		void OnRender() override;
		AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override;

		bool TrackPointValid() const { return m_TrackPointValid; }
		vec2 TrackPointPos() const { return m_TrackPointPos; }
	};

	// 3. Legit Agent (KRX spec 7)
	class CLegitAgent : public BLAgent
	{
	public:
		CLegitAgent(CGameClient *pClient) : BLAgent(pClient) {}
		AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override;
	};

	// 4. Fentbot Agent (KRX spec 8)
	class CFentbotAgent : public BLAgent
	{
	private:
		int m_LastMapWidth = 0;
		int m_LastMapHeight = 0;
		std::vector<vec2> m_vFlowField;
		bool m_FlowFieldCalculated = false;

		void CalculateFlowField(CCollision *pCollision);

	public:
		CFentbotAgent(CGameClient *pClient) : BLAgent(pClient) {}
		void OnReset() override;
		void OnRender() override;
		AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override;
	};

	// 5. Pilot Agent
	class CPilotAgent : public BLAgent
	{
	public:
		CPilotAgent(CGameClient *pClient) : BLAgent(pClient) {}
		void OnReset() override;
		void OnRender() override;
		AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override;
	};

} // namespace Avoid

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
