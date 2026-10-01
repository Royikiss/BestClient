// Tests for the physics recipe the Avoid forward simulator relies on.
//
// CAvoid::SimulateInput() clones a live CCharacterCore and advances it by hand instead of
// standing up a whole CGameWorld (see docs/AVOID_TECHNICAL_DOCUMENTATION.md, 6.3 path A).
// That clone is only trustworthy while it steps the core exactly the way the engine does, so the
// two things pinned down here are:
//
//   1. Tick(true, DoDeferredTick = false) + Move() has to leave the same state behind as the
//      engine's own order, Tick(true, deferred) followed by TickDeferred()'s Move(). Both are
//      driven over a real collision map, because the interesting part of the recipe is how it
//      interacts with walls, ceilings and the map tuning, not the free-fall arithmetic.
//   2. The clone has to survive having no CWorldCore (no other players). That is exactly the
//      configuration which makes IsSwitchActiveCb() and the ClampVel() call sites fall back to
//      neutral behaviour; if either ever starts demanding a world, this test fails instead of the
//      client crashing mid-game.

#include "test.h"

#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/map.h>
#include <engine/storage.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace
{
	// A map tuning that is deliberately not the default one: the simulator has to follow the tuning
	// it is handed, never a constant copied out of tuning.h.
	CTuningParams MakeTuning()
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

	// The exact body of CAvoid::SimulateInput(), reduced to the parts the engine sees.
	void StepLikeSimulator(CCharacterCore &Core, const CNetObj_PlayerInput &Input)
	{
		Core.m_Input = Input;
		Core.Tick(true, false);
		Core.Move();
	}

	// What CCharacter::PreTick() + CCharacter::TickDeferred() do, for a tee that has a world but
	// nobody else in it. `Tick(true, true)` runs TickDeferred() internally, and the extra Move()
	// mirrors the one CCharacter::TickDeferred() performs itself.
	void StepLikeCharacter(CCharacterCore &Core, const CNetObj_PlayerInput &Input)
	{
		Core.m_Input = Input;
		Core.Tick(true, true);
		Core.Move();
	}

	bool SameCore(const CCharacterCore &A, const CCharacterCore &B)
	{
		return A.m_Pos == B.m_Pos &&
		       A.m_Vel == B.m_Vel &&
		       A.m_HookState == B.m_HookState &&
		       A.m_HookPos == B.m_HookPos &&
		       A.m_HookDir == B.m_HookDir &&
		       A.m_HookTick == B.m_HookTick &&
		       A.m_Jumped == B.m_Jumped &&
		       A.m_JumpedTotal == B.m_JumpedTotal &&
		       A.m_Direction == B.m_Direction &&
		       A.m_Angle == B.m_Angle;
	}

	// Loads a real map so that CCollision has geometry to offer. The tile maps that ship with the
	// client are enough: the test only needs walls and a tuning, not a specific level.
	class CAvoidSimulatorTest : public ::testing::Test
	{
	protected:
		CTestInfo m_TestInfo;
		std::unique_ptr<IStorage> m_pStorage;
		std::unique_ptr<IMap> m_pMap;
		CLayers m_Layers;
		CCollision m_Collision;
		vec2 m_StartPos = vec2(0.0f, 0.0f);

		void SetUp() override
		{
			m_pStorage = m_TestInfo.CreateTestStorage();
			ASSERT_NE(m_pStorage, nullptr);

			m_pMap = CreateMap();
			ASSERT_NE(m_pMap, nullptr);
			// "coverage" is the map the server side tests load as well, so it is guaranteed to be
			// present next to the test binary.
			if(!m_pMap->Load(m_pStorage.get(), "maps/coverage.map", IStorage::TYPE_ALL))
			{
				GTEST_SKIP() << "maps/coverage.map is not available in this checkout";
				return;
			}

			m_Layers.Init(m_pMap.get(), false, false);
			ASSERT_NE(m_Layers.GameLayer(), nullptr);
			m_Collision.Init(&m_Layers);
			ASSERT_GT(m_Collision.GetWidth(), 0);
			ASSERT_GT(m_Collision.GetHeight(), 0);

			m_StartPos = FindGroundedSpot(4);
		}

		bool BoxFits(vec2 Pos) const
		{
			return !m_Collision.TestBox(Pos, CCharacterCore::PhysicalSizeVec2());
		}

		bool IsGrounded(vec2 Pos) const
		{
			return m_Collision.IsOnGround(Pos, CCharacterCore::PhysicalSize());
		}

		bool HasClearRunRight(vec2 Pos, int Tiles) const
		{
			for(int i = 1; i <= Tiles; i++)
			{
				if(!BoxFits(Pos + vec2((float)i * 32.0f, 0.0f)))
					return false;
			}
			return true;
		}

		// A spot where a tee can fall a little, land on solid ground and then walk `Clearance` tiles to
		// the right without hitting a wall. Scanning outwards from the middle of the map keeps the test
		// independent of which level ships as `coverage`.
		vec2 FindGroundedSpot(int Clearance)
		{
			const vec2 Center(m_Collision.GetWidth() * 32.0f / 2.0f, m_Collision.GetHeight() * 32.0f / 2.0f);
			for(int Radius = 0; Radius <= 512; Radius += 16)
			{
				for(int Dy = -Radius; Dy <= Radius; Dy += 16)
				{
					for(int Dx = -Radius; Dx <= Radius; Dx += 16)
					{
						const vec2 Candidate = Center + vec2((float)Dx, (float)Dy);
						if(!BoxFits(Candidate))
							continue;

						// Fall straight down from the candidate until the tee is standing.
						CCharacterCore Probe;
						Probe.Reset();
						Probe.SetCoreWorld(nullptr, &m_Collision, nullptr);
						Probe.m_Pos = Candidate;
						Probe.m_Tuning = MakeTuning();
						Probe.m_Id = 0;
						for(int i = 0; i < 100 && !IsGrounded(Probe.m_Pos); i++)
							StepLikeSimulator(Probe, MakeInput(0, 0, 0));

						if(!IsGrounded(Probe.m_Pos))
							continue;
						if(!BoxFits(Probe.m_Pos))
							continue;
						if(!HasClearRunRight(Probe.m_Pos, Clearance))
							continue;
						return Probe.m_Pos;
					}
				}
			}
			// Nothing suitable: the tests then fail loudly instead of passing vacuously.
			return Center;
		}

		void InitCore(CCharacterCore &Core, CWorldCore *pWorld, CTeamsCore *pTeams, const CTuningParams &Tuning)
		{
			Core.Reset();
			Core.SetCoreWorld(pWorld, &m_Collision, pTeams);
			Core.m_Pos = m_StartPos;
			Core.m_Vel = vec2(0.0f, 0.0f);
			Core.m_Tuning = Tuning;
			// The simulator neutralises the id so that no team related branch can be reached.
			Core.m_Id = 0;
		}
	};

	TEST_F(CAvoidSimulatorTest, StepRecipeMatchesTheEngineTickSequence)
	{
		const CTuningParams Tuning = MakeTuning();

		// The simulator: no world at all.
		CCharacterCore Simulated;
		InitCore(Simulated, nullptr, nullptr, Tuning);

		// The engine: a world of its own, but nobody inside it.
		CWorldCore World;
		CTeamsCore Teams;
		CCharacterCore Reference;
		InitCore(Reference, &World, &Teams, Tuning);

		ASSERT_EQ(Simulated.m_Pos, Reference.m_Pos);

		// A run that covers every branch the simulator relies on: falling, walking both ways,
		// braking, a ground jump, air control and a hook that flies off and retracts.
		const CNetObj_PlayerInput aSequence[] = {
			MakeInput(0, 0, 0),
			MakeInput(0, 0, 0),
			MakeInput(1, 0, 0),
			MakeInput(1, 1, 0),
			MakeInput(1, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(0, 0, 0),
			MakeInput(0, 0, 1, 100, -30),
			MakeInput(0, 0, 1, 100, -30),
			MakeInput(1, 0, 1, 100, -30),
			MakeInput(1, 0, 0),
			MakeInput(1, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(0, 0, 0),
			MakeInput(0, 1, 0),
			MakeInput(0, 1, 0),
			MakeInput(1, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(0, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(-1, 0, 0),
			MakeInput(0, 0, 0),
			MakeInput(1, 0, 0),
			MakeInput(1, 0, 0),
			MakeInput(0, 0, 0),
		};

		int Tick = 0;
		for(const CNetObj_PlayerInput &Input : aSequence)
		{
			StepLikeSimulator(Simulated, Input);
			StepLikeCharacter(Reference, Input);

			char aDetail[192];
			str_format(aDetail, sizeof(aDetail), "tick %d: pos (%.4f, %.4f) vs (%.4f, %.4f), vel (%.4f, %.4f) vs (%.4f, %.4f)",
				Tick, Simulated.m_Pos.x, Simulated.m_Pos.y, Reference.m_Pos.x, Reference.m_Pos.y,
				Simulated.m_Vel.x, Simulated.m_Vel.y, Reference.m_Vel.x, Reference.m_Vel.y);
			ASSERT_TRUE(SameCore(Simulated, Reference)) << aDetail;
			Tick++;
		}
	}

	TEST_F(CAvoidSimulatorTest, WalkingAcrossTheRealMapIsReproducible)
	{
		const CTuningParams Tuning = MakeTuning();
		CCharacterCore Simulated;
		InitCore(Simulated, nullptr, nullptr, Tuning);

		// 50 ticks of walking right, i.e. the full `bc_avoid_check_ticks` window twice over. This is
		// the part that would catch a missing collision pointer or a mis-copied jump state.
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0);
		const vec2 Start = Simulated.m_Pos;
		for(int i = 0; i < 50; ++i)
			StepLikeSimulator(Simulated, Walk);

		EXPECT_GT(Simulated.m_Pos.x, Start.x);
		EXPECT_TRUE(std::isfinite(Simulated.m_Pos.x));
		EXPECT_TRUE(std::isfinite(Simulated.m_Pos.y));
		// The tee must never end up inside solid geometry.
		EXPECT_FALSE(m_Collision.TestBox(Simulated.m_Pos, CCharacterCore::PhysicalSizeVec2()));
	}

	TEST_F(CAvoidSimulatorTest, MapTuningDrivesThePrediction)
	{
		// A `tune` map with a slower tee has to produce a visibly different trajectory from the
		// default one. This is the regression guard for "never hard-code physics constants".
		CCharacterCore Default;
		InitCore(Default, nullptr, nullptr, MakeTuning());

		CTuningParams SlowTuning = MakeTuning();
		SlowTuning.m_GroundControlSpeed = 5.0f;
		CCharacterCore Slow;
		InitCore(Slow, nullptr, nullptr, SlowTuning);

		// Both tees are standing on solid floor at this point (FindGroundedSpot guarantees it), so
		// the ten ticks below measure ground acceleration and nothing else.
		ASSERT_TRUE(IsGrounded(Default.m_Pos));
		const CNetObj_PlayerInput Walk = MakeInput(1, 0, 0);
		const vec2 DefaultStart = Default.m_Pos;
		const vec2 SlowStart = Slow.m_Pos;

		for(int i = 0; i < 10; ++i)
		{
			StepLikeSimulator(Default, Walk);
			StepLikeSimulator(Slow, Walk);
		}

		EXPECT_FLOAT_EQ(Default.m_Vel.x, 10.0f);
		EXPECT_FLOAT_EQ(Slow.m_Vel.x, 5.0f);
		EXPECT_GT(Default.m_Pos.x - DefaultStart.x, Slow.m_Pos.x - SlowStart.x);
	}

	// The Basic agent pays for one simulation of the player's input plus three candidates, each up
	// to `bc_avoid_check_ticks` ticks deep. This measures that worst case with the same stepping
	// the simulator uses, and reports it against the 1.5 ms per tick budget the delivery document
	// sets for the whole engine (section 6.4). It is a benchmark, not a hard gate: the assertion is
	// deliberately generous so that a slow CI machine cannot fail the suite.
	TEST_F(CAvoidSimulatorTest, WorstCaseDecisionCostStaysInsideTheTickBudget)
	{
		constexpr int CheckTicks = 26; // bc_avoid_check_ticks default
		constexpr int Candidates = 4; // player input + left/keep/right
		constexpr int Decisions = 2000;

		const CTuningParams Tuning = MakeTuning();

		const auto RunOneDecision = [&]() {
			long long TotalTicks = 0;
			for(int Candidate = 0; Candidate < Candidates; Candidate++)
			{
				CCharacterCore Core;
				InitCore(Core, nullptr, nullptr, Tuning);
				// Start already moving so that every tick is real work, no idle warm-up.
				Core.m_Vel = vec2(6.0f, 0.0f);

				const CNetObj_PlayerInput Input = MakeInput(Candidate % 3 - 1, 0, 0);
				for(int Tick = 0; Tick < CheckTicks; Tick++)
				{
					StepLikeSimulator(Core, Input);
					TotalTicks++;
				}
			}
			return TotalTicks;
		};

		RunOneDecision(); // warm the caches up

		const int64_t Start = time_get();
		for(int i = 0; i < Decisions; i++)
			RunOneDecision();
		const double ElapsedMs = (double)(time_get() - Start) * 1000.0 / (double)time_freq();
		const double PerDecisionMs = ElapsedMs / Decisions;
		const double PerTickMs = PerDecisionMs / Candidates;

		printf("[avoid-sim] worst case %.4f ms per decision, %.4f ms per candidate simulation, budget 1.5 ms/tick\n",
			PerDecisionMs, PerTickMs);

		EXPECT_LT(PerDecisionMs, 1.5);
	}
} // namespace
