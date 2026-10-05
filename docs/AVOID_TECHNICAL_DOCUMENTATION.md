# BestClient Avoid（Gores Bot）维护与验收技术文档

> 适用对象：接下来要改 Avoid 模块的人。
> 快照时间：2026-10-05（v5.0：五级前置门控 + 10-Tick 探针 + 扇区准星扫描 + Legit 26-Tick 后验增益仲裁 +
> Blatant 八级级联（含 `std::async` 并发贪心、提前松勾、净空二段跳、上半球出勾雷达与 NSIF 保留式回放）；写作时逐行复核过下列文件）。
> 覆盖的源码：`src/game/client/components/bestclient/` 下的 `avoid.h` / `avoid.cpp`（输入拦截、**前置门控与
> 探针调度**、遥测、HUD、世界覆盖层、瓦片编辑器交互）、`avoid_decision.h`（**纯函数**决策规则：玩法黑名单、
> 探针阈值、扇区角度、26-Tick 增益仲裁、Legit 启发式与根节点选择、两个候选集、接近目标输入）、
> `avoid_engine.h` / `avoid_engine.cpp`（前向推演、探针与扇区扫描、并发分支推演、导航网格、五个代理）、
> `avoid_tile_editor.h` / `.cpp`（规格 §10 瓦片编辑器）、`menus_avoid.cpp`
> （菜单）；`src/engine/shared/config_variables_bestclient.h` 的 `bc_avoid_*` 区块（616–677 行）；
> `scripts/avoid_selfcheck.sh`、`src/test/avoid_decision_test.cpp`（23 条决策回归测试）；
> `src/game/client/gameclient.cpp` 的 `OnSnapInput()`（输入钩子）。
>
> 参考实现规格：`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`（1658 行）。下文用 “spec §N” 指该文件的章节。
> 本文不含任何“以后会做”的内容；每一句都对应当前代码里的一条语句。已知的、有意偏离 spec 字面的
> 地方全部集中在 §12，改代码前先读那一节。
> 定位约定：这个模块还在演进，逐语句行号比文档过期得快，因此正文以**函数名 / 结构名 / 参数名**定位，
> 只有整段稳定的范围（配置区块、自检块）才写行号。

---

## 1. 速览

下一个接手的人必须知道这 8 件事，其余章节都是它们的展开。

**1. 参数集被整体替换了：旧 33 个 → 新 56 个（v4.0 新增 6 个 `bc_avoid_tile_editor_*`；v5.0 的流水线、仲裁与并发全部复用已有参数，没有新增、也没有改默认值），数值不许改。**
`bc_avoid_*` 的默认值、最小值、最大值逐条来自参考客户端的 CVar 注册表（`config_variables_bestclient.h` 的 `bc_avoid_*` 区块，616–677 行），头文件里写明了 “must not be changed”。
旧集合（`bc_avoid_active`、`bc_avoid_sensing_radius`、`bc_avoid_quality`、`bc_avoid_randomness`、`bc_avoid_log`、`bc_avoid_debug_override`、`bc_avoid_show_hud`、`bc_avoid_show_visuals`、`bc_avoid_tile_*`、`bc_avoid_*_assist` …）已经全部不存在，源码里搜不到任何一个；`bc_avoid_tile_editor_*` 里没有 “tile_death / tile_freeze” 这类旧名。
升级路径：`src/engine/client/client.cpp:5350-5355` 在 `m_ClConfigVersion < 3` 时把 `g_Config.m_BcAvoidEnabled = 0`，即老配置不会被静默地变成“已武装的另一个代理”。
旧名与新名的对应关系见 §6.5。

**2. 没有“感知半径”这个概念了。**
当前没有任何半径扫描层，参数表里也没有半径开关。危险判定只发生在克隆世界的逐步推演里：`avoid_engine.cpp` 匿名命名空间里的 `TickHitHazard()`。
唯一带 `radius` 的参数是 `bc_avoid_fent_light_tile_radius`，它描述的是导航网格上的“浅冻可通行”距离，与威胁探测无关。

**3. 没有 `bc_avoid_active`，也没有 `bc_avoid_show_hud`；`WantsEveryTickInput` 也已删除。**
唯一总开关是 `bc_avoid_enabled`（`CAvoid::IsEnabled()`）。状态 HUD 是普通 HUD 模块：`HudLayout::MODULE_AVOID`，默认关闭，开关只有一份状态（`hud_layout.cpp:28-53` 的布局表 + `hud_editor.cpp:950`）。
输入钩子现在是**三态**的：`CAvoid::ApplyInput()` 返回 `INPUT_IDLE` / `INPUT_DRIVEN` / `INPUT_YIELDED`，其中 `INPUT_YIELDED` 专门处理“代理松手后必须把玩家自己的输入发出去一次”的交接问题（闩锁式，见 §3.3）。
代理之前还有一条前置流水线：门控 0（总开关 + 本地 Tee）→ 门控 1–4（`CAvoid::PreActivation()`，玩法黑名单 / 观战与暂停 / 自身已冻结 / AFK）→ 门控 5（`CAvoid::RunLightweightProbe()`，10-Tick 基线探针）→ 阶段 2（`Avoid::RunSectorScan()`，扇区准星扫描）→ 阶段 3（代理）。任何一道门控命中都直接放行玩家原输入，`m_LastGate` 记下是哪一道（§3.5）。

**4. 五个代理，一个 dispatcher。**
`Avoid::BLAgent` 是抽象基类，`CBasicAgent` / `CLegitAgent` / `CBlatantAgent` / `CFentbotAgent` / `CPilotAgent` 各实现一个 `GetAction()`；与参考无关的**纯决策规则**（玩法黑名单 `IsBlacklistedGametype`、探针阈值 `ProbeIsSafe`、扇区角度 `SectorScanAngle`、26-Tick 增益仲裁 `ArbitrationGain` / `ArbitrationAllowsOverride`、Legit 启发式与根节点选择、两个候选集枚举、接近目标输入，以及 v5.1 的提前松勾判定 `PreemptiveHookReleaseWins`、净空规则 `HeadroomAllowsAirJump`、雷达射线表 `EmergencyRadarDirs` 与锚点过滤 `RadarTargetIsHookable`）单独放在 `avoid_decision.h` 里，可脱离游戏单测（§5.3、§11）。
Basic / Legit / Blatant 在本次调用里把搜索全部算完（同步）；Fentbot / Pilot 是**分片规划器**，一次调用只推进一个有上限的切片，跨 tick 记住搜索状态，而且**不过门控 5**（§12.18）。
`CAvoid::ApplyInput()` 的调用顺序被自检第 6 步钉死为 `RunLightweightProbe` → `RunSectorScan` → `pAgent->GetAction`。

**5. 规划器与流水线都有明确的时间切片常量。**
`PLANNER_STEPS_PER_TICK = 600`（每帧最多推进 600 个仿真 tick）、`NAV_WORK_PER_TICK = 20000`（每帧最多处理的网格瓦片数）、`SEARCH_COOLDOWN_TICKS = 10`（Fentbot 一轮结束后的冷却）、`PLAN_GUARD_TICKS = 6`（执行期的闭环护栏）、`LEGIT_DEADLINE_MS = 8.0`（Legit 的墙钟护栏）、`LIGHT_FREEZE_MIN_SPEED = 1.5f`（浅冻豁免的速度门槛）：这六个集中在 `avoid_engine.cpp` 顶部的匿名命名空间常量区。
另外十一个是流水线、仲裁与 v5.1 救援的固定窗口，定义在 `avoid_decision.h`：`PROBE_CHECK_TICKS = 10`、`PROBE_SAFE_TICKS = 7`、`SECTOR_SCAN_TICKS = 21`、`LEGIT_ARBITRATION_TICKS = 26`、`AUTO_DRAG_MAX_DIST = 380.0f`、`AUTO_DRAG_MIN_DIST = 16.0f`、`HOOK_MAX_DISTANCE = 380.0f`、`AIR_JUMP_HEADROOM = 48.0f`、`AIR_JUMP_MIN_CLEARANCE = 32.0f`、`AIR_JUMP_MIN_GAIN_TICKS = 8`、`RADAR_MIN_SURVIVAL_TICKS = 10`（另有 `EMERGENCY_RADAR_RAYS = 5`）。详见 §3.5、§5.2、§5.3、§9。

**6. 本地化是硬契约。**
所有用户可见标签走 `BcLocalize()`，查询上下文固定为 `BestClient`（`localization.cpp:28-33`）。
每个 `BcLocalize("…")` 的字面量必须在 `data/BestClient/languages/simplified_chinese.txt` **和** `russian.txt` 的 `[BestClient]` 上下文下存在同名 key。
当前契约规模（写作时逐项复核）：5 个源文件共 **195** 个 key，两个语言文件各 **885** 条 `[BestClient]` 词条，缺失 **0**。自检脚本第 4 步会重新断言这件事，见 §8、§11。

**7. 自检脚本是 `scripts/avoid_selfcheck.sh`，共 8 个块（7 步 + 6b）。**
它先构建 `DDNet`，然后依次校验参数契约、参数接线、本地化、占位文案、引擎契约（含 v5.0 的流水线与仲裁断言，以及 v5.1 的三个救援机制、12 分支动作空间与八级级联顺序）、瓦片编辑器与决策规则、HUD 接线与渲染顺序，最后构建并运行整个 `testrunner`（其中包含 23 条 Avoid 决策单测）。
动了这个模块的任何一行，跑它。每步断言什么见 §11。

**8. 参考规格的位置与章节对应。**
`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`：
§2 = 参数矩阵（默认/极值），§3 = 数据结构与 `BLAgent`/`BLAvoid` 接口，§4.1 = 前向推演引擎（含 `0x270f`、冻结条件、death/tele 判定），§5 = 前置门控与预推演探针流水线（5.1 玩法黑名单、5.2 观战与有效性、5.3 已冻结、5.4 AFK 状态机、5.5 10-Tick 探针、5.6 扇区扫描、**5.7 提前松勾抢断、5.8 净空二段跳、5.9 上半球出勾雷达**），§6 = Basic，§7 = Blatant（7.1 kick-in 迟滞、7.2 全笛卡尔候选空间、7.3 Auto Drag、7.4 解冻块逃逸、7.5 并发贪心、7.6 NSIF 与 Track Point 调度）；§8 = Legit（8.1 MCTS 节点、8.2 UCT 公式、8.3 MCTS 与 26-Tick 后验增益仲裁），§9 = Fentbot（9.1 档位表、9.2 流场点积 1750.0f），§10 = Tile Editor（数据结构、ClearAll、AutoFinish、AutoTunnels、鼠标绘制、流场重算），§11 = Pilot，§12 = 输入拦截管线与组件集成，§14 = 行为一致性验证清单（100 分基准的 6 条）。
本实现与它的字面差异是**有意为之**，逐条列在 §12。

---

## 2. 架构

### 2.1 分层

```
CAvoid (CComponent, avoid.h/.cpp)
  ├─ 输入钩子      ApplyInput()          ← gameclient.cpp 的 OnSnapInput 调用（三态，§3）
  ├─ 前置流水线    PreActivation() / RunLightweightProbe() → Avoid::RunSectorScan()
  │                                     门控 1–4 → 门控 5 探针 → 阶段 2 扇区扫描（§3.5）
  ├─ 配置快照      ReadSettings()        → Avoid::SSettings
  ├─ 渲染          OnRender() / RenderHudModule() / RenderWorldOverlay()
  ├─ 遥测          STelemetry            → 菜单状态栏 / HUD / avoid_status
  └─ 分派          m_apAgents[NUM_AGENTS] → Avoid::BLAgent::GetAction(SContext, CGameWorld*)

Avoid::BLAgent (avoid_engine.h)
  ├─ CBasicAgent    同步，6 tick 固定前瞻，只改方向
  ├─ CLegitAgent    同步，UCT/MCTS + 拟人加权 + 26-Tick 后验增益仲裁
  ├─ CBlatantAgent  同步，kick-in 迟滞 + Auto Drag + 解冻块逃逸 + 并发贪心 + 瞄准层 + NSIF
  ├─ CFentbotAgent  分片：CNavigator 流场 + 遗传微调 + CSimSession（不过门控 5）
  └─ CPilotAgent    分片：种群序列搜索 + 三种导航模式 + CSimSession（不过门控 5）

纯决策层 (avoid_decision.h, header-only, 单测覆盖)
  └─ IsBlacklistedGametype / ProbeIsSafe / SectorScanAngle / ArbitrationGain /
     ArbitrationAllowsOverride / LegitHeuristicScore / SelectLegitRootChild /
     BuildLegitCandidates / BuildBlatantCandidates / BuildApproachInput
     不依赖世界，可脱离游戏验证

推演层 (avoid_engine.cpp)
  ├─ SimulateCandidate() / SimulatePlan()   阻塞式：一次调用算完
  ├─ SimulateBranchesParallel()             std::async 并发跑一轮候选环（失败退回串行，§9.2）
  ├─ RunLightweightProbe() / RunSectorScan()  流水线的两个入口（§3.5）
  ├─ CSimSession                            可恢复：Begin/Step/Finished/Outcome/Abort
  └─ CForwardSim + RunPlan + TickHitHazard  三者共用的物理与危险判定内核

导航层 (avoid_engine.h 声明, avoid_engine.cpp 实现)
  └─ CNavigator    瓦片分类（含编辑器隧道/目标）→ 浅冻标记 → BFS 洪水 → 梯度流场
                   （全部按 NAV_WORK_PER_TICK 分片；目标与通行限制来自 CTileEditor，§5.6）
```

规则：代理不直接读 `g_Config`，只读 `SContext::m_Settings`；代理不直接改玩家输入，只通过 `AvoidInput::m_Active` 表达“我要接管”。
`bc_avoid_draw_track_point` / `bc_avoid_draw_aimbot` 只被组件的世界覆盖层读取，`SSettings` 里没有对应字段。

### 2.2 文件地图

| 文件 | 作用 | 关键位置 |
| :--- | :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` | `CAvoid` 组件声明、`EAgent`、`EState`、`EPreActivation`、`STelemetry`、`EInputResult`、`ApplyInput`、`CTileEditor` 成员 | `EAgent` / `EState` / `EPreActivation` / `STelemetry` / `EInputResult`+`ApplyInput` 五个声明块 |
| `src/game/client/components/bestclient/avoid.cpp` | 生命周期、控制台命令、`ReadSettings`、输入拦截与**前置门控**（`PreActivation` / `IsGamemodeBlacklisted` / `IsPlayerInactive` / `IsCharacterFrozen` / `IsAfk` / `UpdateAfkTimer` / `RunLightweightProbe` / `FinishInput`）、遥测、HUD 模块、世界覆盖层、瓦片编辑器交互（`UpdateTileEditor` / `RenderTileEditorOverlay`） | 按函数名检索；`OnRender` 是渲染入口 |
| `src/game/client/components/bestclient/avoid_decision.h` | **纯函数**决策规则（header-only，无世界依赖）：`IsBlacklistedGametype`、`PROBE_*` / `SECTOR_SCAN_TICKS` / `LEGIT_ARBITRATION_TICKS` / `AUTO_DRAG_*` 常量、`ProbeIsSafe`、`SectorScanAngle`、`ArbitrationSurvival` / `ArbitrationGain` / `ArbitrationAllowsOverride`、`LegitHeuristicScore`、`SLegitRootChild` + `SelectLegitRootChild`、`BuildLegitCandidates`、`BuildBlatantCandidates`、`BuildApproachInput` | 全文（379 行） |
| `src/game/client/components/bestclient/avoid_engine.h` | `Avoid` 命名空间：常量、`SSettings`、`SSimFlags`、`SFlowField`、`SContext`、`AvoidInput`、`CSimSession`、`CNavigator`、`BLAgent` 与五个代理；声明 `RunLightweightProbe` / `RunSectorScan`；**包含 `avoid_decision.h`** | 常量区 / `SSettings` / `SSimFlags`~`AvoidInput` / `CSimSession` / `CNavigator` / `BLAgent`+五个代理 |
| `src/game/client/components/bestclient/avoid_engine.cpp` | 危险判定、推演内核、流水线入口（`RunLightweightProbe` / `RunSectorScan`）、并发分支推演（`SimulateBranchesParallel`）、档位表、`SimulateCandidate`/`SimulatePlan`/`CSimSession`、流场网格、五个代理的算法 | `TickHitHazard` / `RunPlan` / `RunLightweightProbe` / `RunSectorScan` / `SimulateBranchesParallel` / `ResolveFentPreset` / `Simulate*` / `CNavigator` / 五个 `GetAction` |
| `src/game/client/components/bestclient/avoid_tile_editor.h` / `.cpp` | 规格 §10 的瓦片编辑器：Tunnel / Finish 两个集合、`Revision()`、`Mark*` / `Erase` / `Interact` / `AutoFinish` / `AutoTunnels` / `ClearAll` | 全文（70 / 128 行） |
| `src/game/client/components/bestclient/menus_avoid.cpp` | Avoid 设置页：状态栏、代理选择、共用框、每个代理的参数页签、Tile Editor 页签、Defaults；文件开头的三个排版帮手（`AvoidHintHeight` / `AvoidHint` / `AvoidHintBottom`） | 按页签名与 `PANEL_*` 枚举检索 |
| `src/engine/shared/config_variables_bestclient.h` | 56 个 `bc_avoid_*` 的唯一定义处 | 616–677 |
| `src/test/avoid_decision_test.cpp` | 23 条决策规则回归测试（含“根子节点访问次数并列时不得选到左侧子节点”、探针阈值、扇区角度、26-Tick 增益仲裁，以及 v5.1 的 12 分支笛卡尔积、提前松勾判定、净空规则与雷达射线） | 全文 |
| `scripts/avoid_selfcheck.sh` | 8 块回归自检 | 全文（513 行） |
| `docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md` | 参考实现规格（行为与常量的出处） | 全文（1658 行） |

### 2.3 接线点（改动这几个文件就等于改这个模块的行为）

| 位置 | 做了什么 |
| :--- | :--- |
| `src/game/client/gameclient.h:255` | `CAvoid m_Avoid;` 组件成员（紧随 `CTas m_Tas;`） |
| `src/game/client/gameclient.cpp:208` | 注册进渲染组件列表，位于 `m_Hud` 与 `m_Tas` 之后 |
| `src/game/client/gameclient.cpp:629-667` | `OnSnapInput()`：TAS 回放优先返回；录制读采样缓冲区；`m_Avoid.ApplyInput(&Input) != CAvoid::INPUT_IDLE` 时由这里写出数据包 |
| `src/game/client/components/controls.cpp` | 与 Avoid 已无任何耦合（`WantsEveryTickInput` 已删除） |
| `src/game/client/components/bestclient/menus_tas.cpp:595-621` | TAS& 的第二个子页签 “Avoid” → `RenderSettingsAvoid(MainView)`（`g_Config.m_BcTasTab == 1`） |
| `src/game/client/components/hud_layout.h:40` | `MODULE_AVOID` 枚举 |
| `src/game/client/components/hud_layout.cpp:28-53 / 56-110 / 475-503` | 默认布局行（索引 24）、`"avoid"` id、`"Avoid"` 名称、`IsEditorModule` 白名单 |
| `src/game/client/components/bestclient/hud_editor.cpp:528-531 / 587 / 950` | 编辑器取 `GetHudEditorRect()`、把模块加入可视化列表、`HudLayout::SetEnabled` 写同一状态 |
| `src/engine/client/client.cpp:5350-5355` | 配置版本迁移：`m_ClConfigVersion < 3` 时关闭 `bc_avoid_enabled` |
| `CMakeLists.txt:3039-3046` / `4005` | 八个 Avoid 源文件加入客户端目标（含 `avoid_decision.h` 与瓦片编辑器）；`src/test/avoid_decision_test.cpp` 加入 `testrunner` |
| `data/BestClient/languages/{simplified_chinese,russian}.txt` | `[BestClient]` 上下文下的全部标签 |

### 2.4 与参考标识符的对应关系

| BestClient | 参考（spec / 二进制） | 说明 |
| :--- | :--- | :--- |
| `CAvoid` | `BLAvoid`（spec §3.3） | 主控制器。参考的 `ProcessInput()` 在这里叫 `ApplyInput()`，挂在 `OnSnapInput` 而不是 `CControls::OnMessage` 上（§3.1） |
| `CAvoid::EInputResult` | 无 | 本实现新增的三态返回值，用来表达“没动 / 接管了 / 刚松手要交接”（§3） |
| `CAvoid::STelemetry` | 无 | 本实现新增，仅供菜单/HUD/`avoid_status` 读取 |
| `CAvoid::EPreActivation` + `PreActivation()` | spec §5.1–5.4 的四道门控（`0x1402f3f88` / `0x1402f4380` / `0x1402f3fd0` / `0x140311fe5`） | 门控 1–4 的判定结果；`PRE_OK` 之外的值直接阻断这一 tick，`m_LastGate` 记住是哪一道（§3.5） |
| `Avoid::RunLightweightProbe` | spec §5.5（`0x140312258`） | 10-Tick 基线探针；`>= PROBE_SAFE_TICKS` 时返回 `SIMULATION_SAFE_CONSTANT`，调度器据此完全跳过代理 |
| `Avoid::RunSectorScan` | spec §5.6（`0x1403122d3`） | 以玩家准星为中心的 FOV 扫描（`Segments + 1` 条射线 × 21 帧），只由 `bc_avoid_track_point` / `bc_avoid_aimbot` 打开 |
| `Avoid::BLAgent` | `BLAgent`（spec §3.2） | 虚基类：`GetAction` / `OnRender` / `OnReset`；本实现额外有 `NavigatorReady()` |
| `Avoid::AvoidInput` | `AvoidInput`（spec §3.1） | `m_Input` + `m_Active` 语义一致；扩展了 `m_SurvivalTicks` / `m_UsedFallback` / `m_vPath` / `m_TrackPoint` / `m_AimTarget` / `m_aReason` |
| `Avoid::SContext` | `GetAction(const CNetObj_PlayerInput*)` 的参数 | 把 tick、本地 client id、输入、设置、瓦片编辑器打成一个包；代理只读它，不改组件状态 |
| `Avoid::SimulateCandidate` | `SimulateCandidate`（spec §4.1，`func_0x00014036a8d0`） | 克隆世界 + 逐 tick 推演 + 危险判定；实现上是 `SimulatePlan` 的单输入特例 |
| `Avoid::SimulatePlan` | 无独立符号 | 多 tick 输入序列版本（Fentbot/Pilot 的基因是多 tick 的，Blatant 的刹车余量也是两段） |
| `Avoid::CSimSession` | 无 | 同一个物理内核的**可恢复**版本，给两个规划器分片用 |
| `Avoid::CForwardSim` | 克隆世界 + 角色指针 | 内部实现细节 |
| `Avoid::CNavigator` | Fentbot 的流场（spec §10.6 的重算算法） | 可导航网格 + BFS 距离 + 梯度流场，增量构建；`IsLightTile()` 同时被模拟器用作“浅冻可通行”的裁判 |
| `CLegitAgent` 内的 `MCTSNode` | `MCTSNode`（spec §8.1，二进制 0x58 字节） | 局部结构体，每次决策 new/delete 整棵树 |
| `Avoid::CTileEditor` | `BLTileEditor`（spec §10.1） | Tunnel / Finish 两个集合 + `Revision()`；参考把流场网格也放在编辑器里，本实现把网格留在 `CNavigator`（§5.6） |
| `Avoid::LegitHeuristicScore` / `SelectLegitRootChild` / `BuildLegitCandidates` / `BuildBlatantCandidates` / `BuildApproachInput` | spec §8.2 / §8.3（Legit）与 §7.2 / §7.3 / §7.4（Blatant）的同名逻辑 | 抽成 `avoid_decision.h` 里的纯函数，可单测；Legit 的根节点选择是 v4.0 修掉的“自己往左走”的所在地（§5.3） |
| `Avoid::ArbitrationGain` / `ArbitrationAllowsOverride` | spec §8.3（`0x140338a5f`：`sub ebx, eax; cmp ebx, 0x1; cmovge`） | Legit 的 26-Tick 后验增益仲裁：候选比玩家多活 ≥1 帧才允许覆盖输入 |
| `Avoid::ResolveFentPreset` | `0x1403356ba`（spec §9.1） | 档位表 88/160/1000 + 88/160/300 + 固定 8 tick / 10000 horizon |
| `FENT_FLOW_WEIGHT = 1750.0f` | `0x14054a6b8`（spec §9.2） | 速度·流场点积权重 |
| `WEIGHT_SCALE = 0.01f` | `0x1405300e4`（spec §8.2） | Legit 启发式的浮点缩放 |
| `SIMULATION_SAFE_CONSTANT = 9999` | `0x270f`（spec §4.1） | 整个前瞻窗口都活下来时的返回值；探针也用它表示“直接放行” |
| `PROBE_CHECK_TICKS = 10` / `PROBE_SAFE_TICKS = 7` | spec §5.5（`mov edx, 0xa` / `cmp eax, 0x7; jge`） | 探针窗口与阈值：10 帧里活过 ≥7 帧就不唤醒任何代理 |
| `SECTOR_SCAN_TICKS = 21` | spec §5.6（`mov edx, 0x15`） | 扇区扫描每条射线的推演长度 |
| `LEGIT_ARBITRATION_TICKS = 26` | spec §8.3（`mov edi, 0x1a`） | 后验增益仲裁的固定窗口，不跟随 `bc_avoid_legit_check_ticks` |
| `AUTO_DRAG_MAX_DIST = 380.0f` / `AUTO_DRAG_MIN_DIST = 16.0f` | spec §7.3 | Auto Drag 的射程上下限 |
| `BASIC_CHECK_TICKS = 6` | spec §6.1 第 1 条 | Basic 的固定前瞻，故意不留 CVar |
| `LEGIT_DEADLINE_MS = 8.0` | 无 | 本实现新增的墙钟护栏（参考会掉帧），见 §12.4 |
| `LIGHT_FREEZE_MIN_SPEED = 1.5f` | 无 | 浅冻豁免的速度门槛：停在浅冻里不再算“穿过”（§12.1） |
| `Avoid::SimulateBranchesParallel` | spec §7.5（`0x14032eb50`） | 用 `std::async` 把一轮贪心的候选环一次发出去；线程创建失败自动退回串行 |
| `m_vPendingPlan` / `m_PlanPending` | 无 | Fentbot 的计划暂存位，保证整条基因组一起换挡（§5.4） |

### 2.5 数据流

1. `CControls::SnapInput(pData)` 把这一 tick 的按键采样写进 `m_aInputData[g_Config.m_ClDummy]`。
2. `CGameClient::OnSnapInput()` 取该缓冲区的**只读引用**，复制成局部 `Input`，交给 `CAvoid::ApplyInput(&Input)`。
3. `ApplyInput` 组包：`Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy)`、`Ctx.m_Settings = ReadSettings()`、`Ctx.m_Input = *pInput`、`Ctx.m_pTileEditor = &m_TileEditor`，`Ctx.m_LocalClientId` 由 `ActiveCore()` 决定。
4. 门控 0（总开关 + 活着的本地 Tee）→ `PreActivation()` 的四道环境门控 → 10-Tick 探针 → 扇区扫描；任一门控命中就直接跳到第 6 步（§3.5）。
5. `Avoid::GetActiveWorld(GameClient())` 选择推演世界（Fast Practice 沙盒 > 预测世界 > 游戏世界 > 预测世界）；被选中的代理返回 `AvoidInput`。
6. `FinishInput()` 决定这一 tick 的返回值：接管 → 合并字段后覆盖 `*pInput` 并返回 `INPUT_DRIVEN`；松手后尚未交接过 → 原样返回 `INPUT_YIELDED`；其余 → `INPUT_IDLE`。
7. `OnSnapInput` 只在返回值不是 `INPUT_IDLE` 时 `mem_copy(pData, &Input, sizeof(Input))` 并把 `Ret` 置成 `sizeof(Input)`，让客户端把这份输入发出去/记账。
8. `UpdateTelemetry()` 把状态、存活 tick、耗时、瞄准标记和 reason 写进 `STelemetry`，供菜单状态栏、HUD 模块和 `avoid_status` 读取。

---

## 3. 输入拦截管线

### 3.1 钩子位置、三态返回值与数据流

唯一挂载点是 `CGameClient::OnSnapInput()` 的 `!Dummy` 分支（`src/game/client/gameclient.cpp:629-667`）：

```cpp
int CGameClient::OnSnapInput(int *pData, bool Dummy, bool Force)
{
    if(m_Tas.IsPlaybackActive())                                    // 632-638
    {
        int TasSize = m_Tas.OnSnapInput(pData, Dummy, Force);
        if(TasSize)
            return TasSize;                                          // TAS 回放：Avoid 完全旁路
    }
    const int Conn = g_Config.m_ClDummy ^ (int)Dummy;                 // 641
    if(!Dummy)
    {
        int Ret = m_Controls.SnapInput(pData);                        // 644：采样按键 → m_aInputData
        const CNetObj_PlayerInput &Sampled = m_Controls.m_aInputData[g_Config.m_ClDummy];   // 649
        if(m_Tas.IsRecordingActive() && !m_FastPractice.Enabled())
            m_Tas.OnRecordInput(reinterpret_cast<const int *>(&Sampled), Conn == 1);        // 653-654
        CNetObj_PlayerInput Input = Sampled;                          // 659：局部副本
        if(m_Avoid.ApplyInput(&Input) != CAvoid::INPUT_IDLE)          // 660
        {
            mem_copy(pData, &Input, sizeof(Input));                   // 662：生成数据包
            Ret = sizeof(Input);                                      // 663：即使采样器本来不想发
        }
        return Ret;                                                   // 666
    }
    ...
}
```

三态定义在 `avoid.h` 的 `CAvoid::EInputResult`：

```cpp
enum EInputResult
{
    INPUT_IDLE = 0, // nothing to do, the sampler's own send decision stands
    INPUT_DRIVEN,   // the input was rewritten and has to be sent now
    INPUT_YIELDED,  // the agent stopped driving, so the player's input has to be sent once
};
EInputResult ApplyInput(CNetObj_PlayerInput *pInput);
```

要点，逐条对应代码：

1. **挂点在 `OnSnapInput`，不在 `CControls::OnMessage`（spec §12.3 的位置）。** 原因是这里能同时覆盖“要发包”和“不需要发包”的 tick，而且 `pData`/`Client()->GetInput()` 这条链路正是服务端与本地预测共同读取的输入（`gameclient.cpp` 的 `Client()->GetInput(Tick, …)` 拿到的就是 `client.cpp` 交给 `OnSnapInput` 的那块 `m_aInputs[...]` 内存）。
2. **组件从不写 `m_Controls.m_aInputData`。** 自检第 6 步的 (e) 段断言 `avoid.cpp` 里不出现字符串 `m_Controls.m_aInputData`，同时断言存在 `*pInput = m_LastOverride;` 与 `FinishInput(`。原因见 3.2。
3. **每 tick 都会被调用一次。** `ApplyInput` 只依赖采样缓冲区，所以 `OnSnapInput` 不看 `Ret` 就调用它；`SnapInput` 只在“该发包”时填 `pData`，但这不影响判断。旧的 `WantsEveryTickInput()` 强制发包方案已删除，`controls.cpp` 与 Avoid 再无耦合（自检第 6 步断言这个符号在 `avoid.cpp` 与 `controls.cpp` 里都不存在）。
4. **由钩子按需索要数据包。** `INPUT_DRIVEN` 与 `INPUT_YIELDED` 都会让 `OnSnapInput` 把 `Ret` 提升为 `sizeof(Input)`；`INPUT_IDLE` 时 `Ret` 保持 `SnapInput` 的决定，采样器原本的合并策略（变化才发 / 至少 25 Hz）完全不受影响（自检第 6 步断言 `!= CAvoid::INPUT_IDLE` 这个测试存在）。
5. **返回值语义**写在 `avoid.h` 的 `EInputResult` 块里；`INPUT_YIELDED` 的用途见 3.3。
6. **代理前面还有三段流水线**（门控 1–4 → 门控 5 探针 → 阶段 2 扇区扫描），它们被门控阻断时同样要经过 `FinishInput`，见 3.5。

### 3.2 为什么绝不能写 `m_Controls.m_aInputData`

契约：**Avoid 只写调用者给的局部副本，从不写按键采样状态。** `avoid.cpp` 里唯一的写回发生在 `FinishInput()`：

```cpp
if(Drives)
{
    m_DroveTick = Tick;
    m_YieldPending = true;                          // 交接闩锁置位，见 3.3
    if(!m_LastAimTargetValid)                       // 代理这一 tick 没有采纳瞄准目标
    {
        m_LastOverride.m_TargetX = pInput->m_TargetX;   // 准星用玩家自己的
        m_LastOverride.m_TargetY = pInput->m_TargetY;
    }
    m_LastOverride.m_Jump = pInput->m_Jump;         // 这四个字段永远来自玩家采样
    m_LastOverride.m_Fire = pInput->m_Fire;
    m_LastOverride.m_PlayerFlags = pInput->m_PlayerFlags;
    m_LastOverride.m_WantedWeapon = pInput->m_WantedWeapon;
    m_LastOverride.m_NextWeapon = pInput->m_NextWeapon;
    m_LastOverride.m_PrevWeapon = pInput->m_PrevWeapon;
    *pInput = m_LastOverride;
    return INPUT_DRIVEN;
}
```

字段级的合并规则：`m_LastOverride` 是代理返回的 `AvoidInput::m_Input` 的整体拷贝，然后在写回之前被上面这几行改写，于是**跳跃 / 开火 / 玩家标记 / 三个武器槽永远等于玩家这一 tick 的采样值**，准星只有在代理真的采纳了一个瞄准目标（`m_LastAimTargetValid`，即 `Action.m_Active != 0 && Action.m_AimTarget.m_Valid`）时才由代理决定。代理能改的只有方向、钩索和它自己声明过的瞄准。

四个理由（每个都能在代码里核对）：

1. `m_aInputData` 是**按键采样状态**：`controls.cpp` 把方向/跳跃/钩索直接绑到按键状态指针上，它又被复制到 `m_aLastData` 作为“是否要发包”的比较基准。写它等于把机器人的输出变成下一 tick 的“玩家意图”。
2. 所有代理都以 `Ctx.m_Input`（来自采样缓冲区，也就是玩家的真实输入）作为“人类意图”基准：Basic 的基线测试与候选枚举、Blatant 的 kick-in 迟滞与平局规则、Legit 的 `|DirDiff-2|` / `|HookDiff-1|` 拟人权重、26-Tick 仲裁的人类一侧全靠它。一旦被污染，模块会把自己的上一次输出当成玩家意图并自我强化。
3. 这块缓冲区被别的功能共用：TAS 回放（`tas.cpp` 写入）、TAS 录制（现在从这里读，见 3.4）、快速换枪（`bestclient.cpp`）、dummy 交换（`gameclient.cpp`）、Fast Practice 的中立输入（`fast_practice.cpp`）。Avoid 写它会把机器人决策泄漏进这些功能。
4. 松开按键必须能立即收回控制权。只改这一份局部副本时，下一 tick 的 `Sampled` 又是玩家真实按键；改采样状态则会让机器人的方向“粘”在输入里。

### 3.3 每 tick 只决策一次、`m_LastOverride` 重放与 driving → yielded 交接

`FinishInput()` 是全部状态收口的地方：

```cpp
CAvoid::EInputResult CAvoid::FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick)
{
    if(Drives)
    {
        m_DroveTick = Tick;
        m_YieldPending = true;
        /* ……字段合并写回，见 3.2…… */
        *pInput = m_LastOverride;
        return INPUT_DRIVEN;
    }

    // 交接不是可选项：驱动那一 tick 强制发出的数据包里是机器人的输入，而采样器只比较它自己的
    // 原始状态，永远看不见这次改写。因此用一个闩锁（m_YieldPending）保证“玩家自己的输入”一定
    // 被送出去一次，送出去之后才清掉。
    m_DroveTick = -1;
    if(m_YieldPending)
    {
        m_YieldPending = false;
        m_YieldTick = Tick;
        return INPUT_YIELDED;
    }
    return INPUT_IDLE;
}
```

* **交接为什么必要**：采样器决定“要不要发包”时比较的是它自己的原始状态（`m_aInputData` vs `m_aLastData`），而机器人的改写只存在于发给服务端的数据包里。如果代理驱动了一 tick 后松手，而玩家自己的按键没有变化，采样器会认为“没变化、不需要发”，服务端就会继续沿用机器人的输入。`INPUT_YIELDED` 强制发一次包，把玩家的真实输入送出去。
* **交接是闩锁而不是“下一 tick”**：驱动 tick 把 `m_YieldPending` 置位，之后**第一次**走到 `FinishInput(false, …)` 就必定返回 `INPUT_YIELDED` 并清掉闩锁。早期实现用的是“`Tick == m_DroveTick + 1` 才算交接”，一旦预测 tick 在两次调用之间不是严格 +1（预测时间重置、卡顿丢 tick、快速输入排队），这一次交接就永远不会发生，**服务端会一直沿用机器人最后一次发出的输入**——表现就是“机器人明明松手了，人物还自己往一个方向走”。闩锁版本没有这个窗口，而且因为清闩锁与返回 `INPUT_YIELDED` 在同一次调用里，同一 tick 的重发也只会交接一次。
* **被门控阻断也会交接**：门控 0（总开关关闭 / 没有活的本地 Tee）是唯一的提前返回，它直接走 `FinishInput(false, pInput, Ctx.m_Tick)`；门控 1–5 与“没有世界”分支同样只把 reason 写进 `Action`、不置 `m_Active`，于是这一 tick 也会经过同一个 `FinishInput(false, …)`。所以“机器人被关掉 / 人物消失 / 玩家挂机 / 探针放行”的那一 tick 都会把玩家输入送出去（§3.5）。
* **重置会清掉交接状态**：`OnReset()` 与 `OnMapLoad()` 除了清遥测/路径/决策 tick/门控状态之外，也把 `m_DroveTick` / `m_YieldTick` / `m_YieldPending` 复位，所以换图或重置之后不会凭空产生一个交接包。
* **`SetEnabled()` 故意保留闩锁**：如果代理正在驱动时被关掉，下一 tick 仍需交接（`avoid.cpp` 里 `SetEnabled()` 的注释写明了这一点）；它同时把 `m_LastDecisionTick` 复位，让下一 tick 重新决策。
* 每 tick 只决策一次：`Ctx.m_Tick != m_LastDecisionTick` 时才跑流水线与 `GetAction`，同一 tick 的重发只重放结论（`m_LastOverrideActive` + `m_LastOverride`）。规划器的切片计数、`m_CandidateIndex`、`m_Generation` 因此每 tick 至多前进一次；决策/接管/NSIF 三个计数器也是每 tick 至多 +1；`m_CostMs` 在重放路径上不刷新。
* `UpdateAfkTimer()` 在 tick 守卫之内、`PreActivation()` **之前**调用，喂的是**采样出来的玩家输入**（不是上一 tick 发出去的东西）：方向 / 跳跃 / 开火 / 钩索任一变化，或准星位移 > 2 px，就刷新 `m_LastActiveTime`；`IsAfk()` 用 `(time_get() - m_LastActiveTime) / time_freq() >= bc_avoid_afk_time`（同一时刻的时间戳比较，不再是“调用次数 × 50 Hz”的估算）。AFK 命中只阻断这一 tick 并把状态置成 `STATE_AFK`，**不写 `bc_avoid_enabled`**；从非 AFK 进入 AFK 的那一 tick 打印一次 `Avoid: AFK protection paused the bot`（§12.19）。
* `SetAgent()` 会**同时重置旧代理和新代理**：新代理不能继承任何状态，旧代理要释放它建好的导航网格（本模块最大的分配）；它同时丢弃路径、复位决策 tick 与两个“上一 tick 结论”标志。

### 3.4 与 TAS、Fast Practice、dummy 的关系

| 机制 | 行为 | 位置 |
| :--- | :--- | :--- |
| TAS 回放 | `m_Tas.IsPlaybackActive()` 为真时 `OnSnapInput` 直接返回 TAS 的输入，Avoid 完全旁路 | `gameclient.cpp:632-638` |
| TAS 录制 | 录制读的是**采样缓冲区**（`Sampled`），而不是 `SnapInput` 可能没填过的 `pData`；录到的是玩家原始输入，回放轨道保持 bit-exact | `gameclient.cpp:652-653` |
| Fast Practice | 录制被 `!m_FastPractice.Enabled()` 关掉；推演改用练习沙盒世界（`ActiveCore()` / `GetActiveWorld()`） | `gameclient.cpp:652`、`avoid.cpp` 的 `ActiveCore`、`avoid_engine.cpp` 的 `GetActiveWorld` |
| dummy | `ApplyInput` 只在 `!Dummy` 分支被调用；它没有 `Dummy` 形参，tick 一律取 `g_Config.m_ClDummy`（`Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy)`） | `gameclient.cpp:642`、`avoid.cpp` 的 `ApplyInput` |
| 菜单入口 | TAS& 页面第二个子页签 “Avoid”（`g_Config.m_BcTasTab == 1`） | `menus_tas.cpp:595-621` |

### 3.5 代理之前的前置流水线：门控 1–4、10-Tick 探针与扇区扫描

参考实现在把输入交给代理之前还有一整条流水线（spec §5，汇编 `0x140311eb0`–`0x140312611`）。本实现按同样的顺序落在 `CAvoid::ApplyInput()` 里，自检第 6 步会把这段顺序钉死（`probe → sector scan → agent`，以及四道环境门控的先后）。
参考的入口是 `BLAvoid::ProcessInput()`（spec §12.2）；本实现把它拆成两半：**门控 0、每 tick 一次的决策守卫、遥测与写回留在组件**（`CAvoid::ApplyInput` / `FinishInput`，§3.1），**判定逻辑放进引擎**（`CAvoid::PreActivation` / `IsGamemodeBlacklisted` / `IsPlayerInactive` / `IsCharacterFrozen` / `IsAfk` / `UpdateAfkTimer` 在 `avoid.cpp`，`Avoid::RunLightweightProbe` / `Avoid::RunSectorScan` 在 `avoid_engine.cpp`）。纯函数那一半（黑名单匹配、探针阈值、扇区角度）在 `avoid_decision.h`，所以这一段可以脱离游戏单测。

| 阶段 | 代码 | 判定字段 / 阈值 | 命中后的行为 |
| :--- | :--- | :--- | :--- |
| 门控 0 | `!IsEnabled() \|\| !ActiveCore(&Ctx.m_LocalClientId)` | 总开关 + 本地 Tee 是否可解析（Fast Practice 沙盒走 `ResolvePracticeRoles`，否则用 `m_Snap.m_LocalClientId`） | 遥测置 `STATE_OFF`、清路径与接管标志、`m_LastGate = PRE_OK`，直接 `FinishInput(false, …)` |
| 门控 1 | `IsGamemodeBlacklisted()` → `Avoid::IsBlacklistedGametype(GameClient()->m_GameInfo.m_aGameType)` | 整串、大小写不敏感地比对 `fng` / `vanilla` / `f-ddrace` / `blockworlds` | `PRE_GAMEMODE`，reason `gamemode blacklisted` |
| 门控 2 | `IsPlayerInactive()` | 没有有效本地 id / 观战（`m_Snap.m_SpecInfo.m_Active`）/ `TEAM_SPECTATORS` / 死亡等待重生（`m_Snap.m_aCharacters[id].m_Active` 为假）/ `GAMESTATEFLAG_PAUSED`；Fast Practice 激活时不看连接快照（那四个判定都只对连接本身有意义） | `PRE_INACTIVE`，reason `player not active` |
| 门控 3 | `IsCharacterFrozen()` | `pChar->m_FreezeTime > 0 \|\| pChar->m_FrozenLastTick \|\| pChar->Core()->m_DeepFrozen`（没有活体角色也算命中） | `PRE_FROZEN`，reason `character frozen` |
| 门控 4 | `IsAfk()` | `bc_avoid_afk_protection` 打开且 `(time_get() - m_LastActiveTime) / time_freq() >= bc_avoid_afk_time`；计时由 `UpdateAfkTimer()` 按采样输入刷新 | `PRE_AFK`，reason `AFK protection`；状态置 `STATE_AFK`（只有进入 AFK 的那一 tick 打印一行，§12.19） |
| 门控 5 | `RunLightweightProbe(pWorld, pInput, Ctx.m_Settings)` | 把**玩家原输入**推演 `PROBE_CHECK_TICKS = 10` 帧（`AvoidDeath = true`、`AvoidTeles = false`、`m_PredictPlayers` 取全局参数） | `Survival >= PROBE_SAFE_TICKS(7)` → `ProbeIsSafe()` 为真 → 返回 `SIMULATION_SAFE_CONSTANT`，`PRE_SAFE`，reason `probe: player input safe`，`m_SurvivalTicks = PROBE_CHECK_TICKS`，**一个代理都不唤醒**。<br>低于 7 帧时返回**原始存活帧数**，流水线继续往下走 |
| 阶段 2 | `Avoid::RunSectorScan(GameClient(), pWorld, &Action.m_Input, Ctx.m_Settings, &Action.m_AimTarget)` | 只在 `bc_avoid_track_point` 或 `bc_avoid_aimbot` 打开时运行；以玩家准星角 `atan2(m_TargetY, m_TargetX)` 为中心，把 `bc_avoid_aimbot_fov` 按 `bc_avoid_aimbot_segments` 切成 **`Segments + 1`** 条射线（`SectorScanAngle`），每条把准星指向该方向、`Hook = 1`，推演 `SECTOR_SCAN_TICKS = 21` 帧 | 取存活最久的射线；`bc_avoid_safe_aim_tracking` 关闭时“活过 1 帧”即可接受，打开时必须整窗安全（`== 9999`）。接受且与玩家原准星不同就改写 `Action.m_Input.m_TargetX/TargetY`，并把瞄点写进 `Action.m_AimTarget`（`m_Pos = 角色位置 + 方向 × 128`） |
| 阶段 3 | `pAgent->GetAction(AgentCtx, pWorld)` | `AgentCtx` 是 `Ctx` 的副本，只有 `m_Input` 被替换成**扇区扫描改写过**的输入 | 代理返回的 `AvoidInput`；`m_Active` 为真则整体采纳，否则只继承 reason / 存活帧数 / `m_UsedFallback` |

几条必须记住的语义：

* **门控 1–5 全部“阻断而不改写”**：命中后 `Action.m_Active` 保持 0，玩家的输入原样送出去；但**这一 tick 仍然要经过 `FinishInput(false, …)`**，也就是说它照样会触发（或消费）driving → yielded 的交接闩锁（§3.3）。这正是“门控不会把上一次的机器人输入留在服务端”的原因。
* **探针的阈值语义是“够用就放行”，不是“安全才放行”**：10 帧里活过 7 帧即视为有足够余量，`ProbeIsSafe()` 把它折算成 `SIMULATION_SAFE_CONSTANT`；只有存活 ≤ 6 帧才继续跑扇区扫描与代理。
* **`m_LastGate` 与 `PRE_*` 枚举**：`CAvoid::EPreActivation` 的取值 `PRE_OK / PRE_GAMEMODE / PRE_INACTIVE / PRE_FROZEN / PRE_AFK / PRE_SAFE` 就是上表六行的判定结果（`PRE_OK` = 流水线可以继续），`CAvoid::PreActivationName()` 把它们翻译成 HUD `Plan:` 行与 `avoid_status` 里的短文本；`m_LastGate` 保留上一 tick 的值，用来只在“刚刚进入 AFK”时打印一次提示。
* **扇区扫描的改写本身算一次接管**：如果代理最终没有 `m_Active`，而准星确实被扫描改写过（`CrosshairLocked`），调度器会把这一 tick 标成 `Action.m_Active = 1`、reason `sector scan locked the crosshair`，好让这个新准星真的发到服务端（§5.2）。
* **门控 5 只拦 Basic / Legit / Blatant**：`ProbeGated = AgentId != AGENT_FENTBOT && AgentId != AGENT_PILOT`；两个分片规划器保留自己的每帧预算与 `PLAN_GUARD_TICKS` 护栏（这是有意偏离，见 §12.18）。
* **扇区扫描对五个代理都跑**：它在探针之后、代理之前，与选中的代理无关；Fentbot / Pilot 也会拿到被改写过的准星（它们不区分这个准星是不是扫描改写的，只当作这一 tick 的准星用）。**阶段 2 不等于 Blatant 的瞄准层**：阶段 2 只决定“代理接下来用哪个准星”，而 Blatant 瞄准层里的 FOV 扫描是 `bc_avoid_auto_aim` / `bc_avoid_aim_assist` 的采样源，两者职责不同、会各自扫一遍（§5.2 第 7b 点、§9.1）。

---

## 4. 前向推演引擎

### 4.1 `CGameWorld::CopyWorldClean`

推演的起点是克隆世界（`CForwardSim::Begin`、`SimulateCandidate`、Fentbot / Pilot 取快照处）：

```cpp
m_World.CopyWorldClean(pBaseWorld);
m_World.m_WorldConfig.m_PredictEvents = false;
```

`CopyWorldClean`（`src/game/client/prediction/gameworld.cpp:669-716`）与 `CopyWorld`（同文件 718 起）的区别只有一处，但很关键：`CopyWorld` 会把副本挂进预测世界的父子链（`m_pParent` / `m_pChild`，并把链上旧副本标记为失效），`CopyWorldClean` 做的是**脱离链条的完整深拷贝**。
模拟器必须要“干净”的克隆：一个 tick 里可能同时存在多个克隆（Fentbot 的 `m_pSnapshot` 加 `CSimSession` 的世界），它们绝不能互相把对方标记成失效，也不能干扰客户端自己的 `m_PredictedWorld`。这是有意选择，见 §12.2。
`m_PredictEvents = false` 关掉克隆里的预测事件重放，避免推演时触发副作用。

### 4.2 单 tick 步进与推演主循环

`CForwardSim`（`avoid_engine.cpp` 匿名命名空间里的内部类）持有克隆世界、本地角色指针、碰撞指针和本地 client id：

```cpp
m_World.CopyWorldClean(pBaseWorld);                                  // 克隆世界
m_pChar = m_World.GetCharacterById(m_LocalClientId);                 // 解析本地角色
...
void SetPredictPlayers(bool PredictPlayers)                          // 其他 tee 是否挡路
{
    if(PredictPlayers) return;
    for(int i = 0; i < MAX_CLIENTS; ++i)
        if(i != m_LocalClientId)
            m_World.m_Core.m_apCharacters[i] = nullptr;              // 其他 tee 不再挡路
}
void Step(const CNetObj_PlayerInput &Input)                          // 一个 50 Hz 物理帧
{
    m_pChar->OnDirectInput(&Input);
    m_pChar->OnPredictedInput(&Input);
    m_World.Tick();
    m_pChar = m_World.GetCharacterById(m_LocalClientId);             // 角色可能在 Tick 里被销毁
}
```

`SimulatePlan()` 在 `Begin()` 之后调用一次 `Sim.SetPredictPlayers(Flags.m_PredictPlayers)`；`CSimSession::Begin()` 里对着自己的克隆做同样的清理。这就是 `bc_avoid_player_prediction` 的全部实现，见 §12.6。

`RunPlan()` 是 `SimulatePlan` 与 `CSimSession` 共用的循环体：

* 可选记录轨迹：先把起点压入 `pvPath`，每 tick 再压一次当前位置；调用方用 `bc_avoid_draw_path` 决定是否要路径。
* 可选流场打分：`FlowScore += (Vel.x*Dir.x + Vel.y*Dir.y) * pFlow->m_Scale`，索引取自 `GetPureMapIndex(位置)`。
* 危险判定在**步进之后**：`if(TickHitHazard(...)) break;`，返回值为“完整活下来的 tick 数”。
* 活满整个窗口 → 返回 `SIMULATION_SAFE_CONSTANT`。

输入取法（`InputAt`）：`pInputs[min(Tick, NumInputs - 1)]`。因此 `SimulateCandidate`（单输入）等于“把这个输入按住整个前瞻窗口”，`SimulatePlan`（多输入）等于“先按序列走完，再用最后一个输入补满”。

失败即安全：`SimulatePlan()` 在 `pInputs` 为空 / `NumInputs <= 0` / `CheckTicks <= 0`，或者克隆世界或本地角色不存在时，直接返回 `SIMULATION_SAFE_CONSTANT`。这是有意的失败安全策略，副作用写在 §12.9。

### 4.3 危险判定谓词（`TickHitHazard`）

判定顺序固定：**死亡 → 冻结 → 传送 → 解冻**。死亡排在冻结之前是有意的：同一格可能一层是 freeze、另一层是 death，浅冻豁免绝不能把这种情况判成安全。

| 谓词 | 代码条件 | 参考出处 | 开关 |
| :--- | :--- | :--- | :--- |
| 死亡 | 位置周围 5 点（中心 + 半径 `GetProximityRadius()/3.0f` 的四个对角）上，任一 `GetCollisionAt(...) == TILE_DEATH` **或** `GetFrontCollisionAt(...) == TILE_DEATH` | spec §4.1 第 4 步；角点几何与引擎自己的死亡探测一致（`character.cpp` 的 `CCharacterCore::Tick` 一带） | `Flags.m_AvoidDeath` |
| 冻结（永远生效） | `pChar->m_FreezeTime > 0 \|\| pChar->m_FrozenLastTick \|\| pCore->m_DeepFrozen` | spec §4.1 第 3 步（`0x14036a98b-0x14036a9a4`） | `Flags.m_AvoidFreeze`；所有代理都硬编码为 `true` |
| 浅冻豁免 | 上面的冻结判定只有在 `Flags.m_AllowLightFreeze && Flags.m_pNav && !pCore->m_DeepFrozen && length(pCore->m_Vel) > LIGHT_FREEZE_MIN_SPEED && m_pNav->IsLightTile(Pos)` 全真时才放行 | 无（本实现把 Fentbot 的浅冻规则同步给模拟器，并加了速度门槛，见 §12.1） | 仅 Fentbot 打开：`Flags.m_AllowLightFreeze = Set.m_FentLightTile`、`Flags.m_pNav = &m_Nav` |
| 传送 | `GetPureMapIndex(位置)` 属于 `IsTeleport` / `IsEvilTeleport` / `IsCheckTeleport` / `IsCheckEvilTeleport` / `IsTeleCheckpoint` | spec §4.1 第 4 步（只点名了 `GetTeleCheckpoint`），见 §12.1 | `Flags.m_AvoidTeles` |
| 解冻 | 仅当 `Tick < Flags.m_UnfreezeTicks` 时，`IsUnfreezeTile`（`TILE_UNFREEZE`，含 front 层） | spec §2 的 `krx_avoid_tile_legit/blatant_unfreeze_tile(_ticks)` | `Flags.m_AvoidUnfreeze` + `Flags.m_UnfreezeTicks` |

**`TILE_DEATH` 是瓦片编号（2），不是位掩码。** 这里曾经写成 `GetCollisionAt(...) & TILE_DEATH`：按位与会把编号 3（nohook）、6、7、10、**11（unfreeze）**、14、15 和 **34（finish）** 全部命中，于是“站在终点块 / 解冻块上”会被推演判成死亡。现在中心点与四个角点都改成 `== TILE_DEATH`；自检第 6 步同时断言 `== TILE_DEATH` 存在、`& TILE_DEATH` 不存在（§11）。这条假危险源对 Basic / Fentbot / Pilot **一直生效**（它们的 `m_AvoidDeath` 恒为 `true`，见 §5.1、§5.4、§5.5），修掉之后这三个代理不再在终点块与解冻块附近无缘无故接管、也不再无理由地报 `no safer plan`。

**冻结判定只看 tee 自己携带的三个标志。** 引擎的 `pCore->m_IsInFreeze` 与 `pCore->m_LiveFrozen` 被刻意排除在外：`m_IsInFreeze` 在“站在 FREEZE / DFREEZE / LFREEZE / DEATH 瓦片上”或“角点碰到 death 瓦片”时同样为真（`character.cpp` 的 `CCharacterCore::Tick`），把它算进来会让**死亡格**和**已经深冻**的 tee 多判一次“正在冻结”——那是它们本来就已经无法离开的状态。自检第 6 步会断言 `m_Core.m_IsInFreeze` 不再出现在这个条件里。

速度门槛的意义：`LIGHT_FREEZE_MIN_SPEED = 1.5f`（px/tick）。没有它，停在浅冻瓦片上的 tee 会被判成“活着”，搜索就会把“停在冻结里”当成满分手牌；有了它，只有仍在移动地穿过浅冻才算过关。豁免还要求 `!m_DeepFrozen`：深冻永远算危险，即使它落在 unfreeze 半径里。

`SIMULATION_SAFE_CONSTANT = 9999` 定义在 `avoid_engine.h` 的 `Avoid` 常量区（对应 `0x270f`），它只是**推演层的返回值**：每个代理在报 `m_SurvivalTicks` 时都会把它换算成自己实际要看的 tick 数（Basic → 6、Blatant/Legit → `CheckTicks`、两个规划器 → 护栏窗口），所以**没有任何代理会把 9999 交给 UI**，状态栏与 HUD 的存活读数永远是一个 tick 数（见 §7.2、§7.6）。**唯一的例外在流水线上**：门控 5 把“≥7 帧”折算成 9999 作为“放行”的信号，这个 9999 从不进入遥测。

### 4.4 `CSimSession`（可恢复推演）

`CSimSession`（`avoid_engine.h` 声明、`avoid_engine.cpp` 实现）是同一个物理内核的分片版本，只有两个规划器用它：

| 接口 | 语义 |
| :--- | :--- |
| `Begin(client, world, pInputs, NumInputs, CheckTicks, Flags, pFlow)` | 先 `Abort()`，再 `new CGameWorld` + `CopyWorldClean(pBaseWorld)`，解析本地角色与碰撞，按 `Flags.m_PredictPlayers` 清空其他 tee；失败时释放并返回 `false`。成功时把 `m_Outcome.m_EndPos` 预置为起点 |
| `Step(MaxSteps)` | 最多推进 `MaxSteps` 个仿真 tick，返回实际推进数；中途角色消失或命中危险都会置 `m_Finished` 并写 `m_Outcome.m_SurvivalTicks` |
| `Finished()` | 本候选是否算完 |
| `Outcome()` | `m_SurvivalTicks`（活满窗口则为 9999）、`m_FlowScore`、`m_EndPos` |
| `Abort()` | `delete m_pWorld`，清空全部指针与结果；`m_Finished = true`。析构与 `OnReset()` 都会调用 |

与 `SimulatePlan` 的差异：多返回 `m_EndPos`（Pilot 用来算距离惩罚）、多累加 `m_FlowScore`、按 tick 分片、每个候选一个 `CGameWorld` 堆对象。
调用方的循环模式是固定的（Fentbot 的搜索切片与 Pilot 的种群评估各一处）：`Finished()` → `Begin()` 下一个候选 → `Step(StepsLeft)` → `StepsLeft -= Taken` → 若 `Finished()` 则结算 fitness。`Taken <= 0` 时跳出（防死循环）。

**指针生存期规则（改这里最容易踩）。**
会话同时持有两类外部指针，它们都指向 `CNavigator` 的内部缓冲区：

```cpp
const std::vector<vec2> &vFlow = m_Nav.Flow();
m_SessionFlow.m_pDir = vFlow.empty() ? nullptr : vFlow.data();  // Fentbot 与 Pilot 各一处
Flags.m_pNav = &m_Nav;                                          // 浅冻判定用整个网格
```

* 只要不调用 `CNavigator::Reset()` / `Rebuild()`，地址就不变：构建阶段只在原地写元素（`ClassifyTiles`、`StepMarkLight`、`BuildGradient`），`FlowAt()` / `IsLightTile()` 也只读。因此“搜索进行中同时建网格”是安全的。
* `Rebuild()` 走 `Reset()`，会把 `m_vGrid` / `m_vLightSeen` / `m_vDist` / `m_vFlow` 全部 `clear()` 后重新 `assign`，缓冲区必然换地址。**任何可能重建导航器的分支，都必须先 `m_Session.Abort()`**：Fentbot 的“网格与流场”段、Pilot 的“导航网格”段，以及 Fentbot 的计划过期分支（它同时清掉 `m_SnapshotValid` 与 `m_Cooldown`）。
* `SFlowField` 是值成员（Fentbot 与 Pilot 各一个 `m_SessionFlow`，头文件注释里写明了原因），`SSimFlags::m_pNav` 也活在整个会话期间，不要改成局部变量或临时对象。

### 4.5 流场打分

`RunPlan` 与 `CSimSession::Step` 用同一式子：每 tick 取当前速度与所在瓦片流场单位向量的点积，乘以 `pFlow->m_Scale` 累加。
`m_Scale` 由调用方定：Fentbot 用 `FENT_FLOW_WEIGHT / 50.0f = 35`（注释说明速度单位是 px/tick，所以按参考的浮点常量做了同比缩放，见 §12.17），Pilot 用 `1.0f` 并在 fitness 里再乘 4。

---

## 5. 五个代理

选中的代理由 `bc_avoid_agent` 决定，`CAvoid::Agent()` 会把它夹到 `[0, NUM_AGENTS-1]`；`SetAgent()` 会重置新旧两个代理的状态并丢弃路径。
Basic / Legit / Blatant 的 `GetAction` 只有在**前置流水线放行之后**才会被调用（§3.5）：门控 1–4 挡住了环境不合适的情况，门控 5 挡住了“玩家原输入还有余量”的情况。Fentbot / Pilot 不过门控 5。

### 5.1 Basic（`CBasicAgent::GetAction`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §6（Basic Agent 1:1） |
| 前瞻 | 固定 6 tick：`BASIC_CHECK_TICKS = 6`（`avoid_engine.h`），故意没有 CVar |
| 候选空间 | `{0, -1, 1}`，正好这个顺序（`s_aCandidateDirs[3] = {0, -1, 1}`） |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`（硬编码；Basic 在参考里也没有 teles 参数，传送瓦片不算危险） |
| 读取的参数 | `bc_avoid_player_prediction`（→ `Flags.m_PredictPlayers`）、`bc_avoid_draw_path`（只为填 `Out.m_vPath`） |
| 跨 tick 状态 | **无**。`CBasicAgent` 没有任何成员变量 |

算法（顺序即语义）：

1. 先推演玩家原输入 6 tick。返回 9999 → `m_Active = 0`，reason `"player input safe"`，`Out.m_SurvivalTicks = 6`。
2. 否则以基线为 `BestScore`，按 `{0, -1, 1}` 依次推演，**只在严格更优时**替换（`Score > BestScore`），一旦某个候选返回 9999 立即 `break`。因此“同时可行”时以枚举顺序里最先达到 9999 的候选为准。
3. `BestScore > Baseline` → 只改 `Out.m_Input.m_Direction`，`m_Active = 1`；reason 按方向给 `"brake before hazard"` / `"steer left before hazard"` / `"steer right before hazard"`；开了 `bc_avoid_draw_path` 时再推一次用于画线。
4. 没有任何候选更优 → reason `"no safer plan"`，原输入照发。

`m_SurvivalTicks` 在 Basic 下最多是 6（9999 被归一化成 `BASIC_CHECK_TICKS`），所以 HUD 的 Safe 行在 Basic 接管时显示 “6 tick”。（事实上所有代理都不会把 9999 报给 UI，Safe 行永远是 tick 数，见 §7.2。）跳跃、钩索、准星一概不动。

Basic 的 `m_AvoidDeath` 恒为 `true`——它一直把死亡瓦片算作危险，这一点没有变；变的是死亡判定的写法（§4.3）。旧代码的 `& TILE_DEATH` 会把 34（finish）与 11（unfreeze）误判成死亡格，于是站在终点块或解冻块附近时 Basic 会给出 `no safer plan` 或者做一次毫无意义的刹车，现在这类假接管没有了。

### 5.2 Blatant（`CBlatantAgent::GetAction`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §7（7.1 kick-in 迟滞、7.2 全笛卡尔候选空间、7.3 Auto Drag、7.4 解冻块逃逸、7.5 并发贪心、7.6 八级级联与 NSIF/Track Point 调度）以及 §5.7 / §5.8 / §5.9（v5.1 的提前松勾、净空二段跳、上半球出勾雷达，它们排在级联的第 2 / 4 / 5 级） |
| 危险开关 | `m_AvoidFreeze = true` + `m_AvoidDeath = m_BlatantDeath` + `m_AvoidTeles = m_BlatantTeles` + `m_AvoidUnfreeze = m_BlatantUnfreeze` / `m_UnfreezeTicks = m_BlatantUnfreezeTicks` |
| 读取的参数 | `bc_avoid_kick_in_ticks`、`bc_avoid_blatant_check_ticks`、`bc_avoid_blatant_direction`、`bc_avoid_blatant_hook`、`bc_avoid_blatant_teles/death/unfreeze/unfreeze_ticks`、`bc_avoid_nsif`、`bc_avoid_track_point`、`bc_avoid_safe_aim_tracking`、`bc_avoid_auto_drag`、`bc_avoid_aimbot`、`bc_avoid_aimbot_fov`、`bc_avoid_aimbot_segments`、`bc_avoid_auto_aim`、`bc_avoid_aim_assist`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_TrackPointValid` / `m_TrackPointPos`（锁定瞄准点）、`m_SavedSafeSequence`（NSIF 缓存）。`OnReset()` 清空三者 |

执行顺序就是 spec §7.6 的八级级联（迟滞 → **提前松勾抢断** → Auto Drag → **净空二段跳自救** → **上半球出勾雷达** → 解冻块逃逸 → 全笛卡尔并发贪心 + 瞄准层 → NSIF → 尽力而为），外加最前面的 track point 刷新。第 2、4、5 级（提前松勾、二段跳、雷达）都是**推演通过就立刻接管返回**的救援级，它们排在搜索之前：这三条正是 v5.1 之前缺掉的自救路径，排在搜索之后等于让贪心搜索先替它们做决定。自检第 6 步断言这八段的先后顺序，并断言贪心那一轮是并发的。

0. **track point**：开着 `bc_avoid_track_point` 时，用 `IsHookable()` 沿玩家当前准星方向做钩索射线；命中可钩瓦片就把命中点记成 `m_TrackPointPos`（`m_TrackPointValid = true`）。关掉该参数时立即失效。有效时写进 `Out.m_TrackPoint`（HUD 世界覆盖层用）。它在迟滞之前刷新，所以“这一 tick 让路”不会让锁定的瞄准点过期。
1. **kick-in 迟滞**：用 `bc_avoid_kick_in_ticks`（默认 26）推演玩家原输入。返回 9999 → 清空 `m_SavedSafeSequence`、`m_Active = 0`、reason `"player input safe"`、`Out.m_SurvivalTicks = KickInTicks`。只要玩家自己能活够 kick-in 窗口，机器人就让路，即使 check-ticks 窗口更长的搜索能找到别的活路。
   紧跟其后还有一次**刹车余量**判定：玩家这一 tick 在横向移动（`m_Direction != 0`）时，用 `SimulatePlan()` 推演“第 0 tick 照玩家的输入、之后刹车（`m_Direction = 0`）”这两段、窗口同样取 kick-in ticks；如果整窗安全，也走“让路”分支（清缓存、reason `"player input safe"`、`Out.m_SurvivalTicks = KickInTicks`）。它的意思是**危险还在几格之外、玩家有完整的刹车距离**，此时不该抢方向。
2. **提前松勾抢断（Preemptive Hook Release，无专属开关）**：角色正挂在钩索上（`Core()->m_HookState == HOOK_GRABBED`）**或**玩家正按着钩索键时，用同一个 check 窗口做两路推演——分支 A 把 `m_Hook = 1`（照玩家继续按），分支 B 把 `m_Hook = 0`（立刻脱钩）。判定走纯函数 `PreemptiveHookReleaseWins(Keep, Release, CheckTicks)`：先把两边的 9999 折算成窗口长度，再要求 `Keep < CheckTicks && Release > Keep`。命中就把输入的 `m_Hook` 强制置 0 并立刻返回（reason `"release the hook before the swing"`）：这一次接管**只改钩索这一个比特**，方向与准星一概不动。
   物理含义（规格 §5.7，汇编 `0x1403286f0`–`0x1403289e0`）：钟摆越过低点前后继续按钩，向心力与重力会把角色拖进下方的黑水；而在切线拐点脱钩，摆动的速度就变成水平飞行。没有这一步，一个“死按右键不放”的玩家只能看着自己被荡进冻结池——这正是 v5.0 之前的行为。

3. **Auto Drag（勾队友救命）**：`bc_avoid_auto_drag` 打开时，按 client id 从小到大遍历所有其他 tee，跳过距离 `> AUTO_DRAG_MAX_DIST(380px)` 或 `< AUTO_DRAG_MIN_DIST(16px)` 的，对第一个通过距离筛选的用 `BuildApproachInput(..., Hook = true)` 造一个“朝它走并勾住”的候选（准星指向目标的原始偏移，方向按 ±12 px 死区取 −1/0/1），并用 `bc_avoid_blatant_check_ticks` 推演它；只要返回 9999 就立刻接管并返回：`m_AimTarget` 指向那名队友，reason `"auto drag a teammate"`。
   注意这不是“挑最近的那个”：射程规则只决定谁进入候选，**第一个推演通过的人赢**，所以 client id 较小、同样可救的队友优先。推不成不消耗任何状态，继续走第 4 步；它的瞄准仍会作为瞄准候选出现在第 7b 步。
   Auto Drag **不受 `bc_avoid_aimbot` 控制**（它是救援步骤，不是瞄准辅助），也**不额外要求 `bc_avoid_player_prediction`**：推演里队友是否实体由该参数决定，关掉时钩中也不会被拉扯，救援候选自然测不出 9999（§12.15）。
4. **净空二段跳自救（Emergency Air Jump）**：规格 §5.8。三个进入条件缺一不可：
   * `CanUseAirJump()`：`!(Core()->m_Jumped & 2)` 且 `!IsGrounded()`。位 2 才是“空中跳已经用完”（角色脚下变黑）的那个标志；位 1 只表示“当前这次按键已经触发过一跳”，用它判会误判成“没有二段跳”。
   * `CheckHeadroomClearance()`：从角色位置向正上方投一条长 `AIR_JUMP_HEADROOM = 48.0f` 的射线，再取正上方 `AIR_JUMP_MIN_CLEARANCE = 32.0f`（正好一格）处的瓦片；命中点距离不足一格、或命中点 / 正上方是致死或冻结瓦片就否决。规则本身是纯函数 `HeadroomAllowsAirJump(SHeadroomProbe)`：顶着一格外的天花板或冻结顶棚起跳，等于自己把角色送回去。
     取瓦片用的是 `ProbeTileAt()` 而不是 `GetCollisionAt()`：实心碰撞层只会报告四种**实心**瓦片，冻结不在其中——只看碰撞层的话，冻结顶棚、以及带冻结前景层的墙都会被判成“可以跳上去”，正是规格里那几个位掩码想拦而没拦住的情况。`ProbeTileAt()` 先把游戏层 / 前景层 / 开关层的原始编号折进来，规则才看得到 `TILE_FREEZE` / `DFREEZE` / `LFREEZE` / `DEATH`；雷达的锚点判断（`RadarTargetIsHookable(ProbeTileAt(...))`）用的是同一个函数。
   * 增益门槛：把 `m_Jump = 1`（二段跳冲量 `m_Vel.y = -m_Tuning.m_AirJumpImpulse`）注入 `{-1, 0, 1}` 三个方向，取存活最久者，要求它比“什么都不做”**严格更久**且 `>= AIR_JUMP_MIN_GAIN_TICKS = 8`。
   命中即接管并返回（reason `"spend the air jump"`）。它是在 12 分支动作空间**之外**再单独保一道：即使搜索因为平局规则（并列时先保钩索、再保方向，跳跃维不参与偏好）没选跳跃，这一步也会先把二段跳花在救命上。
5. **上半球天花板 / 边缘墙体应急雷达（Emergency Upper Hemisphere Radar）**：规格 §5.9。把 `EmergencyRadarDirs()` 的五条射线——正上 `(0, -1)`、左上 `(-0.7071, -0.7071)`、右上 `(0.7071, -0.7071)`、左侧 `(-1, -0.2)`、右侧 `(1, -0.2)`——从角色位置向外投 `HOOK_MAX_DISTANCE = 380.0f`（钩索最大有效延伸），**完全无视玩家准星**；命中点瓦片必须通过纯函数 `RadarTargetIsHookable()`（不是 `TILE_FREEZE` / `DFREEZE` / `LFREEZE` / `DEATH` / `NOHOOK`）。命中就构造候选：准星 = 命中点相对角色的原始偏移、`m_Hook = 1`，再按“中立 → 朝锚点方向”（`{0, Lean}`，规格 §5.9 的写法）各推演一次取存活最久者，`BestSurvival > RADAR_MIN_SURVIVAL_TICKS = 10` 就接管并返回（reason `"hook the ceiling above"`，`m_AimTarget` 指向锚点）。
   为什么必须有这一层：阶段 2 的扇区扫描只在玩家准星 ±`bc_avoid_aimbot_fov`/2 之内扫，鼠标正朝下看着黑水时永远扫不到头顶的天花板与身后的边缘墙。这条雷达是那种视角下唯一的出路。
   **瓦片判定按编号比较**：参考写法是把 `TILE_DEATH` 与 `TILE_FREEZE` 或起来再按位与（`2 | 9 = 11`），套在瓦片编号上会连带否掉 1（实心）与 11（解冻块）——正好是雷达要找的墙和逃生要踩的解冻块。本实现用 `IsLethalOrFreezingTile()` / `RadarTargetIsHookable()` 逐编号比较（§12.1）。
6. **解冻块逃逸**：`bc_avoid_blatant_unfreeze` 打开时，用 `FindNearestUnfreezeTile()` 在 `bc_avoid_blatant_unfreeze_ticks`（这里当半径用，单位是瓦片；四邻 BFS 逐层扩散、不穿实心、不检查起点格）内找最近的 `TILE_UNFREEZE` 中心；找到就按“勾索 / 不勾索”两种候选各推演一次（`{true, false}` 顺序，钩索优先），命中 9999 → 接管、`m_AimTarget` 指向解冻块、reason `"escape to an unfreeze tile"`。
   这一步用一份临时拷贝的 `SSimFlags` 推演（`m_AvoidUnfreeze = false`、`m_UnfreezeTicks = 0`）：搜索把“前 N tick 踩到解冻块”当危险，而逃生恰恰要踩上去，所以逃生候选**豁免**这条规则（§12.14）。
7. **并发贪心搜索**：
   * 候选来自 `avoid_decision.h` 的 `BuildBlatantCandidates()`：按 spec §7.2 的**全笛卡尔积** `Dirs[3] = {0, -1, 1} × Hooks[2] = {0, 1} × Jumps[2] = {0, 1}`，最多 **12** 条分支。v5.0 的“按玩家当前输入六选一的动态 5 分支优先级表”（旧 spec §6.2 / `0x14032e530`）已被 v5.1 取代——那张表里**根本没有跳跃维度**，于是空中的自救只能靠玩家自己按着空格。
     * 跳跃那一维只在 `CanAirJump` 为真时打开，判据是 `CanUseAirJump(pWorld, LocalId)`：`!(Core()->m_Jumped & 2)`（位 2 = 空中跳已用完）且 `!IsGrounded()`。关闭时 `m_Jump` **保持玩家当前值**，不会把跑动中的地面跳清掉。
     * `bc_avoid_blatant_direction` / `bc_avoid_blatant_hook` 关掉时，把对应维度的循环数降到 1，也就是**保持玩家当前值**（与 v5.0 的“过滤掉不符的候选”不同：过滤在只剩一维时会返回空集合，而笛卡尔积永远至少给出玩家自己的那一支）。
     * 嵌套顺序就是规格顺序，也就是平局规则：`(0,0,0) → (0,0,1) → (0,1,0) → (0,1,1) → (-1,0,0) → … → (1,1,1)`。零向量准星在入环前归一化成 `m_TargetY = -1`。
   * 整个候选环一次交给 `SimulateBranchesParallel()`：每个候选用 `bc_avoid_blatant_check_ticks` 推演（`Survival` 取“9999 → CheckTicks”），**单候选时直接内联**，多候选时每个候选一个 `std::async(std::launch::async)`（spec §7.5，`0x14032eb50`）。线程创建抛 `std::system_error` 时先把已经发出的 future `wait()` 掉，再在本线程**串行重做整轮**，不会丢决策（§9.2）。
   * 结果按候选顺序喂给同一个 `Consider()`：`Survival > BestSurvival` 才替换；分数并列时依次比较“候选是否保持玩家的钩索”“候选是否保持玩家的方向”，都保持则保留更早的那个。因此**枚举顺序就是平局规则**。`BestSurvival` 的初值是 `KickSafety`（玩家原输入在 kick-in 窗口里的实际存活帧数），`BestAction` 的初值是玩家原输入。
   * 任一候选返回 9999 就置 `FoundSafe = true`。
7b. **内部瞄准层**（`!FoundSafe && bc_avoid_aimbot`）：它只**产生候选**，不直接决定动作。
   * 固定候选：track point（`bc_avoid_track_point` 且 `m_TrackPointValid`；开 `bc_avoid_safe_aim_tracking` 时先把该瞄准 `hook = 1` 塞进玩家输入、用同一个 check 窗口推演一次，**只有返回 9999 才算可用**，否则这个候选直接被丢掉）；Auto Drag 的瞄准（`bc_avoid_auto_drag` 时取 380 px 内、16 px 外**最近**的 tee，距离并列时留下 client id 较大的那个）。固定候选无条件入选。
   * FOV 扫描：本层**自己扫一遍**这段视野，即使调度器刚刚在阶段 2 扫过同一片——两者职责不同：阶段 2 改写的是“代理接下来用哪个准星”，本层的扫描是 `bc_avoid_auto_aim` / `bc_avoid_aim_assist` 的采样源，跳过它这两个参数就没有任何效果。以 `Ctx.m_Input` 的准星角为中心（这就是**阶段 2 改写之后**的准星，所以两次扫描的扇区中心不一定相同）、`bc_avoid_aimbot_fov`（10–360°）为扇角、`bc_avoid_aimbot_segments` 条采样，**每条**先 `IsHookable()` 过滤，再对命中的方向做一次 `hook = 1`、长度为 check 窗口的探针（这个采样是 `Segments` 条、不是阶段 2 的 `Segments + 1` 条，采样点用 `Segment / (Segments - 1)` 均匀铺满扇角）。`bc_avoid_auto_aim` 选“探针存活最久”的那条，`bc_avoid_aim_assist` 选“探针整窗安全且离准星最近”的那条（比较的是扇区相对准星的偏移，`|Offset|`，必要时折算成 `2π - |Offset|`）；两者可以同时入选，重复方向由后面的 `AddAim` 去重。
     代价：`bc_avoid_aimbot` 打开时，一个危险 tick 会有**两次** FOV 扫描（阶段 2 的 `(Segments + 1) × 21` 帧 + 本层的 `≤ Segments` 条可钩射线 × check 窗口），§9.1 的预算表把两者都算进去了。
   * `AddAim` 用 `dot > 0.9999f` 去重，避免为同一个方向重复付费。
   * 最后对每个入选瞄准 × 候选环里的每个动作各推演一次，走的还是 `Consider(..., FromAimbot = true)`：**生存搜索裁决一切**，瞄准永远不会让机器人比普通候选更不安全。`Out.m_AimTarget` 也只在真正采纳了瞄准候选时才发布（reason 变成 `"hook the safe aim"` / `"aim clear of the hazard"`）。关掉 `bc_avoid_aimbot` 时整层静默：track point 只剩 HUD 覆盖层那一个点，Auto Drag 只剩第 3 步的救援动作。
8. **NSIF（保留式回放）**：`FoundSafe` → 先清空再压入 `BestAction`（缓存里因此只有一步）；否则当 `bc_avoid_nsif` **或** `bc_avoid_track_point` 打开且缓存非空时 → 取出 `front()`；**只有长度大于 1 时才 `erase(begin())`**，长度为 1 时保留它（否则“必死局”的第二个 tick 就没东西可发了，§12.20）。然后用同一个 check 窗口重推它来得到**真实的**存活 tick（不伪造 9999）并置 `UsedFallback = true`。
9. **尽力而为**：gate 是 `BestSurvival > KickSafety`——候选（含 NSIF 回放）必须比玩家当前输入**严格活得更久**才接管。这正是“必死局里也要活得最久”的语义：`<= KickSafety` 时玩家保留自己的输入，走 `else` 分支给出 reason `"player input safe"` 与 `Out.m_SurvivalTicks = KickSafety > 0 ? KickSafety : BestSurvival`（Blatant **不再**产生 `"no safer plan"`）。
    接管时 `Out.m_Input = BestAction`、`m_Active = 1`、`Out.m_SurvivalTicks = BestSurvival`（NSIF 回放时就是刚测出来的真实值）；reason 优先级为 NSIF（`"NSIF: replay saved safe input"`）→ 瞄准变化（`"hook the safe aim"` / `"aim clear of the hazard"`）→ 钩索变化（`"hook to safety"` / `"release hook"`）→ 方向（`"brake before hazard"` / `"steer away from hazard"`）。路径只在 `bc_avoid_draw_path` 且非 NSIF 回放时重算。
    代码里那段注释（`BestSurvHook0` 一类的保护性否决）说明了这里**没有**额外的保守护栏：一旦门控 5 放行，玩家输入在 check 窗口内就是会死的，任何“为了保护玩家输入而取消救援”的分支都正好否掉算出来的活路（§12.21）。

`STATE_NSIF` 只在这个回放路径上产生（`CAvoid::UpdateTelemetry` 看 `m_UsedFallback`）。单步缓存在回放时被保留，所以 NSIF 可以连续亮多个 tick，直到缓存被一次 `FoundSafe` 覆盖、或 kick-in 让路分支把它清空。

### 5.3 Legit（`CLegitAgent::GetAction`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §8（MCTS 节点 8.1、UCT 公式 8.2、MCTS 与 26-Tick 后验增益仲裁 8.3） |
| 前瞻 | `bc_avoid_legit_check_ticks`（默认 6） |
| 迭代数 | `bc_avoid_legit_iterations`（默认 100，上限 1000） |
| 危险开关 | `m_AvoidFreeze = true` + `m_AvoidDeath = m_LegitDeath` + `m_AvoidTeles = m_LegitTeles` + `m_AvoidUnfreeze = m_LegitUnfreeze` / `m_UnfreezeTicks = m_LegitUnfreezeTicks` |
| 读取的参数 | 上面 6 个 + `bc_avoid_legit_direction` / `bc_avoid_legit_hook` / `bc_avoid_legit_direction_weight` / `bc_avoid_legit_lifespan_weight` / `bc_avoid_legit_hook_weight` / `bc_avoid_legit_exploration` / `bc_avoid_player_prediction` / `bc_avoid_draw_path` |
| 跨 tick 状态 | **无**。搜索树在单次 `GetAction` 里建立、选完即 `delete pRoot` |

UCT 启发式（`avoid_decision.h` 的 `LegitHeuristicScore()`，逐字对应 spec §8.2）：

```
DirDiff   = |(float)Action.m_Direction - (float)Ctx.m_Input.m_Direction|
DirScore  = |DirDiff - 2.0f|  × (WeightDir      × 0.01f)
HookDiff  = |(float)Action.m_Hook      - (float)Ctx.m_Input.m_Hook|
HookScore = |HookDiff - 1.0f| × (WeightHook     × 0.01f)
LifeScore = SurvivalTicks     × (WeightLifespan × 0.01f)
Heuristic = DirScore + HookScore + LifeScore          // WEIGHT_SCALE = 0.01f
```

注意方向项的形状：候选与玩家方向相同时 `DirDiff = 0` → `|0-2| = 2`（满分），相差 2（左↔右）时得 0 分。

**入口没有 baseline / 刹车早退。** 参考 spec §8.3 里 Legit 没有自己的安全闸：门控 5 的 10-Tick 探针已经判定“玩家原输入余量不足”才会放行到代理，26-Tick 仲裁才是决定“能不能覆盖输入”的那一层。这里再放一次更短的 baseline 早退，只会把代理存在的最后那几个危险 tick 吞掉。历史实现里的“钩索二次校验（`PlayerSurv > 3` 就不许自动钩）”与“6 帧内安全就退回玩家方向”两段护栏已被删除，理由见 §12.21。

四阶段（每次迭代）：

1. **Selection**：从根往下，选 `Score` 最大的子节点；`Score = Exploitation + Exploration + Heuristic(child)`，其中 `Exploitation = m_TotalValue / m_Visits`，`Exploration = ExplorationC × sqrt(log(max(1, parentVisits)) / childVisits)`，`ExplorationC = bc_avoid_legit_exploration`（整数直接当 double 用）。未访问过的子节点得 `3.402823466e+38`（FLT_MAX），保证每个子节点至少被走一次。**终局节点的启发项按 0 计**（`m_IsTerminal ? 0.0 : Heuristic(...)`，对应 `0x14033838d`）。
2. **Expansion**：当前节点非终局且 `m_Visits > 0` 时调用 `BuildLegitCandidates()`（`avoid_decision.h`）展开，然后用**全局 `rand()`** 随机挑一个孩子继续（`rand() % 子节点数`）。展开顺序就是 spec §8.3 的顺序，也是本版修 bug 的关键之一：
   * `bc_avoid_legit_direction` 打开时先出 **hook = 0 环**：`m_Direction` 依次取 `{-1, 0, 1}`，`m_Hook` 固定 0（若 `bc_avoid_legit_hook` 关掉则保持父节点的钩索状态）；
   * **空中二段跳维度（v5.1）**：`CanAirJump` 为真时每个方向的 hook = 0 候选出**两条**，`m_Jump` 依次取 `{0, 1}`；否则 `m_Jump` **继承父节点**（详见 §12.22）。`CanAirJump` 在整棵树开始搜索之前算一次，判据是 `CanUseAirJump()` **且** `CheckHeadroomClearance(..., AIR_JUMP_HEADROOM)`（与 Blatant 的第 7 级不同：Legit 连净空一起要求，规格 §8.3 就是这么写的），世界在搜索期间不动，所以一次判定对整个树都成立；
   * 再出 **hook = 1 环**：`m_Direction` 依次取 `{-1, 0, 1}`，`m_Hook = 1`，`m_Jump` 保持父节点的值（规格如此，钩索环不带跳跃维度）；
   * 只开钩索时（方向关、钩索开）退化成只改钩索的两个候选，`m_Hook` 取 `{0, 1}`；
   * 每个候选在入树前做瞄准归一化：`TargetX == 0 && TargetY == 0` 时改成 `TargetY = -1`（Teeworlds 协议不允许零向量）。这个顺序让**第一个创建的子节点永远是 `m_Direction = -1`、`m_Hook = 0`**，§5.3 末尾的历史 bug 正是踩在这一点上。
   有空中二段跳时一次展开是 **9** 个子节点（3 方向 × 2 跳跃 + 3 钩索），没有时仍是 6 个。
   Basic 用的是 `{0, -1, 1}`，Blatant 用的是 12 分支笛卡尔积（§5.2），三套顺序不可互换。
3. **Rollout**：`SimulateCandidate(m_pClient, pWorld, pCurr->m_Action, CheckTicks, Flags)`；`m_LifespanTicks = (9999 ? CheckTicks : Survival)`；奖励 `Reward = (double)m_LifespanTicks`（对应 `0x14033904b`-`0x140339075`，与启发式的量纲一致）；**只有非根节点且 `Survival <= 0` 时**才把该节点标记为终局（`0x140339054`；根节点永远不能变成终局，否则第 1 轮迭代就跳过展开、树再也长不出来）。
4. **Backpropagation**：沿父链把 `m_Visits++` 与 `m_TotalValue += Reward`；**终局节点把自己的 `m_TotalValue` 直接置 0**，不再累积奖励（对应 `0x140338ee0` / `0x140338220`）。

墙钟护栏：`LEGIT_DEADLINE_MS = 8.0`，循环开头每 8 次迭代检查一次（`if((Iter & 7) == 0 && time_get() >= Deadline)`），命中就 `break` 并记住 `DeadlineHit`（§12.4）。菜单的 Priority 页签有一条提示专门说明这件事。

**最终决策**（`avoid_decision.h` 的 `SelectLegitRootChild()`，对应 spec §8.3 / 反编译 `0x1403392cb`）：把根节点的子节点拷进 `SLegitRootChild`，然后

* 跳过 `m_Visits == 0` 的子节点（没测过的东西不能选）；
* 对剩下的取 `Exploitation + Heuristic` 最大者，其中 `Exploitation = m_TotalValue / m_Visits`、`Heuristic = LegitHeuristicScore(动作, 玩家输入, m_LifespanTicks, 三个权重)`，**探索项固定为 0**；两个候选的分数落在 `1e-7` 之内时按“先保钩索、再保方向”打破平局；
* 全部子节点都没访问过 → 返回 −1，此时**不接管**：什么都没测到就不动玩家的输入（reason `"search budget reached"`（护栏命中）或 `"player input safe"`）。

**CVar 掩码（`0x140338ab4` / `0x140338ac6`）**：选中的动作不会整条照抄——`Override` 以玩家输入为底，只有 `bc_avoid_legit_direction` 打开时才写入候选的 `m_Direction`，只有 `bc_avoid_legit_hook` 打开时才写入候选的 `m_Hook`。瞄准永远不改。

**提前松勾抢断（v5.1，规格 §8.3 第 4.5 步，汇编 `0x1403389ba` 的 `call 0x1403286f0`）**：在最终决策选出根子节点之后、26-Tick 仲裁之前，Legit 也会调用同一个 `CheckPreemptiveHookRelease()`（窗口取 `bc_avoid_legit_check_ticks`，默认 6）。命中就直接返回强制松勾的输入（reason `"release the hook before the swing"`），**根本不看** MCTS 选了什么：角色挂在钩索上、继续按钩会在窗口内触冻时，切线脱钩在构造上就是更优解，任何搜索结果都不该把它劝回来。注意这里的窗口是 Legit 自己的 check ticks，比 Blatant 的 26 帧短；荡水时更早、更有把握的抢断来自 Blatant（`bc_avoid_agent 2`），Legit 上想提前触发就调大 `bc_avoid_legit_check_ticks`（§12.23）。

**26-Tick 后验增益仲裁**（`0x140338a0c`-`0x140338a68`，`avoid_decision.h` 的 `ArbitrationGain()` / `ArbitrationAllowsOverride()`）：掩码之后的候选必须在**固定 26 帧**（`LEGIT_ARBITRATION_TICKS`，不跟随 `bc_avoid_legit_check_ticks`）的窗口里比玩家原输入**多活至少 1 帧**才允许覆盖输入。两路推演用的是同一份 `SSimFlags`（同一套危险开关），并且先把 9999 折算成窗口长度（`ArbitrationSurvival`）再相减，否则减法没有意义。

* `Gain >= 1` 且掩码后的输入与玩家输入**不同** → 接管，reason 是 `pBase (+Gain)`：`pBase` 依次取 `"hook and steer to safety"`（方向与钩索都变）/ `"hook to safety"` / `"release hook"`（只变钩索）/ `"brake before hazard"`（只把方向改成 0）/ `"steer to safety"`（其余方向变化）。增益写进 reason 就是为了让 HUD 上能看到仲裁的依据。
* `Gain < 1` → **无条件驳回**，输入保持原样，reason `"post hoc gain below 1 tick"`。
* `Gain >= 1` 但掩码把候选还原成了玩家输入 → 没有可覆盖的东西，reason `"player input safe"`，也不额外发包。
* `Out.m_SurvivalTicks` 在仲裁之前就按 `Best.m_LifespanTicks > 0 ? Best.m_LifespanTicks : CheckTicks` 写好；`bc_avoid_draw_path` 时只在实际接管的分支里重算路径。

**历史：为什么不能用“访问次数最多”。** v3.0 的最终决策写成了“取 `m_Visits` 最大的根子节点”。搜索跑满时两者结果接近，但 Legit 有 8 ms 预算：护栏一旦命中，根子节点的访问次数就是**并列**的，而“严格大于”在这种并列下等价于“取第一个创建的子节点”，扩展顺序里第一个子节点正是 `m_Direction = -1`。于是只要玩家输入不是明显危险，机器人就把方向改成左——表现为“一进游戏就自己往左走”。改成 `Exploitation + Heuristic` 后安全时玩家自己那个子节点得分最高（方向 +2.0、钩索 +1.0，权重 170/260），机器人不再碰玩家的输入；`src/test/avoid_decision_test.cpp` 里有专门的回归用例钉住这一点（§11）。

**预算检查点保证“至少 8 轮迭代、每个环子节点都被访问过”。** 检查在 `(Iter & 7) == 0` 上，而 Deadline 是在进入循环时才设为 `now + 8 ms` 的，所以第 0–7 轮一定跑完：第 0 轮在根上做一次 rollout，第 1 轮把整个环（6 个子节点）一次建齐并随机走其中一个，剩下 6 轮里未访问的子节点每次都拿 `FLT_MAX`，会被优先走完。因此只要搜索真的开始，参考扩展出的每个根子节点都至少被访问过一次——这也是“跳过未访问子节点”不会误伤的原因。护栏是检查点而不是硬中断：最坏会多跑 7 次迭代（§9.4）。

### 5.4 Fentbot（`CFentbotAgent::GetAction`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §9（档位表 9.1、流场点积 9.2） |
| 类型 | 分片规划器：`CNavigator` 流场 + 遗传微调 + `CSimSession` |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`，外加浅冻豁免 `m_AllowLightFreeze = Set.m_FentLightTile` / `m_pNav = &m_Nav`。`m_AvoidDeath` 恒为 `true`：死亡瓦片一直是危险，v4.0 修的只是它的比较方式（§4.3），所以终点块/解冻块不再被误判 |
| 读取的参数 | `bc_avoid_fent_quality`、`bc_avoid_fent_advanced`、`bc_avoid_fent_ticks`、`bc_avoid_fent_tweaker_actions`、`bc_avoid_fent_tweaker_ticks`、`bc_avoid_fent_tweaker_dosage`、`bc_avoid_fent_light_tile`、`bc_avoid_fent_light_tile_radius`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_Nav`、`m_pSnapshot` + `m_SnapshotValid`、`m_vCandidates` + `m_vFitness`、`m_CandidateCount`、`m_CandidateLength`、`m_CandidateIndex`、`m_Generation`、`m_Cooldown`、`m_SessionFlow`、`m_Session`、`m_vPlan`、`m_vPendingPlan` + `m_PlanPending`、`m_PlanIndex`、`m_BestFitness`；`OnReset()` 逐个复位 |

**档位表（`ResolveFentPreset`，对应 `0x1403356ba`）**：`bc_avoid_fent_advanced == 0` 时档位覆盖这四个值，四档的 horizon 都是 10000、hold 都是 8 tick：

| `bc_avoid_fent_quality` | 档位 | `m_FentActions` | `m_FentDosage` | `m_FentHoldTicks` | `m_FentHorizon` |
| :---: | :--- | ---: | ---: | ---: | ---: |
| 0 | Low | 88 | 88 | 8 | 10000 |
| 1 | Mid | 160 | 160 | 8 | 10000 |
| 2 | Max | 1000 | 300 | 8 | 10000 |

`bc_avoid_fent_advanced == 1` 时改为读取自定义值并在 `ResolveFentPreset` 里再夹一次：`actions = clamp(tweaker_actions,50,5000)`、`dosage = clamp(tweaker_dosage,1,500)`、`hold = clamp(tweaker_ticks,1,30)`、`horizon = clamp(fent_ticks,1000,10000)`。菜单里档位选择器与高级滑条互斥显示。

每次 `GetAction` 的四段：

1. **网格与流场**：地图尺寸变化、浅冻设置变化（`LightTile()` / `LightRadius()` 与当前参数不一致）、**瓦片编辑器 `Revision()` 变化**（`m_Nav.EditorRevision() != pEditor->Revision()`）、或既没就绪也没在构建时 → `m_Session.Abort()` + `m_Nav.Rebuild(pCollision, Set.m_FentLightTile, Set.m_FentLightTileRadius, pEditor)` + `m_SnapshotValid = false`；未就绪则 `m_Nav.Update(pCollision, NAV_WORK_PER_TICK)`。`Length = clamp(m_FentHoldTicks,1,30)`、`Horizon = clamp(m_FentHorizon,1,10000)`。
2. **保底动作**（`FallbackAction`）：任何时刻都有输出。`Guard = clamp(Length, PLAN_GUARD_TICKS, 10)`（故意便宜：它可能连续跑很多 tick）；先推玩家原输入作为基线，再按流场方向决定方向槽的顺序，枚举 `3 方向 × 2 跳跃 × 2 钩索 = 12` 个候选（钩索时准星对准流场方向），严格更优才替换。reason：接管则 `"survival fallback while searching"`，否则 `"player input safe"`。
   **方向槽是一个真正的排列**：`aDirs[0] = 0`（不动）、`aDirs[1] = -1`（左）、`aDirs[2] = 1`（右）；`aOrder` 是 `{1,2,0}`（流场不可用时：左、右、不动），有右向流场时 `{2,0,1}`（右、不动、左），有左向流场时 `{1,0,2}`（左、不动、右）。三个槽各被枚举一次，流场最可能想要的那个排在最前，所以最可能的候选最先被推演。
3. **搜索切片**：`m_Cooldown` 每 tick 递减；为 0 时执行
   * 还没有快照 → `m_Session.Abort()`、`CopyWorldClean(pWorld)` 到 `m_pSnapshot`、`SeedGeneration()`；
   * 刷新 `m_SessionFlow`（`m_pDir = m_Nav.Flow().data()`、宽高取自 `m_Nav`、`m_Scale = FENT_FLOW_WEIGHT / 50.0f = 35`）；
   * `StepsLeft = PLANNER_STEPS_PER_TICK`；候选逐个 `Begin`→`Step`，结算 `Fitness = Survival + FlowScore − 2 × DistanceAt(m_EndPos)`（`Survival` 把 9999 换成 `Horizon`；导航距离为 −1 时不扣）；
   * **随时发布（anytime publication）**：某个候选的 `Fitness > m_BestFitness` 时，把这一条基因组写进 **`m_vPendingPlan`** 并置 `m_PlanPending = true`（不直接改正在执行的 `m_vPlan`）。`m_BestFitness` 在 `SeedGeneration` 里重置，因此它在一整轮（多代）内单调递增，跨代继承“目前最好”。
   * 整代算完（`m_CandidateIndex >= Count`）→ `m_Generation + 1 < max(1, m_FentDosage)` 时 `Breed()`，否则 `m_SnapshotValid = false; m_Cooldown = SEARCH_COOLDOWN_TICKS`（睡 10 tick 后开新一轮）。
* 种子生成（`SeedGeneration`）：`Count = clamp(m_FentActions,1,5000)`、`Length = clamp(m_FentHoldTicks,1,30)`；碱基是玩家输入但清空跳跃与钩索；偶数个体跟流场走（`Flow.x > 0.25` → 右，`< -0.25` → 左；`Flow.y < -0.25` → 跳），`i % 4 == 1` 的个体方向取 `(i % 3) - 1`；只有在 `i % 2 == 1 || t > 0` 时才做突变（`rand()` 位测试：方向 1/4、跳跃 1/16、钩索 1/16）。也就是说“偶数个体的第 0 个基因”正好是干净的流场跟随动作。
* 繁殖（`Breed`）：`Keep = clamp(Count/4, 1, Count)`；按 fitness 稳定降序排序；前 `Keep` 个精英原样保留（`ParentA = vOrder[i % Keep]`），其余逐个基因按 `rand() & 1` 从 `ParentA = vOrder[i % Keep]` 与 `ParentB = vOrder[(i * 7 + 3) % Keep]` 两个父本里取，再按 1/5 方向、1/11 跳跃、1/13 钩索突变；之后 `m_Generation++`、fitness 重置为 `-1e30`、`m_CandidateIndex = 0`。**注意**：种群形状以正在跑的这一代为准，参数变化只在下一次 `SeedGeneration` 生效。
4. **执行与闭环护栏**：
   * 没有计划就直接返回保底动作。
   * **换挡是一次原子交换**：只有当 `m_PlanPending` 为真**且**当前基因组已经走到最后一步（`m_PlanIndex >= LastStep`）时，才 `m_vPlan.swap(m_vPendingPlan)`、清空暂存、`m_PlanPending = false`、`m_PlanIndex = 0`。这条规则的作用是：**永远不会把旧基因组的尾巴和新基因组的头连着开**（那会把同一个跳跃/钩索重新触发一遍）；没有更新的基因组时，最后一个输入被一直保持。
   * 用 `PLAN_GUARD_TICKS = 6` 推演这一步：不是 9999 时再推玩家原输入，如果“计划 ≤ 玩家”就认定计划过期 → 清空计划与暂存、`m_PlanPending = false`、`m_PlanIndex = 0`、`m_SnapshotValid = false`、`m_Cooldown = 0`、`m_CandidateCount/Length = 0`、`m_Session.Abort()`，返回保底动作。
   * 计划可用则 `m_PlanIndex++`（只在未到末尾时）、`m_Active = 1`、reason `"fentbot plan"`、`m_SurvivalTicks = (9999 ? 6 : PlanSafety)`。`bc_avoid_draw_path` 时沿流场走 40 步、每步 24 px 生成可视化折线。

网格分类（`CNavigator`）：`NAV_BLOCKED=0 / NAV_OPEN=1 / NAV_GOAL=2 / NAV_LIGHT=3`。分类阶段先应用瓦片编辑器的通行限制与目标（`InsideTunnel || EditorGoal`，详见 §5.6）：实心或死亡瓦片一律 blocked；deep freeze 永远 blocked；普通 freeze 先一律 blocked，unfreeze 瓦片同时被收集成浅冻 BFS 的种子。`StepMarkLight` 从每个 unfreeze 种子向外做最多 `m_LightRadius` 步的四邻 BFS：穿过可走瓦片，遇到 blocked 且是普通 freeze 的瓦片就标成 `NAV_LIGHT`（可通行），**实心、deep freeze 与死亡瓦片永不通行**；每个瓦片只访问一次（`m_vLightSeen`），深度到 `Radius` 就停。目标瓦片优先取 finish（`NAV_GOAL`，含编辑器标记的），一个都没有时退化为 unfreeze 瓦片，回退同样限定在隧道内（§5.6）。阶段顺序 `PHASE_CLASSIFY → PHASE_MARK_LIGHT → PHASE_FLOOD → PHASE_GRADIENT → PHASE_IDLE`，每帧总工作量受 `NAV_WORK_PER_TICK` 限制，`Ready()` 之前 `NavigatorReady()` 为假（菜单状态栏显示 `grid: building`）。
浅冻规则不只影响寻路：`Flags.m_AllowLightFreeze` + `m_pNav->IsLightTile()` 让模拟器也接受这些瓦片（§4.3），并且要求角色仍在移动，否则规划会把“停在冻结里”当成满分。

### 5.5 Pilot（`CPilotAgent::GetAction`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §11（模式与四个种群参数） |
| 类型 | 分片规划器：种群序列搜索 + `CNavigator` 流场 + `CSimSession` |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`（硬编码）。与 Fentbot 一样，死亡瓦片一直是危险，v4.0 修的是比较方式（§4.3） |
| 读取的参数 | `bc_avoid_pilot_mode`、`bc_avoid_pilot_population`、`bc_avoid_pilot_depth`、`bc_avoid_pilot_top_k`、`bc_avoid_pilot_sequence`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_Nav`、`m_pSnapshot` + `m_SnapshotValid`、`m_vPopulation`、`m_vFitness`、`m_vNext`（breeding scratch buffer，`SeedPopulation()` 不从它继承任何东西）、`m_IndividualCount`、`m_IndividualDepth`、`m_Individual`、`m_Rng`、`m_SessionFlow`、`m_Session`、`m_vPlan`、`m_PlanIndex`、`m_PlanTick`、`m_PlanEpoch`、`m_HeldEpoch`、`m_Target`、`m_TargetValid`；`OnReset()` 全部复位，`m_Rng` 回到 `0x1f123bb5` |

* **随机数**：`NextRand()` 是自带 LCG `m_Rng = m_Rng * 1103515245 + 12345`，返回值 `((unsigned)m_Rng >> 16) & 0x7fff`。Pilot 不用全局 `rand()`，所以在相同输入下是唯一可复现的搜索代理。
* **导航网格**：地图尺寸变化、**瓦片编辑器 `Revision()` 变化**、或导航器当前是按浅冻规则建的（`m_Nav.LightTile()`），就 `m_Session.Abort()` + `m_Nav.Rebuild(pCollision, false, 0, pEditor)` + `m_SnapshotValid = false`。Pilot 没有浅冻开关，它的网格永远不把 freeze 当可通行；编辑器定义的目标与隧道限制照常生效（§5.6）。
* **导航模式**：
  * `0` 自主：`m_Nav.Ready() && GoalTiles() > 0` 时 `m_Target = Pos + FlowAt(Pos) × 256.0f`；
  * `1` 跟随准星：`m_Target = m_Controls.m_aMousePos[g_Config.m_ClDummy]`；
  * `2` 跟随玩家：取最近的另一名 tee 位置。
  目标有效时一并写进 `Out.m_AimTarget`（所以 `bc_avoid_draw_aimbot` 也画 Pilot 的目标点）。
* **种群循环**——这一段的规则是“快照可以换代，种群不轻易重建”：
  * `ShapeChanged` 为真当且仅当：还没有种群（`m_IndividualCount <= 0` / `m_IndividualDepth <= 0`）、`bc_avoid_pilot_population` / `bc_avoid_pilot_depth` 与当前种群形状不一致、或缓冲区尺寸不匹配。
  * 只有在 `!m_SnapshotValid || ShapeChanged` 时才做一次 `CopyWorldClean(pWorld)`。
  * **取完新快照之后**：`ShapeChanged`（或种群为空）→ 调 `SeedPopulation()` 建全新的初始种群；**否则保留同一批后代**，只把 `m_vFitness` 重置为 `-1e30`、`m_Individual = 0`，让**同一批个体**在新快照上重新评估。这条是 `bc_avoid_pilot_top_k` 与交叉真正起作用的关键：如果每个新快照都重新播种，`Breed()` 产出的后代会被丢掉，搜索只是在反复评估父母。
  * 逐个体 `Begin(..., GenomeDepth, GenomeDepth, ...)`（每个个体的仿真深度与采纳计划长度都是 `bc_avoid_pilot_depth`）→ `Step(StepsLeft)`；结算 `Fitness = Survival × 100 + FlowScore × 4 − distance(m_EndPos, m_Target)（目标有效时） − DistanceAt(m_EndPos)（网格就绪时）`，`Survival` 把 9999 换成 `GenomeDepth`。
  * 整代评估完 → `Breed()`、`m_PlanEpoch++`、`m_SnapshotValid = false`，下一 tick 取一张新快照并按上面的规则继续（形状没变就还是这批后代）。
  * 播种（`SeedPopulation`）：`Count = clamp(population,1,8192)`、`Depth = clamp(depth,1,50)`；**只在种群为空或形状变化时调用**，会完整重建 `Count × Depth` 个输入。`i % 4 == 0` 跟流场、`i % 4 == 1` 朝目标（x 阈值 ±8，y < −8 时跳）、其余随机；只有在 `i % 4 >= 2 || t > 0` 时突变，概率 1/4 方向、1/9 跳跃、1/17 钩索。**没有**从 `m_vNext` 继承精英的步骤。
  * 繁殖（`Breed`）：`TopK = clamp(m_PilotTopK,1,min(100,Count))`；按 fitness 稳定降序；前 TopK 精英原样保留，其余做**均匀交叉**（每个基因随机取父 A 或父 B）+ 1/7 方向、1/23 跳跃、1/29 钩索突变；然后 `m_vPopulation.swap(m_vNext)`、fitness 重置、`m_Individual = 0`。精英主义在种群内部延续（TopK 被抄进新种群），但没有任何“跨快照继承”。
* **计划采纳与执行**：
  * `NewerGeneration = m_PlanEpoch != m_HeldEpoch`；`WantPlan = 计划为空 || (NewerGeneration && (m_PlanTick >= bc_avoid_pilot_sequence || m_PlanIndex + 1 >= (int)m_vPlan.size()))`。`m_PlanIndex` 会饱和在最后一步，所以“走完”必须用“下一步就越界”来判。
  * 采纳时要求种群与 fitness 的尺寸自洽，取**当前种群**里 fitness 最大的个体（未评估的个体是 `-1e30`，不会中选）作为新计划，并把 `m_HeldEpoch = m_PlanEpoch`。
  * 执行时用 `PLAN_GUARD_TICKS = 6` 推演这一步，**并且把玩家自己的输入也在同一窗口推演一次**：`PlanUsable = PlanSafety == 9999 || PlanSafety > PlayerSafety`。计划与玩家输入的比较因此是对称的。
  * 可用则 `m_Active = 1`、`m_SurvivalTicks = (9999 ? 6 : PlanSafety)`、reason `"pilot plan"`、`m_PlanIndex++`（未到末尾时）/ `m_PlanTick++`；否则丢弃计划（清空 + 计数与 `m_HeldEpoch` 归位）。
  * 两次世代之间当前计划继续向前走、走到末尾就保持最后一个输入，所以同一个动作不会被重启。
* **保底**：没有接管时推玩家原输入作基线，再按 `{0, -1, 1}` 找严格更优的方向，接管则 reason `"survival fallback while evolving"`，否则 `"player input safe"`。
* **路径可视化**：与 Fentbot 相同的“沿流场走 40 步、每步 24 px”，仅在 `bc_avoid_draw_path` 且已接管时填充。

### 5.6 瓦片编辑器与流场（`Avoid::CTileEditor` + `CNavigator`）

`Avoid::CTileEditor`（`avoid_tile_editor.h` / `.cpp`）是规格 §10 的落地，也是 Fentbot / Pilot 唯一的目标集合与通行限制来源。它是 `CAvoid` 的成员，通过 `SContext::m_pTileEditor` 交给代理（`avoid.cpp` 的 `ApplyInput` 组包处），菜单、世界叠加层与两个规划器读写的是同一个实例。

| 组成 | 语义 |
| :--- | :--- |
| `m_TunnelTiles` | `unordered_set<uint64_t>`，键是 `(y << 32) \| x`（`Key()`）。非空时**隧道外全部视为墙**，但见下面“终点永远可达” |
| `m_FinishTiles` | 同一套键的目标瓦片集合 |
| `Revision()` / `Touch()` | 任何一次真正改变集合的增删都自增；`Rebuild()` 把当时的 revision 记进 `EditorRevision()`，代理据此判断“网格是不是过期了” |
| `ClearAll()` | 清空两个集合（对应 spec §10.2） |
| `MarkTunnel` / `MarkFinish` / `Erase` | 单瓦片增删。`MarkFinish` 会把同一格从隧道集合里删掉（一格要么是走廊、要么是目标）；`AutoTunnels` 反过来，隧道格保持已有目标不变（见下） |
| `AutoFinish()` | 扫全图 Game 层与 Front 层的 `TILE_FINISH`（34），返回标记数（spec §10.3） |
| `AutoTunnels(vTrajectory, Width, …)` | 用已加载的 TAS 回放轨迹，按 `bc_avoid_tile_editor_auto_tunnel_width`（0–10）在每一点周围扩张成方形通道（spec §10.4）；落在管道里的目标块**不会被删掉** |
| `Interact(...)` | 世界坐标 → 瓦片坐标（`floor(pos/32)`），左键按 `bc_avoid_tile_editor_type` 画 Tunnel 或 Finish、右键擦除，越界忽略（spec §10.5）。调用点用准星落点 `m_Controls.m_aTargetPos`，不是屏幕像素反投影（§12.16） |
| 一次性动作 | `bc_avoid_tile_editor_{clear,auto_tunnel,auto_finish}` 由 `CAvoid::UpdateTileEditor()` 消费并**自复位为 0**，所以即使值被写进配置文件也不会在下次启动时重放；菜单按钮只是把它们置 1 |

`CNavigator::Rebuild(pCollision, LightTile, LightRadius, pEditor)` 把编辑器快照成 `m_pEditor` + `m_EditorRevision`，然后在 `ClassifyTiles()` 里消费它：

* `InsideTunnel = !m_pEditor || !m_pEditor->HasTunnels() || m_pEditor->IsTunnel(X, Y)`——没有隧道时全图照旧；一旦有隧道，隧道外的格子不再成为走廊或目标。
* `EditorGoal = m_pEditor->IsFinish(X, Y)`：编辑器标记的终点与地图自带的 `TILE_FINISH` 一起成为 `NAV_GOAL`。
* 两者是**或**关系：`if((InsideTunnel || EditorGoal) && !IsSolid && !IsDeathTile)`。**编辑器标记的终点块即使在隧道外也保持可通行**，否则一个画在管道外的终点永远无法到达、整张图会退化成“一个目标都没有”。
* 目标回退顺序：**编辑器 / 地图的 finish 瓦片 → 解冻瓦片**。一个 finish 都没找到时才把解冻瓦片升级成目标，而这个回退同样只考虑隧道内的格子（隧道外的不算，编辑器目标除外）。
* 浅冻规则不变（`StepMarkLight`）：只有 unfreeze 附近、仍在移动的 freeze 可通行；隧道限制不会把不可通行格放开，`NAV_LIGHT` 也不会被当目标。

**任何一次编辑都会让两个规划器重建网格与流场**：代理每 tick 比较 `m_Nav.EditorRevision()` 与 `pEditor->Revision()`，不等就 `m_Session.Abort()` + `Rebuild(...)`（Fentbot 的“网格与流场”段、Pilot 的“导航网格”段）。菜单里的 “Recalculate” 按钮做的就是手动 `Avoid.TileEditor().Touch()`，让下一次决策重新构建；构建本身是分片的（`NAV_WORK_PER_TICK`），所以状态栏会短暂回到 `grid: building`。重建会丢弃搜索状态（`m_SnapshotValid = false`），但对已经采纳的计划没有影响——计划仍然按 §5.4/§5.5 的护栏执行。

---

## 6. 参数全表

### 6.1 全表（56 个，顺序与 `config_variables_bestclient.h` 的 `bc_avoid_*` 区块一致）

“读取方”一列是引擎里的真实读取点；“可观察影响”只描述代码支持的效果。

| # | 脚本名 | C++ 成员 | 默认 | min | max | 读取方 | 可观察影响 |
| ---: | :--- | :--- | ---: | ---: | ---: | :--- | :--- |
| 1 | `bc_avoid_enabled` | `m_BcAvoidEnabled` | 0 | 0 | 1 | `CAvoid`（`IsEnabled`/`SetEnabled`/`ApplyInput`） | 关→状态 OFF、完全不介入、不额外发包（除松手交接那一 tick）；开→每 tick 决策一次，接管时由钩子索要数据包 |
| 2 | `bc_avoid_agent` | `m_BcAvoidAgent` | 0 | 0 | 4 | `CAvoid::Agent`/`SetAgent` | 切换代理（0…4）；HUD 标题、菜单页签、读取的参数集随之改变；`SetAgent` 重置新旧两个代理 |
| 3 | `bc_avoid_afk_protection` | `m_BcAvoidAfkProtection` | 0 | 0 | 1 | `CAvoid::IsAfk`（门控 4） | 打开后，连续无操作到 `afk_time` 秒时**只阻断这一 tick**：状态变 `AFK` 并打印一行，玩家一碰按键立刻恢复，不改总开关（§12.19） |
| 4 | `bc_avoid_afk_time` | `m_BcAvoidAfkTime` | 5 | 5 | 300 | `CAvoid::IsAfk`（`SSettings::m_AfkTime` 里另有一份快照） | 触发阻断所需的空闲秒数；计时是墙钟：方向 / 跳跃 / 开火 / 钩索变化或准星位移 > 2 px 就刷新 `m_LastActiveTime` |
| 5 | `bc_avoid_player_prediction` | `m_BcAvoidPlayerPrediction` | 1 | 0 | 1 | 五个代理（`SSimFlags.m_PredictPlayers`） | 关→克隆世界里其他 tee 的 core 条目被清空，推演不再被它们挡住；开→考虑其他玩家 |
| 6 | `bc_avoid_draw_path` | `m_BcAvoidDrawPath` | 1 | 0 | 1 | `CAvoid` 覆盖层 + 五个代理 | 世界坐标里画蓝色预测折线，并单独控制瓦片编辑器叠加层（与总开关无关）；关掉时代理不再重算路径（省掉一次推演），编辑过的瓦片也不再画出来 |
| 7 | `bc_avoid_draw_track_point` | `m_BcAvoidDrawTrackPoint` | 0 | 0 | 1 | 仅 `CAvoid::RenderWorldOverlay` | 画出到 Blatant 锁定瞄准点的连线 + 十字/叉（数据来自 `bc_avoid_track_point` 打开时的 Blatant） |
| 8 | `bc_avoid_draw_aimbot` | `m_BcAvoidDrawAimbot` | 0 | 0 | 1 | 仅 `CAvoid::RenderWorldOverlay` | 画出橙色瞄准目标（Blatant 真正采纳的那次瞄准，或 Pilot 的模式目标） |
| 9 | `bc_avoid_tile_editor_enable` | `m_BcAvoidTileEditorEnable` | 0 | 0 | 1 | `CAvoid::UpdateTileEditor` | 打开→世界里左键画、右键擦（菜单/控制台打开时把光标让给它们）；关→只响应菜单里的按钮 |
| 10 | `bc_avoid_tile_editor_type` | `m_BcAvoidTileEditorType` | 0 | 0 | 1 | `CAvoid::UpdateTileEditor` → `CTileEditor::Interact` | 左键放哪种瓦片：0=Tunnel（通道限制）、1=Finish（目标） |
| 11 | `bc_avoid_tile_editor_clear` | `m_BcAvoidTileEditorClear` | 0 | 0 | 1 | `CAvoid::UpdateTileEditor` | 置 1 后在下一次 `OnRender` 清空两个集合并**自复位为 0**，控制台打印一行 |
| 12 | `bc_avoid_tile_editor_auto_tunnel` | `m_BcAvoidTileEditorAutoTunnel` | 0 | 0 | 1 | `CAvoid::UpdateTileEditor` | 置 1 后用已加载的 TAS 回放轨迹生成通道（没有回放时只打印提示），自复位 |
| 13 | `bc_avoid_tile_editor_auto_tunnel_width` | `m_BcAvoidTileEditorAutoTunnelWidth` | 2 | 0 | 10 | `CTileEditor::AutoTunnels` | 轨迹每一点向四周扩张的瓦片半径；0 等于只画轨迹压到的那一格 |
| 14 | `bc_avoid_tile_editor_auto_finish` | `m_BcAvoidTileEditorAutoFinish` | 0 | 0 | 1 | `CAvoid::UpdateTileEditor` | 置 1 后扫描全图 `TILE_FINISH`（34，含 Front 层）并标记，自复位 |
| 15 | `bc_avoid_legit_direction_weight` | `m_BcAvoidLegitDirectionWeight` | 170 | 1 | 1000 | Legit（`Heuristic`） | UCT 启发式里的方向一致性权重，越大越倾向保持玩家方向 |
| 16 | `bc_avoid_legit_lifespan_weight` | `m_BcAvoidLegitLifespanWeight` | 160 | 1 | 1000 | Legit（`Heuristic`） | 存活 tick 的奖励权重，越大越优先活得久 |
| 17 | `bc_avoid_legit_hook_weight` | `m_BcAvoidLegitHookWeight` | 260 | 1 | 1000 | Legit（`Heuristic`） | 钩索状态一致性权重，越大越少改钩索 |
| 18 | `bc_avoid_legit_exploration` | `m_BcAvoidLegitExploration` | 4 | 1 | 1000 | Legit（UCT 探索项） | UCT 探索常数：越大越常试没走过的分支 |
| 19 | `bc_avoid_legit_iterations` | `m_BcAvoidLegitIterations` | 100 | 1 | 1000 | Legit（迭代上限） | 每次决策的 MCTS 迭代数；直接决定耗时，受 8 ms 护栏压制（§9.4） |
| 20 | `bc_avoid_legit_check_ticks` | `m_BcAvoidLegitCheckTicks` | 6 | 1 | 50 | Legit（单次推演深度） | 每个模拟动作必须活过的 tick 数；也决定 HUD 的 Safe 上限 |
| 21 | `bc_avoid_legit_direction` | `m_BcAvoidLegitDirection` | 1 | 0 | 1 | Legit（候选空间/接管） | 关→Legit 不改方向 |
| 22 | `bc_avoid_legit_hook` | `m_BcAvoidLegitHook` | 1 | 0 | 1 | Legit（候选空间/接管） | 关→Legit 不改钩索 |
| 23 | `bc_avoid_legit_teles` | `m_BcAvoidLegitTeles` | 0 | 0 | 1 | Legit（`m_AvoidTeles`） | 打开→把传送瓦片算作失败 |
| 24 | `bc_avoid_legit_death` | `m_BcAvoidLegitDeath` | 0 | 0 | 1 | Legit（`m_AvoidDeath`） | 打开→把死亡瓦片算作失败 |
| 25 | `bc_avoid_legit_unfreeze` | `m_BcAvoidLegitUnfreeze` | 0 | 0 | 1 | Legit（`m_AvoidUnfreeze`） | 打开→解冻瓦片在前 N tick 内算作失败 |
| 26 | `bc_avoid_legit_unfreeze_ticks` | `m_BcAvoidLegitUnfreezeTicks` | 5 | 1 | 30 | Legit（`m_UnfreezeTicks`） | 上面那条的前瞻窗口 |
| 27 | `bc_avoid_blatant_check_ticks` | `m_BcAvoidBlatantCheckTicks` | 26 | 1 | 50 | Blatant（候选推演深度） | 每个候选动作必须活过的 tick 数；也是 HUD Safe 的上限，还是 safe aim tracking 与 Auto Drag 判定用的窗口 |
| 28 | `bc_avoid_kick_in_ticks` | `m_BcAvoidKickInTicks` | 26 | 1 | 50 | Blatant（迟滞窗口 + 刹车余量窗口） | 玩家原输入（或“先按原输入、下一 tick 刹车”）能活够这么多 tick 就完全不介入；越大越“不抢手” |
| 29 | `bc_avoid_blatant_direction` | `m_BcAvoidBlatantDirection` | 1 | 0 | 1 | Blatant（候选空间） | 关→把动态候选表里 `m_Direction` 与玩家当前值不同的分支过滤掉 |
| 30 | `bc_avoid_blatant_hook` | `m_BcAvoidBlatantHook` | 1 | 0 | 1 | Blatant（候选空间） | 关→把动态候选表里 `m_Hook` 与玩家当前值不同的分支过滤掉 |
| 31 | `bc_avoid_blatant_teles` | `m_BcAvoidBlatantTeles` | 0 | 0 | 1 | Blatant（`m_AvoidTeles`） | 打开→传送瓦片算作失败 |
| 32 | `bc_avoid_blatant_death` | `m_BcAvoidBlatantDeath` | 0 | 0 | 1 | Blatant（`m_AvoidDeath`） | 打开→死亡瓦片算作失败 |
| 33 | `bc_avoid_blatant_unfreeze` | `m_BcAvoidBlatantUnfreeze` | 0 | 0 | 1 | Blatant（`m_AvoidUnfreeze`） | 打开→解冻瓦片算作失败 |
| 34 | `bc_avoid_blatant_unfreeze_ticks` | `m_BcAvoidBlatantUnfreezeTicks` | 26 | 0 | 30 | Blatant（`m_UnfreezeTicks`） | 解冻判定的前瞻窗口（可以是 0，等于关闭时间条件） |
| 35 | `bc_avoid_nsif` | `m_BcAvoidNsif` | 1 | 0 | 1 | Blatant（NSIF 分支） | 打开→没有全安全方案时回放缓存的安全输入（状态变 NSIF，计数器 +1）。`bc_avoid_track_point` 也能单独打开这条回放路径（§12.20） |
| 36 | `bc_avoid_track_point` | `m_BcAvoidTrackPoint` | 0 | 0 | 1 | Blatant（track point）+ 阶段 2 扇区扫描 | 打开→记住最后一次可钩到瓦片的准星方向并据此瞄准；同时打开门控 5 之后的扇区准星扫描（§3.5） |
| 37 | `bc_avoid_safe_aim_tracking` | `m_BcAvoidSafeAimTracking` | 0 | 0 | 1 | Blatant（瞄准过滤/探针）+ 扇区扫描的接受条件 | 打开→锁定的 track point 必须用整个 check 窗口推演一次、返回 9999（整窗安全）才进入候选；扇区扫描也必须“整窗安全”才接受一条射线；关掉则扇区扫描“活过 1 帧”即可接受、track point 无条件进入候选（裁决仍在生存搜索，§5.2） |
| 38 | `bc_avoid_auto_drag` | `m_BcAvoidAutoDrag` | 0 | 0 | 1 | Blatant（救援步骤 + 瞄准候选） | 打开→先按 client id 顺序找第一个 380 px 内、16 px 外且整窗安全的队友并立刻接管（reason `auto drag a teammate`）；否则它的瞄准（取最近的队友）作为普通候选进入瞄准层 |
| 39 | `bc_avoid_aimbot` | `m_BcAvoidAimbot` | 0 | 0 | 1 | Blatant（瞄准层门控） | 关→瞄准层（track point、Auto Drag 的瞄准、扇形扫描、auto aim、aim assist）都不产生候选；Auto Drag 的救援动作与解冻块逃逸**不受它控制**。菜单会用琥珀色提示这一点 |
| 40 | `bc_avoid_aimbot_fov` | `m_BcAvoidAimbotFov` | 90 | 10 | 360 | Blatant（扇形扫描） | 以玩家准星为中心的扫描扇角（度） |
| 41 | `bc_avoid_aimbot_segments` | `m_BcAvoidAimbotSegments` | 5 | 1 | 64 | Blatant（扇形扫描）+ 阶段 2 扇区扫描 | 扇角内的采样条数；阶段 2 扫 `Segments + 1` 条射线 × 21 帧，瞄准层内部再扫 `Segments` 条可钩射线 × check 窗口——职责不同，`bc_avoid_aimbot` 打开时两者都会跑（§3.5、§9.1） |
| 42 | `bc_avoid_auto_aim` | `m_BcAvoidAutoAim` | 0 | 0 | 1 | Blatant（瞄准选择） | 打开→把瞄准层内部 FOV 扫描里“探针存活最久”的那条射线加入待评估瞄准（与 `bc_avoid_aim_assist` 互不排斥，可同时入选） |
| 43 | `bc_avoid_aim_assist` | `m_BcAvoidAimAssist` | 1 | 0 | 1 | Blatant（瞄准选择） | 打开→把瞄准层内部 FOV 扫描里“探针整窗安全且离准星最近”的那条射线加入待评估瞄准 |
| 44 | `bc_avoid_fent_quality` | `m_BcAvoidFentQuality` | 0 | 0 | 2 | `ResolveFentPreset` | 高级关时决定 88/160/1000 个体、88/160/300 代、8 tick、10000 horizon |
| 45 | `bc_avoid_fent_advanced` | `m_BcAvoidFentAdvanced` | 0 | 0 | 1 | `ResolveFentPreset` | 打开→档位失效，改用下面四个自定义值（菜单同时切换显示） |
| 46 | `bc_avoid_fent_ticks` | `m_BcAvoidFentTicks` | 1000 | 1000 | 10000 | Fentbot（`m_FentHorizon`） | 每个候选基因的推演深度（高级模式生效） |
| 47 | `bc_avoid_fent_tweaker_actions` | `m_BcAvoidFentTweakerActions` | 50 | 50 | 5000 | Fentbot（`m_FentActions`） | 每代候选输入序列数量（高级模式生效） |
| 48 | `bc_avoid_fent_tweaker_ticks` | `m_BcAvoidFentTweakerTicks` | 1 | 1 | 30 | Fentbot（`m_FentHoldTicks`） | 一次优化周期覆盖多少个输入 tick（高级模式生效） |
| 49 | `bc_avoid_fent_tweaker_dosage` | `m_BcAvoidFentTweakerDosage` | 1 | 1 | 500 | Fentbot（`m_FentDosage`） | 每轮搜索的遗传代数（高级模式生效） |
| 50 | `bc_avoid_fent_light_tile` | `m_BcAvoidFentLightTile` | 0 | 0 | 1 | `CNavigator` + `SSimFlags.m_AllowLightFreeze` | 打开→unfreeze 半径内的 freeze 瓦片变成可通行（NAV_LIGHT），模拟器以“仍在移动”为条件接受它们（死亡瓦片永不放行） |
| 51 | `bc_avoid_fent_light_tile_radius` | `m_BcAvoidFentLightTileRadius` | 1 | 0 | 20 | `CNavigator`（BFS 步数） | 浅冻可通行的半径；改动会触发导航器重建；0 等于关闭该规则 |
| 52 | `bc_avoid_pilot_mode` | `m_BcAvoidPilotMode` | 0 | 0 | 2 | Pilot（目标选择） | 0 自主（流向 finish/unfreeze）、1 跟随准星、2 跟随最近的玩家 |
| 53 | `bc_avoid_pilot_population` | `m_BcAvoidPilotPopulation` | 2048 | 128 | 8192 | Pilot（`SeedPopulation`） | 每代个体数；乘上深度就是每代的仿真 tick 总量；改动会重建种群形状 |
| 54 | `bc_avoid_pilot_depth` | `m_BcAvoidPilotDepth` | 17 | 5 | 50 | Pilot（计划长度 + 推演深度） | 每个个体被仿真的 tick 数，也是采纳计划的最大长度；改动会重建种群形状 |
| 55 | `bc_avoid_pilot_top_k` | `m_BcAvoidPilotTopK` | 10 | 1 | 100 | Pilot（`Breed`） | 直接进入下一代的精英数，也是交叉时的父本池 |
| 56 | `bc_avoid_pilot_sequence` | `m_BcAvoidPilotSequence` | 5 | 1 | 20 | Pilot（计划刷新） | 一个有新世代可用时，计划最多被执行多少 tick 后被替换 |

### 6.2 表的可信度

* 表里的 name/default/min/max 与头文件逐字一致，并且被自检第 2 步用一个硬编码的 56 元组契约钉死（`avoid_selfcheck.sh:28-98`）。
* 自检第 3 步还会断言：每个参数都在 `src/**/*.cpp|h`（头文件本身除外）里被读到（名字或 `m_BcAvoidXxx` 成员出现），并且菜单里必须有对应控件；只有 `bc_avoid_enabled` / `bc_avoid_agent` 例外，它们分别由 `Avoid.SetEnabled(` / `Avoid.SetAgent(` 提供入口（`avoid_selfcheck.sh:117-152`）。
* 运行时代码还会再夹一次范围（`CAvoid::ReadSettings`：它逐字段 `std::clamp`，并在最后调一次 `Avoid::ResolveFentPreset`），与 CVar 的 min/max 完全一致。即使有人用脚本把配置写成越界值，引擎侧也只会用夹紧后的值；两个规划器在 `SeedGeneration` / `SeedPopulation` 里还会再夹一次（种群 1–5000 / 1–8192，长度 1–30 / 1–50）。

### 6.3 与代理的对应（与菜单 defaults 表同一划分）

| 代理 | 专属参数 | defaults 表条目 |
| :--- | :--- | ---: |
| Basic | 无（只用 `player_prediction` 与 `draw_path`） | 1（`bc_avoid_player_prediction`） |
| Legit | 12 个 `bc_avoid_legit_*` | 12 |
| Blatant | 17 个（`blatant_*`、`kick_in_ticks`、`nsif`、`track_point`、`safe_aim_tracking`、`auto_drag`、`aimbot*`、`auto_aim`、`aim_assist`） | 17 |
| Fentbot | 8 个 `bc_avoid_fent_*` + `bc_avoid_tile_editor_enable` / `_type` / `_auto_tunnel_width`（Tile Editor 页签） | 11 |
| Pilot | 5 个 `bc_avoid_pilot_*` + 同样三个 `bc_avoid_tile_editor_*` | 8 |
| 共用（“All bots” 框 + 状态栏） | `bc_avoid_enabled`、`bc_avoid_agent`、`bc_avoid_afk_*`、`bc_avoid_player_prediction`、`bc_avoid_draw_*` | 7 |

瓦片编辑器的 6 个参数里，`enable` / `type` / `auto_tunnel_width` 是持久设置，因此同时挂在 Fentbot 与 Pilot 的 defaults 表末尾（两处都点得到）；`clear` / `auto_tunnel` / `auto_finish` 是自复位的一次性动作，不写进任何 defaults 表。6 张表合计 **56** 条，正好覆盖全部参数。

### 6.4 与参考的实现差异（有意为之，详见 §12）

1. 参数名换了前缀（`krx_*` / `cl_avoid_*` → `bc_avoid_*`），数值不变；参考里成对的参数（Legit/Blatant 各一套）在本实现里也是分开的两个 CVar。
2. 危险判定按 spec §4.1 的瓦片**编号**逐项比较：死亡先判、两层都查，冻结只看 tee 自己的三个标志，浅冻豁免带速度门槛，传送覆盖全部传送类型；旧实现用位与判死亡，终点块与解冻块会被误判，这一条已在 v4.0 修掉（§4.3、§12.1）。
3. Legit 多了一个 spec 没有的 8 ms 墙钟护栏；Fentbot/Pilot 是时间切片规划器而不是同步算完，见 §12.4/§12.5。
4. Fentbot 的计划采用“暂存 + 走到末尾整体换挡”，Pilot 的计划按 `sequence` 节奏换挡并把玩家输入纳入同窗口比较；spec 没有描述这些记账字段，但它们只影响“什么时候换计划/是否采用”，不改变候选与评分。
5. 寻路的目标集合与通行限制来自 spec §10 的瓦片编辑器（v4.0 新增）：参考把流场网格一起放在编辑器对象里，本实现把流场留在 `CNavigator`，编辑器只提供 Tunnel / Finish 两个集合与 `Revision()`（§5.6）。
6. 前置流水线按 spec §5 落地（v5.0），但**门控 5 不拦 Fentbot / Pilot**；AFK 门控只阻断当前 tick，不写 `bc_avoid_enabled`（§12.18、§12.19）。
7. Blatant 的候选环按 spec §7.2 的动态 5 分支表生成（不是 `{0,-1,1} × {0,1}` 的笛卡尔积），并且用 `std::async` 并发推演（spec §7.5）；NSIF 的单步缓存在回放时保留、接管仍要求“严格活得更久”（§12.20）。
8. Legit 在 MCTS 之后按 spec §8.3 做 26-Tick 后验增益仲裁（v5.0）；参考之外的两段自造护栏（钩索二次校验、方向保护）已删除，`BestSurvHook0` 一类保护性否决也不再存在（§12.21）。

### 6.5 旧参数名 → 现状（升级/迁移时看这张表）

| 旧名（已从代码中删除） | 现在用什么 |
| :--- | :--- |
| `bc_avoid_active` | 无。唯一开关是 `bc_avoid_enabled`（迁移时会被强制置 0） |
| `bc_avoid_show_hud` | 无 CVar。状态面板是 `HudLayout::MODULE_AVOID` |
| `bc_avoid_show_visuals` | 拆成 `bc_avoid_draw_path` / `bc_avoid_draw_track_point` / `bc_avoid_draw_aimbot` |
| `bc_avoid_sensing_radius` | 无。判定改为克隆世界推演里的 `TickHitHazard` |
| `bc_avoid_check_ticks` | 拆成 `bc_avoid_legit_check_ticks` / `bc_avoid_blatant_check_ticks` |
| `bc_avoid_direction_assist` / `bc_avoid_hook_assist` | 拆成 `bc_avoid_legit_direction` / `bc_avoid_legit_hook` / `bc_avoid_blatant_direction` / `bc_avoid_blatant_hook` |
| `bc_avoid_direction_weight` / `bc_avoid_hook_weight` / `bc_avoid_life_weight` | `bc_avoid_legit_direction_weight` / `bc_avoid_legit_hook_weight` / `bc_avoid_legit_lifespan_weight` |
| `bc_avoid_quality` | `bc_avoid_legit_iterations`（Legit）；Fentbot 的“质量”是 `bc_avoid_fent_quality` |
| `bc_avoid_randomness` | `bc_avoid_legit_exploration` |
| `bc_avoid_tile_death` / `_freeze` / `_tele` / `_unfreeze` / `bc_avoid_unfreeze_ticks` | 拆成 `bc_avoid_legit_*` 与 `bc_avoid_blatant_*` 两套；冻结永远算危险，没有开关 |
| `bc_avoid_afk_protect` / `bc_avoid_afk_time` | `bc_avoid_afk_protection` / `bc_avoid_afk_time`（默认值也变了：60 秒 → 5 秒） |
| `bc_avoid_aimbot_mode` | 拆成 `bc_avoid_auto_aim` + `bc_avoid_aim_assist` |
| `bc_avoid_aimbot_fov` / `_segments` | 同名保留，范围改为 10–360 / 1–64 |
| `bc_avoid_log` / `bc_avoid_debug_override` | 无。调试入口是 `avoid_status` / `avoid_reset` 与三个 `draw_*` |
| （v3.0 里没有对应项） | v4.0 新增的 6 个 `bc_avoid_tile_editor_*`：Tunnel / Finish 目标集与通行限制，规格 §10（§5.6） |

---

## 7. 界面规格

入口：主菜单 → TAS& → 第二个子页签 **Avoid**（`menus_tas.cpp:595-621`，`g_Config.m_BcTasTab == 1` → `CMenus::RenderSettingsAvoid`，实现从 `menus_avoid.cpp:309` 开始）。

### 7.1 排版契约：`CUi::DoLabel` 不会自动换行

这是本节所有尺寸代码存在的理由，也是下一个人最容易踩的坑：

* `SLabelProperties::m_MaxWidth` 默认是 **−1**（`src/game/client/ui.h:217`），而 `DoLabel` 把这个值原样复制进文本光标：`Cursor.m_LineWidth = LabelProps.m_MaxWidth`（`src/game/client/ui.cpp:872` 与 `ui.cpp:904`）。
* 文本层的换行分支要求 `m_LineWidth > 0.0f`（`src/engine/client/text.cpp:1805`、`1936`），所以**不给 `m_MaxWidth` 就等于不换行**。
* 与此同时 `m_EnableWidthCheck` 默认 true、`m_MinimumFontSize` 默认 5.0f（`ui.h:220-221`）：字符串放不下时引擎不是换行，而是**一路缩小字号直到 5 px 下限**（`ui.cpp:805-811`）。一段没有 `m_MaxWidth` 的说明文字因此会渲染成一条几乎读不清的 5 px 细线，而不是换行成两行。
* 本仓库的既有写法是显式给宽度，例如 `Props.m_MaxWidth = Label.w - 5.0f;`（`src/game/client/components/menus_settings_player.cpp:152`）。Avoid 页面照这个约定办：**页面里所有说明文字都经由下面两个帮手绘制**。

三个排版帮手（`menus_avoid.cpp:66-91`）：

| 帮手 | 作用 |
| :--- | :--- |
| `AvoidHintHeight(pTextRender, pText, Size, Width)`（66-71） | 用 `ITextRender::TextBoundingBox(Size, pText, -1, Width).m_H` 量出这段文字在给定宽度下**换行后**需要的高度；空串或宽度 ≤1 返回 0 |
| `AvoidHint(pUi, pTextRender, Rect, pText, Size)`（73-82） | 唯一的绘制入口：`SLabelProperties Props; Props.m_MaxWidth = Rect.w;`（77-78）→ `DoLabel(..., Props)`（80）；颜色 `AVOID_TEXT_DIM`，画完复原 |
| `AvoidHintBottom(pUi, pTextRender, pRect, pText, Size)`（85-91） | 面板尾注：先用 `AvoidHintHeight` 量高，再 `HSplitTop(min(Needed, 剩余高度))` 从面板底部**精确预留**这段高度并绘制 |

因此：**面板里已经没有给说明文字用的固定高度槽位**（旧的 `HSplitTop(46/56/60)` 全部删除），页面里也不存在直接调用 `AvoidHint` 的地方 —— 每一处尾注都是 `AvoidHintBottom`（当前 19 处），唯一的例外是 Blatant 的琥珀色警告：它自己量高、自己设 `m_MaxWidth`（`menus_avoid.cpp:742-750`：`AvoidHintHeight` → `HSplitTop(...)` 预留 `AimHint` → `AimProps.m_MaxWidth = AimHint.w` → `DoLabel`）。两个语言文件的文案长度不同时面板高度会跟着变，这是有意的。自检第 5 步把这条契约钉死了（§11）。

### 7.2 布局

```
┌─ StatusBar (高 52) ────────────────────────────────────────────────────────────────┐
│ [状态徽章] [代理徽章]           Plan: <reason> / safe·cost·took over │ [Enable] │
│                                                                      │ [Reset]  │
│                                        bind X toggle bc_avoid_enabled 1 0 (右对齐) │
├─ LeftColumn (与右栏间隔 14) ────┬─ RightColumn ────────────────────────────────────┤
│ Gores bot (高度按实测文案推导)  │ [页签 / 标题…]                 [Defaults (92 宽)] │
│   [Basic][Legit][Blatant]       │ ┌ 参数面板（Margin 10）─────────────────────────┐ │
│   [Fentbot][Pilot]              │ │ 该代理的参数（见 7.4），尾注自动预留高度      │ │
│   [x] Enable Gores bot          │ └───────────────────────────────────────────────┘ │
│   代理说明文字（底部对齐）      │                                                  │
│ All bots (高度按实测文案推导)   │                                                  │
│   [x] Player prediction         │                                                  │
│   [x] AFK protection            │                                                  │
│   AFK time ──○──── 5–300 s      │                                                  │
│ Visuals                         │                                                  │
│   [x] Status HUD                │                                                  │
│   [x] Render path               │                                                  │
│   [ ] Render track point        │                                                  │
│   [ ] Render aimbot target      │                                                  │
└─────────────────────────────────┴──────────────────────────────────────────────────┘
```

细节（行号为当前快照）：

* 状态栏：`MainView.HSplitTop(52.0f, &StatusBar, ...)`、间隔 8、左右栏 `VSplitMid(..., 14.0f)`（320-322）。
* **状态栏的三段宽度按比例夹紧**（332-338）：以 `Inner.w` 为 `BarWidth`，状态徽章 `min(104, BarWidth*0.13)`、代理徽章 `min(140, BarWidth*0.18)`、右侧块 `min(210, BarWidth*0.28)`。原因是 `VSplitLeft`/`VSplitRight` **不做夹紧**：在 4:3 窗口或调高 `cl_ui_scale` 时，原来的固定宽度会把中间的文字区压成 0，标签直接画到按钮上（328-330 的注释写明了这一点）。
* 状态徽章显示 `OFF` / `WATCH` / `ASSIST` / `NSIF` / `AFK`（`CAvoid::StateName`，走本地化），颜色由 `AvoidStateColor` 决定（NSIF 红、ASSIST 橙、WATCH 绿、AFK 灰）。总开关关闭时一律显示 `OFF`（342-343）。
* 代理徽章显示 `AVOID: <代理名>`，颜色每个代理一套（`AvoidAgentColor`）（345-349）。
* 两行信息（352-353）：第一行 `Plan: <reason>`；第二行**永远是纯 tick 计数** `safe %d | cost %.2f ms | took over %d/%d`（364-366），选中的是 Fentbot 或 Pilot 时再追加 `grid: ready|building`（367-373）。这里没有 9999 分支 —— 没有任何代理会把 `SIMULATION_SAFE_CONSTANT` 报成 `m_SurvivalTicks`（362-363 的注释写明了这一点），所以那句话删掉了，对应的本地化词条也一起删了（§8.3）。
* 左栏三个盒子**没有固定高度**（401-444）：`BOX_GAP = 6.0f`；首选高度由**实测文案高度**推导 —— `AgentCopyHeight = AvoidHintHeight(..., (LeftColumn.w - 16.0f) * 0.5f)`（409-415，按半栏宽度量，是保守上界），`GeneralCopyHeight = AvoidHintHeight(..., LeftColumn.w - 16.0f)`（416-419），`aPreferred = {max(120, 120 + AgentCopyHeight), max(126, 110 + GeneralCopyHeight), 126}`（419-422）；最小高度 `aMinimum = {106, 106, 106}`（423）。可用高度不足时三个盒子按同一个比例因子在最小与首选之间线性收缩（425-437），Visuals 盒再额外夹到剩余高度（443）——所以窗口变矮时是先裁说明文字，而不是把复选框挤出屏幕。
* 代理说明文字不再有固定 56 高的槽位：由 `AvoidHintBottom` 在盒子底部按实测高度预留（505）；“All bots” 的尾注同样（525-526）；“Visuals” 盒只有四个复选框、没有尾注（528-556）。
* 代理选择分两行（Basic/Legit/Blatant 一行，Fentbot/Pilot 一行），点击调用 `Avoid.SetAgent()`（462-479）。

### 7.3 StatusBar 右侧

* `Enable` / `Disable` 按钮 → `Avoid.SetEnabled()`（等价于 `bc_avoid_enabled` 取反）（387-389）。
* `Reset counters` → `Avoid.ResetCounters()`（只清零 decisions / overrides / nsif 三个计数器并打印一行，不动任何搜索状态）（390-392）。
* 绑定提示固定为字符串 `bind X toggle bc_avoid_enabled 1 0`（不翻译，因为它是命令本身）（394-396）。

### 7.4 页签、右栏头部与每个页签暴露的参数

**右栏头部是统一的**（`menus_avoid.cpp:563-612`）：无论选中的是哪个代理，都先切出一条 22 高的 `NavBar` 并在右侧留出 92 宽的 `Defaults` 按钮（570-576）；`PanelCount > 1` 时 `NavBar` 画页签，否则画一行标题 —— 单页签画该页签名，**Basic（0 页签）画代理名**（578-594）。也就是说 Basic 也有可达的 “Defaults” 按钮（它重置 `bc_avoid_player_prediction` + 共用表），`g_aBasicDefaultParams` 不再是死代码。

页签选择按代理记忆（`static int s_aPanelByAgent[CAvoid::NUM_AGENTS]`，`menus_avoid.cpp:559`）。

| 代理 | 页签数 | 页签 | 暴露的参数 |
| :--- | ---: | :--- | :--- |
| Basic | 0 | （无页签，标题是代理名；内容只有两段 `AvoidHintBottom` 说明，`626-632`） | 无（只重置共用表 + Basic 表） |
| Legit | 3 | Settings（635） | `bc_avoid_legit_direction`、`bc_avoid_legit_hook`、`bc_avoid_legit_check_ticks` |
| | | Priority（654） | `bc_avoid_legit_iterations`（Quality）、`bc_avoid_legit_exploration`（Randomness）、`bc_avoid_legit_direction_weight`、`bc_avoid_legit_hook_weight`、`bc_avoid_legit_lifespan_weight`；另有一条固定提示说明 8 ms 护栏（674-677） |
| | | Tiles（683） | `bc_avoid_legit_death`、`bc_avoid_legit_teles`、`bc_avoid_legit_unfreeze`、`bc_avoid_legit_unfreeze_ticks` |
| Blatant | 4 | Avoid（701） | `bc_avoid_nsif` |
| | | Settings（715） | `bc_avoid_blatant_direction`、`bc_avoid_blatant_hook`、`bc_avoid_blatant_check_ticks`、`bc_avoid_kick_in_ticks`、`bc_avoid_track_point`、`bc_avoid_safe_aim_tracking`、`bc_avoid_auto_drag`；当 `bc_avoid_aimbot` 关闭时用琥珀色（`0.95,0.72,0.30`）提示“这些开关需要先到 Aimbot 页签打开内部瞄准”（742-750） |
| | | Tiles（756） | `bc_avoid_blatant_death`、`bc_avoid_blatant_teles`、`bc_avoid_blatant_unfreeze`、`bc_avoid_blatant_unfreeze_ticks` |
| | | Aimbot（774） | `bc_avoid_aimbot`、`bc_avoid_aimbot_segments`、`bc_avoid_aimbot_fov`、`bc_avoid_auto_aim`、`bc_avoid_aim_assist` |
| Fentbot | 2 | Calculation（797） | `bc_avoid_fent_light_tile`、`bc_avoid_fent_light_tile_radius`、`bc_avoid_fent_advanced`，以及互斥显示的两组：高级关 → `bc_avoid_fent_quality`（Low/Mid/Max 分段控件）；高级开 → `bc_avoid_fent_ticks`、`bc_avoid_fent_tweaker_actions`、`bc_avoid_fent_tweaker_ticks`、`bc_avoid_fent_tweaker_dosage` |
| | | Tile Editor（851） | 见下方“Tile Editor 页签” |
| Pilot | 3 | Main（908） | `bc_avoid_pilot_mode`（Autonomous / Follow cursor / Follow player 分段控件 + 自动量高的模式说明） |
| | | Settings（937） | `bc_avoid_pilot_population`、`bc_avoid_pilot_depth`、`bc_avoid_pilot_top_k`、`bc_avoid_pilot_sequence` |
| | | Tile Editor（851，与 Fentbot 共用同一个面板分支） | 见下方 |
| 所有代理 | — | 左栏 “All bots”（509-527） | `bc_avoid_player_prediction`、`bc_avoid_afk_protection`、`bc_avoid_afk_time` |
| 所有代理 | — | 左栏 “Visuals”（528-556） | HUD 开关（`HudLayout`）+ `bc_avoid_draw_path`、`bc_avoid_draw_track_point`、`bc_avoid_draw_aimbot` |

**Tile Editor 页签**（`PANEL_TILE_EDITOR`，Fentbot 与 Pilot 共享同一个 case）自上而下是：

| 控件 | 行为 |
| :--- | :--- |
| 标题 “Tile editor” + `Enable editor` 复选框 | 写 `bc_avoid_tile_editor_enable`（世界里左键画 / 右键擦的总开关） |
| 标题 “Tile type” + 分段控件 `Tunnel` / `Finish` | 写 `bc_avoid_tile_editor_type`（左键放哪种瓦片） |
| 标题 “Auto tunnels” + 滑条 `Auto tunnel width`（0–10，单位 `tiles`） | 写 `bc_avoid_tile_editor_auto_tunnel_width` |
| 四个动作按钮：`Recalculate` / `Auto finish` / `Auto tunnels` / `Clear all` | `Recalculate` 调用 `Avoid.TileEditor().Touch()`；`Auto finish` / `Auto tunnels` / `Clear all` 分别置位 `bc_avoid_tile_editor_auto_finish` / `_auto_tunnel` / `_clear`，由 `CAvoid::UpdateTileEditor()` 消费（自复位）并打印一行回显 |
| 计数行 `Tunnel tiles: N    Finish tiles: M` | 直接读 `Avoid.TileEditor().TunnelCount()` / `FinishCount()`，每次重绘刷新 |
| 尾注 | 说明“Tunnel 是唯一可走的格子、Finish 是导航目标；编辑器为空时用地图终点块并以解冻块兜底；打开开关后在世界里点击绘制、右键擦除” |

新词条（两种语言都已补齐，§8）：`Tile Editor`、`Tile editor`、`Enable editor`、`Tile type`、`Tunnel`、`Finish`、`Auto tunnels`、`Auto tunnel width`、`tiles`、`Recalculate`、`Auto finish`、`Clear all`、`Tunnel tiles`、`Finish tiles`，以及回显用的 `Avoid tile editor: all edited tiles cleared` / `Avoid tile editor: finish tiles marked` / `Avoid tile editor: tunnel tiles marked` / `Avoid tile editor: no TAS replay loaded` / `Avoid tile editor: pathfinding grid recalculated`。

### 7.5 “Defaults” 按钮的行为

* 统一头部保证**每个代理**都有这个按钮（Basic 也算了），可见 `menus_avoid.cpp:597-612`。
* 点击后遍历两张表：该代理的 defaults 表 + `g_aGeneralDefaultParams`，对每个名字调用 `IConfigManager::Reset(name)`（604-608），也就是**从 CVar 定义处恢复默认值**，代码里没有第二份默认值副本可以漂移。
* 表总条目 **56** 条，正好覆盖全部参数：general 7、Basic 1、Legit 12、Blatant 17、Fentbot 11、Pilot 8（自检第 3 步断言 6 张表、无重复、名字都已声明）。
* 反馈：按钮文字在 2 秒内变成“已恢复”（`Client()->LocalTime()` 比较），并在控制台打印 `Avoid: parameters restored to their defaults` 的本地化字符串（609-610）。

### 7.6 HUD 模块接线

| 项 | 值 | 出处 |
| :--- | :--- | :--- |
| 枚举 | `HudLayout::MODULE_AVOID` | `hud_layout.h:40` |
| 持久化 id | `"avoid"` | `hud_layout.cpp:81` |
| 显示名 | `"Avoid"`（经 `BcLocalize`） | `hud_layout.cpp:109` 的名字表 + `Name()` 访问器（`hud_layout.cpp:516-520`） |
| 默认布局 | X=286, Y=52, scale=100, 位置模式=左上, **enabled=false**, background=true, color=0x66000000, alpha=100 | `hud_layout.cpp:28-53`（索引 24） |
| 持久化位置 | `BestClient/hud_layout.cfg`（配置域 `HUDLAYOUT`） | `config_domains.h:11` |
| 编辑器 | 在白名单里；取 `m_Avoid.GetHudEditorRect()`；把模块加入可视化列表；`HudLayout::SetEnabled` 写同一个状态 | `hud_layout.cpp:475-503`、`hud_editor.cpp:528-531`、`587`、`950` |
| 菜单入口 | 左栏 “Visuals” → “Status HUD” 复选框，`HudLayout::SetEnabled(MODULE_AVOID, !HudEnabled)` | `menus_avoid.cpp:544-547` |
| 可见性判定 | `CAvoid::IsHudVisible()` = `HudLayout::IsEnabled(MODULE_AVOID)` | `avoid.cpp` 的 `IsHudVisible` |

因此“HUD 开不开”只有一份状态：菜单复选框与 HUD 编辑器改的是同一个 `HudLayout` 值；`bc_avoid_enabled` 只决定状态栏里显示什么内容（关闭时状态徽章为 `OFF`）。

面板几何（`avoid.cpp` 的 `RenderHudModule`）：基准尺寸 122×62（HUD 画布 500×300，常量在 `avoid.cpp:32-40`），scale 夹到 0.25–3.0，alpha 夹到 0.05–1.0；头部 9 高、每行 7.5 高、标题字号 5.5、行字号 5.0；徽章宽 34、标签列宽 40；四行固定为 `Plan:` / `Safe:` / `Cost:` / `Override:`，值分别为 reason、存活 tick（`%d tick`，**没有 9999 分支**）、`X.XX ms`、`Overrides / Decisions`。
面板背景跟随模块设置：`if(Layout.m_BackgroundEnabled) Canvas.Draw(ColorRGBA(0.06f,0.08f,0.12f,0.85f × Alpha), ...)`（773-774）——背景开关由 HUD 编辑器统一控制，底色仍用面板自己的深色常量；位置、缩放、位置模式与 alpha 同样来自 `HudLayout`。
HUD 编辑器预览走 `GetHudEditorRect()`（强制取矩形）+ `RenderPreview()`（`ForcePreview = true`，此时固定显示 Basic 代理、`WATCH` 状态、`player input safe`、`26 tick`、`0.00 ms`、`0 / 0`）。

### 7.7 四个世界覆盖层（`avoid.cpp` 的 `RenderWorldOverlay` 与 `RenderTileEditorOverlay`）

前三个覆盖层的前提是 `IsEnabled()` 且三个 `draw_*` 里至少一个打开；**瓦片编辑器叠加层只要求 `bc_avoid_draw_path`，与总开关无关**（`CAvoid::OnRender` 里它是独立的一次调用）：图块通常是在启用机器人之前就画好的，而且它是流场的输入。渲染时把屏幕切到以相机中心/缩放构造的世界坐标，画完恢复。

| 覆盖层 | 开关 | 数据 | 画法 |
| :--- | :--- | :--- | :--- |
| 预测路径 | `bc_avoid_draw_path` | `m_vLastPath`（代理在 `bc_avoid_draw_path` 打开时填充） | 至少 2 个点时逐段画线，颜色 `(0.40,0.80,1.00,0.85)`；跳过非有限坐标 |
| Blatant 锁定瞄准点 | `bc_avoid_draw_track_point` | `m_Telemetry.m_TrackPoint`（Blatant 在 `bc_avoid_track_point` 打开且钩索射线命中时写入） | 从角色到目标一条连线 + 十字 + 叉（5 条线段），颜色 `(0.36,0.68,1.00,0.90)` |
| 瞄准目标 | `bc_avoid_draw_aimbot` | `m_Telemetry.m_AimTarget`：Blatant **真正采纳**的那次瞄准（只在接管分支里发布），或 Pilot 的模式目标 | 连线 + 十字（3 条线段），颜色 `(0.98,0.62,0.16,0.90)` |
| 瓦片编辑器 | `bc_avoid_draw_path` | `CAvoid::m_TileEditor` | 屏幕内可见的编辑瓦片画成半透明方块：Tunnel 颜色 `(0.25,0.62,1.00,0.16)`、Finish 颜色 `(0.35,0.95,0.45,0.22)`；同一格同时属于两个集合时按 Finish 画。只为世界屏幕矩形覆盖到的瓦片建批，不遍历全图 |

两个标记都要求与角色的距离 > 1.0，避免在脚下画出一团噪声。瓦片叠加层在 `RenderWorldOverlay` 之后绘制（半透明，路径线仍可辨认），并且只要两个集合非空就画，即使机器人没接管、甚至总开关是关的。

---

## 8. 本地化

### 8.1 `BcLocalize` 的契约

```cpp
// src/game/localization.cpp:28-33
const char *BcLocalize(const char *pStr)
{
    const char *pNewStr = g_Localization.FindString(str_quickhash(pStr), str_quickhash("BestClient"));
    return pNewStr ? pNewStr : Localize(pStr);
}
```

* 先按 `(原文, "BestClient")` 查表；查不到再退回 `Localize(pStr)`（默认上下文，即 DDNet/TClient 的通用词条）。
* 语言文件分三层加载（`gameclient.cpp:369-384`，切语言时 `1625-1647` 重放同一序列）：
  1. `languages/<file>`，`Clear = true`（基础语言）；
  2. `tclient/<file>`，`Clear = false`（叠加 TClient 词条）；
  3. `BestClient/<file>`，`Clear = false`（叠加 BestClient 词条，也就是本模块用的那层）。
  最终 `m_ClLanguagefile` 形如 `languages/simplified_chinese.txt`，于是第三层就是 `data/BestClient/languages/simplified_chinese.txt`。

### 8.2 硬性约定

1. **每个 `BcLocalize("…")` 的字面量必须在 `[BestClient]` 上下文下存在同名 key**，两个语言文件都要有：`data/BestClient/languages/simplified_chinese.txt`、`data/BestClient/languages/russian.txt`。
2. key 必须与 C++ 字面量逐字节一致（查找用的是字符串哈希，大小写、空格、标点都算）。
3. 文件格式：可选的 `#` 注释行；`origin` 行后面紧跟一行以 `== ` 开头的译文；上下文行是 `[BestClient]`。加载器对格式错误会打印 `log_error` 并跳过，所以自检第 4 步用同构的 Python 解析器复刻了 `CLocalizationDatabase::Load` 的规则（`avoid_selfcheck.sh:175-205`），格式错误会直接让自检失败。
4. 约定上每条词条都单独带一行 `[BestClient]` 上下文（现有两个文件就是这样组织的，各 885 条 `[BestClient]` 词条）。

### 8.3 当前规模与校验方式

* 参与契约的源文件（与自检第 4 步一致）：`avoid.cpp`、`avoid_engine.cpp`、`menus_avoid.cpp`、`menus_tas.cpp`、`hud_layout.cpp`。
* 当前唯一 key 数 **195**（自检断言 `> 120`；旧的状态栏 “safe for the whole lookahead” 词条已随该死分支一起删除；v4.0 新增的 Tile Editor 页签与回显词条、v5.0 的 `Avoid: AFK protection paused the bot` 都在内）；两个语言文件各 **885** 条 `[BestClient]` 词条，缺失 **0**（写作时按自检同样的解析器逐条复核）。
* 引擎侧的 reason 文本**不是**本地化标签：`avoid_engine.cpp` 里 0 处 `BcLocalize`；代理写的是英文常量，HUD 与菜单原样显示 `m_aReason`。当前共 23 个引擎取值：

  `no world`、`no character`、`player input safe`、`brake before hazard`、`steer left before hazard`、`steer right before hazard`、`steer away from hazard`、`no safer plan`、`auto drag a teammate`、`escape to an unfreeze tile`、`NSIF: replay saved safe input`、`hook the safe aim`、`aim clear of the hazard`、`hook to safety`、`release hook`、`post hoc gain below 1 tick`（v5.0，26-Tick 仲裁的驳回路径）、`search budget reached`、`survival fallback while searching`、`fentbot plan`、`pilot plan`、`survival fallback while evolving`、`steer to safety`、`hook and steer to safety`。

  Legit 真正接管时还会把增益拼在基础 reason 后面，形如 `steer to safety (+14)`（格式串 `"%s (+%d)"`）。
* 组件侧（`avoid.cpp` 的 `PreActivationName()` 与调度器）另有 8 个字面量，它们同样不进 `BcLocalize`：`ready`、`gamemode blacklisted`、`player not active`、`character frozen`、`AFK protection`、`probe: player input safe`、`sector scan locked the crosshair`、`agent unavailable`。
  两边合计 30 个唯一取值（`no world` 两边都会用）。
  控制台/界面上它们出现在 `Plan: <reason>`（HUD 与菜单状态栏）与 `[avoid] last plan: <reason>`（`avoid_status`）。要改这些文本就等于改行为可读性，注意别把它们塞进 `BcLocalize` 除非同时补两份语言文件。

### 8.4 新增一种语言

1. 把语言写进语言索引（`data/languages/index.txt`；TClient 的词条索引在 `data/tclient/languages/index.txt`）：文件名、母语名、ISO 3166-1 数字国家码、RFC 3066 标签，各占一行，后三者以 `== ` 开头。
2. 复制一份 `data/BestClient/languages/<name>.txt`（例如以 `russian.txt` 为模板），把全部 `[BestClient]` 词条的 `== ` 行换成新语言；不要动 key 行。
3. 缺的词条会被 `BcLocalize` 退化到 `Localize()`，再退化到英文原文——界面不会崩，但自检第 4 步会因为 key 缺失而失败，所以必须补齐。
4. 游戏内 `cl_languagefile "languages/<name>.txt"` 切换（或在设置里选语言）；切换会重放三层加载（`gameclient.cpp:1625-1647`）。
5. 跑 `./scripts/avoid_selfcheck.sh`，第 4 步通过即表示契约完整。

---

## 9. 性能与预算

### 9.1 一次决策的推演预算（流水线 + Basic / Blatant / Legit）

**流水线的定位：安全时一次决策只有 1 次 10 帧克隆。**

| 阶段 | 推演次数 | 每次的 tick 数 | 触发条件 |
| :--- | :--- | :--- | :--- |
| 门控 5 探针 | 1 | 10（`PROBE_CHECK_TICKS`） | 门控 0–4 全部通过，且选中的是 Basic / Legit / Blatant |
| 阶段 2 扇区扫描 | ≤ `bc_avoid_aimbot_segments` + 1（默认 6） | 21（`SECTOR_SCAN_TICKS`） | 探针判定“余量不足”，且 `bc_avoid_track_point` 或 `bc_avoid_aimbot` 打开 |

* 探针就是“安全状态 0 唤醒、满帧率”的全部来源：玩家原输入在 10 帧里活过 7 帧时，这一次 10 帧克隆就是这一 tick 的全部开销——MCTS、贪心环、瞄准扫描一次都不会被调用，输入一个比特都不改（§3.5）。
* 探针判定余量不足之后，扇区扫描仍是**可选**的：两个开关都关时 `RunSectorScan()` 直接返回 `false`，一次推演都不做。
* 阶段 2 与瞄准层会**各扫一遍** FOV（职责不同，§5.2 第 7b 点）：`bc_avoid_aimbot` 打开时，一个危险 tick 的扫描成本是阶段 2 的 `(Segments + 1) × 21` 帧推演**加上**瞄准层的 `≤ Segments` 条可钩射线 × check 窗口探针；两者都随 `bc_avoid_aimbot_segments` 线性增长。
* Fentbot / Pilot 不过门控 5（§12.18），所以“安全时只有 1 次克隆”这句话对它们不成立：它们每 tick 花的是自己的分片预算（§9.5），只有闭环护栏那 1–2 次推演是固定开销。

**代理本体（都在单次 `GetAction` 里把活干完）：**

| 代理 | 每次决策的推演次数（上限） | 每次推演的 tick 数 | 备注 |
| :--- | :--- | :--- | :--- |
| Basic | 4（1 基线 + ≤3 候选） | 6（`BASIC_CHECK_TICKS`，写死） | 基线安全时 1 次就返回；候选命中 9999 立即 break |
| Blatant | 1（kick-in）+ ≤1（刹车余量：两段计划）+ **2（提前松勾，命中即返回）** + ≤63（Auto Drag，只对距离落在 380/16 px 之间的人推演）+ **≤4（二段跳：基线 1 + 三方向 3，命中即返回）** + **≤10（雷达：≤5 条射线 × 2 个倾靠方向，命中即返回）** + ≤2（解冻块逃逸：勾索 / 不勾索，命中即返回）+ **≤12（候选环，并发）** + ≤1（safe aim tracking 探针）+ ≤`aimbot_segments`（瞄准层内部的 FOV 探针，只有 `IsHookable()` 命中的采样才推演）+ ≤96（入选瞄准 ≤8 × 候选环 ≤12）+ ≤1（NSIF 回放）+ ≤1（画路径） | `kick_in_ticks` / `blatant_check_ticks`（≤50） | 瞄准层与三个可选救援步骤默认全关（`aimbot = 0`、`auto_drag = 0`、`blatant_unfreeze = 0`）时代理本体约 **≤21**；把 `bc_avoid_aimbot_segments` 拉满 64、并打开 aimbot 与两个可选救援步骤时约 ≤190（代理之外还有阶段 2 的 `(Segments + 1) × 21` 帧）。第 2/4/5 级一旦命中就立刻返回，后面的级联一次都不跑；任一候选返回 9999（`FoundSafe`）会跳过整个瞄准层 |
| Legit | ≤ `legit_iterations`（默认 100，≤1000）+ 2（26-Tick 仲裁两路）+ ≤1（画路径） | `legit_check_ticks`（≤50），仲裁固定 26 | 每次迭代 1 次推演 + 建树/回传；受 8 ms 墙钟护栏限制（§9.4） |

每推演一次就 `CopyWorldClean` 一份世界，这是本模块 CPU 占用的主要来源。Blatant 的瞄准层与两个救援步骤默认都是关的，把 `aimbot_segments` 调大之前先看 `Cost` 读数。

### 9.2 并发、线程开销与并发安全性（Blatant 的候选环）

Blatant 第 7 步的候选环交给 `SimulateBranchesParallel()`（spec §7.5，`0x14032eb50`）：

* 单候选直接内联在调用线程里跑（为一次克隆创建一个线程不划算）；多候选时每个候选取一个 `std::async(std::launch::async)`，`future` 在这一轮结束时析构，也就是**每次决策最多创建/销毁 12 个线程**（`BuildBlatantCandidates` 的笛卡尔积最多返回 3 × 2 × 2 = 12 条；v5.0 是 5 条）。没有线程池，这是这一层唯一的额外系统开销。
* **失败回退**：`std::async` 抛 `std::system_error`（线程创建失败）时，代码先把已经发出的 future 全部 `wait()` 掉，再在调用线程里**串行重做整轮**。决策不会丢，结果与并发路径逐条一致，只是这一轮的墙钟变长。
* **并发安全性的论证**（也写在代码注释里）：每个分支在自己的 `CForwardSim` 里 `CopyWorldClean` 出一份**独立**世界，只读共享的基世界；推演共用的碰撞数据、调参、地图数据都是只读的；`m_WorldConfig.m_PredictEvents = false` 保证推演不触发副作用；`CForwardSim` 只持有自己的世界 / 角色指针 / 碰撞指针 / 本地 client id，`SetPredictPlayers()` 也只清自己克隆的 `m_Core.m_apCharacters`。因此分支之间没有共享可写状态，**预测世界不接触任何客户端组件**。

### 9.3 Blatant 没有墙钟护栏

只有 Legit 有 deadline。Blatant 的最坏成本随 `aimbot_segments × blatant_check_ticks` 线性增长（§9.1 的公式）；慢机器上把 `bc_avoid_aimbot_segments` 调小、把 `bc_avoid_blatant_check_ticks` 调小，或者关掉 `bc_avoid_aimbot`；`bc_avoid_auto_drag` / `bc_avoid_blatant_unfreeze` 每次决策也会各多出最多 63 次 / 2 次推演。
接管 gate（`BestSurvival > KickSafety`，§5.2 第 7 点）只影响“是否应用计划”，不改变推演次数：所有候选仍然要算完才知道谁活得更久。

### 9.4 Legit 的 8 ms 护栏

* `LEGIT_DEADLINE_MS = 8.0`，检查点是 `(Iter & 7) == 0`，即每 8 次迭代看一次表：护栏是**检查点**而不是硬中断，最坏会多跑 7 次迭代。
* 命中护栏时 `break`，然后走规格 §8.3 的最终规则：在**已经访问过**的根子节点里取“利用值 + 启发值”最大者继续；一个都没访问到就不接管，玩家的输入原样保留（§5.3）。
* 参考默认值（100 次迭代 / 6 tick）在任何现代机器上都碰不到这个护栏（`LEGIT_DEADLINE_MS` 的注释也这么写）；把 `bc_avoid_legit_iterations` 拉到 1000 且 `check_ticks` 拉到 50 时才会稳定命中，此时 HUD 的 `Cost` 会停在 8 ms 附近（见 §12.4）。菜单的 Priority 页签已经把这件事写给用户了。

### 9.5 Fentbot / Pilot 是分片规划器

| 常量 | 值 | 含义 |
| :--- | ---: | :--- |
| `PLANNER_STEPS_PER_TICK` | 600 | 每个渲染 tick 内，两个规划器最多推进的**仿真 tick** 总数（跨候选共享一个预算，`StepsLeft` 递减） |
| `NAV_WORK_PER_TICK` | 20000 | 每帧网格构建（分类 / 浅冻标记 / 洪水 / 梯度）最多处理的瓦片数 |
| `SEARCH_COOLDOWN_TICKS` | 10 | Fentbot 一轮结束后的休眠 tick 数 |
| `PLAN_GUARD_TICKS` | 6 | 执行计划前的闭环护栏推演深度 |

* 每 tick 的固定额外开销：护栏推演 —— Fentbot 1 次（6 tick，必要时再加 1 次玩家输入）、Pilot 2 次（计划 + 玩家输入，同窗口比较）；没有可用计划时再加一次保底搜索 —— Fentbot 12 个候选（`Guard = clamp(Length,6,10)`）、Pilot 3 个候选（6 tick）。
* Fentbot 的量级（Low 档，`actions = 88`、`horizon = 10000`、`dosage = 88`）：单代 88 × 10000 = 880,000 仿真 tick ÷ 600 ≈ 1467 个渲染 tick ≈ **29 秒**一代；一轮 88 代是小时级。候选一碰到危险就提前结束，所以实际远小于这个上界，但“开箱即用地秒出结果”不是这个模块的行为。Mid/Max 档更大（Max：1000 × 10000 = 10,000,000 tick/代）。计划是**随时暂存**的：第一个候选算完（`Fitness > -1e30`）就会有一条可执行的暂存计划，只要当前基因组走到末尾就整体换上去。
* Pilot 的量级（默认 `population = 2048`、`depth = 17`）：一代 2048 × 17 = 34,816 仿真 tick ÷ 600 ≈ 58 个渲染 tick ≈ **1.2 秒**一代。快照失效时**不会**重建种群：形状没变就把同一批后代的 fitness 清空、在新快照上重新评估（同样约 1.2 秒），只有种群为空或形状（`population` / `depth`）变化时才调 `SeedPopulation()` 重建 `Count × Depth` 个输入（`m_IndividualCount` / `m_IndividualDepth` 与参数不一致或缓冲区尺寸不匹配时）。计划在新世代完成且 `bc_avoid_pilot_sequence`（默认 5）tick 的节奏到达时才换。
* 两个规划器**自己不开线程**：它们的推演全部在 `GetAction` 里按 `PLANNER_STEPS_PER_TICK` 分片推进。全模块唯一的 `std::async` 在 Blatant 的候选环里（§9.2）。

### 9.6 HUD 与菜单的 Cost 读数

* 采样点：`ApplyInput` 里 `StartTime` 取在 `GetAction` 之前，`CostMs = (time_get()-StartTime) × 1000 / time_freq()`。它**只包含这一次决策**（导航器构建 + 播种 + 切片搜索 + 护栏/保底推演），不含渲染、组包、其他组件。
* 只在决策 tick 更新：同一 tick 的重复 `ApplyInput` 会保留上一个值。
* 显示精度：HUD 与菜单 `%.2f ms`，控制台 `avoid_status` 为 `%.3f ms`。
* 它不是一个平均值，也没有历史；要平均就自己把 `avoid_status` 的 `cost` 除以 `decisions`，或用外部 profiler。
* 网络开销不再是“启用就 50 Hz”：只有接管 tick（`INPUT_DRIVEN`）和松手后的交接 tick（`INPUT_YIELDED`）才会额外生成数据包（§3.1、§3.3）。

---

## 10. 验收清单

前置（每次都做）：

1. 进入在线服务器（`Client()->State() == STATE_ONLINE`），操控一个真实存在的 tee；状态 HUD 由 HUD 编辑器或菜单 “Visuals → Status HUD” 打开。
2. 记下操作前的 `avoid_status` 输出，操作后再打一次，比较 `decisions` / `overrides` / `nsif` 三个计数器。
3. 所有验收都可以在 `avoid_reset` 之后重做一遍，计数器归零不影响已选代理。

规格 §14 的 6 条验收项与本文的对应关系：

| 规格 §14 | 验收主题 | 本文小节 |
| :---: | :--- | :--- |
| 1 | 10-Tick 基线探针：安全状态 0 唤醒、满帧、按键 100% 透传 | §10.6 |
| 2 | Basic 边缘收缩：贴到冻结块前 1 个 Tee 宽度时方向被改掉、绝不接触冻结 | §10.1 |
| 3 | Legit 26-Tick 二次增益仲裁与拟人防抖：`Gain >= 1` 才允许覆盖 | §10.2 |
| 4 | Blatant Auto Drag：380 px 内的队友把自己拉出危险区 | §10.3 动作 F |
| 5 | Blatant NSIF：必死局里连续回放历史安全序列 | §10.3 动作 D |
| 6 | Tile Editor 与 Auto Finish：终点块扫描 + 流场铺满全图 | §10.7 的瓦片编辑器条目 |

### 10.1 Basic

* 准备：`bc_avoid_agent 0`、`bc_avoid_enabled 1`。
* 动作：在普通冻结房（或任何有连续冻结块的图）按住 D 撞向冻结块——规格 §14 第 2 条的“边缘收缩”场景。
* 必须看到：
  * 状态徽章在贴近冻结块前从 `WATCH` 变 `ASSIST`，`Plan` 变成 `brake before hazard` 或 `steer left/right before hazard`；
  * `Safe` 显示 `6 tick`（Basic 的前瞻就是 6；所有代理的 Safe 行都是 tick 数，见 §5.1/§7.1）；
  * tee 在冻结块前沿悬停、绝不冻结；松开按键后立刻恢复玩家控制（松手那一 tick 会额外发一个数据包，见 §3.3）；
  * 在安全路段行走时 `Plan: probe: player input safe`、`Override` 计数不增长（门控 5 在代理之前就放行了，Basic 根本没被调用，§3.5）。
* 反向验证：把 `bc_avoid_player_prediction` 关掉，在有两名玩家并排的窄通道里，Basic 会更容易给出 `no safer plan`（克隆世界里其他 tee 不再阻挡）。

### 10.2 Legit

* 准备：`bc_avoid_agent 1`、`bc_avoid_enabled 1`（默认 `iterations = 100`、`check_ticks = 6`、权重 170/260/160）。
* 动作 A：在安全路段正常起跳、飞行、钩索。
* 必须看到：`Plan: player input safe`，`Override` 几乎不增长，`Cost` 是零点几毫秒量级；操作手感不被改写（方向与钩索状态都保留）。
* 动作 B：朝冻结墙跳过去。
* 必须看到：接管时 reason 是 `steer to safety` / `hook to safety` / `release hook` / `hook and steer to safety` 之一，并且**后面带着仲裁增益**（形如 `steer to safety (+14)`）；`Safe` 显示 `6 tick`（把 `bc_avoid_legit_check_ticks` 调大后显示对应值）；方向与钩索在最后几个可救 tick 内被改掉。
* 动作 B2（仲裁驳回）：在冻结墙前 3 格轻微左右微调（或任何“MCTS 想动、但动一下并不会多活一帧”的处境）。
* 必须看到：**没有任何接管**，`Override` 不增长，reason 是 `post hoc gain below 1 tick`（如果它选中了候选但被仲裁驳回）或 `player input safe`——探索噪声不会写进输入（§5.3）。
* 动作 C（护栏）：`bc_avoid_legit_iterations 1000`、`bc_avoid_legit_check_ticks 50`。
* 必须看到：`Cost` 上升到 8 ms 附近并稳定（护栏生效）；如果一个根子节点都没来得及 rollout，reason 变成 `search budget reached`；把两个值调回默认后 Cost 回落。
* 动作 D（不会自己走）：在安全路段正常左右移动，或只短促点按一下方向键。
* 必须看到：`Override` 不增长，方向**不会**被改成 `-1`；即使把 `bc_avoid_legit_iterations` 调到 1000 让护栏提前命中，也不会出现“松手就自己往左走”（§5.3 的历史 bug）。

### 10.3 Blatant

* 准备：`bc_avoid_agent 2`、`bc_avoid_enabled 1`、`bc_avoid_nsif 1`（默认 `kick_in_ticks = 26`、`blatant_check_ticks = 26`）。
* 动作 A（迟滞）：保持自己的输入安全（在平地上正常走）。
* 必须看到：状态停在 `WATCH`，`Plan: player input safe`，`Safe: 26 tick`，`Override` 不增长——这就是 kick-in 迟滞。
* 动作 B（贪心 + 钩索）：从高处落进狭长冻结池。
* 必须看到：状态变 `ASSIST`，reason 出现 `hook to safety` / `release hook` / `brake before hazard` / `steer away from hazard`，`Override` 增长；开着 `bc_avoid_draw_path` 时能看到预测折线。
* 动作 C（无解情形）：找一个所有候选都不比玩家原输入活得久的处境（例如已经贴在致死/冻结面上且没有任何方向能多撑一 tick）。
* 必须看到：不接管、状态仍是 `WATCH`、玩家输入保持原样，reason 是 `player input safe`（Blatant 不再产生 `no safer plan`）——gate 是 `BestSurvival > KickSafety`（§5.2 第 9 点）。
* 动作 D（NSIF）：同一条命里先制造一次 `FoundSafe` 的接管（例如让它先躲开一次冻结），再落进无法全安全的必死局（规格 §14 第 5 条）。
* 必须看到：状态徽章变红 `NSIF`，`Plan: NSIF: replay saved safe input`，计数器的 `nsif` 增长；`Safe` 显示的是**重新推演出来的真实存活 tick**（不是伪造的 9999）。
  注意：缓存里只有一步时它会被**保留**而不是消费掉，所以 NSIF 可以连续亮多个 tick，输入连续、不抽搐（§12.20）；`m_SavedSafeSequence` 会在重置、地图加载和 kick-in 让路分支里被清空，必须在**同一条命**里先攒到一次安全序列。另外，重放下来的那一步只有在比玩家当前输入活得更久时才会真的接管（§5.2 第 6/7 点）。
* 动作 E（瞄准层）：`bc_avoid_aimbot 1`、`bc_avoid_draw_aimbot 1`、`bc_avoid_auto_aim 1`。
* 必须看到：橙色瞄准标记出现并指向一个能活过整个 check 窗口的方向；`Plan` 里出现 `hook the safe aim` / `aim clear of the hazard`。
* 动作 E2（两条 FOV 扫描都在跑）：把 `bc_avoid_aimbot_segments` 从 5 调到 32，`Cost` 明显上升——这次上升是**两份**扫描的叠加（阶段 2 的 `(Segments + 1) × 21` 帧，加瞄准层 `Segments` 条可钩射线 × check 窗口，§9.1）。同时逐个开关 `bc_avoid_auto_aim` 与 `bc_avoid_aim_assist`，观察候选与瞄准标记的变化：前者把“探针存活最久”的射线放进候选，后者把“整窗安全且离准星最近”的射线放进候选，两者可以同时入选、重复方向会被 `dot > 0.9999f` 去重（§5.2 第 5 点）。
* 动作 F（Auto Drag，规格 §14 第 4 条）：`bc_avoid_auto_drag 1`，自身向深渊坠落，**380 px** 内的上方有一名安全停留在安全地面的队友。
* 必须看到：reason `auto drag a teammate`、橙色瞄准标记指向队友（`bc_avoid_draw_aimbot 1`），并且**不需要** `bc_avoid_aimbot`；远端（> 380 px）或几乎重合（< 16 px）的队友都不在射程内。
* 反向验证：把 `bc_avoid_player_prediction` 关掉后，推演里的队友 core 条目被清空、钩中也不产生拉力，这个救援动作通常测不出 9999 而不再出现（§12.6/§12.15）。
* 动作 G（解冻块逃逸）：`bc_avoid_blatant_unfreeze 1`，把 `bc_avoid_blatant_unfreeze_ticks`（同时当作 BFS 半径）调到能覆盖最近解冻块的值，站在冻结前。
* 必须看到：reason `escape to an unfreeze tile`、瞄准标记指向解冻块中心；把 `bc_avoid_blatant_unfreeze` 关掉后这条路径消失（普通候选里踩解冻块仍然算危险）。
* 反向验证 1：在 Aimbot 页签关掉 `bc_avoid_aimbot`，回到 Settings 页签会看到琥珀色提示——此时 `bc_avoid_track_point` / `bc_avoid_safe_aim_tracking` 不再产生任何候选，Auto Drag 只保留救援动作本身。
* 反向验证 2：只在“瞄准层算出了候选、但最终没有被采纳”的 tick 观察 `bc_avoid_draw_aimbot`：标记不会亮，因为 `m_AimTarget` 只在接管分支里发布（§5.2 第 9 点）。
* 反向验证 3（阶段 2）：`bc_avoid_track_point 1` 或 `bc_avoid_aimbot 1` 打开时，危险 tick 的准星会先被扇区扫描改写；如果代理本身没有接管，reason 是 `sector scan locked the crosshair`，`bc_avoid_draw_aimbot 1` 下能看到那个被锁定的方向（§3.5）。

### 10.4 Fentbot

* 准备：`bc_avoid_agent 3`、`bc_avoid_enabled 1`；为了快速看到结果，建议先 `bc_avoid_fent_advanced 1`、`bc_avoid_fent_tweaker_actions 50`、`bc_avoid_fent_ticks 1000`、`bc_avoid_fent_tweaker_dosage 1`。
* 动作：进入一张有冻结段和 finish/unfreeze 目标的图，先站着不动观察。
* 必须看到：
  * 状态栏网格指示从 `grid: building` 变 `grid: ready`（导航器分片构建完成）；
  * 第一个候选算完之前，reason 是 `survival fallback while searching`（保底贪心）或 `player input safe`；
  * 第一个候选算完之后就变成 `fentbot plan`（随时暂存），`Safe: 6 tick`（护栏窗口），路径覆盖层沿流场画出一条折线；
  * 更好的基因组会在当前基因组走到末尾时整体换上（不会出现“旧尾巴接新头”的重复动作）；`avoid_status` 的第二行 `last plan` 与 HUD 一致。
* 预期时间：默认档位是分钟到小时级（§9.5），不要用默认档做“秒级”验收；要看效果就把 `dosage` 设成 1、`actions` 设成 50。
* 反向验证：`bc_avoid_fent_light_tile 1` + `bc_avoid_fent_light_tile_radius 3` 后，浅冻（unfreeze 附近的 freeze）段会进入规划路线（改这两个值会重建网格：`grid` 会短暂回到 `building`）；把 radius 设成 0，这些瓦片重新变回 blocked。
* 再反向验证：站在浅冻瓦片上不动（或让速度低于 1.5 px/tick）时，该瓦片重新算作危险（§4.3），`Plan` 不会再显示“停在冻结里”的方案。

### 10.5 Pilot

* 准备：`bc_avoid_agent 4`、`bc_avoid_enabled 1`（默认 `population = 2048`、`depth = 17`、`top_k = 10`、`sequence = 5`）。
* 动作 A（自主）：`bc_avoid_pilot_mode 0`，进入有 finish 或 unfreeze 瓦片的图。
* 必须看到：网格先 `building` 后 `ready`；第一代结束前 reason 是 `survival fallback while evolving`，之后是 `pilot plan`；tee 自己朝流场方向走，`Safe: 6 tick`（护栏窗口），`bc_avoid_draw_aimbot` 打开时能看到模式目标点。
* 动作 B（跟随准星）：`bc_avoid_pilot_mode 1`，移动鼠标。
* 必须看到：瞄准标记跟着准星走，tee 朝准星方向移动，路径覆盖层沿流场重画。
* 动作 C（跟随玩家）：`bc_avoid_pilot_mode 2`，附近有另一名玩家。
* 必须看到：瞄准标记吸附到最近的那名玩家，tee 朝它移动。
* 动作 D（种群延续）：保持 `population` / `depth` 不变观察多代。
* 必须看到：种群在换代之间保持延续（不会每代都被重新随机播种）；把 `bc_avoid_pilot_population` 改一个值，形状变化会触发一次重新播种（表现为搜索质量短暂回退后再爬升）。
* 反向验证：`bc_avoid_pilot_sequence 1` 后每个新世代一完成就会换计划（`Plan` 刷新更频繁）；设成 20 后当前计划会被执行更久，两次世代之间到达计划末尾时保持最后一个输入。

### 10.6 前置流水线（规格 §14 第 1 条）

* 准备：`bc_avoid_enabled 1`，任选代理（Basic / Legit / Blatant 都会被探针拦下；Fentbot / Pilot 不过门控 5，见下）。
* 动作：在宽阔平地上持续奔跑、起跳，完全远离冻结区。
* 必须看到：
  * `Plan` 稳定显示 `probe: player input safe`，`Override` 计数**完全不增长**，`Safe` 显示 `10 tick`（探针窗口）；
  * `Cost` 远低于 1 ms 量级、且不随代理的参数变化（安全 tick 上代理一次都没被调用，§9.1）；
  * 帧率与关掉避障时无差别（`cl_showfps 1` 对照）；按键 100% 原样透传，没有机械抖动。
* 反向验证 A：朝冻结区走，`Plan` 应在探针失手的那一 tick 换成代理的 reason（`brake before hazard` 等），`Cost` 同时抬升。
* 反向验证 B：把 `bc_avoid_player_prediction` 打开/关闭，观察“两名玩家并排的窄通道”里探针的存活帧数变化（探针用的就是同一套 `SSimFlags`，§3.5）。
* 反向验证 C（扇区扫描）：`bc_avoid_track_point 1` 或 `bc_avoid_aimbot 1` 打开时，探针失手之后会先扫 `bc_avoid_aimbot_segments + 1` 条射线 × 21 帧；`Cost` 在危险 tick 上会随 `bc_avoid_aimbot_segments` 上升，`bc_avoid_safe_aim_tracking` 打开时只有整窗安全的射线才会被接受（§3.5）。
* Fentbot / Pilot 的对照：切到这两个代理后，安全时 `Plan` 不会出现 `probe: player input safe`（它们的 reason 是 `player input safe` / `fentbot plan` / `pilot plan`），因为它们不过门控 5、每 tick 仍在推进自己的分片搜索（§12.18）。

### 10.7 共用项

* AFK 保护：`bc_avoid_afk_protection 1`、`bc_avoid_afk_time 5`，松开所有按键 5 秒以上 → 状态变 `AFK`，控制台打印 `Avoid: AFK protection paused the bot`，**总开关保持打开**；触发的那一 tick 就已经不再接管，并且会走一次交接（§3.3）；随便按一下方向键立刻恢复接管（§12.19）。
* 特效范围：`bc_avoid_draw_track_point` / `bc_avoid_draw_aimbot` 只在 Blatant 或 Pilot 有对应数据时才画出东西；`bc_avoid_draw_path` 对五个代理都有效。
* 代理切换：切到 Fentbot/Pilot 时新旧代理的状态都被重置（网格重新构建、种群清空），HUD 标题与菜单页签同步变化。
* 参数恢复：任何代理（**包括 Basic**）点 “Defaults” → 控制台打印恢复提示，滑条/复选框回到头文件里的默认值。
* HUD 背景：在 HUD 编辑器里关掉 Avoid 模块的 background，面板底色消失但文字仍在（§7.6）。
* 死亡格不再误判：站在地图终点块（34）或解冻块（11）旁边、不接触任何冻结面时，Basic / Fentbot / Pilot 不应给出 `no safer plan`，也不应无故接管。v4.0 之前 `& TILE_DEATH` 会把这两种瓦片当成死亡格（§4.3）。
* 瓦片编辑器（规格 §14 第 6 条）：Fentbot 或 Pilot 的 “Tile Editor” 页签 → `Auto finish` 先把全图 `TILE_FINISH = 34`（Game 层与 Front 层）标成目标，`Recalculate` 再 bump 一次 revision 强制重算；页签底部的 `Tunnel tiles / Finish tiles` 计数实时增长。`Enable editor` 打勾后还可以在世界里左键画、右键擦。画完看状态栏：`grid` 会短暂回到 `building` 再变 `ready`（Revision 变化触发两个规划器重建）。`Auto finish` 打印标记到的终点块数；`Auto tunnels` 用已加载的 TAS 回放生成通道（没有回放时打印 `no TAS replay loaded`）；`Recalculate` 只 bump revision，用于手动强制重算；`Clear all` 把两个计数清零。重建完成后 `bc_avoid_draw_path` 会沿 `CNavigator` 的梯度流场画出指向终点的折线（流场由多源 BFS 从目标瓦片铺满全图，§5.6），同时能看到编辑过的半透明方块——**即使总开关是关的**（瓦片叠加层只要求 `bc_avoid_draw_path`）。
* 隧道限制与目标：画一条把角色和解冻块隔开的隧道，观察 Fentbot/Pilot 只在隧道内规划；再把一个 Finish 画在隧道外的空地上，那个格子仍然可达（`InsideTunnel || EditorGoal`，§5.6）。

### 10.8 控制台命令与推荐绑定

| 命令 | 注册处 | 行为 |
| :--- | :--- | :--- |
| `avoid_toggle` | `avoid.cpp:77` | 等价于 `bc_avoid_enabled` 取反；打印 `Avoid: enabled` / `Avoid: disabled` |
| `avoid_status` | `avoid.cpp:78` | 打印一行状态（`[avoid] state … \| agent … \| enabled … \| safe N ticks \| cost X.XXX ms \| decisions N \| overrides N \| nsif N`）+ 一行 `[avoid] last plan: <reason>` |
| `avoid_reset` | `avoid.cpp:79` | 清零 decisions / overrides / nsif 三个计数器，打印 `Avoid: counters reset` |

推荐绑定（与菜单里显示的提示一致）：

```
bind X toggle bc_avoid_enabled 1 0
```

`bind` 走的是 CVar 直接切换，和菜单按钮、`avoid_toggle` 是同一条状态路径（`CAvoid::IsEnabled()` 只读 `g_Config.m_BcAvoidEnabled`）。

---

## 11. 回归

运行方式（脚本自己 `cd` 到仓库根，`set -e`，任何一步失败即中止）：

```bash
./scripts/avoid_selfcheck.sh
```

前置：`build/build.ninja` 必须已经配置好，否则第 1 步直接报 `error: build/ is not configured; run cmake first` 并以非零码退出。
全部通过时最后一行是 `all checks passed`。脚本共 8 个块（编号 1–7，其中 HUD 检查是 6b）：

| 块 | 标题 | 断言的内容 |
| :--- | :--- | :--- |
| 1 | `[1/7] building` | 构建 `ninja -C build DDNet`。编译不过，后面全部无意义 |
| 2 | `[2/7] bc_avoid_* parameter contract (name, default, min, max)` | 用一个硬编码的 **56 元组**契约表与头文件里的 `MACRO_CONFIG_INT(…, bc_avoid_*, …)` 做**集合相等**比较；多一个、少一个、默认值/最小值/最大值任意一项不同都会打印差异并失败。通过时打印 `ok: N parameters, every default/min/max matches the reference` |
| 3 | `[3/7] every parameter is actually wired` | (a) 每个声明的参数要么名字、要么它的 `m_BcAvoidXxx` 成员必须出现在除头文件以外的 `src/**/*.cpp\|h` 里（“死参数”即失败）；(b) 每个参数必须能在 `menus_avoid.cpp` 里找到它的成员名，例外是 `bc_avoid_enabled` / `bc_avoid_agent`，它们必须分别出现 `Avoid.SetEnabled(` / `Avoid.SetAgent(`；(c) 恰好 6 张 defaults 表（每个代理一张 + 共用一张），每张非空、无重复、所有名字都已声明，并打印总条目数（当前 **56**） |
| 4 | `[4/7] localization follows the client language` | 从 `avoid.cpp`、`avoid_engine.cpp`、`menus_avoid.cpp`、`menus_tas.cpp`、`hud_layout.cpp` 收集全部唯一的 `BcLocalize("…")` 字面量并断言数量 > 120（当前 **195**）；用与 `CLocalizationDatabase::Load` 同构的解析器读两个语言文件（上下文行、`== ` 行格式错误都会失败），然后断言每个 key 都以 `BestClient` 为上下文存在 |
| 5 | `[5/7] no placeholder copy left in the module` + UI 排版契约 | (a) 脚本里硬编码的禁止词表（检索字面量为 `not implemented`、`Not implemented`、`尚未实现`、`Planned`、`planned`、`is missing`、`arrives with the agent`、`reserved for upcoming`）不得出现在 Avoid 相关文件的任何 `BcLocalize` 字面量里、不得出现在 `menus_avoid.cpp` 正文里、也不得作为语言文件里这些 key 的译文出现；注释不受限。(b) 排版契约：菜单里必须仍有 `SLabelProperties Props;` 与 `Props.m_MaxWidth = Rect.w;`（否则说明文字会被压到 5 px 下限）、必须仍调用 `AvoidHintBottom(` 与 `TextBoundingBox(`（否则面板不再为换行后的文案预留高度）；`AVOID_TEXT_VALUE` 与 `SIMULATION_SAFE_CONSTANT` 必须**不**出现在菜单里（前者已删除，后者对应的分支永远不会运行） |
| 6 | `[6/7] engine contract` | 对源码做字符串断言，分五组：(a) 模拟器：`SimulateCandidate` + `CopyWorldClean` + `SIMULATION_SAFE_CONSTANT = 9999`；**危险判定**：存在 `== TILE_DEATH` 且**不存在** `& TILE_DEATH`（位与会把 finish / unfreeze 当死亡格），冻结条件必须是 `pChar->m_FreezeTime > 0 \|\| pChar->m_FrozenLastTick \|\| pCore->m_DeepFrozen` 且**不得**出现 `m_Core.m_IsInFreeze \|\| pCore->m_DeepFrozen`。(b) 决策规则（现在全部在 `avoid_decision.h`）：流水线与仲裁的固定常量 `PROBE_CHECK_TICKS = 10`、`PROBE_SAFE_TICKS = 7`、`ProbeIsSafe(`、`SECTOR_SCAN_TICKS = 21`、`LEGIT_ARBITRATION_TICKS = 26`、`AUTO_DRAG_MAX_DIST = 380.0f` / `AUTO_DRAG_MIN_DIST = 16.0f`、`IsBlacklistedGametype(`；`WEIGHT_SCALE = 0.01f`、`std::abs(DirDiff - 2.0f)`、`std::abs(HookDiff - 1.0f)`、`s_aDirs[3] = {-1, 0, 1}`、`s_aHooks[2] = {0, 1}`、`Exploitation + Heuristic`、`if(Child.m_Visits == 0)`；引擎里必须调用 `SelectLegitRootChild(`，且 `MostVisits` / `m_Visits > MostVisits` **不得**再出现；探针必须把 ≥7 帧折算成安全哨兵（`ProbeIsSafe(Survival) ? SIMULATION_SAFE_CONSTANT : Survival`），引擎同时导出 `RunLightweightProbe` / `RunSectorScan`，组件里必须按 `probe → sector scan → agent` 的顺序调用（脚本用 `index()` 比较三处的先后），五道门控的枚举与 `IsGamemodeBlacklisted()` / `IsPlayerInactive()` / `IsCharacterFrozen()` / `IsAfk()` / `UpdateAfkTimer(` 必须存在、且 `if(IsGamemodeBlacklisted()) < if(IsPlayerInactive()) < if(IsCharacterFrozen()) < if(IsAfk())`；门控 2 要含 `GAMESTATEFLAG_PAUSED` 与 `TEAM_SPECTATORS`，门控 3 要含 `m_FreezeTime > 0 || pChar->m_FrozenLastTick`，组件里**不得**再出现 `SetEnabled(false)`（AFK 不再关总开关）。26-Tick 仲裁必须仍然调用 `ArbitrationGain(CandidateSurvival, HumanSurvival, LEGIT_ARBITRATION_TICKS)` 与 `ArbitrationAllowsOverride(...)`，且 `post hoc gain below 1 tick` 存在、`PlayerSurv` / `BestSurvHook0` **不得**复活。(c) 五个代理：Basic 的 `BASIC_CHECK_TICKS = 6` 与 `s_aCandidateDirs[3] = {0, -1, 1}`；Legit 的 `3.402823466e+38`；Blatant 的八级级联顺序（脚本比较 `KickScore` → `// --- 2. Preemptive hook release` → `// --- 3. Auto drag` → `// --- 4. Emergency air jump` → `// --- 5. Emergency upper hemisphere radar` → `// --- 6. Unfreeze escape` → `// --- 7. Greedy search` → `// --- 8. NSIF` 八处的先后）、`Set.m_KickInTicks`、`m_SavedSafeSequence`、`Set.m_AutoDrag` + `auto drag a teammate`、`FindNearestUnfreezeTile` + `escape to an unfreeze tile`、`Set.m_Aimbot` + `BestSurvivalAim` + `BestNearAim`、`BuildBlatantCandidates(Ctx.m_Input, Set.m_BlatantDirection, Set.m_BlatantHook, CanAirJump, &vActions)`、`const int JumpCount = CanAirJump ? 2 : 1;`、`s_aJumps[2] = {0, 1}`、并发推演 `SimulateBranchesParallel(` + `std::async(std::launch::async`、NSIF 的保留式条件 `(Set.m_Nsif || Set.m_TrackPoint) && !m_SavedSafeSequence.empty()`；v5.1 的三个救援机制（`bool CanUseAirJump(` / `bool CheckPreemptiveHookRelease(` / `bool CheckHeadroomClearance(` / `bool TryEmergencyAirJump(` / `bool TryEmergencyWallCeilingHook(` 必须同时出现在引擎头与引擎实现里，`PreemptiveHookReleaseWins(` / `HeadroomAllowsAirJump(` + `SHeadroomProbe` / `RadarTargetIsHookable(` + `EmergencyRadarDirs()` + `EMERGENCY_RADAR_RAYS = 5` / `HOOK_MAX_DISTANCE = 380.0f` / `AIR_JUMP_HEADROOM = 48.0f` / `AIR_JUMP_MIN_CLEARANCE = 32.0f` / `AIR_JUMP_MIN_GAIN_TICKS = 8` / `RADAR_MIN_SURVIVAL_TICKS = 10` 必须在决策头里，`Tile == TILE_DEATH || IsFreezingTile(Tile)` 必须存在而按位与的写法不得出现，Legit 的 `BuildLegitCandidates(pCurr->m_Action, ..., CanAirJump, &vCandidates)` 与“抢断在仲裁之前”的先后也必须成立；Fentbot 的 `FENT_FLOW_WEIGHT = 1750.0f` 与档位值 88/160/1000/300；Pilot 的三个参数名；每个代理都必须有 `AvoidInput <类名>::GetAction(` 且在组件里 `new Avoid::<类名>(`。(d) 瓦片编辑器：`class CTileEditor`、`CNavigator::Rebuild(..., const CTileEditor *pEditor)` 的完整签名、`m_pEditor->IsTunnel(X, Y)` 与 `IsFinish(X, Y)`、`EditorRevision()` 与 `m_Nav.EditorRevision() != EditorRevision`（编辑必须让流场失效）、组件里有 `Avoid::CTileEditor m_TileEditor` 与 `Ctx.m_pTileEditor = &m_TileEditor;`、`AutoFinish(` / `AutoTunnels(` / `Interact(` / `ClearAll` 被实现且被组件调用、三个自复位参数在组件里确实被消费。(e) 输入钩子：`avoid.h` 里有 `EInputResult ApplyInput(`，组件里有 `*pInput = m_LastOverride;` 与 `FinishInput(`，组件里**不出现** `m_Controls.m_aInputData`，`WantsEveryTickInput` 在组件与 `controls.cpp` 里都不存在，`gameclient.cpp` 里有 `m_Avoid.ApplyInput(&Input) != CAvoid::INPUT_IDLE` 与 TAS 录制那行 |
| 6b | `[6b/7] HUD module wiring and render order` | `hud_layout.h` 有 `MODULE_AVOID,`；`hud_layout.cpp` 有 `"avoid",`、`"Avoid",`、`case MODULE_AVOID:`；`hud_editor.cpp` 有 `MODULE_AVOID` 与 `m_Avoid.RenderPreview()`；渲染组件顺序必须是 `m_MapLayersForeground < m_Hud < m_Tas < m_Avoid`（打印 `ok: foreground < Hud < Tas < Avoid`） |
| 7 | `[7/7] testrunner test suite` | `ninja -C build testrunner` 后运行 `./build/testrunner`，即整个引擎/游戏 gtest 套件；其中 `src/test/avoid_decision_test.cpp` 贡献 23 条 Avoid 决策用例（见下） |

第 7 步现在**有** Avoid 专属用例：`src/test/avoid_decision_test.cpp` 的 23 条 `TEST(AvoidDecision, …)`，覆盖 Legit 启发式的三个权重项与“玩家自己的输入永远排最高”、`SelectLegitRootChild` 的五种情形（安全时保留玩家输入、玩家在移动时保留、玩家输入会死时接管、跳过未访问子节点、整环未访问时返回 −1）、`BuildLegitCandidates` 的顺序与两个开关、`BuildBlatantCandidates` 的 12 分支笛卡尔积与三个开关、两个候选集的瞄准归一化、`BuildApproachInput` 的走向与瞄准，v5.0 的四条（玩法黑名单的整串匹配、探针阈值 `ProbeIsSafe(6)` 为假 / `ProbeIsSafe(7)` 为真、扇区扫描角、26-Tick 增益仲裁），以及 v5.1 新增的六条：`BuildBlatantCandidates` 的 12 分支顺序、跳跃维只在有二段跳时打开且关闭时保留玩家的跳跃键、`BuildLegitCandidates` 的空中二段跳维与“没有二段跳时继承而不是清零”、提前松勾判定（含 9999 折算与 `Keep < CheckTicks` 边界）、雷达五条射线的方向/朝上性与锚点过滤（含解冻块与实心块必须被接受这条反位掩码回归）、净空规则（32 px 边界、冻结/致死顶棚与正上方瓦片）与两个阈值常量。仓库根目录残留的一批 `CAvoidSimulatorTest.*.tmp` 空目录是 v2.x 时期 gtest 留下的临时目录，**不是**回归的一部分。整机行为回归仍然靠第 2–6b 步的契约断言 + §10 的实机验收。

改动前的自查顺序建议：`avoid_selfcheck.sh` → 按 §10 手动过一遍受影响的代理 → 若改了参数集，确认第 2 步的契约表也同步更新（它和头文件必须同时改，否则脚本会报 `parameter set drift`）→ 若改了决策规则，把公式改动落到 `avoid_decision.h` 的纯函数里并在 `avoid_decision_test.cpp` 补一条用例（自检第 6 步只能拦“整块逻辑被换掉”，拦不住公式写错）。

---

## 12. 已知限制与有意偏离

以下每条都是当前代码的事实，不是待办事项。其中多数是**有意偏离 spec 字面**的设计（或者是 spec 没有描述、本实现必须自己定的部分），改动前先确认你不是在“修好”一个刻意为之的行为。

1. **危险判定按瓦片编号比较，不是位掩码；顺序也不同。** 死亡排在冻结之前，并且在角色位置的 5 个点（中心 + `GetProximityRadius()/3` 的四个角）上同时查 `GetCollisionAt` 与 `GetFrontCollisionAt`——同一格一层是 freeze、另一层是 death 时，浅冻豁免永远盖不住它；角点几何与引擎自己的死亡探测一致（引擎也是两层都查）。比较必须用 `== TILE_DEATH`：`TILE_DEATH` 是编号 2，`& 2` 会命中 3（nohook）、6、7、10、11（unfreeze）、14、15 与 34（finish），把终点块与解冻块变成“死亡格”——v4.0 之前 Basic / Fentbot / Pilot 的假接管与假 `no safer plan` 就是这么来的。冻结判定严格取 spec 的三个标志（`m_FreezeTime` / `m_FrozenLastTick` / `m_DeepFrozen`），**刻意不含** `m_IsInFreeze` / `m_LiveFrozen`（它们对死亡格与已深冻的 tee 同样为真）。浅冻豁免额外要求角色**仍在移动**（`length(m_Vel) > LIGHT_FREEZE_MIN_SPEED = 1.5f`）与 `!m_DeepFrozen`，否则停在浅冻里也会被判成安全、搜索会把“停在冻结里”当成满分手牌；导航器也拒绝把死亡瓦片提升成 `NAV_LIGHT`。传送判定覆盖 `IsTeleport` / `IsEvilTeleport` / `IsCheckTeleport` / `IsCheckEvilTeleport` / `IsTeleCheckpoint`，而 spec 只点名了 checkpoint getter。全部是“宁可多判一次危险”。
2. **克隆用 `CopyWorldClean` 而不是 `CopyWorld`，这是故意的。** `CopyWorld` 会把副本挂进实时预测世界的父子链（`gameworld.cpp`），而模拟器一个 tick 里会有多个克隆（Fentbot 的快照 + 会话世界），互相标记失效会破坏客户端自己的 `m_PredictedWorld`。详见 §4.1。
3. **`CSimSession` 持有指向 `CNavigator` 内部缓冲区的指针（流场 + 网格），这是设计约束。** `m_SessionFlow.m_pDir` 指向 `Flow()` 的 `data()`，`SSimFlags::m_pNav` 指向导航器本体；所有重建路径（地图尺寸变化、浅冻设置变化、**瓦片编辑器 Revision 变化**、计划过期重来）都必须先 `m_Session.Abort()`：Fentbot 的“网格与流场”段与计划过期分支、Pilot 的“导航网格”段。改这一带代码时先读 §4.4 的规则。
4. **Legit 有一个 spec 没有的 8 ms 每 tick 墙钟护栏（`LEGIT_DEADLINE_MS`）。** 参考实现在 MCTS 预算被调得离谱时会掉帧；本实现选择让出并返回当前最优。代价是 `bc_avoid_legit_iterations` 调到很大时质量被护栏封顶：`1000` 次迭代 + `check_ticks 50` 会稳定命中，`Cost` 停在 8 ms 附近；只有 `DeadlineHit` **且一个根子节点都没来得及 rollout**（`SelectLegitRootChild` 返回 −1）时 reason 才是 `search budget reached`；只要访问过任何一个子节点，就照常走最终决策。**护栏命中不等于“随便选一个”**：最终决策仍走 spec §8.3 的规则，未访问的子节点被跳过，一个都没访问到就不接管（§5.3）。
5. **Fentbot / Pilot 是实时预算内的分片规划器，不是后台异步求解器。** 搜索切片在 `GetAction` 里执行（`PLANNER_STEPS_PER_TICK = 600` 个仿真 tick/帧），它们自己不开线程（全模块唯一的 `std::async` 在 Blatant 的候选环里，§9.2）；参考档位下 Fentbot 的一代是几十秒级、一轮是小时级（§9.5），一轮结束还会休眠 `SEARCH_COOLDOWN_TICKS = 10` 个 tick；Pilot 的一代约 1.2 秒，但快照每次过期都要把整代重新评估一遍（不重建种群）。寻路的可通行集合与目标集合除了地图碰撞层，还来自 v4.0 新增的瓦片编辑器（§5.6）：有 Tunnel 时隧道外的瓦片视为墙（编辑器标记的 Finish 例外），Finish 集合优先于地图的 `TILE_FINISH`，两者都没有时才退到解冻块。除了 `bc_avoid_fent_light_tile*` 与地图本身，编辑这两个集合是改变寻路行为的唯一界面手段。
6. **两条参数的工作方式与 spec 的自然读法不同，但都是刻意实现。** `bc_avoid_player_prediction` 不是“不预测其他玩家”，而是把其他 tee 从克隆世界的 core 字符表里清空（`CForwardSim::SetPredictPlayers`、`CSimSession::Begin`）——它们不再挡路，也就没有计划需要绕开；`bc_avoid_fent_light_tile` 不仅让导航器把半径内的 freeze 标成 `NAV_LIGHT`（死亡瓦片除外），还通过 `SSimFlags::m_AllowLightFreeze` + `m_pNav` 让模拟器接受这些瓦片，条件是角色仍在移动（§4.3）。
7. **NSIF 缓存里通常只有一步，而这一步会被保留。** 与 spec §7.6 一致：`FoundSafe` 时缓存被清空并只压入那一个好的动作，所以缓存通常只有一步；回放时长度为 1 的缓存**保留**、长度大于 1 的才逐步弹出。重放下来的动作要重新推演以报告真实存活 tick，并且只有在 `BestSurvival > KickSafety` 时才真的接管。验收必须先在同一条命里攒到一次 `FoundSafe`（§10.3、§12.20）。
8. **Basic 永远不能用跳跃和钩索救命。** 它只改 `m_Direction`（§5.1），所以需要跳跃或钩索才能脱险的地图它救不回来。这类图用 Legit / Blatant。
9. **“推演失败”会被当成“安全”。** `SimulatePlan` / `SimulateCandidate` 在没有世界、没有角色或 `CheckTicks <= 0` 时返回 9999，于是代理不介入；由于每个代理都会把 9999 换算成自己的窗口长度（§4.3），HUD 与状态栏此时显示的仍是一个**正常的 tick 数**，看上去和“确认安全”没有区别。这保证了异常时不会乱改输入，代价是读数本身区分不出这两种情况。定位这类情况看 `Plan` 的 reason（`no world` / `no character` / `agent unavailable`）。同一条策略也让门控 5 的探针在“推演失败”时返回 9999，于是这一 tick 直接放行——异常不会导致乱改输入。
10. **Blatant 没有墙钟护栏。** 它的单次决策成本随 `bc_avoid_aimbot_segments`、`bc_avoid_blatant_check_ticks`、`bc_avoid_kick_in_ticks` 线性增长（§9.1 的公式，最坏约百次世界克隆）；接管 gate（`BestSurvival > KickSafety`）不减少推演次数，只减少“算完不用”的情况。慢机器上先降低这几个值。
11. **Legit 与 Fentbot 使用全局 `rand()`，不具备逐位可复现性。** Legit 用 `rand()` 选展开的子节点，Fentbot 用 `rand()` 做播种与变异：同一场景两次运行会得到不同（但同类）的搜索过程。Basic / Blatant 不用随机数；Pilot 用自带 LCG（`m_Rng` 固定种子 `0x1f123bb5`），是唯一在相同输入下逐位可复现的搜索代理。
12. **Legit 的 8 ms 护栏是墙钟检查点，不是硬实时保证。** 它每 8 次迭代才看一次表（§9.4），因此单次决策仍可能略微超过 8 ms（最坏多跑 7 次迭代）；它保证的是“不会因为迭代数拉到 1000 就整帧卡住”，不是“一定在 8 ms 内结束”。
13. **Blatant 的 `m_AimTarget` 只在接管时发布，因此覆盖层可能“什么都不显示”。** 这是有意的（只显示真正被采用的瞄准，§5.2/§7.7）：当 kick-in（或刹车余量）让路、或没有任何候选比玩家原输入活得久时，即使瞄准层内部算出过候选，橙色标记也不会亮。排查“瞄准层是否在工作”要看 `bc_avoid_aimbot_segments` 对 `Cost` 的影响，而不是看标记。
14. **解冻块开关的双重语义。** 英文 KRX 文档写“避免解冻块”，spec §7.4/§7.6 写“冻结前主动逃向解冻块”。本实现在 Blatant 上两者都做：搜索期间前 `bc_avoid_blatant_unfreeze_ticks` 个 tick 内踩到解冻块算危险（文档语义），但第 6 步的逃生候选用一份 `m_AvoidUnfreeze = false` / `m_UnfreezeTicks = 0` 的临时标志推演，**豁免**这条规则（规格语义）。Legit 只保留文档语义（spec §8 没有逃生步骤）。
15. **瞄准层的门控与语义。** spec §7.6 的 Auto Drag **不受** `bc_avoid_aimbot` 控制（本实现照做：它只需要 `bc_avoid_auto_drag`；推演里队友是否实体仍由 `bc_avoid_player_prediction` 决定，不再额外加一道闸）；Track Point / Auto Aim / Aim Assist 属于“需要瞄准辅助的功能”，统一由 `bc_avoid_aimbot` 打开（对应参考的 `krx_avoid_tile_blatant_aimbot`，默认 0）。整层只**产生候选**：所有瞄准候选都走与普通候选相同的 `Consider()`，由生存搜索裁决，因此瞄准永远不会让机器人更不安全，`m_AimTarget` 也只在采纳时点亮（§5.2、§12.13）。
16. **瓦片编辑器在世界中用准星落点绘制。** `CTileEditor::Interact()` 收到的是 `m_Controls.m_aTargetPos`（世界坐标），不是屏幕像素反投影；半径内可达的图块都能画，与鼠标指针在屏幕上的像素位置无关。菜单或控制台打开时不接受点击（光标属于它们）。
17. **Fentbot 适应度里的流场项被同比缩放，并且混入了存活与距离。** spec §9.2 写的是 `Fitness = Σ(v·D) × 1750.0 − Penalty_dist`；本实现的权重常量仍然是 `FENT_FLOW_WEIGHT = 1750.0f`（自检第 6 步会校验它存在），但乘进适应度之前先除以 50（`m_SessionFlow.m_Scale = FENT_FLOW_WEIGHT / 50.0f = 35`，因为速度单位是 px/tick），结算式是 `Fitness = Survival + FlowScore − 2 × DistanceAt(m_EndPos)`：存活 tick 数以 1/tick、终点距离以 2/tick 计入。这样做的原因是避免浮点量级失衡，同时让“活得久”和“离终点近”仍能影响排序；行为上流场项依旧主导（每 tick 最高约 ±350），所以**不要**把它当成与参考逐位一致。
18. **门控 5（探针）只拦 Basic / Legit / Blatant，不拦 Fentbot / Pilot。** spec §5.5 的探针在调度器里位于“代理分派”之前，字面上对所有模式生效；但 Fentbot / Pilot 是**分片规划器**——搜索状态只在被调用的 tick 里推进（`PLANNER_STEPS_PER_TICK`），被探针拦住就永远攒不出计划，而“玩家安全”恰恰是 Pilot 自主巡航 / Fentbot 沿流场寻路的正常工作状态。代码里 `ProbeGated = AgentId != AGENT_FENTBOT && AgentId != AGENT_PILOT`；这两个模式保留自己的每帧预算与 `PLAN_GUARD_TICKS` 闭环护栏（计划不比玩家输入好就丢弃重规划）。Basic / Legit / Blatant 严格按规格被探针拦截（§3.5）。
19. **AFK 门控是每 tick 阻断，不写 `bc_avoid_enabled`。** spec §5.4 的措辞是“避障自动进入睡眠”，门控表把它列为“阻断”；本实现据此实现：`IsAfk()` 命中只让这一 tick 不接管、状态变 `STATE_AFK`，并在进入 AFK 的那一 tick 打印一次 `Avoid: AFK protection paused the bot`。旧实现直接写 `bc_avoid_enabled = 0`——而它是 `CFGFLAG_SAVE`，会落盘，用户离开键盘一次就**永久**失去机器人，必须回菜单手动打开。现在玩家一碰按键立刻恢复（`UpdateAfkTimer()` 刷新 `m_LastActiveTime`），总开关始终由玩家掌握。
20. **NSIF 是“保留式回放”，并且带一道“不许更差”的闸。** 缓存里长度大于 1 时逐步弹出（与规格一致），但长度为 1 时**保留**这一步而不是消费掉：规格的贪心搜索只压入一个动作，缓存里通常只有一步，若按“取出即 `erase`”处理，“必死局”的第二个 tick 就会没有输入可发、把手交还给玩家。另外第 9 步的接管条件对所有候选（含回放下来的旧动作）都是 `BestSurvival > KickSafety`：规格的 NSIF 分支是无条件返回，这里要求重放的动作至少比玩家当前输入多活一帧，否则不接管。回放路径由 `bc_avoid_nsif` **或** `bc_avoid_track_point` 打开（`Set.m_Nsif || Set.m_TrackPoint`）。缓存只在这三种情况下被清空：`FoundSafe`（随即写入新的那一步）、kick-in / 刹车余量的让路分支、`OnReset()` / 地图加载。
21. **删掉了两段规格里没有、而且会挡住自救的“保护性否决”。** 一段在 Legit：MCTS 之后原有的“钩索二次校验（玩家没按钩索时 `PlayerSurv > 3` 就不许自动钩）”与“方向保护（6 帧内安全就退回玩家方向）”；另一段在 Blatant：`BestSurvHook0 > 3` 的钩索否决与方向二次校验。它们都在“玩家输入在 check 窗口内必冻”这个**唯一还会调用代理的窗口**里把已经推演出来的活路否掉（`BestSurvHook0 > 3` 在这种窗口里几乎是常态，等于主动取消勾墙 / 勾队友的救援）。现在整段被规格里的机制取代：Legit 用 26-Tick 双路仲裁（同一把尺子量两边），Blatant 用“严格活得更久”的接管条件与 `Consider()` 的平局规则。自检第 6 步断言 `PlayerSurv` / `BestSurvHook0` 不得复活。
22. **Legit 展开时“没有二段跳”这一支继承父节点的跳跃键，不清零。** 规格 §8.3 的字面写法是 `Act.m_Jump = j`，而 `JumpOptions` 在没有二段跳时只有 `{0}`——照字面实现，跑动中按住空格的玩家在搜索里会**丢掉地面跳**（每个子节点的 `m_Jump` 都被清零，只有玩家自己那一次按键能起跳）。本实现改成 `CanAirJump ? Jump : Parent.m_Jump`：有二段跳时按规格给出 `{0, 1}` 两条分支，没有时保持父节点的值。注意规格 §7.2 的 Blatant 版本本来就是“`if(CanJump)` 才写 `m_Jump`”，两处写法不一致，这里按 Blatant 的语义统一（`avoid_decision.h` 的 `BuildLegitCandidates()` 里有注释说明）。
23. **Legit 的提前松勾窗口是自己的 `bc_avoid_legit_check_ticks`（默认 6），比 Blatant 的 26 帧短。** 规格 §8.3 第 4.5 步传的就是 `CheckTicks`，本实现照做。后果是“荡进黑水”这种要十几帧才发生的危险，Legit 上要等角色离水面只剩 6 帧才抢断（Blatant 默认 26 帧就会提前动手）。想让它更早触发就调大 `bc_avoid_legit_check_ticks`（同时会变慢：它也是 MCTS 每次 rollout 的长度）；这类救援的主力是 Blatant。
