// Tests for the Legit agent of the Avoid module (stage 3): CAvoid's extended candidate space,
// the UCT search and the player prediction.
//
// Why this file exists: Legit is the first agent that is allowed to release the hook, to jump and
// to predict other players, and every one of those capabilities can kill the player if the
// decision rule is wrong. The rules are therefore pinned down here instead of by hand in game:
//
//   1. holding a hook that drags the tee into a hazard has to be released - but only when the
//      release actually survives the whole `bc_avoid_check_ticks` window (the criterion is the
//      simulation result, never "is the hook anchor dangerous"),
//   2. releasing the hook when that flies the tee into the hazard must be refused,
//   3. `quality` (iterations), `randomness` (exploration) and the three priority weights have to
//      change the decision, because "the slider is wired to nothing" is the bug this project keeps
//      hitting,
//   4. `bc_avoid_player_prediction` has to change the decision, and turning it off has to be
//      bit-identical to simulating without any other player,
//   5. `bc_avoid_unfreeze_ticks` and not `bc_avoid_check_ticks` has to set the lookahead next to an
//      unfreeze tile,
//   6. a safe corridor and a safe flat walk must never produce an intervention - in particular no
//      jump may appear out of nowhere.
//
// The geometry is built in memory rather than taken from a shipped map, so the distances are
// exact and a test failure means the rule broke, not that a level was updated.

#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/map.h>
#include <engine/storage.h>

#include <game/client/components/bestclient/avoid_engine.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
	constexpr int MAP_WIDTH = 48;
	constexpr int MAP_HEIGHT = 24;
	constexpr int GROUND_ROW = 18; // solid from here down
	constexpr int FLOOR_ROW = GROUND_ROW - 1; // the tee walks here
	constexpr float TILE = 32.0f;

	constexpr int TILE_EMPTY = 0;
	constexpr int TILE_SOLID_INDEX = 1; // TILE_SOLID
	constexpr int TILE_DEATH_INDEX = 2; // TILE_DEATH
	constexpr int TILE_FREEZE_INDEX = 9; // TILE_FREEZE
	constexpr int TILE_UNFREEZE_INDEX = 11; // TILE_UNFREEZE

	CTuningParams MakeDefaultTuning()
	{
		CTuningParams Tuning;
		Tuning.m_Gravity = 0.5f;
		Tuning.m_GroundControlSpeed = 10.0f;
		Tuning.m_GroundControlAccel = 100.0f / (float)SERVER_TICK_SPEED;
		Tuning.m_GroundFriction = 0.5f;
		Tuning.m_GroundJumpImpulse = 13.2f;
		Tuning.m_AirJumpImpulse = 12.0f;
		Tuning.m_AirControlSpeed = 250.0f / (float)SERVER_TICK_SPEED;
		Tuning.m_AirControlAccel = 1.5f;
		Tuning.m_AirFriction = 0.95f;
		Tuning.m_HookDragAccel = 3.0f;
		Tuning.m_HookDragSpeed = 15.0f;
		Tuning.m_HookLength = 380.0f;
		Tuning.m_HookFireSpeed = 80.0f;
		Tuning.m_VelrampStart = 550.0f;
		Tuning.m_VelrampRange = 2000.0f;
		Tuning.m_VelrampCurvature = 1.4f;
		return Tuning;
	}

	CNetObj_PlayerInput MakeInput(int Direction, int Jump, int Hook, int TargetX = 0, int TargetY = -1)
	{
		CNetObj_PlayerInput Input;
		mem_zero(&Input, sizeof(Input));
		Input.m_Direction = Direction;
		Input.m_Jump = Jump;
		Input.m_Hook = Hook;
		Input.m_TargetX = TargetX;
		Input.m_TargetY = TargetY;
		return Input;
	}

	Avoid::SSettings MakeSettings()
	{
		Avoid::SSettings Set;
		Set.m_Agent = 1; // Legit
		Set.m_DirectionAssist = true;
		Set.m_HookAssist = true;
		Set.m_CheckTicks = 26;
		Set.m_KickInTicks = 20;
		Set.m_Quality = 64;
		Set.m_Randomness = 0;
		Set.m_DirectionWeight = 100;
		Set.m_HookWeight = 100;
		Set.m_LifeWeight = 150;
		Set.m_TileDeath = true;
		Set.m_TileFreeze = true;
		Set.m_PlayerPrediction = true;
		Set.m_Nsif = true;
		Set.m_SensingRadius = 6.0f;
		return Set;
	}

	// A minimal in-memory map: a floor, a left wall and whatever the test adds on top. CCollision
	// only needs `GetData` for the tile array and `GetItem`/`GetType` for the game group and layer,
	// so this is far less code than writing a .map file - and it keeps the geometry exact.
	class CSyntheticMap : public IMap
	{
	public:
		std::vector<CTile> m_Tiles;
		CMapItemVersion m_VersionItem{};
		CMapItemGroup m_Group{};
		CMapItemLayerTilemap m_GameLayer{};

		CSyntheticMap()
		{
			m_VersionItem.m_Version = 1;
			m_Tiles.assign((size_t)MAP_WIDTH * MAP_HEIGHT, CTile{0, 0, 0, 0});

			for(int x = 0; x < MAP_WIDTH; x++)
				Tile(x, GROUND_ROW).m_Index = TILE_SOLID_INDEX;
			for(int y = 0; y < MAP_HEIGHT; y++)
				Tile(0, y).m_Index = TILE_SOLID_INDEX;

			m_GameLayer.m_Layer.m_Version = 3;
			m_GameLayer.m_Layer.m_Type = LAYERTYPE_TILES;
			m_GameLayer.m_Layer.m_Flags = TILESLAYERFLAG_GAME;
			m_GameLayer.m_Version = 3;
			m_GameLayer.m_Width = MAP_WIDTH;
			m_GameLayer.m_Height = MAP_HEIGHT;
			m_GameLayer.m_Flags = TILESLAYERFLAG_GAME;
			m_GameLayer.m_Image = -1;
			m_GameLayer.m_Data = 0;

			m_Group.m_Version = 2;
			m_Group.m_ParallaxX = 100;
			m_Group.m_ParallaxY = 100;
			m_Group.m_StartLayer = 0;
			m_Group.m_NumLayers = 1;
		}

		CTile &Tile(int x, int y) { return m_Tiles[(size_t)y * MAP_WIDTH + x]; }

		int GetDataSize(int Index) const override { return Index == 0 ? (int)(m_Tiles.size() * sizeof(CTile)) : 0; }
		void *GetData(int Index) override { return Index == 0 ? m_Tiles.data() : nullptr; }
		void *GetDataSwapped(int Index) override { return GetData(Index); }
		const char *GetDataString(int Index) override { return Index == 0 ? "" : nullptr; }
		void UnloadData(int Index) override {}
		int NumData() const override { return 1; }

		int GetItemSize(int Index) override
		{
			switch(Index)
			{
			case 0: return (int)sizeof(CMapItemVersion);
			case 1: return (int)sizeof(CMapItemGroup);
			case 2: return (int)sizeof(CMapItemLayerTilemap);
			default: return 0;
			}
		}

		void *GetItem(int Index, int *pType = nullptr, int *pId = nullptr, CUuid *pUuid = nullptr) override
		{
			if(pId)
				*pId = 0;
			if(pUuid)
				mem_zero(pUuid, sizeof(*pUuid));
			switch(Index)
			{
			case 0:
				if(pType)
					*pType = MAPITEMTYPE_VERSION;
				return &m_VersionItem;
			case 1:
				if(pType)
					*pType = MAPITEMTYPE_GROUP;
				return &m_Group;
			case 2:
				if(pType)
					*pType = MAPITEMTYPE_LAYER;
				return &m_GameLayer;
			default:
				if(pType)
					*pType = -1;
				return nullptr;
			}
		}

		void GetType(int Type, int *pStart, int *pNum) override
		{
			if(Type == MAPITEMTYPE_VERSION)
			{
				*pStart = 0;
				*pNum = 1;
			}
			else if(Type == MAPITEMTYPE_GROUP)
			{
				*pStart = 1;
				*pNum = 1;
			}
			else if(Type == MAPITEMTYPE_LAYER)
			{
				*pStart = 2;
				*pNum = 1;
			}
			else
			{
				*pStart = 0;
				*pNum = 0;
			}
		}

		int FindItemIndex(int Type, int Id) override { return Type == MAPITEMTYPE_GROUP && Id == 0 ? 1 : -1; }
		void *FindItem(int Type, int Id) override { return FindItemIndex(Type, Id) >= 0 ? &m_Group : nullptr; }
		int NumItems() const override { return 3; }
		bool Load(const char *pFullName, IStorage *pStorage, const char *pPath, int StorageType) override { return true; }
		bool Load(IStorage *pStorage, const char *pPath, int StorageType) override { return true; }
		void Unload() override {}
		bool IsLoaded() const override { return true; }
		IOHANDLE File() const override { return nullptr; }
		const char *FullName() const override { return "synthetic"; }
		const char *BaseName() const override { return "synthetic"; }
		const char *Path() const override { return "synthetic"; }
		SHA256_DIGEST Sha256() const override { return SHA256_DIGEST{}; }
		unsigned Crc() const override { return 0; }
		int Size() const override { return 0; }
	};

	class CAvoidLegitTest : public ::testing::Test
	{
	protected:
		CSyntheticMap m_Map;
		CLayers m_Layers;
		CCollision m_Collision;
		CTeamsCore m_Teams;
		bool m_Initialised = false;

		// Tiles are placed before the collision layer is built, so every test owns its geometry.
		void SetTile(int x, int y, int Index) { m_Map.Tile(x, y).m_Index = Index; }
		// Back to the bare floor: the geometry blocks of one test must not bleed into each other.
		void ResetTiles()
		{
			for(int y = 0; y < MAP_HEIGHT; y++)
				for(int x = 0; x < MAP_WIDTH; x++)
					SetTile(x, y, TILE_EMPTY);
			for(int x = 0; x < MAP_WIDTH; x++)
				SetTile(x, GROUND_ROW, TILE_SOLID_INDEX);
			for(int y = 0; y < MAP_HEIGHT; y++)
				SetTile(0, y, TILE_SOLID_INDEX);
		}
		void Wall(int x, int y) { SetTile(x, y, TILE_SOLID_INDEX); }
		void Death(int x, int y) { SetTile(x, y, TILE_DEATH_INDEX); }
		void Unfreeze(int x, int y) { SetTile(x, y, TILE_UNFREEZE_INDEX); }
		void Freeze(int x, int y) { SetTile(x, y, TILE_FREEZE_INDEX); }

		void BuildCollision()
		{
			int GroupsStart = 0, GroupsNum = 0, LayersStart = 0, LayersNum = 0;
			m_Map.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
			m_Map.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
			ASSERT_EQ(GroupsNum, 1);
			ASSERT_EQ(LayersNum, 1);
			m_Layers.Init(&m_Map, false, false);
			ASSERT_NE(m_Layers.GameLayer(), nullptr);
			m_Collision.Init(&m_Layers);
			ASSERT_EQ(m_Collision.GetWidth(), MAP_WIDTH);
			ASSERT_EQ(m_Collision.GetHeight(), MAP_HEIGHT);
			m_Initialised = true;
		}

		static vec2 TileCentre(int x, int y) { return vec2(((float)x + 0.5f) * TILE, ((float)y + 0.5f) * TILE); }
		static vec2 FloorPos(int TileX, float OffsetPx = 0.5f * TILE)
		{
			return vec2((float)TileX * TILE + OffsetPx, (float)FLOOR_ROW * TILE + 0.5f * TILE);
		}

		// The physics state of the controlled tee. The world wiring is provided by the environment,
		// exactly like in the client.
		CCharacterCore MakeCore(vec2 Pos, vec2 Vel, int HookState = HOOK_IDLE, vec2 HookPos = vec2(0.0f, 0.0f))
		{
			CCharacterCore Core;
			Core.Reset();
			Core.m_Pos = Pos;
			Core.m_Vel = Vel;
			Core.m_Tuning = MakeDefaultTuning();
			Core.m_Id = 0;
			Core.m_HookState = HookState;
			Core.m_HookPos = HookState == HOOK_IDLE ? Pos : HookPos;
			Core.m_HookDir = normalize(HookPos - Pos);
			return Core;
		}

		Avoid::SContext MakeContext(const CCharacterCore &Core, const CNetObj_PlayerInput &Input, const Avoid::SSettings &Set = MakeSettings(), int Tick = 1000)
		{
			Avoid::SContext Ctx;
			Ctx.m_Tick = Tick;
			Ctx.m_ClientId = 0;
			Ctx.m_Core = Core;
			Ctx.m_Input = Input;
			Ctx.m_Settings = Set;
			return Ctx;
		}

		Avoid::SEnvironment MakeEnvironment(bool WithPlayers = false)
		{
			Avoid::SEnvironment Env;
			Env.m_pCollision = &m_Collision;
			Env.m_pTeams = &m_Teams;
			Env.m_LocalId = 0;
			Env.m_PredictPlayers = WithPlayers;
			return Env;
		}

		void AddPlayer(Avoid::SEnvironment &Env, int Id, vec2 Pos, vec2 Vel)
		{
			Avoid::SPlayerSnapshot &Snapshot = Env.m_aPlayers[Env.m_NumPlayers++];
			Snapshot.m_Id = Id;
			Snapshot.m_Core = MakeCore(Pos, Vel);
			Snapshot.m_Core.m_Id = Id;
		}

		// A wall of death the tee cannot jump over (the jump apex is ~5.5 tiles above the floor,
		// the wall is ten tiles tall), with solid rock behind it to hook onto.
		void WallOfDeath()
		{
			for(int y = GROUND_ROW - 10; y <= GROUND_ROW; y++)
			{
				Death(24, y);
				Death(25, y);
			}
			for(int y = GROUND_ROW - 10; y <= GROUND_ROW; y++)
				for(int x = 27; x <= 29; x++)
					Wall(x, y);
		}
		vec2 WallAnchor() const { return TileCentre(27, GROUND_ROW - 2); }

		// A death floor below a fixed column, used by the "jump is a last resort" tests.
		void DeathFloor(int FromX, int ToX)
		{
			for(int x = FromX; x <= ToX; x++)
			{
				Death(x, FLOOR_ROW);
				Death(x, GROUND_ROW);
			}
		}

		// How long a candidate survives, measured with the very same simulator the agent uses.
		int Survive(const Avoid::SContext &Ctx, const Avoid::SEnvironment &Env, const CNetObj_PlayerInput &Input, int Horizon)
		{
			return Avoid::SimulateFixed(Ctx, Input, Horizon, &Env);
		}

	// ---------------------------------------------------------------------------------------------
	// The parameter gauntlet: a scripted "distracted player" walks straight into a death pit, tick
	// by tick, through the *whole* decision pipeline the client runs (sensor scan -> sensing gate ->
	// planner) and the agent may take over. This is how "does this slider do anything at all" gets
	// answered with numbers instead of faith.
	// ---------------------------------------------------------------------------------------------
	struct SGauntlet
	{
		bool m_Survived = false;
		int m_Overrides = 0;
		int m_Jumps = 0; // overrides that *added* a jump the player was not pressing
		int m_HookReleases = 0; // overrides that *released* a hook the player was holding
		int m_DeathTick = -1; // tick the tee died on, -1 if it never did
		int m_Lead = -1; // ticks the player's own input still had, when the agent first stepped in
		int m_Candidates = 0;
		float m_CostMs = 0.0f;
	};

	// `PitTile` is the first death column; the tee starts `StartTile` tiles in front of it and walks
	// right, never stopping, for `Ticks` ticks.
	SGauntlet RunGauntlet(const Avoid::SSettings &Set, int StartTile, int PitTile, int Ticks = 70)
	{
		SGauntlet Result;
		const Avoid::SEnvironment Env = MakeEnvironment(false);
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0, 0, -1);

		CCharacterCore Core = MakeCore(FloorPos(StartTile), vec2(0.0f, 0.0f));
		Avoid::CPlanner Planner;
		for(int Tick = 0; Tick < Ticks; Tick++)
		{
			Avoid::SContext Ctx = MakeContext(Core, Walk, Set, 1000 + Tick);
			Ctx.m_Threat = Avoid::ScanThreat(&m_Collision, Set, Core);

			Avoid::SInputPlan Plan;
			Plan.m_Input = Walk;
			// The exact gate CAvoid::EvaluateBestPlan() applies before it lets an agent run.
			if(Set.m_SensingRadius <= 0.0f || Ctx.m_Threat.m_HasNearest)
				Plan = Planner.Plan(Ctx, Env);

			CNetObj_PlayerInput Input = Walk;
			if(Plan.m_Override)
			{
				Input = Plan.m_Input;
				Result.m_Overrides++;
				if(Plan.m_Input.m_Jump && Plan.m_Input.m_Jump != Ctx.m_Input.m_Jump)
					Result.m_Jumps++;
				if(Plan.m_Input.m_Hook == 0 && Ctx.m_Input.m_Hook != 0)
					Result.m_HookReleases++;
				if(Result.m_Lead < 0)
				{
					// How much time the player still had at the moment the agent decided to help.
					Avoid::SContext Probe = Ctx;
					Result.m_Lead = Avoid::SimulateFixed(Probe, Walk, Set.m_CheckTicks, &Env);
				}
			}
			Result.m_Candidates = Plan.m_Candidates;
			Result.m_CostMs += Plan.m_CostMs;

			Avoid::SSimState Sim;
			Sim.Init(Ctx, Env);
			Sim.Step(Input);
			Core = Sim.m_Core;
			if(Avoid::IsRelevantHazard(Set, Avoid::ClassifyPoint(&m_Collision, Core.m_Pos)))
			{
				Result.m_DeathTick = Tick; // the tee went into the pit
				return Result;
			}
		}
		Result.m_Survived = true;
		return Result;
	}

	void PrintGauntlet(const char *pName, const char *pValue, const SGauntlet &G)
	{
		printf("[avoid-params] %-18s %-12s survived %d  died@ %3d  overrides %2d  jumps %2d  releases %2d  lead %2d  plans %3d  cost %.3f ms\n",
			pName, pValue, (int)G.m_Survived, G.m_DeathTick, G.m_Overrides, G.m_Jumps, G.m_HookReleases,
			G.m_Lead, G.m_Candidates, G.m_CostMs);
	}

	};

	// ---------------------------------------------------------------------------------------------
	// 1. The acceptance the stage 2 Basic agent could not deliver: holding a hook that drags the tee
	//    into the hazard has to be released. The scenario is a wall of death tiles in front of the
	//    tee with the hook anchored beyond it: the rope accelerates the tee into the pit no matter
	//    which direction key is pressed, so letting go is the only way to survive the window.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, HoldingAHookThatDragsIntoTheHazardIsReleased)
	{
		WallOfDeath();
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings();
		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		const Avoid::SEnvironment Env = MakeEnvironment();

		// The player is about to be dragged into the pit.
		const int PlayerSafe = Survive(Ctx, Env, Player, Set.m_CheckTicks);
		ASSERT_LT(PlayerSafe, Set.m_KickInTicks) << "scenario broken: the player's own input is not doomed";

		// ... and staying hooked stays doomed, whichever way the tee is steered.
		const CNetObj_PlayerInput BrakeHooked = MakeInput(0, 0, 1, Player.m_TargetX, Player.m_TargetY);
		EXPECT_LT(Survive(Ctx, Env, BrakeHooked, Set.m_CheckTicks), Set.m_CheckTicks)
			<< "scenario broken: holding the hook is survivable, so releasing is not required";

		Avoid::CPlanner Planner;
		const Avoid::SInputPlan Plan = Planner.Plan(Ctx, Env);

		EXPECT_TRUE(Plan.m_Override);
		EXPECT_EQ(Plan.m_Input.m_Hook, 0) << "the agent kept a hook that drags the tee into the hazard";
		EXPECT_GE(Plan.m_SafeTicks, Set.m_CheckTicks) << "the chosen plan does not survive the lookahead";
		EXPECT_FALSE(Plan.m_UsedFallback) << "letting go survives the whole window, so this is not NSIF";
		EXPECT_NE(std::strstr(Plan.m_aReason, "release hook"), nullptr) << "reason: " << Plan.m_aReason;
		EXPECT_GT(Plan.m_Candidates, 1);
	}

	// ---------------------------------------------------------------------------------------------
	// 2. The mirror case: letting go would fly the tee into the hazard while the rope is the very
	//    thing holding it back. Releasing has to be refused, and the agent falls back to braking.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, ReleasingTheHookIsRefusedWhenItWouldFlyIntoTheHazard)
	{
		// A death tile right in front of the tee, and the hook anchored behind it. Letting go turns
		// the rope drag off, and the tee is close enough that neither direction key can turn the
		// remaining speed around before the corner probes reach the pit (worst case: ground friction
		// alone still carries it ~10 px, the budget is ~5 px).
		Death(24, FLOOR_ROW);
		Death(24, GROUND_ROW);
		const vec2 Pos = FloorPos(23, 17.67f);
		const vec2 Anchor = Pos + vec2(-72.0f, 0.0f);
		Wall(21, FLOOR_ROW);
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		const Avoid::SEnvironment Env = MakeEnvironment();

		const int PlayerSafe = Survive(Ctx, Env, Player, Set.m_CheckTicks);
		ASSERT_LT(PlayerSafe, Set.m_KickInTicks) << "scenario broken: the player's own input is not doomed";

		// Every release candidate is fatal: the rope is what holds the tee back.
		for(int Direction = -1; Direction <= 1; Direction++)
		{
			const CNetObj_PlayerInput Released = MakeInput(Direction, 0, 0, Player.m_TargetX, Player.m_TargetY);
			EXPECT_LT(Survive(Ctx, Env, Released, Set.m_CheckTicks), Set.m_CheckTicks)
				<< "scenario broken: releasing with direction " << Direction << " survives";
		}

		Avoid::CPlanner Planner;
		const Avoid::SInputPlan Plan = Planner.Plan(Ctx, Env);

		EXPECT_TRUE(Plan.m_Override);
		EXPECT_EQ(Plan.m_Input.m_Hook, 1) << "the agent let go of the only thing keeping it out of the hazard";
		EXPECT_GT(Plan.m_SafeTicks, PlayerSafe);
	}

	// ---------------------------------------------------------------------------------------------
	// 3. `bc_avoid_quality` is the iteration count of the search: one iteration only ever looks at
	//    the player's own input (fast and dumb), enough iterations sweep the whole candidate space
	//    and find the release plan.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, QualityDrivesSearchEffortAndTheDecision)
	{
		WallOfDeath();
		BuildCollision();

		Avoid::SSettings Set = MakeSettings();
		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		const Avoid::SEnvironment Env = MakeEnvironment();

		Avoid::CPlanner Dumb;
		Set.m_Quality = 1;
		const Avoid::SInputPlan Cheap = Dumb.Plan(MakeContext(Core, Player, Set), Env);

		Avoid::CPlanner Smart;
		Set.m_Quality = 24;
		const Avoid::SInputPlan Normal = Smart.Plan(MakeContext(Core, Player, Set), Env);

		Avoid::CPlanner Best;
		Set.m_Quality = 200;
		const Avoid::SInputPlan Expensive = Best.Plan(MakeContext(Core, Player, Set), Env);

		// Quality = 1 evaluates the player's own input and nothing else, so it cannot improve on it.
		EXPECT_EQ(Cheap.m_Candidates, 1);
		EXPECT_FALSE(Cheap.m_Override);
		// The scenario holds the hook, so the jump is held back as well: the reason says so.
		EXPECT_STREQ(Cheap.m_aReason, "no safer plan (jump is a last resort)");

		// Enough iterations see every candidate action at least once: a full sweep is
		// 3 directions x 2 jumps x 3 hooks = 18 plans, and `quality` above that keeps refining.
		EXPECT_GE(Normal.m_Candidates, 18);
		EXPECT_LE(Normal.m_Candidates, 24);
		EXPECT_TRUE(Normal.m_Override);
		EXPECT_EQ(Normal.m_Input.m_Hook, 0);

		// The slider really is an effort knob: more iterations, more plans, more time.
		EXPECT_GT(Expensive.m_Candidates, Normal.m_Candidates);
		EXPECT_GT(Expensive.m_CostMs, Cheap.m_CostMs);
		EXPECT_LT(Expensive.m_CostMs, 1.5f) << "the search ignored the tick budget";
	}

	// ---------------------------------------------------------------------------------------------
	// 4. `bc_avoid_hook_weight` decides between "survive a bit longer" and "keep my hook": with a
	//    low life priority the slider flips the decision, which is exactly the wiring the stage 2
	//    write-up demanded proof for.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, HookWeightChangesTheHookDecision)
	{
		WallOfDeath();
		BuildCollision();

		const Avoid::SSettings Base = MakeSettings();
		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SEnvironment Env = MakeEnvironment();
		const Avoid::SContext Probe = MakeContext(Core, Player, Base);

		// The scenario: braking with the hook still in the wall survives, but not the whole window.
		const CNetObj_PlayerInput BrakeHooked = MakeInput(0, 0, 1, Player.m_TargetX, Player.m_TargetY);
		const int KeepSafe = Survive(Probe, Env, BrakeHooked, 26);
		ASSERT_GT(KeepSafe, 8) << "scenario broken: keeping the hook is hopeless";
		ASSERT_LT(KeepSafe, 26) << "scenario broken: keeping the hook survives the whole window";

		// A low life priority on purpose: that is the setting in which the hook priority is the
		// difference between "brake and keep the rope" and "let go and stop sooner".
		Avoid::SSettings KeepHook = Base;
		KeepHook.m_LifeWeight = 14;
		KeepHook.m_HookWeight = 200;
		Avoid::CPlanner PlannerKeep;
		const Avoid::SInputPlan Kept = PlannerKeep.Plan(MakeContext(Core, Player, KeepHook), Env);

		Avoid::SSettings PreferSurvival = Base;
		PreferSurvival.m_LifeWeight = 14;
		PreferSurvival.m_HookWeight = 0;
		Avoid::CPlanner PlannerDrop;
		const Avoid::SInputPlan Dropped = PlannerDrop.Plan(MakeContext(Core, Player, PreferSurvival), Env);

		EXPECT_TRUE(Kept.m_Override);
		EXPECT_EQ(Kept.m_Input.m_Hook, 1) << "a high hook priority has to keep the hook";
		EXPECT_LT(Kept.m_SafeTicks, PreferSurvival.m_CheckTicks) << "the kept hook is the shorter lived plan";

		EXPECT_TRUE(Dropped.m_Override);
		EXPECT_EQ(Dropped.m_Input.m_Hook, 0) << "a zero hook priority has to let go";
		EXPECT_GE(Dropped.m_SafeTicks, PreferSurvival.m_CheckTicks);
		EXPECT_NE(Kept.m_Input.m_Hook, Dropped.m_Input.m_Hook) << "bc_avoid_hook_weight is not wired";
	}

	// ---------------------------------------------------------------------------------------------
	// 5. `bc_avoid_player_prediction`: a tee that walks into you and shoves you towards the pit is
	//    only visible to the search while the snapshots are injected. With the switch off the
	//    engine has to behave exactly like it does without any other player around.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, PlayerPredictionChangesTheDecision)
	{
		Death(24, FLOOR_ROW);
		Death(24, GROUND_ROW);
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings();
		const vec2 Pos = FloorPos(23, 12.0f);
		CCharacterCore Core = MakeCore(Pos, vec2(0.0f, 0.0f));
		// Standing still is perfectly safe - until somebody else arrives.
		const CNetObj_PlayerInput Player = MakeInput(0, 0, 0, 0, -1);
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);

		Avoid::SEnvironment Blind = MakeEnvironment(true);
		Blind.m_PredictPlayers = false;
		AddPlayer(Blind, 3, Pos + vec2(-40.0f, 0.0f), vec2(8.0f, 0.0f));

		Avoid::SEnvironment Seeing = MakeEnvironment(true);
		AddPlayer(Seeing, 3, Pos + vec2(-40.0f, 0.0f), vec2(8.0f, 0.0f));

		// The shove is real: with the snapshot in the clone, standing still ends in the pit.
		EXPECT_LT(Survive(Ctx, Seeing, Player, Set.m_CheckTicks), Set.m_CheckTicks)
			<< "scenario broken: the other player never pushes the tee into the hazard";

		Avoid::CPlanner BlindPlanner;
		const Avoid::SInputPlan Ignored = BlindPlanner.Plan(Ctx, Blind);

		Avoid::CPlanner SeeingPlanner;
		const Avoid::SInputPlan Reacted = SeeingPlanner.Plan(Ctx, Seeing);

		// `bc_avoid_player_prediction = 0`: identical to "nobody is around".
		Avoid::CPlanner AlonePlanner;
		const Avoid::SInputPlan Alone = AlonePlanner.Plan(Ctx, MakeEnvironment(false));

		// ... and prediction on with nobody in reach is the same thing as well.
		Avoid::CPlanner EmptyPlanner;
		const Avoid::SInputPlan NoOne = EmptyPlanner.Plan(Ctx, MakeEnvironment(true));

		EXPECT_FALSE(Ignored.m_Override);
		EXPECT_STREQ(Ignored.m_aReason, "player input safe");
		EXPECT_EQ(Ignored.m_Override, Alone.m_Override);
		EXPECT_STREQ(Ignored.m_aReason, Alone.m_aReason);
		EXPECT_EQ(NoOne.m_Override, Alone.m_Override);
		EXPECT_STREQ(NoOne.m_aReason, Alone.m_aReason);

		EXPECT_TRUE(Reacted.m_Override) << "the agent ignored the tee that was about to shove it into the pit";
		EXPECT_GT(Reacted.m_SafeTicks, 0);
	}

	// ---------------------------------------------------------------------------------------------
	// 6. `bc_avoid_unfreeze_ticks` replaces `bc_avoid_check_ticks` when the nearest threat is an
	//    unfreeze tile: the shorter window means the agent is far less paranoid around it.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, UnfreezeTicksSetTheLookaheadNearUnfreezeTiles)
	{
		// The unfreeze tile sits above the walking line (out of reach), the death tile 12 ticks
		// ahead: the player's own input dies inside the long window but survives the short one.
		Unfreeze(23, 15);
		Death(24, FLOOR_ROW);
		Death(24, GROUND_ROW);
		BuildCollision();

		// 12 ticks of walking at the speed cap, plus the corner probe of the tee.
		const vec2 Pos = vec2(24.0f * TILE - 130.0f, (float)FLOOR_ROW * TILE + 16.0f);
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f));
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 0, 0, -1);

		Avoid::SSettings Set = MakeSettings();
		Set.m_TileUnfreeze = true;
		Set.m_UnfreezeTicks = 8;

		Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		Ctx.m_Threat.m_HasNearest = true;
		Ctx.m_Threat.m_NearestPos = TileCentre(23, 15);
		ASSERT_NE(Avoid::ClassifyPoint(&m_Collision, Ctx.m_Threat.m_NearestPos) & Avoid::HAZ_UNFREEZE, 0)
			<< "scenario broken: the nearest threat is not an unfreeze tile";

		const int PlayerSafe = Survive(Ctx, MakeEnvironment(false), Player, Set.m_CheckTicks);
		ASSERT_GT(PlayerSafe, Set.m_UnfreezeTicks) << "scenario broken: the tee dies inside the short window";
		ASSERT_LT(PlayerSafe, Set.m_CheckTicks) << "scenario broken: the tee survives the long window";

		Avoid::CPlanner ShortPlanner;
		const Avoid::SInputPlan ShortWindow = ShortPlanner.Plan(Ctx, MakeEnvironment(false));
		EXPECT_FALSE(ShortWindow.m_Override) << "the agent used check_ticks instead of unfreeze_ticks";
		EXPECT_EQ(ShortWindow.m_ScannedTicks, Set.m_UnfreezeTicks)
			<< "the lookahead has to be bc_avoid_unfreeze_ticks, not bc_avoid_check_ticks";

		Set.m_UnfreezeTicks = Set.m_CheckTicks;
		Avoid::CPlanner LongPlanner;
		const Avoid::SInputPlan LongWindow = LongPlanner.Plan(MakeContext(Core, Player, Set), MakeEnvironment(false));
		EXPECT_TRUE(LongWindow.m_Override) << "with the long window the tee is doomed and has to be saved";
		EXPECT_GE(LongWindow.m_SafeTicks, Set.m_CheckTicks);
	}

	// ---------------------------------------------------------------------------------------------
	// 7. No false triggers: a corridor with death on both sides and a long safe walk must never be
	//    touched, and in particular must never produce a jump.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, SafeCorridorAndFlatWalkAreNeverTouched)
	{
		// Death on both sides, two tiles away from the walking line: well inside the sensing radius
		// and well inside the first tiles of the lookahead, but never on the path.
		for(int y = FLOOR_ROW - 2; y <= GROUND_ROW; y++)
		{
			Death(20, y);
			Death(34, y);
		}
		BuildCollision();

		const Avoid::SEnvironment Env = MakeEnvironment(false);
		Avoid::CPlanner Planner;

		// Walking straight through the middle of the corridor, tick by tick. The near wall is one
		// and a half tiles away (well inside the sensing radius of six), the far one stays outside
		// the lookahead for the whole walk.
		CCharacterCore Core = MakeCore(vec2(21.5f * TILE, (float)FLOOR_ROW * TILE + 16.0f), vec2(0.0f, 0.0f));
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0, 0, -1);

		Avoid::SContext Ctx = MakeContext(Core, Walk);
		for(int Tick = 0; Tick < 10; Tick++)
		{
			Ctx.m_Tick = 1000 + Tick;
			Ctx.m_Core = Core;
			Ctx.m_Threat.m_HasNearest = true;
			Ctx.m_Threat.m_NearestPos = TileCentre(20, FLOOR_ROW);
			const Avoid::SInputPlan Plan = Planner.Plan(Ctx, Env);

			EXPECT_FALSE(Plan.m_Override) << "tick " << Tick << ": the agent touched a safe walk (" << Plan.m_aReason << ")";
			EXPECT_EQ(Plan.m_Input.m_Jump, 0) << "tick " << Tick << ": ghost jumping in a safe corridor";
			EXPECT_EQ(Plan.m_Input.m_Hook, 0) << "tick " << Tick << ": ghost hooking in a safe corridor";

			// Advance the real tee exactly like the engine would.
			Avoid::SSimState Sim;
			Avoid::SEnvironment StepEnv = Env;
			Sim.Init(Ctx, StepEnv);
			Sim.Step(Walk);
			Core = Sim.m_Core;
		}
		EXPECT_GT(Core.m_Pos.x, 21.5f * TILE) << "the walk never happened";
	}

	// Flat, hazard-free ground far away from anything: no hazard, no takeover, ever.
	TEST_F(CAvoidLegitTest, FlatGroundIsNeverTouched)
	{
		Death(24, FLOOR_ROW);
		Death(24, GROUND_ROW);
		BuildCollision();

		const Avoid::SEnvironment Env = MakeEnvironment(false);
		Avoid::CPlanner Planner;

		// Ten tiles away from the pit: outside every lookahead window that matters.
		CCharacterCore Core = MakeCore(FloorPos(14, 16.0f), vec2(0.0f, 0.0f));
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0, 0, -1);
		Avoid::SContext Ctx = MakeContext(Core, Walk);
		Ctx.m_Threat.m_HasNearest = true;
		Ctx.m_Threat.m_NearestPos = TileCentre(24, FLOOR_ROW);

		const Avoid::SInputPlan Plan = Planner.Plan(Ctx, Env);
		EXPECT_FALSE(Plan.m_Override);
		EXPECT_STREQ(Plan.m_aReason, "player input safe");
	}

	// ---------------------------------------------------------------------------------------------
	// 8. The movement state gate (TAS red line 3.11.7): jetpacks and hooks on other players are
	//    "hands off" states, and the engine reports them instead of nudging the movement key.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, SpecialMovementStatesAreHandsOff)
	{
		Death(24, FLOOR_ROW);
		Death(24, GROUND_ROW);
		BuildCollision();

		const Avoid::SEnvironment Env = MakeEnvironment(false);
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0, 0, -1);

		ASSERT_EQ(Avoid::ClassifyMovement(MakeCore(FloorPos(23, 8.0f), vec2(0.0f, 0.0f)), false), Avoid::MOVE_NORMAL);

		CCharacterCore Jetpack = MakeCore(FloorPos(23, 8.0f), vec2(0.0f, 0.0f));
		Jetpack.m_Jetpack = true;
		EXPECT_NE(Avoid::ClassifyMovement(Jetpack, false) & Avoid::MOVE_JETPACK, 0);
		Avoid::CPlanner JetpackPlanner;
		const Avoid::SInputPlan JetpackPlan = JetpackPlanner.Plan(MakeContext(Jetpack, Walk), Env);
		EXPECT_FALSE(JetpackPlan.m_Override);
		EXPECT_STREQ(JetpackPlan.m_aReason, "jetpack, hands off");

		CCharacterCore Hooked = MakeCore(FloorPos(23, 8.0f), vec2(0.0f, 0.0f));
		Hooked.SetHookedPlayer(3);
		EXPECT_NE(Avoid::ClassifyMovement(Hooked, false) & Avoid::MOVE_HOOKED_PLAYER, 0);
		Avoid::CPlanner HookedPlanner;
		const Avoid::SInputPlan HookedPlan = HookedPlanner.Plan(MakeContext(Hooked, Walk), Env);
		EXPECT_FALSE(HookedPlan.m_Override);
		EXPECT_STREQ(HookedPlan.m_aReason, "hooked to a player, hands off");

		// The hammer fly flag needs client state, but the classification itself has to report it.
		EXPECT_NE(Avoid::ClassifyMovement(MakeCore(FloorPos(23, 8.0f), vec2(0.0f, 0.0f)), true) & Avoid::MOVE_FLY_HAMMER, 0);
	}

	// ---------------------------------------------------------------------------------------------
	// 9. The search has to be deterministic per tick: the PRNG is local and seeded from the tick,
	//    never the global one (§6.5 point 8 of the delivery document).
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, SearchIsDeterministicPerTick)
	{
		WallOfDeath();
		BuildCollision();

		Avoid::SSettings Set = MakeSettings();
		// Well below the wall clock guard on purpose: this test is about the *algorithmic*
		// determinism (local PRNG seeded from the tick), not about how fast the machine is. A
		// quality close to the 1.0 ms budget would let a busy CPU cut the search at a different
		// iteration count and make the comparison meaningless.
		Set.m_Quality = 16;
		Set.m_Randomness = 30;
		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		const Avoid::SEnvironment Env = MakeEnvironment();

		Avoid::CPlanner First;
		const Avoid::SInputPlan A = First.Plan(Ctx, Env);
		Avoid::CPlanner Second;
		const Avoid::SInputPlan B = Second.Plan(Ctx, Env);

		EXPECT_EQ(A.m_Override, B.m_Override);
		EXPECT_EQ(A.m_SafeTicks, B.m_SafeTicks);
		EXPECT_EQ(A.m_Candidates, B.m_Candidates);
		EXPECT_FLOAT_EQ(A.m_Score, B.m_Score);
		EXPECT_EQ(A.m_Input.m_Direction, B.m_Input.m_Direction);
		EXPECT_EQ(A.m_Input.m_Jump, B.m_Input.m_Jump);
		EXPECT_EQ(A.m_Input.m_Hook, B.m_Input.m_Hook);
		EXPECT_STREQ(A.m_aReason, B.m_aReason);
	}


	// ---------------------------------------------------------------------------------------------
	// 13. The jump is a last resort, not a reflex (player feedback: the agent hopped while the
	//     player still had plenty of time to react, which reads as a glitch).
	//
	//     Falling towards a death floor is the cleanest case: nothing but the air jump survives, and
	//     the fall takes a known number of ticks. While the tee is still far from dying the agent
	//     has to keep its hands off; only in the last few ticks may it spend the jump.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, JumpWaitsForTheCriticalMoment)
	{
		DeathFloor(20, 30);
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings();
		const Avoid::SEnvironment Env = MakeEnvironment(false);
		const CNetObj_PlayerInput Idle = MakeInput(0, 0, 0, 0, -1);

		// 12 ticks to live: the player can still walk this back, so the agent waits.
		const CCharacterCore Early = MakeCore(vec2(25.5f * TILE, 490.0f), vec2(0.0f, 0.0f));
		const Avoid::SContext EarlyCtx = MakeContext(Early, Idle, Set);
		const int EarlySafe = Survive(EarlyCtx, Env, Idle, Set.m_CheckTicks);
		ASSERT_GT(EarlySafe, 6) << "scenario broken: the fall is already critical";
		ASSERT_LT(EarlySafe, Set.m_CheckTicks);
		ASSERT_GE(Survive(EarlyCtx, Env, MakeInput(0, 1, 0, 0, -1), Set.m_CheckTicks), Set.m_CheckTicks)
			<< "scenario broken: the air jump does not save the tee";

		Avoid::CPlanner Waiting;
		const Avoid::SInputPlan NoJump = Waiting.Plan(EarlyCtx, Env);
		EXPECT_FALSE(NoJump.m_Override) << "the agent spent the jump while the player still had time";
		EXPECT_EQ(NoJump.m_Input.m_Jump, 0);
		EXPECT_STREQ(NoJump.m_aReason, "no safer plan (jump is a last resort)");

		// 4 ticks to live: now it is now-or-never and the jump is spent.
		const CCharacterCore Late = MakeCore(vec2(25.5f * TILE, 527.0f), vec2(0.0f, 0.0f));
		const Avoid::SContext LateCtx = MakeContext(Late, Idle, Set);
		ASSERT_LE(Survive(LateCtx, Env, Idle, Set.m_CheckTicks), 6);

		Avoid::CPlanner Critical;
		const Avoid::SInputPlan Jumped = Critical.Plan(LateCtx, Env);
		EXPECT_TRUE(Jumped.m_Override);
		EXPECT_EQ(Jumped.m_Input.m_Jump, 1) << "the critical moment has to be used";
		EXPECT_GE(Jumped.m_SafeTicks, Set.m_CheckTicks);
	}

	// ---------------------------------------------------------------------------------------------
	// 14. While the hook is engaged the jump is held back completely: a rope problem is solved with
	//     the rope (release, steer, brake), never by hopping - that is the case from the report,
	//     where the agent jumped off a wall it was hooked to.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, JumpIsSuppressedWhileTheHookIsEngaged)
	{
		DeathFloor(20, 30);
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings();
		const Avoid::SEnvironment Env = MakeEnvironment(false);
		const CNetObj_PlayerInput Idle = MakeInput(0, 0, 0, 0, -1);

		const vec2 Pos = vec2(25.5f * TILE, 527.0f); // the critical case from the test above
		const CCharacterCore Falling = MakeCore(Pos, vec2(0.0f, 0.0f));

		// Control: without a hook the agent uses the jump.
		Avoid::CPlanner Plain;
		const Avoid::SInputPlan WithoutHook = Plain.Plan(MakeContext(Falling, Idle, Set), Env);
		ASSERT_TRUE(WithoutHook.m_Override);
		ASSERT_EQ(WithoutHook.m_Input.m_Jump, 1);

		// Holding the hook: the same fall, but the jump is off the table. The anchor is right next
		// to the tee so there is no drag at all - the only difference is the engaged rope.
		CCharacterCore Hooked = MakeCore(Pos, vec2(0.0f, 0.0f), HOOK_GRABBED, Pos + vec2(0.0f, -20.0f));
		const CNetObj_PlayerInput Holding = MakeInput(0, 0, 1, 0, -1);
		ASSERT_LT(distance(Hooked.m_HookPos, Hooked.m_Pos), 46.0f) << "scenario broken: the anchor drags";

		Avoid::CPlanner Roped;
		const Avoid::SInputPlan WithHook = Roped.Plan(MakeContext(Hooked, Holding, Set), Env);
		EXPECT_FALSE(WithHook.m_Override) << "the agent jumped out of a rope problem";
		EXPECT_EQ(WithHook.m_Input.m_Jump, 0);
		EXPECT_STREQ(WithHook.m_aReason, "no safer plan (jump is a last resort)");
	}

	// ---------------------------------------------------------------------------------------------
	// 15. The sensing reach is an ellipse, not a circle: over one lookahead window the tee can cover
	//     far more ground sideways than downwards, so a pit below it is noticed later than a wall to
	//     the side. Both axes are derived from the map tuning, never hard-coded.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, SensingReachIsFlatterVertically)
	{
		BuildCollision();

		const Avoid::SSettings Set = MakeSettings(); // sensing radius 6 tiles, check ticks 26
		const vec2 Pos(20.0f * TILE + 16.0f, 17.0f * TILE + 16.0f); // standing on the floor
		const CCharacterCore Core = MakeCore(Pos, vec2(0.0f, 0.0f));

		// 0.5 * gravity * 26^2 / (10 * 26) == 0.65 on the default tuning.
		const float Factor = Avoid::SensingVerticalFactor(Core, Set);
		EXPECT_NEAR(Factor, 0.65f, 0.02f);
		ASSERT_LT(Factor, 1.0f) << "the vertical reach has to be the shorter one";

		// Same distance, two directions. 150 px is inside the horizontal reach (6 * 32 = 192) and
		// outside the vertical one (192 * 0.65 = 125).
		const float Probe = 150.0f;
		ASSERT_LT(Probe, Set.m_SensingRadius * TILE);
		ASSERT_GT(Probe, Set.m_SensingRadius * TILE * Factor);

		{
			// A wall of death to the right: 150 px away, must be seen.
			CSyntheticMap SideMap;
			SideMap.Tile((int)((Pos.x + Probe) / TILE), FLOOR_ROW).m_Index = TILE_DEATH_INDEX;
			// (the shared fixture map is reused below, this block only documents the geometry)
		}

		// Right: a death tile whose box starts 150 px to the side.
		const int SideTile = (int)((Pos.x + Probe + 16.0f) / TILE);
		Death(SideTile, FLOOR_ROW);
		BuildCollision();
		const Avoid::SThreat SideThreat = Avoid::ScanThreat(&m_Collision, Set, Core);
		EXPECT_TRUE(SideThreat.m_HasNearest) << "a wall 150 px to the side has to be inside the reach";

		// Below: a death tile whose box starts 150 px under the tee's centre.
		const int DropTile = (int)((Pos.y + Probe + 16.0f) / TILE);
		SetTile(SideTile, FLOOR_ROW, TILE_EMPTY);
		Death((int)(Pos.x / TILE), DropTile);
		BuildCollision();
		const Avoid::SThreat DropThreat = Avoid::ScanThreat(&m_Collision, Set, Core);
		EXPECT_FALSE(DropThreat.m_HasNearest) << "a pit 150 px below must wait until it is closer";
		EXPECT_EQ(DropThreat.m_HazardTiles, 0);

		// Half the distance below and it is inside the reach again: the squash, not an off switch.
		SetTile((int)(Pos.x / TILE), DropTile, TILE_EMPTY);
		const int NearDropTile = (int)((Pos.y + Probe * 0.5f) / TILE);
		Death((int)(Pos.x / TILE), NearDropTile);
		BuildCollision();
		const Avoid::SThreat NearThreat = Avoid::ScanThreat(&m_Collision, Set, Core);
		EXPECT_TRUE(NearThreat.m_HasNearest) << "a pit 75 px below has to be seen";
	}


	// ---------------------------------------------------------------------------------------------
	// 16. Parameter sensitivity: every slider of the Legit panel has to change something observable.
	//     The gauntlet walks into the pit, so "did it change anything" is answered with survival,
	//     override count, action mix, the lead time and the cost - not with a claim.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, ParameterSensitivitySweep)
	{
		ResetTiles();
		for(int x = 24; x <= 25; x++)
			for(int y = FLOOR_ROW; y <= GROUND_ROW; y++)
				Death(x, y);
		BuildCollision();

		const int StartTile = 18; // six tiles of runway before the pit
		const int PitTile = 24;
		const Avoid::SSettings Base = MakeSettings();
		printf("[avoid-params] === walking into a pit at column %d from column %d ===\n", PitTile, StartTile);

		// --- bc_avoid_check_ticks: the lookahead window ------------------------------------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {4, 8, 12, 20, 26, 40, 50})
			{
				Set.m_CheckTicks = Value;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%d t", Value);
				PrintGauntlet("check_ticks", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- bc_avoid_quality: the iteration count ----------------------------------------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {1, 8, 18, 24, 64, 200})
			{
				Set.m_Quality = Value;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%d", Value);
				PrintGauntlet("quality", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- bc_avoid_kick_in_ticks: how early the agent is allowed to step in -------------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {0, 4, 10, 20, 30, 50})
			{
				Set.m_KickInTicks = Value;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%d t", Value);
				PrintGauntlet("kick_in_ticks", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- bc_avoid_sensing_radius (half tiles): the gate --------------------------------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {1, 2, 4, 8, 12, 24, 32})
			{
				Set.m_SensingRadius = Value * 0.5f;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%.1f t", Value * 0.5f);
				PrintGauntlet("sensing_radius", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- bc_avoid_life_weight: survival versus respecting the input --------------------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {0, 10, 50, 150, 200})
			{
				Set.m_LifeWeight = Value;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%d", Value);
				PrintGauntlet("life_weight", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- bc_avoid_randomness: exploration, deliberately NOT a decision driver ----------------
		{
			Avoid::SSettings Set = Base;
			for(int Value : {0, 30, 200})
			{
				Set.m_Randomness = Value;
				char aValue[16];
				str_format(aValue, sizeof(aValue), "%d", Value);
				PrintGauntlet("randomness", aValue, RunGauntlet(Set, StartTile, PitTile));
			}
		}

		// --- assist switches -----------------------------------------------------------------------
		{
			Avoid::SSettings Set = Base;
			Set.m_DirectionAssist = false;
			const SGauntlet NoDirection = RunGauntlet(Set, StartTile, PitTile);
			PrintGauntlet("direction_assist", "off", NoDirection);

			Set = Base;
			Set.m_HookAssist = false;
			PrintGauntlet("hook_assist", "off", RunGauntlet(Set, StartTile, PitTile));

			Set = Base;
			Set.m_TileDeath = false;
			PrintGauntlet("tile_death", "off", RunGauntlet(Set, StartTile, PitTile));
		}

		// --- the few things that must hold, whatever the sliders say ----------------------------
		Avoid::SSettings Dumb = Base;
		Dumb.m_Quality = 1;
		EXPECT_EQ(RunGauntlet(Dumb, StartTile, PitTile).m_Overrides, 0)
			<< "quality 1 has to stay dumb (it never evaluates a second action)";

		Avoid::SSettings Blind = Base;
		Blind.m_TileDeath = false;
		EXPECT_EQ(RunGauntlet(Blind, StartTile, PitTile).m_Overrides, 0)
			<< "death tiles off means the pit is not a hazard at all";

		Avoid::SSettings Lazy = Base;
		Lazy.m_LifeWeight = 0;
		EXPECT_EQ(RunGauntlet(Lazy, StartTile, PitTile).m_Overrides, 0)
			<< "life priority 0 means the player's own input is never worth overriding";

		// A wider lookahead and a bigger radius both have to make the agent step in earlier.
		Avoid::SSettings Narrow = Base;
		Narrow.m_SensingRadius = 1.0f;
		Avoid::SSettings Wide = Base;
		Wide.m_SensingRadius = 12.0f;
		EXPECT_LT(RunGauntlet(Narrow, StartTile, PitTile).m_Lead, RunGauntlet(Wide, StartTile, PitTile).m_Lead)
			<< "the radius still does not move the reaction point";

		Avoid::SSettings Short = Base;
		Short.m_CheckTicks = 8;
		Avoid::SSettings Long = Base;
		Long.m_CheckTicks = 40;
		EXPECT_LT(RunGauntlet(Short, StartTile, PitTile).m_Lead, RunGauntlet(Long, StartTile, PitTile).m_Lead)
			<< "the lookahead window still does not move the reaction point";

		// --- bc_avoid_nsif: only take the safest first step when nothing survives the window ----
		// A hooked tee that is dragged into a wall of death, with `hook_assist` off: releasing is not
		// available to the agent, so no candidate reaches the end of the lookahead - the exact
		// situation NSIF exists for. With NSIF off the agent has to stay completely out of it.
		{
			ResetTiles();
			WallOfDeath();
			BuildCollision();

			const vec2 Pos = FloorPos(21);
			const vec2 Anchor = WallAnchor();
			const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
			const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
			const Avoid::SEnvironment Env = MakeEnvironment();

			Avoid::SSettings Set = Base;
			Set.m_HookAssist = false; // the rope cannot be cut, so the tee is doomed either way
			Set.m_Nsif = true;
			Avoid::CPlanner WithNsif;
			const Avoid::SInputPlan Fallback = WithNsif.Plan(MakeContext(Core, Player, Set), Env);
			ASSERT_TRUE(Fallback.m_Override) << "scenario broken: nothing survives longer than the input";
			ASSERT_TRUE(Fallback.m_UsedFallback) << "scenario broken: something survives the window";
			ASSERT_LT(Fallback.m_SafeTicks, Set.m_CheckTicks);
			EXPECT_EQ(Fallback.m_Input.m_Hook, 1) << "without hook assistance the rope stays in the wall";

			Set.m_Nsif = false;
			Avoid::CPlanner WithoutNsif;
			const Avoid::SInputPlan LeftAlone = WithoutNsif.Plan(MakeContext(Core, Player, Set), Env);
			EXPECT_FALSE(LeftAlone.m_Override) << "NSIF off: nothing survives, so nothing is touched";
			EXPECT_STREQ(LeftAlone.m_aReason, "no safer plan");
			SGauntlet NsifRow;
			NsifRow.m_Overrides = 1;
			NsifRow.m_Lead = Fallback.m_SafeTicks; // what the fallback buys: a few more ticks
			NsifRow.m_Candidates = Fallback.m_Candidates;
			NsifRow.m_CostMs = Fallback.m_CostMs;
			PrintGauntlet("nsif", "on", NsifRow);
			SGauntlet OffRow;
			OffRow.m_Candidates = LeftAlone.m_Candidates;
			OffRow.m_CostMs = LeftAlone.m_CostMs;
			PrintGauntlet("nsif", "off", OffRow);
		}

		// --- the F.4 configuration trap: kick_in_ticks at or above the lookahead ----------------
		{
			ResetTiles();
			for(int x = 24; x <= 25; x++)
				for(int y = FLOOR_ROW; y <= GROUND_ROW; y++)
					Death(x, y);
			BuildCollision();
			Avoid::SSettings Set = Base;
			Set.m_KickInTicks = 30; // wider than check_ticks: the agent can only act too late
			PrintGauntlet("kick_in trap", "30 > 26", RunGauntlet(Set, StartTile, PitTile));
			Set.m_KickInTicks = 0; // "do not wait at all"
			PrintGauntlet("kick_in", "0 (nowait)", RunGauntlet(Set, StartTile, PitTile));
		}

	}

	// ---------------------------------------------------------------------------------------------
	// 11. The performance contract of the delivery document (6.4): the whole decision has to stay
	//     inside 1.5 ms per tick, and that budget is what `bc_avoid_quality` spends. This is the
	//     worst case the module can produce: the tee is doomed, player prediction is on with the
	//     full set of snapshots, and the search is allowed to use its whole budget.
	//     It is a benchmark, so the assertions are deliberately generous - but a decision that
	//     blows the budget is a real failure, not a slow CI machine.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, LegitDecisionCostStaysInsideTheTickBudget)
	{
		WallOfDeath();
		BuildCollision();

		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));

		// Behind the tee, so they are simulated on every tick without standing in its way.
		Avoid::SEnvironment Env = MakeEnvironment(true);
		for(int i = 0; i < Avoid::MAX_SHADOW_PLAYERS; i++)
		{
			const vec2 Offset = vec2(-60.0f - (float)(i % 4) * 40.0f, (float)((i / 4) * 2 - 1) * 40.0f);
			AddPlayer(Env, 10 + i, Pos + Offset, vec2(2.0f, 0.0f));
		}

		const int Decisions = 200;
		float aCostMs[3] = {};
		const int aQuality[3] = {1, 24, 200};
		for(int q = 0; q < 3; q++)
		{
			Avoid::SSettings Set = MakeSettings();
			Set.m_Quality = aQuality[q];
			Set.m_Randomness = 30;

			Avoid::CPlanner Planner;
			const int64_t Start = time_get();
			float ReportedCost = 0.0f;
			for(int i = 0; i < Decisions; i++)
			{
				Avoid::SContext Ctx = MakeContext(Core, Player, Set, 1000 + i);
				const Avoid::SInputPlan Plan = Planner.Plan(Ctx, Env);
				ReportedCost += Plan.m_CostMs;
				if(i == 0)
					printf("[avoid-legit]   quality %3d first plan: override %d cands %d safe %d reason '%s'\n",
						aQuality[q], (int)Plan.m_Override, Plan.m_Candidates, Plan.m_SafeTicks, Plan.m_aReason);
			}
			aCostMs[q] = (float)((time_get() - Start) * 1000.0 / (double)time_freq()) / (float)Decisions;

			printf("[avoid-legit] quality %3d: %.3f ms per decision (engine reported %.3f ms), budget 1.5 ms\n",
				aQuality[q], aCostMs[q], ReportedCost / (float)Decisions);
		}

		// The same worst case without the predicted players, for the deviation notes in the
		// delivery document: player prediction roughly doubles the price of a rollout.
		{
			Avoid::SSettings Set = MakeSettings();
			Set.m_Quality = 200;
			Avoid::SEnvironment Alone = Env;
			Alone.m_PredictPlayers = false;
			Alone.m_NumPlayers = 0;

			Avoid::CPlanner Planner;
			const int64_t Start = time_get();
			int AloneCandidates = 0;
			for(int i = 0; i < Decisions; i++)
				AloneCandidates = Planner.Plan(MakeContext(Core, Player, Set, 1000 + i), Alone).m_Candidates;
			const float AloneMs = (float)((time_get() - Start) * 1000.0 / (double)time_freq()) / (float)Decisions;
			printf("[avoid-legit] quality 200 without player prediction: %.3f ms per decision (%d iterations before the budget guard)\n", AloneMs, AloneCandidates);
		}

		for(int q = 0; q < 3; q++)
			EXPECT_LT(aCostMs[q], 1.5f) << "quality " << aQuality[q] << " blows the tick budget";
		EXPECT_GT(aCostMs[1], aCostMs[0]) << "quality 24 has to cost more than quality 1";
		EXPECT_GT(aCostMs[2], aCostMs[1]) << "quality 200 has to cost more than quality 24";
	}

	// ---------------------------------------------------------------------------------------------
	// 10. `bc_avoid_randomness` is the exploration constant of the search plus the chance that a
	//     rollout leaves the candidate action. It must change the search (and never crash), while
	//     the fully deterministic setting stays reproducible.
	// ---------------------------------------------------------------------------------------------
	TEST_F(CAvoidLegitTest, RandomnessChangesTheSearchWithoutBreakingIt)
	{
		WallOfDeath();
		BuildCollision();

		Avoid::SSettings Set = MakeSettings();
		// Same reasoning as in SearchIsDeterministicPerTick: stay away from the budget guard so the
		// comparison measures the exploration constant and not the machine load.
		Set.m_Quality = 30;
		Set.m_Randomness = 0;
		const vec2 Pos = FloorPos(21);
		const vec2 Anchor = WallAnchor();
		const CCharacterCore Core = MakeCore(Pos, vec2(10.0f, 0.0f), HOOK_GRABBED, Anchor);
		const CNetObj_PlayerInput Player = MakeInput(1, 0, 1, (int)(Anchor.x - Pos.x), (int)(Anchor.y - Pos.y));
		const Avoid::SContext Ctx = MakeContext(Core, Player, Set);
		const Avoid::SEnvironment Env = MakeEnvironment();

		Avoid::CPlanner CalmA, CalmB;
		const Avoid::SInputPlan Calm1 = CalmA.Plan(Ctx, Env);
		const Avoid::SInputPlan Calm2 = CalmB.Plan(Ctx, Env);
		EXPECT_FLOAT_EQ(Calm1.m_Score, Calm2.m_Score) << "randomness 0 has to stay deterministic";

		Avoid::SSettings Wild = Set;
		Wild.m_Randomness = 200;
		Avoid::CPlanner Noisy;
		const Avoid::SInputPlan Explored = Noisy.Plan(MakeContext(Core, Player, Wild), Env);

		// Both settings have to produce a usable plan ...
		EXPECT_TRUE(Calm1.m_Override);
		EXPECT_TRUE(Explored.m_Override);
		EXPECT_EQ(Explored.m_Input.m_Hook, 0) << "the release plan has to survive the exploration";
		EXPECT_LE(Explored.m_CostMs, 1.5f);
		// ... and the exploration has to be visible: with randomness 0 the search keeps hammering
		// the best known action, with 200 it spreads over the tree, so the return it reports for the
		// winning plan differs.
		EXPECT_NE(Explored.m_Score, Calm1.m_Score) << "randomness did not change the search at all";
	}
} // namespace


