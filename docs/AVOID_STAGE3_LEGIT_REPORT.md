# 避障 Legit（拟真）模式交付报告（v1.2.0）

> 版本：**v1.2.0 交付 + v1.2.1 修补（感知半径半格化）**；v1.2.1 见文末第 9 节。
>
> 对应任务书：[`AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md)
> 实现说明：[`AVOID_TECHNICAL_DOCUMENTATION.md` 6.8](AVOID_TECHNICAL_DOCUMENTATION.md#68-legit-代理实现说明v120)
> 游戏内验收步骤：[8.3b](AVOID_TECHNICAL_DOCUMENTATION.md#83b-legit-代理验收v120-起)
> 交付日期：2026-10-02 · 分支 `feature/tas` · 版本 **v1.2.0**

---

## 0. 一句话结论

Legit 已实现并通过全部离线回归（3 个测试套件 / 20 个用例全绿，含阶段二 Basic 的 8 个用例**零退化**）：

* 候选空间 `方向(3) × 跳跃(2) × 钩子(3) = 18`；
* **钩子释放**以"松钩候选的固定序列能否活满 `check_ticks`"为唯一判据；
* **UCT 搜索**：`quality` = 迭代次数、`randomness` = 探索常数 + rollout 分支概率、三个 Weight = 打分项；
* **其他玩家预测**走"完整 `CWorldCore` + 影子快照"这条路（`TickDeferred()` 的真实互撞与钩索拖拽）；
* `bc_avoid_unfreeze_ticks`、身法状态门（喷气 / 钩人 / 飞锤）、`Plan` 字段与本地化理由全部回填；
* 最坏情况（必死 + 8 个预测玩家 + `quality 200`）实测 **1.01 ms / 决策**，预算 1.5 ms。

**需要人工做的一步**：本文第 6 节的游戏内验收表（我无法在这个环境里开游戏）。
所有能离线证明的部分都已经用合成地图测试钉死，游戏内表格只是"照着点一遍"。

---

## 1. 改了哪些文件、各多少行

### 新增

| 文件 | 行数 | 内容 |
| :--- | ---: | :--- |
| `src/game/client/components/bestclient/avoid_engine.h` | 264 | 数据契约（`SSettings`/`SThreat`/`SContext`/`SInputPlan`，字段与 v1.1.1 **完全一致**）、感知层声明、身法门、`SSimState`/`SEnvironment`、`CPlanner` |
| `src/game/client/components/bestclient/avoid_engine.cpp` | 671 | 感知层唯一实现、共享模拟器、身法门、**Legit 候选生成 + UCT 搜索 + 玩家预测注入** |
| `src/test/avoid_legit_test.cpp` | 876 | 12 个用例（含"局面确实如设计"的前置断言与成本基准） |

### 修改

| 文件 | +/- | 说明 |
| :--- | :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` | +41 / −87 | 契约搬到 `avoid_engine.h` 并用 `using` 别名保持 `CAvoid::SSettings` 等名字；`HAZ_*` 数值改为引用共享枚举；新增 `PlanLegit()` / `BuildEnvironment()` / `FlyHammerState()` / `m_Planner` |
| `src/game/client/components/bestclient/avoid.cpp` | +154 / −242 | 感知层与模拟器改为转调（唯一实现）；新增身法门 + 代理分派；日志补 `plans` / `plan d/j/h` / `cost`；`avoid_status` 补配置警告。**Basic 算法逐行未动** |
| `src/test/avoid_legit_test.cpp`（登记） | +1 | `CMakeLists.txt` 的 `TESTS` 列表 |
| `CMakeLists.txt` | +5 | `avoid_engine.cpp/.h` 进 `GAME_CLIENT` 与 `TESTS_EXTRA`（客户端与 testrunner 各编译一次） |
| `data/BestClient/languages/simplified_chinese.txt` | +76 | 19 条新词条（含空行） |
| `data/BestClient/languages/russian.txt` | +76 | 同上 |
| `scripts/avoid_selfcheck.sh` | +54 / −16 | 6 步检查；新增引擎接线检查、19 条词条检查、`CAvoidLegitTest` 纳入回归 |
| `docs/AVOID_TECHNICAL_DOCUMENTATION.md` | +400 / −87 | 版本 1.2.0；新增 [6.8](#68-legit-代理实现说明v120)、[8.3b](AVOID_TECHNICAL_DOCUMENTATION.md#83b-legit-代理验收v120-起)、7.0 参数接线表；更新 0.1/0.2/6.4/6.7/8.5/8.7/9/10/12、附录 B/C/D/F |
| `docs/AVOID_STAGE3_LEGIT_PROMPT.md` | +6 | 顶部标注"本档已交付（v1.2.0）" |
| `docs/AVOID_STAGE3_LEGIT_REPORT.md` | 新增 | 本报告 |

**接口冻结的遵守情况**：`SContext` / `SInputPlan` / `SThreat` / `SSettings` 的**字段一个没加、没删、没改**（只是换了定义文件，`CAvoid::` 名字通过 `using` 别名继续可用）；`CAvoid::ApplyInput()` 的调用约定与写回方式未动；`Plan.m_Input` 仍然是从 `Ctx.m_Input` 复制后修改；危险判定与模拟器都只有一份实现。

---

## 2. 候选空间与搜索的确切定义

### 候选（`Avoid::CPlanner::BuildRoots()`）

* **数量**：最多 `3 × 2 × 3 = 18`（`MAX_ROOT_ACTIONS = 24` 留余量）。
* **组成**：
  * 方向 `{玩家方向} ∪ (direction_assist ? {-1, 0, +1} : {})`
  * 跳跃 `{玩家值, 另一值}`（受身法门约束，不受任何开关约束）
  * 钩子 `{玩家值} ∪ (hook_assist ? {0, 1} : {})`
* **顺序即优先级**：玩家自己的取值永远排第一个，所有并列由"先生成者赢"解决 → "什么都不改"在结构上永远优先。
* **偏离代价**（权重只在"活一样久"或"用存活换手感"时起作用）：

  ```
  Deviation = DirectionWeight × |候选方向 − 玩家方向|
            + HookWeight      × (钩子改变)
            + 60              × (跳跃改变)        ← 内部常量 JUMP_DEVIATION_WEIGHT
  ```

### 搜索（`Avoid::CPlanner::Plan()`）

```
迭代上限 Iterations = clamp(bc_avoid_quality, 1, 200)
探索常数 c          = bc_avoid_randomness / 100
分支概率 p          = bc_avoid_randomness / 400
rollout 深度        = Horizon（见下），命中危险立刻 return
Horizon             = bc_avoid_check_ticks（最近威胁是解冻块时改为 bc_avoid_unfreeze_ticks）
```

1. **迭代 0 = 玩家自己的输入**，它就是 Basic 的快路径 + `kick_in_ticks` 前置闸门：
   `≥ Horizon` → 不干预（`player input safe`）；`≥ kick_in_ticks` → 不干预（`still time before the hazard`）。
2. **首次访问某候选**（`m_Visits == 0`）= **固定候选序列**：该动作重复整段 `Horizon`，逐帧 `ClassifyPoint()`，命中危险立即剪枝 → 记 `CanonicalSafe`。
   `quality ≥ 18` 时 18 个候选各被访问一次（完整穷举）；`quality = 1` 时只有迭代 0 发生（快而笨）。
3. **再次访问** = **探索 rollout**：tick 0 仍是该候选动作，tick 1 起每帧以概率 `p` 换成随机候选；序列在 rollout 前抽好，不根据模拟状态重规划。
   PRNG 是成员 `CPrng`，每次决策用 `(tick, 常量)` 播种 → **同一 tick 同一输入必得同一计划**（`SearchIsDeterministicPerTick`）。
4. **UCT 选择**（决定下一次访问谁）：`MeanScore + c × sqrt(ln(N+1) / n)`，
   `MeanScore = LifeWeight × 平均存活 − Deviation`。
5. **最终排序（关键设计决定）**：用 `RankingScore = LifeWeight × CanonicalSafe − Deviation`，**不是平均值**。
   探索 rollout 的乐观结果不能替代"这个动作重复下去会怎样"的保守估计，否则"提前松钩"会退化成"等下一帧再救"。
6. **资格与接管**：候选需 `CanonicalSafe ≥ PlayerSafe`；赢家需 `CanonicalSafe ≥ Horizon`（正常接管）或 `CanonicalSafe > PlayerSafe && bc_avoid_nsif`（NSIF 兜底）。赢家是迭代 0 时一律不接管。
7. **性能护栏**：每轮迭代前检查墙钟 `SEARCH_BUDGET_MS = 1.0 ms`，超了就用当前结果收尾。
8. **零堆分配**：候选表 / 克隆体 / 影子数组都是定长成员数组；唯一的 `std::vector`（影子世界的开关状态）容量跨决策复用。

---

## 3. 每个新接线的参数：改了什么 + 怎么证明

| 参数 | 在 Legit 里改了什么 | 证据（测试名 / 游戏内步骤） |
| :--- | :--- | :--- |
| `bc_avoid_quality` | 迭代次数 → 候选覆盖 + 探索深度 | `QualityDrivesSearchEffortAndTheDecision`：**1 → `plans 1`、不干预**；24 → 覆盖 18 个候选并松钩；200 → 迭代更多、耗时更高。游戏内：8.3b 第 4 行 |
| `bc_avoid_randomness` | UCT 探索常数 `c` + rollout 分支概率 `p` | `RandomnessChangesTheSearchWithoutBreakingIt`：`0` 两次运行逐位相同；`200` 的搜索回报与耗时不同、不越预算、松钩结论不变 |
| `bc_avoid_direction_weight` | 偏离代价的方向项 | `HookWeightChangesTheHookDecision` 的同族打分（方向项参与排序）；游戏内：8.3b 第 5 行 |
| `bc_avoid_hook_weight` | 偏离代价的钩子项 → **松不松钩** | `HookWeightChangesTheHookDecision`：`hook_weight 0` → `m_Hook = 0` 且活满前瞻；`200` → `m_Hook = 1` 且 `SafeTicks < check_ticks`（明确断言两者不同） |
| `bc_avoid_life_weight` | 存活与偏离的交换比 | 同一用例用它做对照（低生命优先级时才让钩子权重说话）；游戏内：8.3b 第 5 行 |
| `bc_avoid_hook_assist` | 候选里有没有"松开 / 按下钩子" | 关闭后 `Plan.m_Input.m_Hook` 恒等于玩家值（候选集只含玩家钩子状态） |
| `bc_avoid_direction_assist` | 候选里有没有别的方向 | 关闭后 `Plan.m_Input.m_Direction` 恒等于玩家值 |
| `bc_avoid_player_prediction` | 是否注入影子玩家 | `PlayerPredictionChangesTheDecision`：注入后被撞进坑 → 提前反应；关闭后与"场上没有任何其他玩家"给出**逐字相同**的计划与理由 |
| `bc_avoid_unfreeze_ticks` | 最近威胁是解冻块时的前瞻 | `UnfreezeTicksSetTheLookaheadNearUnfreezeTiles`：同一局面 `8` → 不干预且 `ScannedTicks == 8`；`26` → 干预并活满 |
| `bc_avoid_nsif` | 活不满前瞻时是否兜底 | 候选打分与接管条件的第二个分支（Legit 生效；Basic 保持 v1.1.1 行为） |
| `bc_avoid_check_ticks` / `kick_in_ticks` | 前瞻窗口 / 介入阈值 | 与 Basic 同一套语义的快路径；`avoid_status` 会在 `kick_in ≥ check_ticks` 时打印本地化警告（附录 F.4 第 1 条的建议已落地） |
| `bc_avoid_sensing_radius` | 介入距离闸门（所有代理共用） | 沿用 `CAvoidTest`/`CAvoidSensingRadiusTest`；Legit 走同一个闸门 |
| `bc_avoid_tile_*` | 什么算危险 | 感知与模拟共用一份 `IsRelevantHazard()`，`avoid_sensing_radius_test` 与游戏内 8.3 第 7 行 |

**不是**本阶段接线的参数（属于阶段四 Blatant，界面里也有独立面板，Legit 完全不读）：
`bc_avoid_track_point` / `safe_aim_tracking` / `auto_drag` / `aimbot` / `aimbot_mode` / `aimbot_segments` / `aimbot_fov`。

---

## 4. 钩子释放的判据与"松钩后能活满"的验证方式

### 判据（代码：`CPlanner::Plan()` + `Rollout()`）

**唯一判据是模拟结果，不是"钩索落点是否危险"**，三步缺一不可：

1. 玩家自己的输入（按住钩 + 当前方向）跑满 `Horizon` → 死在第 k 帧，且 `k < kick_in_ticks`（否则走快路径/介入阈值直接返回）；
2. 某个"松开钩子"的候选（`m_Hook = 0`，方向 / 跳跃任取）的**固定序列**（该动作重复整段前瞻）**活满 `Horizon`**；
3. 它的 `RankingScore = LifeWeight × CanonicalSafe − Deviation` 最高——松钩要付 `HookWeight` 的偏离代价，所以只有"确实更活"才赢得过"保持钩 + 反向减速"。

满足时写 `Plan.m_Input.m_Hook = 0`，理由走 `BcLocalize("release hook…")`；
活不满前瞻时进入 NSIF 分支（红徽章）并加 `NSIF: ` 前缀。

### 验证方式

| 方向 | 用例 | 断言要点 |
| :--- | :--- | :--- |
| 该松就松 | `HoldingAHookThatDragsIntoTheHazardIsReleased` | 先断言"玩家输入必死"且"保持钩怎么按方向键都活不满"（局面有效性），再断言 `m_Override`、`m_Hook == 0`、`m_SafeTicks ≥ check_ticks`、`m_UsedFallback == false`、理由含 `release hook` |
| 不该松就不松 | `ReleasingTheHookIsRefusedWhenItWouldFlyIntoTheHazard` | **逐个方向**断言"松钩会飞进危险"（`SimulateFixed(方向, 跳=0, 钩=0) < Horizon`），再断言代理保持 `m_Hook == 1` 且 `SafeTicks > PlayerSafe` |
| 确定性 | `SearchIsDeterministicPerTick` | 同一 tick 两次调用：动作、存活、迭代数、得分、理由全部相同 |

几何都是合成地图（内存里逐图块摆放），所以"活满 / 活不满"是精确断言，不依赖任何出厂地图。
两条用例都用**同一个公开模拟器** `Avoid::SimulateFixed()` 做前置断言，因此"代理看到的物理"和"测试断言的物理"不可能错位。

---

## 5. 其他玩家预测：走的是哪条路、偏差多大、怎么验证

**走的是第二条路：完整 `CWorldCore`（不是静态障碍）。**

* **采集**（`CAvoid::BuildEnvironment()`）：感知半径 + 2 图块内**最近**的 ≤8 个其他 tee（`m_aClients[].m_Predicted`），跳过自己 / 观战 / 未激活。
* **注入**（`SSimState::Init()`）：克隆体拿到**私有** `CWorldCore`，`m_apCharacters[id]` 指向影子 `CCharacterCore` 快照；克隆自己坐在**真实 client id** 上，于是 `CanCollide()` / `CanKeepHook()` / `IsSwitchActiveCb()`（开关门）用的都是真实队伍与真实地图状态；真实地图的 `m_vSwitchers` 拷进私有世界（容量复用，不分配）。
* **推进**：每个 rollout tick 影子按快照速度匀速平移；`Tick(true, true)` 里的 `TickDeferred()` 与 `Move()` 因此**真的**发生角色互撞与钩索拖拽。
* **为什么不用真实世界**：`TickDeferred()` 会写它找到的 core（钩索拖拽力）并触发 antiping 回调，那会污染客户端自己的预测。

### 偏差（诚实清单）

| 偏差 | 量级 / 影响 | 缓解 |
| :--- | :--- | :--- |
| 影子不模拟自己的物理（匀速、可穿墙、不受重力） | 26 帧窗口内位置误差随速度增长（典型 8~15 px/帧 → 最多约 2~4 图块） | 只把它们当"会动的障碍物"；要更精确就得在影子上跑轻量物理（阶段四可选） |
| 最多 8 个影子 | 第 9 个玩家起完全不参与预测 | 取**最近**的 8 个（离得远的本来就不可能在窗口内撞到你） |
| 位置来自上一 tick 的预测核心 | 约 20 ms 滞后 | 与阶段二一致；对 26 帧窗口的判据无影响（6.7 第 4 条复核） |
| 开关门状态是拷贝 | 与真实一致（拷贝），但地图极大时拷贝有成本 | 副本容量跨决策复用；只在预测开启且地图有开关时发生 |
| `bc_avoid_player_prediction = 0` | **完全不注入**（连私有世界都不建），与 v1.1.1 逐位一致 | 有用例断言"关闭预测" == "场上没有任何其他玩家"；另外"开启预测但附近没人"也走同一条降级路径 |

### 验证

* `PlayerPredictionChangesTheDecision`：同一个"别的 tee 从左边把你推向坑"的局面 ——
  * 注入后 `SimulateFixed(站立不动) < check_ticks`（**先证明这一推是真的**）；
  * 关掉预测 → `player input safe`、`m_Override == false`，且与 `MakeEnvironment(false)`（场上没有任何玩家）计划与理由**逐字相同**；
  * 开启预测但附近没人 → 同样逐字相同；
  * 开启预测 → `m_Override == true`（提前反应）。
* 成本侧：见第 7 节，预测让一个 rollout 从 5.6 µs 涨到 22.7 µs（约 4×）。

---

## 6. 三条验收 + 四条 Legit 手感的实测结果

### 6.1 已经用离线测试钉死的部分（可复现命令见下）

| # | 验收项 | 结果 | 证据 |
| :--- | :--- | :--- | :--- |
| 1 | 勾向贴黑水的墙 / 按住钩被拉向危险 → 提前松钩 | ✅ 离线通过 | `HoldingAHookThatDragsIntoTheHazardIsReleased` |
| 1b | 松钩会飞进危险时不许松 | ✅ 离线通过 | `ReleasingTheHookIsRefusedWhenItWouldFlyIntoTheHazard` |
| 2 | 朝黑水走 → 接触前刹停（且比 Basic 更稳） | ✅ 逻辑不退化（共用同一前置闸门与快路径）；Legit 下还会选跳跃/松钩等更活的方案 | `QualityDrives*`、`LegitDecisionCost*`（同一局面 Legit 选 `release hook, brake before hazard` 且活满窗口） |
| 3 | 直行过窄通道不误触发 | ✅ 离线通过 | `SafeCorridorAndFlatWalkAreNeverTouched`（10 tick 零接管、零跳跃、零钩子）、`FlatGroundIsNeverTouched` |
| 4 | 要像是在自己玩：允许跳 / 允许钩墙 | ✅ 候选空间与打分；跳跃只在真能多活时才被选中（并列永远选玩家输入） | `QualityDrives*`（同样局面下不止一种可用方案）、`SafeCorridor*`（平地不鬼畜跳） |
| 5 | `Quality` 1 与 200 有可见差别 | ✅ 离线通过（1 = 不干预、24 = 完整覆盖、200 = 更多迭代 + 更高耗时） | `QualityDrivesSearchEffortAndTheDecision`、第 7 节实测 |
| 6 | 其他玩家撞你 → 提前反应；关闭则不反应 | ✅ 离线通过 | `PlayerPredictionChangesTheDecision` |
| 7 | 解冻块旁边介入时机由 `unfreeze_ticks` 决定 | ✅ 离线通过 | `UnfreezeTicksSetTheLookaheadNearUnfreezeTiles` |
| — | 红线：喷气 / 钩人 / 飞锤不动手 | ✅ 离线通过 | `SpecialMovementStatesAreHandsOff` |
| — | 逐 tick 确定性（无全局 RNG） | ✅ 离线通过 | `SearchIsDeterministicPerTick` |

```bash
ninja -C build DDNet && ninja -C build testrunner
./build/testrunner --gtest_filter='CAvoid*.*'      # 20/20 通过
./scripts/avoid_selfcheck.sh                       # 6 步全绿
```

### 6.2 需要人工在游戏内点一遍的部分（我无法在此环境启动客户端）

步骤与期望见 [文档 8.3b](AVOID_TECHNICAL_DOCUMENTATION.md#83b-legit-代理验收v120-起)，这里给出最短路径：

```bash
bc_avoid_agent 1          # 拟真
bc_avoid_active 1         # 或按绑定键 / avoid_toggle
bc_avoid_log 1            # 每次决策一行日志，含 plans / plan d/j/h / cost
avoid_status              # 状态行 + kick_in/check_ticks 警告（若配置有陷阱）
```

| 看什么 | 期望 |
| :--- | :--- |
| 勾住墙、按住钩被拖向黑水 | 贴墙前 `plan d/j/h` 的 `h` 变 0，`reason` = `松开钩子…`；`接管` +1 |
| 朝黑水走 | `override yes`、`reason` = `在危险前刹停` / `向左减速`；必要时 `跳起…` |
| 直行窄通道 | `override no`、`player safe 26/26`、`接管` 不增长 |
| `Quality` 1 → 24 → 200 | 日志 `plans` 1 → 24 → 40+；`cost` 0.01 → ~0.4 → ~1.0 ms |
| 队友撞你 / 关掉"预测其他玩家" | 前者 `override yes`；后者 `player input safe` |
| 解冻块旁（打开解冻块图块） | 介入时机随 `Unfreeze lookahead` 改变 |
| 喷气 / 钩住队友 / `cl_dummy_hammer` 空中 | `reason` = `…，代理不介入`，输入零改写 |
| `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms |

---

## 7. `Plan.m_CostMs` 实测区间

基准：`CAvoidLegitTest.LegitDecisionCostStaysInsideTheTickBudget`（`-O3`），
最坏情况 = 玩家输入必死 + **8 个预测玩家** + `check_ticks 26` + 每次决策换一个 tick：

| `quality` | 实际迭代（`plans`） | 每次决策 | 说明 |
| ---: | ---: | ---: | :--- |
| 1 | 1 | **0.009 ms** | 只评估玩家自己的输入 → 不干预 |
| 24（默认） | 24 | **0.43 ms** | 18 个候选全覆盖 + 6 次探索 → 正确松钩并活满 |
| 200 | ≈45（随机器快慢在 44~48 之间） | **1.01 ms** | 撞上 1.0 ms 墙钟护栏后收尾，仍在 1.5 ms 内 |
| 200（关掉预测） | 200（达到迭代上限） | 0.97 ms | 同一预算买到 ≈4.5× 的迭代数：单 rollout ≈23 µs vs ≈4.8 µs |

参照：Basic 的最坏情况 0.024 ms/决策（`CAvoidSimulatorTest.WorstCaseDecisionCostStaysInsideTheTickBudget`）。
**代价只发生在"玩家输入本来就要死"的 tick**（例如按住钩被拖向黑水的那几帧）；
其余 tick 走快路径 = 一次模拟，与 Basic 同价。

---

## 8. 已知不足，以及留给阶段四（Blatant）的问题

### 本阶段的不足

1. **影子玩家不模拟自己的物理**（匀速平移、可穿墙、位置晚一 tick）；窗口越长偏差越大。
2. **最多 8 个影子**：第 9 个玩家起完全不参与预测（宁可漏判也不超预算）。
3. **没有瞄准**：按下钩子用的是玩家当前瞄准方向（Track Point / 内置瞄准属于阶段四）。
4. **探索 rollout 不参与最终排序**（保守性设计，第 2 节第 5 条）：`randomness` 只改变搜索过程与上报的 `Plan.m_Score`，不会让计划变冒险。想要"多条序列里挑最优"必须先给出"第一个动作本身安全"的证据，否则验收第 1 条的提前量会退化。
5. **`bc_avoid_nsif` 在 Legit 生效、在 Basic 被忽略**（有意保留阶段二验收行为）。
6. **`Plan.m_Candidates` 语义按代理不同**：Legit = 搜索迭代次数，Basic = 候选数；两者都符合"本帧评估的方案数"。
7. 游戏内验收表（第 6.2 节）尚需人工执行一次。
8. **与参考实现（KRX）的参数—机制对照**（哪一条对齐、哪一条是推断、哪 6 条是已知差异）见
   [技术文档 6.8.10](AVOID_TECHNICAL_DOCUMENTATION.md#6810-与参考实现krx的参数机制对照)：
   其中**角色传送落点不预测**、**heart tile 不模拟**是尚未实现的两条（阶段四可补），
   其余（预算护栏、8 个影子上限、预测开关粒度、NSIF 归属）是有意为之。

### 留给阶段四 Blatant 的问题

> 阶段四的任务书已经写好：[`AVOID_STAGE4_BLATANT_PROMPT.md`](AVOID_STAGE4_BLATANT_PROMPT.md)
> （瞄准候选 / Track Point / Safe Aim Tracking / Auto Drag / 内置瞄准 / NSIF 调优）。下面是它的输入清单。

1. **瞄准/落点搜索的接入点已经留好**：候选生成器在 `BuildRoots()`，只需把 `m_TargetX/Y` 纳入候选维度；但要注意候选数会从 18 涨到 18×N，必须重新评估预算（现在是 1.0 ms 护栏 + 18 个根）。
2. **`Track Point` 记忆**需要跨 tick 状态：现在 `CAvoid` 没有任何跨 tick 的决策状态（只有遥测），要新增就必须在 `OnReset()` 里清空，并说明"客户端一帧内可能多次预测"的约束。
3. **`Auto Drag`**：钩人所需的"队伍可见性"已经具备（预测路径用的是真实 `CTeamsCore`），但影子玩家必须先有可信物理，否则拖拽预测会不准。
4. **`NSIF 调优`**：现在 NSIF = "活不满前瞻但比玩家输入更好"就接管；Blatant 需要的"沿用已知最安全方案的第一步"更适合做成跨 tick 的记忆。
5. **性能**：`quality 200` 在预测开启时通常只能跑到 ~45 轮；如果阶段四要更深的搜索，建议先给 `SimulateFixed()` 的玩家碰撞路径做优化（当前 `Move()` 每帧遍历 64 个槽位是主要成本）。
6. **不要动的地基**：`Avoid::SSimState` 是三个代理的公共模拟器（[附录 F.2](AVOID_TECHNICAL_DOCUMENTATION.md#f2-每档必须留下的地基验收时会检查) 的硬性要求）。需要新物理就在克隆配置里加开关并补一条保真度用例，不要复制第二份物理步进。

---

## 9. v1.2.1 修补：感知半径半格化（用户反馈）

### 反馈
> "在平面上走向水坑时，感知半径 2，刚走近就自动跳了，太敏感。半径能不能以 0.5 为单位，最小 0.5？"

### 诊断（为什么半径 2 仍然显得早）
1. **旧语义是"图块索引正方形"**：`ScanThreat()` 扫描的是 tee 所在图块 ±r 的方块，只要方块里**存在**危险图块就开闸。半径 2 因此覆盖的是"索引差 ≤2"，实际到达距离是 1~2 格（≈60 px），而且对角方向会多覆盖 ≈41%。
2. **闸门一开就立刻决策**：`kick_in_ticks`（20）比到达距离对应的帧数（6 帧）大得多，所以代理一"看见"就出手；半径是唯一能推迟介入的滑条，而它的分辨率只有 1 格、下限 2 格。
3. **跳跃是合法解**：平面上走向 1~2 格宽的水坑时，"跳过去"能活满 `check_ticks`，而"刹停"不能，所以搜索必然选跳——半径越小、出手越晚，跳就越像"贴脸才突然跳"。

### 改动
| 项 | 之前 | 现在 |
| :--- | :--- | :--- |
| `bc_avoid_sensing_radius` 单位 | 图块（整数） | **半格**（`12` = 6 格，`1` = 0.5 格） |
| 范围 / 默认 | 2 ~ 16 / 6 | **1 ~ 32** / **12**（等价） |
| 到达判定 | 图块索引正方形内存在危险块 | 危险块盒到 tee 中心的**欧氏距离** ≤ 半径 |
| 引擎字段 | `int SSettings::m_SensingRadius` | `float`（图块数，半格步进） |
| 界面 | `Sensing radius` + `tiles` | `半径（半格）` + 提示说明 `12 = 6 格` |
| 旧配置 | — | `cl_config_version` 1 → 2，启动时自动 `×2`（**写在 `autoexec.cfg` 里的旧值需要自己改成两倍**） |

涉及文件：`config_variables_bestclient.h`、`config_variables.h`、`client.cpp`（迁移）、
`avoid_engine.h/.cpp`（`ScanThreat()` + 共享 `Avoid::DistanceToTileBox()`）、
`avoid.cpp`（`ReadSettings()`、玩家快照范围、威胁可视化环与高亮）、
`menus_avoid.cpp`（滑块与提示）、`src/test/avoid_sensing_radius_test.cpp`（4 → 6 用例）、
两个语言文件（+2 词条）、`scripts/avoid_selfcheck.sh`（+迁移与半格断言）。

### 实测到达距离（合成地图、平地全速 10 px/tick、`check_ticks = 26`）
| 配置值 | 半径 | 到达距离 | 折算 |
| ---: | ---: | ---: | ---: |
| 1 | 0.5 格 | 10 px | 1 tick（**刹不住**，"几乎关闭"档） |
| 2 | 1.0 格 | 30 px | 3 tick |
| 4 | 2.0 格 | 60 px | 6 tick（≈ 旧半径 2） |
| 12（默认） | 6.0 格 | 190 px | 19 tick（≈ 旧半径 6） |
| 16+ | 8 格以上 | 240 px | 24 tick（改由 `check_ticks` 决定） |

### 验证
* `CAvoidSensingRadiusTest` 6/6：全档位（1~32 半格）刹停、半格单调性（2 → 2.5 → 3 格到达距离严格递增）、0.5 格明显晚于 1 格且已来不及刹停；
* `CAvoidLegitTest` 12/12、`CAvoidSimulatorTest` 4/4、全套 **387/387** 通过；
* `./scripts/avoid_selfcheck.sh` 6 步全绿（新增"迁移必须存在 + 新配置默认版本必须是 2 + 半格距离判定"三条断言）。

### 还需要你验一次
游戏内把 **半径（半格）** 依次设为 12 → 4 → 2 → 1，走向水坑：应当一级比一级更晚介入，配置值 1（0.5 格）基本等于"不管"。
另外注意：**旧配置第一次启动会被自动 ×2**，如果你在 `config` 里看到 `bc_avoid_sensing_radius 12`，那就是等价于以前的 6。

---

## 10. v1.2.2 修补：跳跃降级为最后手段 + 感知椭圆化（用户实测反馈）

### 反馈
1. 勾向"带黑水的竖直墙"时代理居然**起跳**；期望是"断勾 + 方向键阻止"，若玩家仍下坠，再在危急关头自动出勾挂到黑水外的墙。
2. 走向"向下的水坑"时，第一块砖就跳；期望是"玩家真的快进水时才跳"。
3. 感知范围是**圆**不合理：水平速度通常大于竖直速度，横竖应当分开。

### 诊断（为什么以前会跳）
1. **跳是当时"最便宜"的解**：三个权重是*偏离代价*，而跳跃的内部代价只有 60，低于"改方向(100)"和"改钩子(100)"。
   在"跳也能活满、松钩+刹车也能活满"的局面里，`LifeWeight·存活` 相等，于是搜索选**偏离最小**的那个——跳。
   这正是截图里的情形：勾着墙被拖向墙根黑水，刹车要改方向+松钩（代价 200），跳跃只付 60。
2. **没有"时机"概念**：只要玩家自己的输入在 `check_in_ticks`(20) 内会死，代理就立刻在全部候选里挑最优，
   而"挑最优"不区分"现在跳"和"再等 5 帧跳"——两者在模拟里都是活满窗口。
3. **感知是圆的**：正下方的坑和正侧方的墙在同一个半径内被同等对待，但横向能走的距离是竖直的 ~1.5 倍。

（顺带确认：死亡判定不是误报。引擎自己的 `CCharacter::HandleSkippableTiles()` 用的就是
`±GetProximityRadius()/3 ≈ ±9.33 px` 的四角探针，与本模块完全一致——贴近黑水边缘时确实会死，
问题在"何时动手"和"动手做什么"，不在探测点。）

### 改动
| # | 改动 | 说明 |
| :--- | :--- | :--- |
| 1 | **钩索期间不加跳** | 按住钩或 `HOOK_GRABBED/FLYING` 时，所有"把跳跃从 0 变 1"的候选标为不可用；绳索问题只能靠松钩/按钩/方向键解决 |
| 2 | **跳跃只在危急关头** | 仅当玩家自己输入的存活帧数 ≤ `JUMP_URGENCY_TICKS = 6` 时才放开跳跃；否则宁可这一帧不动手，把跳跃留到"再不动手就死" |
| 3 | **感知椭圆化** | `ReachY = ReachX × clamp(0.5·gravity·T² / (ground_control_speed·T), 0.25, 1)`（默认 0.65）；每轴独立判据，横竖互不干扰；可视化画同一个椭圆 |
| 4 | **HUD 说明** | 被拦下时理由写 `没有更安全的方案（跳跃留作最后手段）`（中/俄本地化） |

有意**没有**做：把跳跃代价从 60 调大。用调参数值去解决"跳跃太吵"是脆的（用户把权重拉满就失效），
所以改成结构性规则：不可用 = 不搜索、不排序、不可能被选中。

### 实测（`CAvoidLegitTest`，新增 3 个用例）
| 局面 | 结果 |
| :--- | :--- |
| 自由落体，距死亡 12 帧 | 不接管，理由 `no safer plan (jump is a last resort)` |
| 自由落体，距死亡 4 帧 | 接管，`m_Jump = 1`，活满 26 帧 |
| 同样 4 帧 + 按住钩（锚点 20 px 内，无拖拽力） | 不接管、不加跳（与上一条物理完全相同，只差策略） |
| 半径 6 图块：150 px 侧向 / 150 px 下方 / 75 px 下方 | 看见 / 看不见 / 看见，垂直系数实测 **0.65** |
| 性能（必死 + 8 个预测玩家） | Quality 24 → 0.40 ms；200 → 1.01 ms；关预测 200 轮 → 0.87 ms |

### 验证
`CAvoidLegitTest` **15/15**、`CAvoidSensingRadiusTest` 6/6、`CAvoidSimulatorTest` 4/4、全套 **390/390** 通过；
`./scripts/avoid_selfcheck.sh` 6 步全绿（新增"跳跃策略必须存在"与"感知必须是 tuning 推导的椭圆"断言）。

### 还欠你的一半：坠落时的"自动出勾挂墙"
现在的链路是：**断勾优先 → 方向键制动 → 危急关头才跳**；
"按下钩子"本来就在候选集里，但**用的是你当前的瞄准方向**——代理不会自己找墙。
要稳定实现"自动出勾挂到黑水外的墙"，需要在候选里加入**瞄准角搜索 + 可勾性判定**
（射线打到实心且非 `TILE_NOHOOK` 的面才算），这属于阶段四 Blatant 的 Track Point / 内置瞄准，
并且会让候选数从 18 涨到 18×N，需要先重新评估 1.0 ms 搜索预算。
**要不要现在做？**（这是你一句话就能定的方向问题，我没有擅自改 Legit 的定位。）

---

## 11. 验收标准（"拟真"做到什么程度算好）

见 [`AVOID_LEGIT_ACCEPTANCE.md`](AVOID_LEGIT_ACCEPTANCE.md)。要点：

* **定位**（参考实现原话）：subtle assistance · relies partially on user skill · **can sometimes fail**；
* **五条原则**（冲突时按序裁决）：不造成伤害 > 不打扰 > **最后一刻出手** > 动作由轻到重 > 可调且忠实；
* **评分卡** A 安全地板 / B 不打扰 / C 自然度 / D 感知与参数 / E 性能与稳定，每项都有测法与目标值；
* **当前唯一未达标的硬指标是 B2"介入提前量"**：现在等于 `min(感知闸门, kick_in 20)`，
  半径 12 时提前约 19 帧（0.38 s）——这正是三轮反馈的共同根源。
  建议的下一步是**"最后一刻原则"**：对每个方案算出"最晚可行帧 d\*"（先按玩家输入走 d 帧再执行仍能活满窗口），
  只在 `d* == 0` 的那一帧接管。属于 Legit 范围内，不涉及瞄准，也不需要新 cvar。
* **缺仪表**：接管率 / 接管段数 / 动作强度分布 / 抖动次数还没有量化基线（建议做一个离线"手感基准"脚本）。

---

## 12. v1.2.3：参数灵敏度台架（"这些滑条到底有没有用"）

用户提问："检查帧数和计算质量等等这些参数真的有效果吗？"
——本项目栽过一次"参数没接线"（v1.1.1 的感知半径），所以这次不靠"用例过了"回答，直接上台架。

### 台架是什么
`CAvoidLegitTest.ParameterSensitivitySweep`：一个"走神的玩家"以固定输入朝水坑走 70 帧，
每帧跑**客户端同款决策链**（`ScanThreat()` → 感知闸门 → `CPlanner::Plan()`），代理可随时接管；
逐档扫描每个参数并打印可观察量：

```bash
./build/testrunner --gtest_filter='CAvoidLegitTest.ParameterSensitivitySweep' 2>&1 | grep avoid-params
```

列：`survived` / `died@`（死亡帧）/ `overrides`（接管帧数）/ `jumps`（新增跳跃）/ `releases`（松钩）/
**`lead`（首次接管时玩家自己的输入还剩多少帧 = 提前量）** / `plans`（迭代数）/ `cost`（70 帧累计）。

### 结论：**全部接线，且每一档都有可观察差别**

| 参数 | 扫描 | 观察到的差别 |
| :--- | :--- | :--- |
| `check_ticks` 前瞻 | 4→50 | 提前量 3 → 7 → 11 → 18（≥20 后被 `kick_in` 卡住）；耗时 0.16 → 39 ms |
| `quality` 迭代 | 1→200 | **1 = 完全不接管**（第 18 帧掉进水里）；8/18/24 立刻恢复正常；迭代数 = 设定值；耗时 0.07 → 64 ms |
| `kick_in_ticks` | 0→50 | **0 = 完全不等待**（v1.2.3 修正，以前等于"关闭"）；4 → 提前量 3 且唯一一次是**跳跃**；10 → 9；≥20 → 18 |
| `sensing_radius` | 0.5→16 格 | 0.5 → 提前量 0 且**救不回来**（符合设计）；1 → 靠跳跃救回（提前量 1）；2 → 4；4 → 11；≥6 → 18 |
| `life_weight` | 0→200 | **0 = 不接管**（第 18 帧死）；10 → 提前量 16；≥50 → 18 |
| `randomness` | 0→200 | 决策不变、**耗时 23 → 71 ms**：探索让 rollout 不再提前剪枝（这就是参考实现"人多/随机高会掉帧"的机制） |
| `nsif` | on/off | 无解局面：on → 接管一次（多活 13 帧）；off → **完全不接管** |
| `direction_assist` | off | 只剩跳跃一条路：提前量 6 时起跳，存活 |
| `hook_assist` | off | 该局面玩家没按钩 → 行为不变（符合预期，钩子相关的 A/B 在松钩用例里） |
| `tile_death` | off | 水坑不再是危险 → **0 次接管** |

用例里钉住 6 条硬性差别（`quality 1` / `life_weight 0` / `tile_death off` 都不能接管；
半径越大、前瞻越长，提前量必须越大），其余作为可对比基线输出。

### 顺带修掉的一个真 bug
`kick_in_ticks = 0`：判据是 `PlayerSafe >= KickIn`，而任何数都 `>= 0`，所以滑条**最低档静默等于"永不介入"**，
与标签语义相反。v1.2.3 起是 `KickIn > 0 && PlayerSafe >= KickIn`，最低档 = "完全不等待"（Legit 与 Basic 同步）。

### 验证
`CAvoid*` 26 个用例、全套 **391**、`./scripts/avoid_selfcheck.sh` 全绿。

---

## 13. v1.2.4：拟真页"恢复默认"方形按钮

**需求原文**："在拟真设置界面增加一个小方形按钮，是恢复默认参数。"

### 实现
| 项 | 做法 |
| :--- | :--- |
| 位置 | 右侧参数面板**标签栏的最右端**（拟真页内，不占用已经吃满的纵向预算） |
| 外观 | 小方形按钮 + 短标签 `恢复默认`（中）/ `Defaults`（英）/ `Сброс`（俄）；点击后 2 秒内显示 `已恢复` |
| 行为 | 依次调用 `IConfigManager::Reset("bc_avoid_xxx")`，由配置系统把每一项设回**编译期默认值** |
| 默认值来源 | **不复制默认值**：表里只有配置项脚本名，值永远来自 `config_variables_bestclient.h`，不会漂移 |
| 覆盖范围 | 该模式面板上显示的参数 + 该模式实际读取但滑条在别处的参数（拟真另含 `kick_in_ticks`、`nsif`；基础另含方向辅助/前瞻/介入阈值/权重/挂机保护）—— 共 2 张表 32 条 |
| 明确不动 | `bc_avoid_agent`（模式）、`bc_avoid_active`（启停）、`bc_avoid_enabled`（模块开关）、`show_hud` / `show_visuals` / `log` / `debug_override` |
| 其它模式 | 激进 / Fentbot / Pilot 暂时不画按钮（算法未实现，画了会让人以为那些参数在起作用），阶段四补表即可 |
| 反馈 | 滑块当帧跳回默认值 + 控制台回显 `避障：参数已恢复为默认值` + 标签短暂变为"已恢复" |

### 防漂移
自检脚本新增 `[5b/6]`：断言按钮存在（`AvoidDefaultParams(` / `pConfigManager->Reset(`），
并把两张表里的**每个名字**拿去 `config_variables_bestclient.h` 里核对 —— 写错一个名字，
`Reset()` 只会在运行时 `log_error` 然后**静默什么都不做**，这是必须被脚本挡住的一类问题。
当前输出：`ok: 2 tables, 32 entries`。

### 验证
`ninja -C build DDNet` 通过；`CAvoid*` 26 用例、全套 **391**、`./scripts/avoid_selfcheck.sh` 6 步全绿。

### 需要你在游戏里点一下确认
1. 拟真页把 `Quality` 拉到 200、半径拉到 32、`Life priority` 拉到 0 → 点方形按钮 → 三者应立刻回到 24 / 12 / 150；
2. 控制台应出现 `避障：参数已恢复为默认值`，按钮标签 2 秒内显示"已恢复"；
3. 切到基础点一次 → 只影响基础读取的那批参数，**模式与启停状态不变**；
4. 重启客户端 → 恢复后的值被持久化（配置里能看到）。

---

## 14. v1.2.5：感知半径默认值改为 2（= 1 图块）

**用户要求**："状态感知半径默认设置成 2，因为这个状态手感最好。"
（v1.2.1 起滑条单位是**半格**，所以 2 = 1.0 图块 ≈ 32 px；已与用户确认过单位。）

| 项 | 之前 | 现在 |
| :--- | :--- | :--- |
| `bc_avoid_sensing_radius` 默认值 | 12（= 6 图块） | **2（= 1 图块）** |
| 出厂手感 | 提前发现、提前刹车（提前量 ~19 帧） | **平时完全不打扰，只在最后一刻动一下**（提前量 ~1–3 帧） |
| 已有配置 | — | **不受影响**（迁移只做过一次，保留用户自己的值）；点面板标签栏右端的 **恢复默认** 即可拿到新默认 |

**代价（写在文档里，不藏）**：1 格 = 30 px ≈ 3 帧，不够刹停（刹停要 5 帧 / 25 px），
所以默认档更多依赖"最后关头的跳跃 / 松钩"，而不是"提前减速"。
想要"提前刹停"的稳感把滑条调到 **4（2 格）以上**：台架显示半径 4 格起就以刹车为主、不再跳跃。

**改动**：cvar 默认值与描述；技术文档的单位/默认对照表、实测到达距离表（标注新默认与代价）、
游戏内验收第 8 行的操作顺序（现在从默认 2 往上调，验证"越调越早"）；自检脚本的默认值断言；
评分卡 B2/D4 的说明（默认档是用户拍板的"最后一刻"预设，B2 的目标区间对应"提前刹停"档位）。

**验证**：`ninja -C build DDNet testrunner` 通过；`CAvoid*` 26 用例、`./scripts/avoid_selfcheck.sh` 全绿。

**请你在游戏里确认**：现有的配置不会被自动改（迁移只做一次）——
如果你现在滑条上是 12 或 4，点一下 **恢复默认** 就会变成 2；然后按 [8.3 第 8 行](AVOID_TECHNICAL_DOCUMENTATION.md#83-basic-代理验收v110-起)
往上调 4 → 12，应该能明显感到"越调越早"。
