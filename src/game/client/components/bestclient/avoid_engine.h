/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H

#include <base/vmath.h>

#include <generated/protocol.h>

#include <game/gamecore.h>
#include <game/prng.h>

#include <cstdint>

class CCollision;

/* ---------------------------------------------------------------------------------------------
 * The decision engine of the Avoid module ("Gores bot").
 *
 * Stage 2 (Basic) lived entirely inside CAvoid, which made it untestable: CAvoid is a
 * CComponent and only links into the client, while the testrunner links game-shared. This header
 * therefore holds everything the engine needs that is *not* client specific:
 *
 *   - the data contract (SSettings / SThreat / SContext / SInputPlan, unchanged from stage 2),
 *   - the hazard sensing layer (the single implementation of the probe rules that CAvoid
 *     re-exports, so "the sensor says dangerous" and "the physics says dangerous" cannot drift),
 *   - the shared forward simulator (`SimulateFixed`) - stage 2's CAvoid::SimulateInput() is a thin
 *     wrapper around it, so every agent keeps running the exact same physics recipe,
 *   - CPlanner, the stage 3 Legit agent: extended candidate space (direction x jump x hook),
 *     UCT search over the candidate actions and player prediction.
 *
 * Nothing in here may touch CGameClient, the console, cvars or the UI: it takes collision,
 * world, teams and a list of player snapshots and answers with a plan.
 * ------------------------------------------------------------------------------------------- */
namespace Avoid
{
	// 1 tile = 32 px, the unit the sensing layer and the HUD work in.
	constexpr float TILE_SIZE = 32.0f;

	// Death tiles are probed at the four corners of a smaller box, exactly like
	// CCharacter::HandleSkippableTiles() does. CCharacterCore::PhysicalSize() is 28px,
	// so the probe offset is 28 / 3.
	constexpr float HAZARD_CORNER_PROBE = CCharacterCore::PhysicalSize() / 3.0f;

	// Lookahead limit. `bc_avoid_check_ticks` / `bc_avoid_unfreeze_ticks` are clamped to this.
	constexpr int MAX_SIM_TICKS = 50;

	// Root actions of the search: direction (3) x jump (2) x hook (3) = 18, with head room.
	constexpr int MAX_ROOT_ACTIONS = 24;

	// `bc_avoid_quality` is an iteration count, clamped to MAX_ITERATIONS.
	constexpr int MAX_ITERATIONS = 200;

	// Other players fed into the clone as moving obstacles. Everything beyond this cap keeps
	// being ignored by the prediction (documented deviation).
	constexpr int MAX_SHADOW_PLAYERS = 8;

	// The sensing reach is an *ellipse*, not a circle: what matters is how long the tee would need
	// to get to a hazard, and gravity limits vertical travel far more than the ground control speed
	// limits horizontal travel. The vertical radius is `horizontal * factor`, where the factor is
	// derived from the map tuning (0.5 * gravity * ticks^2 over ground_control_speed * ticks, so a
	// `tune` map with different gravity gets a different ellipse). 0.6 is the fallback for a map
	// without a usable tuning.
	constexpr float SENSING_VERTICAL_FALLBACK = 0.6f;

	// Hazard classification bits. A tile can carry several of them.
	enum
	{
		HAZ_NONE = 0,
		HAZ_DEATH = 1 << 0, // TILE_DEATH and death switches
		HAZ_FREEZE = 1 << 1, // TILE_FREEZE
		HAZ_DEEP = 1 << 2, // TILE_DFREEZE
		HAZ_LIVE = 1 << 3, // TILE_LFREEZE
		HAZ_UNFREEZE = 1 << 4, // TILE_UNFREEZE (optional hazard, off by default)
		HAZ_TELE = 1 << 5, // any teleport tile (optional hazard, off by default)
		HAZ_SELF = 1 << 6, // the tee itself is frozen / deep frozen right now
		HAZ_ANY = HAZ_DEATH | HAZ_FREEZE | HAZ_DEEP | HAZ_LIVE | HAZ_UNFREEZE | HAZ_TELE,
	};

	// Snapshot of the cvars, refreshed once per decision so that the engine always
	// reads a stable configuration.
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
		int m_AimbotMode = 0; // 0 = auto aim, 1 = aim assist
		int m_AimbotSegments = 24;
		int m_AimbotFov = 90;
		// Sensing radius in tiles, half tile steps (`bc_avoid_sensing_radius` is stored in half
		// tiles, so 12 in the config is 6.0 here). Measured from the tee to the hazard tile box.
		float m_SensingRadius = 6.0f;
	};

	// Result of the hazard scan around the controlled tee.
	struct SThreat
	{
		int m_Flags = HAZ_NONE; // union of all hazard bits seen on the tee itself
		vec2 m_NearestPos = vec2(0.0f, 0.0f); // world position of the closest hazard tile centre
		float m_NearestDistPx = 0.0f; // distance from the tee to the closest hazard tile box
		bool m_HasNearest = false;
		int m_SensedTiles = 0; // tiles examined by the scan
		int m_HazardTiles = 0; // tiles inside the sensing radius that are hazardous
		bool m_OnHazard = false; // the tee is standing inside a hazard right now
	};

	// Everything the decision engine is allowed to look at.
	struct SContext
	{
		int m_Tick = 0; // client tick this decision belongs to
		int m_ClientId = -1;
		bool m_Dummy = false; // the controlled tee is the dummy connection
		CCharacterCore m_Core{}; // live physics state of the controlled tee
		CNetObj_PlayerInput m_Input{}; // the input the player is about to send
		bool m_PlayerInputSafe = false; // filled by the engine (stage 2)
		SThreat m_Threat{};
		SSettings m_Settings{};
	};

	// What the decision engine answers with.
	struct SInputPlan
	{
		bool m_Override = false; // true => replace the outgoing input with m_Input
		bool m_UsedFallback = false; // true => NSIF: nothing was fully safe
		CNetObj_PlayerInput m_Input{}; // must be copied from Ctx.m_Input, never built from scratch
		int m_SafeTicks = 0; // how long the chosen plan survives
		int m_ScannedTicks = 0; // simulation depth actually reached
		int m_Candidates = 0; // plans evaluated
		float m_Score = 0.0f; // score of the winning plan (higher is better)
		float m_CostMs = 0.0f; // wall clock spent in the engine
		char m_aReason[64] = ""; // short human readable explanation for the HUD
	};

	// ---------------------------------------------------------------------------------------
	// Hazard sensing (stage 1 rules, single implementation shared with CAvoid)
	// ---------------------------------------------------------------------------------------
	int ClassifyTile(int Tile);
	int ClassifyPoint(CCollision *pCollision, vec2 Pos);
	// Distance from a point to the box of one tile. Shared by the sensor scan and the world
	// overlay, so "what the agent can see" and "what the overlay highlights" cannot drift apart.
	float DistanceToTileBox(vec2 Pos, int TileX, int TileY);
	// The same distance split per axis (0 inside the box on that axis). This is what the elliptical
	// sensing reach is built from: see SENSING_VERTICAL_FALLBACK below.
	vec2 TileBoxDelta(vec2 Pos, int TileX, int TileY);
	// Vertical reach of the sensing ellipse as a fraction of the horizontal one, derived from the
	// map tuning (see SENSING_VERTICAL_FALLBACK). Exported so the world overlay draws the same
	// ellipse the sensor uses.
	float SensingVerticalFactor(const CCharacterCore &Core, const SSettings &Set);
	int HazardMask(const SSettings &Set);
	bool IsRelevantHazard(const SSettings &Set, int Flags);
	SThreat ScanThreat(CCollision *pCollision, const SSettings &Set, const CCharacterCore &Core);

	// ---------------------------------------------------------------------------------------
	// Movement state gate (stage 3, TAS document 3.11.7 red line)
	//
	// The avoid agent is allowed to rewrite the movement keys of the tee the player controls, but
	// only to stop it from dying. While the tee is in a two player flight (hammer fly / deep fly)
	// or on a jetpack, "the movement key" is not a walking key at all - nudging it there is what
	// the TAS red line forbids, so those states are "hands off" by default.
	// ---------------------------------------------------------------------------------------
	enum
	{
		MOVE_NORMAL = 0,
		MOVE_JETPACK = 1 << 0, // CCharacterCore::m_Jetpack
		MOVE_HOOKED_PLAYER = 1 << 1, // the tee is hooked onto another tee and is being dragged
		MOVE_FLY_HAMMER = 1 << 2, // the client reported an active hammer fly / deep fly
	};
	int ClassifyMovement(const CCharacterCore &Core, bool FlyHammer);

	// ---------------------------------------------------------------------------------------
	// Forward simulation
	// ---------------------------------------------------------------------------------------
	struct SPlayerSnapshot
	{
		int m_Id = -1; // client id, needed for the team checks inside the core physics
		CCharacterCore m_Core{};
	};

	struct SEnvironment
	{
		CCollision *m_pCollision = nullptr;
		// Live world and teams core. Required for the switch (door) state and for the team checks
		// of the player collision; pass them whenever they exist.
		CWorldCore *m_pWorld = nullptr;
		CTeamsCore *m_pTeams = nullptr;
		int m_LocalId = -1; // client id of the controlled tee (-1 when unknown, e.g. in tests)
		bool m_PredictPlayers = false;
		int m_NumPlayers = 0;
		SPlayerSnapshot m_aPlayers[MAX_SHADOW_PLAYERS]{};
	};

	// Clone state of one rollout: the simulated tee plus the private world that carries the
	// predicted other players. Held by value in CPlanner (never copied: the private world points
	// into the shadow array) and on the stack by SimulateFixed().
	struct SSimState
	{
		CCharacterCore m_Core;
		CWorldCore m_World;
		CCharacterCore m_aShadows[MAX_SHADOW_PLAYERS];
		int m_NumShadows = 0;
		bool m_Deferred = false;
		CCollision *m_pCollision = nullptr;

		void Init(const SContext &Ctx, const SEnvironment &Env);
		void Step(const CNetObj_PlayerInput &Input);
	};

	// The shared simulator: clone the core, advance it with the real DDNet core physics and the
	// map tuning, query ClassifyPoint() after every step. `pEnv` may be null, in which case the
	// clone gets no world at all (the stage 2 configuration). With an environment that carries
	// player snapshots the clone also runs TickDeferred() against a private world containing
	// those players. Returns the number of ticks the input survives.
	int SimulateFixed(const SContext &Ctx, const CNetObj_PlayerInput &Input, int MaxTicks,
		const SEnvironment *pEnv, vec2 *pOutPos = nullptr, vec2 *pOutVel = nullptr);

	// ---------------------------------------------------------------------------------------
	// The Legit (stage 3) agent: UCT search over direction x jump x hook.
	// ---------------------------------------------------------------------------------------
	class CPlanner
	{
	public:
		SInputPlan Plan(const SContext &Ctx, const SEnvironment &Env);

	private:
		// One candidate first action plus everything the search learned about it.
		struct SRoot
		{
			CNetObj_PlayerInput m_Input{};
			int m_Direction = 0;
			int m_Jump = 0;
			int m_Hook = 0;
			bool m_Available = true; // false => the action exists but is held back (see Plan())
			int m_Deviation = 0; // weighted distance from the player's own input
			int m_CanonicalSafe = 0; // survival of the fixed (unrandomised) sequence
			int m_BestSafe = 0; // best survival any rollout from this action reached
			int m_SafeSum = 0; // sum of all rollout survivals (the MCTS return)
			int m_Visits = 0;
		};

		SRoot m_aRoots[MAX_ROOT_ACTIONS];
		int m_NumRoots = 0;
		CPrng m_Prng;

		// The clone of this decision, set up once per Plan() and re-used by every rollout. It has
		// to stay a member: `m_World.m_apCharacters[]` points into its shadow array.
		SSimState m_Sim;
		CCharacterCore m_InitCore;
		CCharacterCore m_aInitShadows[MAX_SHADOW_PLAYERS];

		void BuildRoots(const SContext &Ctx);
		// Which of the candidate actions may actually be taken this tick. Called after the player's
		// own input was simulated, because the jump is a last resort that waits for the critical
		// moment (`JUMP_URGENCY_TICKS`) and never fires while the hook is the thing to solve.
		void UpdateAvailability(const SContext &Ctx, int PlayerSafe);
		bool JumpEngaged(const SContext &Ctx) const;
		int Rollout(int RootIndex, bool Canonical, int Horizon, const SSettings &Set, CCollision *pCollision);
		int SelectRoot(int TotalVisits, float Exploration, const SSettings &Set) const;
		// The value the search optimises while it decides where to look next: the mean survival of
		// the rollouts through this action. Exploration lowers it, which is the MCTS return.
		float MeanScore(const SRoot &Root, const SSettings &Set) const;
		// The value the plan is chosen by: survival of the fixed sequence (what the tee actually
		// gets if it keeps the action) traded against how much of the player's input it throws
		// away. Deliberately *not* the mean: an exploratory rollout must never be able to talk the
		// agent into an action whose own continuation dies earlier.
		float RankingScore(const SRoot &Root, const SSettings &Set) const;
		int FindBest(const SSettings &Set, int PlayerSafe) const;
		void DescribePlan(SInputPlan &Plan, const SContext &Ctx, bool SafeEnough, const SRoot &Best) const;
	};
} // namespace Avoid

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_ENGINE_H
