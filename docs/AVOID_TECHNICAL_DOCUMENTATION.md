# BestClient 避障 (Avoid / Gores Bot) 技术架构与开发交付文档

> **面向后续开发人员与 AI Agent 的完整技术规范**
> **文档版本**: 1.1.1 (阶段二 Basic 交付 + 感知半径介入距离修复)
> **适用代码分支**: `feature/tas`
> **最后更新**: 2026-10-01 (v1.1.1: 感知半径闸门，介入距离随滑条变化)
> **本阶段交付**: 完整界面 + 完整输入管线 + 完整危险感知层 + **Basic 决策引擎（Legit / Blatant 留待阶段三）**

---

## 速览：下一个 AI 先读这 8 条

> 这份文档是**自解释**的：只读本节也能安全接手。详细内容在后续章节。

1. **任务来源**：复刻参考客户端（KRX，闭源）的 Avoid / Gores Bot 功能，需求原文见 [1.1](#11-需求原文用户描述的目标体验)。
2. **当前进度**：界面、配置、危险感知层、输入拦截管线、HUD 全部完成（[0.1](#01-本次做了什么)），
   **Basic 代理的决策引擎已完成（v1.1.0）**；Legit / Blatant / Fentbot / Pilot 的算法仍待实现。
3. **决策引擎入口**：`CAvoid::EvaluateBestPlan(const SContext &Ctx)`，位于
   `src/game/client/components/bestclient/avoid.cpp`。它调用的前向模拟器是
   `CAvoid::SimulateInput()` —— **后面所有代理（含 MCTS）都复用它**，见 [6.7](#67-basic-代理实现说明v110)。
4. **不要改接口**：`SContext`（输入）与 `SInputPlan`（输出）已经接好写回通路、性能计时、UI 显示与状态机，
   直接填算法即可，见 [6.1](#61-函数签名与契约)。
5. **写回方式**：只需要填 `Plan.m_Input` 并置 `Plan.m_Override = true`，
   `ApplyInput()` 会把它写进 `pData`。**绝对不要**写 `m_Controls.m_aInputData`（原因见 [3.2](#32-为什么不写-m_controlsm_ainputdata)）。
6. **实时性已经铺好**：代理启用时 `SnapInput()` 强制 50 Hz 发包（[3.5](#35-启用代理时强制-50-hz-输入发送阶段一已实现关键)），
   `ApplyInput()` 每 tick 调用一次，用 `m_LastDecisionTick` 去重。
7. **危险判定必须复用** `ClassifyPoint()` / `IsRelevantHazard()`，探测点与 DDNet 引擎逐条对齐（[4.2](#42-探测点规则与-ccharacter-完全一致避免误判)）。
8. **改完必须做的三件事**：`ninja -C build DDNet`；跑 [附录 D](#附录-d回归自检脚本) 的自检脚本；
   更新本文档的 [附录 C 变更历史](#附录-c变更历史)。

**阶段三任务书已经写好在 [附录 A](#附录-a阶段二任务书可直接交给下一个-ai)，可以整体复制给下一个 AI。**

> 推进方式：**Basic 已完成**（[`docs/AVOID_STAGE2_BASIC_PROMPT.md`](AVOID_STAGE2_BASIC_PROMPT.md) 的任务书，
> 覆盖三条验收里的"走"与"荡"）。
> **下一档是 Legit**：[`docs/AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md)
> （钩子释放 + 跳跃 + MCTS + 其他玩家预测），整档交给下一个 AI 即可。
> 分档与依赖关系见 [附录 F](#附录-f交付分档与路线图)；
> 附录 A 是最初的完整阶段二任务书，其中"第 1 步 前向模拟器"**已经做完**
> （`CAvoid::SimulateInput()`，见 [6.7](#67-basic-代理实现说明v110)），从"第 2 步"之后的思路仍然有效，
> 但**不要再直接把附录 A 当任务书下发**（它把 Track Point / 内置瞄准和 Legit 混在一起了，
> 那两项属于阶段四 Blatant）。

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

> [!IMPORTANT]
> **阶段三需要做的是**：把候选集从"三个方向键"扩展到"方向 × 跳跃 × 钩子 × 瞄准"，
> 并把打分换成 MCTS。**前向模拟器（D14）不用重写**——它是所有代理的公共地基。
> 详见 [第 6 章](#6-决策引擎实现指南) 与 [6.7](#67-basic-代理实现说明v110)。

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

**仍然不做的事**（属于阶段三）：

* **不会松开钩子**：朝贴有黑水的墙勾过去时，Basic 只会用左右键减速，不会提前脱钩（验收第 3 条留给阶段三）；
* **不会跳跃、不会瞄准、不会开火**：Basic 只改 `m_Direction`，其余字段原样保留玩家输入；
* **不预测其他玩家**：模拟器只推算自己（`bc_avoid_player_prediction` 目前不影响 Basic）；
* Legit / Blatant / Fentbot / Pilot 的算法仍是占位（切到这些模式不会生效，状态栏会显示原因）。

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
| `settings.md` | `cl_prediction_margin` 建议、传送预测、死亡图块预测、移动限制预测、玩家体积预测 |

> [!NOTE]
> 参考实现的 Basic 代理是**免费版**功能，只用左右方向键；Legit/Blatant 是**基于蒙特卡洛树搜索 (MCTS)** 的
> 前向模拟（文档里 `krx_avoid_num_iterations` = 迭代次数、`krx_avoid_tile_exploration_constant` = UCT 探索常数、
> `krx_avoid_tile_lifespan_weight` = 生存权重，全部是 MCTS 术语）。本模块的参数命名与语义与之对齐，
> 阶段二可以直接沿用这套搜索框架。

---

## 2. 代码结构与文件地图

| 模块/文件 | 路径 | 核心职责 |
| :--- | :--- | :--- |
| **避障核心组件** | `src/game/client/components/bestclient/avoid.h`<br>`src/game/client/components/bestclient/avoid.cpp` | 状态机、配置快照、危险感知、输入管线、决策引擎（空槽）、游戏内 HUD、世界空间可视化、控制台命令 |
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

* 以角色为中心，扫描 `bc_avoid_sensing_radius`（默认 6）图块半径内的正方形区域，
  即默认 13 × 13 = 169 个图块采样点。
* 每个采样点取图块中心 `((Tx + 0.5) * 32, (Ty + 0.5) * 32)` 调 `ClassifyPoint()`。
* 若命中且 `IsRelevantHazard()`（按 `bc_avoid_tile_*` 开关过滤），计入 `m_HazardTiles`，
  并用**角色点到图块 AABB 的最近距离**更新 `m_NearestPos / m_NearestDistPx`。
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
> 这样界面已经完整表达了参考客户端的参数面，阶段三实现算法时把它们换成真实控件即可。

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

> **实现状态（v1.1.0）**：**Basic 部分已实现** —— 前向模拟器 `CAvoid::SimulateInput()` 与
> 只改 `m_Direction` 的三方向候选决策都在 `avoid.cpp` 里跑通了（见 [6.7](#67-basic-代理实现说明v110)）。
> **Legit / Blatant / Fentbot / Pilot 仍待实现**：候选集扩展（跳跃 / 钩子 / 瞄准）、
> MCTS + NSIF 搜索、钩子释放、Track Point、Auto Drag、内置瞄准都还是空的，
> 参数面板里这些代理的参数目前不影响任何行为。
> 6.1–6.6 仍然是把它们接上去时的契约与参考实现指南；6.7 记录 Basic 实际是怎么写的。

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
	SSettings m_Settings;            // 完整配置快照（见 avoid.h）
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

> 这一节记录 6.1–6.6 落实成代码时**实际**做的选择，以及留给阶段三的已知不足。

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

**感知半径的作用（`bc_avoid_sensing_radius`，2~16 图块）：**

模拟器本身**没有**距离上限——它把 tee 一帧帧往前推，推到哪里就查哪里的 `ClassifyPoint()`。
这让物理预测很精确，但也意味着**如果不加限制，代理会去躲感知范围之外的十万八千里外的危险**。
所以决策引擎的第 0 层（上表第 2 行）显式用感知层的结果做闸门：

* `ScanThreat()` 只会对通过 `IsRelevantHazard()` 的图块置 `m_HasNearest`，
  所以这一个标志同时折叠了**感知半径**与全部 `bc_avoid_tile_*` 开关；
* 半径越大 → 越早发现 → **刹车点离危险越远**；半径越小 → 越晚介入；
* 闸门同时受 `KickInTicks` 约束：**它只决定"能不能开始管"，不决定"管多久"**。
  半径很大时，真正拦住代理的是 `check_ticks` 的前瞻窗口，
  所以半径超过约 8 图块后，再往上调在平地上就感觉不出差别了（这点已被测试固定下来）。

实测（`CAvoidSensingRadiusTest`，合成地图、平地全速行走、`check_ticks = 26`）：

| 感知半径 | 开始刹车的距离（tee 中心 → 危险图块边缘） |
| :--- | :--- |
| 2（下限） | ≈ 2 图块（64 px），刹停后仍留有余量 |
| 6（默认） | ≈ 6 图块（190 px） |
| 8 ~ 16 | 不再增长（改由 `check_ticks` 决定） |

> [!IMPORTANT]
> **半径下限是安全的**：以默认 tuning（`ground_control_speed = 10`、
> `ground_control_accel = 2 px/tick²`）计算，从全速刹停只需要 **5 帧、约 25 px**，
> 在半径 2（64 px 余量）内绰绰有余。所以整条滑条都是可用的，
> 只是"反应早晚"不同，不存在"调到最小就刹不住"的情况。

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

**已知不足（留给阶段三）**：

1. 不预测其他玩家：被队友/敌人撞进危险的场景不会被拦（`bc_avoid_player_prediction` 无效）。
2. 不松钩：勾向黑水墙时只能反向减速，"提前脱钩"要等阶段三（见 6.5 第 6 条）。
3. 候选集只有三个方向键：没有跳跃、没有瞄准，所以"必须跳一下才能活"的局面会退化成 NSIF。
4. 前瞻是从**上一 tick 的核心状态**（`m_aClients[].m_Predicted`）出发的，比当前渲染帧晚 1 tick；
   20 ms 的滞后在 26 帧窗口里可以忽略，但阶段三做钩索释放时值得复核。
5. `Randomness` / `Quality` / `HookWeight` 等参数对 Basic 无影响（Basic 是确定性穷举，忠于参考实现）。

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
| `bc_avoid_sensing_radius` | int | `6` | 2~16 | 感知半径（图块） |
| `bc_avoid_show_hud` | int | `1` | 0~1 | AVOID HUD 总开关（还需 HUD 编辑器里的 `Avoid` 模块处于开启状态） |
| `bc_avoid_show_visuals` | int | `0` | 0~1 | 世界空间威胁可视化 |
| `bc_avoid_log` | int | `0` | 0~1 | 决策日志输出到控制台 |
| `bc_avoid_debug_override` | int | `0` | 0~1 | **输入管线自检**：强制每帧接管并反转左右方向键，用于验证拦截管线（见 [8.4](#84-输入拦截管线验证bc_avoid_debug_override)） |

### 7.1 控制台命令

| 命令 | 参数 | 说明 |
| :--- | :--- | :--- |
| `avoid_toggle` | - | 启用/解除当前代理（等价于 `toggle bc_avoid_active 1 0`） |
| `avoid_status` | - | 打印状态、感知数据、决策计数与耗时 |
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

### 8.2 危险感知层验证

前提：进入一张有黑水/冻结块的图（在线状态），**不需要**启用代理，只要"启用避障模块"是打开的（默认开）。
观察左栏 **实时状态** 卡片：

| 操作 | 期望观察 | 证明了什么 |
| :--- | :--- | :--- |
| 朝黑水走过去 | `最近` 行距离**持续减小**（单位：图块，1 图块 = 32 px），到达黑水边缘时接近 `0.0` | 半径扫描与最近威胁计算实时工作 |
| 踩进黑水/冻结块 | `角色` 行显示 `DEATH`（红）或 `FREEZE`（蓝），并给出角色图块坐标 | 自身状态判定与图块分类正确 |
| 站在空旷处 | `危险 / 已探测` 的分子为 0，`最近` 行显示 `范围内无危险` | 没有误报 |
| 把 `Tiles` 面板的 **死亡块** 取消勾选 | `危险 / 已探测` 的分子立刻下降，`角色` 行不再显示 DEATH | 分类结果按配置实时过滤（`IsRelevantHazard`） |
| 拖动 **感知半径** | `危险 / 已探测` 的**分母**变化：半径 2 → 25，半径 6 → 169，半径 16 → 1089（公式 `(2r+1)²`） | 扫描半径真的改变了采样规模 |

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
| 8 | 把 **感知半径** 从 6 调到 2，再朝黑水走 | **明显更晚**才开始减速（半径 6 约 6 图块外就刹车，半径 2 约 2 图块）；调到 12 则与 6 差不多 | 半径是真正的介入距离控制（见 6.7） |
| 9 | 反复开关代理再走一段 | 不崩溃、不卡顿、无异常日志 | 状态机与缓存计划（`m_LastPlan`）安全 |

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

感知半径另有一组测试（`src/test/avoid_sensing_radius_test.cpp`，自带合成地图所以数字是精确的）：

```bash
./build/testrunner --gtest_filter='CAvoidSensingRadiusTest.*'
```

| 用例 | 锁住的行为 |
| :--- | :--- |
| `BrakesInTimeAcrossTheWholeRadiusRange` | 半径 2~16 每一档都能在撞上危险前刹停 |
| `WiderRadiusReactsFurtherFromTheHazard` | 半径越大刹车点越远（半径 6 比半径 2 早 4 图块以上） |
| `MinimumRadiusStillFitsTheBrakingDistance` | 最小半径下"发现危险 → 需要刹停"仍有足够帧数 |
| `CheckTicksTakesOverAtLargeRadii` | 半径很大时改为由 `check_ticks` 决定，反应距离不再增长 |

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

# 4. 决策引擎实现存在（v1.1.0 起不再是空槽），且模拟器回归测试通过
grep -n "CAvoid::SimulateInput" src/game/client/components/bestclient/avoid.cpp
ninja -C build testrunner && ./build/testrunner --gtest_filter='CAvoidSimulatorTest.*'
```

---

## 10. 已知限制与风险

| 风险 | 说明 | 处理建议 |
| :--- | :--- | :--- |
| **Basic 只做方向键制动** | 不松钩、不跳跃、不瞄准（见 [6.7](#67-basic-代理实现说明v110) 已知不足） | 阶段三按附录 A 接钩子释放与 MCTS |
| **模拟器不预测其他玩家** | 克隆体没有世界，`TickDeferred()` 被跳过，被撞进危险不会被拦 | 阶段三做 `m_PlayerPrediction` 时把其他角色位置作为静态障碍/速度场补进模拟 |
| **状态徽章为英文短标签** | `WATCH / ASSIST / NSIF / AFK` 未本地化，与 TAS 页 `IDLE/REC` 风格一致 | 如需中文可在 `RenderSettingsAvoid` 里改用 `BcLocalize` |
| **HUD 只在启用时显示** | 未启用代理且未开可视化时，游戏内不会出现 AVOID HUD（避免打扰普通玩家） | 如需常驻可自行放开 `CAvoid::OnRender()` 里的条件 |
| **`bc_tas_tab` 语义变化** | 原 0=TAS / 1=辅助模块；现 0=TAS / 1=避障 / 2=辅助模块。老配置里值为 1 的用户打开 TAS& 会直接看到避障页 | 已知且可接受；如需兼容可加一次性迁移 |
| **网络延迟下预测漂移** | 参考实现明确要求 `cl_prediction_margin` 略高于 ping（如 50ms ping 设 70） | 文档提示即可；后续可在 UI 上给出建议值 |
| **UiScale 极高时底部裁切** | 布局总高 462 px，UiScale 110 时可用约 471 px，留有余量但不大 | 新增控件时必须复核 [5.1 节](#51-布局严格遵循左上角状态栏--左栏模式与状态--右栏参数) 的高度预算 |
| **沙盒/实战感知一致性** | `ActiveCore()` 已兼容 TAS 本地沙盒，模拟器也改成从沙盒世界的 `m_Core` / `Teams()` 取世界与队伍指针 | 见 6.3 方案 B 的风险栏 |
| **Fentbot / Pilot 仅有 UI** | 参数为只读占位，不含算法 | 阶段三实现；阶段二不要顺手改它们的语义 |

---

## 11. 与 TAS 模块的边界（避免互相破坏）

| 场景 | TAS 行为 | 避障行为 | 结论 |
| :--- | :--- | :--- | :--- |
| TAS 录制中 | `OnRecordInput()` 记录原始输入 | 在记录**之后**运行，只改发出去/预测用的 `pData` | 录进 `.tas` 的是玩家原始输入，轨迹纯净 |
| TAS 回放中 | `OnSnapInput` 直接返回预录制数据 | `IsPlaybackActive()` 时提前 return，不介入 | 回放逐帧精确，不受避障干扰 |
| TAS 本地沙盒激活 | `CFastPractice::Active()`，物理由沙盒驱动 | 感知层自动切换到沙盒角色 | 练图时感知正确 |
| 分身连接 | `Dummy == true` 分支 | 只在 `!Dummy` 分支挂载 | 永远不会动到分身输入 |

---

## 12. 变更文件总览（本阶段）

```
新增:
  src/game/client/components/bestclient/avoid.h
  src/game/client/components/bestclient/avoid.cpp
  src/game/client/components/bestclient/menus_avoid.cpp
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

*阶段二 Basic 交付完成（v1.1.0）。下一步：按 [第 6 章](#6-决策引擎实现指南) 与附录 A 实现钩子释放与 MCTS 搜索，不要改动任何既有接口。*


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
| `avoid.cpp:578` | `CAvoid::SimulateInput()` | **阶段二前向模拟器**：克隆 `CCharacterCore` 推演 N 帧，返回存活帧数 |
| `avoid.cpp:683` | `CAvoid::EvaluateBestPlan()` | **Basic 决策引擎**：快路径 → 介入阈值 → 三方向候选 → NSIF |
| `avoid.cpp:667` | `CAvoid::LogTrace()` | `bc_avoid_log 1` 的每决策日志（校准模拟器用） |
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
| `avoid.h:204` | `MAX_CANDIDATES` / `MAX_SIM_TICKS` / `m_aSamples[]` | 决策路径的定长缓冲（无堆分配） |
| `src/test/avoid_sim_test.cpp` | `CAvoidSimulatorTest` | 模拟器保真度回归（3 个用例） |
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

echo "[1/5] building"
ninja -C build DDNet

echo "[2/5] bc_avoid_* config variables"
python3 - <<'EOF'
text = open("src/engine/shared/config_variables_bestclient.h", encoding="utf-8").read()
n = text.count("MACRO_CONFIG_INT(BcAvoid") + text.count("MACRO_CONFIG_STR(BcAvoid")
assert n == 33, f"expected 33 bc_avoid_* cvars, found {n}"
print("  ok:", n)
EOF

echo "[3/5] localization entries"
python3 - <<'EOF'
keys = ["Avoid", "Assist mode", "Arm Avoid agent", "Check ticks", "NSIF on no safe input",
        "Basic", "Legit", "Blatant", "Input pipeline self-test",
        "Tee", "Nearest", "Hazard / sensed", "Safe ahead", "Overrides", "Status HUD",
        # stage 2 (Basic decision engine) reasons and log strings
        "brake before hazard", "steer left before hazard", "steer right before hazard",
        "player input safe", "still time before the hazard", "frozen, agent idle"]
for path in ("data/BestClient/languages/simplified_chinese.txt",
             "data/BestClient/languages/russian.txt"):
    text = open(path, encoding="utf-8").read()
    for k in keys:
        assert "\n%s\n== " % k in text, (path, k)
print("  ok")
EOF

echo "[4/5] HUD module wiring"
python3 - <<'EOF'
assert "MODULE_AVOID," in open("src/game/client/components/hud_layout.h", encoding="utf-8").read()
t = open("src/game/client/components/hud_layout.cpp", encoding="utf-8").read()
assert '"avoid",' in t and '"Avoid",' in t and "case MODULE_AVOID:" in t
t = open("src/game/client/components/bestclient/hud_editor.cpp", encoding="utf-8").read()
assert "MODULE_AVOID" in t and "m_Avoid.RenderPreview()" in t
print("  ok")
EOF

echo "[5/5] render order (map foreground layer must come before the HUD pass)"
python3 - <<'EOF'
t = open("src/game/client/gameclient.cpp", encoding="utf-8").read()
fg = t.index("\t\t\t\t\t      &m_MapLayersForeground,")
hud = t.index("\t\t\t\t\t      &m_Hud,")
tas = t.index("\t\t\t\t\t      &m_Tas, // bestclient")
avo = t.index("\t\t\t\t\t      &m_Avoid, // bestclient")
assert fg < hud < tas < avo, "HUD components must render after the map layers"
print("  ok: foreground < Hud < Tas < Avoid")
EOF

echo "[5b/5] decision engine wiring (Basic agent)"
python3 - <<'EOF'
t = open("src/game/client/components/bestclient/avoid.cpp", encoding="utf-8").read()
# The Basic agent must exist and must still route every hazard decision through the stage 1
# sensing helpers instead of a private copy of the probe rules.
assert "int CAvoid::SimulateInput(" in t, "forward simulator is missing"
assert "CAvoid::SInputPlan CAvoid::EvaluateBestPlan(" in t, "decision engine is missing"
assert "IsRelevantHazard(ClassifyPoint(Sim.m_Pos))" in t, "simulator must reuse ClassifyPoint/IsRelevantHazard"
assert "m_Tuning = Ctx.m_Core.m_Tuning" in t, "simulator must use the map tuning, not constants"
# The sensing radius has to gate the engine, otherwise the setting only moves the HUD readout.
assert "!Ctx.m_Threat.m_HasNearest" in t, "the sensing radius gate is missing from the decision engine"
assert "m_Controls.m_aInputData" not in t.replace("`m_Controls.m_aInputData`", ""), "avoid must never write the raw key state buffer"
assert "STAGE 2 IMPLEMENTATION SLOT" not in t, "the stage 2 slot should be filled in now"
# Nothing but the direction may be rewritten in Basic mode.
assert "Plan.m_Input.m_Hook" not in t and "Plan.m_Input.m_Jump" not in t, "Basic must only change m_Direction"
print("  ok")
EOF

echo "[5c/5] simulator fidelity + cost benchmark + sensing radius tests"
ninja -C build testrunner
./build/testrunner --gtest_filter='CAvoidSimulatorTest.*:CAvoidSensingRadiusTest.*'

echo "all checks passed"
```

---

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
| **阶段三** | **Legit** | ⬜ 下一档 | [`AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md) | 钩子（含**提前松钩**）+ 跳跃 + **MCTS** + **其他玩家预测** | 勾向黑水墙前脱钩；`Quality`/`Randomness`/三个 Weight 真实生效 |
| **阶段四** | **Blatant** | ⬜ | 由阶段三交付后编写 | Track Point、Safe Aim Tracking、Auto Drag、内置瞄准、NSIF 调优 | 极端 Gores 图上的生存率与瞄准辅助 |
| **阶段五** | **Fentbot** | ⬜ | 由阶段四交付后编写 | Fent Ticks / Tweaker 系列（当前为灰色占位） | 与参考实现 Fent 行为对齐 |
| **阶段六** | **Pilot** | ⬜ | 由阶段五交付后编写 | 种群 / 探索深度 / Top-K / 序列长度（当前为灰色占位） | 整段动作序列搜索 |

### F.2 每档必须留下的地基（验收时会检查）

| 档位 | 留给下一档的东西 |
| :--- | :--- |
| 阶段二 | `CAvoid::SimulateInput()`（**三个代理的公共模拟器**）、`bc_avoid_log` 逐决策日志、`CAvoidSimulatorTest` 保真度回归、`CAvoidSensingRadiusTest` 合成地图夹具 |
| 阶段三 | 扩展后的候选生成器（方向/跳跃/钩子）、MCTS 打分器、玩家快照注入点、钩子释放的判据 |
| 阶段四 | 瞄准/落点搜索、Track Point 记忆、Auto Drag 的队友选择 |

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
| `bc_avoid_sensing_radius` | 二/三/四/五/六 | `CAvoidSensingRadiusTest.*`；游戏内：半径 6 → 2 应明显更晚减速（§8.3 第 8 行） |
| `bc_avoid_direction_assist` | 二 | 关闭后代理不再改方向（Basic 只剩玩家自己的方向） |
| `bc_avoid_check_ticks` | 二/三 | 前瞻窗口；调小 → 反应更晚，调大 → 更早但更贵 |
| `bc_avoid_kick_in_ticks` | 二/三 | ≥ 26（> check_ticks）会导致代理永不介入，见 F.4 |
| `bc_avoid_tile_*` | 二/三 | 关掉“死亡块”后走向黑水不再被拦（§8.3 第 7 行） |
| `bc_avoid_hook_assist` / `m_HookWeight` | **三** | Legit：松不松钩的倾向；Basic 下无效（Basic 只改方向） |
| `bc_avoid_player_prediction` | **三** | 队友从侧面撞你向危险时的反应 |
| `bc_avoid_unfreeze_ticks` | **三** | 站在解冻块旁时的介入时机 |
| `bc_avoid_quality` / `randomness` / 三个 Weight | **三** | `Quality` 1 vs 200 的 cost 与决策质量 |
| `bc_avoid_track_point` / `safe_aim_tracking` / `auto_drag` / `aimbot*` | **四** | 阶段四任务书负责 |

### F.4 已知的“配置陷阱”（写进用户文档，也写进下一档任务书）

1. **`kick_in_ticks` ≥ `check_ticks` 会把干预窗口压到 0**：
   当前规则是“玩家输入还能安全 ≥ `KickInTicks` 就不干预，否则才搜索”，
   而搜索的前瞻只有 `CheckTicks` 帧。所以 `KickInTicks ≥ CheckTicks`（默认 20 < 26）时，
   代理只会在“已经来不及完整挽救”的局面里出手 —— 表现为**介入明显偏晚**，
   而且永远走 NSIF 兜底（红徽章）。
   下一档建议加一条**配置提示**（UI 或 `avoid_status`），在 `KickInTicks >= CheckTicks` 时警告。
2. **感知半径很大时改由 `check_ticks` 决定介入距离**（见 §6.7）：
   半径超过约 8 图块后在平地上就感觉不出差别了，这是预期行为。
3. **网络延迟**：参考实现要求 `cl_prediction_margin` 略高于 ping（如 50 ms ping 设 70）。
