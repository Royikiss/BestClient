# BestClient Avoid（避障 / Gores Bot）交接文档

> **文档版本**：5.0.0
> **适用代码分支**：`feature/tas`
> **面向对象**：后续维护开发者与 AI Agent
> **更新时间**：2026-10-05
> **深度技术细节**：见 [`docs/AVOID_TECHNICAL_DOCUMENTATION.md`](AVOID_TECHNICAL_DOCUMENTATION.md)
> **还原基准**：见 [`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`](avoid/KRX_AVOID_REPRODUCTION_SPEC.md)

---

## 0. 这一版修了什么（接手先读）

v3.0 把参数集对齐了参考端，但**决策算法本身**还有几处“看着像、实际不一样”的地方，实测表现为两个完全不可用的 bug。v4.0 按 `KRX_AVOID_REPRODUCTION_SPEC.md` 逐条比对后重写了受影响的部分，并补上了 v4.0 规格新增的瓦片编辑器。

v5.0（本版）补的是规格 v5.0 新增的**动作生效前后的三段流水线**。v4.0 只做对了“危险时能不能自救”，却完全没有“安全时不要乱动、不要烧算力”的那一层：

* **五级全局环境门控**（规格 §5.1–5.4）：玩法黑名单 / 观战与暂停 / 自身已冻结 / AFK 超时。
* **10-Tick 轻量级基线探针**（规格 §5.5，汇编 `0x140312258`）：玩家原输入在 10 帧里活过 ≥ 7 帧就直接放行，**根本不唤醒任何代理**。这是“安全状态 0 微抖动 + 500+ FPS”那一半分数的全部来源。
* **扇区准星扫描预处理器**（规格 §5.6，汇编 `0x1403122d3`）：危险时才按 FOV 扫 `Segments+1` 条射线，每条推演 21 帧，锁定最安全的准星。
* **Legit 26-Tick 二次后验增益仲裁**（规格 §8.3，汇编 `0x140338a0c`）：MCTS 选出的动作必须比玩家原输入**多活至少 1 帧**才允许覆盖输入，否则无条件驳回。
* **Blatant 六级优先级级联**（规格 §7.3–7.6）：迟滞 → Auto Drag（固定 380px 射程）→ 解冻块逃逸 → **并发**贪心（`std::async`，汇编 `0x14032eb50`）→ NSIF → 最长存活动作。

同步删掉了两处“参考端没有、而且会挡住自救”的自造护栏（Legit 的钩索/方向二次保护、Blatant 的 `BestSurvHook0 > 3` 钩索否决）：它们会在“玩家 6 帧内必冻”的窗口里把已经算出来的活路否掉。细节见 §0.4。

### 0.1 Bug：拟真模式（Legit）进游戏就自己往左走

三个原因叠加（前两个属于 Legit 算法本身，第三个是整条输入管线的共性缺陷，对所有 Agent 都成立）：

**原因 A（决策规则本身）**：参考规格 7.3 的最终决策是「**利用值 + 拟人启发值**，探索项 C = 0」（反编译 `0x1403392cb`），v3.0 却写成了「取访问次数最多的根子节点」。一旦玩家的输入被推演判为不安全，机器人就会去挑另一个子节点；而参考的扩展顺序是 `(-1,0) (0,0) (1,0) (-1,1) (0,1) (1,1)`，在「左转」和「右转」同样安全时，**第一个达到最高分的候选胜出 → 永远选左**。用离线复现 MCTS 循环验证过：只要 `(0,0)` 这个「照玩家原样」的子节点是终止态（不安全），旧规则必然收敛到 `m_Direction = -1`。

**原因 B（危险判定过宽，导致原因 A 被触发）**：v3.0 的冻结判定把 `m_IsInFreeze` 也算进去，而它是引擎的「在冻结/深冻/活冻**或四个角探针碰到死亡格**」合成标记。于是站在死亡块附近的**安全**位置上，玩家「什么都不按」也会被推演判死 → 触发原因 A → 机器人一路往左。规格 4.1 的判定只有 `m_FreezeTime > 0 || m_FrozenLastTick || m_DeepFrozen`，不看 `m_IsInFreeze`。

**原因 C（输入交接漏发，表现为「松手了还在走」）**：机器人接管时会强制发一个包，包里是**机器人改过的输入**；而采样器决定「要不要发包」时只比较自己的原始按键状态，看不见这次改写。v3.0 用「松手后的**下一 tick**（`Tick == m_DroveTick + 1`）才交接」判断，一旦预测 tick 在两次调用之间不是严格 +1（预测时间重置、卡顿丢 tick、快速输入排队），这次交接就永远不会发生，**服务端会一直沿用机器人最后一次发出的输入**——人物就自己往那个方向走。因为参考实现只改方向（`m_Direction`），这个残留输入最常表现为「一直自己往左走」。

**修复**：

1. 最终决策严格照规格 7.3 实现，抽成纯函数 `Avoid::SelectLegitRootChild()`：跳过 `m_Visits == 0` 的子节点，取 `Q/n + LegitHeuristicScore()` 最大者（回归测试 `src/test/avoid_decision_test.cpp`）。
2. 由于预算检查写在 `(Iter & 7) == 0` 上，搜索至少跑完 8 轮迭代，而参考的扩展一次就把 6 个子节点建齐——所以只要玩家输入真的安全，**对应它的子节点必定已被访问过**，而它的启发值最高（方向 +2.0、钩索 +1.0，权重 170/260），机器人就不会碰玩家的输入。
3. 一个子节点都没访问到时返回 `-1`，此时**不接管**（原实现会在这种情况下按未访问的子节点行动，这是同一类错误）。
4. 危险判定收窄到规格的三条标志，`m_IsInFreeze` / `m_LiveFrozen` 不再算作冻结（原因 B 消失）。
5. 输入交接改成**闩锁**（`m_YieldPending`）：驱动过就必定在随后的第一次决策里把玩家自己的输入发出去一次，不再依赖「tick 必须刚好 +1」（原因 C 消失）。这条对所有 Agent 生效。

### 0.2 Bug：激进模式（Blatant）辅助不精细、快速出勾抖动、走不了路

四处独立原因，全部按规格 6 修正：

| # | 问题 | 修复 |
| :-- | :--- | :--- |
| 1 | **危险判定写错**：`GetCollisionAt() & TILE_DEATH`。`TILE_DEATH` 是**瓦片编号 2**，不是位掩码，`& 2` 会把编号 3(nohook)、6、7、10、**11(unfreeze)**、14、15、**34(finish)** 全部当成死亡格 | 改为 `== TILE_DEATH`，并按引擎自己的四个角探针判定（规格 4.1）。这条对 Basic / Fentbot / Pilot 是**一直生效**的假危险源：站在终点块或解冻块附近，推演会直接判死 |
| 2 | **冻结判定过宽**：v3.0 把 `m_IsInFreeze`、`m_LiveFrozen` 也算作冻结。`m_IsInFreeze` 在**死亡格**和**已深冻**时同样为真 | 严格按规格 4.1 只保留 `m_FreezeTime > 0 \|\| m_FrozenLastTick \|\| m_DeepFrozen`（Fentbot 的浅冻豁免另算） |
| 3 | **缺少规格 6.3/6.4 两步**：Auto Drag（勾队友救命）和 Unfreeze Escape（冻结前逃向解冻块）根本没实现，Aimbot 层还把它们一起关在 `bc_avoid_aimbot` 里 | `GetAction` 重写为规格 6.6 的六步：迟滞 → Auto Drag → Unfreeze Escape → 贪心搜索(+瞄准层) → NSIF → 尽力而为。Auto Drag 只受自己的开关（+ 玩家预判）控制 |
| 4 | **瞄准层语义偏差**：Safe Aim Tracking 允许「比玩家原输入好一点」就锁定；实际文档要求「整个 Check Ticks 窗口都安全」 | 改为 `可用 = !SafeAimTracking \|\| 推演结果 == 9999`，并整层收敛为「只产生候选，最终由生存搜索裁决」，瞄准永远不会让机器人更不安全 |

另外补齐了规格 6.2 的候选瞄准归一化（`TargetX == 0 && TargetY == 0 → TargetY = -1`）与扩展顺序。

### 0.3 规格 v4.0 新增：瓦片编辑器（Tile Editor）

规格 §9 与 6 个新参数（`krx_tile_editor_*`）在 v3.0 里完全缺失。v4.0 落地为：

- `Avoid::CTileEditor`（`avoid_tile_editor.h/.cpp`）：Tunnel / Finish 两个图块集合，`ClearAll`、`AutoFinish`（扫地图 34 号终点块）、`AutoTunnels`（用已加载的 TAS 回放轨迹按宽度扩张）、鼠标左键绘制 / 右键擦除。
- `CNavigator::Rebuild(..., const CTileEditor *)`：有 Tunnel 时**隧道外全部视为墙**，Finish 直接成为流场终点；编辑器为空时退回「地图终点块 → 解冻块」。
- `CTileEditor::Revision()` 变化会让两个规划器的网格与流场自动重建（菜单里的“重新计算”按钮就是手动触发这件事）。
- Fentbot / Pilot 菜单新增 “Tile Editor” 页；世界里开启编辑器后可直接点击绘制，图块以半透明方块渲染（跟随 `bc_avoid_draw_path`）。

### 0.4 规格 v5.0：前置门控、10-Tick 探针、扇区扫描与 26-Tick 后验仲裁

参考端在**任何代理拿到输入之前**还有一整条前置流水线（汇编 `0x140311eb0`–`0x140312611`）。v4.0 完全没有这一层，后果正是规格书开头写的那两条：安全状态下每帧都在跑 100 轮世界克隆（帧率崩塌），以及 MCTS 的探索噪声被直接写进玩家输入（机械抖动）。

| 流水线 | 落地位置 | 行为 |
| :--- | :--- | :--- |
| 门控 1 玩法黑名单 | `CAvoid::IsGamemodeBlacklisted()` + 纯函数 `Avoid::IsBlacklistedGametype()` | 整串、大小写不敏感地比对 `fng` / `vanilla` / `f-ddrace` / `blockworlds`（规格 §5.1 是精确比对，不是子串匹配）；命中即放行原输入 |
| 门控 2 观察者与有效性 | `CAvoid::IsPlayerInactive()` | 本地 id 无效 / 观战 / 队伍为 `TEAM_SPECTATORS` / `GAMESTATEFLAG_PAUSED` / 没有活体 Tee → 阻断。Fast Practice 沙盒用自己的世界，此时不看连接快照 |
| 门控 3 已冻结 | `CAvoid::IsCharacterFrozen()` | `m_FreezeTime > 0` 或 `m_FrozenLastTick` 或 `Core()->m_DeepFrozen` → 阻断（被冻住时按键本来就不产生位移，推演纯属浪费） |
| 门控 4 AFK | `UpdateAfkTimer()` / `IsAfk()` | 按**规格 §5.4 的时间戳状态机**：方向 / 跳跃 / 开火 / 钩索任一变化，或准星位移 > 2px 就刷新 `m_LastActiveTime`；超过 `bc_avoid_afk_time` 秒 → 阻断该 tick 并置 `STATE_AFK`。**不再把 `bc_avoid_enabled` 改成 0**（见下） |
| 门控 5 10-Tick 探针 | `CAvoid::RunLightweightProbe()` → `Avoid::RunLightweightProbe()` | 固定 10 帧推演玩家原输入（`m_PredictPlayers` 取全局参数、`AvoidDeath = true`、`AvoidTeles = false`）；`Survival >= 7` → 返回 9999 → **直接放行，一个代理都不叫**；`< 7` 才继续。阈值来自汇编 `cmp eax, 0x7`，窗口来自 `mov edx, 0xa` |
| 阶段 2 扇区扫描 | `Avoid::RunSectorScan()` | 只有 `bc_avoid_track_point` 或 `bc_avoid_aimbot` 打开时才跑；以玩家准星角为中心，把 `bc_avoid_aimbot_fov` 按 `bc_avoid_aimbot_segments` 切成 **Segments+1** 条射线，每条塞 `Hook = 1` 推演 **21 帧**（汇编 `mov edx, 0x15`），取存活最久者；`bc_avoid_safe_aim_tracking` 关时「活过 1 帧」即可接受，开时必须整窗安全。接受就改写 `TargetX/TargetY`，这次改写本身算一次接管（会把包发出去） |
| 阶段 3 Legit 仲裁 | `CLegitAgent::GetAction()` 末尾 | 固定 **26 帧**双路推演：候选动作 vs 玩家原输入，`Gain = CandSurv - HumanSurv`，**只有 `Gain >= 1` 才允许覆盖输入**，否则保留玩家原输入（reason `post hoc gain below 1 tick`）。`Gain` 会写进 reason（如 `steer to safety (+14)`），便于对照 |
| 阶段 3 Blatant 级联 | `CBlatantAgent::GetAction()` | 迟滞 → **提前松勾** → Auto Drag → **净空二段跳** → **上半球出勾雷达** → 解冻块逃逸 → 全笛卡尔并发贪心 → NSIF → 最长存活，顺序即优先级（规格 §7.6；加粗的三级是 v5.1 新增，详见 §0.5） |

**Blatant 的三处具体变化**：

1. **Auto Drag 改成规格 §7.3 的射程规则**：固定 `AUTO_DRAG_MAX_DIST = 380.0f`（钩索最大有效延伸）与 `AUTO_DRAG_MIN_DIST = 16.0f`，按 client id 顺序遍历所有其他 Tee，**第一个**推演 26 帧返回 9999 的立刻接管；不再「只挑最近的那个」，也不再额外要求 `bc_avoid_player_prediction`（推演本身仍然按该参数决定是否模拟队友）。
2. **贪心搜索并发化**（规格 §7.5，`0x14032eb50`）：`SimulateBranchesParallel()` 用 `std::async` 把候选环一次发出去。安全性来自推演本身的结构：每个分支 `CopyWorldClean` 出自己的世界，只读基世界，碰撞 / 调参 / 地图数据只读，预测世界不碰任何客户端组件；线程创建失败（`std::system_error`）时自动退回串行，不会丢决策。
3. **NSIF 的缓存与重放**：缓存里只有一步时**保留**而不是消费掉（多步计划仍然逐步弹出），否则「必死局」的第二个 tick 就没东西可发了；且重放下来的那一步只有在**比玩家当前输入活得更久**时才真的接管（规格里是无条件返回，这里加了一道「不许更差」的闸，见 §4 第 2 条）。

**两处有意删掉的自造护栏**（规格书里没有，而且正好挡住自救）：

- Legit：原先在 MCTS 之后还有一段「钩索二次校验」（玩家没按钩索时 `PlayerSurv > 3` 就不许自动钩）与「方向保护」（6 帧内安全就退回玩家方向）。现在整段被 26-Tick 仲裁取代——仲裁用的是同一把尺子（两路都推 26 帧），比这两条更严格也更简单。
- Blatant：原先的 `BestSurvHook0 > 3 || BestSurvival <= BestSurvHook0` 钩索否决与方向二次校验。在「玩家输入 6 帧内必冻」这个**唯一还会调用代理的窗口**里，`BestSurvHook0 > 3` 几乎是常态，等于把已经算出来的活路（勾墙、勾队友）主动否掉。

**两处有意偏离规格字面**（都写进了代码注释，细节见技术文档 §12）：

1. **门控 5 不拦 Fentbot / Pilot**。这两个是**分片规划器**：搜索状态只在被调用的 tick 里推进（`PLANNER_STEPS_PER_TICK`）。若按规格字面用探针拦截，玩家安全时它们永远攒不出计划，而「安全」正是 Pilot 自主巡航 / Fentbot 沿流场寻路的正常工作状态——等于把这两个模式废掉。它们保留自己的每帧预算与闭环护栏（`PLAN_GUARD_TICKS`，计划比玩家输入差就丢弃重规划），Basic / Legit / Blatant 三个避障代理严格按规格被探针拦截。
2. **AFK 是每 tick 阻断，不是关闭总开关**。规格 §5.4 写的是「避障自动进入睡眠」，门控表也把 AFK 明确列为「阻断」。旧实现直接把 `bc_avoid_enabled` 写成 0——而这个参数是 `CFGFLAG_SAVE`，会落盘：用户离开键盘一次就**永久**失去机器人，必须回菜单手动打开。现在只是那一段时间不接管，玩家一碰键盘立刻恢复，进入 AFK 时在控制台提示一次 `Avoid: AFK protection paused the bot`。


### 0.5 规格 v5.1：三个“滞空自救”机制、12 分支动作空间与八级级联

用户实测报回来的三个缺口，根因都在**动作空间**与**决策顺序**上：v5.0 的 Blatant 只枚举 `方向{0,-1,1} × 钩索{0,1}`（最多 5 条），`m_Jump` 根本不在候选里——玩家自己不在空中狂按空格时，AI 永远推演不出“二段跳自救”；而自瞄预处理器又只在玩家准星 ±FOV/2 的扇区里扫，视线看着前方或下方时扫不到头顶的天花板。规格书因此升到 v5.1（§5.7 / §5.8 / §5.9 / §7.2 / §7.6 / §8.3），本次实现逐条落地。

| v5.1 机制 | 落地位置 | 行为与判据 |
| :--- | :--- | :--- |
| **提前松勾抢断**（规格 §5.7，`0x1403286f0`–`0x1403289e0`） | `Avoid::CheckPreemptiveHookRelease()` + 纯函数 `PreemptiveHookReleaseWins()` | 挂在钩索上（`m_HookState == HOOK_GRABBED`）**或**正按着钩索键时，用同一窗口双路推演：分支 A `m_Hook=1`（照按）、分支 B `m_Hook=0`（脱钩）。`Keep < CheckTicks && Release > Keep` → 强制把 `m_Hook` 置 0。**只改钩索这一个比特**，方向与准星不动 |
| **净空二段跳自救**（规格 §5.8） | `Avoid::CheckHeadroomClearance()` + `TryEmergencyAirJump()` + 纯函数 `HeadroomAllowsAirJump()` | 三条件：`CanUseAirJump()`（`!(m_Jumped & 2)` 且 `!IsGrounded()`）、头顶 48px 射线 + 正上方 32px 瓦片都不致死/不冻结、注入 `m_Jump=1` 后比“什么都不做”**严格更久**且 `≥ AIR_JUMP_MIN_GAIN_TICKS(8)`。方向在 `{-1,0,1}` 里挑活得最久的 |
| **上半球出勾雷达**（规格 §5.9） | `Avoid::TryEmergencyWallCeilingHook()` + `EmergencyRadarDirs()` + `RadarTargetIsHookable()` | **完全无视准星**，从角色位置向外投 5 条射线（正上 / 左上 / 右上 / 左侧 `(-1,-0.2)` / 右侧 `(1,-0.2)`），长度 `HOOK_MAX_DISTANCE = 380.0f`；命中点不是冻结 / 深冻 / 浅冻 / 致死 / `TILE_NOHOOK` 才可用。命中就强写准星 + `m_Hook=1`，按“中立 → 朝锚点方向”推演两次，`存活 > RADAR_MIN_SURVIVAL_TICKS(10)` 即接管 |
| **12 分支全笛卡尔暴搜**（规格 §7.2） | `avoid_decision.h` 的 `BuildBlatantCandidates()` | `Dirs[3]{0,-1,1} × Hooks[2]{0,1} × Jumps[2]{0,1}`，最多 **12** 条并发推演；跳跃维只在 `CanAirJump`（空中且还有二段跳）为真时打开，关闭时 `m_Jump` 保持玩家当前值（不会把跑动的地面跳搜没）。旧的“按玩家输入六选一的 5 分支优先级表”（旧 spec §6.2 / `0x14032e530`）已被取代 |
| **八级级联**（规格 §7.6） | `CBlatantAgent::GetAction()` | 迟滞 → 提前松勾 → Auto Drag → 净空二段跳 → 上半球雷达 → 解冻块逃逸 → 12 分支并发贪心 + 瞄准层 → NSIF 回放 → 最长存活。第 2/4/5 级推演通过就**立刻接管返回**，后面的级联一次都不跑 |
| **Legit 同步增强**（规格 §8.3） | `BuildLegitCandidates()` + `CLegitAgent::GetAction()` | MCTS 展开加入空中二段跳维（`CanAirJump` 时每个方向出 `m_Jump ∈ {0,1}` 两条，一次展开 9 个子节点；判据比 Blatant 多一条净空要求）；最终决策之后、26-Tick 仲裁之前挂载同一个提前松勾抢断（窗口取 `bc_avoid_legit_check_ticks`） |

**瓦片判定必须按编号比较**：规格把危险写成 `Tile & (TILE_DEATH | TILE_FREEZE)`，那是 `2 | 9 = 11` 的位掩码，套在瓦片编号上会连带否掉 1（实心）与 11（解冻块）——正好是雷达要找的墙、以及解冻块逃生要踩的格子。本实现用 `IsLethalOrFreezingTile()` / `RadarTargetIsHookable()` 逐编号比较，与 §4 第 7 条同一条红线。

**代价**：危险 tick 的 Blatant 推演从“≤5 分支环”变成“≤12 分支环 + 最多 16 次救援推演（2 松勾 + 4 二段跳 + 10 雷达）”，并发线程数从每次决策 ≤5 变 ≤12；安全状态下这些一次都不跑（门控 5 的 10-Tick 探针先放行）。性能账见技术文档 §9.1。

**两处有意偏离规格字面**（细节见技术文档 §12.22 / §12.23）：Legit 里“没有二段跳”时 `m_Jump` **继承父节点**而不是按字面清零（清零会让搜索丢掉玩家的地面跳）；Legit 的抢断窗口是自己的 `check_ticks`（默认 6，比 Blatant 的 26 帧短），想要更早抢断请调大该参数或改用 Blatant。

---

## 1. 系统架构

```
                     OnSnapInput()  (每 tick，50Hz)
                             │
                             ▼
       ┌────────────────────────────────────────────────────────┐
       │  CAvoid::ApplyInput  (avoid.cpp)                       │
       │  【门控 0】总开关 / 本地 Tee 是否存活                    │
       │  【门控 1】玩法黑名单 fng/vanilla/f-ddrace/blockworlds  │
       │  【门控 2】观战 / 暂停 / 无实体                          │
       │  【门控 3】自身已冻结（FreezeTime/FrozenLastTick/Deep）  │
       │  【门控 4】AFK 超时（按 tick 阻断，不改总开关）           │
       │  【门控 5】10-Tick 基线探针 ≥7 帧 → 直接放行，不叫代理    │
       │  【阶段 2】扇区准星扫描（TrackPoints/Aimbot 开时才跑）    │
       │  · 每 tick 只决策一次（m_LastDecisionTick）             │
       │  · 复用同一 tick 的决策（m_LastOverride）               │
       │  · 遥测 / 覆盖写回                                      │
       └───────────────┬────────────────────────────────────────┘
                       │ SContext（输入 + 设置快照 + 瓦片编辑器）
                       ▼
  ┌──────────────────────────────────────────────────────────┐
  │  BLAgent 分派（avoid_engine.h/.cpp）                      │
  │  Basic │ Legit(+26-Tick 后验仲裁) │ Blatant(八级级联)     │
  │  Fentbot │ Pilot  ← 这两个规划器不过门控 5（见 §0.4）      │
  └───────────────┬──────────────────────────────────────────┘
                  │
       ┌──────────┴───────────┐
       ▼                      ▼
┌──────────────┐      ┌────────────────────┐
│ 前向推演引擎  │      │ CNavigator 流场网格 │
│ CGameWorld   │      │ (Fentbot / Pilot)  │
│ 克隆 + Tick  │      │ 增量构建 + 预算     │
└──────────────┘      └─────────┬──────────┘
                                │ 目标集合 / 通行限制
                                ▼
                      ┌────────────────────┐
                      │ CTileEditor (§9)   │
                      │ Tunnel / Finish    │
                      └────────────────────┘
```

### 1.1 文件地图

| 文件 | 职责 |
| :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` / `.cpp` | `CAvoid` 组件：输入拦截、**五级前置门控 + 10-Tick 探针**、tick 内决策守卫、遥测、HUD 模块、世界叠加层、瓦片编辑器交互、控制台命令 |
| `src/game/client/components/bestclient/avoid_decision.h` | **纯函数**决策规则：玩法黑名单 / 探针阈值 / 扇区扫描角度 / 26-Tick 增益仲裁 / Legit 启发式 / 根节点选择 / 两个候选集枚举 / 接近目标输入。不依赖世界，可单测 |
| `src/game/client/components/bestclient/avoid_engine.h` / `.cpp` | 规格书里的全部算法：`SSettings` / `SContext` / `AvoidInput`、前向推演引擎（含可恢复的 `CSimSession`）、`RunLightweightProbe` / `RunSectorScan`、并发分支推演、`CNavigator`、五个 Agent |
| `src/game/client/components/bestclient/avoid_tile_editor.h` / `.cpp` | 规格 §9 的瓦片编辑器：Tunnel / Finish 集合、Auto Finish、Auto Tunnels、鼠标绘制 |
| `src/game/client/components/bestclient/menus_avoid.cpp` | “TAS& → 避障”页面：状态栏、机器人选择、共用设置、每个 Agent 的参数页、Tile Editor 页 |
| `src/engine/shared/config_variables_bestclient.h` | 56 个 `bc_avoid_*` 参数定义（**唯一真源**） |
| `src/test/avoid_decision_test.cpp` | 23 条决策规则回归测试（含“并列访问次数不得选到左侧子节点”、探针阈值、扇区扫描角度、26-Tick 增益仲裁，以及 v5.1 的 12 分支笛卡尔积、提前松勾判定、净空规则与雷达射线的回归） |
| `scripts/avoid_selfcheck.sh` | 8 步契约自检，改动本模块后必须跑 |

---

## 2. 参数集（56 项）

- **总开关**：`bc_avoid_enabled`（参考端 `krx_avoidfreeze`，默认 **0**，绑定方式 `bind X toggle bc_avoid_enabled 1 0`）。
- **Agent 选择**：`bc_avoid_agent`（0=Basic, 1=Legit, 2=Blatant, 3=Fentbot, 4=Pilot，默认 0）。
- **Legit 的“Quality / Randomness”** 分别是 `bc_avoid_legit_iterations`（100）与 `bc_avoid_legit_exploration`（4）。
- **两套独立的 check ticks**：`bc_avoid_legit_check_ticks` = **6**，`bc_avoid_blatant_check_ticks` = **26**；`bc_avoid_kick_in_ticks` = **26**。不要“统一”。
- **Fentbot 档位**：`bc_avoid_fent_quality`（0=Low 88/88/8、1=Mid 160/160/8、2=Max 1000/300/8，前瞻均为 10000 tick）。只有 `bc_avoid_fent_advanced = 1` 时四个 Tweaker 参数才覆盖档位。
- **Tile Editor（6 项，v4.0 新增）**：`bc_avoid_tile_editor_enable`(0)、`_type`(0)、`_clear`(0，自复位)、`_auto_tunnel`(0，自复位)、`_auto_tunnel_width`(**2**, 0..10)、`_auto_finish`(0，自复位)。
- **已删除且不应复活**：`bc_avoid_sensing_radius`、`bc_avoid_active`、`bc_avoid_show_hud`、`bc_avoid_show_visuals`、`bc_avoid_debug_override`、`bc_avoid_log`、`bc_avoid_tile_death/freeze/unfreeze/tele` 等全局图块开关。

**HUD 开关不是 cvar**：它直接读写 `HudLayout::MODULE_AVOID`，与 HUD 编辑器共享同一份状态。

---

## 3. 五个 Agent 一句话摘要

| Agent | 参考规格 | 算法要点 |
| :--- | :--- | :--- |
| **Basic** | §6 | 固定前瞻 **6 tick**（写死，无参数）；原输入安全就绝不介入；否则按 `{0, -1, 1}` 顺序枚举，**第一个**达到全安全的候选胜出；只改 `m_Direction` |
| **Legit** | §8 | UCT MCTS，`bc_avoid_legit_iterations` 次迭代；扩展 `{-1,0,1}×{hook=0}`（有二段跳时每个方向再带 `m_Jump` ∈ `{0,1}`）后追加 `{-1,0,1}×{hook=1}`；启发式 `\|Δdir−2\|·w_d·0.01 + \|Δhook−1\|·w_h·0.01 + 存活·w_l·0.01`；**最终取 `利用率 + 启发值`（探索项 = 0）最大的根子节点**，未访问的跳过；选出的动作还要过 **26 帧双路后验仲裁**（`CandSurv − HumanSurv ≥ 1`）才允许覆盖输入；仲裁之前先挂一道 **提前松勾抢断**（窗口是 `bc_avoid_legit_check_ticks`） |
| **Blatant** | §7 | 八级级联：`kick_in_ticks` 迟滞 → **提前松勾抢断**（死按钩子钟摆入水时强制脱钩）→ Auto Drag（380px 内第一个推演出 9999 的 Tee）→ **净空二段跳**（头顶 48px 无阻挡且增益 ≥8 帧时注入 `m_Jump=1`）→ **上半球出勾雷达**（无视准星，正上/左上/右上/两侧 380px 内抓天花板）→ Unfreeze Escape（BFS 找最近解冻块）→ **并发**贪心搜索（`{0,-1,1}×{0,1}×{0,1}` 最多 12 分支，首个达到最大存活者胜）+ 内部瞄准层 → NSIF 回放 → 最长存活动作。瞄准层只在 `bc_avoid_aimbot` 打开时产生候选 |
| **Fentbot** | §8 | `CNavigator` 增量构建可通行网格与流场（终点优先，否则解冻块；浅冻规则让半径内的冻结块可通行）；遗传式输入微调按 `tweaker_actions × tweaker_dosage` 搜索，适应度为规格 §8.2 的速度-流场点积（权重 1750.0f）叠加终点距离惩罚 |
| **Pilot** | §10 | 种群进化：`population` 条长度为 `depth` 的输入序列，按存活 / 流场 / 目标距离评分，保留 `top_k` 精英交叉变异；每 `sequence_length` tick 采纳一次当前最优序列；模式 0 自主、1 跟随准星、2 跟随玩家 |

Fentbot / Pilot 是**分片规划器**：每个渲染 tick 只花固定预算，搜索跨 tick 持续推进，因此不会卡帧；参数越大收敛越慢但解越好。

---

## 4. 与参考端**有意的**差异（改动前先读这一节）

1. **Legit 的 8 ms 搜索预算**（`LEGIT_DEADLINE_MS`）。参考端没有预算，Quality 拉满就是掉帧；本客户端不允许卡死，因此达到预算就返回当前最优。预算检查在 `(Iter & 7) == 0` 上，至少跑完 8 轮，保证参考的那 6 个子节点都被访问过。
2. **NSIF 的缓存与重放**。规格 §7.5 给出的贪心搜索只压入一个动作，所以缓存里通常只有一步；本实现在**只有一步时保留它**（多步计划仍逐步弹出），让「必死局」的后续 tick 仍有输入可发，而不是第二个 tick 就交还给玩家。同时第 6 步的接管条件仍是「严格活得更久」：重放下来的旧动作如果已经比玩家当前输入差，就不接管（规格里 NSIF 分支是无条件返回）。
3. **解冻块开关的双重语义**。英文 KRX 文档写“避免解冻块”，规格 §6.4/§6.6 写“冻结前主动逃向解冻块”。本实现在 Blatant 上两者都做：搜索期间前 `unfreeze_ticks` 个 tick 内踩到解冻块算危险（文档语义），但第 3 步的逃生候选**豁免**这条规则（规格语义）。Legit 只保留文档语义（规格 §7 没有逃生步骤）。
4. **瞄准层的门控**。规格 §7.6 的 Auto Drag 不受 `bc_avoid_aimbot` 控制（已照做）；Track Point / Auto Aim / Aim Assist 属于“需要瞄准辅助的功能”，统一由 `bc_avoid_aimbot` 打开（默认关，与参考默认一致）。Auto Drag 只在 `bc_avoid_auto_drag` 上；`bc_avoid_player_prediction` 关掉时推演里的队友条目被清空，救援候选自然测不出 9999（与参考一致，不再额外加一道闸）。
5. **瓦片编辑器在世界中用准星落点绘制**（`m_Controls.m_aTargetPos`），不是屏幕像素反投影；半径内可达的图块都能画。
6. **Dummy 与 TAS**：避障只作用于玩家自己控制的那条连接；TAS 录制读到的是玩家原始输入（机器人编辑发生在录制之后）。
7. **门控 5 不拦 Fentbot / Pilot**。规格 §5.5 的探针在 dispatcher 里位于代理分派之前，字面上对所有模式生效；但这两个是分片规划器，搜索只在被调用的 tick 里推进，被探针拦住就永远攒不出计划（而「玩家安全」正是它们的工作状态）。它们保留 `PLANNER_STEPS_PER_TICK` 预算与 `PLAN_GUARD_TICKS` 闭环护栏；Basic / Legit / Blatant 严格按规格被拦截。
8. **AFK 门控是每 tick 阻断，不写 `bc_avoid_enabled`**。规格 §5.4 的措辞是「自动进入睡眠」，门控表列为「阻断」；本实现据此实现，避免 `CFGFLAG_SAVE` 参数被落盘成 0 从而让用户永久失去机器人（旧行为）。
9. **Blatant 删掉了自造的两段「保护性否决」**（钩索否决 `BestSurvHook0 > 3`、方向二次校验），Legit 删掉了「钩索二次校验 + 方向保护」，两处都被规格里的仲裁/级联取代。保留它们会在「玩家 6 帧内必冻」的窗口里否掉已算出的活路，详见 §0.4。
10. **Legit 展开时「没有二段跳」这一支继承父节点的跳跃键，不清零。** 规格 §8.3 的字面写法是 `Act.m_Jump = j`（而 `JumpOptions` 在无二段跳时只有 `{0}`），照字面实现会让跑动中按住空格的玩家在搜索里丢掉地面跳；规格 §7.2 的 Blatant 版本本来就是「`if(CanJump)` 才写 `m_Jump`」，这里按 Blatant 的语义统一。详见技术文档 §12.22。
11. **Legit 的提前松勾窗口是自己的 `bc_avoid_legit_check_ticks`（默认 6），比 Blatant 的 26 帧短。** 规格 §8.3 第 4.5 步传的就是 `CheckTicks`，本实现照做；「荡进黑水」这类要十几帧才发生的危险，Legit 上要等到只剩 6 帧才抢断。想更早触发就调大该参数（它同时是 MCTS 的 rollout 长度，会变慢）或改用 Blatant。详见技术文档 §12.23。
12. **v5.1 的三个救援机制会抢在搜索之前动手。** 提前松勾 / 二段跳 / 雷达只要推演通过就立刻接管返回，`bc_avoid_blatant_direction` / `bc_avoid_blatant_hook` 关掉也拦不住它们：这三个开关管的是动作空间，不是救援。救援推演用的是与搜索相同的危险开关；要完全关掉它们只能换代理或关掉总开关（`bc_avoid_enabled`）。

---

## 5. 验证

```bash
# 一条命令跑完编译 + 8 步契约自检 + 全量单测
./scripts/avoid_selfcheck.sh
```

自检覆盖：参数契约（56 项名字/默认值/范围）、参数全部被读取且全部能在菜单里改到、195 条词条在两种语言里都存在、模块内无占位文案、引擎常量与算法结构、瓦片编辑器接线、HUD 模块接线与渲染顺序、testrunner 全量测试。

其中“引擎契约”这一步在 v5.0 里新增了对流水线的断言：探针常量（`PROBE_CHECK_TICKS = 10`、`PROBE_SAFE_TICKS = 7`）、21 帧扇区扫描、26 帧仲裁常量与 `ArbitrationGain/ArbitrationAllowsOverride`、五级门控的顺序、`probe → sector scan → agent` 的调用顺序、并发分支推演（`SimulateBranchesParallel` + `std::async`）、以及“`PlayerSurv` / `BestSurvHook0` / `SetEnabled(false)` 不得复活”；v5.1 又追加了三个救援机制的函数与常量、12 分支动作空间（`s_aJumps[2] = {0, 1}`、`JumpCount = CanAirJump ? 2 : 1`）、Blatant 八级级联的先后顺序、Legit 的空中二段跳展开与“抢断在仲裁之前”的顺序，以及按编号比较瓦片（`Tile == TILE_DEATH || IsFreezingTile(Tile)`，且按位与写法不得出现）。

游戏内还有三条控制台命令：

| 命令 | 作用 |
| :--- | :--- |
| `avoid_toggle` | 等价于 `toggle bc_avoid_enabled` |
| `avoid_status` | 打印状态、决策次数、接管次数、最近一次决策原因与耗时 |
| `avoid_reset` | 清零遥测计数 |

推荐绑定：`bind X toggle bc_avoid_enabled 1 0`。

### 5.1 游戏内验收步骤

**门控 5 探针：安全状态必须 0 唤醒**（规格 §14 第 1 条，70 分项）：

1. `bc_avoid_enabled 1`，任选代理；在宽阔平地上持续奔跑、起跳。
2. HUD 的 `Plan` 应稳定显示 `probe: player input safe`，`Override` 计数**完全不增长**，`Cost` 应远低于 1 ms。
3. 帧率应与关掉避障时无差别（`cl_showfps 1` 对照）；掉帧说明探针又被绕过了——检查是否有人把 `RunLightweightProbe` 的返回值改成了“忽略”。

**Legit 不再自己往左走 + 26-Tick 仲裁**（`bc_avoid_agent 1`）：

1. 站在安全平地什么都不按 5 秒：方向**不应**被改，`Override` 不增长。
2. 按住 D 向右跑过一段安全路：方向应保持向右，不应被改成左或 0。
3. 朝冻结墙跑：应在最后可救帧平滑减速或反向，且只在那几帧接管；`Plan` 会给出带增益的 reason（如 `steer to safety (+14)`），增益为 0 或负数时**不应**出现任何接管。
4. 在冻结墙前 3 格轻微左右微调：不应出现每帧来回改方向的抖动（这是 26 帧仲裁要滤掉的探索噪声）。
5. 如果 `Plan` 频繁显示 `steer to safety`，说明该处的推演确实判定原输入会触冻，属正常介入；把地图与坐标发回来再查。

**Blatant 恢复手感**（`bc_avoid_agent 2`，先用默认参数 `check 26 / kick 26`）：

1. 安全区域正常走路：HUD 应一直 `probe: player input safe`，`Override` 不增长。
2. **贴着死亡块 / 终点块 / 解冻块走路**：同样不应接管——这是 `& TILE_DEATH` 假阳性被修掉的地方（旧版会把它们当死亡格）。
3. 朝冻结池跑：应减速或反向，**不应**出现连续的勾-放抖动；必要时打开 `bc_avoid_draw_path` 看预测路径是否平滑。
4. **Auto Drag（规格 §14 第 4 条）**：`bc_avoid_auto_drag 1`，自身向深渊坠落、**380px** 内上方有一名停留在安全地面的队友；准星应瞬间转向队友并抛钩，`Plan` 显示 `auto drag a teammate`，不需要走到贪心搜索。
5. **NSIF（规格 §14 第 5 条）**：`bc_avoid_nsif 1`，从极高空垂直坠入封闭冻结池（必死局）；搜索找不到全安全解时 `Plan` 应变成 `NSIF: replay saved safe input`，`STATE` 徽标变 `NSIF`，输入连续不丢帧、不抽搐。
6. **提前松勾（规格 §14 第 1 条）**：在长冻结池上方钩住天花板，在空中大幅摆动，**全程死死按住右键**。应在抛物线切点被强行夺走钩索：`Plan` 显示 `release the hook before the swing`，角色借惯性飞越池子；如果一直按到入水，检查是不是在用 Legit（它的窗口只有 6 帧）或者玩家输入在 26 帧内本来就安全（迟滞直接放行了）。
7. **净空二段跳（规格 §14 第 2 条）**：从高台跳向黑水，半空中留一段二段跳，头顶是无遮挡天空，**双手离开键盘**。应在离水面还剩若干帧时自动注入二段跳：`Plan` 显示 `spend the air jump`。头顶有冻结顶棚或一格内就是实心天花板时**不应**触发（这是净空规则的负例）。
8. **上半球出勾雷达（规格 §14 第 3 条）**：耗尽二段跳后垂直坠向黑水，头顶或侧上方 380px 内有未冻结的实心墙体，**鼠标故意瞄准正下方的黑水**。准星应被强行上扬并抛钩：`Plan` 显示 `hook the ceiling above`，橙色瞄准标记指向锚点。若没触发：先确认该墙不是 `TILE_NOHOOK`、命中点不是冻结块。
9. **12 分支压制（规格 §14 第 4 条）**：同一张地狱级 Gores 地图上对比 `bc_avoid_agent 1` 与 `2`，Blatant 应把二段跳、甩摆松勾、全向抓附与 Auto Drag 连成一套，生还率明显高于 Legit。
10. 再逐项打开 `bc_avoid_aimbot`（`bc_avoid_track_point` / `bc_avoid_auto_aim` / `bc_avoid_aim_assist`）与 `bc_avoid_blatant_unfreeze`，确认每项都只在应该介入时介入。开了 `bc_avoid_track_point` 或 `bc_avoid_aimbot` 时，阶段 2 的扇区扫描会先跑一遍，接管时 reason 是 `sector scan locked the crosshair`。
11. 仍观察到异常时用 `avoid_status` 取下最近一次的 `reason` 与 `safe ticks`，连同 `bc_avoid_*` 参数一起发回来。

---

## 6. 维护红线

1. **不要改参数的默认值与范围**。它们逐项取自参考端的 CVar 注册表，自检第 2 步会逐项比对，改动即失败。
2. **不要新增“参考端没有”的参数**。任何新参数都必须能回答“它改变了哪一个可观察行为”，并在自检第 3 步里被证明真的被读取。
3. **推演必须在克隆出的世界里进行**，绝不能直接改动游戏实体的实时状态（`GetActiveWorld()` 返回的世界只被读、被 `CopyWorldClean` 克隆）。
4. **不要把 `CAvoid` 挂到 `m_Controls.m_aInputData` 上**。只允许修饰即将发包的 `CNetObj_PlayerInput`，否则预测回滚会被污染。
5. **每个 tick 只决策一次**。同一 tick 内客户端可能要求重发，此时必须复用 `m_LastOverride`。
6. **决策规则改动必须落在 `avoid_decision.h` 的纯函数里并补 `src/test/avoid_decision_test.cpp`**。这一层的错误不会崩、只会“玩起来不对”，只有单测 + 自检能拦住（Legit 往左走就是这么漏出去的）。
7. **危险判定用比较，不要用位与**。`TILE_DEATH` / `TILE_FINISH` 等是瓦片编号；只有 `GetMoveRestrictions` 之类的返回值才是位掩码。
8. **界面文案一律走 `BcLocalize()`**，并同步补齐 `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt`（`[BestClient]` 段）。自检第 4 步会拦截漏翻。
9. **不要给代理加“比参考更保守”的自造否决**。v5.0 删掉的两段护栏（Legit 的钩索/方向二次校验、Blatant 的 `BestSurvHook0`）都是这么来的：它们看起来“更安全”，实际把已经推演出来的活路否掉。要更保守就调参数，不要加代码路径；要防误干预就加在有推演证据的地方（探针、仲裁）。
10. **门控 5（探针）只拦 Basic / Legit / Blatant**。给 Fentbot / Pilot 也加上等于废掉那两个模式（分片规划器只在被调用的 tick 里推进），改之前先读 §0.4。

---

## 7. 历史文档

`docs/AVOID_STAGE2_BASIC_PROMPT.md`、`AVOID_STAGE3_LEGIT_PROMPT.md`、`AVOID_STAGE3_LEGIT_REPORT.md`、`AVOID_STAGE4_BLATANT_PROMPT.md`、`AVOID_LEGIT_ACCEPTANCE.md` 是 v2.x 分阶段开发时留下的任务书与验收记录，描述的是**已废弃**的 33 参数实现。它们只作历史留存，**不要**按它们实现或验收。
