# BestClient Avoid（避障 / Gores Bot）交接文档

> **文档版本**：3.0.0
> **适用代码分支**：`feature/tas`
> **面向对象**：后续维护开发者与 AI Agent
> **更新时间**：2026-10-05
> **深度技术细节**：见 [`docs/AVOID_TECHNICAL_DOCUMENTATION.md`](AVOID_TECHNICAL_DOCUMENTATION.md)
> **还原基准**：见 [`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`](avoid/KRX_AVOID_REPRODUCTION_SPEC.md)

---

## 0. 这一版做了什么（接手先读）

上一版实现偏离参考客户端：它自创了 33 个 `bc_avoid_*` 参数（含"感知半径""威胁可视化""输入管线自检"等参考端不存在的开关），五个 Agent 里 Fentbot / Pilot 只有菜单占位、Legit / Blatant 的评分公式与参考端不一致，菜单里还留着 "planned / 尚未实现 / not implemented yet" 之类文案，且全部英文词条没有中俄翻译。

v3.0 按参考端规格**整体重做**：

1. **参数集完全对齐参考端**：旧的 33 个参数全部删除，换成 **50 个**与参考端一一对应的参数（名字换成 BestClient 的 `bc_avoid_*` 前缀，**默认值 / 最小值 / 最大值逐项一致**）。规格里没有的参数一律不保留。
2. **五个 Agent 全部真实可用**：Basic / Legit / Blatant 严格按规格第 5/6/7 节的算法与常量实现；Fentbot（流场 + 遗传输入微调 + 档位 + 浅冻）与 Pilot（种群进化 + 三种模式）在引擎内实时可用版本上落地，参数全部生效。
3. **菜单重做**：按参考端分页（Legit 3 页、Blatant 4 页、Fentbot 1 页、Pilot 2 页、Basic 无参数），删掉全部占位文案与无效控件。
4. **全量本地化**：175 条界面词条在简体中文与俄语两份 BestClient 语言文件中全部落地，自检脚本强制校验。
5. **HUD 保留并适配**：状态面板仍是标准 HUD 模块（`HudLayout::MODULE_AVOID`），默认关闭，可从避障菜单的"视觉"框或 HUD 编辑器开启——两处写的是同一份状态，不存在重复参数。
6. **输入管线重做**：`CAvoid::ApplyInput` 只读 `m_Controls.m_aInputData` 的副本，返回"是否需要因此发包"。`CControls::SnapInput` 不再为避障无条件 `Send = true`，机器人只在真正接管时才要求发包；TAS 录制改从采样缓冲读取，因此录到的始终是玩家的原始输入。

---

## 1. 系统架构

```
                     OnSnapInput()  (每 tick，50Hz)
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  CAvoid::ApplyInput  (avoid.cpp)           │
        │  · 每 tick 只决策一次（m_LastDecisionTick） │
        │  · 复用同一 tick 的决策（m_LastOverride）   │
        │  · AFK 保护 / 遥测 / 覆盖写回               │
        └───────────────┬────────────────────────────┘
                        │ SContext（输入 + 设置快照）
                        ▼
   ┌──────────────────────────────────────────────────────────┐
   │  BLAgent 分派（avoid_engine.h/.cpp）                      │
   │  Basic │ Legit │ Blatant │ Fentbot │ Pilot                │
   └───────────────┬──────────────────────────────────────────┘
                   │
        ┌──────────┴───────────┐
        ▼                      ▼
 ┌──────────────┐      ┌────────────────────┐
 │ 前向推演引擎  │      │ CNavigator 流场网格 │
 │ CGameWorld   │      │ (Fentbot / Pilot)  │
 │ 克隆 + Tick  │      │ 增量构建 + 预算     │
 └──────────────┘      └────────────────────┘
```

### 1.1 文件地图

| 文件 | 职责 |
| :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` / `.cpp` | `CAvoid` 组件：输入拦截、tick 内决策守卫、AFK 保护、遥测、HUD 模块、世界叠加层、控制台命令 |
| `src/game/client/components/bestclient/avoid_engine.h` / `.cpp` | 规格书里的全部算法：`SSettings` / `SContext` / `AvoidInput`、前向推演引擎（含可恢复的 `CSimSession`）、`CNavigator`、五个 Agent |
| `src/game/client/components/bestclient/menus_avoid.cpp` | "TAS& → 避障"页面：状态栏、机器人选择、共用设置、每个 Agent 自己的参数页 |
| `src/engine/shared/config_variables_bestclient.h` | 50 个 `bc_avoid_*` 参数定义（**唯一真源**） |
| `src/game/client/components/hud_layout.cpp` | `MODULE_AVOID` 的默认布局、持久化与本地化 |
| `src/game/client/components/bestclient/menus_tas.cpp` | TAS& 的子页签；"辅助"页里的 HUD 编辑器入口 |
| `scripts/avoid_selfcheck.sh` | 8 步契约自检，改动本模块后必须跑 |

---

## 2. 参数集（50 项）

完整表格（含 C++ 成员名、每个参数被哪个 Agent 读取、可观察效果）见技术文档第 6 节。这里只列关键约定：

- **总开关**：`bc_avoid_enabled`（参考端 `krx_avoidfreeze`，默认 **0**，绑定方式 `bind X toggle bc_avoid_enabled 1 0`）。上一版额外的 `bc_avoid_active`（Arm/Disarm）已删除——参考端只有一个开关。
- **Agent 选择**：`bc_avoid_agent`（0=Basic, 1=Legit, 2=Blatant, 3=Fentbot, 4=Pilot，默认 0）。
- **Legit 的"Quality / Randomness"** 分别对应 `bc_avoid_legit_iterations`（默认 100）与 `bc_avoid_legit_exploration`（默认 4）。
- **Legit 与 Blatant 各有独立的 check ticks**：`bc_avoid_legit_check_ticks` 默认 **6**，`bc_avoid_blatant_check_ticks` 默认 **26**——两者不同是参考端的真实行为，不要"统一"。
- **Fentbot 档位**：`bc_avoid_fent_quality`（0=Low 88/88/8、1=Mid 160/160/8、2=Max 1000/300/8，前瞻均为 10000 tick）。只有 `bc_avoid_fent_advanced = 1` 时，那四个 Tweaker 参数才会覆盖档位。
- **已删除且不应复活**：`bc_avoid_sensing_radius`、`bc_avoid_active`、`bc_avoid_show_hud`、`bc_avoid_show_visuals`、`bc_avoid_debug_override`、`bc_avoid_log`、`bc_avoid_tile_death/freeze/unfreeze/tele` 等全局图块开关——参考端把它们做成每个 Agent 独立参数。

**HUD 开关不再是一个 cvar**：它直接读写 `HudLayout::MODULE_AVOID`，与 HUD 编辑器共享同一份状态，因此不可能出现"参数与界面对不上"。

---

## 3. 五个 Agent 一句话摘要

| Agent | 参考规格 | 算法要点 |
| :--- | :--- | :--- |
| **Basic** | §5 | 固定前瞻 **6 tick**（写死，无参数）；原输入安全就绝不介入；否则按 `{0, -1, 1}` 顺序枚举，**第一个**达到全安全的候选胜出；只改 `m_Direction` |
| **Legit** | §7 | UCT MCTS，`cl_avoid_num_iterations` 次迭代；拟人启发式 `\|Δdir−2\|·w_d·0.01 + \|Δhook−1\|·w_h·0.01 + 存活·w_l·0.01`；扩展空间 `{-1,0,1}×{0,1}`；最终取**访问次数最多**的根子节点，只要与玩家输入不同就生效 |
| **Blatant** | §6 | 先用 `kick_in_ticks` 做迟滞判定（原输入够安全就不介入）；否则在 `{0,-1,1}×{0,1}` 上做贪心搜索取存活最久者；找不到全安全方案时按 `nsif` 回放上一段已知安全序列的首步；可选内置瞄准层（自瞄 / 瞄准辅助 / 锁定点 / 队友拉扯） |
| **Fentbot** | §8 | `CNavigator` 增量构建可通行网格与流场（终点优先，否则解冻块；浅冻规则让半径内的冻结块可通行）；遗传式输入微调按 `tweaker_actions × tweaker_dosage` 搜索，适应度为规格 §8.2 的速度-流场点积（权重 1750.0f）叠加终点距离惩罚 |
| **Pilot** | §9 | 种群进化：`population` 条长度为 `depth` 的输入序列，按存活 / 流场 / 目标距离评分，保留 `top_k` 精英交叉变异；每 `sequence_length` tick 采纳一次当前最优序列；模式 0 自主、1 跟随准星、2 跟随玩家 |

Fentbot / Pilot 是**分片规划器**：每个渲染 tick 只花固定预算（见技术文档第 9 节），搜索跨 tick 持续推进，因此不会卡帧；参数越大收敛越慢但解越好，这与参考端"计算可能耗时数分钟"的取舍一致。

---

## 4. 新增功能首次使用的注意事项

1. **默认是关闭的**。第一次用请到 `ESC → TAS& → 避障`，选机器人，再点"启用"。旧配置升级时会由 `cl_config_version 3` 迁移强制把 `bc_avoid_enabled` 置 0，避免升级后突然自动接管。
2. **Pilot / Fentbot 会自己驾驶 Tee**。这两个 Agent 的设计目标就是自主移动（参考端文档同样如此），开启即视为授权；Basic / Legit / Blatant 仍然只在你的输入会出事时才接管，安全时输出与你的输入逐位一致。
3. **Fentbot / Pilot 首次启用需要等网格构建**。状态栏会显示"网格：构建中 / 就绪"，构建完成前机器人使用便宜的保命兜底动作。
4. **Legit 的 CPU 开销与 `bc_avoid_legit_iterations` 成正比**。引擎内置 8 ms 硬上限，达到上限会返回当前最优解并把状态写成"search budget reached"，避免客户端卡死。

---

## 5. 验证

```bash
# 一条命令跑完编译 + 8 步契约自检 + 365 项单测
./scripts/avoid_selfcheck.sh
```

自检覆盖：参数契约（50 项名字/默认值/范围）、参数全部被读取且全部能在菜单里改到、175 条词条在两种语言里都存在、模块内无占位文案、引擎常量与算法结构、HUD 模块接线与渲染顺序、testrunner 全量测试。

游戏内还有三条控制台命令：

| 命令 | 作用 |
| :--- | :--- |
| `avoid_toggle` | 等价于 `toggle bc_avoid_enabled` |
| `avoid_status` | 打印状态、决策次数、接管次数、最近一次决策原因与耗时 |
| `avoid_reset` | 清零遥测计数 |

推荐绑定：`bind X toggle bc_avoid_enabled 1 0`。

---

## 6. 维护红线

1. **不要改参数的默认值与范围**。它们逐项取自参考端的 CVar 注册表，自检脚本会逐项比对，改动即失败。
2. **不要新增"参考端没有"的参数**。任何新参数都必须能回答"它改变了哪一个可观察行为"，并在自检的第 3 步里被证明真的被读取。
3. **推演必须在克隆出的世界里进行**，绝不能直接改动游戏实体的实时状态（`GetActiveWorld()` 返回的世界只被读、被 `CopyWorldClean` 克隆）。
4. **不要把 `CAvoid` 挂到 `m_Controls.m_aInputData` 上**。只允许修饰即将发包的 `CNetObj_PlayerInput`，否则预测回滚会被污染。
5. **每个 tick 只决策一次**。同一个 tick 内客户端可能要求重发，此时必须复用 `m_LastOverride`，否则会重复推进 Fentbot / Pilot 的搜索状态。
6. **界面文案一律走 `BcLocalize()`**，并同步补齐 `data/BestClient/languages/simplified_chinese.txt` 与 `russian.txt`（`[BestClient]` 段）。自检第 4 步会拦截漏翻。

---

## 7. 历史文档

`docs/AVOID_STAGE2_BASIC_PROMPT.md`、`AVOID_STAGE3_LEGIT_PROMPT.md`、`AVOID_STAGE3_LEGIT_REPORT.md`、`AVOID_STAGE4_BLATANT_PROMPT.md`、`AVOID_LEGIT_ACCEPTANCE.md` 是 v2.x 分阶段开发时留下的任务书与验收记录，描述的是**已废弃**的 33 参数实现。它们只作历史留存，**不要**按它们实现或验收。
