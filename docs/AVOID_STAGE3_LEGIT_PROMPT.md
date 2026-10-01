# 交给下一个 AI 的提示词：实现「避障」模块的 Legit（拟真）模式

> 用法：把下面 `====` 之间的整段内容原样发给下一个 AI（或直接说“读 `docs/AVOID_STAGE3_LEGIT_PROMPT.md` 并执行”）。
>
> 前置状态：**Basic 模式已交付并验收通过（v1.1.1）**。不要重写 Basic，不要动它的验收行为。
>
> **状态（2026-10-02）：本档已交付（v1.2.0）。**
> 实现说明见 [`AVOID_TECHNICAL_DOCUMENTATION.md` 6.8](AVOID_TECHNICAL_DOCUMENTATION.md#68-legit-代理实现说明v120)，
> 游戏内验收见 [8.3b](AVOID_TECHNICAL_DOCUMENTATION.md#83b-legit-代理验收v120-起)，
> 交付报告见 [`AVOID_STAGE3_LEGIT_REPORT.md`](AVOID_STAGE3_LEGIT_REPORT.md)。
> 本任务书保留作为记录，不要再重复执行。

====

# 任务：为 BestClient 的「避障 / Avoid」模块实现 **Legit（拟真）模式** 的决策算法

## 背景（你没有上下文，先读这段）

BestClient 是 DDNet 的一个分支客户端，工作目录就是仓库根目录，当前分支 `feature/tas`。

避障模块已经完成了**三个阶段**：

* **阶段一（v1.0.x）**：界面（5 种模式的独立参数面板）、33 个 `bc_avoid_*` 配置项、
  危险感知层（`ClassifyPoint()` / `ScanThreat()` / `IsRelevantHazard()`）、输入拦截管线、
  HUD 与世界可视化、控制台命令、中俄本地化。全部完成且仍在正常工作。
* **阶段二 Basic（v1.1.0 / v1.1.1）**：**前向模拟器 `CAvoid::SimulateInput()`** 与
  **方向键制动决策引擎 `CAvoid::EvaluateBestPlan()`**。
  Basic 只用 `{-1, 0, +1}` 三个方向候选、确定性穷举、只改 `m_Direction`。
  验收已通过：走向危险被刹停、荡向危险被反向键减速、窄通道不误触发；
  感知半径真实影响介入距离；最坏情况 0.02 ms/tick（预算 1.5 ms）。

**你现在要做的是 Legit（拟真）模式**。它相对 Basic 有四个本质升级：

1. **允许使用钩子**：能按下钩子、也能**提前松开钩子**（这是 Basic 明确不做的，也是三条验收里
   唯一还没实现的那条 —— “勾向贴有黑水的墙时提前脱钩”）；
2. **允许使用跳跃**（受配置约束，且要遵守仓库红线：飞锤 / DF 场景严禁自动走动）；
3. **搜索升级为 MCTS**：`Quality` / `Randomness` / `LifeWeight` / `DirectionWeight` / `HookWeight`
   这些参数在 Basic 里是死的，在 Legit 里必须真正生效；
4. **预测其他玩家**（`bc_avoid_player_prediction`）：Basic 明确不做，Legit 必须做。

**不要从零开始**：`SimulateInput()` 是三个代理的公共地基，已经写好并且有保真度回归测试。
你的工作是**扩展候选空间 + 升级搜索 + 补上缺失的物理**，而不是重写模拟器。

## 必读材料（按顺序读，别跳过）

1. `docs/AVOID_TECHNICAL_DOCUMENTATION.md`
   - 开头「速览：下一个 AI 先读这 8 条」
   - **第 6 章**：6.1 契约、6.3 物理前向模拟两条路径、**6.4 性能预算（Legit 会吃掉整个预算，
     这是本阶段最大的风险）**、6.5 踩坑清单、**6.7 Basic 实现说明（含“已知不足”，那五条就是你
     本阶段的待办清单）**
   - 附录 B 关键代码位置速查
   - **附录 F 交付分档与路线图**（本阶段在全局里的位置）
2. `docs/avoid/legit.md` —— 参考客户端对 Legit 的定义与参数语义（权威需求来源）。
3. `docs/avoid/avoid.md` —— 5 种代理的定位；`docs/avoid/blatant.md` 用来**划清边界**：
   NSIF 在参考实现里是 Blatant 专属、`Track Point` / `Safe Aim Tracking` / `Auto Drag` /
   内置瞄准**全部不属于 Legit**，本阶段不要实现它们（界面里 Legit 也没有这些控件）。
4. `docs/AVOID_STAGE2_BASIC_PROMPT.md` —— Basic 的任务书；附录 A 是完整阶段二的原始任务书。

## 本阶段的边界（做与不做）

**做**：

* 扩展候选输入空间：方向 ±1/0、跳跃 0/1、钩子保持/按下/松开；
* 钩子释放：从“按住钩”变成“该松时松”，包括**脱钩后仍要能存活整个前瞻窗口**这一硬约束；
* MCTS（UCT）搜索，把 `Quality` 变成迭代次数、`Randomness` 变成探索常数、
   三个 Weight 变成打分项；
* 其他玩家预测：把附近玩家的位置与速度注入模拟，让“被撞进危险”也能被拦；
* `bc_avoid_unfreeze_ticks`：当最近威胁是解冻块时，用这个值替代 `CheckTicks`；
* 回填 `Plan` 的全部字段，并让 HUD/日志说清楚“为什么松钩 / 为什么跳”。

**不做（留给阶段四 Blatant / 五 Fentbot / 六 Pilot，或用户明确要求时才做）**：

* Track Point、Safe Aim Tracking、Auto Drag、内置瞄准（Segments / FOV / Auto Aim / Aim Assist）；
* NSIF 的**行为改动**（Basic 已经在做“最安全第一步”的兜底，保持现状即可）；
* Fentbot / Pilot 的算法与它们的灰色占位参数；
* 不改界面布局、HUD 模块、渲染顺序、33 个 cvar 的数量与语义。

## 硬性约束（违反 = 返工）

* **不要改** `SContext` / `SInputPlan` / `SThreat` / `SSettings` 的结构，
  不要改 `CAvoid::ApplyInput()` 的调用约定与写回方式。
* `Plan.m_Input` **必须从 `Ctx.m_Input` 复制后再改**，以保留
  `m_PlayerFlags` / `m_NextWeapon` / `m_PrevWeapon` / `m_WShots`。
* **只写 `Plan.m_Input`**。绝对不要写 `m_Controls.m_aInputData`（那里的 `m_Fire` 是边沿计数语义）。
* 危险判定**必须复用** `CAvoid::ClassifyPoint()` 与 `CAvoid::IsRelevantHazard()`，
  禁止另写探测点。
* 物理**必须使用本地图 tuning**（`Ctx.m_Core.m_Tuning`）；禁止硬编码重力/加速度/最大速度/摩擦。
* **性能**：每 tick ≤ 1.5 ms（填进 `Plan.m_CostMs`，UI 直接显示）。
  Legit 是本模块的性能分水岭：**候选数 × 前瞻深度要设上限**，
  必须有早期剪枝与“同 tick 只决策一次”的前提，禁止堆分配（用定长成员缓冲）。
* **只有玩家输入会致死时才接管**。手感生命线：宁可少管，不要多管。
* **飞锤 / DF 红线**：TAS 文档 3.11.7 严禁“未获显式授权就自动注入 `m_Direction`”。
  避障的语义是“阻止你进入危险”，不是“帮你走图”。**本阶段必须把这件事显式化**：
  1. 分身连接永远不碰（当前靠 `OnSnapInput` 的 `!Dummy` 分支保证，保持它）；
  2. 本体只在“继续按当前键会致死、且还没到 `KickInTicks`”时才改写 —— 保持这个前置条件的严格性；
  3. 请在决策前置检查里补一条**显式的身法状态判定**（飞锤 / HDF / 喷气背包 `m_Jetpack`），
     把它们作为“默认不动手”的场景处理，并让 `Plan.m_aReason` 说得出来。
     详见 [附录 F.5](AVOID_TECHNICAL_DOCUMENTATION.md#f5-与-tas自动位移红线的关系每档都必须复核)。
* 代码风格跟仓库一致（tab 缩进、clang-format 20，见 `.clang-format`）。
  新增界面字符串必须走 `BcLocalize()`，并同步
  `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt`
  （词条**不要**用 `[ ]` 包裹）。
* 不要动 Basic 的验收行为。Basic 必须继续只改 `m_Direction`、继续零干预安全 tick。

## 期望行为（验收标准）

### 三条用户验收里还欠的那一条

1. **勾向贴有黑水的墙 / 持续按住钩子被拉向危险 → 提前松开钩子。**
   ⚠️ 注意这条的**正确判据不是“钩索落点危险”**：钩子挂在安全墙面上、但角色被拖向危险是完全可能的，
   而脱钩又会保留当前速度把角色甩出去。唯一可靠的判据是**模拟**：
   对“松开钩子”的候选跑满 `CheckTicks`，只有它确实能活满（且比按住钩更好）才松。

### Basic 已有的两条必须保持不退化

2. 朝黑水走 → 在接触前被刹停（Basic 验收，Legit 下同样成立，而且应该更早更稳）。
3. 直行经过窄通道 → 不误触发。

### Legit 新增的手感要求

4. **要像是在自己玩**：允许跳动、允许钩墙；优先级滑块必须能感觉到差别
   （把 `Life Priority` 拉满 → 更保守；把 `Direction Priority` 拉满 → 更贴着玩家意图）。
5. **`Quality` 要真的有效**：1 和 200 的决策质量/耗时都要有可见差别（一个快而笨、一个慢而准）。
6. 其他玩家朝你撞过来 / 你被钩索拖向队友 → 能提前反应（`bc_avoid_player_prediction` 关闭时则不反应）。
7. 站在解冻块旁边时，介入时机由 `bc_avoid_unfreeze_ticks` 决定，而不是 `check_ticks`。

## 建议实现顺序（每步都能独立验证）

### 第 1 步：候选空间扩展 + 跳跃（先不碰钩子）

* 候选加到 `方向(3) × 跳跃(2)`，仍用确定性打分（存活帧数主导，权重项做 tie-break）。
* `Set.m_DirectionAssist` 关闭时只保留玩家方向；跳跃候选只在“玩家自己没有在飞锤/DF”时启用。
* 验收：朝黑水走更稳；在地刺/需要跳一下的场景能跳过去；**不要出现原地鬼畜跳**。

### 第 2 步：钩子能力（本阶段的核心）

* 候选再加“钩子保持 / 按下 / 松开”（受 `Set.m_HookAssist` 约束）。
* 关键实现点：`SimulateInput()` 克隆体目前 **`m_HookedPlayer` 永远是 -1**，
  也就是“钩到墙”能模拟、“钩到人”不能。钩墙只要 `m_HookState` 正确就会自然复现，
  先保证这条；钩人留给第 4 步（玩家预测）一起做。
* **测试必须覆盖**：钩住墙 → 荡向危险 → 松钩候选如果能活满就松开；
  松钩会飞进危险时 → 不许松（宁可按反向键硬减速）。
* 验收：三条用户验收全部通过。

### 第 3 步：MCTS 搜索替换穷举

* UCT：`Quality` = 迭代次数，`Randomness` = 探索常数 `c`，三个 Weight = 打分项。
* **`Randomness` 不能破坏确定性**：用 `CWorldCore::RandomOr0()` 或本地 PRNG，
  **禁止** `rand()` / 全局 RNG（仓库用 `-D_GLIBCXX_ASSERTIONS`，且预测可能一帧内跑多次）。
* 每次 rollout 用“固定候选序列”跑满前瞻，命中危险立刻剪枝。
* 验收：`Quality` 1 vs 200 的 `cost` 与生存帧数有可见差别；`Randomness` 0 vs 200 行为不同但都不崩。

### 第 4 步：其他玩家预测

* 把 `Radius`（或全图）内其他角色的位置/速度快照成一小组 `CCharacterCore`，
  作为静态/匀速障碍注入模拟；若要完整复刻 `TickDeferred()`（角色互撞 + 钩索拖拽），
  需要给克隆体一个能解析 `m_apCharacters[]` 的 `CWorldCore` —— 两条路都可以，
  但**必须在报告里写清成本与偏差**。
* 验收：队友从侧面把你撞向黑水 → 提前反应；`bc_avoid_player_prediction` 关闭后不反应。

### 第 5 步：收尾

* 解冻块专用前瞻（`bc_avoid_unfreeze_ticks`）。
* `Plan.m_aReason` 给出可读理由（“松开钩子 / 跳 / 反向减速 / NSIF”），全部走 `BcLocalize()`。
* 更新文档：附录 C 变更历史、第 6 章状态行、6.7 或新增小节记录 Legit 的实现与偏差。

## 容易踩的三个坑（前两个是 Basic 留下的现成教训）

1. **参数接线要验证**：Basic 曾经把 `bc_avoid_sensing_radius` 漏在决策之外，
   导致滑条怎么调都在同一距离被刹住。Legit 有 6 个新参数，
   **每一个都要有“改了就一定有可观察差别”的验证**（离线测试或游戏内步骤）。
2. **调参必须走地图 tuning**：模拟器已经做到，扩展候选时不要引入任何硬编码常量。
3. **判断“危险”不要用钩索落点**，要用“松钩后的模拟结果”（见验收第 1 条）。

## 自检与验收

```bash
ninja -C build DDNet
./scripts/avoid_selfcheck.sh                      # 必须全绿（含 Basic 的回归）
./build/testrunner --gtest_filter='CAvoid*.*'     # 已有的 8 个用例不许退化
```

**必须新增测试**（`src/test/`，风格参考 `avoid_sensing_radius_test.cpp` 的合成地图夹具：
在内存里造几何，数字才是精确的）：

| 用例 | 锁住的行为 |
| :--- | :--- |
| 钩墙荡向危险会松钩并存活 | 验收第 1 条 |
| 松钩会飞进危险时不许松 | 防止“为了松钩而送死” |
| `Quality` 影响决策质量/耗时 | 参数真的接线了 |
| `HookWeight` 影响“松不松钩” | 优先级滑块真的接线了 |
| 其他玩家预测开关生效 | 验收第 6 条 |
| 窄通道 / 平地直行不误触发 | 验收第 3 条不许退化 |

游戏内验证（`bc_avoid_agent 1` 即 Legit，勾选“启用避障代理”或 `avoid_toggle`）：

| 检查 | 期望 |
| :--- | :--- |
| 勾住墙、按住钩子被拖向黑水 | 贴墙前**松开钩子**，角色减速或荡开 |
| 朝黑水走 | 与 Basic 一样停住（且更早） |
| 直行过窄通道 | 不误触发 |
| `Quality` 1 → 200 | `cost` 上升、决策更稳；掉帧说明预算超了 |
| 队友从侧面撞你向黑水 | 提前反应；关掉“预测其他玩家”后不反应 |
| `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms（Quality 200 时重点看） |
| 开关代理、切回 Basic、再切回 Legit | 不崩溃、不卡顿、Basic 行为不退化 |

## 交回给我的报告格式

1. 改了哪些文件、各多少行；
2. 候选空间与搜索的确切定义（多少候选、多少迭代、rollout 多深、怎么剪枝）；
3. **每个新接线的参数**：它改了什么、你怎么证明它生效（测试名或游戏内步骤）；
4. 钩子释放用的判据，以及“松钩后能活满”的验证方式；
5. 其他玩家预测走的是哪条路（静态障碍 / 完整 `CWorldCore`），与真实物理的偏差有多大、怎么验证的；
6. 三条验收 + 四条 Legit 手感的实测结果（`avoid_status` 输出、日志或截图）；
7. `Plan.m_CostMs` 的实测区间（`Quality` 1 / 24 / 200 各一组）；
8. 已知不足，以及留给阶段四（Blatant：Track Point / Auto Drag / 内置瞄准 / NSIF 调优）的问题。

## 明确不做（留给后续阶段）

* Track Point、Safe Aim Tracking、Auto Drag、内置瞄准、Fentbot / Pilot 算法；
* 不改界面布局、参数面板、HUD 模块与渲染顺序；
* 不改 Basic 的验收行为；不改 33 个 cvar 的数量与语义。

====

---

## 备注（给人类，不用发给 AI）

* 本文件是 [`AVOID_STAGE2_BASIC_PROMPT.md`](AVOID_STAGE2_BASIC_PROMPT.md) 的下一档。
  分档与依赖关系见 [`AVOID_TECHNICAL_DOCUMENTATION.md` 附录 F](AVOID_TECHNICAL_DOCUMENTATION.md#附录-f交付分档与路线图)。
* 本阶段刻意**不引入 Track Point / 内置瞄准**：它们是 Blatant 的功能，
  提前塞进 Legit 会让“拟真”的定位失效，也会把性能预算提前吃光。
* 交回来的结果不好时，先查这四件事：
  1. `bc_avoid_player_prediction` 是不是真的接线了（Basic 的感知半径就是栽在这类问题上）；
  2. 松钩判据是不是“模拟结果”而不是“落点是否危险”；
  3. `Quality` / `Randomness` / 三个 Weight 是否真的改变行为；
  4. `cost` 是否在 Quality 200 下仍然 ≤ 1.5 ms（没填 cost = 没做性能验证）。
