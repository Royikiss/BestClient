# 交给下一个 AI 的提示词：实现「避障」模块的激进（Blatant）模式

> 用法：把下面 `====` 之间的整段内容原样发给下一个 AI（或直接说“读 `docs/AVOID_STAGE4_BLATANT_PROMPT.md` 并执行”）。
>
> 前置状态：**Basic（v1.1.0）与 Legit（v1.2.0，v1.2.1~v1.2.5 打磨）都已交付并验收通过**。
> 不要重写它们，不要动它们的验收行为；本阶段的默认目标是**只加能力，不改既有手感**。

====

# 任务：为 BestClient 的「避障 / Avoid」模块实现 **激进（Blatant）** 模式的决策算法

## 背景（你没有上下文，先读这段）

BestClient 是 DDNet 的一个分支客户端，工作目录就是仓库根目录，当前分支 `feature/tas`。

避障模块已经完成：

* **阶段一（v1.0.x）**：界面（5 种模式的独立参数面板）、33 个 `bc_avoid_*` 配置项、
  危险感知层（`ClassifyPoint()` / `IsRelevantHazard()` / `ScanThreat()`）、输入拦截管线、
  HUD 与世界可视化、控制台命令、中俄本地化。
* **阶段二 Basic（v1.1.x）**：前向模拟器 + 三方向候选制动，只改 `m_Direction`。
* **阶段三 Legit（v1.2.0~v1.2.5）**：钩子释放 + 跳跃 + UCT 搜索 + 其他玩家预测；
  感知半径半格化/椭圆化；跳跃降级为最后手段；参数灵敏度台架；"恢复默认"按钮。
  实现说明见 [`AVOID_TECHNICAL_DOCUMENTATION.md` 6.8](AVOID_TECHNICAL_DOCUMENTATION.md#68-legit-代理实现说明v120)，
  验收评分卡见 [`AVOID_LEGIT_ACCEPTANCE.md`](AVOID_LEGIT_ACCEPTANCE.md)。

**你现在要做的是 Blatant（激进）模式**。它相对 Legit 的本质升级只有一个词：**瞄准**。

Legit 与 Blatant 的分界（[`docs/avoid/blatant.md`](avoid/blatant.md)）：
Legit 是"subtle assistance，靠玩家自己的技术，允许失败"；
Blatant 是"safety above all"，允许**更早介入**（`kick_in_ticks`）、允许**改瞄准方向**去勾墙/勾人，
面向极端 Gores 图。参考实现把这几样东西全部放在 Blatant 名下：

1. **Track Point**：记住"上一次瞄向可勾住实地的方向"，在安全计算中优先保持这个瞄准方向；
2. **Safe Aim Tracking**：只有在整个 `Check Ticks` 前瞻里都保持安全时，才允许锁定跟踪方向；
   关掉它则允许"以后会变得不安全"的方向也继续跟踪（更顽固但更冒险）；
3. **Auto Drag**：预测安全时，自动瞄准并勾住**最近的玩家**（内部用瞄准逻辑选目标与边缘点）；
4. **内置瞄准（Aimbot）**：`Segments` / `FOV` / `Auto Aim`（视野内找"活得最久"的方向）/
   `Aim Assist`（视野内找"离当前准星最近且活得最久"的方向）；
5. **NSIF 调优**：找不到任何能活满前瞻的方案时，沿用**已知最安全方案的第一步**
   （参考实现明确推荐默认开启；本项目已经有 `bc_avoid_nsif`，但 Legit 只是"活得更久就兜底"）。

## 必读材料（按顺序读，别跳过）

1. `docs/AVOID_TECHNICAL_DOCUMENTATION.md`
   - 开头「速览：下一个 AI 先读这 8 条」与 [0.1/0.2](AVOID_TECHNICAL_DOCUMENTATION.md#01-本次做了什么)（现在到哪一步了）
   - **第 6 章**：6.1 契约、6.3 物理前向模拟、**6.4 性能预算（本阶段候选数会暴涨，这是最大风险）**、
     6.5 踩坑清单、6.7 Basic、**6.8 Legit（尤其 6.8.2 候选空间、6.8.3 搜索、6.8.10 与参考实现的对照）**、
     6.9 恢复默认按钮
   - **附录 B** 关键代码位置、**附录 F** 分档与红线
2. `docs/avoid/blatant.md` —— 本阶段的**权威需求来源**（参数语义逐条对齐它）。
3. `docs/avoid/legit.md` / `avoid.md` —— 用来**划清边界**：Legit 已有的能力不要重做，也不要降级。
4. `docs/AVOID_LEGIT_ACCEPTANCE.md` —— 用同一套"可测判据"的写法为本阶段立验收标准。
5. `docs/AVOID_STAGE3_LEGIT_REPORT.md` —— **第 8 节「已知不足，以及留给阶段四的问题」就是你的待办清单**。
6. `docs/AVOID_STAGE3_LEGIT_PROMPT.md` —— 上一档的任务书（了解风格与既往踩坑）。

## 本阶段的边界（做与不做）

**做**（全部围绕"瞄准"这一个新维度）：

* **瞄准候选生成**：从"玩家当前瞄准"扩展为一小组"可勾方向"（射线打到实心、非 `TILE_NOHOOK` 的墙面，
  距离 ≤ `m_Tuning.m_HookLength`），并把它接进现有候选空间（`Avoid::CPlanner::BuildRoots()`）；
* **Track Point**（`bc_avoid_track_point`）：跨 tick 记住"最后一次瞄向可勾实地的方向/落点"；
* **Safe Aim Tracking**（`bc_avoid_safe_aim_tracking`）：只有整段前瞻都安全才锁定跟踪方向；
* **Auto Drag**（`bc_avoid_auto_drag`）：预测安全时勾住最近的玩家（需要 `bc_avoid_player_prediction`）；
* **内置瞄准**（`bc_avoid_aimbot` / `aimbot_mode` / `aimbot_segments` / `aimbot_fov`）：Auto Aim 与 Aim Assist；
* **NSIF 调优**：把 `bc_avoid_nsif` 从"活得更久就兜底"升级为"沿用已知最安全的第一步"（跨 tick 记忆）；
* **可视化**：`bc_avoid_show_visuals` 打开时，在现有世界叠加层里画出 Track Point 与选中瞄点；
* 补上 `Plan.m_aReason` 的可读理由（"锁定跟踪点 / 自动拖拽 / 瞄准视野内最安全方向"等）与中俄词条；
* 为上述每一条写离线用例，并把参数灵敏度台架（[8.3c](AVOID_TECHNICAL_DOCUMENTATION.md#83c-参数灵敏度台架这个滑条到底有没有用)）扩展到 Blatant。

**不做（留给阶段五 Fentbot / 六 Pilot，或本来就永远不做）**：

* Fentbot 的 Fent Ticks / Tweaker 系列、Pilot 的种群/探索深度/Top-K/序列长度（它们的参数仍是灰色占位）；
* **不要改 Basic 与 Legit 的既有行为**（它们的验收用例必须继续全绿）；
* **不改界面布局、参数面板、HUD 模块与渲染顺序**，**不改 33 个 cvar 的数量与语义**
  （Blatant 需要的 7 个参数**早就存在**并已在面板上暴露，只是目前不影响行为）；
* **不做"帮你走图"**：永远不做目标点自动位移（TAS 文档 3.11.7 的红线，见 [F.5](AVOID_TECHNICAL_DOCUMENTATION.md#f5-与-tas自动位移红线的关系每档都必须复核)）。

## 硬性约束（违反 = 返工）

* **不要改** `SContext` / `SInputPlan` / `SThreat` / `SSettings` 的结构（阶段三只把 `SSettings::m_SensingRadius`
  从 int 改成 float，其余字段一个没动；你也一个都不许加）。**瞄准不需要新字段**：
  `CNetObj_PlayerInput` 里本来就有 `m_TargetX` / `m_TargetY`。
* `Plan.m_Input` **必须从 `Ctx.m_Input` 复制后再改**（保留 `m_PlayerFlags` / `m_NextWeapon` / `m_PrevWeapon` / `m_WShots`），
  改瞄准时只动 `m_TargetX` / `m_TargetY`；**只写 `Plan.m_Input`**，绝对不要写 `m_Controls.m_aInputData`。
* **危险判定必须复用** `Avoid::ClassifyPoint()` / `Avoid::IsRelevantHazard()` / `Avoid::ScanThreat()`，
  禁止另写探测点；**物理必须走共享模拟器** `Avoid::SSimState` / `Avoid::SimulateFixed()`，
  禁止复制第二份物理步进（[附录 F.2](AVOID_TECHNICAL_DOCUMENTATION.md#f2-每档必须留下的地基验收时会检查) 的硬性要求）；
  勾索射线用引擎自己的 `CCollision::IntersectLine*`，长度取**地图 tuning** 的 `m_HookLength`，禁止硬编码。
* **性能**：每 tick ≤ 1.5 ms（填进 `Plan.m_CostMs`）。候选数是本阶段最大的风险：
  `方向(3) × 跳跃(2) × 钩子(3)` 已经是 18，**不要再乘一个无上限的瞄准维度**。
  建议：瞄准只作为"按钩"候选的属性（或 ≤ 8 个"可勾方向"），总根数硬上限仍走 `MAX_ROOT_ACTIONS`，
  并保留 1.0 ms 墙钟护栏与逐帧剪枝；**决策路径禁止堆分配**（定长成员数组）。
* **只有玩家输入会致死时才接管**；`bc_avoid_direction_assist` / `bc_avoid_hook_assist` 关闭时对应的输入维度不允许被改
  （瞄准也要遵守这个精神：`aimbot` 关闭且 `track_point` 关闭时，**一个像素的准星都不许动**）。
* **红线**：分身连接永远不碰（`OnSnapInput` 的 `!Dummy` 分支）；喷气 / 钩住玩家 / 飞锤仍然"不动手"
  （`Avoid::ClassifyMovement()`，不要把它放开）；不改 `ApplyInput()` 的调用约定；每 tick 只决策一次。
* 代码风格跟仓库一致（tab 缩进、clang-format，见 `.clang-format`）；新增界面/日志字符串必须走 `BcLocalize()`
  并同步 `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt`（词条**不要**用 `[ ]` 包裹）。
* Blatant 的参数在 `menus_avoid.cpp` 的 `g_aBlatantPanels` 里已经排好（`Assist` / `Tuning` / `Priorities` /
  `Aimbot` / `Safety`）；**不要新增面板或控件**，把现有控件接到算法上即可。
* 别忘了 `AvoidDefaultParams()`（"恢复默认"按钮）里给 Blatant 补一张参数表——阶段四之前它刻意没有。

## 期望行为（验收标准）

### 核心：Legit 做不到、Blatant 必须做到

1. **坠落时的"自动出勾挂墙"**：玩家从平台边缘掉下去、下方是黑水时，代理能在危急关头
   **自己找一个可勾的墙面**、把准星转过去、按下钩子并在黑水外挂住——这是 Legit 明确做不到的事
   （Legit 只会在玩家自己的准星恰好对着可勾面时试试运气）。验收：合成地图里，玩家直接掉进死亡地板，
   Legit 结果为"没救"，Blatant 结果为"挂住并活满前瞻窗口"。
2. **Track Point 生效**：玩家上一次瞄到可勾墙面之后，即使把准星甩开，代理在安全计算里仍然把它算作候选；
   `bc_avoid_track_point` 关闭时该方向**不会**被加进候选。
3. **Safe Aim Tracking 生效**：同一个局面下，`safe_aim_tracking` 打开 → 只有当该方向整段前瞻都安全时才锁定；
   关闭 → 允许锁定"以后会变危险"的方向（能观察到两种结果不同）。
4. **Auto Drag 生效**：最近的队友在旁边时，预测安全才勾住并拖拽；
   `bc_avoid_auto_drag` 关闭 或 `bc_avoid_player_prediction` 关闭时不勾人。
5. **内置瞄准生效**：`aimbot_segments`（4~128）与 `aimbot_fov`（10~180°）改变被选中的瞄点；
   `aimbot_mode` 的 Auto Aim 与 Aim Assist 在同一局面可以给出**不同**的方向
   （前者只挑"活最久"，后者在"活最久"的集合里挑离玩家准星最近的）。
6. **NSIF 调优生效**：无解局面下，代理沿用**已知最安全的第一步**（跨 tick 稳定），
   `bc_avoid_nsif` 关闭时完全不接管。

### 不许退化的既有验收

7. **Basic 三条验收**（走向黑水被刹停 / 荡向黑水被减速 / 窄通道不误触发）在 Blatant 下同样成立；
8. **Legit 的全部用例**（26 个避障用例）继续全绿；Legit 的行为与参数语义不变；
9. **安全局面零干预**：玩家输入在前瞻窗口内安全时，哪怕代理"知道"往哪边更安全，也**不许**动方向、跳跃、钩子或准星；
10. **红线**：喷气 / 钩住玩家 / 飞锤不动手；分身连接永远不碰；不做目标点自动位移。

### 手感与可视化

11. 瞄准的改动要"像人"：`safe_aim_tracking` 打开时不出现"甩准星又甩回来"的抖动
    （同一段危险里瞄点方向翻转次数要能被测量，建议加一条离线抖动检测）；
12. `bc_avoid_show_visuals 1` 且对应功能开启时，世界里能看到 Track Point 与当前选中瞄点
    （颜色与现有危险色板一致，不新增面板）；
13. `bc_avoid_log 1` 的每决策日志要能读出"为什么改准星"（理由字符串走 `BcLocalize()`）。

## 建议实现顺序（每步都能独立验证）

### 第 1 步：可勾方向探测（不改行为）

* 写一个纯函数式的探测：从角色位置出发，在 `[当前瞄准 ± aimbot_fov/2]` 内按 `aimbot_segments` 采样方向，
  对每个方向做 `CCollision::IntersectLine*`（含 `TILE_NOHOOK` 判定），保留命中实心且距离 ≤ `m_HookLength` 的方向，
  按"离当前准星的角度差"排序；
* 先只做**测试**：合成地图上造一面墙，断言"可勾方向数量/最近角度"符合几何预期；
* 验收：探测本身正确、不改变任何决策（既有 26 个用例仍全绿）。

### 第 2 步：瞄准候选接进搜索 + Track Point

* `BuildRoots()` 里把"按钩"候选扩展为"按钩 × ≤8 个可勾方向"（其余候选的准星保持玩家值，
  除非 `track_point` 打开且该方向安全）；
* Track Point 记忆放在**成员**里（`OnReset()` 清空），记录最后一次"玩家自己瞄到可勾面"的方向与落点；
* 验收：坠落挂墙用例通过（第 1 条验收）；`track_point` 开关可观察。

### 第 3 步：Safe Aim Tracking + 抖动控制

* `safe_aim_tracking` 打开时，只有当"锁定该方向并保持"能活满 `CheckTicks` 才允许覆盖准星；
* 加一条抖动检测用例：同一段危险里瞄点方向翻转次数 ≤ 1；
* 验收：第 3、11 条验收。

### 第 4 步：内置瞄准（Auto Aim / Aim Assist）

* Auto Aim：在可勾方向里挑"活最久"；Aim Assist：在"活最久"里挑"离玩家准星最近"；
* `segments` / `fov` 决定采样密度与范围，必须能观察到差别（改一个就换一个瞄点）；
* 验收：第 5 条验收；性能仍在 1.5 ms 内（这是最贵的一步，先量再调）。

### 第 5 步：Auto Drag

* 目标选择：最近的、可勾的、预测安全的玩家（复用现有 `SEnvironment` 玩家快照 + 共享模拟器的互撞/拖拽）；
* 关闭 `auto_drag` 或 `player_prediction` 时不勾人；
* 验收：第 4 条验收。

### 第 6 步：NSIF 调优 + 收尾

* NSIF 跨 tick 记忆"已知最安全的第一步"；`Plan.m_UsedFallback` 与理由字符串同步；
* 补可视化（Track Point / 选中瞄点）、中俄词条、`AvoidDefaultParams()` 的 Blatant 表；
* 更新文档：附录 C 变更历史、第 6 章状态行、**新增 6.10「Blatant 实现说明」**（含与参考实现的偏差）、
  7.0 参数生效表、8.3d 游戏内验收、附录 B 代码位置、附录 D 自检脚本、附录 F 分档表（阶段四打勾）；
* 出一份 `docs/AVOID_STAGE4_BLATANT_REPORT.md`（格式见文末）。

## 容易踩的坑（前三条是前三档的现成教训）

1. **参数接线必须逐条验证**：本项目栽过两次（`sensing_radius` 只喂给 HUD；`kick_in_ticks = 0` 等于"永不介入"）。
   Blatant 有 **7 个**从未生效过的参数，**每一个都要有"改了就一定有可观察差别"的用例或游戏内步骤**，
   并把它们加进参数灵敏度台架（[8.3c](AVOID_TECHNICAL_DOCUMENTATION.md#83c-参数灵敏度台架这个滑条到底有没有用)）。
2. **候选空间爆炸**：18 × 瞄准方向是天真做法；瞄准只能挂在"按钩"上或作为受限的方向集合，
   总数必须有硬上限，并保留墙钟护栏。**先测成本再调质量**。
3. **改准星比改按键更"显眼"**：这是本阶段唯一会让玩家一眼看出"有东西在动我鼠标"的能力，
   所以 `safe_aim_tracking` 的"整段前瞻安全才锁"必须是默认行为，且抖动检测要进回归。
4. **跨 tick 状态**：Track Point / NSIF 记忆都要放成员并在 `OnReset()` 清空——
   客户端一帧内可能多次预测，绝不能用函数内 `static`。
5. **确定性**：搜索用现有本地 `CPrng`（按 tick 播种），禁止 `rand()` / 全局 RNG。
6. **别把 Legit 拽下水**：Legit 的候选生成、跳跃策略、排序规则都要原样保留；
   如果一段逻辑要两边共用，就抽成 `Avoid::` 里的共享函数，而不是改 Legit 的行为。

## 自检与验收

```bash
ninja -C build DDNet
./scripts/avoid_selfcheck.sh                      # 必须全绿（含 Basic / Legit 的既有回归）
./build/testrunner --gtest_filter='CAvoid*.*'     # 既有 26 个用例不许退化
./build/testrunner --gtest_filter='CAvoidLegitTest.ParameterSensitivitySweep'   # 台架要扩到 Blatant
```

**必须新增测试**（`src/test/`，沿用 `avoid_legit_test.cpp` 的合成地图夹具与"先断言局面有效"的写法）：

| 用例 | 锁住的行为 |
| :--- | :--- |
| 坠落时自动勾墙存活 | 验收第 1 条（Legit 做不到的那条） |
| Track Point 记住可勾方向 / 关闭后不记 | 验收第 2 条 |
| Safe Aim Tracking 开关改变锁定结果 | 验收第 3 条 |
| Auto Drag 只在安全时勾人 / 关预测不勾 | 验收第 4 条 |
| `segments` / `fov` / `aimbot_mode` 改变选中瞄点 | 验收第 5 条（参数真的接线） |
| NSIF 沿用已知最安全的第一步 | 验收第 6 条 |
| 安全局面零干预（含准星零改动） | 验收第 9 条 |
| 瞄点抖动检测（同一段危险翻转 ≤ 1） | 验收第 11 条 |
| 最坏情况成本 ≤ 1.5 ms（全候选 + 8 个预测玩家 + quality 200） | 性能预算 |

游戏内验证（`bc_avoid_agent 2` 即 Blatant，勾选“启用避障代理”或 `avoid_toggle`）：

| 检查 | 期望 |
| :--- | :--- |
| 从平台边缘走向下方黑水、故意不救 | 危急关头自己甩准星勾墙并挂住；`Plan` 行给出理由 |
| 打开/关闭 `Track point` | 关闭后不再保持上次的可勾瞄向 |
| 打开/关闭 `Safe aim tracking` | 关闭后更容易看到"锁定了一个随后会变危险的瞄向" |
| 队友在旁边、打开 `Auto drag` | 安全时勾住拖拽；关掉或关掉玩家预测则完全不勾 |
| `Aimbot` 的 `Segments` 8 → 64、`FOV` 30 → 120 | 选中瞄点会变；`cost` 随之变化 |
| 安全平台 / 窄通道直行 | 零接管、零跳跃、**准星一个像素都不动** |
| `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms（`quality 200` 时重点看） |
| 切 Basic → Legit → Blatant 来回切 | 不崩溃、不卡顿；Basic / Legit 行为不退化 |

## 交回给我的报告格式

1. 改了哪些文件、各多少行；
2. 瞄准候选的确切定义（采样方式、上限、可勾判定、怎么排序）、它如何并入搜索、总根数的硬上限；
3. **每个新接线的参数**：它改了什么、你怎么证明它生效（测试名或游戏内步骤）；
4. 勾墙/勾人的判据，以及"挂住后能活满前瞻"的验证方式；
5. Track Point / NSIF 的跨 tick 记忆放在哪、怎么清空、为什么不会破坏确定性；
6. 验收 13 条 + 抖动的实测结果（`avoid_status` 输出、日志或 `Plan` 行）；
7. `Plan.m_CostMs` 的实测区间（最坏情况：全候选 + 8 个预测玩家，quality 1 / 24 / 200 各一组）；
8. 已知不足，以及留给阶段五（Fentbot）/ 六（Pilot）的问题。

## 明确不做（留给后续阶段）

* Fentbot 的 Fent Ticks / Tweaker 系列、Pilot 的种群 / 探索深度 / Top-K / 序列长度；
* 目标点自动位移（永远不做）；
* 新增 cvar、改 33 个既有 cvar 的数量与语义、改界面布局与面板集合。

====

---

## 备注（给人类，不用发给 AI）

* 本文件是 [`AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md) 的下一档。
  分档与依赖见 [`AVOID_TECHNICAL_DOCUMENTATION.md` 附录 F](AVOID_TECHNICAL_DOCUMENTATION.md#附录-f交付分档与路线图)。
* 本阶段的**唯一新维度是"瞄准"**：Legit 已经证明的那套地基（共享模拟器、感知层、UCT、跳跃/松钩策略、
  玩家预测、参数台架、恢复默认按钮）全部可以直接复用，任务书里已经把它们点出来了，不要重造。
* 交回来的结果不好时，先查这五件事：
  1. 7 个 Blatant 参数是不是真的逐个接线了（本项目栽过两次）；
  2. 坠落自动勾墙是不是**真的**在"勾墙 → 挂住"这条链路上（而不是靠跳跃蒙混过关）；
  3. `safe_aim_tracking` 打开时有没有甩准星的抖动；
  4. 候选空间是否爆炸、`cost` 在 quality 200 + 8 个预测玩家时是否仍 ≤ 1.5 ms；
  5. Basic / Legit 的 26 个既有用例是否仍然全绿。
