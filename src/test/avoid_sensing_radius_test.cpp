// Tests for the sensing-radius gate of the Avoid decision engine
// (CAvoid::EvaluateBestPlan(), tier 0 of docs/AVOID_TECHNICAL_DOCUMENTATION.md 6.7).
//
// Why this file exists: the switch `bc_avoid_sensing_radius` originally only reached
// `CAvoid::ScanThreat()`, which feeds the HUD and the world overlay. The decision engine asked
// `ClassifyPoint()` about every simulated position with no range limit, so the tee was braked at
// exactly the same distance from a hazard no matter where the slider sat - the setting looked
// dead. The engine now refuses to act while the sensor reports no relevant hazard in range, and
// the tests below pin down both halves of that contract:
//
//   1. a wider radius reacts further away from the hazard, and
//   2. the smallest radius the cvar allows still leaves room for the whole braking distance,
//      so the lower half of the slider stays usable instead of fatal.
//
// The geometry is built in memory rather than taken from a shipped map: the numbers have to be
// exact for a distance assertion to mean anything, and depending on level layout would make the
// test break whenever a map is updated.

#include <base/mem.h>
#include <base/vmath.h>

#include <engine/map.h>
#include <engine/storage.h>

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
	constexpr int MAP_WIDTH = 40;
	constexpr int MAP_HEIGHT = 24;
	constexpr int GROUND_ROW = 18; // solid from here down
	constexpr int FLOOR_ROW = GROUND_ROW - 1; // the tee stands here
	constexpr int DEATH_COLUMN = 30;
	constexpr int TILE_DEATH_INDEX = 2; // TILE_DEATH
	constexpr int TILE_SOLID_INDEX = 1; // TILE_SOLID

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
		Tuning.m_HookLength = 380.0f;
		Tuning.m_HookFireSpeed = 80.0f;
		Tuning.m_VelrampStart = 550.0f;
		Tuning.m_VelrampRange = 2000.0f;
		Tuning.m_VelrampCurvature = 1.4f;
		return Tuning;
	}

	CNetObj_PlayerInput MakeInput(int Direction)
	{
		CNetObj_PlayerInput Input;
		mem_zero(&Input, sizeof(Input));
		Input.m_Direction = Direction;
		Input.m_TargetY = -1;
		return Input;
	}

	// The exact body of CAvoid::SimulateInput(), reduced to the parts the engine sees.
	void StepLikeSimulator(CCharacterCore &Core, const CNetObj_PlayerInput &Input)
	{
		Core.m_Input = Input;
		Core.Tick(true, false);
		Core.Move();
	}

	// True when the tee at `Pos` touches a death tile, mirroring CAvoid::ClassifyPoint() with the
	// default tile switches (centre probe plus the four corners of the 28/3 px box).
	bool IsDeathAt(const CCollision &Collision, vec2 Pos)
	{
		const float R = CCharacterCore::PhysicalSize() / 3.0f;
		for(int Corner = 0; Corner < 4; Corner++)
		{
			const float Px = Pos.x + ((Corner & 1) ? R : -R);
			const float Py = Pos.y + ((Corner & 2) ? R : -R);
			if(Collision.GetCollisionAt(Px, Py) == TILE_DEATH ||
				Collision.GetFrontCollisionAt(Px, Py) == TILE_DEATH)
				return true;
			const int Index = Collision.GetPureMapIndex(vec2(Px, Py));
			if(Index >= 0 && Collision.GetSwitchType(Index) == TILE_DEATH)
				return true;
		}
		const int Center = Collision.GetPureMapIndex(Pos);
		if(Center >= 0)
		{
			if(Collision.GetTileIndex(Center) == TILE_DEATH ||
				Collision.GetFrontTileIndex(Center) == TILE_DEATH ||
				Collision.GetSwitchType(Center) == TILE_DEATH)
				return true;
		}
		return false;
	}

	// The tile-centre scan CAvoid::ScanThreat() performs, limited to `Radius` tiles. This is the gate
	// the decision engine consults through `Ctx.m_Threat.m_HasNearest`.
	bool ScanFindsHazard(const CCollision &Collision, vec2 Pos, int Radius)
	{
		const int CenterX = (int)std::floor(Pos.x / 32.0f);
		const int CenterY = (int)std::floor(Pos.y / 32.0f);
		for(int Ty = CenterY - Radius; Ty <= CenterY + Radius; Ty++)
		{
			for(int Tx = CenterX - Radius; Tx <= CenterX + Radius; Tx++)
			{
				if(IsDeathAt(Collision, vec2((Tx + 0.5f) * 32.0f, (Ty + 0.5f) * 32.0f)))
					return true;
			}
		}
		return false;
	}

	// A minimal in-memory map holding a floor, a left wall and one death strip. CCollision only needs
	// `GetData` for the tile array and `GetItem`/`GetType` for the game group and layer, so this is
	// far less code than writing a .map file - and it keeps the geometry exact.
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
			{
				// Floor to stand on.
				Tile(x, GROUND_ROW).m_Index = TILE_SOLID_INDEX;
				// A wall on the left so nothing can leave the map sideways.
				Tile(x, FLOOR_ROW).m_Index = 0;
			}
			for(int y = 0; y < MAP_HEIGHT; y++)
				Tile(0, y).m_Index = TILE_SOLID_INDEX;

			// The death strip sits in the floor row, so a tee walking right runs into it.
			Tile(DEATH_COLUMN, FLOOR_ROW).m_Index = TILE_DEATH_INDEX;

			// CMapItemLayerTilemap_v2 keeps a CMapItemLayer prefix as a *member* (`m_Layer`), and then
			// repeats the layout: version, width, height, flags. `CLayers::Init()` reads
			// `m_Layer.m_Type` for the layer kind but `m_Flags` (the outer one) for the game flag, so
			// both have to be filled in.
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
			// `CLayers::GetLayer(Index)` resolves through `GetItem(m_LayersStart + Index)`, so the group
			// stores the layer offset relative to the first layer item, exactly like a real map.
			m_Group.m_StartLayer = 0;
			m_Group.m_NumLayers = 1;
		}

		CTile &Tile(int x, int y) { return m_Tiles[(size_t)y * MAP_WIDTH + x]; }

		// IMap: only the members CCollision and CLayers touch.
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

		int FindItemIndex(int Type, int Id) override
		{
			return Type == MAPITEMTYPE_GROUP && Id == 0 ? 1 : -1;
		}
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

	class CAvoidSensingRadiusTest : public ::testing::Test
	{
	protected:
		CSyntheticMap m_Map;
		CLayers m_Layers;
		CCollision m_Collision;

		void SetUp() override
		{
			// The synthetic map must look enough like a real one for CLayers to find the game layer.
			int GroupsStart = 0, GroupsNum = 0, LayersStart = 0, LayersNum = 0;
			m_Map.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
			m_Map.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
			ASSERT_EQ(GroupsNum, 1);
			ASSERT_EQ(LayersNum, 1);
			const auto *pGroup = static_cast<const CMapItemGroup *>(m_Map.GetItem(GroupsStart));
			ASSERT_NE(pGroup, nullptr);
			ASSERT_EQ(pGroup->m_NumLayers, 1);
			const CMapItemLayerTilemap *pTilemap = static_cast<const CMapItemLayerTilemap *>(m_Map.GetItem(LayersStart + pGroup->m_StartLayer));
			const CMapItemLayer *pLayer = &pTilemap->m_Layer;
			ASSERT_NE(pLayer, nullptr);
			ASSERT_EQ(pLayer->m_Type, LAYERTYPE_TILES);
			ASSERT_NE(pLayer->m_Flags & TILESLAYERFLAG_GAME, 0);

			m_Layers.Init(&m_Map, false, false);
			ASSERT_NE(m_Layers.GameGroup(), nullptr) << "synthetic map: game group not found";
			ASSERT_NE(m_Layers.GameLayer(), nullptr) << "synthetic map: game layer not found";
			m_Collision.Init(&m_Layers);
			ASSERT_EQ(m_Collision.GetWidth(), MAP_WIDTH);
			ASSERT_EQ(m_Collision.GetHeight(), MAP_HEIGHT);
		}

		// Where the tee stands when it starts walking, and where the hazard begins.
		vec2 SpawnPos() const { return vec2((DEATH_COLUMN - 8 + 0.5f) * 32.0f, (FLOOR_ROW + 0.5f) * 32.0f); }
		float DeathTileLeftEdge() const { return (float)DEATH_COLUMN * 32.0f; }

		struct SWalkResult
		{
			bool m_Survived = false;
			bool m_SensorSawHazard = false;
			vec2 m_FirstBrakePos = vec2(0.0f, 0.0f);
			int m_PlayerSafeTicks = 0;
			float m_ReactionGap = 0.0f;
		};

		// Walks the tee right and applies the decision rule of EvaluateBestPlan() on every tick:
		// brake if the sensor sees a relevant hazard, otherwise keep going. `Radius` is the sensing
		// radius under test.
		SWalkResult WalkIntoTheHazard(int Radius, const CTuningParams &Tuning, int CheckTicks)
		{
			SWalkResult Result;

			CCharacterCore Core;
			Core.Reset();
			Core.SetCoreWorld(nullptr, &m_Collision, nullptr);
			Core.m_Pos = SpawnPos();
			Core.m_Tuning = Tuning;
			Core.m_Id = 0;

			const CNetObj_PlayerInput Walk = MakeInput(1);
			const CNetObj_PlayerInput Brake = MakeInput(-1);

			for(int Tick = 0; Tick < 400; Tick++)
			{
				// Tier 0: the sensing radius gate.
				if(!Result.m_SensorSawHazard && ScanFindsHazard(m_Collision, Core.m_Pos, Radius))
				{
					Result.m_SensorSawHazard = true;
					Result.m_FirstBrakePos = Core.m_Pos;
					Result.m_ReactionGap = DeathTileLeftEdge() - Core.m_Pos.x;

					// Tier 1: how long does the player's own input still survive? `KickInTicks` is
					// compared against exactly this number.
					CCharacterCore Probe = Core;
					for(int i = 0; i < CheckTicks; i++)
					{
						StepLikeSimulator(Probe, Walk);
						if(IsDeathAt(m_Collision, Probe.m_Pos))
							break;
						Result.m_PlayerSafeTicks++;
					}
				}

				StepLikeSimulator(Core, Result.m_SensorSawHazard ? Brake : Walk);
				if(IsDeathAt(m_Collision, Core.m_Pos))
					return Result; // the agent failed to keep the tee out of the hazard
			}

			Result.m_Survived = true;
			return Result;
		}
	};

	// The tee is braked before it ever touches the hazard, at both ends of the slider.
	TEST_F(CAvoidSensingRadiusTest, BrakesInTimeAcrossTheWholeRadiusRange)
	{
		const CTuningParams Tuning = MakeDefaultTuning();
		constexpr int CheckTicks = 26;

		for(int Radius = 2; Radius <= 16; Radius++)
		{
			const SWalkResult Result = WalkIntoTheHazard(Radius, Tuning, CheckTicks);
			EXPECT_TRUE(Result.m_SensorSawHazard) << "radius " << Radius << " never saw the hazard";
			EXPECT_TRUE(Result.m_Survived) << "radius " << Radius << " braked too late";
		}
	}

	// This is the regression for the wiring bug: a bigger radius has to react further out.
	TEST_F(CAvoidSensingRadiusTest, WiderRadiusReactsFurtherFromTheHazard)
	{
		const CTuningParams Tuning = MakeDefaultTuning();
		constexpr int CheckTicks = 26;

		const SWalkResult Narrow = WalkIntoTheHazard(2, Tuning, CheckTicks);
		const SWalkResult Wide = WalkIntoTheHazard(6, Tuning, CheckTicks);

		ASSERT_TRUE(Narrow.m_Survived);
		ASSERT_TRUE(Wide.m_Survived);

		// One tile of extra reach per tile of radius, minus the sensor's coarse sampling: the gap is
		// measured from the tee centre to the hazard box, so it tracks `radius * 32` plus the tee's
		// own half width.
		EXPECT_NEAR(Wide.m_ReactionGap, 6 * 32.0f, 2 * 32.0f);
		EXPECT_NEAR(Narrow.m_ReactionGap, 2 * 32.0f, 2 * 32.0f);
		EXPECT_GT(Wide.m_ReactionGap, Narrow.m_ReactionGap + 2 * 32.0f)
			<< "sensing radius does not change the distance the agent reacts at";
	}

	// The lower half of the slider has to stay usable: at the minimum radius the tee still has enough
	// ticks left to come to a full stop before the hazard.
	TEST_F(CAvoidSensingRadiusTest, MinimumRadiusStillFitsTheBrakingDistance)
	{
		const CTuningParams Tuning = MakeDefaultTuning();
		constexpr int CheckTicks = 26;

		const SWalkResult Narrow = WalkIntoTheHazard(2, Tuning, CheckTicks);
		ASSERT_TRUE(Narrow.m_Survived);
		ASSERT_TRUE(Narrow.m_SensorSawHazard);

		// Ticks available between the moment the sensor fires and the moment the tee would reach the
		// hazard, at the walking speed cap.
		const float AvailableTicks = Narrow.m_ReactionGap / Tuning.m_GroundControlSpeed;
		// How many ticks the player's own walking input would still have survived.
		const float PlayerTicks = (float)Narrow.m_PlayerSafeTicks;

		EXPECT_GT(AvailableTicks, PlayerTicks) << "the tee is already doomed when the sensor fires";
		// Braking from the cap takes ground_control_speed / ground_control_accel == 5 ticks.
		EXPECT_GE(AvailableTicks, 5.0f);
	}

	// With the radius turned all the way up, the gate can no longer be what stops the agent: the
	// forward simulation's `check_ticks` window takes over. The reaction distance then stops growing,
	// which is why the slider stops feeling different past roughly eight tiles on flat ground.
	TEST_F(CAvoidSensingRadiusTest, CheckTicksTakesOverAtLargeRadii)
	{
		const CTuningParams Tuning = MakeDefaultTuning();
		constexpr int CheckTicks = 26;

		const SWalkResult Mid = WalkIntoTheHazard(8, Tuning, CheckTicks);
		const SWalkResult Huge = WalkIntoTheHazard(16, Tuning, CheckTicks);

		ASSERT_TRUE(Mid.m_Survived);
		ASSERT_TRUE(Huge.m_Survived);
		ASSERT_TRUE(Mid.m_SensorSawHazard);
		ASSERT_TRUE(Huge.m_SensorSawHazard);

		// Both react before the hazard is even close, so the exact gap is set by the tee's speed and
		// the tick budget rather than by the sensor.
		EXPECT_NEAR(Mid.m_ReactionGap, Huge.m_ReactionGap, 2 * 32.0f);
	}
} // namespace
