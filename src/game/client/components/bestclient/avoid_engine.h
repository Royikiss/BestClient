/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H

#include "avoid_decision.h"

#include <base/vmath.h>

#include <generated/protocol.h>

#include <game/gamecore.h>

#include <deque>
#include <vector>

class CCharacter;
class CCollision;
class CGameClient;
class CGameWorld;

namespace Avoid
{
	class CTileEditor;
}

/* --------------------------------------------------------------------------------------------
 * Avoid decision engine.
 *
 * This is a 1:1 reproduction of the reference client's Avoid (Gores bot) subsystem, see
 * docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md. The reference identifiers are mapped onto the
 * BestClient `bc_avoid_*` prefix; defaults, ranges and semantics are unchanged.
 *
 * Everything the agents need arrives through SSettings / SContext, and every agent returns an
 * AvoidInput. The player input is only replaced when AvoidInput::m_Active is set.
 * ------------------------------------------------------------------------------------------ */
namespace Avoid
{
	// Reported by the forward simulator when the whole lookahead was survived (reference: 0x270f).
	constexpr int SIMULATION_SAFE_CONSTANT = 9999;

	// Ticks the Basic agent looks ahead. Hard-coded on purpose, the reference has no CVar for it.
	constexpr int BASIC_CHECK_TICKS = 6;

	enum
	{
		AGENT_BASIC = 0,
		AGENT_LEGIT = 1,
		AGENT_BLATANT = 2,
		AGENT_FENTBOT = 3,
		AGENT_PILOT = 4,
		NUM_AGENTS = 5,
	};

	// ---------------------------------------------------------------------------------------
	// Settings: one field per Avoid CVar, snapshotted once per tick.
	// ---------------------------------------------------------------------------------------
	struct SSettings
	{
		int m_Agent = AGENT_BASIC;
		bool m_AfkProtection = false;
		int m_AfkTime = 5;
		bool m_PlayerPrediction = true;
		bool m_DrawPath = true;

		// Legit
		int m_LegitDirectionWeight = 170;
		int m_LegitLifespanWeight = 160;
		int m_LegitHookWeight = 260;
		int m_LegitExploration = 4;
		int m_LegitIterations = 100;
		int m_LegitCheckTicks = 6;
		bool m_LegitDirection = true;
		bool m_LegitHook = true;
		bool m_LegitTeles = false;
		bool m_LegitDeath = false;
		bool m_LegitUnfreeze = false;
		int m_LegitUnfreezeTicks = 5;

		// Blatant
		int m_BlatantCheckTicks = 26;
		int m_KickInTicks = 26;
		bool m_BlatantDirection = true;
		bool m_BlatantHook = true;
		bool m_BlatantTeles = false;
		bool m_BlatantDeath = false;
		bool m_BlatantUnfreeze = false;
		int m_BlatantUnfreezeTicks = 26;
		bool m_Nsif = true;
		bool m_TrackPoint = false;
		bool m_SafeAimTracking = false;
		bool m_AutoDrag = false;
		bool m_Aimbot = false;
		int m_AimbotFov = 90;
		int m_AimbotSegments = 5;
		bool m_AutoAim = false;
		bool m_AimAssist = true;

		// Fentbot
		int m_FentQuality = 0;
		bool m_FentAdvanced = false;
		int m_FentTicks = 1000;
		int m_FentTweakerActions = 50;
		int m_FentTweakerTicks = 1;
		int m_FentTweakerDosage = 1;
		bool m_FentLightTile = false;
		int m_FentLightTileRadius = 1;

		// Pilot
		int m_PilotMode = 0;
		int m_PilotPopulation = 2048;
		int m_PilotDepth = 17;
		int m_PilotTopK = 10;
		int m_PilotSequence = 5;

		// Resolved Fentbot preset (filled by ResolveFentPreset), never read from a CVar directly.
		int m_FentActions = 50;
		int m_FentDosage = 1;
		int m_FentHorizon = 1000;
		int m_FentHoldTicks = 1;
	};

	class CNavigator;

	// What the forward simulator treats as a hazard (reference: the extra SimulateCandidate args).
	struct SSimFlags
	{
		bool m_PredictPlayers = true;
		bool m_AvoidFreeze = true;
		bool m_AvoidDeath = false;
		bool m_AvoidTeles = false;
		bool m_AvoidUnfreeze = false;
		int m_UnfreezeTicks = 0;
		// Light freeze support: while this is on, standing on a tile the navigator classified as
		// NAV_LIGHT (a freeze tile with an unfreeze tile inside the light radius) does not count
		// as running into a hazard. Only the Fentbot turns this on.
		bool m_AllowLightFreeze = false;
		const CNavigator *m_pNav = nullptr;
	};

	// Velocity/flow dot product used by the Fentbot fitness (reference constant 1750.0f).
	struct SFlowField
	{
		const vec2 *m_pDir = nullptr;
		int m_Width = 0;
		int m_Height = 0;
		float m_Scale = 0.0f;
	};

	// Filled once per tick by CAvoid, consumed by every agent.
	struct SContext
	{
		int m_Tick = 0;
		int m_LocalClientId = -1;
		CNetObj_PlayerInput m_Input{};
		SSettings m_Settings{};
		// Goal set and movement restriction of the two planners, owned by the component.
		const CTileEditor *m_pTileEditor = nullptr;
	};

	struct SAimMarker
	{
		bool m_Valid = false;
		vec2 m_Pos = vec2(0.0f, 0.0f);
	};

	// Agent result (reference struct AvoidInput, extended with the data the HUD/overlay needs).
	struct AvoidInput
	{
		CNetObj_PlayerInput m_Input{};
		int m_Active = 0; // 0: keep the player input, 1: the bot intervened
		int m_SurvivalTicks = 0;
		bool m_UsedFallback = false;
		std::vector<vec2> m_vPath; // predicted path, only filled while bc_avoid_draw_path is on
		SAimMarker m_TrackPoint{};
		SAimMarker m_AimTarget{};
		char m_aReason[64] = "";
	};

	// Resolves the Fentbot quality preset into the concrete search shape (reference 0x1403356ba).
	// When the advanced switch is on the custom CVar values win, otherwise the preset wins.
	void ResolveFentPreset(SSettings &Set);

	// ---------------------------------------------------------------------------------------
	// Shared helpers
	// ---------------------------------------------------------------------------------------
	vec2 AimDirection(int TargetX, int TargetY);
	bool AimTargetsFrom(vec2 Dir, int *pTargetX, int *pTargetY);
	bool IsHookable(CCollision *pCollision, vec2 From, vec2 Dir, float HookLength, vec2 *pOutPos = nullptr);

	// Blatant (reference spec 6.4): nearest TILE_UNFREEZE inside RadiusTiles, breadth first.
	bool FindNearestUnfreezeTile(CCollision *pCollision, vec2 From, int RadiusTiles, vec2 *pOutCentre);

	// ---------------------------------------------------------------------------------------
	// Pre-activation pipeline (reference spec 5). Both functions are the heavy half of the
	// dispatcher: they run after the five environment gates of CAvoid::ApplyInput allowed it.
	// ---------------------------------------------------------------------------------------

	// Reference spec 5.5: the 10 tick baseline probe over the player's own input. Returns
	// SIMULATION_SAFE_CONSTANT when the input survives at least PROBE_SAFE_TICKS of the window -
	// the dispatcher then skips the agents completely, which is what keeps a safe player at 0%
	// wake-ups and full frame rate - and the raw survival count when it does not.
	int RunLightweightProbe(CGameClient *pClient, CGameWorld *pBaseWorld, const CNetObj_PlayerInput &Input, const SSettings &Set);

	// Reference spec 5.6: sweeps the field of view around the player's crosshair in
	// krx_avoid_tile_aimbot_segments steps and, when the switch rules allow it, locks the crosshair
	// onto the scanned ray that survived longest. Returns true once the sweep ran, which is what
	// tells the Blatant aim layer that the crosshair is already the winner of that scan; whether
	// the aim was actually rewritten can be seen by comparing m_TargetX/m_TargetY around the call.
	bool RunSectorScan(CGameClient *pClient, CGameWorld *pBaseWorld, CNetObj_PlayerInput *pInput, const SSettings &Set, SAimMarker *pOutAim = nullptr);

	// ---------------------------------------------------------------------------------------
	// v5.1 airborne rescue mechanisms (reference spec 5.7 - 5.9)
	//
	// All three are world probes plus a lookahead, so they live here instead of in the pure
	// decision header. Every one of them is entirely self contained: it returns false (and leaves
	// pOutInput alone) whenever it has no business touching the input, so a caller can chain them
	// in the reference priority order without checking anything itself.
	// ---------------------------------------------------------------------------------------

	// Reference spec 7.2: the air-jump axis only exists while the tee is off the ground and still
	// owns its air jump (bit 2 of m_Jumped is the "all jumps used up" flag, not bit 1 - that one
	// only means a jump was made on the currently held key).
	bool CanUseAirJump(CGameWorld *pWorld, int LocalClientId);

	// Reference spec 5.7 (assembly 0x1403286f0 - 0x1403289e0): a tee hanging from a hook swings
	// around its anchor. Holding the hook drags it into the freeze below the pendulum's low point,
	// letting go at the tangent converts the swing into a flight that clears the pool. Both
	// branches are simulated and the hook key is taken away from the player only when that is
	// measurably better. Returns true when the release was forced.
	bool CheckPreemptiveHookRelease(CGameClient *pClient, CGameWorld *pWorld, const CNetObj_PlayerInput &CurrentInput, CNetObj_PlayerInput *pOutInput, int CheckTicks, const SSimFlags &Flags);

	// Reference spec 5.8: is there room above Pos for an air jump, i.e. no ceiling closer than one
	// tile and nothing lethal or freezing overhead?
	bool CheckHeadroomClearance(CCollision *pCollision, vec2 Pos, float RequiredHeight);

	// Reference spec 5.8: spends the tee's air jump when it is falling into a hazard, the headroom
	// allows it and the jump buys at least AIR_JUMP_MIN_GAIN_TICKS over doing nothing.
	bool TryEmergencyAirJump(CGameClient *pClient, CGameWorld *pWorld, const CNetObj_PlayerInput &CurrentInput, CNetObj_PlayerInput *pOutInput, int CheckTicks, const SSimFlags &Flags);

	// Reference spec 5.9: ignores the crosshair completely and casts the five upper hemisphere
	// escape rays up to the maximum hook reach. The first ray with a hookable surface is hooked,
	// with the horizontal key leaning into the pull when that survives longer.
	bool TryEmergencyWallCeilingHook(CGameClient *pClient, CGameWorld *pWorld, const CNetObj_PlayerInput &CurrentInput, CNetObj_PlayerInput *pOutInput, int CheckTicks, const SSimFlags &Flags);

	// ---------------------------------------------------------------------------------------
	// Forward simulator (reference func_0x00014036a8d0)
	// ---------------------------------------------------------------------------------------

	// Clones pBaseWorld, injects CandidateInput for CheckTicks and reports how many ticks were
	// survived, or SIMULATION_SAFE_CONSTANT when the whole lookahead was survived.
	int SimulateCandidate(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput &CandidateInput,
		int CheckTicks,
		const SSimFlags &Flags,
		std::vector<vec2> *pvPath = nullptr);

	// Same, but injects one input per tick and repeats the last one for the remaining ticks.
	int SimulatePlan(
		CGameClient *pClient,
		CGameWorld *pBaseWorld,
		const CNetObj_PlayerInput *pInputs,
		int NumInputs,
		int CheckTicks,
		const SSimFlags &Flags,
		const SFlowField *pFlow = nullptr,
		float *pOutFlowScore = nullptr,
		std::vector<vec2> *pvPath = nullptr,
		vec2 *pOutEndPos = nullptr);

	// Resumable variant of SimulatePlan. The planners (Fentbot, Pilot) use it so that a long
	// evaluation can be spread over many ticks without stalling the client: every Step() advances
	// at most MaxSteps simulated ticks and reports whether the candidate is done.
	class CSimSession
	{
	public:
		struct SOutcome
		{
			int m_SurvivalTicks = SIMULATION_SAFE_CONSTANT;
			float m_FlowScore = 0.0f;
			vec2 m_EndPos = vec2(0.0f, 0.0f);
		};

		// Starts a new candidate. Returns false when there is nothing to simulate.
		bool Begin(
			CGameClient *pClient,
			CGameWorld *pBaseWorld,
			const CNetObj_PlayerInput *pInputs,
			int NumInputs,
			int CheckTicks,
			const SSimFlags &Flags,
			const SFlowField *pFlow = nullptr);
		// Advances at most MaxSteps simulated ticks and returns how many were taken.
		int Step(int MaxSteps);
		bool Finished() const { return m_Finished; }
		const SOutcome &Outcome() const { return m_Outcome; }
		void Abort();

	private:
		bool HitHazard() const;

		CGameWorld *m_pWorld = nullptr;
		CCharacter *m_pChar = nullptr;
		CCollision *m_pCollision = nullptr;
		int m_LocalClientId = -1;
		const CNetObj_PlayerInput *m_pInputs = nullptr;
		int m_NumInputs = 0;
		int m_Horizon = 0;
		int m_Tick = 0;
		bool m_Finished = true;
		SSimFlags m_Flags{};
		const SFlowField *m_pFlow = nullptr;
		SOutcome m_Outcome{};
	};

	// ---------------------------------------------------------------------------------------
	// Navigable grid + flow field shared by Fentbot and Pilot.
	//
	// The grid is built incrementally: every Update() call spends at most MaxWork tile visits so
	// that a whole map never blocks a frame. Rebuild() invalidates it (map change, settings
	// change, tile edit). Light freeze tiles become navigable when an unfreeze tile is close enough,
	// which is the reference "light tile" rule.
	//
	// The goal set and the movement restriction come from the tile editor (reference spec 9): when
	// the editor has finish tiles they are the goals, and when it has tunnel tiles the search is
	// confined to them. With an empty editor the planner falls back to the map's own TILE_FINISH
	// and, if the map has none, to the unfreeze tiles.
	// ---------------------------------------------------------------------------------------
	class CNavigator
	{
	public:
		enum
		{
			NAV_BLOCKED = 0,
			NAV_OPEN = 1,
			NAV_GOAL = 2,
			NAV_LIGHT = 3, // freeze tile that is crossable because an unfreeze tile is nearby
		};

		bool Ready() const { return m_Ready; }
		bool Building() const { return m_Building; }
		int Width() const { return m_Width; }
		int Height() const { return m_Height; }
		int GoalTiles() const { return m_GoalTiles; }
		const std::vector<vec2> &Flow() const { return m_vFlow; }
		const std::vector<unsigned char> &Grid() const { return m_vGrid; }
		vec2 FlowAt(vec2 Pos) const;
		int DistanceAt(vec2 Pos) const;
		// True while Pos is on a freeze tile that the light-tile rule made crossable.
		bool IsLightTile(vec2 Pos) const;

		void Reset();
		// Starts (or restarts) a build for the given map. The light-tile rule is a property of the
		// build, not of every query, so it is fixed here and reported back through LightTile() and
		// LightRadius() to let the caller notice a settings change. The editor pointer may be null;
		// it is only read while the grid is built, so it has to outlive the build.
		void Rebuild(CCollision *pCollision, bool LightTile, int LightRadius, const CTileEditor *pEditor = nullptr);
		// Advances the build, returns true once the field is usable again.
		bool Update(CCollision *pCollision, int MaxWork);
		bool LightTile() const { return m_LightTile; }
		int LightRadius() const { return m_LightRadius; }
		// Revision of the tile editor the running grid was built from.
		unsigned EditorRevision() const { return m_EditorRevision; }

	private:
		enum EPhase
		{
			PHASE_IDLE = 0,
			PHASE_CLASSIFY,
			PHASE_MARK_LIGHT,
			PHASE_FLOOD,
			PHASE_GRADIENT,
		};

		// Light-tile seeds carry their BFS depth next to the tile index.
		void ClassifyTiles(CCollision *pCollision);
		bool StepMarkLight(CCollision *pCollision);
		bool StepFlood();
		bool BuildGradient();

		std::vector<unsigned char> m_vGrid;
		std::vector<unsigned char> m_vLightSeen;
		std::vector<int> m_vDist;
		std::vector<vec2> m_vFlow;
		std::vector<int> m_vQueue;
		std::deque<std::pair<int, int>> m_LightQueue;
		int m_Width = 0;
		int m_Height = 0;
		int m_QueueHead = 0;
		int m_QueueTail = 0;
		int m_GoalTiles = 0;
		int m_Cursor = 0;
		int m_Phase = PHASE_IDLE;
		bool m_LightTile = false;
		int m_LightRadius = 0;
		const CTileEditor *m_pEditor = nullptr;
		unsigned m_EditorRevision = 0;
		bool m_Ready = false;
		bool m_Building = false;
	};

	// ---------------------------------------------------------------------------------------
	// Agent base (reference BLAgent)
	// ---------------------------------------------------------------------------------------
	// The world the agents predict in: the Fast Practice sandbox when it is running, otherwise
	// the predicted world the client keeps in sync with the server.
	CGameWorld *GetActiveWorld(CGameClient *pClient);

	class BLAgent
	{
	protected:
		CGameClient *m_pClient = nullptr;

	public:
		BLAgent(CGameClient *pClient) :
			m_pClient(pClient) {}
		virtual ~BLAgent() = default;

		virtual AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) = 0;
		virtual void OnRender() {}
		virtual void OnReset() {}
		// True once the planner's navigable grid is usable. Only the two planners use one.
		virtual bool NavigatorReady() const { return false; }
	};

	// 1. Basic (reference spec 5): fixed 6 tick lookahead, direction keys only.
	class CBasicAgent : public BLAgent
	{
	public:
		CBasicAgent(CGameClient *pClient) :
			BLAgent(pClient) {}
		AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) override;
	};

	// 2. Legit (reference spec 7): UCT MCTS with human-likeness weights.
	class CLegitAgent : public BLAgent
	{
	public:
		CLegitAgent(CGameClient *pClient) :
			BLAgent(pClient) {}
		AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) override;
	};

	// 3. Blatant (reference spec 6): kick-in hysteresis, greedy search, NSIF, aimbot layer.
	class CBlatantAgent : public BLAgent
	{
	private:
		bool m_TrackPointValid = false;
		vec2 m_TrackPointPos = vec2(0.0f, 0.0f);
		std::vector<CNetObj_PlayerInput> m_SavedSafeSequence;

	public:
		CBlatantAgent(CGameClient *pClient) :
			BLAgent(pClient) {}
		void OnReset() override;
		AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) override;
	};

	// 4. Fentbot (reference spec 8): flow field pathfinding plus a genetic input tweaker.
	class CFentbotAgent : public BLAgent
	{
	private:
		CNavigator m_Nav;
		// One search round works on a frozen snapshot of the world and is spread over ticks.
		CGameWorld *m_pSnapshot = nullptr;
		bool m_SnapshotValid = false;
		std::vector<CNetObj_PlayerInput> m_vCandidates; // m_CandidateCount genomes of m_CandidateLength
		std::vector<float> m_vFitness;
		int m_CandidateCount = 0;
		int m_CandidateLength = 0;
		int m_CandidateIndex = 0;
		int m_Generation = 0;
		int m_Cooldown = 0;
		// The session holds a pointer into m_Nav.Flow(), so it must live as long as the session.
		SFlowField m_SessionFlow{};
		CSimSession m_Session;

		// The plan currently being executed. `m_PlanIndex` only ever moves forward, so a plan is
		// never restarted from its first tick: that would re-trigger the same jump or hook over
		// and over. A published genome is staged and swapped in together with its index, so the
		// tail of an old plan and the head of a new one can never be driven back to back.
		std::vector<CNetObj_PlayerInput> m_vPlan;
		std::vector<CNetObj_PlayerInput> m_vPendingPlan;
		bool m_PlanPending = false;
		int m_PlanIndex = 0;
		float m_BestFitness = -1e30f; // best fitness of the running round, shared by all generations

		void SeedGeneration(const SSettings &Set, const SContext &Ctx);
		void Breed(const SSettings &Set);

	public:
		CFentbotAgent(CGameClient *pClient) :
			BLAgent(pClient) {}
		~CFentbotAgent() override;
		void OnReset() override;
		AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) override;
		bool NavigatorReady() const override { return m_Nav.Ready(); }
	};

	// 5. Pilot: population based sequence search with three navigation modes.
	class CPilotAgent : public BLAgent
	{
	private:
		CNavigator m_Nav;
		CGameWorld *m_pSnapshot = nullptr;
		bool m_SnapshotValid = false;
		std::vector<CNetObj_PlayerInput> m_vPopulation; // m_IndividualCount genomes of m_IndividualDepth
		std::vector<float> m_vFitness;
		std::vector<CNetObj_PlayerInput> m_vNext; // breeding scratch buffer, swapped into m_vPopulation
		int m_IndividualCount = 0;
		int m_IndividualDepth = 0;
		int m_Individual = 0;
		int m_Rng = 0x1f123bb5;
		// The session holds a pointer into m_Nav.Flow(), so it must live as long as the session.
		SFlowField m_SessionFlow{};
		CSimSession m_Session;

		// See CFentbotAgent: the plan index only moves forward and a new plan is adopted from the
		// newest completed generation.
		std::vector<CNetObj_PlayerInput> m_vPlan;
		int m_PlanIndex = 0;
		int m_PlanTick = 0;
		int m_PlanEpoch = 0; // bumped whenever a generation completes
		int m_HeldEpoch = 0; // the epoch the held plan came from
		vec2 m_Target = vec2(0.0f, 0.0f);
		bool m_TargetValid = false;

		int NextRand();
		void SeedPopulation(const SSettings &Set, const SContext &Ctx);
		void Breed(const SSettings &Set);

	public:
		CPilotAgent(CGameClient *pClient) :
			BLAgent(pClient) {}
		~CPilotAgent() override;
		void OnReset() override;
		AvoidInput GetAction(const SContext &Ctx, CGameWorld *pWorld) override;
		bool NavigatorReady() const override { return m_Nav.Ready(); }
	};

} // namespace Avoid

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
