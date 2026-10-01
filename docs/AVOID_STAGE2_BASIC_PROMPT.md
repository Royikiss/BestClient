# 交给下一个 AI 的提示词：实现「避障」模块的 Basic（基础）模式  —— ✅ 已于 v1.1.1 交付

> 用法：把下面 `====` 之间的整段内容原样发给下一个 AI（或直接说"读 `docs/AVOID_STAGE2_BASIC_PROMPT.md` 并执行"）。

====

# 任务：为 BestClient 的「避障 / Avoid」模块实现 **Basic（基础）模式** 的决策算法

## 背景（你没有上下文，先读这段）

BestClient 是 DDNet 的一个分支客户端，工作目录就是仓库根目录，当前分支 `feature/tas`。

仓库里刚完成了一个 **避障 / Avoid** 模块的**阶段一**：界面、33 个配置项、危险感知层、
输入拦截管线、游戏内 HUD 全部就绪并且已通过验收。
**唯一没写的是决策算法** —— `CAvoid::EvaluateBestPlan()` 目前是个空壳，永远返回"不干预"，
所以现在无论你怎么走向黑水，角色都不会被自动停住。

本任务只做 **Basic 模式**：**只用左右方向键**，在玩家即将撞上黑水 / 冻结块时接管方向输入把人停住，
其余一概不管（不动钩子、不动跳跃、不动瞄准）。

这是整个避障功能的地基：后面 Legit / Blatant 的 MCTS 搜索都会复用你这一步写出来的**前向模拟器**，
所以模拟器的正确性比 Basic 本身的手感更重要。

## 必读材料（按顺序读，别跳过）

1. `docs/AVOID_TECHNICAL_DOCUMENTATION.md`
   - 开头「速览：下一个 AI 先读这 8 条」
   - **第 3 章 输入拦截管线** —— 尤其 3.2（为什么绝对不能写 `m_Controls.m_aInputData`）
     和 3.5（为什么已经保证代理启用时 50 Hz 发包）
   - **第 4 章 危险感知层** —— 4.2 探测点规则必须与 DDNet 引擎逐条一致
   - **第 6 章 阶段二实现指南** —— 6.1 函数签名与契约、6.2 推荐算法、
     6.3 物理前向模拟的两种路径与各自风险、6.4 性能预算、6.5 踩坑清单
   - 附录 B 关键代码位置速查（直接给你文件与函数）
2. `docs/avoid/avoid.md` —— 参考客户端对 Basic 的定位：
   *"Uses only directional keys (left/right) for basic freeze avoidance. No configurable settings."*
3. 想了解后续阶段的完整参数语义可翻 `docs/avoid/legit.md`、`docs/avoid/blatant.md`。

## 交付物

1. **前向模拟器**（新函数，建议 `CAvoid::SimulateInput()`，签名自定，写在 `avoid.cpp` 内）
   给定一个候选输入，从角色当前物理状态出发推演 N 帧，返回"能安全存活多少帧"，
   以及（可选）最终位置/速度，便于调试。
2. **决策引擎**：把 `CAvoid::EvaluateBestPlan()` 从空壳填成真实现，**只允许改 `m_Direction`**；
   `m_Hook` / `m_Jump` / `m_Fire` / `m_TargetX` / `m_TargetY` / 武器字段一律保持玩家原样。
3. **文档更新**：
   - `docs/AVOID_TECHNICAL_DOCUMENTATION.md` 附录 C 变更历史加一行（v1.1.0）；
   - 第 6 章标题下标注「Basic 部分已实现，Legit / Blatant 仍待实现」；
   - 若你新增了配置项（应当不需要），同步更新第 7 章表格。
4. `./scripts/avoid_selfcheck.sh` 五项全绿。

## 硬性约束（违反 = 返工）

- **不要改** `SContext` / `SInputPlan` / `SThreat` / `SSettings` 的结构，
  不要改 `CAvoid::ApplyInput()` 的调用约定与写回方式。
- `Plan.m_Input` **必须从 `Ctx.m_Input` 复制后再改**，以保留
  `m_PlayerFlags` / `m_NextWeapon` / `m_PrevWeapon`。
- **只写 `Plan.m_Input`**。绝对不要写 `m_Controls.m_aInputData`
  （那里的 `m_Fire` 是边沿计数语义，写进去会污染开火判定）。
- 危险判定**必须复用** `CAvoid::ClassifyPoint()` 与 `CAvoid::IsRelevantHazard()`，
  不要自己另写一套探测点，否则会出现"感知说危险、物理其实安全"的错位。
- 前向模拟的物理**必须使用本地图的 tuning**
  （`CCharacterCore::m_Tuning` 或 `GetTuning(GetOverriddenTuneZone())`），
  **不要硬编码**重力、跳跃加速度、最大速度、摩擦等常量 —— 自定义 tuning 地图上会整段跑偏。
- **性能**：每 tick ≤ 1.5 ms，填进 `Plan.m_CostMs`（UI 会直接显示这个值，
  超了就是没做完）。决策路径里禁止堆分配，用成员缓冲或定长数组。
- **玩家输入本身安全时完全不干预**。这是手感生命线：宁可少管，也不要多管。
- 代码风格跟仓库一致（tab 缩进、`.clang-format`）。
  新增任何界面字符串必须走 `BcLocalize()`，并同步
  `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt`
  （词条**不要**用 `[ ]` 包裹，见 TAS 文档 6.3.2 的解析器陷阱）。
- 不要动 Legit / Blatant / Fentbot / Pilot 的专属面板与参数语义，不要改 HUD 模块与界面布局。

## 期望行为（用户原话，就是验收标准）

1. 操控 tee 朝黑水（或其它自杀方块）**走**过去 → 按键失效或迅速减速，能**停在危险边缘**；
2. 通过**钩子荡**向黑水 → 会自动按下反方向的键减速
   （本阶段**不要**松开钩子，"提前松钩"是下一阶段的事）；
3. 直行经过黑水旁的**窄通道** → **不能误触发**（这是最容易翻车的地方，
   探测点规则一错就会在这里暴露）。

## 建议实现顺序

1. **先写模拟器，单独调通**。用 `bc_avoid_log 1` 打印每帧位置与危险判定，
   和游戏内实际轨迹目测校准。校准工具：
   - `bc_avoid_show_visuals 1` —— 世界里画出感知半径、危险图块与最近威胁连线；
   - `bc_avoid_debug_override 1` —— 强制接管并反转左右键，用来确认输入管线本身是通的
     （打开后按住 D 角色会往左走，说明写回生效）。
2. **再做决策**，逻辑保持极简：
   - 用玩家当前输入前向模拟 `m_CheckTicks` 帧；能安全活满 → 直接返回
     `Plan.m_Override = false`（完全不干预）；
   - 否则在 `{-1, 0, +1}` 三个方向里挑"活得最久"的那个，写进 `Plan.m_Input`，
     置 `Plan.m_Override = true`。
3. 用 `m_KickInTicks` 控制介入时机：玩家输入还能安全这么久就先别动，避免过度干预。
4. 回填 `Plan.m_SafeTicks`（所选方案存活帧数）、`Plan.m_ScannedTicks`、
   `Plan.m_Candidates`、`Plan.m_CostMs`，
   以及 `Plan.m_aReason`（给 HUD 看的一句短说明，例如 `stop before DEATH`，
   新增字符串记得走 `BcLocalize`）。

## 自检与验收

```bash
ninja -C build DDNet
./scripts/avoid_selfcheck.sh        # 5 项必须全绿
```

游戏内验证（`bc_avoid_agent 0` 即 Basic，勾选"启用避障代理"或控制台 `avoid_toggle`）：

| 检查 | 期望 |
| :--- | :--- |
| 朝黑水走 | 接触前 1~2 图块内被停住或明显减速 |
| 什么都不按 | 零干预，`avoid_status` 里的 `overrides` 不增长 |
| 直行过窄通道 | 不误触发（不出现莫名其妙的减速） |
| `avoid_status` 的 `cost` | 稳定 ≤ 1.5 ms |
| 开关一次代理再走一段 | 不崩溃、不卡顿、无异常日志 |

## 交回给我的报告格式

1. 改了哪些文件、各多少行；
2. 模拟器走的是哪条路径（轻量复刻 / 沙盒克隆），与真实物理的偏差有多大、你**怎么验证**的；
3. 三条验收标准的实测结果（`avoid_status` 输出或截图）；
4. `Plan.m_CostMs` 的实测区间（最坏情况）；
5. 已知不足，以及留给下一步（Legit / Blatant / 钩子释放）的问题。

## 明确不做（留给后续阶段）

- 不实现钩子释放、Track Point、内置瞄准、Auto Drag、NSIF、MCTS / 遗传搜索；
- 不实现 Legit / Blatant / Fentbot / Pilot 的算法；
- 不改界面布局、参数面板、HUD 模块与渲染顺序。

====

---

## 备注（给人类，不用发给 AI）

* 这份任务书刻意**只做 Basic 模式的方向键制动**。原因是它同时覆盖了用户最初描述的三条验收里的前两条
  （"走向黑水被停住"和"荡向黑水被反向键减速"），而且它写出来的前向模拟器是后续所有代理的公共地基。
* 本任务书已于 v1.1.1 交付完毕，**不要再执行本文件**。
* 下一档是 Legit：[`docs/AVOID_STAGE3_LEGIT_PROMPT.md`](AVOID_STAGE3_LEGIT_PROMPT.md)
  （钩子释放 + 跳跃 + MCTS + 其他玩家预测）。
* 最低层的完整任务书仍在 [`AVOID_TECHNICAL_DOCUMENTATION.md` 附录 A](AVOID_TECHNICAL_DOCUMENTATION.md#附录-a阶段二任务书可直接交给下一个-ai)，
  但其中“第 1 步 前向模拟器”已完成，而且混入了阶段四的 Track Point / 内置瞄准，
  请以附录 F 的分档表与各档任务书为准。
* 如果下一个 AI 交回的结果不好，先检查这三件事：
  1. 模拟器是否用了地图 tuning（硬编码常量 = 必翻车）；
  2. 是否复用了 `ClassifyPoint()`（另写探测点 = 窄通道误触发）；
  3. `cost` 是否被真实测量并填写（没填 = 没做性能验证）。
