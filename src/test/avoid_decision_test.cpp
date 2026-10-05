// Tests for the pure decision rules of the Avoid module (reference spec 6.2, 6.3, 7.2 and 7.3).
//
// Why this file exists: the Legit agent used to apply the root child with the most visits, and when
// the search budget cut the iteration loop short those visit counts are tied - which makes "most
// visits" mean "the first child the tree ever created", i.e. direction -1. The bot walked left on
// its own. The decision rules are pure functions of their arguments, so they are pinned down here
// instead of by hand in game:
//
//   1. the heuristic has to reward the player's own direction and hook state (reference 7.2),
//   2. the final root selection has to use exploitation + heuristic with zero exploration and has
//      to skip children that were never rolled out (reference 7.3),
//   3. both candidate enumerations have to keep the reference order, because the search keeps the
//      first candidate that reaches the best survival count,
//   4. (0, 0) may never be left in an aim vector, the protocol does not allow aiming at the centre.

#include <game/client/components/bestclient/avoid_decision.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
	CNetObj_PlayerInput MakeInput(int Direction, int Hook, int Jump = 0, int TargetX = 0, int TargetY = -1)
	{
		CNetObj_PlayerInput Input{};
		Input.m_Direction = Direction;
		Input.m_Hook = Hook;
		Input.m_Jump = Jump;
		Input.m_TargetX = TargetX;
		Input.m_TargetY = TargetY;
		return Input;
	}

	Avoid::SLegitRootChild MakeChild(int Direction, int Hook, int Visits, double TotalValue, int LifespanTicks)
	{
		Avoid::SLegitRootChild Child;
		Child.m_Action = MakeInput(Direction, Hook);
		Child.m_Visits = Visits;
		Child.m_TotalValue = TotalValue;
		Child.m_LifespanTicks = LifespanTicks;
		return Child;
	}

	// The root ring as BuildLegitCandidates() creates it with both assists on: the hook = 0 ring
	// first, then the hook = 1 ring, each over the directions {-1, 0, 1}.
	std::vector<Avoid::SLegitRootChild> MakeReferenceRing(int Visits, double TotalValue, int LifespanTicks)
	{
		std::vector<Avoid::SLegitRootChild> vChildren;
		vChildren.push_back(MakeChild(-1, 0, Visits, TotalValue, LifespanTicks));
		vChildren.push_back(MakeChild(0, 0, Visits, TotalValue, LifespanTicks));
		vChildren.push_back(MakeChild(1, 0, Visits, TotalValue, LifespanTicks));
		vChildren.push_back(MakeChild(-1, 1, Visits, TotalValue, LifespanTicks));
		vChildren.push_back(MakeChild(0, 1, Visits, TotalValue, LifespanTicks));
		vChildren.push_back(MakeChild(1, 1, Visits, TotalValue, LifespanTicks));
		return vChildren;
	}
} // namespace

// -------------------------------------------------------------------------------------------
// Legit heuristic (reference spec 7.2)
// -------------------------------------------------------------------------------------------

TEST(AvoidDecision, LegitHeuristicMatchesTheReferenceConstants)
{
	// Default weights, six safe ticks and the player standing still: the direction bonus is
	// 2 * 170 * 0.01 = 3.4, the hook bonus 260 * 0.01 = 2.6 and the life bonus 6 * 160 * 0.01 = 9.6.
	const CNetObj_PlayerInput Human = MakeInput(0, 0);
	const float Identity = Avoid::LegitHeuristicScore(MakeInput(0, 0), Human, 6, 170, 260, 160);
	EXPECT_FLOAT_EQ(Identity, 15.6f);

	// Walking away from what the player pressed is worth less, and the exact opposite direction
	// loses the direction term completely.
	EXPECT_FLOAT_EQ(Avoid::LegitHeuristicScore(MakeInput(-1, 0), Human, 6, 170, 260, 160), 1.7f + 2.6f + 9.6f);
	EXPECT_FLOAT_EQ(Avoid::LegitHeuristicScore(MakeInput(-1, 1), Human, 6, 170, 260, 160), 1.7f + 0.0f + 9.6f);

	// A different hook state costs the hook term but keeps the direction term.
	EXPECT_FLOAT_EQ(Avoid::LegitHeuristicScore(MakeInput(0, 1), Human, 6, 170, 260, 160), 3.4f + 0.0f + 9.6f);
}

TEST(AvoidDecision, LegitHeuristicAlwaysRanksThePlayersOwnInputHighest)
{
	for(int Direction = -1; Direction <= 1; ++Direction)
	{
		for(int Hook = 0; Hook <= 1; ++Hook)
		{
			const CNetObj_PlayerInput Human = MakeInput(Direction, Hook);
			const float Identity = Avoid::LegitHeuristicScore(Human, Human, 6, 170, 260, 160);
			for(int OtherDirection = -1; OtherDirection <= 1; ++OtherDirection)
			{
				for(int OtherHook = 0; OtherHook <= 1; ++OtherHook)
				{
					if(OtherDirection == Direction && OtherHook == Hook)
						continue;
					const float Other = Avoid::LegitHeuristicScore(
						MakeInput(OtherDirection, OtherHook), Human, 6, 170, 260, 160);
					EXPECT_LT(Other, Identity)
						<< "human " << Direction << "/" << Hook
						<< " other " << OtherDirection << "/" << OtherHook;
				}
			}
		}
	}
}

// -------------------------------------------------------------------------------------------
// Legit root selection (reference spec 7.3)
// -------------------------------------------------------------------------------------------

// The regression this module was rebuilt for: with every child visited exactly once and the same
// reward, the winner has to be the child that matches the player's input. Picking the first child
// of the ring instead is what made the bot walk left while the player was standing still.
TEST(AvoidDecision, LegitRootSelectionKeepsThePlayersInputWhenItIsSafe)
{
	const std::vector<Avoid::SLegitRootChild> vChildren = MakeReferenceRing(1, 1.0, 6);
	const CNetObj_PlayerInput Human = MakeInput(0, 0);

	const int Best = Avoid::SelectLegitRootChild(vChildren, Human, 170, 260, 160);
	ASSERT_EQ(Best, 1) << "the identity child is direction 0 / hook 0, not the first child of the ring";
	EXPECT_EQ(vChildren[(size_t)Best].m_Action.m_Direction, Human.m_Direction);
	EXPECT_EQ(vChildren[(size_t)Best].m_Action.m_Hook, Human.m_Hook);
}

TEST(AvoidDecision, LegitRootSelectionKeepsThePlayersInputWhenThePlayerIsMoving)
{
	// Same ring, but the player holds "right": the child at index 2 is the identity one, and the
	// left child (index 0) must not win a tie against it.
	const std::vector<Avoid::SLegitRootChild> vChildren = MakeReferenceRing(2, 2.0, 6);
	const CNetObj_PlayerInput Human = MakeInput(1, 0);

	const int Best = Avoid::SelectLegitRootChild(vChildren, Human, 170, 260, 160);
	ASSERT_EQ(Best, 2);
	EXPECT_EQ(vChildren[(size_t)Best].m_Action.m_Direction, 1);
}

TEST(AvoidDecision, LegitRootSelectionTakesOverWhenThePlayersInputDies)
{
	// The player's own action freezes immediately (reward 0, no survival), turning away survives the
	// whole window: the search has to take the safe child even though the human-likeness bonus of
	// the player's action is larger.
	std::vector<Avoid::SLegitRootChild> vChildren = MakeReferenceRing(0, 0.0, 0);
	vChildren[1] = MakeChild(0, 0, 1, 0.0, 0); // the identity child, dies instantly
	vChildren[0] = MakeChild(-1, 0, 4, 4.0, 6);

	const CNetObj_PlayerInput Human = MakeInput(0, 0);
	const int Best = Avoid::SelectLegitRootChild(vChildren, Human, 170, 260, 160);
	ASSERT_EQ(Best, 0);
	EXPECT_EQ(vChildren[(size_t)Best].m_LifespanTicks, 6);
}

TEST(AvoidDecision, LegitRootSelectionSkipsUnvisitedChildren)
{
	std::vector<Avoid::SLegitRootChild> vChildren = MakeReferenceRing(0, 0.0, 0);
	vChildren[4] = MakeChild(0, 1, 1, 1.0, 6);

	const CNetObj_PlayerInput Human = MakeInput(0, 0);
	// Only index 4 was ever rolled out, so it is the only child that carries information.
	EXPECT_EQ(Avoid::SelectLegitRootChild(vChildren, Human, 170, 260, 160), 4);
}

TEST(AvoidDecision, LegitRootSelectionReportsAnEmptyRing)
{
	const std::vector<Avoid::SLegitRootChild> vChildren = MakeReferenceRing(0, 0.0, 0);
	// -1 tells the agent that nothing was measured, which is what keeps it from acting on an
	// unvisited child when the search budget stops the loop before the first expansion.
	EXPECT_EQ(Avoid::SelectLegitRootChild(vChildren, MakeInput(0, 0), 170, 260, 160), -1);
	EXPECT_EQ(Avoid::SelectLegitRootChild({}, MakeInput(0, 0), 170, 260, 160), -1);
}

// -------------------------------------------------------------------------------------------
// Candidate enumerations (reference spec 6.2 and 7.3 step 2)
// -------------------------------------------------------------------------------------------

TEST(AvoidDecision, LegitCandidatesFollowTheReferenceOrder)
{
	std::vector<CNetObj_PlayerInput> vCandidates;
	Avoid::BuildLegitCandidates(MakeInput(0, 0), true, true, false, &vCandidates);

	const int aExpected[6][2] = {{-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
	ASSERT_EQ(vCandidates.size(), 6u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, aExpected[i][0]) << "candidate " << i;
		EXPECT_EQ(vCandidates[i].m_Hook, aExpected[i][1]) << "candidate " << i;
	}
}

TEST(AvoidDecision, LegitCandidatesAddTheAirJumpAxisToTheDirectionRing)
{
	// Reference spec 8.3: while the tee owns its air jump and has headroom, every direction of the
	// hook = 0 ring is offered twice - once without and once with the jump injected. The hook = 1
	// ring below inherits the parent's jump state, exactly like the reference.
	std::vector<CNetObj_PlayerInput> vCandidates;
	Avoid::BuildLegitCandidates(MakeInput(0, 0, 1), true, true, true, &vCandidates);

	const int aExpected[9][3] = {
		{-1, 0, 0}, {-1, 0, 1},
		{0, 0, 0}, {0, 0, 1},
		{1, 0, 0}, {1, 0, 1},
		{-1, 1, 1}, {0, 1, 1}, {1, 1, 1}};
	ASSERT_EQ(vCandidates.size(), 9u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, aExpected[i][0]) << "candidate " << i;
		EXPECT_EQ(vCandidates[i].m_Hook, aExpected[i][1]) << "candidate " << i;
		EXPECT_EQ(vCandidates[i].m_Jump, aExpected[i][2]) << "candidate " << i;
	}

	// Without an air jump to spend the jump key of the parent is inherited, not cleared: clearing it
	// would take the ground jump away from every child and the agent could never jump while running.
	Avoid::BuildLegitCandidates(MakeInput(1, 1, 1), true, false, false, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 3u);
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
		EXPECT_EQ(Candidate.m_Jump, 1);
}

TEST(AvoidDecision, LegitCandidatesRespectTheAssistSwitches)
{
	std::vector<CNetObj_PlayerInput> vCandidates;

	// Direction only: the three directions, the hook state of the parent is kept.
	Avoid::BuildLegitCandidates(MakeInput(0, 1), true, false, false, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 3u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, (int)i - 1);
		EXPECT_EQ(vCandidates[i].m_Hook, 1);
	}

	// Hook only: both hook states, the direction of the parent is kept.
	Avoid::BuildLegitCandidates(MakeInput(1, 0), false, true, false, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 2u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, 1);
		EXPECT_EQ(vCandidates[i].m_Hook, (int)i);
	}

	// Nothing enabled: no children at all, the agent then keeps the player's input.
	Avoid::BuildLegitCandidates(MakeInput(1, 1), false, false, false, &vCandidates);
	EXPECT_TRUE(vCandidates.empty());
}

TEST(AvoidDecision, CandidateEnumerationsNormalizeTheCentreAim)
{
	std::vector<CNetObj_PlayerInput> vCandidates;
	Avoid::BuildLegitCandidates(MakeInput(0, 0, 0, 0, 0), true, true, true, &vCandidates);
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
	{
		EXPECT_FALSE(Candidate.m_TargetX == 0 && Candidate.m_TargetY == 0);
		EXPECT_EQ(Candidate.m_TargetY, -1);
	}

	Avoid::BuildBlatantCandidates(MakeInput(0, 0, 0, 0, 0), true, true, true, &vCandidates);
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
	{
		EXPECT_FALSE(Candidate.m_TargetX == 0 && Candidate.m_TargetY == 0);
		EXPECT_EQ(Candidate.m_TargetY, -1);
	}
}

TEST(AvoidDecision, BlatantCandidatesEnumerateTheFullCartesianSpace)
{
	// Reference spec 7.2: Blatant does not walk a preference list any more, it simulates the whole
	// action space - Dirs[3] = {0, -1, 1} x Hooks[2] = {0, 1} x Jumps[2] = {0, 1}. The axis order is
	// the reference order and it decides every survival tie, because the greedy search keeps the
	// first branch that reaches the best survival count.
	std::vector<CNetObj_PlayerInput> vCandidates;
	Avoid::BuildBlatantCandidates(MakeInput(1, 1), true, true, true, &vCandidates);

	const int aExpected[12][3] = {
		{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1},
		{-1, 0, 0}, {-1, 0, 1}, {-1, 1, 0}, {-1, 1, 1},
		{1, 0, 0}, {1, 0, 1}, {1, 1, 0}, {1, 1, 1}};
	ASSERT_EQ(vCandidates.size(), 12u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, aExpected[i][0]) << "candidate " << i;
		EXPECT_EQ(vCandidates[i].m_Hook, aExpected[i][1]) << "candidate " << i;
		EXPECT_EQ(vCandidates[i].m_Jump, aExpected[i][2]) << "candidate " << i;
	}
}

TEST(AvoidDecision, BlatantCandidatesOnlyOpenTheJumpAxisWhenTheAirJumpIsThere)
{
	std::vector<CNetObj_PlayerInput> vCandidates;

	// No air jump to spend: six branches, and the jump key the player is holding survives in all
	// of them - a grounded tee keeps its ground jump that way.
	Avoid::BuildBlatantCandidates(MakeInput(0, 0, 1), true, true, false, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 6u);
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
		EXPECT_EQ(Candidate.m_Jump, 1);

	// Both assist switches off: the player's own direction and hook survive, only the jump axis
	// remains. This is the state the agent has to be able to act in as well.
	Avoid::BuildBlatantCandidates(MakeInput(-1, 1, 0), false, false, true, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 2u);
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
	{
		EXPECT_EQ(Candidate.m_Direction, -1);
		EXPECT_EQ(Candidate.m_Hook, 1);
	}
	EXPECT_EQ(vCandidates[0].m_Jump, 0);
	EXPECT_EQ(vCandidates[1].m_Jump, 1);

	// Direction only: three directions times the two jump options, the hook state is kept.
	Avoid::BuildBlatantCandidates(MakeInput(1, 1), true, false, true, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 6u);
	const int aExpectedDirs[6] = {0, 0, -1, -1, 1, 1};
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, aExpectedDirs[i]);
		EXPECT_EQ(vCandidates[i].m_Hook, 1);
	}

	// Hook only: two hook states, no jump axis to open.
	Avoid::BuildBlatantCandidates(MakeInput(1, 0), false, true, false, &vCandidates);
	ASSERT_EQ(vCandidates.size(), 2u);
	for(size_t i = 0; i < vCandidates.size(); ++i)
	{
		EXPECT_EQ(vCandidates[i].m_Direction, 1);
		EXPECT_EQ(vCandidates[i].m_Hook, (int)i);
	}
}

TEST(AvoidDecision, ApproachInputWalksAndAimsAtTheTarget)
{
	// Auto drag and the unfreeze escape share this shape: raw aim offset, direction towards the
	// target, hook as requested.
	const CNetObj_PlayerInput Base = MakeInput(0, 0, 1, 100, 100);
	const CNetObj_PlayerInput Approach = Avoid::BuildApproachInput(Base, vec2(100.0f, 100.0f), vec2(220.0f, 60.0f), true);
	EXPECT_EQ(Approach.m_TargetX, 120);
	EXPECT_EQ(Approach.m_TargetY, -40);
	EXPECT_EQ(Approach.m_Direction, 1);
	EXPECT_EQ(Approach.m_Hook, 1);
	// Everything the caller did not ask to change stays exactly as it was.
	EXPECT_EQ(Approach.m_Jump, Base.m_Jump);

	const CNetObj_PlayerInput Left = Avoid::BuildApproachInput(Base, vec2(100.0f, 100.0f), vec2(20.0f, 100.0f), false);
	EXPECT_EQ(Left.m_Direction, -1);
	EXPECT_EQ(Left.m_Hook, 0);

	// A target almost straight above: the horizontal dead zone keeps the tee from jittering.
	const CNetObj_PlayerInput Above = Avoid::BuildApproachInput(Base, vec2(100.0f, 100.0f), vec2(105.0f, 40.0f), true);
	EXPECT_EQ(Above.m_Direction, 0);
	EXPECT_EQ(Above.m_TargetX, 5);
	EXPECT_EQ(Above.m_TargetY, -60);
}

// -------------------------------------------------------------------------------------------
// Pre-activation pipeline (reference spec 5)
// -------------------------------------------------------------------------------------------

TEST(AvoidDecision, GamemodeBlacklistMatchesTheReferenceSetExactly)
{
	// The four game types the reference refuses to run in (0x140532320). The comparison is a whole
	// string, case insensitive - a mode that merely mentions one of them is not blacklisted.
	EXPECT_TRUE(Avoid::IsBlacklistedGametype("fng"));
	EXPECT_TRUE(Avoid::IsBlacklistedGametype("FNG"));
	EXPECT_TRUE(Avoid::IsBlacklistedGametype("vanilla"));
	EXPECT_TRUE(Avoid::IsBlacklistedGametype("f-ddrace"));
	EXPECT_TRUE(Avoid::IsBlacklistedGametype("blockworlds"));

	EXPECT_FALSE(Avoid::IsBlacklistedGametype("DDraceNetwork"));
	EXPECT_FALSE(Avoid::IsBlacklistedGametype("ddnet"));
	EXPECT_FALSE(Avoid::IsBlacklistedGametype("fng something"));
	EXPECT_FALSE(Avoid::IsBlacklistedGametype(""));
	EXPECT_FALSE(Avoid::IsBlacklistedGametype(nullptr));
}

TEST(AvoidDecision, ProbeSafetyThresholdIsSevenOfTenTicks)
{
	// 0x14031226d: `cmp eax, 0x7; jge`. Six ticks of survival is danger (the agents are woken),
	// seven ticks is not, and the simulator's fully-safe sentinel always passes.
	EXPECT_FALSE(Avoid::ProbeIsSafe(0));
	EXPECT_FALSE(Avoid::ProbeIsSafe(6));
	EXPECT_TRUE(Avoid::ProbeIsSafe(7));
	EXPECT_TRUE(Avoid::ProbeIsSafe(9));
	EXPECT_TRUE(Avoid::ProbeIsSafe(9999));

	EXPECT_EQ(Avoid::PROBE_CHECK_TICKS, 10);
	EXPECT_EQ(Avoid::PROBE_SAFE_TICKS, 7);
}

TEST(AvoidDecision, SectorScanWalksTheWholeFieldOfViewInclusive)
{
	// Segments = 5 over a 90 degree opening: six rays from -45 to +45 degrees, one per step.
	const float Base = 0.0f;
	const float Fov = 1.5707963f; // 90 degrees
	const float Start = Avoid::SectorScanAngle(Base, Fov, 5, 0);
	EXPECT_NEAR(Start, -0.7853981f, 1e-5f);
	EXPECT_NEAR(Avoid::SectorScanAngle(Base, Fov, 5, 5), 0.7853981f, 1e-5f);

	// The rays are evenly spaced and the middle one keeps the player's own angle.
	EXPECT_NEAR(Avoid::SectorScanAngle(Base, Fov, 5, 1), -0.4712389f, 1e-5f);
	EXPECT_NEAR(Avoid::SectorScanAngle(Base, Fov, 5, 2), -0.1570796f, 1e-5f);
	EXPECT_NEAR(Avoid::SectorScanAngle(Base, Fov, 5, 3), 0.1570796f, 1e-5f);

	// The sweep is centred on whatever the player is already aiming at.
	EXPECT_NEAR(Avoid::SectorScanAngle(1.0f, Fov, 2, 1), 1.0f, 1e-5f);
}

// -------------------------------------------------------------------------------------------
// Legit post hoc gain arbitration (reference spec 8.3)
// -------------------------------------------------------------------------------------------

TEST(AvoidDecision, GainArbitrationNeedsAWholeExtraTick)
{
	constexpr int Horizon = Avoid::LEGIT_ARBITRATION_TICKS;
	EXPECT_EQ(Horizon, 26);

	// Both sides are measured inside the same horizon: the fully-safe sentinel becomes the horizon
	// itself, not 9999, otherwise the subtraction would be meaningless.
	EXPECT_EQ(Avoid::ArbitrationSurvival(9999, Horizon), Horizon);
	EXPECT_EQ(Avoid::ArbitrationSurvival(12, Horizon), 12);
	EXPECT_EQ(Avoid::ArbitrationSurvival(Horizon, Horizon), Horizon);

	// 0x140338a5f: `sub ebx, eax; cmp ebx, 0x1; cmovge`. One tick of extra survival is the
	// threshold; equal survival, a tie, or a worse candidate are all rejected.
	EXPECT_TRUE(Avoid::ArbitrationAllowsOverride(9999, 20, Horizon));
	EXPECT_TRUE(Avoid::ArbitrationAllowsOverride(21, 20, Horizon));
	EXPECT_FALSE(Avoid::ArbitrationAllowsOverride(20, 20, Horizon));
	EXPECT_FALSE(Avoid::ArbitrationAllowsOverride(19, 20, Horizon));
	EXPECT_FALSE(Avoid::ArbitrationAllowsOverride(9999, 9999, Horizon));

	EXPECT_EQ(Avoid::ArbitrationGain(9999, 20, Horizon), Horizon - 20);
	EXPECT_EQ(Avoid::ArbitrationGain(5, 9999, Horizon), 5 - Horizon);
}

// -------------------------------------------------------------------------------------------
// Airborne rescue mechanisms (reference spec 5.7 - 5.9, the v5.1 additions)
//
// These are the three rules that were missing from the module: the agent used to watch a player
// who never lets go of the hook swing into the freeze, could not spend an air jump on its own, and
// could not grab a ceiling the crosshair was not pointing at.
// -------------------------------------------------------------------------------------------

TEST(AvoidDecision, PreemptiveHookReleaseOnlyStealsTheKeyWhenLettingGoIsBetter)
{
	constexpr int CheckTicks = 26;

	// Holding the hook freezes the tee inside the window, letting go carries it across: steal.
	EXPECT_TRUE(Avoid::PreemptiveHookReleaseWins(12, 20, CheckTicks));
	// Releasing perfectly while holding on dies: the strongest case of the rule.
	EXPECT_TRUE(Avoid::PreemptiveHookReleaseWins(20, 9999, CheckTicks));
	// The two branches are tied or the release is worse: the player keeps the hook key.
	EXPECT_FALSE(Avoid::PreemptiveHookReleaseWins(12, 12, CheckTicks));
	EXPECT_FALSE(Avoid::PreemptiveHookReleaseWins(12, 11, CheckTicks));
	// Holding the hook already survives the whole window: there is nothing to rescue.
	EXPECT_FALSE(Avoid::PreemptiveHookReleaseWins(9999, 9999, CheckTicks));
	EXPECT_FALSE(Avoid::PreemptiveHookReleaseWins(CheckTicks, 9999, CheckTicks));
	// One tick short of a safe hold is already enough for the rescue to fire.
	EXPECT_TRUE(Avoid::PreemptiveHookReleaseWins(CheckTicks - 1, CheckTicks, CheckTicks));
}

TEST(AvoidDecision, RadarCastsTheFiveUpperHemisphereRays)
{
	// Reference spec 5.9: straight up, the two upper diagonals and both side walls. They are what
	// makes the rescue work while the player is staring at the floor.
	const vec2 *pDirs = Avoid::EmergencyRadarDirs();
	ASSERT_EQ(Avoid::EMERGENCY_RADAR_RAYS, 5);

	EXPECT_FLOAT_EQ(pDirs[0].x, 0.0f);
	EXPECT_FLOAT_EQ(pDirs[0].y, -1.0f);
	EXPECT_NEAR(pDirs[1].x, -0.7071f, 1e-4f);
	EXPECT_NEAR(pDirs[1].y, -0.7071f, 1e-4f);
	EXPECT_NEAR(pDirs[2].x, 0.7071f, 1e-4f);
	EXPECT_NEAR(pDirs[2].y, -0.7071f, 1e-4f);
	EXPECT_FLOAT_EQ(pDirs[3].x, -1.0f);
	EXPECT_FLOAT_EQ(pDirs[3].y, -0.2f);
	EXPECT_FLOAT_EQ(pDirs[4].x, 1.0f);
	EXPECT_FLOAT_EQ(pDirs[4].y, -0.2f);

	// Every ray of the fan points upwards: the radar never hooks deeper into the hazard.
	for(int i = 0; i < Avoid::EMERGENCY_RADAR_RAYS; ++i)
		EXPECT_LT(pDirs[i].y, 0.0f) << "ray " << i;

	// The reach is the maximum hook extension of the engine.
	EXPECT_FLOAT_EQ(Avoid::HOOK_MAX_DISTANCE, 380.0f);
}

TEST(AvoidDecision, RadarRejectsFreezingLethalAndUnhookableAnchors)
{
	EXPECT_TRUE(Avoid::RadarTargetIsHookable(TILE_SOLID));
	EXPECT_TRUE(Avoid::RadarTargetIsHookable(TILE_NOLASER));
	EXPECT_FALSE(Avoid::RadarTargetIsHookable(TILE_FREEZE));
	EXPECT_FALSE(Avoid::RadarTargetIsHookable(TILE_DFREEZE));
	EXPECT_FALSE(Avoid::RadarTargetIsHookable(TILE_LFREEZE));
	EXPECT_FALSE(Avoid::RadarTargetIsHookable(TILE_DEATH));
	EXPECT_FALSE(Avoid::RadarTargetIsHookable(TILE_NOHOOK));

	// The reference writes `Tile & (TILE_DEATH | TILE_FREEZE)`, a mask over 2 | 9 = 11. Applied to a
	// tile index that also rejects tile 1 (solid) and 11 (unfreeze), i.e. exactly the wall the radar
	// is looking for and the refuge an unfreeze escape wants. The rules compare indices instead.
	EXPECT_TRUE(Avoid::RadarTargetIsHookable(TILE_UNFREEZE));
	EXPECT_TRUE(Avoid::RadarTargetIsHookable(TILE_TELEIN));
	EXPECT_TRUE(Avoid::RadarTargetIsHookable(TILE_FINISH));
	EXPECT_FALSE(Avoid::IsLethalOrFreezingTile(TILE_SOLID));
	EXPECT_FALSE(Avoid::IsLethalOrFreezingTile(TILE_UNFREEZE));
	EXPECT_FALSE(Avoid::IsLethalOrFreezingTile(TILE_NOHOOK));
}

TEST(AvoidDecision, HeadroomVetoesTheAirJumpUnderACeiling)
{
	// Open sky: the probe hit nothing, the air jump is allowed.
	Avoid::SHeadroomProbe Clear;
	EXPECT_TRUE(Avoid::HeadroomAllowsAirJump(Clear));

	// A ceiling further away than the ray: still open enough.
	Avoid::SHeadroomProbe High;
	High.m_Hit = true;
	High.m_Distance = Avoid::AIR_JUMP_HEADROOM;
	High.m_HitTile = TILE_SOLID;
	EXPECT_TRUE(Avoid::HeadroomAllowsAirJump(High));

	// The threshold is one tile: 32 px is fine, anything closer bounces the tee back down.
	Avoid::SHeadroomProbe Exact;
	Exact.m_Hit = true;
	Exact.m_Distance = Avoid::AIR_JUMP_MIN_CLEARANCE;
	Exact.m_HitTile = TILE_SOLID;
	EXPECT_TRUE(Avoid::HeadroomAllowsAirJump(Exact));

	Avoid::SHeadroomProbe Low;
	Low.m_Hit = true;
	Low.m_Distance = Avoid::AIR_JUMP_MIN_CLEARANCE - 1.0f;
	Low.m_HitTile = TILE_SOLID;
	EXPECT_FALSE(Avoid::HeadroomAllowsAirJump(Low));

	// A ceiling that freezes or kills vetoes the jump no matter how far away it is.
	Avoid::SHeadroomProbe FreezingCeiling;
	FreezingCeiling.m_Hit = true;
	FreezingCeiling.m_Distance = Avoid::AIR_JUMP_HEADROOM;
	FreezingCeiling.m_HitTile = TILE_FREEZE;
	EXPECT_FALSE(Avoid::HeadroomAllowsAirJump(FreezingCeiling));

	Avoid::SHeadroomProbe DeadlyCeiling = FreezingCeiling;
	DeadlyCeiling.m_HitTile = TILE_DEATH;
	EXPECT_FALSE(Avoid::HeadroomAllowsAirJump(DeadlyCeiling));

	// Same for a lethal or freezing tile directly above the tee, which the ray may have missed.
	Avoid::SHeadroomProbe Above;
	Above.m_AboveTile = TILE_DFREEZE;
	EXPECT_FALSE(Avoid::HeadroomAllowsAirJump(Above));
	Above.m_AboveTile = TILE_DEATH;
	EXPECT_FALSE(Avoid::HeadroomAllowsAirJump(Above));
	Above.m_AboveTile = TILE_SOLID;
	EXPECT_TRUE(Avoid::HeadroomAllowsAirJump(Above));

	// The probe geometry of the reference: 48 px of headroom, one tile of minimum clearance.
	EXPECT_FLOAT_EQ(Avoid::AIR_JUMP_HEADROOM, 48.0f);
	EXPECT_FLOAT_EQ(Avoid::AIR_JUMP_MIN_CLEARANCE, 32.0f);
}

TEST(AvoidDecision, AirJumpAndRadarKeepTheirReferenceThresholds)
{
	// Reference spec 5.8: the injected jump has to buy at least 8 ticks over doing nothing.
	EXPECT_EQ(Avoid::AIR_JUMP_MIN_GAIN_TICKS, 8);
	// Reference spec 5.9: a radar candidate has to survive strictly more than 10 ticks.
	EXPECT_EQ(Avoid::RADAR_MIN_SURVIVAL_TICKS, 10);
}
