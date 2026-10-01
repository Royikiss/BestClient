# BestClient 避障 (Avoid / Gores Bot) 技术架构与开发交付文档

> **面向后续开发人员与 AI Agent 的完整技术规范**
> **文档版本**: 1.2.5 (阶段三 Legit 交付 + 感知半格化/椭圆化 + 跳跃最后手段 + 参数灵敏度台架 + 恢复默认按钮)
> **适用代码分支**: `feature/tas`
> **最后更新**: 2026-10-02 (v1.2.5: 感知半径默认值 12 → **2**（= 1 图块），出厂手感定为"最后一刻才出手"；**已有配置不受影响**，点"恢复默认"可拿到新默认)
> **本阶段交付**: 完整界面 + 完整输入管线 + 完整危险感知层 + **Basic 决策引擎** + **Legit 决策引擎（Blatant / Fentbot / Pilot 留待阶段四及以后）**

---

## 速览：下一个 AI 先读这 8 条

> 这份文档是**自解释**的：只读本节也能安全接手。详细内容在后续章节。

1. **任务来源**：复刻参考客户端（KRX，闭源）的 Avoid / Gores Bot 功能，需求原文见 [1.1](#11-需求原文用户描述的目标体验)。
2. **当前进度**：界面、配置、危险感知层、输入拦截管线、HUD 全部完成（[0.1](#01-本次做了什么)），
   **Basic（v1.1.0）与 Legit（v1.2.0，并在 v1.2.1~v1.2.5 打磨：半格/椭圆感知、跳跃最后手段、参数台架、
   恢复默认按钮、默认半径 1 格）的决策引擎都已完成**；Blatant / Fentbot / Pilot 的算法仍是显式占位。
3. **决策引擎入口**：`CAvoid::EvaluateBestPlan(const SContext &Ctx)`，位于
   `src/game/client/components/bestclient/avoid.cpp`（前置闸门 + 代理分派 + Basic 算法）。
   前向模拟器与 Legit 的 UCT 搜索在 `avoid_engine.h/.cpp`（`Avoid::` 命名空间）里 ——
   **所有代理与全部离线测试都复用同一个模拟器**，见 [6.7](#67-basic-代理实现说明v110) 与 [6.8](#68-legit-代理实现说明v120)。
4. **不要改接口**：`SContext`（输入）与 `SInputPlan`（输出）已经接好写回通路、性能计时、UI 显示与状态机，
   直接填算法即可，见 [6.1](#61-函数签名与契约)。
5. **写回方式**：只需要填 `Plan.m_Input` 并置 `Plan.m_Override = true`，
   `ApplyInput()` 会把它写进 `pData`。**绝对不要**写 `m_Controls.m_aInputData`（原因见 [3.2](#32-为什么不写-m_controlsm_ainputdata)）。
6. **实时性已经铺好**：代理启用时 `SnapInput()` 强制 50 Hz 发包（[3.5](#35-启用代理时强制-50-hz-输入发送阶段一已实现关键)），
   `ApplyInput()` 每 tick 调用一次，用 `m_LastDecisionTick` 去重。
7. **危险判定必须复用** `ClassifyPoint()` / `IsRelevantHazard()`，探测点与 DDNet 引擎逐条对齐（[4.2](#42-探测点规则与-ccharacter-完全一致避免误判)）。
8. **改完必须做的三件事**：`ninja -C build DDNet`；跑 [附录 D](#附录-d回归自检脚本) 的自检脚本；
   更新本文档的 [附录 C 变更历史](#附录-c变更历史)。

> 推进方式：**Basic 已完成**（[`docs/AVOID_STAGE2_BASIC_PROMPT.md`](AVOID_STAGE2_BASIC_PROMPT.md)），
> **Legit 已完成**（[`docs/AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md)，v1.2.0）。
> **下一档是 Blatant**（Track Point / Safe Aim Tracking / Auto Drag / 内置瞄准 / NSIF 调优），
> 任务书在阶段四开工前编写；分档与依赖关系见 [附录 F](#附录-f交付分档与路线图)。
> 附录 A 是最初的完整阶段二任务书：其中"第 1 步 前向模拟器"已经完成
> （`CAvoid::SimulateInput()` / `Avoid::SimulateFixed()`，见 [6.7](#67-basic-代理实现说明v110)），
> 从"第 2 步"之后的思路仍然有效，但**不要再直接把附录 A 当任务书下发**
> （它把 Track Point / 内置瞄准和 Legit 混在一起了，那两项属于阶段四 Blatant）。

---

## 0. 本阶段交付边界（先读这一节）

### 0.1 本次做了什么

| 编号 | 交付内容 | 状态 |
| :--- | :--- | :--- |
| D1 | `TAS&` 菜单下新增第三个子标签页 **避障 / Avoid**，完整 UI（顶部状态栏 + 左栏模式与状态 + 右栏参数分组） | ✅ 完成 |
| D2 | 独立客户端组件 `CAvoid`（`avoid.h` / `avoid.cpp`），完整生命周期、状态机、遥测 | ✅ 完成 |
| D3 | 33 个持久化配置变量 `bc_avoid_*`（对齐参考客户端 avoid 文档的全部参数面） | ✅ 完成 |
| D4 | **危险感知层**：与 DDNet 物理引擎逐条对齐的图块分类 + 半径内最近威胁扫描 | ✅ 完成 |
| D5 | **输入拦截管线**：`CGameClient::OnSnapInput` 挂点，逐帧把玩家输入交给避障层 | ✅ 完成 |
| D6 | 游戏内状态 HUD + 世界空间威胁可视化（危险图块高亮 / 感知环 / 威胁连线） | ✅ 完成 |
| D7 | 控制台命令 `avoid_toggle` / `avoid_status` / `avoid_reset` | ✅ 完成 |
| D8 | 中英俄三语本地化词条（中文 95 条 + 俄文 95 条） | ✅ 完成 |
| D9 | **输入管线自检开关**（`bc_avoid_debug_override`）：强制接管并反转左右键，用于证明拦截管线端到端有效 | ✅ 完成 |
| D10 | **决策引擎 `CAvoid::EvaluateBestPlan()`** | ✅ **Basic 完成 (v1.1.0)**；Legit/Blatant/Fentbot/Pilot 仍待实现 |
| D11 | **游戏内 AVOID 面板接入 HUD 编辑器**（`HudLayout::MODULE_AVOID`）：可拖动、缩放、调透明度、持久化，内容与设置页一致 | ✅ 完成 (v1.0.1) |
| D12 | **危险圆环重影修复**：世界空间叠加层改锚定角色实际渲染位置 | ✅ 完成 (v1.0.1) |
| D13 | **HUD 面板被地图前景墙遮挡的修复**：`m_Tas` / `m_Avoid` 从世界渲染阶段移入 HUD 渲染阶段 | ✅ 完成 (v1.0.2) |
| D14 | **前向模拟器 `CAvoid::SimulateInput()`**：克隆 `CCharacterCore`，用地图 tuning + `ClassifyPoint()` 推演 N 帧 | ✅ 完成 (v1.1.0) |
| D15 | **Basic 决策**：`{-1, 0, +1}` 三方向候选 + 快路径/介入阈值/NSIF，只改 `m_Direction` | ✅ 完成 (v1.1.0) |
| D16 | **模拟器保真度回归测试** `src/test/avoid_sim_test.cpp`（3 个用例，`ninja -C build testrunner`） | ✅ 完成 (v1.1.0) |
| D17 | **决策引擎拆分** `avoid_engine.h/.cpp`：契约、感知、共享模拟器、身法门、`Avoid::CPlanner` 全部脱离 `CGameClient`，可被 `testrunner` 链接 | ✅ 完成 (v1.2.0) |
| D18 | **Legit 候选空间** `方向(3) × 跳跃(2) × 钩子(3)`，玩家自己的取值恒在集合内并排第一 | ✅ 完成 (v1.2.0) |
| D19 | **钩子释放**：以"松钩候选的固定序列能否跑满 `check_ticks`"为唯一判据，配合 `bc_avoid_hook_assist` / `bc_avoid_hook_weight` | ✅ 完成 (v1.2.0) |
| D20 | **UCT 搜索**：`quality`=迭代次数、`randomness`=探索常数与 rollout 分支概率、三个 Weight=打分项；本地 PRNG 保证逐 tick 确定性 | ✅ 完成 (v1.2.0) |
| D21 | **其他玩家预测**：私有 `CWorldCore` + ≤8 个影子 `CCharacterCore` 快照，`TickDeferred()` 角色互撞与钩索拖拽在克隆里真实发生 | ✅ 完成 (v1.2.0) |
| D22 | **解冻块专用前瞻**（`bc_avoid_unfreeze_ticks`）、**身法状态门**（喷气 / 钩人 / 飞锤）、**Strict 理由回填**与 `avoid_status` 配置提示 | ✅ 完成 (v1.2.0) |
| D23 | **Legit 决策回归测试** `src/test/avoid_legit_test.cpp`（16 个用例，含坏场景自证断言、参数灵敏度台架与性能基准） | ✅ 完成 (v1.2.0 / v1.2.3) |
| D24 | **感知半径半格化 + 椭圆化**：单位从"图块"改"半格"（1~32），到达判定改为"到危险块盒的椭圆距离"，竖直系数由地图 tuning 推导（默认 0.65） | ✅ 完成 (v1.2.1 / v1.2.2) |
| D25 | **跳跃降级为最后手段**：钩索期间（按住钩 / `HOOK_GRABBED` / `HOOK_FLYING`）一律不新增跳跃；其余情况只在 `PlayerSafe ≤ 6` 的危急关头才放开 | ✅ 完成 (v1.2.2) |
| D26 | **参数灵敏度台架**：脚本化"走神玩家"逐档扫描全部滑条，输出存活/接管帧数/跳跃/松钩/提前量/耗时 | ✅ 完成 (v1.2.3) |
| D27 | **"恢复默认"方形按钮**（面板标签栏右端）+ 出厂默认半径改为 **2 半格（= 1 图块）** | ✅ 完成 (v1.2.4 / v1.2.5) |

> [!IMPORTANT]
> **下一档（阶段四 Blatant）需要做的是**：瞄准方向搜索（Track Point / Safe Aim Tracking）、
> Auto Drag、内置瞄准（Segments / FOV / Auto Aim / Aim Assist）与 NSIF 调优。
> **前向模拟器（D14/D17）不用重写**——它是所有代理的公共地基；
> 需要新物理就在 `Avoid::SSimState` 的克隆配置里加开关，并补一条保真度用例（[附录 F.2](#f2-每档必须留下的地基验收时会检查)）。
> 详见 [第 6 章](#6-决策引擎实现指南)、[6.7](#67-basic-代理实现说明v110) 与 [6.8](#68-legit-代理实现说明v120)。

### 0.2 当前实际行为（验收时你会看到什么）

打开 `设置 → TAS& → 避障`：

* **顶部状态栏**实时显示：模块状态徽章（`DISABLED` / `STANDBY` / `WATCH` / `ASSIST` / `NSIF` / `AFK`）、当前代理徽章、最近危险类型与距离、当前决策文字、接管次数。
* **左栏**包含三张卡片：**辅助模式**（5 种模式的按钮 + 说明）、**总控**（启停避障 + 模块/HUD/可视化/管线自检开关）、**实时状态**（角色状态、最近危险距离与坐标、已探测图块数、危险图块数、安全前瞻帧数、接管次数）。
* **右栏**是**当前模式专属**的参数面板：Basic 2 个、Legit 4 个、Blatant 5 个、Fentbot 与 Pilot 各 4 个，面板名称与内容随模式改变；所有滑块与勾选框即时写回 cvar 并持久化。
* **游戏内状态面板**：避障页勾选"状态 HUD"（或在 HUD 编辑器里开启 `Avoid` 模块）后，游戏内出现
  与设置页"实时状态"卡片同样详细的 AVOID 面板；它可以像其他 HUD 一样**拖动、缩放、调透明度**。
  默认关闭，不会打扰不使用避障的玩家。
* 打开 `bc_avoid_show_visuals 1` 后，世界里会高亮标出感知半径内所有危险图块，并画出感知环与指向最近威胁的连线。

**Basic 代理已经能用了**：勾选"启用避障代理"（Basic 模式）后，朝黑水/冻结块走过去会被反向按键刹停，
钩索荡向危险时也会自动按下反方向的键减速。**玩家本身安全的 tick 完全不干预**——
状态徽章在 `WATCH`（观察）与 `ASSIST`（接管）之间切换，每一次接管都会在"安全前瞻"里给出存活帧数。

**Legit 代理也能用了（v1.2.0）**：切到 **拟真** 并启用代理后，除了 Basic 的方向键制动，它还会

* **提前松开钩子**：按住钩被拖向黑水/贴黑水的墙时，只有"松钩候选能活满 `check_ticks`"才会松（判据是模拟结果，不是落点是否危险）；
* **在需要时跳一下**：跳跃只在真的能多活时才被选中（并列时永远选玩家自己的输入，所以不会原地鬼畜跳）；
* **预测其他玩家**：感知半径内最近的 8 个 tee 会被注入克隆体，队友把你撞向黑水也能提前反应（关掉 `bc_avoid_player_prediction` 则完全不预测）；
* 把 `Quality` / `Randomness` / 三个优先级滑块真正接进了搜索（[6.8](#68-legit-代理实现说明v120)）。

**v1.2.1 ~ v1.2.5 的打磨（同上，切到拟真后直接生效）**：

* **感知范围是椭圆**：横向 = 滑条值（半格步进，0.5 ~ 16 图块），纵向 = 横向 × 竖直系数（默认 0.65，由地图 gravity /
  地面速度推出），所以"正下方的坑"比"同距离的侧墙"更晚被发现；
* **出厂默认半径 = 2 半格（1 图块）**：平时完全不打扰，只在最后一刻动一下（代价：1 格不够刹停，
  想提前刹停请调到 4 以上）；
* **跳跃是最后手段**：勾着墙被拖向黑水时**不起跳**（只会松钩 / 反向键）；没有钩的时候，跳跃也只在你自己的输入
  还剩 ≤ 6 帧时才放开——你还有时间自己反应，代理就不会动；
* **面板标签栏右端有"恢复默认"方形按钮**：把当前模式的参数一键恢复出厂值（不动模式选择与启停）；
* **参数灵敏度台架**（[8.3c](#83c-参数灵敏度台架这个滑条到底有没有用)）能量化回答"这个滑条到底有没有用"。

**仍然不做的事**（属于阶段四及以后）：

* **不会瞄准、不会开火、不会 Track Point / Auto Drag**：这些是 Blatant 的功能（见 [`docs/avoid/blatant.md`](avoid/blatant.md)），Legit 一律不碰；
* **Blatant / Fentbot / Pilot 的算法仍是占位**：切到这三个模式不会生效，状态栏会显示 `该代理尚未实现`（v1.1.1 里它们会静默地跑 Basic 的算法，与本节描述不符，v1.2.0 已按描述改成显式占位）；
* **飞锤 / 深飞 / 喷气背包中不动手**：这是 TAS 红线，见 [6.8](#68-legit-代理实现说明v120) 的身法门。

**调试开关**：左栏总控里的 **输入管线自检**（`bc_avoid_debug_override`）会强制接管并反转左右键，
这是专门用来验证"拦截管线真的能操控角色"的调试开关，默认关闭。验证步骤见 [8.4](#84-输入拦截管线验证bc_avoid_debug_override)；
`bc_avoid_log 1` 会每 tick 打印一行决策日志（见 [6.7](#67-basic-代理实现说明v110)）。

---

## 1. 需求来源与效果目标

### 1.1 需求原文（用户描述的目标体验）

> 我操控 tee 往黑水走（或者其他的自杀方块之类的）或者通过钩子荡过去、拉向附着有黑水的墙壁等任何速度朝向黑水，
> 并且我反应不过来时他就能立刻停在黑水边上，向左向右的黑水无论如何也走、荡、勾不进去。
> 走，这个按键会不起作用或者迅速减速；荡也是如此，会自己按下反方向的按键减速；
> 勾，这个是朝着勾的方向给一个向量力，如果我持续按住钩子，我就会往那个地方走，如果那个地方有黑水，他会提前松开钩子。

拆解成可验证的三条验收标准：

| 动作 | 期望效果 | 物理本质 |
| :--- | :--- | :--- |
| **走** (按 A/D 朝危险移动) | 按键失效或迅速减速，停在危险边缘 | 反向 `m_Direction` 制动 + 必要时置 0 |
| **荡** (钩住地面靠钩索摆动撞向危险) | 自动按下反方向键减速 | 用 `m_Direction` 抵消切向速度分量 |
| **勾** (勾向贴有黑水的墙 / 持续按住钩子被拉向危险) | 提前松开钩子 (`m_Hook = 0`)，必要时给反向向量 | 预测钩索落点是否危险 → 提前释放 |

核心难点是 **实时性**：DDNet 物理固定 50 TPS（20 ms/tick），必须在**当前 tick 之内**判定"玩家当前输入在 N 帧内是否会致死"并给出替代输入。

### 1.2 参考实现的功能面（`docs/avoid/`）

参考客户端（KRX，闭源）的 avoid 文档已拷贝到 `docs/avoid/`，本模块的参数面完全对齐它：

| 文档 | 对应本模块 |
| :--- | :--- |
| `avoid.md` | 5 种代理下拉：Basic / Legit / Blatant / Fentbot / Pilot |
| `legit.md` | 玩家预测、AFK 保护、Check Ticks、Quality、Randomness、方向/钩子/生命优先级、Teles/Death/Unfreeze 图块 |
| `blatant.md` | NSIF、Kick in Ticks、Track Point、Safe Aim Tracking、Auto Drag、内置瞄准（Segments / FOV / Auto Aim / Aim Assist） |
| `fentbot.md` | Fent Ticks / Tweaker Inputs / Tweaker Ticks / Tweaker Dosage（本阶段仅预留 UI） |
| `pilotbot.md` | Mode / Population Size / Exploration Depth / Top-K / Sequence Length（本阶段仅预留 UI） |
| `settings.md` | `cl_prediction_margin` 建议、传送预测、死亡图块预测、移动限制预测、玩家体积预测 —— 其中哪些已经对齐、哪些还没做，见 [6.8.10](#6810-与参考实现krx的参数机制对照) |

> [!NOTE]
> 参考实现的 Basic 代理是**免费版**功能，只用左右方向键；Legit/Blatant 是**基于蒙特卡洛树搜索 (MCTS)** 的
> 前向模拟（文档里 `krx_avoid_num_iterations` = 迭代次数、`krx_avoid_tile_exploration_constant` = UCT 探索常数、
> `krx_avoid_tile_lifespan_weight` = 生存权重，全部是 MCTS 术语）。本模块的参数命名与语义与之对齐，
> 阶段二可以直接沿用这套搜索框架。

---

## 2. 代码结构与文件地图

| 模块/文件 | 路径 | 核心职责 |
| :--- | :--- | :--- |
| **避障核心组件** | `src/game/client/components/bestclient/avoid.h`<br>`src/game/client/components/bestclient/avoid.cpp` | 状态机、配置快照、输入管线、决策前置闸门、Basic 算法、游戏内 HUD、世界空间可视化、控制台命令 |
| **决策引擎（可离线测试）** | `src/game/client/components/bestclient/avoid_engine.h`<br>`src/game/client/components/bestclient/avoid_engine.cpp` | 数据契约、危险感知层（唯一实现）、共享前向模拟器、身法状态判定、**Legit 的候选生成与 UCT 搜索** |
| **Legit 决策回归测试** | `src/test/avoid_legit_test.cpp` | 16 个用例：松钩/不松钩、Quality、HookWeight、玩家预测、解冻前瞻、窄通道、身法门、确定性、Randomness、跳跃时机、钩索禁跳、椭圆感知、参数灵敏度台架、成本预算 |
| **"拟真"验收评分卡** | `docs/AVOID_LEGIT_ACCEPTANCE.md` | 五条设计原则 + A~E 类可测判据 + 三分钟手感流程 + 未达标项 |
| **下一档任务书** | `docs/AVOID_STAGE4_BLATANT_PROMPT.md` | 阶段四（激进 / Blatant）的开发提示词 |
| **避障菜单页** | `src/game/client/components/bestclient/menus_avoid.cpp` | `CMenus::RenderSettingsAvoid()`：顶部状态栏 + 左栏（模式/状态）+ 右栏（4 个参数面板） |
| **TAS& 子标签栏** | `src/game/client/components/bestclient/menus_tas.cpp` | `RenderSettingsTasAnd()` 现在是 3 个子标签：`TAS` / `Avoid` / `Auxiliary` |
| **菜单声明** | `src/game/client/components/menus.h` | 新增 `void RenderSettingsAvoid(CUIRect MainView);` |
| **配置变量定义** | `src/engine/shared/config_variables_bestclient.h` | `bc_avoid_*` 共 30 个持久化变量 |
| **客户端组件注册** | `src/game/client/gameclient.h`<br>`src/game/client/gameclient.cpp` | `CAvoid m_Avoid;` 成员声明 + 组件列表注册 + `OnSnapInput` 输入挂点 |
| **构建源文件清单** | `CMakeLists.txt` | 新增 `avoid.cpp/.h`、`menus_avoid.cpp` |
| **多语言** | `data/BestClient/languages/simplified_chinese.txt`<br>`data/BestClient/languages/russian.txt` | 各新增 95 条 `[BestClient]` 词条 |

### 2.1 关键改动点（改动极小，便于回归审查）

```cpp
// src/game/client/gameclient.cpp : CGameClient::OnSnapInput()
	if(!Dummy)
	{
		int Ret = m_Controls.SnapInput(pData);
		if(m_Tas.IsRecordingActive() && !m_FastPractice.Enabled())
			m_Tas.OnRecordInput(pData, Conn == 1);
		// bestclient: Avoid runs after TAS so that a recorded track always stays bit exact,
		// and only ever on the connection the player is actually controlling.
		if(Ret > 0)
			m_Avoid.ApplyInput(pData, Ret, Conn == 1);
		// bestclient
		return Ret;
	}
```

```cpp
// src/game/client/gameclient.cpp : 组件列表（紧跟 TAS 之后）
					      &m_FastPractice, // bestclient
					      &m_Tas, // bestclient
					      &m_Avoid, // bestclient
```

---

## 3. 输入拦截管线（阶段二的生命线，务必理解）

### 3.1 为什么挂在 `OnSnapInput`

`CClient::SendInput()` 每个 tick 对两条连接各调用一次：

```cpp
// src/engine/client/client.cpp
for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
{
	if(!DummyConnected() && Dummy != 0)
		break;
	int i = g_Config.m_ClDummy ^ Dummy;   // Dummy==0 → i == 玩家当前控制的连接
	int Size = GameClient()->OnSnapInput(m_aInputs[i][m_aCurrentInput[i]].m_aData, Dummy, Force);
	...
}
```

三个关键结论：

1. **`Dummy == false` 那一支就是"玩家当前正在操控的连接"**。`g_Config.m_ClDummy ^ 0 == g_Config.m_ClDummy`。
   因此避障层只在 `if(!Dummy)` 分支里挂载，永远不会动到分身连接。
2. **`pData` 直接指向 `m_aInputs[...]` 输入环形缓冲区**，而客户端物理预测正是通过
   `Client()->GetInput(Tick, ...)` 从这个环形缓冲区取输入（见 `CGameClient::OnPredict()`）。
   → **只要改写 `pData`，本地预测与发往服务器的数据包会同时生效**，无需任何额外同步。
3. `CControls::SnapInput()` 在无输入变化时返回 `0`；返回非 0 时 `Size == sizeof(CNetObj_PlayerInput) == 40`。
   所以 `ApplyInput` 只在真正有输入包时被调用（约 50 Hz）。

### 3.2 为什么**不**写 `m_Controls.m_aInputData`

TAS 回放会同时写 `m_Controls.m_aInputData[...]`，因为它要把客户端辅助系统全部切换到回放数据。
避障层**故意不这么做**，原因：

* `m_aInputData` 里保存的是**玩家真实按键状态**，其中 `m_Fire` 采用"边沿计数"语义
  （`m_Fire = (m_Fire + 1) | 1` 表示按下，`& ~1` 表示松开）。避障层写回会污染下一 tick 的开火边沿判定。
* 避障的目标是"帮玩家不撞危险"，不是"替换玩家操作"。玩家的按键状态必须保持原样，
  这样一旦判定玩家的输入本身是安全的（绝大多数 tick），玩家感受不到任何干预。

> [!CAUTION]
> **阶段二实现时同样遵守此约定**：只改 `Plan.m_Input`（最终写回 `pData`），不要写 `m_Controls.m_aInputData`。

### 3.3 tick 内的执行顺序

```
CClient::Update()
 ├─ SendInput()                    → CGameClient::OnSnapInput() → CAvoid::ApplyInput()   ← 避障在这里决策
 │                                                                  ├─ ScanThreat()      感知
 │                                                                  ├─ EvaluateBestPlan() 决策（阶段二）
 │                                                                  └─ 写回 pData
 └─ GameClient()->OnPredict()      → 用（可能已被改写的）输入做本地预测 → 画面立刻反映避障动作
```

**这意味着"反应不过来时立刻停住"在物理上是成立的**：改写后的输入在同一 tick 就进入预测，
没有任何一帧的延迟（延迟只来自感知半径与前瞻帧数的配置）。

### 3.4 与 TAS 的关系

* 避障在 TAS 录制写入 `OnRecordInput()` **之后**执行 → 录进 `.tas` 文件的是**玩家原始输入**，
  不受避障影响（保证 TAS 轨迹纯净、可回放）。
* 避障在 TAS 回放态下**完全不介入**：`m_Tas.IsPlaybackActive()` 时 `OnSnapInput` 提前 return，
  根本走不到避障分支。
* 如果 TAS 本地沙盒 (`CFastPractice`) 处于 `Active()` 状态，避障的感知层会自动
  `ResolvePracticeRoles()` 并读取沙盒世界里的角色实体（见 `CAvoid::ActiveCore()`），
  保证"练图时的感知"和"实战时的感知"一致。

### 3.5 启用代理时强制 50 Hz 输入发送（阶段一已实现，关键）

`CControls::SnapInput()` 有一条例行节流：

```cpp
Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
```

也就是说**玩家按住某个键不动时，客户端只以 25 Hz 发包**，另外一半 tick 上
`SnapInput()` 返回 `0`，`CGameClient::OnSnapInput()` 里的避障挂点**根本不会被调用**——
正好是最需要反应的时刻（稳定朝危险走/荡）。

因此本模块在 `SnapInput()` 中增加了与 `m_FastPractice.Enabled()` 同款的旁路：

```cpp
// bestclient: an armed Avoid agent has to be able to rewrite the input on EVERY tick.
if(GameClient()->m_Avoid.WantsEveryTickInput())
	Send = true;
```

* `WantsEveryTickInput()` 在**代理已启用**（`bc_avoid_enabled && bc_avoid_active`）时返回 true。
* 效果：启用代理后 `CAvoid::ApplyInput()` 稳定以 **50 Hz** 被调用，与物理帧严格 1:1 对齐，
  阶段二的前向模拟可以直接假设"每一 tick 都能改写输入"。
* 未启用代理时完全不生效，普通玩家的发包频率与带宽不受影响。
* 同类先例：TAS 回放与 FastPractice 沙盒也各自强制每 tick 发包。

> [!IMPORTANT]
> 阶段二如果发现"明明算出了方案却没生效"，第一件事就是确认 `WantsEveryTickInput()` 仍然返回 true，
> 以及挂点没有被 `Ret > 0` 的判断挡掉。

---

## 4. 危险感知层（阶段一已实现，阶段二直接复用）

### 4.1 与 DDNet 引擎逐条对齐的图块分类

`CAvoid::ClassifyTile(int Tile)` 只做纯映射，不查地图：

| 图块常量 | 分类位 | 说明 |
| :--- | :--- | :--- |
| `TILE_DEATH` | `HAZ_DEATH` | 黑水 / 自杀方块 |
| `TILE_FREEZE` | `HAZ_FREEZE` | 冻结 |
| `TILE_DFREEZE` | `HAZ_DEEP` | 深层冻结（不可自行解冻） |
| `TILE_LFREEZE` | `HAZ_LIVE` | 活冻结 |
| `TILE_UNFREEZE` | `HAZ_UNFREEZE` | 解冻（可选危险，默认不躲） |
| `TILE_TELEIN` / `TILE_TELEOUT` / `TILE_TELECHECK` / `TILE_TELECHECKIN` / `TILE_TELECHECKOUT` / `TILE_TELEINEVIL` / `TILE_TELECHECKINEVIL` / `TILE_TELEINWEAPON` / `TILE_TELEINHOOK` | `HAZ_TELE` | 传送（可选危险，默认不躲） |

### 4.2 探测点规则（与 `CCharacter` 完全一致，避免误判）

`CAvoid::ClassifyPoint(vec2 Pos)` 复刻官方两处实现：

```cpp
// 1) 中心点探测 —— 对应 CCharacter::HandleTiles()
const int Index = Collision()->GetPureMapIndex(Pos);
Flags |= ClassifyTile(Collision()->GetTileIndex(Index));        // 主层
Flags |= ClassifyTile(Collision()->GetFrontTileIndex(Index));   // 前景层
Flags |= ClassifyTile(Collision()->GetSwitchType(Index));       // 开关层（死亡开关）

// 2) 四角探测 —— 对应 CCharacter::HandleSkippableTiles()
constexpr float HAZARD_CORNER_PROBE = CCharacterCore::PhysicalSize() / 3.0f;  // 28 / 3 ≈ 9.333 px
(Pos.x ± R, Pos.y ± R)  →  GetCollisionAt / GetFrontCollisionAt == TILE_DEATH
```

> [!IMPORTANT]
> **探测点必须严格等于官方**。TAS 模块 `CTas::IsHazard()` 在文档 v1.3.1 中已经踩过这个坑：
> 多用外延探测点会在穿越一格宽（32 px）通道时误触发。四角跨度 `2 × 9.33 = 18.67 px`，
> 通道余量 `32 - 18.67 = 13.33 px`。**阶段二做前向模拟时也必须沿用同一套探测点**，
> 否则会出现"感知说危险、物理其实安全"（或反之）的错位。

### 4.3 半径扫描

```cpp
CAvoid::SThreat CAvoid::ScanThreat(const CCharacterCore &Core) const
```

* 以角色为中心，扫描"水平 `ceil(Rx)` × 垂直 `ceil(Ry)`"的矩形区域
  （`Rx` = 配置值 ÷ 2 图块，默认 6；`Ry = Rx × 垂直系数`，见下），默认 13 × 9 个采样点。
* 每个采样点取图块中心 `((Tx + 0.5) * 32, (Ty + 0.5) * 32)` 调 `ClassifyPoint()`。
* 若命中且 `IsRelevantHazard()`（按 `bc_avoid_tile_*` 开关过滤），**并且**满足
  **椭圆判据** `(dx/ReachX)² + (dy/ReachY)² ≤ 1`（`dx/dy` 取自 `Avoid::TileBoxDelta()`，
  即角色中心到图块盒的**每轴**距离），才计入 `m_HazardTiles`，
  并用欧氏距离更新 `m_NearestPos / m_NearestDistPx`（HUD 与可视化读数保持"像素距离"）。
* **垂直系数**由地图 tuning 推出：`0.5 · gravity · check_ticks² / (ground_control_speed · check_ticks)`，
  默认 tuning + 26 帧 = **0.65**，被夹在 `[0.25, 1]`。含义是"同一个前瞻窗口内，垂直方向受重力限制
  只能走水平方向的 65%"，所以下方/上方的危险会比同距离的侧向危险**更晚**被发现（详见 [6.8.11](#6811-跳跃策略与椭圆感知v122)）。
* 角色自身冻结状态（`m_FreezeEnd != 0 || m_DeepFrozen || m_LiveFrozen`）会置 `HAZ_SELF`，
  该位**不受图块开关影响**（它是结果状态，不是图块）。
* 地图未加载时（`Collision()->GetWidth() <= 0`）直接返回空结果，避免 `GetPureMapIndex` 对空网格做
  `std::clamp(v, 0, -1)` 的 UB。

性能：每 tick 约 1700 次数组查询，可忽略。实测计入 `Plan.m_CostMs` 由 UI 显示。

### 4.4 数据结构

```cpp
struct SThreat
{
	int m_Flags;              // 危险位掩码（HAZ_* 的并集）
	vec2 m_NearestPos;        // 最近危险图块中心（世界像素）
	float m_NearestDistPx;    // 角色到该图块包围盒的最近距离（像素）
	bool m_HasNearest;
	int m_SensedTiles;        // 本帧扫描的图块总数
	int m_HazardTiles;        // 其中被判定为危险的图块数
	bool m_OnHazard;          // 角色当前是否已在危险中
};
```

---

## 5. 界面规格（阶段一已实现）

### 5.1 布局（严格遵循"左上角状态栏 / 左栏模式与状态 / 右栏参数"）

```
┌───────────────────────────────────────────────────────────────────────────────┐
│ [50px 顶部状态栏] 状态徽章 | AVOID:代理徽章 | 威胁/决策/前瞻/接管 | 快捷绑定提示 │
├──────────────────────────────────┬────────────────────────────────────────────┤
│ 左栏 (切换模式 + 查看状态)         │ 右栏 (调整具体参数)                          │
│ ┌ 辅助模式 118px ───────────────┐ │ [面板导航栏：随模式变化] 22px                │
│ │ Basic | Legit | Blatant       │ │ ┌ 参数面板 376px ─────────────────────────┐ │
│ │ Fentbot | Pilot               │ │ │ 滑块 / 勾选框 / 说明文字                  │ │
│ │ 代理说明文字                   │ │ │                                          │ │
│ └───────────────────────────────┘ │ │                                          │ │
│ ┌ 总控 122px ───────────────────┐ │ │                                          │ │
│ │ [启用/解除避障代理] + 说明      │ │ │                                          │ │
│ │ [x]模块 [x]HUD                 │ │ │                                          │ │
│ │ [ ]可视化 [ ]管线自检           │ │ │                                          │ │
│ └───────────────────────────────┘ │ │                                          │ │
│ ┌ 实时状态 132px ───────────────┐ │ │                                          │ │
│ │ 角色 / 最近危险 / 危险-探测数  │ │ │                                          │ │
│ │ 安全前瞻 / 接管次数 + 说明      │ │ │                                          │ │
│ └───────────────────────────────┘ │ └──────────────────────────────────────────┘ │
└──────────────────────────────────┴────────────────────────────────────────────┘
```

**高度预算**（关键，改动布局时必须复核）：总高 `50 + 8 + (118+6+122+6+132) = 442 px`。
DDNet 的 UI 虚拟屏高为 `600 / (clamp(g_Config.m_UiScale, 50, 110) / 100)`。
最坏情况（UiScale = 110）虚拟屏高 545 px，减去外边距 40 px 后主视图为 505 px，
再减去 TAS& 子标签栏的开销（旧界面 `-20 +20 -8 -24 -10`，新界面 `-8 +8 -6 -24 -10`），
**可用内容高为 483 px（旧界面）/ 473 px（新界面）**。所以整页必须 **≤ 473 px**。
TAS 原页面用满 500 px，在 UiScale 110 下底部会被裁掉；避障页刻意做小到 442 px 以留出余量。

### 5.2 右栏面板：**每个模式一套，互不相同**

面板导航栏的内容完全由当前模式决定（`g_aBasicPanels` / `g_aLegitPanels` / `g_aBlatantPanels` /
`g_aFentPanels` / `g_aPilotPanels`），切换模式时面板集合与内容一起变化，且每个模式会**记住自己**
上次选中的面板（`s_aPanelByAgent[]`）。

| 模式 | 面板 1 | 面板 2 | 面板 3 | 面板 4 | 面板 5 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Basic** | `Basic` 代理说明（无参数，忠于参考实现） | `Tiles` 死亡/冻结 + 感知半径 | — | — | — |
| **Legit** | `Assist` 方向/钩子辅助、玩家预测、AFK 开关与时间 | `Tuning` 检查帧数、计算质量、随机程度 | `Priorities` 方向/钩子/生存权重 | `Tiles` 死亡/冻结/解冻/传送 + 解冻前瞻 + 感知半径 | — |
| **Blatant** | `Assist` 方向/钩子辅助、跟踪瞄准点、安全瞄准跟踪、自动拖拽、玩家预测 | `Tuning` 检查帧数、介入阈值、计算质量、随机程度 | `Priorities` 方向/钩子/生存权重 | `Aimbot` 开关、自动瞄准/瞄准辅助、扫描分段、视野角 | `Safety` NSIF、AFK 开关与时间 |
| **Fentbot** | `Avoid` 玩家预测、浅层解冻（解冻块 + 解冻前瞻） | `Calculation` 质量预设/Fent 帧数/调优输入/帧长/剂量（灰色占位） | `Visuals` 渲染路径、渲染寻路、旁观扫描（灰色占位） | `Tiles` 全套图块 + 感知半径 | — |
| **Pilot** | `Main` 玩家预测、导航模式（自主/跟随鼠标/跟随玩家，灰色占位） | `Settings` 种群/探索深度/Top-K/序列长度（灰色占位） | `Visuals` 渲染路径、渲染寻路（灰色占位） | `Tiles` 图块 + 感知半径 | — |

**唯一刻意共享的是 `Tiles` 面板的内容**：它描述的是"哪些图块算危险"，属于模块级感知配置而不是代理参数，
所有模式共用同一份设置是正确行为（且 Basic 与 Pilot 少了"解冻/传送"这一行，仍按模式区分）。

> [!NOTE]
> Fentbot / Pilot 的专属参数以**灰色只读行 + `未实现` 标签**呈现（`AvoidPlannedRow()`）。
> 这样界面已经完整表达了参考客户端的参数面；阶段四/五/六实现算法时把它们换成真实控件即可。

### 5.3 游戏内 HUD 模块（已接入 HUD 编辑器）与威胁可视化

**AVOID 状态面板是一个正式注册的 HUD 模块**，模块 id `avoid`、名称 `Avoid`，位于
`HudLayout::MODULE_AVOID`。它与计分板、按键显示、Edge Info 等模块完全平级：

| 能力 | 说明 |
| :--- | :--- |
| 显示/隐藏 | HUD 编辑器里的眼睛图标，或避障页的"状态 HUD"勾选框（**同一个开关**，两处实时同步） |
| 拖动位置 | 在 HUD 编辑器里直接拖动 |
| 缩放 | 编辑器右下角把手，25%~300% |
| 背景/透明度/颜色 | 编辑器右键面板（与其他模块一致） |
| 重置 | 编辑器的"重置位置/重置缩放/重置设置" |
| 持久化 | 随 `ConfigDomain::HUDLAYOUT` 写入并读取，id 为 `avoid` |

**面板内容与设置页的"实时状态"卡片一致**（这是刻意的，信息质量不打折）：

```
┌────────────────────────────────────────────┐
│ [WATCH]  AVOID  拟真                        │  ← 状态徽章（按状态着色）+ 当前模式
│ 角色      CLEAR  (12.4, 8.9)                │  ← 自身状态 + 图块坐标
│ 最近危险  DEATH  3.2 图块  (14.1, 9.5)      │  ← 最近威胁类型/距离/坐标（按类型着色）
│ 危险/探测 12 / 169                          │  ← 感知半径内的危险数与采样数
│ 安全前瞻  0 帧  (0.00 毫秒)                 │  ← 当前方案存活帧数 + 决策耗时
│ 接管      0 / 0                             │  ← 接管次数 / 决策次数
│ 调试接管 (输入管线自检)                      │  ← 当前决策说明（超宽自动省略号截断）
└────────────────────────────────────────────┘
```

* 基准尺寸 122 × 60 个 HUD 画布单位（画布高固定 300 单位，见 `HudLayout::CANVAS_HEIGHT`），
  默认位置 `(286, 52)`，与 Edge Info / 击杀提示同一片区域，互不重叠。
* **默认关闭**（`gs_aModuleLayouts[MODULE_AVOID].m_Enabled = false`），
  这样升级后不会给不使用避障的玩家平白多出一块面板；在设置页勾选"状态 HUD"即可开启。
* `bc_avoid_show_hud` 是控制台级总开关（默认 1），最终显示条件为
  `bc_avoid_show_hud && HudLayout::IsEnabled(MODULE_AVOID)`，与其他 HUD 模块的约定一致。
* HUD 编辑器打开时，`CAvoid::RenderPreview()` 会绘制面板预览，即使面板当前是关闭状态。

**威胁可视化**（`bc_avoid_show_visuals`，默认关）仍走世界坐标系绘制：
危险图块彩色方框（死亡=红、冻结=蓝、解冻=青、传送=紫）、感知圆环、指向最近威胁的连线。

> [!CAUTION]
> **渲染阶段红线（v1.0.2 修复）**：`&m_Tas` 与 `&m_Avoid` 必须注册在组件列表的 **HUD 阶段**
> （紧跟 `&m_Hud` 之后），不能放在 `m_FastPractice` 后面那一段世界渲染阶段。
> 原因有两条，都是实测踩出来的：
> 1. `&m_MapLayersForeground`（前景地图层）在世界阶段之后绘制，会把先画的 HUD 整块盖住 ——
>    表现就是"人走到墙后面，HUD 也被墙挡住"。
> 2. `CAspectRatio::PrepareComponent()` 在遍历到 `&m_Hud` 时才会关闭画面宽高比覆盖
>    （`SetScreenAspectOverrideEnabled(false)`），并在组件循环结束后由 `BeforeFogRect()` 恢复。
>    放在 `m_Hud` 之前的 HUD 会用**过**的宽高比绘制，在非 16:9 分辨率或开启 HUD/游戏分离宽高比时面板尺寸会算错。
>
> 两者的 `OnUpdate/OnMessage` 顺序不受影响；TAS 的 `CheckRaceAutoTrigger()` 只读快照，改到帧内更晚执行同样正确。

> [!IMPORTANT]
> **重影修复（v1.0.1）**：圆环与连线的锚点改用角色**实际被绘制的位置**
> `GameClient()->m_LocalCharacterPos`（`CAvoid::OverlayAnchor()`），而不是 tick 量化的
> `CCharacterCore::m_Pos`。后者在快速移动时每 20 ms 才跳一次，而角色是按帧插值平滑移动的，
> 两者错位就表现为圆环"重影/拖影"。锚点带 128 px（4 图块）的距离保护，
> 防止刚重生/传送后平滑位置尚未跟上时画到旧位置。
> 图块扫描本身仍以物理核心位置为准（`Core.m_Pos`），所以判定精度不受影响。

## 6. 决策引擎实现指南

> **实现状态（v1.2.0）**：**Basic 与 Legit 都已实现**。
> 契约（6.1）、推荐算法（6.2）、模拟路径（6.3）、性能预算（6.4）与踩坑清单（6.5）依然有效；
> [6.7](#67-basic-代理实现说明v110) 记录 Basic 实际怎么写的，
> [6.8](#68-legit-代理实现说明v120) 记录 Legit（钩子释放 / 跳跃 / UCT / 玩家预测）实际怎么写的，
> 以及它与真实物理的偏差。
> **Blatant / Fentbot / Pilot 仍是显式占位**：切到这三个模式不会改任何输入，状态栏给出原因。

### 6.1 函数签名与契约

```cpp
CAvoid::SInputPlan CAvoid::EvaluateBestPlan(const SContext &Ctx);
```

**输入（只读）**

```cpp
struct SContext
{
	int m_Tick;                      // 本次决策所属的客户端 tick
	int m_ClientId;                  // 角色客户端 id（沙盒模式下为沙盒内 id）
	bool m_Dummy;                    // 被控角色是否为分身连接
	CCharacterCore m_Core;           // 角色当前完整物理状态（位置/速度/钩索/冻结/跳跃次数…）
	CNetObj_PlayerInput m_Input;     // 玩家本 tick 将要发出的输入（注意：含 NextWeapon/PrevWeapon）
	bool m_PlayerInputSafe;          // 预留给引擎回填（可选）
	SThreat m_Threat;                // 已算好的危险扫描结果
	SSettings m_Settings;            // 完整配置快照（见 avoid.h；m_SensingRadius 是 float 图块数，半格步进）
};
```

**输出**

```cpp
struct SInputPlan
{
	bool m_Override;             // true = 用 m_Input 替换玩家输入；false = 完全不干预
	bool m_UsedFallback;         // true = NSIF 兜底（没有任何方案能活满前瞻）
	CNetObj_PlayerInput m_Input; // 必须从 Ctx.m_Input 复制后修改，不要从零构造
	int m_SafeTicks;             // 所选方案能存活的帧数
	int m_ScannedTicks;          // 所有候选里模拟到的最深帧数（= 本次搜索的实际深度）
	int m_Candidates;            // 本帧评估的方案数
	float m_Score;               // 最优方案得分（越大越好；Basic 用生存帧数主导）
	float m_CostMs;              // 引擎自身耗时（毫秒），会显示在 UI 上
	char m_aReason[64];          // 给 HUD 看的短说明（必须走 BcLocalize(...)）
};
```

**写回**：`ApplyInput()` 已经在 `Plan.m_Override == true` 时执行
`mem_copy(pData, &Plan.m_Input, sizeof(CNetObj_PlayerInput))`，你不需要碰 `pData`。

### 6.2 推荐算法（与参考实现同构，可直接落地）

参考客户端用的是 MCTS（UCT）。推荐先用**更简单、更好调、确定性更强**的方案，跑通后再升级：

```
输入: Ctx
1. 快速路径（最重要，决定"手感"）
   - 用 Ctx.m_Input 前向模拟 CheckTicks 帧。
   - 若全程不碰危险 → 直接 return {m_Override=false}（玩家完全自由）。
   - 若最早致死帧 >= KickInTicks → 同样不干预（"还能再玩一会儿"，避免过度干预）。
2. 候选生成（Quality 控制数量）
   - 方向: {-1, 0, +1}（受 m_DirectionAssist 限制，关闭时只能保持玩家方向）
   - 跳跃: {0, 1}
   - 钩子: {保持, 松开, 按下}（受 m_HookAssist 限制）
   - 瞄准: 在玩家当前瞄准角 ± FOV 内取 AimbotSegments 个角度；Track Point 命中时把它加为候选
   - 组合后用 Randomness 做加权采样，取 Quality 个候选
3. 模拟打分
   score = m_LifeWeight      * survived_ticks
         - m_DirectionWeight * |候选方向 - 玩家意图方向|
         - m_HookWeight      * (钩子状态是否改变)
         - m_Randomness      * 探索项
   生存帧数用"与该候选输入相同的固定序列"跑满 CheckTicks，
   一旦任一帧的 ClassifyPoint(角色中心+四角) 命中相关危险位即停止。
4. 选择
   - 有方案活满 CheckTicks → 取 score 最大者，m_Override=true。
   - 全都活不满 → 若 m_Nsif 为真，取"存活最久"方案的第一步，m_UsedFallback=true；
     否则 m_Override=false。
5. 回填 m_SafeTicks / m_ScannedTicks / m_Candidates / m_Score / m_CostMs / m_aReason
```

### 6.3 物理前向模拟的两种实现路径

| 方案 | 做法 | 优点 | 风险 |
| :--- | :--- | :--- | :--- |
| **A. 轻量复刻**（推荐先做） | 复制 `CCharacterCore`，自己按 DDNet 的加速度/摩擦/重力/跳跃/钩索公式步进，碰撞用 `Collision()->MoveBox()` + `ClassifyPoint()` | 无副作用、可任意并行、不受实体系统影响 | 需要与 `CCharacter::Tick()` 的调参（tuning）严格一致，否则预测漂移 |
| **B. 沙盒克隆** | 在 `GameClient()->m_FastPractice.PracticeWorld()` 里克隆实体做真实 `GameWorld::Tick()` | 物理 100% 一致 | 有实体增删副作用、性能开销大、需要还原状态 |

> [!IMPORTANT]
> **调参必须走 `GetTuning(GetOverriddenTuneZone())` / `CCharacterCore::m_Tuning`**，
> 不能硬编码 `ms_PhysSize`、重力、跳跃加速度等常量，否则在自定义 tuning 的地图上预测会整段跑偏。

### 6.4 性能预算（50 TPS 的硬约束）

* 每 tick 预算：**≤ 1.5 ms**（否则 50 TPS 下会掉帧；参考客户端也承认 Legit 会掉帧）。
* 估算：单次模拟最长 26 帧 × 每次约 10 次碰撞查询 ≈ 260 次查询；24 个候选 ≈ 6200 次查询/tick
  ≈ 31 万次/秒。这在现代 CPU 上没问题，但**必须**：
  * 用 `time_get()` 在引擎入口/出口打点填 `m_CostMs`，UI 已显示该值；
  * 提供早期剪枝：模拟中命中危险立刻 break；
  * 绝不在引擎里做堆分配（用固定数组或 `static`/成员缓冲）。
* **`m_Telemetry.m_CostMs` 就是给你的性能仪表**，超过 1.5 ms 就要降 Quality 或加剪枝。

**阶段三实测（v1.2.0）**：最坏情况 = 玩家输入在 `kick_in_ticks` 内必死 + 8 个预测玩家 + `check_ticks = 26`，
`CAvoidLegitTest.LegitDecisionCostStaysInsideTheTickBudget` 在 `-O3` 下测得

| `bc_avoid_quality` | 实际迭代 | 每次决策 | 说明 |
| :--- | :--- | :--- | :--- |
| 1 | 1 | 0.009 ms | 只看玩家自己的输入（快而笨） |
| 24（默认） | 24 | 0.43 ms | 18 个候选全部覆盖 + 6 次探索 |
| 200 | 44~48 | 1.01 ms | 撞上 `SEARCH_BUDGET_MS = 1.0 ms` 护栏后停止 |
| 200（关掉玩家预测） | 200（迭代上限） | 0.97 ms | 一个 rollout 从 ≈23 µs 降到 ≈4.8 µs（约 5 倍） |

也就是说**每 tick 只决策一次**（`ApplyInput()` 的 `m_LastDecisionTick` 去重）的前提下，
最贵的一次决策仍在 1.5 ms 以内；而"玩家输入本来就安全"的 tick 走快路径，只花一次模拟（与 Basic 相同）。

### 6.5 必须遵守的既有约定（踩坑清单）

1. **只改 `Plan.m_Input`，不要写 `m_Controls.m_aInputData`**（见 3.2）。
2. **`Plan.m_Input` 必须从 `Ctx.m_Input` 复制**，保留 `m_PlayerFlags / m_NextWeapon / m_PrevWeapon`，
   否则会破坏菜单/聊天标记与武器切换。
3. **危险判定必须复用 `ClassifyPoint()` 与 `IsRelevantHazard()`**，不要自己另写一套探测点。
4. **冻结判定优先于一切**：`HAZ_SELF`（已经冻结）时任何方案都无意义，直接不干预。
5. **不要自动注入位移输入除非确实需要**：参考 TAS 模块的红线——飞锤/DF 场景严禁自动走动。
   避障的语义是"阻止你进入危险"，不是"帮你走图"，因此**只在玩家输入会致死时才接管**。
6. **钩子释放的实现**：`m_Hook` 在 DDNet 里是"按住"语义，松开即 `m_Hook = 0`；
   提前松钩要在"钩索落点/当前钩索目标格危险"时触发，而不是等角色贴到墙上。
7. **UI 字符串一律走 `BcLocalize()`**，并同步在
   `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt` 补齐词条；
   词条**不要用 `[` `]` 包裹**（见 TAS 文档 6.3.2 解析器陷阱）。
8. **不要新增跨 tick 的静态可变状态**用于决策（客户端可能一帧内多次预测）；
   需要历史时放进 `CAvoid` 成员并明确在 `OnReset()` 清空。
9. **决策按 tick 去重**：`ApplyInput()` 用 `m_LastDecisionTick` 保证同一个 tick 只评估一次，
   复用 `m_LastPlan`。阶段二不要把昂贵搜索放在这个去重之外。

### 6.6 建议的实现顺序（阶段二拆成 4 个小步）

1. ✅ **前向模拟器**（v1.1.0 完成）：`int CAvoid::SimulateInput(const SContext&, const CNetObj_PlayerInput&, int MaxTicks, vec2 *pOutPos, vec2 *pOutVel)`，
   用 `bc_avoid_log 1` 打印每 tick 位置与判定，与游戏内实际轨迹目测校准。
2. ✅ **方向键制动**（v1.1.0 完成，覆盖"走"与"荡"两个验收场景）：只允许改 `m_Direction`，`m_Override` 生效。
3. ⬜ **钩子释放**（覆盖"勾"场景）：允许改 `m_Hook`，验收：朝黑水墙勾过去会在贴墙前脱钩。
4. ⬜ **升级为 MCTS + NSIF + Track Point + 内置瞄准**，并把 Fentbot / Pilot 的占位参数接上。

### 6.7 Basic 代理实现说明（v1.1.0）

> 这一节记录 6.1–6.6 落实成 Basic 时**实际**做的选择，以及它留给后续档位的问题（阶段三的处理结果见 [6.8](#68-legit-代理实现说明v120)）。

**模拟器走的是路径 A（轻量复刻），但只复刻"步进"，公式仍然由引擎执行**：

```cpp
// CAvoid::SimulateInput()（avoid.cpp）
CCharacterCore Sim;            // 栈上克隆，绝不碰真实实体
Sim.Reset();                   // 清掉 m_pWorld / m_pTeams / m_HookedPlayer
Sim.SetCoreWorld(World, Collision(), Teams);
Sim.m_Pos = Ctx.m_Core.m_Pos;  // …只搬运物理状态（位置/速度/钩索/跳跃/切换/冻结/静音/tuning）
Sim.m_Tuning = Ctx.m_Core.m_Tuning;   // ← 地图 tuning，绝不硬编码
Sim.m_Id = 0;                  // 把克隆从所有队伍相关分支里摘出去
Sim.m_Input = Input;
while(Safe < Depth) { Sim.Tick(true, false); Sim.Move(); if(IsRelevantHazard(ClassifyPoint(Sim.m_Pos))) break; Safe++; }
```

* **为什么不是"自己写加速度公式"**：直接调用 `CCharacterCore::Tick()` / `Move()`，
  物理公式就永远是引擎那一份，不存在"复刻走样"的可能；需要与引擎一致的只有**状态搬运清单**。
* **为什么 `DoDeferredTick = false`**：`TickDeferred()` 会遍历 `m_pWorld->m_apCharacters[]` 并走
  `m_pTeams->CanCollide()`。克隆没有本体世界，传 `false` 同时回避了空指针和"打乱分身物理"两个坑。
  Basic 因此**不预测其他玩家**（`bc_avoid_player_prediction` 暂时不生效）。
* **克隆里的 `m_Id = 0`**：`GetMoveRestrictions()` 会调 `IsSwitchActiveCb`，而回调在 `m_pWorld`
  为空时才短路；给个合法 id 就能让开关块（switch）在模拟里也保持一致，同时不触碰队伍逻辑。
* **碰撞指针**：`CCollision` 被游戏世界、TAS 沙盒和感知层共用（`avoid.h` 里 `Collision()`），
  所以模拟和真实角色跑在完全相同的几何上；`Reset()` 之后即使指针缺失也只会退化成无碰撞推演。
* **危险判定**：每一帧都用 `IsRelevantHazard(ClassifyPoint(Sim.m_Pos))`，探测点与
  `CCharacter::HandleTiles()` / `HandleSkippableTiles()` 逐条一致（见 4.2），
  所以"感知说危险"和"物理说危险"不可能错位。

**决策规则（`EvaluateBestPlan()`，Basic 只改 `m_Direction`）**：

| 顺序 | 条件 | 结果 |
| :--- | :--- | :--- |
| 0 | `bc_avoid_debug_override` | 强制接管并反转左右键（阶段一自检开关，优先级最高） |
| 1 | 无地图 / `HAZ_SELF`（已冻结） | 不干预，理由写进 HUD（`no map data` / `frozen, agent idle`） |
| 2 | **感知半径内没有任何相关危险**（`m_Threat.m_HasNearest == false`） | **不干预**（`no hazard within sensing range`）——见下方"感知半径的作用" |
| 3 | 玩家自己的输入能活满 `CheckTicks` | **不干预**（`player input safe`） |
| 4 | 玩家输入还能活 ≥ `KickInTicks` | **不干预**（`still time before the hazard`） |
| 5 | 候选 `{-1, 0, +1}`（玩家方向恒在集合内；`direction_assist` 关闭时只剩玩家方向） | 逐个模拟 `CheckTicks` 帧 |
| 6 | 有候选活满，**或**活得更久且方向不同 | **接管**，写 `m_Direction`，`m_Override = true` |
| 7 | 最好的也活不满 | `m_Override = true` + `m_UsedFallback = true`（NSIF，红徽章） |
| 8 | 其他（并列、玩家方向已是最优） | **不干预**（`no safer direction`） |

**感知半径的作用（`bc_avoid_sensing_radius`，**半格单位**，0.5 ~ 16 图块 = 配置值 1 ~ 32）：**

模拟器本身**没有**距离上限——它把 tee 一帧帧往前推，推到哪里就查哪里的 `ClassifyPoint()`。
这让物理预测很精确，但也意味着**如果不加限制，代理会去躲感知范围之外的十万八千里外的危险**。
所以决策引擎的第 0 层（上表第 2 行）显式用感知层的结果做闸门：

* `ScanThreat()` 只会对通过 `IsRelevantHazard()`、**并且到危险图块盒的欧氏距离 ≤ 半径**的图块
  置 `m_HasNearest`，所以这一个标志同时折叠了**感知半径**与全部 `bc_avoid_tile_*` 开关；
* 半径越大 → 越早发现 → **刹车点离危险越远**；半径越小 → 越晚介入；
* 闸门同时受 `KickInTicks` 约束：**它只决定"能不能开始管"，不决定"管多久"**。
  半径很大时，真正拦住代理的是 `check_ticks` 的前瞻窗口，
  所以半径超过约 8 图块后，再往上调在平地上就感觉不出差别了（这点已被测试固定下来）。

**单位与配置迁移（v1.2.1）**

| 项 | v1.1.1 及以前 | v1.2.1 起 | v1.2.5 起（默认值） |
| :--- | :--- | :--- | :--- |
| 配置项单位 | 图块（整数） | **半格**（`12` = 6.0 图块） | 半格（`2` = **1.0 图块**） |
| 范围 / 默认 | 2 ~ 16 / 6 | 1 ~ 32 / 12 | **1 ~ 32 / 2** |
| 到达判定 | 图块索引正方形 ±r 内**存在**危险图块（对角方向会多覆盖 ≈41%） | 危险图块盒到 tee 中心的**欧氏距离** ≤ r |
| 界面 | `Sensing radius` + `tiles` | `半径（半格）` + 面板提示说明 `12 = 6 格` |

> [!IMPORTANT]
> **旧配置会被自动换算一次**：`cl_config_version` 从 1 升到 2，启动时把旧的"图块"值 `×2`
> （6 → 12）。全新配置的默认值本身就是 12、版本本身就是 2，所以不会重复翻倍。
> 唯一需要手动处理的是**自己写在 `autoexec.cfg` 里的旧值**：那行不会被再次换算，
> 升级后请按新单位改成两倍（6 → 12）。

实测（`CAvoidSensingRadiusTest`，合成地图、平地全速行走 10 px/tick、`check_ticks = 26`；
下表是**水平方向**的到达距离——正前方同行的危险块，`dy = 0` 时椭圆退化成横向半径；
"到达距离" = 传感器发现危险那一刻，tee 中心到危险图块边缘的距离）：

| 配置值（半格） | 半径（图块） | 到达距离 | 折算 | 结果 |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 0.5 | 10 px（0.3 格） | 1 tick | **刹不住**（"几乎关闭"档，见下） |
| 2 | 1.0 | 30 px（0.9 格） | 3 tick | 刹停，余量很小 |
| 3 | 1.5 | 40 px（1.2 格） | 4 tick | 刹停 |
| 4 | 2.0 | 60 px（1.9 格） | 6 tick | 刹停（≈ v1.1.1 的半径 2） |
| 6 | 3.0 | 90 px（2.8 格） | 9 tick | 刹停 |
| 8 | 4.0 | 120 px（3.7 格） | 12 tick | 刹停 |
| 12 | 6.0 | 190 px（5.9 格） | 19 tick | 刹停（≈ v1.1.1 的半径 6） |
| 16 ~ 32 | 8 ~ 16 | 240 px（7.5 格） | 24 tick | 不再增长，改由 `check_ticks` 决定 |

> [!NOTE]
> **出厂默认是 2（= 1 图块，30 px，约 3 帧）**：这是 v1.2.5 定的默认手感——**平时完全不打扰，只在最后一刻动一下**。
> 代价要说清楚：1 格的距离不够刹停（刹停要 5 帧 / 25 px），所以默认档更多依赖"最后关头的跳跃/松钩"，
> 而不是"提前减速"。想要"提前刹停"的稳感就把滑条调到 **4（2 格）以上**（台架：半径 4 格起就以刹车为主、不再跳跃）。
> **已有配置不会被改**：迁移只做过一次，老配置保留自己的值；想拿到新默认请点面板标签栏右端的 **恢复默认**。

> [!IMPORTANT]
> **下限 0.5 是"几乎关闭"档**：以默认 tuning（`ground_control_speed = 10`、
> `ground_control_accel = 2 px/tick²`）全速前进时，从发现到撞上只剩 ~1 帧，
> 而刹停需要 5 帧 / 25 px，所以这个档位**救不回来是预期行为**——它的用途是
> "我只想在自己已经贴到边缘时被碰一下"。仍然能刹停的最小档位是 1 格（配置值 2）。
> 两条都已被 `HalfATileIsTheAlmostOffEnd` 与 `HalfTileStepsAreRealSteps` 固定下来。

* **打分**：`score = m_LifeWeight * 存活帧数 - m_DirectionWeight * |候选方向 - 玩家方向|`。
  生存是硬指标，方向权重只在"活一样久"时决定谁更省事；**并列时永远选玩家自己的方向**，
  这样"宁可少管不多管"是结构上保证的，不是靠调参。
* **钩子 / 跳跃 / 瞄准 / 武器字段**：整份 `Ctx.m_Input` 原样复制后只改 `m_Direction`，
  所以 `m_PlayerFlags` / `m_NextWeapon` / `m_PrevWeapon` / `m_WShots` 等全部保留（见 6.5 第 2 条）。
* **性能**：每 tick 最多 `1 + 3 = 4` 次模拟 × `CheckTicks`（默认 26）帧。`Plan.m_CostMs` 用
  `time_get()` 真实测量并直接显示在 UI 上。离线的**最坏情况基准**（`CAvoidSimulatorTest.WorstCaseDecisionCostStaysInsideTheTickBudget`，
  4 个候选 × 26 帧 × 2000 次决策）实测 **约 0.02 ms / 决策**（`-O3`，单候选约 0.006 ms），
  相对 1.5 ms 的每 tick 预算还有 60 倍以上余量（Release 构建下的一般结论；Debug 构建会明显更慢）。
  决策路径里没有任何堆分配：候选样本放在定长成员数组 `m_aSamples[MAX_CANDIDATES]` 里，克隆体在栈上。

**调试与校准**：

* `bc_avoid_log 1`：每个决策 tick 打印一行
  `tick / pos(图块) / vel / threat / hazard / player safe N/CheckTicks / override / safe / reason`。
  沿着黑水边走边看 `player safe` 掉到多少，就能判断模拟器与肉眼轨迹是否一致。
* `bc_avoid_show_visuals 1`：世界里画出感知半径、危险图块与最近威胁连线，用来对照日志里的坐标。
* `ninja -C build testrunner && ./build/testrunner --gtest_filter='CAvoidSimulatorTest.*'`：
  3 个回归用例，锁住"克隆步进 == 引擎步进"、"真实地图上可复现"、"物理跟随地图 tuning"。

**已知不足（阶段三的处理结果）**：

1. ~~不预测其他玩家~~ → **v1.2.0 已在 Legit 里实现**（私有 `CWorldCore` + 影子快照，见 [6.8](#68-legit-代理实现说明v120)）；Basic 仍然不预测（忠于阶段二验收）。
2. ~~不松钩~~ → **v1.2.0 已在 Legit 里实现**（松钩候选的固定序列必须活满前瞻，见 [6.8](#68-legit-代理实现说明v120)）。
3. 候选集只有三个方向键：Basic 保持不变（这是它的定义）；Legit 已扩展到方向 × 跳跃 × 钩子，**瞄准仍然没有**（属于阶段四 Blatant）。
4. 前瞻从**上一 tick 的核心状态**（`m_aClients[].m_Predicted`）出发，比当前渲染帧晚 1 tick。
   v1.2.0 复核结论：松钩只依赖"速度 + 钩索锚点 + 输入"，20 ms 的状态滞后对 26 帧窗口的判据没有影响；
   如果以后要做逐帧精确的落点搜索（阶段四），需要把状态换成当前帧。
5. `Randomness` / `Quality` / `HookWeight` 对 Basic 仍然无效（Basic 是确定性穷举，忠于参考实现）；对 Legit 全部生效。

### 6.8 Legit 代理实现说明（v1.2.0）

> 这一节记录阶段三（钩子释放 + 跳跃 + UCT + 其他玩家预测）落实成代码时**实际**做的选择、
> 每条判据、与真实物理的偏差，以及留给阶段四的问题。
> 全部规则都有离线用例钉住：`src/test/avoid_legit_test.cpp`（15 个用例，
> `./build/testrunner --gtest_filter='CAvoidLegitTest.*'`）。
> **"拟真"做到什么程度算合格** 另有一份可执行的评分卡：
> [`AVOID_LEGIT_ACCEPTANCE.md`](AVOID_LEGIT_ACCEPTANCE.md)（五条设计原则 + A~E 类判据 + 三分钟手感流程 +
> 当前未达标项）。

#### 6.8.1 为什么把引擎拆出去

`CAvoid` 是 `CComponent`，只能链进客户端；`testrunner` 只链 `game-shared`。
所以决策引擎、感知层与模拟器都搬到了 `avoid_engine.h/.cpp`（`namespace Avoid`），只依赖
`CCollision` / `CWorldCore` / `CTeamsCore` —— 这三样测试里都能自己造。
`CAvoid` 保留的东西一行没少：状态机、配置快照、输入管线、HUD、可视化、控制台命令、Basic 算法。

* 数据契约（`SSettings` / `SThreat` / `SContext` / `SInputPlan`）**字段与 v1.1.1 完全一致**，只是换了文件；
  `avoid.h` 用 `using SSettings = Avoid::SSettings;` 等别名继续提供 `CAvoid::SSettings` 这些名字，
  所有既有调用点（菜单、HUD、测试）不需要改。
* 危险判定只有一份实现：`Avoid::ClassifyPoint()` / `Avoid::IsRelevantHazard()`；
  `CAvoid::ClassifyPoint()` / `CAvoid::IsRelevantHazard()` / `ScanThreat()` 都是转调。
* 模拟器只有一份实现：`Avoid::SimulateFixed()`（固定输入）与 `Avoid::SSimState`（可逐 tick 喂输入）；
  `CAvoid::SimulateInput()` 是它的薄包装（[附录 F.2](#f2-每档必须留下的地基验收时会检查) 的硬性要求）。

#### 6.8.2 候选空间

```
方向(3) × 跳跃(2) × 钩子(3) = 最多 18 个候选（MAX_ROOT_ACTIONS = 24 留余量）
```

* **玩家自己的取值恒在集合内，并且排在最前面**：`方向 = {玩家方向, 其余}`、`跳跃 = {玩家值, 另一值}`、
  `钩子 = {玩家值, 其余}`。所有并列都由"先生成的候选赢"，所以"什么都不改"在结构上永远优先——
  "宁可少管，不要多管"不是调参调出来的。
* `bc_avoid_direction_assist = 0` → 方向只剩玩家自己的；`bc_avoid_hook_assist = 0` → 钩子只剩玩家自己的。
  跳跃没有单独的开关，它受身法门（6.8.6）约束。
* 每个候选的**偏离代价**（只用于在"活一样久"之间取舍，以及 `bc_avoid_life_weight` 的显式交换）：

  ```
  Deviation = DirectionWeight * |候选方向 - 玩家方向|
            + HookWeight      * (钩子是否改变)
            + 60              * (跳跃是否改变)      // 跳跃没有 cvar，见下面的说明
  ```

  跳跃权重没有对应的 `bc_avoid_*`（33 个 cvar 的数量与语义不许改），所以用内部常量 `JUMP_DEVIATION_WEIGHT = 60`
  （0 = 不改，200 = 最大滑块值，60 表示"跳一下比改方向便宜、比什么都不做贵"），定义在 `avoid_engine.cpp`。

#### 6.8.3 搜索：UCT

```
quality    = 迭代次数（1..200）
randomness = UCT 探索常数 c = randomness / 100，同时决定 rollout 分支概率 p = randomness / 400
```

* **迭代 0 永远是玩家自己的输入**，它同时就是 Basic 的快路径与 `kick_in_ticks` 前置闸门：
  `>= check_ticks` → 不干预（`player input safe`）；`>= kick_in_ticks` → 不干预（`still time before the hazard`）。
  所以"玩家输入安全"的 tick 只花一次模拟，与 Basic 完全同价。
* **首次访问某个候选 = 固定候选序列**（该动作重复整段前瞻，命中危险立刻剪枝），记为 `CanonicalSafe`。
  `quality >= 18` 时 18 个候选各被访问一次 → 等于一次完整穷举；`quality = 1` 时只有迭代 0 会发生，
  于是"快而笨"（永远不会干预）与"慢而准"是同一套代码的两个端点。
* **再次访问 = 探索 rollout**：tick 0 仍然是该候选动作，tick 1 起以概率 `p` 切到随机候选。
  序列在 rollout 前抽好，不根据模拟状态重规划（"固定候选序列"）。
* UCT 选择：`平均分 + c * sqrt(ln(N+1)/n)`，其中 `平均分 = LifeWeight * 平均存活帧 - Deviation`。
* **最终排序用的是 `RankingScore = LifeWeight * CanonicalSafe - Deviation`，不是平均值。**
  这是本阶段最重要的一条设计决定：探索 rollout 的乐观结果**不能**替代"这个动作重复下去会怎样"的保守估计，
  否则"提前松钩"会退化成"等下一帧再救"（验收第 1 条要求的就是提前量）。
  `Randomness` 因此只改变搜索过程与上报的 `Plan.m_Score`（MCTS 回报），不会让计划变得更冒险。
* 资格与接管：候选必须 `CanonicalSafe >= PlayerSafe`（不许比玩家自己的输入更差）；
  赢家还要 `CanonicalSafe >= Horizon`（正常接管）或 `CanonicalSafe > PlayerSafe && bc_avoid_nsif`（NSIF 兜底，红徽章）。
  赢家是迭代 0（玩家自己的输入）时一律不接管。
* **确定性**：`CPrng` 是成员，每次决策用 `(tick, 常量)` 播种；决策路径里没有 `rand()` / 全局 RNG。
  同一个 tick、同一个迭代预算 → 同一个计划（`SearchIsDeterministicPerTick`）。
  ⚠️ 唯一例外是那道 1.0 ms **墙钟护栏**：机器很忙时它可能提前收尾，于是迭代次数（极端情况下连计划）
  会随负载变化。在意这一点的场景（离线测试、回放）请把 `quality` 压在护栏以下。
* **无堆分配**：候选表、克隆体、影子数组都是定长成员数组；唯一的 `std::vector` 是影子世界的
  `m_vSwitchers`（真实地图的开关状态拷贝），容量跨决策复用，第一次之后不再分配。

#### 6.8.4 钩子释放的判据（验收第 1 条）

**判据不是"钩索落点是否危险"，而是模拟结果**，三步缺一不可：

1. 玩家自己的输入（按住钩、按当前方向）跑满前瞻 → 死在第 k 帧（`PlayerSafe < kick_in_ticks`）；
2. 某个"松开钩子"的候选（`m_Hook = 0`，方向 / 跳跃任取）的固定序列跑满前瞻 → 活满 `check_ticks`；
3. 它的 `RankingScore` 最高——松钩要付 `HookWeight` 的偏离代价，所以只有"确实更活"才赢得过
   "保持钩 + 反向减速"。

三条都满足才会写 `Plan.m_Input.m_Hook = 0`，理由是 `release hook before hazard`
（中/俄本地化词条由 [附录 D](#附录-d回归自检脚本) 的自检脚本逐个校验）。

**反向用例同样是硬约束**：如果松开钩子会让角色带着当前速度飞进危险，那么所有松钩候选的
`CanonicalSafe` 都更小，永远输给"保持钩 + 刹停"，表现为 `m_Hook` 保持玩家值。
`ReleasingTheHookIsRefusedWhenItWouldFlyIntoTheHazard` 用例除了断言这一点，还**逐个方向**断言
"松钩候选确实会死"——既防止"为了松钩而送死"，也防止测试本身退化成空转。

#### 6.8.5 其他玩家预测（`bc_avoid_player_prediction`）

走的是任务书里的**第二条路（完整 `CWorldCore`）**：

* **采集**（`CAvoid::BuildEnvironment()`）：感知半径 + 2 图块内最近的 ≤8 个其他 tee
  （`m_aClients[].m_Predicted`），跳过自己、观战者与未激活的 client。
* **注入**（`Avoid::SSimState::Init()`）：克隆体拿到一个**私有** `CWorldCore`，
  `m_apCharacters[id]` 指向影子数组里的 `CCharacterCore` 快照；克隆自己坐在真实的 client id 上，
  于是 `CanCollide()` / `CanKeepHook()` / 开关门（`IsSwitchActiveCb`）用的都是真实队伍与真实地图状态。
* **推进**：每个 rollout tick，影子按快照速度匀速平移（`m_Pos += m_Vel`）。
* **为什么不用真实世界**：`TickDeferred()` 会写它找到的 core（钩索拖拽力），还会调用 antiping 回调——
  那会污染客户端自己的预测。私有世界同时让"被队友撞进危险""钩索拖向队友"能在克隆里真实发生。
* **开关门保真**：真实 `CWorldCore::m_vSwitchers` 会被拷进私有世界（容量复用，不分配），
  否则克隆会穿过玩家过不去的门。
* **偏差（写清楚）**：影子不模拟自己的物理（不受重力 / 墙体 / 钩索影响，会穿墙）、最多 8 个
  （第 9 个玩家起完全不参与预测）、位置来自上一 tick 的预测核心（约 20 ms 滞后）、
  `cl_prediction_margin` 仍需按参考实现设置（见 [F.4](#f4-已知的配置陷阱写进用户文档也写进下一档任务书)）。
  `bc_avoid_player_prediction = 0` 时**完全不注入**，模拟与 v1.1.1 逐位一致（有用例断言
  "关掉预测"与"场上没有任何其他玩家"给出完全相同的计划与理由）。

#### 6.8.6 身法状态门（TAS 红线 3.11.7 的显式化）

`Avoid::ClassifyMovement()` 把三种"默认不动手"的状态显式化，`CAvoid::EvaluateBestPlan()` 在
**感知半径闸门之后、任何代理运行之前**统一判定，并让 `Plan.m_aReason` 说得出来：

| 状态 | 判定 | 理由字符串 | 为什么 |
| :--- | :--- | :--- | :--- |
| 喷气背包 | `CCharacterCore::m_Jetpack` | `jetpack, hands off` | 位移语义特殊，方向键不是"走路" |
| 钩住玩家 | `HookedPlayer() != -1` | `hooked to a player, hands off` | 双人互动身法，拽钩/被拽时改方向会拆掉节奏 |
| 飞锤 / 深飞 | `Client()->DummyConnected() && cl_dummy_hammer && 本体不在落地状态` | `fly hammer, hands off` | HDF 的开关就是 `cl_dummy_hammer`，DF 的 bind 在开火时也会把它置 1；落地状态下不算在飞 |

这一层对**所有代理**生效（含 Basic）。Basic 的三条验收（走 / 荡 / 窄通道）都不涉及这三种状态，
所以没有退化；分身连接本身仍然永远不碰（`OnSnapInput` 的 `!Dummy` 分支）。

#### 6.8.7 解冻块专用前瞻（`bc_avoid_unfreeze_ticks`）

条件：`bc_avoid_tile_unfreeze` 打开，且**最近威胁**是解冻块。
"最近威胁是不是解冻块"用 `ClassifyPoint(Ctx.m_Threat.m_NearestPos)` 现场分类得到
（`m_NearestPos` 就是最近危险图块的中心，见 [4.4](#44-数据结构)），因此不需要给 `SThreat` 加字段。
满足条件时前瞻从 `check_ticks` 换成 `bc_avoid_unfreeze_ticks`：解冻块不致命，
窗口更短 → 代理在它旁边明显更不神经质。用例：
`UnfreezeTicksSetTheLookaheadNearUnfreezeTiles`（同一局面，`unfreeze_ticks = 8` 不干预、`= 26` 干预）。

#### 6.8.8 参数接线与验证（每一条都有"改了就一定有可观察差别"）

| 参数 | 在 Legit 里改了什么 | 证据 |
| :--- | :--- | :--- |
| `bc_avoid_quality` | 迭代次数（候选覆盖 + 探索深度） | `QualityDrivesSearchEffortAndTheDecision`；游戏内观察 `plans` 与 `cost` |
| `bc_avoid_randomness` | UCT 探索常数 + rollout 分支概率 | `RandomnessChangesTheSearchWithoutBreakingIt` |
| `bc_avoid_direction_weight` | 偏离代价里的方向项 | `QualityDrives*` / 手感：拉满更贴玩家意图 |
| `bc_avoid_hook_weight` | 偏离代价里的钩子项（松不松钩） | `HookWeightChangesTheHookDecision` |
| `bc_avoid_life_weight` | 存活与偏离的交换比 | `HookWeightChanges*` 用它做低生命优先级的对照组 |
| `bc_avoid_hook_assist` | 候选里有没有"松开/按下钩子" | 关掉后 `Plan.m_Input.m_Hook` 恒等于玩家值 |
| `bc_avoid_direction_assist` | 候选里有没有别的方向 | 关掉后 `Plan.m_Input.m_Direction` 恒等于玩家值 |
| `bc_avoid_player_prediction` | 有没有注入影子玩家 | `PlayerPredictionChangesTheDecision` |
| `bc_avoid_unfreeze_ticks` | 解冻块旁边的前瞻长度 | `UnfreezeTicksSetTheLookaheadNearUnfreezeTiles` |
| `bc_avoid_nsif` | 活不满前瞻时是否还兜底 | 关掉时"没有安全解"的局面完全不干预 |
| `bc_avoid_check_ticks` / `kick_in_ticks` | 前瞻窗口与介入阈值 | 快路径（与 Basic 同一套语义） |
| `bc_avoid_sensing_radius` | 介入距离闸门（在 `CAvoid` 里，所有代理共用）；半格步进，0.5 起 | `HalfATileIsTheAlmostOffEnd`、`HalfTileStepsAreRealSteps`；游戏内：[8.3](#83-basic-代理验收v110-起) 第 8 行 |
| `bc_avoid_tile_*` | 什么算危险（感知与模拟共用一份 `IsRelevantHazard`） | [8.2](#82-危险感知层验证) |

`avoid_status` 现在会在 `kick_in_ticks >= check_ticks` 时打印一条配置警告（[F.4](#f4-已知的配置陷阱写进用户文档也写进下一档任务书) 第 1 条）。

#### 6.8.9 已知不足（留给阶段四 / 阶段五）

1. **影子玩家不模拟自己的物理**（匀速平移、可穿墙），预测窗口越长偏差越大；
   阶段四如果要做落点级瞄准搜索，需要在影子上跑一份轻量物理或改用沙盒克隆。
2. **最多 8 个影子**：拥挤服务器上第 9 个玩家起完全不参与预测（宁可漏判也不超预算）。
3. **没有瞄准**：按下钩子用的是玩家当前瞄准方向，没有 Track Point / 内置瞄准（阶段四）。
4. **探索 rollout 不参与最终排序**（6.8.3 的保守性理由）；`Randomness` 只影响搜索过程与
   `Plan.m_Score`，不会让计划变冒险。如果阶段四想要"多条序列里挑最好的"，必须同时给出
   "第一条动作本身安全"的证据，否则验收第 1 条的提前量会退化。
5. **`bc_avoid_nsif` 在 Legit 里生效，Basic 里仍然忽略**（保持阶段二验收行为不变）——
   这是有意为之的不一致，写在这里以免被当成 bug。
6. `Plan.m_Candidates` 的语义在 Legit 下是"本次搜索的迭代次数（rollout 数）"，
   在 Basic 下是"评估的候选数"；两者都符合 [6.1](#61-函数签名与契约) 的"本帧评估的方案数"。
7. **不预测角色传送落点、不模拟 heart tile、没有参考实现那层独立的"预测开关"** ——
   逐条对照见 [6.8.10](#6810-与参考实现krx的参数机制对照)。
8. **"坠落时自动出勾挂墙"还没做**：现在只有在玩家自己的瞄准方向恰好指向可勾墙面时，
   "按下钩子"候选才可能救回来；要稳定做到需要**瞄准方向搜索**（属于阶段四 Blatant 的
   Track Point / 内置瞄准），见 [6.8.11](#6811-跳跃策略与椭圆感知v122) 的说明。

#### 6.8.11 跳跃策略与椭圆感知（v1.2.2）

> 来源：用户实测反馈 —— ① 勾向"带黑水的竖直墙"时代理竟然起跳；② 走向向下的水坑时，
> 还没到真正危险就跳；③ 感知范围是圆的并不合理，因为水平速度通常大于竖直速度。

##### 跳跃是最后手段，不是反射

两条规则（`CPlanner::UpdateAvailability()` / `JumpEngaged()`，常量 `JUMP_URGENCY_TICKS = 6`）：

| 规则 | 判定 | 效果 |
| :--- | :--- | :--- |
| **钩索期间不加跳** | `m_Input.m_Hook != 0` 或 `m_HookState` 是 `HOOK_GRABBED` / `HOOK_FLYING` | 所有"把跳跃键从 0 变 1"的候选被标为不可用：绳索问题只能用绳索解决（松钩 / 按钩 / 方向键） |
| **只在危急关头跳** | 玩家自己输入的存活帧数 `PlayerSafe > 6` | 同上：代理宁可这一帧什么都不做，把跳跃留到"再不动手就死"的时刻 |

* 只拦"**新加**跳跃"，玩家自己按住的跳跃、以及"松开跳跃"都不受影响；
* 被拦下的候选不参与搜索也不参与排序（`m_Available == false` 的根既不会被访问，也不会被 `FindBest` 选中），
  所以它们**不可能**被误选；
* 如果这一帧唯一的出路本来是跳跃，`Plan.m_aReason` 会明确写
  `没有更安全的方案（跳跃留作最后手段）`，日志里 `plan d/j/h` 的 `j` 保持玩家值。

实测（`CAvoidLegitTest`，自由落体掉向死亡地板，只有空中跳能活）：

| 局面 | 结果 |
| :--- | :--- |
| 距离死亡 12 帧 | 不接管，理由 `no safer plan (jump is a last resort)` |
| 距离死亡 4 帧 | 接管，`m_Jump = 1`，活满 26 帧 |
| 同样 4 帧 + 玩家按住钩（锚点在 46 px 内、无拖拽力） | 不接管、不加跳（纯策略差异，物理完全相同） |

> 为什么"跳"以前会赢：三个权重是偏离代价，而跳跃的内部代价只有 60，比"改方向(100)"和"改钩子(100)"都便宜，
> 于是在"跳也能活满、松钩+刹车也能活满"的局面里，搜索会选最便宜的**跳**。现在结构性规则把它压到最后一档，
> 不再依赖某个权重值的大小。

##### 感知范围：椭圆，而不是圆

```
ReachX = bc_avoid_sensing_radius × 32                     （用户滑块，0.5 ~ 16 图块，半格步进）
ReachY = ReachX × 垂直系数
垂直系数 = clamp( 0.5 · gravity · check_ticks² / (ground_control_speed · check_ticks), 0.25, 1 )
         = 0.65（默认 tuning，check_ticks = 26）
```

* 物理含义：同一个前瞻窗口里，横向最多走 `v·T`，竖直方向被重力限制在 `½·g·T²`，
  所以"同样远"的上下危险其实**更晚**才会致命，感知范围就该是扁的；
* 每轴距离独立计算（`Avoid::TileBoxDelta()`），所以水平与竖直互不干扰：正下方的坑不会因为"横向很近"而被算进来；
* 系数取自**地图 tuning**（不是硬编码），`tune` 图上重力不同，椭圆自动不同；
* `bc_avoid_show_visuals 1` 画的感知环也是同一个椭圆（同一个 `Avoid::SensingVerticalFactor()`），
  高亮图块的范围与传感器完全一致。

实测（`CAvoidLegitTest.SensingReachIsFlatterVertically`，半径 6 图块 → `ReachX = 192 px`、`ReachY = 125 px`）：

| 距离 | 方向 | 是否进入感知 |
| ---: | :--- | :--- |
| 150 px | 正侧方 | ✅ 看见（150 < 192） |
| 150 px | 正下方 | ❌ 看不见（150 > 125） |
| 75 px | 正下方 | ✅ 看见 |

##### 还没做的那一半：坠落时的"自动出勾挂墙"

用户描述的完整期望是"断勾 → 方向键阻止 → 若玩家仍下坠，则到危急关头自动出勾挂到黑水外的墙"。
现在已经做到：断勾优先、方向键制动、危急关头才动用跳跃、"按下钩子"候选本身也在候选集里。
**缺的是瞄准**：按下钩子用的是玩家当前的瞄准方向，代理不会自己找墙。
要稳定实现需要在候选里加入**瞄准角采样 + 可勾性判定**（射线打到 `TILE_NOHOOK` 之外的实心面才算数），
那是阶段四 Blatant 的 Track Point / 内置瞄准的范畴（[附录 F.1](#f1-分档总表)），
而且会让候选数从"18 个根动作"变成"18 × N 个角度"，必须先重新评估预算（现在是 1.0 ms 搜索护栏）。

#### 6.8.10 与参考实现（KRX）的参数—机制对照

参考实现闭源，我们只有 [`docs/avoid/`](avoid/avoid.md) 的文档与其中泄露的 cvar 名。
下面把"从参数名与文档原文能推断出的底层机制"逐条对照到本实现，并明确**哪些是相同的、哪些是推断、哪些是已知差异**。
这张表也是阶段四的起点：任何"参考实现里能做、我们没做"的东西都写在这里，而不是靠猜。

| 参考参数（文档原文要点） | 能推断出的底层机制 | 本实现 | 判定 |
| :--- | :--- | :--- | :--- |
| `krx_avoid_num_iterations`（Quality）"Adjusts the number of simulations (iterations) the bot runs" | 每次决策的迭代次数（MCTS） | `bc_avoid_quality` = UCT 迭代次数 | ✅ 相同语义；**差异**：本实现另有 1.0 ms 墙钟护栏（见下） |
| `krx_avoid_tile_exploration_constant`（Randomness）"balance between exploring new move sequences and exploiting known good ones (related to the UCT exploration parameter)" | UCT 探索常数 **+ 对"新动作序列"的探索** | `bc_avoid_randomness` = UCT 常数 `c` **且** rollout 分支概率 `p` | ✅ 相同语义（文档那句"exploring new move sequences"正好对应分支概率） |
| `krx_avoid_tile_lifespan_weight`（Life Priority）"how much weight is given to simply surviving longer versus matching player input" | 打分里的生存项 | `life_weight × 存活帧` | ✅ 相同 |
| `krx_avoid_tile_direction_weight`（Direction Priority）"how strongly the bot prefers maintaining the player's intended movement direction" | 方向偏离惩罚 | `direction_weight × \|Δ方向\|` | ✅ 相同 |
| `krx_avoid_tile_hook_weight`（Hook Priority）"how strongly the bot prefers maintaining the player's current hook state (hooked or not hooked)" | 钩子状态偏离惩罚 | `hook_weight × 钩子是否改变`；`HookWeightChangesTheHookDecision` 证明它真的翻转"松不松钩" | ✅ 相同 |
| Hook Assistance "activates hook inputs (allows the bot to use hook/**unhook** actions)" | 候选空间含按钩与**松钩** | 候选钩子维 `{玩家值, 松, 按}`（`hook_assist` 关闭时只剩玩家值） | ✅ 相同 |
| Direction Assistance "activates directional inputs (left/right/neutral)" | 候选空间含三方向 | 候选方向维 `{玩家方向} ∪ {-1,0,+1}` | ✅ 相同 |
| Check Ticks "how far into the future (in ticks) the bot simulates potential moves to evaluate their safety" | 逐 tick 前向模拟的前瞻长度 | `bc_avoid_check_ticks` = Horizon（逐 tick 步进真实 core 物理） | ✅ 相同 |
| Player Prediction "predicts the movements of other players…can increase performance cost" + `krx_predictionplayers`"Player Loop (Collision): predicts character-to-character collisions" | **角色互撞**的角色循环（不是静态障碍） | 私有 `CWorldCore` + 影子 `CCharacterCore`，`TickDeferred()`/`Move()` 里真实的互撞与钩索拖拽 | ✅ 同一条技术路线；**差异**：最多 8 个影子 |
| `krx_predictionmoverestriction`"predicts tiles that restrict movement (stoppers)…disabling makes them unaware of stoppers" | 前向模拟里跑真实 stopper 规则 | 克隆体直接调 `CCharacterCore::Tick()` → `GetMoveRestrictions()`（含开关门回调） | ✅ 相同（我们没有独立开关，等于参考文档推荐的默认"常开"） |
| `krx_predictiondeathtile`"predicts death tiles for bots…disabling ignores death tiles" | 每个模拟帧查询死亡图块（并作为性能杠杆） | 每个 rollout 帧 `ClassifyPoint()`；开关是 `bc_avoid_tile_death` | ✅ 效果相同；**粒度不同**：参考是"预测层开关"，我们是"危险类型开关" |
| Teles / Death / Unfreeze 图块 + Unfreeze Ticks "adjusts the lookahead duration (in ticks) specifically for checking unfreeze tiles" | 危险图块分类 + 解冻块专用前瞻 | `bc_avoid_tile_*`（感知与模拟共用 `IsRelevantHazard()`）+ 最近威胁是解冻块时 Horizon 换成 `bc_avoid_unfreeze_ticks` | ✅ 相同 |
| Afk Protection / Afk Time | 挂机自动解除代理 | 阶段一已实现，未改 | ✅ 相同 |
| `cl_prediction_margin` 建议值 | 与算法无关的客户端预测余量 | 同样适用（[F.4](#f4-已知的配置陷阱写进用户文档也写进下一档任务书) 第 3 条） | ✅ 相同 |

**已知差异（有意为之或尚未实现）**

| # | 差异 | 说明 | 归属 |
| :--- | :--- | :--- | :--- |
| 1 | **1.0 ms 墙钟护栏** | 参考文档自己承认 Legit 会掉帧（FAQ 让人降 Quality / Check Ticks），说明它没有硬预算；本模块的 1.5 ms/tick 是硬约束，所以 `quality = 200` 实测只跑到 44~48 轮就用当前结果收尾 | 有意为之（[6.4](#64-性能预算-50-tps-的硬约束)） |
| 2 | **玩家预测上限 8 个 + 位置晚一 tick** | 参考按玩家数线性涨价（人多就掉帧）；我们取最近的 8 个，宁可漏判也不超预算 | 有意为之（[6.8.5](#685-其他玩家预测bc_avoid_player_prediction)） |
| 3 | **不预测角色传送落点** | 参考的 `krx_predictionteleports` 打开时会预测传送目的地（关闭时"把传送块当冻结块"）。本实现的克隆体只跑 `CCharacterCore`，而**角色传送在 `CCharacter::HandleTiles()` 里**，所以克隆不会传送；传送块目前只是可选的危险类型（`bc_avoid_tile_tele`，默认关）。只有**钩索传送**（`TILE_TELEINHOOK`）因为写在 `CCharacterCore::Tick()` 里而被模拟 | 待定（阶段四/五可补：在 `SSimState::Step()` 里加一层传送落点解析） |
| 4 | **不模拟 heart tile（拾取块）** | 参考有 `krx_predictionheart`；对避障影响很小（会让某些图的拾取块引发冻结），但没有建模 | 待定 |
| 5 | **预测层开关粒度** | 参考把"死亡块/位移限制/玩家循环/传送/heart"做成一层独立预测开关；我们的 33 个 cvar 冻结，只在**危险类型**层面提供开关（`tile_*`），stoppers 与玩家互撞永远预测（等于参考的推荐默认） | 有意为之（cvar 数量冻结） |
| 6 | **NSIF 的归属** | 参考文档把 NSIF 放在 Blatant（Legit 面板没有）；本实现让 Legit 也读 `bc_avoid_nsif`（默认开 = 活不满时按最优第一步兜底），Basic 仍按 v1.1.1 忽略它 | 需要你确认是否符合预期 |
| 7 | **搜索的树形态不可知** | 参考的参数名带 `krx_avoid_tile_` 前缀，但同前缀的 `krx_avoid_tile_auto_drag` 是个布尔功能开关（Blatant 的 Auto Drag），所以这**是"避障（图块）子系统"的命名空间前缀，不能据此推断 MCTS 的节点是 tile 粒度**。本实现是"根动作树 + 按 tick 展开的序列 rollout" | 无法从文档验证 |

> **结论**：能从参数名与文档推断出来的机制，本实现都有对应物，参数语义一一对齐，且"角色互撞的角色循环"（`krx_predictionplayers`）这条推断正好证实了我们选的"完整 `CWorldCore` + 影子角色"路线，而不是静态障碍。
> 但**不能声称"实现与参考相同"**：打分归一化、树的确切形态、rollout 策略、均值还是最大值、剪枝细节都不可知；本实现是按文档语义重建，不是逆向。

---

## 7. 配置变量（CVars）完整参考

定义于 `src/engine/shared/config_variables_bestclient.h`，全部 `CFGFLAG_CLIENT | CFGFLAG_SAVE`。

| 变量名 | 类型 | 默认值 | 范围 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `bc_avoid_enabled` | int | `1` | 0~1 | 避障模块总开关（关闭时感知也停止） |
| `bc_avoid_active` | int | `0` | 0~1 | **是否已启用代理**（可用 `bind X toggle bc_avoid_active 1 0` 绑定按键） |
| `bc_avoid_agent` | int | `0` | 0~4 | 代理类型：0=Basic, 1=Legit, 2=Blatant, 3=Fentbot, 4=Pilot |
| `bc_avoid_direction_assist` | int | `1` | 0~1 | 允许代理改动左右方向键 |
| `bc_avoid_hook_assist` | int | `1` | 0~1 | 允许代理使用/松开钩子 |
| `bc_avoid_check_ticks` | int | `26` | 2~50 | **前瞻帧数**：必须保证安全的时长 |
| `bc_avoid_kick_in_ticks` | int | `20` | 0~50 | **介入阈值**：玩家输入还能安全这么久就先不干预 |
| `bc_avoid_quality` | int | `24` | 1~200 | 搜索质量（每帧模拟的候选方案数） |
| `bc_avoid_randomness` | int | `30` | 0~200 | 搜索随机度（MCTS 探索权重） |
| `bc_avoid_direction_weight` | int | `100` | 0~200 | 优先级：保持玩家意图方向 |
| `bc_avoid_hook_weight` | int | `100` | 0~200 | 优先级：保持当前钩子状态 |
| `bc_avoid_life_weight` | int | `150` | 0~200 | 优先级：单纯活得更久 |
| `bc_avoid_tile_death` | int | `1` | 0~1 | 把死亡块当危险 |
| `bc_avoid_tile_freeze` | int | `1` | 0~1 | 把冻结块当危险 |
| `bc_avoid_tile_unfreeze` | int | `0` | 0~1 | 把解冻块当危险（默认关） |
| `bc_avoid_unfreeze_ticks` | int | `26` | 2~50 | 解冻块专用的检查时长 |
| `bc_avoid_tile_tele` | int | `0` | 0~1 | 把传送块当危险（默认关） |
| `bc_avoid_player_prediction` | int | `1` | 0~1 | 预测其他玩家移动 |
| `bc_avoid_nsif` | int | `1` | 0~1 | 无安全解时沿用已知最安全方案的第一步 |
| `bc_avoid_afk_protect` | int | `1` | 0~1 | 挂机自动解除代理 |
| `bc_avoid_afk_time` | int | `60` | 5~600 | 挂机判定秒数 |
| `bc_avoid_track_point` | int | `0` | 0~1 | 保持"上次可勾住实地的瞄准方向" |
| `bc_avoid_safe_aim_tracking` | int | `1` | 0~1 | 仅在整段前瞻都安全时才锁定跟踪方向 |
| `bc_avoid_auto_drag` | int | `0` | 0~1 | 安全时自动勾住最近队友拖拽 |
| `bc_avoid_aimbot` | int | `0` | 0~1 | 内置瞄准辅助开关 |
| `bc_avoid_aimbot_mode` | int | `0` | 0~1 | 0=自动瞄准，1=瞄准辅助 |
| `bc_avoid_aimbot_segments` | int | `24` | 4~128 | 视野内扫描角度分段数 |
| `bc_avoid_aimbot_fov` | int | `90` | 10~180 | 视野角（度） |
| `bc_avoid_sensing_radius` | int | **`2`** | **1~32** | 感知半径，**半格**为单位（`2` = 1 图块 = 默认，`12` = 6 图块）；v1.2.1 起旧配置会在启动时自动 ×2；v1.2.5 起出厂默认 = 1 图块（已有配置不变） |
| `bc_avoid_show_hud` | int | `1` | 0~1 | AVOID HUD 总开关（还需 HUD 编辑器里的 `Avoid` 模块处于开启状态） |
| `bc_avoid_show_visuals` | int | `0` | 0~1 | 世界空间威胁可视化 |
| `bc_avoid_log` | int | `0` | 0~1 | 决策日志输出到控制台 |
| `bc_avoid_debug_override` | int | `0` | 0~1 | **输入管线自检**：强制每帧接管并反转左右方向键，用于验证拦截管线（见 [8.4](#84-输入拦截管线验证bc_avoid_debug_override)） |

### 6.9 "恢复默认"按钮（v1.2.4）

右侧面板标签栏的最右端有一个小方形按钮（**恢复默认**），点击后把**当前所选模式**的参数恢复成出厂默认：

* 它只存**配置项脚本名**（`g_aLegitDefaultParams` / `g_aBasicDefaultParams`），值由配置系统自己取
  （`IConfigManager::Reset(name)` → `ResetToDefault()`），所以**不存在第二份会漂移的默认值表**；
* 列表 = "该模式面板上显示的参数" + "该模式实际读取、但滑条放在别的面板里的参数"
  （拟真额外包含 `kick_in_ticks`、`nsif`；基础额外包含方向辅助、前瞻、介入阈值、权重、挂机保护）；
* **不碰** `bc_avoid_agent`（当前模式）、`bc_avoid_active`（启停）、`bc_avoid_enabled`（模块开关）、
  `show_hud` / `show_visuals` / `log` / `debug_override` —— 恢复参数不应该顺手把你切回基础或把代理关掉；
* 激进 / Fentbot / Pilot **暂时没有这个按钮**：它们的算法还没实现，给一个"恢复默认"只会让人以为那些参数在起作用
  （阶段四补齐列表即可，代码里已留注释）；
* 自检脚本会校验：所有列出的名字都必须是真实存在的 `bc_avoid_*` 配置项（写错名字只会在运行时
  `log_error` 并静默什么都不做，这类问题必须被脚本挡住）。

### 7.0 哪些参数在哪个代理里生效（v1.2.0）

| 代理 | 接线的参数 |
| :--- | :--- |
| **Basic** | `enabled` / `active` / `agent` / `direction_assist` / `check_ticks` / `kick_in_ticks` / `direction_weight` / `life_weight` / `tile_*` / `sensing_radius` / `afk_*` / `show_*` / `log` / `debug_override` |
| **Legit** | Basic 的全部（除 `direction_weight` 之外的取舍项都换成了搜索里的打分项）**加上** `quality` / `randomness` / `hook_weight` / `unfreeze_ticks` / `player_prediction` / `nsif` / `hook_assist`（松钩与按钩）；`direction_assist` 仍然限定方向候选 |
| **Blatant / Fentbot / Pilot** | 未实现：不参与决策，`Plan.m_aReason` = `agent not implemented yet`；它们的参数面板仍是占位 |

> `track_point` / `safe_aim_tracking` / `auto_drag` / `aimbot*` 属于阶段四 Blatant，Legit 不读它们
> （见 [`docs/avoid/blatant.md`](avoid/blatant.md) 的边界说明）。

### 7.1 控制台命令

| 命令 | 参数 | 说明 |
| :--- | :--- | :--- |
| `avoid_toggle` | - | 启用/解除当前代理（等价于 `toggle bc_avoid_active 1 0`） |
| `avoid_status` | - | 打印状态、感知数据、决策计数与耗时；`kick_in_ticks >= check_ticks` 时额外给一条配置警告 |
| `avoid_reset` | - | 重置决策/接管/NSIF 计数器 |

### 7.2 推荐绑定

```
bind X toggle bc_avoid_active 1 0
```

---

## 8. 验收与验证

### 8.1 界面验收清单（请按顺序检查）

| # | 操作 | 期望结果 |
| :--- | :--- | :--- |
| 1 | `ninja -C build DDNet` | 编译通过，无新增警告 |
| 2 | 进入 `设置 → TAS&` | 顶部子标签栏显示 **TAS / 避障 / 辅助模块** 三个标签 |
| 3 | 点击 **避障** | 页面正常渲染：顶部状态栏 + 左栏（辅助模式、总控、实时状态）+ 右栏面板 |
| 4 | 左栏标题 | 显示 **辅助模式**（不是"避障代理"） |
| 5 | 依次点击 5 个模式按钮 | **右栏面板集合随之改变**：Basic 只有 2 个面板，Legit 4 个，Blatant 5 个，Fentbot/Pilot 各 4 个；面板名称与内容都不同 |
| 6 | 在 Legit 选到第 3 个面板 → 切到 Blatant → 再切回 Legit | 回到 Legit 时仍停在第 3 个面板（每个模式独立记忆选中项） |
| 7 | 切换系统语言为中文 | 模式按钮显示 **基础 / 拟真 / 激进 / Fentbot / Pilot**；左上角标题为 **辅助模式**；面板名称为 辅助/调参/优先级/图块/瞄准/自救/寻路/可视化 等 |
| 8 | 拖动任意滑块，重启客户端 | 数值被持久化到 `config` 中的 `bc_avoid_*` |
| 9 | 点击"启用避障代理" | 顶部状态徽章从 `STANDBY` 变为 `WATCH`。⚠️ 默认开启的"挂机保护"会在 **60 秒无任何输入** 后自动解除并显示 `AFK`（参考客户端同款行为）；验收时保持操作或关掉 `bc_avoid_afk_protect` |
| 10 | 控制台执行 `avoid_status` | 打印完整状态行，无崩溃 |
| 11 | 在 **拟真** 页把几个滑块拉到极端（`Quality 200`、半径 32、`Life priority 0`），点右侧标签栏**最右端的方形按钮 `恢复默认`** | 该模式的参数立刻回到默认值（滑块跳回 24 / 12 / 150）；控制台出现 `避障：参数已恢复为默认值`；按钮标签在 2 秒内显示 `已恢复` | 一键恢复默认参数 |
| 12 | 切到 **基础** 后同样点一次 | 基础读取的参数（方向辅助、前瞻、介入阈值、权重、图块、感知半径、挂机保护）回到默认；**基础 / 拟真 / 激进的选择与实际启用状态不变** | 按钮只动参数，不动模式与启停 |

### 8.2 危险感知层验证

前提：进入一张有黑水/冻结块的图（在线状态），**不需要**启用代理，只要"启用避障模块"是打开的（默认开）。
观察左栏 **实时状态** 卡片：

| 操作 | 期望观察 | 证明了什么 |
| :--- | :--- | :--- |
| 朝黑水走过去 | `最近` 行距离**持续减小**（单位：图块，1 图块 = 32 px），到达黑水边缘时接近 `0.0` | 半径扫描与最近威胁计算实时工作 |
| 踩进黑水/冻结块 | `角色` 行显示 `DEATH`（红）或 `FREEZE`（蓝），并给出角色图块坐标 | 自身状态判定与图块分类正确 |
| 站在空旷处 | `危险 / 已探测` 的分子为 0，`最近` 行显示 `范围内无危险` | 没有误报 |
| 把 `Tiles` 面板的 **死亡块** 取消勾选 | `危险 / 已探测` 的分子立刻下降，`角色` 行不再显示 DEATH | 分类结果按配置实时过滤（`IsRelevantHazard`） |
| 拖动 **半径（半格）** | `危险 / 已探测` 的**分母**变化：4（2 格）→ 5 × 3 = 15，12（6 格）→ 13 × 9 = 117，32（16 格）→ 33 × 21 = 693（横向 `2·ceil(Rx)+1` × 纵向 `2·ceil(Ry)+1`，`Ry = Rx × 垂直系数`）；分子只统计落在**椭圆**内的危险块 | 扫描规模与"到达距离"都真的随滑条变化，且竖直方向更短 |

### 8.3 Basic 代理验收（v1.1.0 起）

前提：一张有黑水/冻结块的图，Basic 模式，勾选 **启用避障代理**（或 `avoid_toggle`）。
把 `bc_avoid_log 1` 打开可以同时看到每 tick 的决策日志（`player safe N/26` 是最有用的一列）。

| # | 操作 | 期望结果 | 证明了什么 |
| :--- | :--- | :--- | :--- |
| 1 | 什么都不按，站在空地 | `接管` 计数**不增长**；状态徽章停在绿色 `WATCH`；日志里 `override no`、`player safe 26/26` | 玩家安全时零干预（手感生命线） |
| 2 | 按住 D 直冲黑水 | 接触前 **1~2 图块**内被刹停或明显减速；徽章转橙色 `ASSIST`，`Plan` 显示 `在危险前刹停` / `向左减速`，接管 +1 | 验收第 1 条：走向黑水被停住 |
| 3 | 钩住一侧地面，荡向黑水 | 出现反向按键制动，速度明显下降（**不会**自动松钩，这是阶段三） | 验收第 2 条：荡向危险被反向键减速 |
| 4 | 直行穿过黑水旁的窄通道 | **不出现**莫名其妙的减速；`接管` 不增长 | 验收第 3 条：探测点正确、无误触发 |
| 5 | `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms（实测约 0.02 ms，见 6.7） | 性能预算达标（见 6.4） |
| 6 | 走进冻结块 | 徽章仍为 `WATCH`，`Plan` 显示 `已冻结，代理待机` | `HAZ_SELF` 优先于一切（6.5 第 4 条） |
| 7 | 关掉 `Tiles` 面板的 **死亡块** | 走向黑水不再被拦（因为黑水不再算危险） | 危险判定确实走 `IsRelevantHazard()` 配置 |
| 8 | 把 **半径（半格）** 从 2（默认，1 格）依次调到 4（2 格）、12（6 格），每次朝黑水走 | 每次都**明显更早**才开始减速（1 格 ≈ 30 px、2 格 ≈ 60 px、6 格 ≈ 190 px 到达距离）；1 格时多半只在最后关头跳/松钩，4 格起以刹车为主；调到 24 以上与 12 差不多 | 半径是真正的介入距离控制，半格是真实步进（见 6.7） |
| 9 | 反复开关代理再走一段 | 不崩溃、不卡顿、无异常日志 | 状态机与缓存计划（`m_LastPlan`）安全 |

### 8.3b Legit 代理验收（v1.2.0 起）

前提：一张有黑水/冻结块的图，`bc_avoid_agent 1`（拟真），勾选 **启用避障代理**（或 `avoid_toggle`），
右栏 **调参 / 优先级** 面板保持默认（`quality 24`、`randomness 30`、三个权重 100/100/150）。
`bc_avoid_log 1` 可以看到每次决策的 `plans`（迭代次数）、`plan d/j/h`（最终动作）与 `cost`。

| # | 操作 | 期望结果 | 证明了什么 |
| :--- | :--- | :--- | :--- |
| 1 | 勾住一面墙，按住钩子被拖向黑水 | 贴上黑水前**松开钩子**（`plan d/j/h` 的 h 变 0），角色减速或荡开；`Plan` 行显示 `松开钩子…` | 验收第 1 条：提前脱钩（[6.8.4](#684-钩子释放的判据验收第-1-条)） |
| 2 | 朝黑水走 | 与 Basic 一样在接触前停住，且常常是 `在危险前刹停`；必要时会看到 `跳起…` | 验收第 2 条不退化，而且多了一条出路 |
| 3 | 直行过窄通道 | **不出现**莫名其妙的减速/跳跃；`接管` 不增长；日志里 `override no` | 验收第 3 条：不误触发，也不鬼畜跳 |
| 4 | `Quality` 1 → 24 → 200 | 1：`plans 1`、几乎不干预、`cost` 接近 0；24：`plans 24`、正常决策；200：`plans` 更大、`cost` 上升到 ~1 ms 但仍 ≤ 1.5 ms | 验收第 5 条：Quality 真的影响质量与耗时 |
| 5 | `Life priority` 拉满 / 归零 | 拉满 → 更保守（宁可改输入也要活）；归零 → 更贴玩家输入（干预变少） | 验收第 4 条：优先级滑块能感觉到 |
| 6 | `Hook priority` 拉满 | 能刹住就不松钩（`h` 保持玩家值）；归零则更愿意松钩换取存活 | `bc_avoid_hook_weight` 真的接线 |
| 7 | 让队友从侧面把你撞向黑水 | 提前出现反向键/跳跃；关掉 **预测其他玩家** 后完全不反应（`player input safe`） | 验收第 6 条 |
| 8 | 站在解冻块旁边（打开 **解冻块** 图块） | 介入时机由 **Unfreeze lookahead** 决定，与 `check_ticks` 无关 | 验收第 7 条 |
| 9 | 打开喷气背包 / 钩住队友 / 开着 `cl_dummy_hammer` 在空中 | 状态栏 `Plan` 显示 `喷气背包中，代理不介入` / `钩住玩家中，代理不介入` / `飞锤中，代理不介入`，输入完全不被改写 | TAS 红线（[6.8.6](#686-身法状态门tas-红线-3117-的显式化)） |
| 10 | `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms（Quality 200 + 8 个附近玩家时重点看） | 性能预算（[6.4](#64-性能预算-50-tps-的硬约束)） |
| 11 | 开关代理、Basic ↔ Legit 来回切 | 不崩溃、不卡顿；Basic 的行为与 v1.1.1 完全一致 | 状态机与缓存计划安全 |

### 8.3c 参数灵敏度台架（"这个滑条到底有没有用"）

`CAvoidLegitTest.ParameterSensitivitySweep` 让一个"走神的玩家"以固定输入一直朝水坑走 70 帧，
每帧都跑**客户端同款决策链**（`ScanThreat()` → 感知闸门 → `CPlanner::Plan()`），代理可以随时接管，
然后逐个参数扫档并打印：存活 / 死亡帧 / 接管帧数 / 新增跳跃 / 松钩 / **首次接管时的剩余帧数（提前量）** / 迭代数 / 耗时。

```bash
./build/testrunner --gtest_filter='CAvoidLegitTest.ParameterSensitivitySweep' 2>&1 | grep avoid-params
```

它同时回答两件事：**参数有没有接线**（每一档都要有差别）、**手感指标到底是多少**（提前量、动作分布、成本）。
用例里只钉住"必须成立"的那几条（`quality 1` 不接管、`life_weight 0` 不接管、`tile_death 0` 看不到危险、
半径与前瞻越大提前量越大、`nsif` 开关一进一出），其余作为可对比的基线输出。

### 8.4 输入拦截管线验证（`bc_avoid_debug_override`）

要单独证明"拦截到的输入真的能操控角色"，而不是只看决策日志，用这个自检开关：
它会强制每帧接管，并把左右方向键取反。

1. 左栏 **总控** 卡片勾选 **输入管线自检**（等价于控制台 `bc_avoid_debug_override 1`）
2. 顶部状态徽章变为橙色 **ASSIST**，`Plan` 行显示 **调试接管 (输入管线自检)**，`接管` 计数开始每帧累加
3. **按住 D → tee 向左走；按住 A → tee 向右走；不按键则不动**
   - 这一步同时验证了本地预测与服务器发包：`pData` 正是预测取样与网络发包共用的那个缓冲区
4. 取消勾选，行为立刻恢复正常
5. 控制台 `avoid_status` 会打印 `decisions` 与 `overrides`：
   - `decisions` 每 tick +1（约 50/秒）→ 说明 `ApplyInput()` 确实以 50 Hz 被调用
   - 自检开启时 `overrides == decisions`；关闭时 `overrides` 只在 Basic 真接管时才增长 → 说明写回通路是受控的

> [!NOTE]
> 自检开关默认关闭，且只在勾选时生效；它是调试时的常备工具，不是产品功能。

### 8.5 模拟器离线回归（不需要开游戏）

```bash
ninja -C build testrunner
./build/testrunner --gtest_filter='CAvoidSimulatorTest.*'
```

3 个用例分别锁住：

1. `StepRecipeMatchesTheEngineTickSequence` —— 模拟器的步进序列（`Tick(true,false)` + `Move()`）
   与引擎的 `Tick(true,true)` + `Move()` 在真实地图上**逐帧完全一致**（含钩索飞行、跳跃、撞墙）；
2. `WalkingAcrossTheRealMapIsReproducible` —— 50 帧行走不穿墙、不出 NaN；
3. `MapTuningDrivesThePrediction` —— 换一份 tuning（`ground_control_speed` 10 → 5）会得到不同轨迹，
   也就是"物理真的跟随地图 tuning"，这条是"禁止硬编码常量"的回归护栏；
4. `WorstCaseDecisionCostStaysInsideTheTickBudget` —— 打印最坏情况的每决策耗时并与 1.5 ms 预算对比
   （见 [6.7](#67-basic-代理实现说明v110) 的性能段）。

感知半径另有一组测试（`src/test/avoid_sensing_radius_test.cpp`，自带合成地图所以数字是精确的，
v1.2.1 起是 6 个用例）：

```bash
./build/testrunner --gtest_filter='CAvoidSensingRadiusTest.*'
```

| 用例 | 锁住的行为 |
| :--- | :--- |
| `BrakesInTimeAcrossTheWholeRadiusRange` | 半径 2~16 每一档都能在撞上危险前刹停 |
| `WiderRadiusReactsFurtherFromTheHazard` | 半径越大刹车点越远（半径 6 比半径 2 早 4 图块以上） |
| `MinimumRadiusStillFitsTheBrakingDistance` | 最小半径下"发现危险 → 需要刹停"仍有足够帧数 |
| `CheckTicksTakesOverAtLargeRadii` | 半径很大时改为由 `check_ticks` 决定，反应距离不再增长 |
| `HalfATileIsTheAlmostOffEnd` | 0.5 格比 1 格明显更晚，且 0.5 格已经来不及刹停（"几乎关闭"档） |
| `HalfTileStepsAreRealSteps` | 2 → 2.5 → 3 格的到达距离严格单调（半格不是四舍五入） |

**Legit 决策回归（`CAvoidLegitTest`，12 个用例，`src/test/avoid_legit_test.cpp`）**：
同样使用"在内存里造几何"的合成地图夹具，数字是精确的；
每个用例都先断言"局面确实如设计"（例如"玩家自己的输入确实会死"），再断言代理的行为，
避免用例在几何变化后退化成空转。

| 用例 | 锁住的行为 |
| :--- | :--- |
| `HoldingAHookThatDragsIntoTheHazardIsReleased` | 验收第 1 条：按住钩被拖向危险时松钩，且松钩后活满前瞻 |
| `ReleasingTheHookIsRefusedWhenItWouldFlyIntoTheHazard` | 防止"为了松钩而送死"：逐个方向断言松钩会死，且代理保持钩子 |
| `QualityDrivesSearchEffortAndTheDecision` | `quality` = 迭代次数：1 → 不干预，24 → 覆盖 18 个候选并松钩，200 → 更多迭代与更高耗时 |
| `HookWeightChangesTheHookDecision` | `bc_avoid_hook_weight` 在两个方向上翻转"松不松钩" |
| `PlayerPredictionChangesTheDecision` | 验收第 6 条：注入影子后提前反应；关掉后与"场上没人"逐字相同 |
| `UnfreezeTicksSetTheLookaheadNearUnfreezeTiles` | 验收第 7 条：最近威胁是解冻块时前瞻换成 `unfreeze_ticks` |
| `SafeCorridorAndFlatWalkAreNeverTouched` | 验收第 3 条：两侧贴危险的通道直行 10 tick，零接管、零跳跃、零钩子 |
| `FlatGroundIsNeverTouched` | 平地上危险在 10 图块外时永远 `player input safe` |
| `SpecialMovementStatesAreHandsOff` | 喷气 / 钩人 / 飞锤三种状态显式不动手，并给出理由 |
| `SearchIsDeterministicPerTick` | 同一 tick 的同一输入 → 同一个计划（本地 PRNG，无全局 RNG） |
| `RandomnessChangesTheSearchWithoutBreakingIt` | `randomness = 0` 可复现；`= 200` 搜索过程与回报不同且不越预算 |
| `LegitDecisionCostStaysInsideTheTickBudget` | 最坏情况（必死 + 8 个预测玩家 + quality 200）≤ 1.5 ms，并打印 quality 1/24/200 的实测 |

### 8.6 游戏内 HUD 模块与威胁可视化验证

| 操作 | 期望观察 |
| :--- | :--- |
| 打开 `设置 → TAS& → 避障`，总控里勾选 **状态 HUD** | 游戏内立刻出现 AVOID 面板，内容与设置页"实时状态"卡片一致 |
| 按 `ESC` 关闭设置回到游戏 | 面板仍在（不再需要打开设置才能看状态） |
| 打开 HUD 编辑器（`设置 → BestClient → 外观(Visuals)` 页顶部的 **HUD editor** 按钮，需先进游戏） | 画布中出现 **Avoid** 模块；可拖动、右下角把手可缩放、右键可改背景与透明度 |
| 在编辑器里用眼睛图标关闭 `Avoid` 模块 | 面板消失；回到避障页，"状态 HUD"勾选框同步变为未勾选（同一个开关） |
| 在编辑器里把它拖到别处、缩放到 150% | 位置与缩放被持久化，重启客户端后保持 |
| 单击编辑器里的"重置位置/重置缩放" | 回到默认位置 `(286, 52)` 与 100% 缩放 |
| 勾选 **威胁可视化** | 世界中绘制危险图块彩色方框、感知圆环、指向最近威胁的连线 |
| **快速奔跑/钩索荡动时观察圆环** | 圆环与连线**紧贴角色平滑移动，不再出现重影/拖影**（v1.0.1 修复） |
| 走进冻结块 | 面板"角色"行变蓝显示 `FREEZE`，圆环与方框颜色一致 |
| 拖动 **感知半径** | 世界里的圆环与高亮范围随之放大/缩小；面板"危险 / 探测"分母同步变化 |

### 8.7 证据对照表（每一层能力对应可观察的证据）

| 声称的能力 | 直接证据 | 章节 |
| :--- | :--- | :--- |
| 危险感知层 | 实时状态卡片的距离/计数随移动与图块开关实时变化 | [8.2](#82-危险感知层验证) |
| 输入拦截管线 | 自检开关下角色反向移动 + `decisions` 以 50 Hz 增长 | [8.4](#84-输入拦截管线验证bc_avoid_debug_override) |
| **Basic 决策（走 / 荡）** | 朝黑水走被刹停、荡向黑水被反向键减速；`接管` 与 `Plan` 行实时变化 | [8.3](#83-basic-代理验收v110-起) |
| **Legit 松钩 / 跳跃 / 搜索** | 按住钩被拖向黑水时提前脱钩；`plans` 随 `quality` 变化；`cost` ≤ 1.5 ms | [8.3b](#83b-legit-代理验收v120-起)、[6.8](#68-legit-代理实现说明v120) |
| **其他玩家预测** | 队友把你撞向黑水时提前反应；关掉开关后完全不反应 | [8.3b](#83b-legit-代理验收v120-起) 第 7 行 |
| **红线：身法状态不动手** | 喷气 / 钩人 / 飞锤时 `Plan` 给出 `…，代理不介入`，输入零改写 | [6.8.6](#686-身法状态门tas-红线-3117-的显式化) |
| **前向模拟器正确性** | 离线回归：与引擎步进逐帧一致 + 跟随地图 tuning | [8.5](#85-模拟器离线回归不需要开游戏) |
| 游戏内 HUD / 可视化 | 可在 HUD 编辑器里拖动的 AVOID 面板 + 世界中彩色图块、圆环、连线 | [8.6](#86-游戏内-hud-模块与威胁可视化验证) |

## 9. 编译与回归验证

```bash
# 1. 编译
ninja -C build DDNet

# 2. 校验避障词条已落地（中英俄）
python3 - <<'PY'
import io
keys = ["Avoid", "Assist mode", "Arm Avoid agent", "Check ticks", "NSIF on no safe input",
        "Basic", "Legit", "Blatant", "Input pipeline self-test",
        "brake before hazard", "steer left before hazard", "player input safe"]
for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for k in keys:
        assert "\n%s\n== " % k in text, (path, k)
print("avoid localization OK")
PY

# 3. 校验避障配置变量已注册
python3 - <<'PY'
text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
n = text.count("MACRO_CONFIG_INT(BcAvoid") + text.count("MACRO_CONFIG_STR(BcAvoid")
assert n == 33, n
print("avoid cvars OK:", n)
PY

# 4. 决策引擎实现存在（v1.1.0 Basic / v1.2.0 Legit），且模拟器与代理回归全部通过
grep -n "CAvoid::SimulateInput" src/game/client/components/bestclient/avoid.cpp
grep -n "SInputPlan CPlanner::Plan" src/game/client/components/bestclient/avoid_engine.cpp
ninja -C build testrunner && ./build/testrunner --gtest_filter='CAvoid*.*'

# 5. 一条命令跑完全部避障回归（附录 D 的脚本，CI/交付都用它）
./scripts/avoid_selfcheck.sh
```

---

## 10. 已知限制与风险

| 风险 | 说明 | 处理建议 |
| :--- | :--- | :--- |
| **Basic 只做方向键制动** | Basic 的定义如此：不松钩、不跳跃、不瞄准（[6.7](#67-basic-代理实现说明v110)）；需要这些就切 Legit | 保持不变（阶段二验收行为） |
| **玩家预测的偏差** | Legit 会注入最近的 8 个 tee，但它们只做匀速平移、会穿墙、位置来自上一 tick | 见 [6.8.5](#685-其他玩家预测bc_avoid_player_prediction) 与 [6.8.9](#689-已知不足留给阶段四--阶段五) |
| **Legit 没有瞄准** | 按下钩子只能用玩家当前瞄准方向；Track Point / Auto Drag / 内置瞄准属于阶段四 | 阶段四任务书负责 |
| **探索 rollout 不参与排序** | `Randomness` 只改变搜索过程与 `Plan.m_Score`，不会让计划更冒险（保守性决定，见 [6.8.3](#683-搜索uct)） | 阶段四若要用序列搜索，必须同时保留"第一个动作本身安全"的证据 |
| **质量护栏** | `quality = 200` 在预测开启时通常会在 ~45 次迭代后撞上 1.0 ms 护栏 | 预期行为；预算见 [6.4](#64-性能预算-50-tps-的硬约束) |
| **状态徽章为英文短标签** | `WATCH / ASSIST / NSIF / AFK` 未本地化，与 TAS 页 `IDLE/REC` 风格一致 | 如需中文可在 `RenderSettingsAvoid` 里改用 `BcLocalize` |
| **HUD 只在启用时显示** | 未启用代理且未开可视化时，游戏内不会出现 AVOID HUD（避免打扰普通玩家） | 如需常驻可自行放开 `CAvoid::OnRender()` 里的条件 |
| **`bc_tas_tab` 语义变化** | 原 0=TAS / 1=辅助模块；现 0=TAS / 1=避障 / 2=辅助模块。老配置里值为 1 的用户打开 TAS& 会直接看到避障页 | 已知且可接受；如需兼容可加一次性迁移 |
| **网络延迟下预测漂移** | 参考实现明确要求 `cl_prediction_margin` 略高于 ping（如 50ms ping 设 70） | 文档提示即可；后续可在 UI 上给出建议值 |
| **UiScale 极高时底部裁切** | 布局总高 462 px，UiScale 110 时可用约 471 px，留有余量但不大 | 新增控件时必须复核 [5.1 节](#51-布局严格遵循左上角状态栏--左栏模式与状态--右栏参数) 的高度预算 |
| **沙盒/实战感知一致性** | `ActiveCore()` 已兼容 TAS 本地沙盒，模拟器也改成从沙盒世界的 `m_Core` / `Teams()` 取世界与队伍指针 | 见 6.3 方案 B 的风险栏 |
| **Blatant / Fentbot / Pilot 仅有 UI** | 参数为只读占位，不含算法；切过去不会改输入，状态栏显示 `该代理尚未实现` | 阶段四（Blatant）及以后；不要顺手改它们的语义 |

---

## 11. 与 TAS 模块的边界（避免互相破坏）

| 场景 | TAS 行为 | 避障行为 | 结论 |
| :--- | :--- | :--- | :--- |
| TAS 录制中 | `OnRecordInput()` 记录原始输入 | 在记录**之后**运行，只改发出去/预测用的 `pData` | 录进 `.tas` 的是玩家原始输入，轨迹纯净 |
| TAS 回放中 | `OnSnapInput` 直接返回预录制数据 | `IsPlaybackActive()` 时提前 return，不介入 | 回放逐帧精确，不受避障干扰 |
| TAS 本地沙盒激活 | `CFastPractice::Active()`，物理由沙盒驱动 | 感知层自动切换到沙盒角色 | 练图时感知正确 |
| 分身连接 | `Dummy == true` 分支 | 只在 `!Dummy` 分支挂载 | 永远不会动到分身输入 |

---

## 12. 变更文件总览（阶段一 + 阶段二）

> 阶段三（v1.2.0）的文件清单见 [附录 C](#附录-c变更历史) 的 v1.2.0 条目。

```
新增:
  src/game/client/components/bestclient/avoid.h
  src/game/client/components/bestclient/avoid.cpp
  src/game/client/components/bestclient/avoid_engine.h                       (阶段三)
  src/game/client/components/bestclient/avoid_engine.cpp                     (阶段三)
  src/game/client/components/bestclient/menus_avoid.cpp
  src/test/avoid_sim_test.cpp                                                (阶段二)
  src/test/avoid_sensing_radius_test.cpp                                     (v1.1.1)
  src/test/avoid_legit_test.cpp                                              (阶段三)
  docs/AVOID_TECHNICAL_DOCUMENTATION.md   (本文档)
  docs/TAS_TECHNICAL_DOCUMENTATION.md                        (交叉引用与 bc_tas_tab 范围更新)
  scripts/avoid_selfcheck.sh                                 (附录 D 的回归自检脚本)

修改:
  CMakeLists.txt                                            (+3 行：avoid.cpp/.h、menus_avoid.cpp)
  src/engine/shared/config_variables_bestclient.h           (+33 行 bc_avoid_*、1 行注释、bc_tas_tab 上限 1→2)
  src/game/client/gameclient.h                              (+2 行：include + 成员)
  src/game/client/gameclient.cpp                            (+5 行输入挂点，组件注册：TAS/Avoid 移入 HUD 渲染阶段)
  src/game/client/components/controls.cpp                   (+4 行：代理启用时强制 50 Hz 发包)
  src/game/client/components/hud_layout.h                   (+1 行：MODULE_AVOID 枚举)
  src/game/client/components/hud_layout.cpp                 (+4 行：模块表/默认布局/编辑器白名单)
  src/game/client/components/bestclient/hud_editor.cpp      (+9 行：编辑器矩形/收集/预览)
  src/game/client/components/menus.h                        (+1 行：声明)
  src/game/client/components/bestclient/menus_tas.cpp       (子标签栏 2 → 3，分支路由)
  data/BestClient/languages/simplified_chinese.txt          (+135 词条)
  data/BestClient/languages/russian.txt                     (+135 词条)
```

---

*阶段三 Legit 交付完成（v1.2.0）。下一步：阶段四 Blatant ——
瞄准方向搜索（Track Point / Safe Aim Tracking）、Auto Drag 与内置瞄准，
并把 NSIF 调优补上；前向模拟器与感知层不需要重写，见 [附录 F](#附录-f交付分档与路线图)。*


---

## 附录 A：阶段二任务书（可直接交给下一个 AI）

> 下面整段可以原样复制给下一个 AI，它不需要额外的口头背景。

```text
任务：为 BestClient（DDNet 分支）实现"避障 / Avoid"模块的决策算法（阶段二）。

仓库与文档
- 仓库根目录：BestClient（分支 feature/tas）
- 必读文档：docs/AVOID_TECHNICAL_DOCUMENTATION.md（先读"速览"和"第 6 章"）
- 参考需求：docs/avoid/*.md（闭源参考客户端 KRX 的功能文档，已拷贝进仓库）

你要做的事
实现 src/game/client/components/bestclient/avoid.cpp 里的唯一空函数：

    CAvoid::SInputPlan CAvoid::EvaluateBestPlan(const SContext &Ctx);

目标效果（用户原话）
1. 朝黑水/自杀块走：按键失效或迅速减速，能停在危险边缘。
2. 钩索荡向危险：自动按反方向键减速。
3. 勾向贴有黑水的墙 / 持续按住钩子被拉向危险：提前松开钩子。

硬性约束（违反会导致返工）
- 不要改动 SContext / SInputPlan / SThreat / SSettings 的结构，不要改 ApplyInput() 的调用约定。
- 只写 Plan.m_Input（必须从 Ctx.m_Input 复制后修改），不要写 m_Controls.m_aInputData。
- 危险判定必须复用 ClassifyPoint() / IsRelevantHazard()，不要另写一套探测点。
- 物理前向模拟必须使用本地图的 tuning（GetTuning / m_Tuning），不要硬编码重力、跳跃加速度等常量。
- 每 tick 预算 ≤ 1.5 ms，用 Plan.m_CostMs 上报（UI 会显示）。禁止在引擎里做堆分配。
- 代码风格遵循仓库现有风格（tab 缩进、clang-format 配置见 .clang-format）。
- 新增任何界面字符串必须走 BcLocalize()，并在
  data/BestClient/languages/simplified_chinese.txt 与 russian.txt 同步补齐词条
  （词条不要用 [ ] 包裹，见 TAS 文档 6.3.2）。

建议实现顺序（每步都可独立验证）
第 1 步 前向模拟器：bool SimulateInput(上下文, 候选输入, 最大帧数, int &实际存活帧数)
        先用 bc_avoid_log 1 打印每帧位置，与游戏内实际轨迹目测校准。
        ★ 校准技巧：把 bc_avoid_show_visuals 打开，世界里会画出感知半径与危险图块，
          再配合 bc_avoid_debug_override 的接管计数确认管线在工作。
第 2 步 方向键制动（覆盖验收 1、2）：只允许改 m_Direction，跑通"停得住"。
第 3 步 钩子释放（覆盖验收 3）：允许改 m_Hook。
第 4 步 升级为 MCTS + NSIF + Track Point + 内置瞄准，并把 Fentbot / Pilot 的占位参数接上。

验收标准
- 走进黑水：在接触前 1~2 图块内被停住或明显减速；玩家不按方向键时完全不干预。
- 钩索荡向黑水：出现反向按键制动，能停下或明显减速。
- 朝黑水墙勾：在贴墙前脱钩。
- 玩法手感：直行经过黑水旁的窄通道时不应该被误触发（这是最容易翻车的地方，
  探测点规则见文档 4.2，必须与 CCharacter::HandleSkippableTiles 一致）。
- `avoid_status` 里的 cost 稳定 ≤ 1.5 ms；掉帧说明搜索太贵。
- 完成后更新 docs/AVOID_TECHNICAL_DOCUMENTATION.md（附录 C 变更历史 + 第 6 章标记为已实现）。

自检
    ninja -C build DDNet
    python3 <文档 附录 D 的脚本>
```

---

## 附录 B：关键代码位置速查

> 行号对应 v1.1.0，改动后会漂移；优先按函数名搜索。

| 位置 | 内容 | 说明 |
| :--- | :--- | :--- |
| `avoid_engine.cpp` | `Avoid::SimulateFixed()` / `Avoid::SSimState::Step()` | **共享前向模拟器**：克隆 `CCharacterCore` 推演 N 帧，可选注入预测玩家 |
| `avoid_engine.cpp` | `Avoid::CPlanner::Plan()` | **Legit 决策引擎**：候选生成 → UCT → 保守排序 → NSIF（[6.8](#68-legit-代理实现说明v120)） |
| `avoid_engine.cpp` | `Avoid::CPlanner::Rollout()` | 固定候选序列 / 探索序列；命中危险立刻剪枝 |
| `avoid_engine.cpp` | `Avoid::ClassifyMovement()` | 身法状态门（喷气 / 钩人 / 飞锤） |
| `avoid_engine.cpp` | `Avoid::SensingVerticalFactor()` / `TileBoxDelta()` | 感知椭圆的竖直系数（由地图 tuning 推出）与每轴图块盒距离 |
| `avoid_engine.cpp` | `CPlanner::UpdateAvailability()` / `JumpEngaged()` | 跳跃策略：钩索期间不跳、只在危急关头跳 |
| `avoid.cpp` | `CAvoid::EvaluateBestPlan()` | **前置闸门 + 代理分派 + Basic 决策**：快路径 → 介入阈值 → 三方向候选 → NSIF |
| `avoid.cpp` | `CAvoid::BuildEnvironment()` | 克隆世界 + 最近 8 个其他 tee 的快照（玩家预测的数据源） |
| `avoid.cpp` | `CAvoid::FlyHammerState()` | 飞锤 / 深飞的客户端标记（`DummyConnected` + `cl_dummy_hammer` + 离地） |
| `avoid.cpp` | `CAvoid::LogTrace()` | `bc_avoid_log 1` 的每决策日志（含 `plans` / `plan d/j/h` / `cost`） |
| `menus_avoid.cpp` | `g_aLegitDefaultParams` / `AvoidDefaultParams()` | "恢复默认"按钮的参数表（只存配置项脚本名） |
| `menus_avoid.cpp` | 面板标签栏右端 | "恢复默认"方形按钮（`IConfigManager::Reset()`） |
| `avoid.cpp:471` | `CAvoid::ApplyInput()` | 每 tick 入口：感知 → 决策 → 写回 `pData` → 遥测 |
| `avoid.cpp:542` | `CAvoid::UpdateTelemetry()` | 把感知/决策结果翻译成 UI 与 HUD 的读数 |
| `avoid.cpp:310` | `CAvoid::ClassifyPoint()` | 危险探测点（中心点 + 四角），与引擎逐条对齐 |
| `avoid.cpp:352` | `CAvoid::ScanThreat()` | 半径扫描 + 最近威胁（AABB 距离） |
| `avoid.cpp:264` | `CAvoid::ClassifyTile()` | 图块 → 危险位掩码的纯映射 |
| `avoid.cpp:302` | `CAvoid::IsRelevantHazard()` | 按 `bc_avoid_tile_*` 过滤危险类型 |
| `avoid.cpp:400` | `CAvoid::ActiveCore()` | 取当前受控角色的物理核心（兼容 TAS 本地沙盒） |
| `avoid.cpp:432` | `CAvoid::OverlayAnchor()` | 世界可视化锚点（角色实际渲染位置，重影修复点） |
| `avoid.cpp:448` | `CAvoid::CheckAfkProtection()` | 挂机自动解除 |
| `avoid.cpp:195` | `CAvoid::ReadSettings()` | cvar → `SSettings` 快照 |
| `avoid.cpp:873` | `CAvoid::GetHudRect()` | HUD 模块矩形（画布坐标） |
| `avoid.cpp:906` | `CAvoid::RenderHudModule()` | 游戏内面板绘制 |
| `avoid.cpp:1004` | `CAvoid::RenderWorldOverlay()` | 危险图块高亮 / 感知环 / 威胁连线 |
| `avoid.h` | `MAX_CANDIDATES` / `MAX_SIM_TICKS` / `m_aSamples[]` / `m_Planner` | 决策路径的定长缓冲（无堆分配）与 Legit 引擎实例 |
| `avoid_engine.h` | `SSimState` / `SEnvironment` / `CPlanner` / `MAX_ROOT_ACTIONS` | 契约、影子世界与搜索的定长缓冲（无堆分配） |
| `src/test/avoid_sim_test.cpp` | `CAvoidSimulatorTest` | 模拟器保真度回归（3 个用例） |
| `src/test/avoid_sensing_radius_test.cpp` | `CAvoidSensingRadiusTest` | 感知半径闸门回归（4 个用例） |
| `src/test/avoid_legit_test.cpp` | `CAvoidLegitTest` | Legit 决策回归 + 成本基准（12 个用例） |
| `menus_avoid.cpp:198` | `CMenus::RenderSettingsAvoid()` | 避障页（状态栏 + 左栏 + 右栏面板） |
| `menus_avoid.cpp:46` | `g_aBasicPanels` 等面板表 | **每个模式的面板集合在这里定义** |
| `hud_layout.h:40` | `MODULE_AVOID` | HUD 编辑器模块枚举 |
| `hud_layout.cpp:47` | `gs_aModuleLayouts[MODULE_AVOID]` | 默认位置 `(286, 52)`、默认关闭 |
| `hud_editor.cpp:528` | `GetModuleVisual()` 的 Avoid 分支 | 编辑器里的可拖动矩形 |
| `gameclient.cpp:207` | `&m_Tas` / `&m_Avoid` 注册 | **必须留在 HUD 渲染阶段**（见 5.3 红线） |
| `gameclient.cpp:650` | `OnSnapInput` 挂点 | 避障输入拦截入口 |
| `controls.cpp` | `SnapInput()` 里的 `WantsEveryTickInput()` | 代理启用时强制 50 Hz |
| `config_variables_bestclient.h:609` | `bc_avoid_*` 共 33 个 | 配置面 |

---

## 附录 C：变更历史

| 版本 | 日期 | 内容 |
| :--- | :--- | :--- |
| **1.0.0** | 2026-10-01 | 阶段一交付：避障子标签页、`CAvoid` 组件、33 个 cvar、危险感知层、输入拦截管线、HUD 与世界可视化、控制台命令、中俄本地化；决策引擎留空并写明契约 |
| **1.0.1** | 2026-10-01 | ① AVOID 状态面板接入 HUD 编辑器（`MODULE_AVOID`，可拖动/缩放/调透明度/持久化），内容与设置页对齐；② 危险圆环重影修复（锚点改用角色实际渲染位置） |
| **1.0.2** | 2026-10-01 | HUD 面板被地图前景墙遮挡的修复：`m_Tas` / `m_Avoid` 移入 HUD 渲染阶段；顺带修正非 16:9 下 HUD 宽高比计算 |
| **1.1.1** | 2026-10-01 | **修复：感知半径对决策无效**。`bc_avoid_sensing_radius` 过去只喂给 `ScanThreat()`（HUD/可视化），决策引擎自己逐帧 `ClassifyPoint()` 无距离上限，导致滑条怎么调都在同一距离被刹住。现在 `EvaluateBestPlan()` 增加第 0 层闸门（`m_HasNearest`），半径真正决定介入距离；新增 `src/test/avoid_sensing_radius_test.cpp`（含合成地图夹具）4 个用例 + 2 条本地化词条 |
| **1.2.5** | 2026-10-02 | **感知半径默认值改为 2 半格（= 1 图块，用户实测手感最好）**：① `bc_avoid_sensing_radius` 默认 12 → **2**，描述里写明单位与默认；② 文档同步：单位/默认对照表、实测到达距离表（标注新默认与代价：1 格不够刹停，默认档更多依赖最后关头的跳跃/松钩）、游戏内验收第 8 行；③ 自检脚本的默认值断言同步；④ **已有配置不受影响**（迁移只做一次），点"恢复默认"按钮即可拿到新默认 |
| **1.2.4** | 2026-10-02 | **"恢复默认"方形按钮（用户要求）**：① 拟真（与基础）面板标签栏右端新增小方形按钮，点击后经 `IConfigManager::Reset()` 把该模式的参数恢复为默认值，标签在 2 秒内显示"已恢复"，并在控制台回显一行；② 默认值不复制，只列配置项脚本名，避免与 `config_variables_bestclient.h` 漂移；③ 不动模式选择 / 启停 / 模块开关 / HUD 与可视化开关；④ 激进 / Fentbot / Pilot 暂无按钮（算法未实现，留注释待阶段四）；⑤ +3 条中俄词条；⑥ 自检脚本新增"按钮存在 + 表内每个名字都是真实 cvar"的检查（2 张表 32 条） |
| **1.2.3** | 2026-10-02 | **参数灵敏度台架 + `kick_in_ticks = 0` 语义修正**：① 新增 `CAvoidLegitTest.ParameterSensitivitySweep`：脚本化"走神玩家"走 70 帧，逐档扫描 `check_ticks`/`quality`/`kick_in_ticks`/`sensing_radius`/`life_weight`/`randomness`/`nsif`/`direction_assist`/`hook_assist`/`tile_death`，输出存活、死亡帧、接管帧数、跳跃、松钩、**提前量**、迭代数、耗时，并钉住 6 条硬性差别；② **修复**：`kick_in_ticks = 0` 过去因为 `PlayerSafe >= 0` 恒真而等于"永不介入"，现在表示"完全不等待"（Legit 与 Basic 同步修正）；③ 测试夹具新增 `ResetTiles()`，几何块之间不再互相污染；④ 避障用例 26 个 |
| **1.2.2** | 2026-10-02 | **跳跃降级为最后手段 + 感知椭圆化（用户实测反馈）**：① `CPlanner::UpdateAvailability()` / `JumpEngaged()`：钩索期间（按住钩或 `HOOK_GRABBED/FLYING`）**一律不新增跳跃**，其余情况跳跃只在 `PlayerSafe <= JUMP_URGENCY_TICKS(6)` 的危急关头才放开，被拦下的候选既不搜索也不参与排序，理由字符串 `no safer plan (jump is a last resort)`；② 感知范围从圆改为**椭圆**：`ReachY = ReachX × SensingVerticalFactor()`，系数由地图 tuning 推出（默认 0.65），`ScanThreat()` 用 `TileBoxDelta()` 做每轴判据，威胁可视化画同一个椭圆；③ 新增 3 个用例（危急关头才跳 / 钩索期间不跳 / 椭圆竖平比），共 25 个避障用例；④ +1 条中俄词条；⑤ 自检脚本新增跳跃策略与椭圆断言 |
| **1.2.1** | 2026-10-02 | **感知半径半格化（用户反馈：半径 2 仍然太敏感）**：① `bc_avoid_sensing_radius` 单位从"图块"改为"**半格**"，范围 2~16 → **1~32**，默认 6 → **12**（等价）；② 到达判定从"图块索引正方形"改为"**到危险图块盒的欧氏距离 ≤ 半径**"，半格是真实步进、对角方向不再多覆盖 ≈41%；③ `SSettings::m_SensingRadius` 改为 `float`（图块数），`ScanThreat()`、`BuildEnvironment()` 与威胁可视化共用 `Avoid::DistanceToTileBox()`；④ 界面滑块与提示文案改为半格说明（+2 条中俄词条）；⑤ `cl_config_version` 1 → 2，旧配置启动时自动 ×2（autoexec 里的旧值需手动改，见 6.7）；⑥ `CAvoidSensingRadiusTest` 扩到 6 个用例（半格单调性 + 0.5 是"几乎关闭"档）；⑦ 自检脚本新增迁移与半格断言 |
| **1.2.0** | 2026-10-02 | **阶段三 Legit 交付**：① 决策引擎拆到 `avoid_engine.h/.cpp`（契约 / 感知 / 共享模拟器 / 身法门 / `Avoid::CPlanner`），`CAvoid::SSettings` 等名字用 `using` 别名保持兼容；② 候选空间 `方向(3)×跳跃(2)×钩子(3)`，玩家取值恒排第一；③ **钩子释放**：以"松钩候选的固定序列能否跑满 `check_ticks`"为唯一判据，配合 `hook_assist` / `hook_weight`；④ **UCT 搜索**：`quality`=迭代次数、`randomness`=探索常数+分支概率、三个 Weight=打分项，本地 PRNG 逐 tick 确定；⑤ **其他玩家预测**：私有 `CWorldCore` + ≤8 个影子核心（含开关门状态拷贝），`TickDeferred()` 互撞与钩索拖拽真实生效；⑥ `bc_avoid_unfreeze_ticks` 专用前瞻；⑦ **身法状态门**（喷气 / 钩人 / 飞锤）显式不动手并给出理由；⑧ Blatant/Fentbot/Pilot 改为显式占位；⑨ `avoid_status` 增加 `kick_in_ticks >= check_ticks` 警告；⑩ 19 条中俄词条；⑪ 新增 `src/test/avoid_legit_test.cpp`（12 个用例，含成本基准）；⑫ 文档新增 [6.8](#68-legit-代理实现说明v120) 与 [8.3b](#83b-legit-代理验收v120-起) |
| **1.1.0** | 2026-10-01 | **阶段二 Basic 交付**：① 前向模拟器 `CAvoid::SimulateInput()`（克隆 `CCharacterCore` + 地图 tuning + 复用 `ClassifyPoint()` 探测点）；② `CAvoid::EvaluateBestPlan()` 实现：快路径不干预、`KickInTicks` 介入阈值、`{-1,0,+1}` 三方向候选、NSIF 兜底，**只改 `m_Direction`**；③ `bc_avoid_log 1` 每决策日志；④ 12 条新界面/日志词条（中俄同步）；⑤ 新增保真度回归测试 `src/test/avoid_sim_test.cpp`（3 个用例）；⑥ 文档新增 [6.7](#67-basic-代理实现说明v110) 记录实现细节与已知不足 |

---

## 附录 D：回归自检脚本

脚本已随仓库提供，改完任何与避障相关的代码后直接执行：

```bash
#!/usr/bin/env bash
# BestClient - Avoid (Gores bot) module self check.
#
# Run this after touching anything related to the Avoid module:
#     ./scripts/avoid_selfcheck.sh
#
# See docs/AVOID_TECHNICAL_DOCUMENTATION.md (appendix D) for the rationale of every step.

set -e

cd "$(dirname "$0")/.."

if [ ! -f build/build.ninja ]; then
	echo "error: build/ is not configured; run cmake first" >&2
	exit 1
fi

echo "[1/6] building"
ninja -C build DDNet

echo "[2/6] bc_avoid_* config variables"
python3 - <<'EOF'
text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
n = text.count("MACRO_CONFIG_INT(BcAvoid") + text.count("MACRO_CONFIG_STR(BcAvoid")
assert n == 33, f"expected 33 bc_avoid_* cvars, found {n}"
print("  ok:", n)
EOF

echo "[3/6] localization entries"
python3 - <<'EOF'
keys = ["Avoid", "Assist mode", "Arm Avoid agent", "Check ticks", "NSIF on no safe input",
        "Basic", "Legit", "Blatant", "Input pipeline self-test",
        "Tee", "Nearest", "Hazard / sensed", "Safe ahead", "Overrides", "Status HUD",
        # stage 2 (Basic decision engine) reasons and log strings
        "brake before hazard", "steer left before hazard", "steer right before hazard",
        "player input safe", "still time before the hazard", "frozen, agent idle",
        # stage 3 (Legit decision engine) reasons, state gate and config hint
        "release hook before hazard", "release hook, brake before hazard",
        "release hook, steer left before hazard", "release hook, steer right before hazard",
        "release hook and jump before hazard",
        "press hook before hazard", "press hook, brake before hazard",
        "press hook, steer left before hazard", "press hook, steer right before hazard",
        "jump before hazard", "jump, brake before hazard",
        "jump, steer left before hazard", "jump, steer right before hazard",
        "no safer plan", "jetpack, hands off", "hooked to a player, hands off",
        "fly hammer, hands off", "agent not implemented yet",
        "warning: kick in ticks is not below check ticks, the agent will react late",
        # v1.2.1: sensing radius in half tiles
        "Radius (half tiles)",
        "no safer plan (jump is a last resort)",
        # v1.2.4: restore-defaults button
        "Defaults", "Restored", "Avoid: parameters restored to their defaults",
        "The sensing radius is how far ahead the agent may notice a hazard, in half tiles (12 = 6 tiles). Lower it to react later, raise it to react earlier; the scan itself costs almost nothing."]
for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for k in keys:
        assert "\n%s\n== " % k in text, (path, k)
print("  ok")
EOF

echo "[4/6] HUD module wiring"
python3 - <<'EOF'
assert "MODULE_AVOID," in open("src/game/client/components/hud_layout.h", encoding="utf-8").read()
t = open("src/game/client/components/hud_layout.cpp", encoding="utf-8").read()
assert '"avoid",' in t and '"Avoid",' in t and "case MODULE_AVOID:" in t
t = open("src/game/client/components/bestclient/hud_editor.cpp", encoding="utf-8").read()
assert "MODULE_AVOID" in t and "m_Avoid.RenderPreview()" in t
print("  ok")
EOF

echo "[5/6] render order (map foreground layer must come before the HUD pass)"
python3 - <<'EOF'
t = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
fg = t.index("\t\t\t\t\t      &m_MapLayersForeground,")
hud = t.index("\t\t\t\t\t      &m_Hud,")
tas = t.index("\t\t\t\t\t      &m_Tas, // bestclient")
avo = t.index("\t\t\t\t\t      &m_Avoid, // bestclient")
assert fg < hud < tas < avo, "HUD components must render after the map layers"
print("  ok: foreground < Hud < Tas < Avoid")
EOF

echo "[5b/6] menu: restore-defaults button"
python3 - <<'EOF'
import re
t = open("src/game/client/components/bestclient/menus_avoid.cpp", encoding="utf-8").read()
assert "AvoidDefaultParams(" in t and "pConfigManager->Reset(" in t, "the restore-defaults button is missing"
cvars = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
tables = re.findall(r'const char \*const (g_a\w+DefaultParams)\[\] = \{(.*?)\};', t, re.S)
assert tables, "the defaults tables are missing"
total = 0
for table_name, body in tables:
    names = re.findall(r'"(bc_avoid_[a-z_]+)"', body)
    assert names, table_name + " is empty"
    assert len(names) == len(set(names)), "duplicate entry inside " + table_name
    for name in names:
        assert (", %s," % name) in cvars, "unknown cvar in %s: %s" % (table_name, name)
    total += len(names)
print("  ok:", len(tables), "tables,", total, "entries")
EOF

echo "[6/6] decision engine wiring"
python3 - <<'EOF'
# --- the shared simulator and the agents ------------------------------------------------
t = open("src/game/client/components/bestclient/avoid_engine.cpp", encoding="utf-8").read()
assert "int SimulateFixed(" in t, "forward simulator is missing"
assert "SInputPlan CPlanner::Plan(" in t, "the Legit planner is missing"
# Sensor reuse: the engine has to ask the stage 1 probe rules, never a private copy.
assert "IsRelevantHazard(Set, ClassifyPoint(pCollision, m_Sim.m_Core.m_Pos))" in t, \
    "the simulator must reuse ClassifyPoint()/IsRelevantHazard()"
assert "m_Core.m_Tuning = Src.m_Tuning" in t, "the simulator must use the map tuning, not constants"
# Candidate space: direction x jump x hook, with the player's own values first.
assert "MAX_ROOT_ACTIONS" in t and "const int aJumps[2]" in t and "int aHooks[3]" in t, \
    "the Legit candidate space is not direction x jump x hook"
# Search: UCT with a deterministic local PRNG, a quality-driven iteration count and a budget guard.
assert "m_Prng.Seed(" in t, "the search must use a local, seeded PRNG"
assert "rand()" not in t and "srand" not in t, "no global RNG in the decision path"
assert "MeanScore(" in t and "RankingScore(" in t, "the UCT value functions are missing"
assert "SEARCH_BUDGET_MS" in t, "the search has no wall clock guard"
# Player prediction: a private world carrying the snapshots.
assert "m_aShadows[NumShadows] = Snapshot.m_Core" in t and "m_World.m_apCharacters[m_aShadows[" in t, \
    "the predicted players are not injected into the clone world"
assert "Env.m_PredictPlayers" in t, "bc_avoid_player_prediction is not wired into the simulator"
# Unfreeze lookahead and the movement state gate.
assert "m_UnfreezeTicks" in t, "bc_avoid_unfreeze_ticks is not wired into the engine"
# Jump policy: a rope problem is never solved by hopping, and the jump waits for the critical
# moment. Both live in UpdateAvailability()/JumpEngaged().
assert "JUMP_URGENCY_TICKS" in t and "void CPlanner::UpdateAvailability(" in t, \
    "the jump is not held back until the critical moment any more"
assert "bool CPlanner::JumpEngaged(" in t and "HOOK_GRABBED" in t, \
    "jumps are no longer suppressed while the hook is engaged"
# The sensing reach is an ellipse derived from the map tuning, not a circle.
assert "float SensingVerticalFactor(" in t and "SENSING_VERTICAL_FALLBACK" in t, \
    "the sensing reach is not the tuning derived ellipse any more"
assert "TileBoxDelta(Pos, Tx, Ty)" in t, "the sensor does not use the per axis tile box distance"
# The sensing radius is a radius in tiles with half tile steps, measured to the hazard box.
assert "const float RadiusX = std::clamp(Set.m_SensingRadius, 0.5f, 16.0f);" in t, \
    "the sensing radius is not applied as a fractional distance to the hazard box"
assert "int ClassifyMovement(" in t, "the movement state gate is missing"
assert "ClassifyMovement(Ctx.m_Core, FlyHammerState(Ctx)" in open(
    "src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read(), \
    "the client component does not apply the movement state gate"

# --- config compatibility ----------------------------------------------------------------
# `bc_avoid_sensing_radius` changed from tiles to half tiles; the migration has to stay, together
# with the version bump that makes it run exactly once.
t = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
assert "bc_avoid_sensing_radius, 2, 1, 32" in t, "bc_avoid_sensing_radius is not in half tiles any more"
t = open("src/engine/client/client.cpp", encoding="utf-8").read()
assert "g_Config.m_BcAvoidSensingRadius = std::clamp(g_Config.m_BcAvoidSensingRadius * 2, 1, 32);" in t, \
    "the half tile migration of bc_avoid_sensing_radius is missing"
assert "g_Config.m_ClConfigVersion = 2;" in t, "the config version was not bumped for the migration"
t = open("src/engine/shared/config_variables.h", encoding="utf-8").read()
assert "ClConfigVersion, cl_config_version, 2," in t, \
    "a fresh config must already be at version 2, otherwise it would migrate its own default"

# --- the client component ---------------------------------------------------------------
t = open("src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read()
assert "int CAvoid::SimulateInput(" in t, "forward simulator wrapper is missing"
assert "CAvoid::SInputPlan CAvoid::EvaluateBestPlan(" in t, "decision engine is missing"
# The sensing radius has to gate the engine, otherwise the setting only moves the HUD readout.
assert "!Ctx.m_Threat.m_HasNearest" in t, "the sensing radius gate is missing from the decision engine"
assert "m_Controls.m_aInputData" not in t.replace("`m_Controls.m_aInputData`", ""), "avoid must never write the raw key state buffer"
assert "STAGE 2 IMPLEMENTATION SLOT" not in t, "the stage 2 slot should be filled in now"
# Basic must still only rewrite the direction: the Legit agent is the one that touches jump/hook,
# and it lives in avoid_engine.cpp. Reads of the fields (the log line) are fine, writes are not.
for forbidden in (".m_Hook = ", ".m_Jump = ", ".m_Fire = "):
    assert forbidden not in t, f"avoid.cpp must not write {forbidden.strip()}"
assert "Plan.m_Input.m_Direction = BestDir" in t, "Basic must only change m_Direction"
print("  ok")
EOF

echo "[6b/6] simulator fidelity + sensing radius + Legit agent tests"
ninja -C build testrunner
./build/testrunner --gtest_filter='CAvoidSimulatorTest.*:CAvoidSensingRadiusTest.*:CAvoidLegitTest.*'

echo "all checks passed"
```

## 附录 E：与 TAS 文档的分工

| 文档 | 覆盖范围 |
| :--- | :--- |
| [`docs/TAS_TECHNICAL_DOCUMENTATION.md`](TAS_TECHNICAL_DOCUMENTATION.md) | TAS 录制/回放、本地沙盒（FastPractice）、检查点、`.tas` 文件格式、DF/HDF |
| 本文档 | TAS& 菜单下的 **避障 / Avoid** 子模块：感知、决策（Basic 已实现，见第 6 章）、HUD、参数面 |

两者共享同一套基础设施，交叉点只有三处，改动时注意不要互相破坏：

1. `CTas` 与 `CAvoid` 都在 `CGameClient::OnSnapInput()` 里挂载，**TAS 回放优先级更高**，避障在回放态完全不介入。
2. `CAvoid::ActiveCore()` 会通过 `CFastPractice::ResolvePracticeRoles()` 读取本地沙盒角色，
   所以 TAS 录制期间避障的感知仍然正确。
3. 两者都是 HUD 组件，**必须一起留在 HUD 渲染阶段**（见 [5.3 红线](#53-游戏内-hud-模块已接入-hud-编辑器与威胁可视化)）。

---

## 附录 F：交付分档与路线图

避障模块按“**每个档位都能独立验收、下一档只做增量**”的方式推进。每档的产出一份任务书、
一次提交、一个 tag，并且**必须给下一档留下可复用的地基**。

### F.1 分档总表

| 档位 | 模式 | 状态 | 任务书 | 核心增量 | 验收锚点 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **阶段一** | 全部 | ✅ v1.0.2 | 已归档 | 界面 + 33 cvar + 危险感知层 + 输入拦截管线 + HUD/可视化 + 本地化 | 界面验收清单（§8.1）、感知层验证（§8.2） |
| **阶段二** | **Basic** | ✅ **v1.1.1** | [`AVOID_STAGE2_BASIC_PROMPT.md`](AVOID_STAGE2_BASIC_PROMPT.md) | **前向模拟器** + 方向键制动决策引擎（只改 `m_Direction`） | 走向危险被刹停、荡向危险被减速、窄通道不误触发 |
| **阶段三** | **Legit** | ✅ **v1.2.0** | [`AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md) | 钩子（含**提前松钩**）+ 跳跃 + **UCT 搜索** + **其他玩家预测** + 解冻前瞻 + 身法门 | 勾向黑水墙前脱钩；`Quality`/`Randomness`/三个 Weight 真实生效（[§8.3b](#83b-legit-代理验收v120-起)） |
| **阶段四** | **Blatant** | ⬜ 下一档 | [`AVOID_STAGE4_BLATANT_PROMPT.md`](AVOID_STAGE4_BLATANT_PROMPT.md) | Track Point、Safe Aim Tracking、Auto Drag、内置瞄准、NSIF 调优 | 极端 Gores 图上的生存率与瞄准辅助 |
| **阶段五** | **Fentbot** | ⬜ | 由阶段四交付后编写 | Fent Ticks / Tweaker 系列（当前为灰色占位） | 与参考实现 Fent 行为对齐 |
| **阶段六** | **Pilot** | ⬜ | 由阶段五交付后编写 | 种群 / 探索深度 / Top-K / 序列长度（当前为灰色占位） | 整段动作序列搜索 |

### F.2 每档必须留下的地基（验收时会检查）

| 档位 | 留给下一档的东西 |
| :--- | :--- |
| 阶段二 | `CAvoid::SimulateInput()`（**三个代理的公共模拟器**）、`bc_avoid_log` 逐决策日志、`CAvoidSimulatorTest` 保真度回归、`CAvoidSensingRadiusTest` 合成地图夹具 |
| 阶段三 | ✅ `avoid_engine.h/.cpp`：候选生成器（方向/跳跃/钩子）、UCT 打分器（`MeanScore`/`RankingScore`）、玩家快照注入点（`SEnvironment` + 私有 `CWorldCore`）、钩子释放的判据、身法状态门、`CAvoidLegitTest` 合成地图夹具 + 成本基准 |
| 阶段四 | 瞄准/落点搜索（在 `SInputPlan` 的 `m_TargetX/Y` 上做，候选生成器已经预留了位置）、Track Point 记忆、Auto Drag 的队友选择；注意 [6.8.9](#689-已知不足留给阶段四--阶段五) 的 4 条偏差 |

> [!IMPORTANT]
> **模拟器是所有档位的公共地基**。任何档位都不许复制一份自己的物理步进；
> 需要新物理（例如角色互撞）就在 `SimulateInput()` 的克隆配置里加开关，
> 并给它补一条 `CAvoidSimulatorTest` 的保真度用例。

### F.3 参数接线检查表（本项目踩过的坑）

**每一个 `bc_avoid_*` 参数都必须有“改了就一定有可观察差别”的证据**，否则就是没接线。
v1.1.1 修过一次这类 bug：`bc_avoid_sensing_radius` 只喂给了感知层，
决策引擎自己逐帧查图块时没有距离上限，导致滑条怎么调都在同一距离被刹住。

| 参数 | 生效档位 | 怎么验证 |
| :--- | :--- | :--- |
| `bc_avoid_sensing_radius` | 二/三/四/五/六 | `CAvoidSensingRadiusTest.*`（6 个用例，含半格单调性）；游戏内：12（6 格）→ 4（2 格）→ 2（1 格）应逐级更晚减速（§8.3 第 8 行） |
| `bc_avoid_direction_assist` | 二 | 关闭后代理不再改方向（Basic 只剩玩家自己的方向） |
| `bc_avoid_check_ticks` | 二/三 | 前瞻窗口；调小 → 反应更晚，调大 → 更早但更贵 |
| `bc_avoid_kick_in_ticks` | 二/三 | ≥ 26（> check_ticks）会导致代理永不介入，见 F.4 |
| `bc_avoid_tile_*` | 二/三 | 关掉“死亡块”后走向黑水不再被拦（§8.3 第 7 行） |
| `bc_avoid_hook_assist` / `m_HookWeight` | **三 ✅** | `CAvoidLegitTest.HookWeightChangesTheHookDecision`；游戏内：`Hook priority` 拉满/归零看 `plan d/j/h` 的 h |
| `bc_avoid_player_prediction` | **三 ✅** | `CAvoidLegitTest.PlayerPredictionChangesTheDecision`（关掉后与"场上没人"逐字相同） |
| `bc_avoid_unfreeze_ticks` | **三 ✅** | `CAvoidLegitTest.UnfreezeTicksSetTheLookaheadNearUnfreezeTiles` |
| `bc_avoid_quality` / `randomness` / 三个 Weight | **三 ✅** | `QualityDrivesSearchEffortAndTheDecision`、`RandomnessChangesTheSearchWithoutBreakingIt`、`HookWeightChanges*`、`LegitDecisionCostStaysInsideTheTickBudget` |
| `bc_avoid_track_point` / `safe_aim_tracking` / `auto_drag` / `aimbot*` | **四** | 阶段四任务书负责 |

### F.4 已知的“配置陷阱”（写进用户文档，也写进下一档任务书）

0. **`kick_in_ticks = 0` 曾经等于"永不介入"**（v1.2.2 及以前）：判据是 `PlayerSafe >= KickIn`，
   而任何数都 `>= 0`，于是滑条最低档静默地变成了"关闭"。v1.2.3 起改成 `KickIn > 0 && PlayerSafe >= KickIn`，
   最低档现在表示"**完全不等待**"（感知范围一到就出手）。Basic 与 Legit 共用这条修正。
1. **`kick_in_ticks` ≥ `check_ticks` 会把干预窗口压到 0**：
   当前规则是“玩家输入还能安全 ≥ `KickInTicks` 就不干预，否则才搜索”，
   而搜索的前瞻只有 `CheckTicks` 帧。所以 `KickInTicks ≥ CheckTicks`（默认 20 < 26）时，
   代理只会在“已经来不及完整挽救”的局面里出手 —— 表现为**介入明显偏晚**，
   而且永远走 NSIF 兜底（红徽章）。
   **v1.2.0 已加**：`avoid_status` 会在这种情况下打印一条本地化警告
   （`warning: kick in ticks is not below check ticks, the agent will react late`）。
2. **感知半径很大时改由 `check_ticks` 决定介入距离**（见 §6.7）：
   半径超过约 8 图块后在平地上就感觉不出差别了，这是预期行为。
3. **网络延迟**：参考实现要求 `cl_prediction_margin` 略高于 ping（如 50 ms ping 设 70）。

### F.5 与 TAS「自动位移红线」的关系（每档都必须复核）

TAS 文档 3.11.7 立的红线是：**严禁在未获玩家显式授权下自动注入角色位移输入
（`m_Direction = -1 / 1`）**，尤其严禁在飞锤（DF / HDF）时自动走动。
避障模块与这条红线的关系必须分档说清，因为它**确实**会写 `m_Direction`：

| 场景 | 当前行为 | 判断 |
| :--- | :--- | :--- |
| 飞锤 / HDF 时的**分身**连接 | 避障只挂在 `OnSnapInput` 的 `!Dummy` 分支，**永远不碰分身** | ✅ 不违反红线（红线针对的正是分身自动走动） |
| 玩家在飞锤时**操作本体**（按住方向键） | **v1.2.0 起显式不动手**：`DummyConnected() && cl_dummy_hammer && 离地` → `飞锤中，代理不介入`（[6.8.6](#686-身法状态门tas-红线-3117-的显式化)） | ✅ 已覆盖 |
| 深度冻结 / 活冻结 / 常规冻结中 | `HAZ_SELF` 直接不干预 | ✅ 安全 |
| 喷气背包（`m_Jetpack`）中 | **v1.2.0 起显式不动手**：`喷气背包中，代理不介入` | ✅ 已覆盖 |
| 钩住其他玩家（被拖拽）时 | **v1.2.0 起显式不动手**：`钩住玩家中，代理不介入` | ✅ 已覆盖 |
| TAS 录制 / 回放中 | 录制记录**原始**输入（避障在记录之后运行）；回放态完全不介入 | ✅ 录像纯净 |

**结论**：避障的自动位移是"**为了防止立即死亡**"，而不是"帮你走图"，所以语义上站得住。
阶段三点名的两件事都已经落地：

1. ✅ 飞锤 / 深飞 / 喷气背包 / 钩住玩家都有一个**显式条件**写进决策前置检查
   （`Avoid::ClassifyMovement()`，对所有代理生效），并在日志与 `Plan.m_aReason` 里可见；
2. ✅ 文档与 UI 上都写明：**避障永远不会碰分身连接**（`OnSnapInput` 的 `!Dummy` 分支），
   本体只在"继续按当前键会在 `kick_in_ticks` 内致死、且还没到该阈值"时才被改写。
