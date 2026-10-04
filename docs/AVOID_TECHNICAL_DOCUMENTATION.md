# BestClient Avoid（Gores Bot）维护与验收技术文档

> 适用对象：接下来要改 Avoid 模块的人。
> 快照时间：2026-10-05 04:07（写作前逐行复核过下列文件；若行号对不上，先确认源码是否又动过）：
> `src/game/client/components/bestclient/avoid.h`（132 行）、`avoid.cpp`（719 行）、
> `avoid_engine.h`（462 行）、`avoid_engine.cpp`（2176 行）、`menus_avoid.cpp`（883 行）、
> `src/engine/shared/config_variables_bestclient.h` 的 `bc_avoid_*` 区块（610–670 行）、
> `scripts/avoid_selfcheck.sh`（327 行）、`src/game/client/gameclient.cpp:629-667`（输入钩子）、
> `docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`（790 行）。
>
> 参考实现规格：`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`。下文用 “spec §N” 指该文件的章节。
> 本文不含任何“以后会做”的内容；每一句都对应当前代码里的一条语句。已知的、有意偏离 spec 字面的
> 地方全部集中在 §12，改代码前先读那一节。

---

## 1. 速览

下一个接手的人必须知道这 8 件事，其余章节都是它们的展开。

**1. 参数集被整体替换了：旧 33 个 → 新 50 个，数值不许改。**
`bc_avoid_*` 的默认值、最小值、最大值逐条来自参考客户端的 CVar 注册表（`config_variables_bestclient.h:610-670`），头文件里写明了 “must not be changed”。
旧集合（`bc_avoid_active`、`bc_avoid_sensing_radius`、`bc_avoid_quality`、`bc_avoid_randomness`、`bc_avoid_log`、`bc_avoid_debug_override`、`bc_avoid_show_hud`、`bc_avoid_show_visuals`、`bc_avoid_tile_*`、`bc_avoid_*_assist` …）已经全部不存在，源码里搜不到任何一个。
升级路径：`src/engine/client/client.cpp:5350-5355` 在 `m_ClConfigVersion < 3` 时把 `g_Config.m_BcAvoidEnabled = 0`，即老配置不会被静默地变成“已武装的另一个代理”。
旧名与新名的对应关系见 §6.5。

**2. 没有“感知半径”这个概念了。**
当前没有任何半径扫描层，参数表里也没有半径开关。危险判定只发生在克隆世界的逐步推演里：`TickHitHazard()`（`avoid_engine.cpp:97-156`）。
唯一带 `radius` 的参数是 `bc_avoid_fent_light_tile_radius`，它描述的是导航网格上的“浅冻可通行”距离，与威胁探测无关。

**3. 没有 `bc_avoid_active`，也没有 `bc_avoid_show_hud`；`WantsEveryTickInput` 也已删除。**
唯一总开关是 `bc_avoid_enabled`（`avoid.cpp:158`）。状态 HUD 是普通 HUD 模块：`HudLayout::MODULE_AVOID`，默认关闭，开关只有一份状态（`hud_layout.cpp:28-53` 的布局表 + `hud_editor.cpp:950`）。
输入钩子现在是**三态**的：`ApplyInput()` 返回 `INPUT_IDLE` / `INPUT_DRIVEN` / `INPUT_YIELDED`，其中 `INPUT_YIELDED` 专门处理“代理松手那一 tick 必须把玩家自己的输入发出去一次”的交接问题（§3）。

**4. 五个代理，一个 dispatcher。**
`Avoid::BLAgent` 是抽象基类，`CBasicAgent` / `CLegitAgent` / `CBlatantAgent` / `CFentbotAgent` / `CPilotAgent` 各实现一个 `GetAction()`。
Basic / Legit / Blatant 在本次调用里把搜索全部算完（同步）；Fentbot / Pilot 是**分片规划器**，一次调用只推进一个有上限的切片，跨 tick 记住搜索状态。

**5. 规划器有明确的时间切片常量。**
`PLANNER_STEPS_PER_TICK = 600`（每帧最多推进 600 个仿真 tick）、`NAV_WORK_PER_TICK = 20000`（每帧最多处理的网格瓦片数）、`SEARCH_COOLDOWN_TICKS = 10`（Fentbot 一轮结束后的冷却）、`PLAN_GUARD_TICKS = 6`（执行期的闭环护栏）、`LEGIT_DEADLINE_MS = 8.0`（Legit 的墙钟护栏）、`LIGHT_FREEZE_MIN_SPEED = 1.5f`（浅冻豁免的速度门槛）。全部在 `avoid_engine.cpp:27-48`。详见 §9。

**6. 本地化是硬契约。**
所有用户可见标签走 `BcLocalize()`，查询上下文固定为 `BestClient`（`localization.cpp:28-33`）。
每个 `BcLocalize("…")` 的字面量必须在 `data/BestClient/languages/simplified_chinese.txt` **和** `russian.txt` 的 `[BestClient]` 上下文下存在同名 key。
当前契约规模（写作时逐项复核）：5 个源文件共 **177** 个 key，两个语言文件各 **866** 条，缺失 **0**。自检脚本第 4 步会重新断言这件事，见 §8、§11。

**7. 自检脚本是 `scripts/avoid_selfcheck.sh`（315 行），共 8 个块（7 步 + 6b）。**
它先构建 `DDNet`，然后依次校验参数契约、参数接线、本地化、占位文案、引擎契约、HUD 接线与渲染顺序，最后构建并运行整个 `testrunner`。
动了这个模块的任何一行，跑它。每步断言什么见 §11。

**8. 参考规格的位置与章节对应。**
`docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md`：
§2 = 参数矩阵（默认/极值），§3 = 数据结构与 `BLAgent`/`BLAvoid` 接口，§4.1 = 前向推演引擎（含 `0x270f`、冻结条件、death/tele 判定），§5 = Basic，§6 = Blatant（kick-in 迟滞、候选空间、NSIF），§7 = Legit（MCTS 节点、UCT 公式、四阶段），§8 = Fentbot（档位表、流场点积 1750.0f），§9 = Pilot，§10 = 输入拦截管线。
本实现与它的字面差异是**有意为之**，逐条列在 §12。

---

## 2. 架构

### 2.1 分层

```
CAvoid (CComponent, avoid.h/.cpp)
  ├─ 输入钩子      ApplyInput()          ← gameclient.cpp 的 OnSnapInput 调用（三态，§3）
  ├─ 配置快照      ReadSettings()        → Avoid::SSettings
  ├─ 渲染          OnRender() / RenderHudModule() / RenderWorldOverlay()
  ├─ 遥测          STelemetry            → 菜单状态栏 / HUD / avoid_status
  └─ 分派          m_apAgents[NUM_AGENTS] → Avoid::BLAgent::GetAction(SContext, CGameWorld*)

Avoid::BLAgent (avoid_engine.h:334-348)
  ├─ CBasicAgent    同步，6 tick 固定前瞻，只改方向
  ├─ CLegitAgent    同步，UCT/MCTS + 拟人加权
  ├─ CBlatantAgent  同步，kick-in 迟滞 + 贪心 + 瞄准层 + NSIF
  ├─ CFentbotAgent  分片：CNavigator 流场 + 遗传微调 + CSimSession
  └─ CPilotAgent    分片：种群序列搜索 + 三种导航模式 + CSimSession

推演层 (avoid_engine.cpp)
  ├─ SimulateCandidate() / SimulatePlan()   阻塞式：一次调用算完
  ├─ CSimSession                            可恢复：Begin/Step/Finished/Outcome/Abort
  └─ CForwardSim + RunPlan + TickHitHazard  两者共用的物理与危险判定内核

导航层 (avoid_engine.h:259-325 声明, avoid_engine.cpp:506-785 实现)
  └─ CNavigator    瓦片分类 → 浅冻标记 → BFS 洪水 → 梯度流场（全部按 NAV_WORK_PER_TICK 分片）
```

规则：代理不直接读 `g_Config`，只读 `SContext::m_Settings`；代理不直接改玩家输入，只通过 `AvoidInput::m_Active` 表达“我要接管”。
`bc_avoid_draw_track_point` / `bc_avoid_draw_aimbot` 只被组件的世界覆盖层读取，`SSettings` 里没有对应字段。

### 2.2 文件地图

| 文件 | 作用 | 关键位置 |
| :--- | :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` | `CAvoid` 组件声明、`EAgent`、`EState`、`STelemetry`、`EInputResult`、`ApplyInput` | 25-33 / 35-42 / 47-61 / 77-85 / 102-116 |
| `src/game/client/components/bestclient/avoid.cpp` | 生命周期、控制台命令、`ReadSettings`、输入拦截（`FinishInput`）、遥测、HUD 模块、世界覆盖层 | 74-118 / 215-265 / 306-458 / 535-610 / 612-697 |
| `src/game/client/components/bestclient/avoid_engine.h` | `Avoid` 命名空间：常量、`SSettings`、`SSimFlags`、`SFlowField`、`SContext`、`AvoidInput`、`CSimSession`、`CNavigator`、`BLAgent` 与五个代理 | 30-48 / 48-111 / 116-166 / 209-249 / 259-325 / 334-458 |
| `src/game/client/components/bestclient/avoid_engine.cpp` | 模拟器、危险判定、流场网格、五个代理的算法 | 97-156 / 166-264 / 267-301 / 347-500 / 506-785 / 808-2174 |
| `src/game/client/components/bestclient/menus_avoid.cpp` | Avoid 设置页：状态栏、代理选择、共用框、每个代理的参数页签、Defaults；66-91 是三个排版帮手（`AvoidHintHeight` / `AvoidHint` / `AvoidHintBottom`） | 272-883 |
| `src/engine/shared/config_variables_bestclient.h` | 50 个 `bc_avoid_*` 的唯一定义处 | 610-670 |
| `scripts/avoid_selfcheck.sh` | 8 块回归自检 | 全文 |
| `docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md` | 参考实现规格（行为与常量的出处） | 全文 |

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
| `CMakeLists.txt:3039-3043` | 五个源文件加入客户端目标 |
| `data/BestClient/languages/{simplified_chinese,russian}.txt` | `[BestClient]` 上下文下的全部标签 |

### 2.4 与参考标识符的对应关系

| BestClient | 参考（spec / 二进制） | 说明 |
| :--- | :--- | :--- |
| `CAvoid` | `BLAvoid`（spec §3.3） | 主控制器。参考的 `ProcessInput()` 在这里叫 `ApplyInput()`，挂在 `OnSnapInput` 而不是 `CControls::OnMessage` 上（§3.1） |
| `CAvoid::EInputResult` | 无 | 本实现新增的三态返回值，用来表达“没动 / 接管了 / 刚松手要交接”（§3） |
| `CAvoid::STelemetry` | 无 | 本实现新增，仅供菜单/HUD/`avoid_status` 读取 |
| `Avoid::BLAgent` | `BLAgent`（spec §3.2） | 虚基类：`GetAction` / `OnRender` / `OnReset`；本实现额外有 `NavigatorReady()` |
| `Avoid::AvoidInput` | `AvoidInput`（spec §3.1） | `m_Input` + `m_Active` 语义一致；扩展了 `m_SurvivalTicks` / `m_UsedFallback` / `m_vPath` / `m_TrackPoint` / `m_AimTarget` / `m_aReason` |
| `Avoid::SContext` | `GetAction(const CNetObj_PlayerInput*)` 的参数 | 把 tick、本地 client id、输入、设置打成一个包 |
| `Avoid::SimulateCandidate` | `SimulateCandidate`（spec §4.1，`func_0x00014036a8d0`） | 克隆世界 + 逐 tick 推演 + 危险判定 |
| `Avoid::SimulatePlan` | 无独立符号 | 多 tick 输入序列版本（Fentbot/Pilot 的基因是多 tick 的） |
| `Avoid::CSimSession` | 无 | 同一个物理内核的**可恢复**版本，给两个规划器分片用 |
| `Avoid::CForwardSim` | 克隆世界 + 角色指针 | 内部实现细节（`avoid_engine.cpp:166-212`） |
| `Avoid::CNavigator` | Fentbot 的流场 | 可导航网格 + BFS 距离 + 梯度流场，增量构建；`IsLightTile()` 同时被模拟器用作“浅冻可通行”的裁判 |
| `CLegitAgent` 内的 `MCTSNode` | `MCTSNode`（spec §7.1，二进制 0x58 字节） | 局部结构体，每次决策 new/delete 整棵树 |
| `Avoid::ResolveFentPreset` | `0x1403356ba`（spec §8.1） | 档位表 88/160/1000 + 88/160/300 + 固定 8 tick / 10000 horizon |
| `FENT_FLOW_WEIGHT = 1750.0f` | `0x14054a6b8`（spec §8.2） | 速度·流场点积权重 |
| `WEIGHT_SCALE = 0.01f` | `0x1405300e4`（spec §7.2） | Legit 启发式的浮点缩放 |
| `SIMULATION_SAFE_CONSTANT = 9999` | `0x270f`（spec §4.1） | 整个前瞻窗口都活下来时的返回值 |
| `BASIC_CHECK_TICKS = 6` | spec §5.1 第 1 条 | Basic 的固定前瞻，故意不留 CVar |
| `LEGIT_DEADLINE_MS = 8.0` | 无 | 本实现新增的墙钟护栏（参考会掉帧），见 §12.4 |
| `LIGHT_FREEZE_MIN_SPEED = 1.5f` | 无 | 浅冻豁免的速度门槛：停在浅冻里不再算“穿过”（§12.1） |
| `m_vPendingPlan` / `m_PlanPending` | 无 | Fentbot 的计划暂存位，保证整条基因组一起换挡（§5.4） |

### 2.5 数据流

1. `CControls::SnapInput(pData)` 把这一 tick 的按键采样写进 `m_aInputData[g_Config.m_ClDummy]`。
2. `CGameClient::OnSnapInput()` 取该缓冲区的**只读引用**，复制成局部 `Input`，交给 `CAvoid::ApplyInput(&Input)`。
3. `ApplyInput` 组包：`Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy)`、`Ctx.m_Settings = ReadSettings()`、`Ctx.m_Input = *pInput`、`Ctx.m_LocalClientId` 由 `ActiveCore()` 决定。
4. `Avoid::GetActiveWorld(GameClient())` 选择推演世界（Fast Practice 沙盒 > 预测世界 > 游戏世界 > 预测世界）。
5. 被选中的代理返回 `AvoidInput`；`FinishInput()` 决定这一 tick 的返回值：接管 → 覆盖 `*pInput` 并返回 `INPUT_DRIVEN`；松手后的第一 tick → 原样返回 `INPUT_YIELDED`；其余 → `INPUT_IDLE`。
6. `OnSnapInput` 只在返回值不是 `INPUT_IDLE` 时 `mem_copy(pData, &Input, sizeof(Input))` 并把 `Ret` 置成 `sizeof(Input)`，让客户端把这份输入发出去/记账。
7. `UpdateTelemetry()` 把状态、存活 tick、耗时、瞄准标记和 reason 写进 `STelemetry`，供菜单状态栏、HUD 模块和 `avoid_status` 读取。

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

三态定义在 `avoid.h:79-85`：

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

1. **挂点在 `OnSnapInput`，不在 `CControls::OnMessage`（spec §10.2 的位置）。** 原因是这里能同时覆盖“要发包”和“不需要发包”的 tick，而且 `pData`/`Client()->GetInput()` 这条链路正是服务端与本地预测共同读取的输入（`gameclient.cpp:3168-3169` 的 `Client()->GetInput(Tick, …)` 拿到的就是 `client.cpp:402` 交给 `OnSnapInput` 的那块 `m_aInputs[...]` 内存）。
2. **组件从不写 `m_Controls.m_aInputData`。** 自检第 6 步断言 `avoid.cpp` 里不出现字符串 `m_Controls.m_aInputData`（`avoid_selfcheck.sh:297`），同时断言存在 `*pInput = m_LastOverride;` 与 `FinishInput(`（295-296）。原因见 3.2。
3. **每 tick 都会被调用一次。** `ApplyInput` 只依赖采样缓冲区，所以 `OnSnapInput` 不看 `Ret` 就调用它；`SnapInput` 只在“该发包”时填 `pData`，但这不影响判断。旧的 `WantsEveryTickInput()` 强制发包方案已删除，`controls.cpp` 与 Avoid 再无耦合（自检 298-299 断言这个符号在 `avoid.cpp` 与 `controls.cpp` 里都不存在）。
4. **由钩子按需索要数据包。** `INPUT_DRIVEN` 与 `INPUT_YIELDED` 都会让 `OnSnapInput` 把 `Ret` 提升为 `sizeof(Input)`；`INPUT_IDLE` 时 `Ret` 保持 `SnapInput` 的决定，采样器原本的合并策略（变化才发 / 至少 25 Hz）完全不受影响（自检 301 断言 `!= CAvoid::INPUT_IDLE` 这个测试存在）。
5. **返回值语义**写在 `avoid.h:77-85`；`INPUT_YIELDED` 的用途见 3.3。

### 3.2 为什么绝不能写 `m_Controls.m_aInputData`

契约：**Avoid 只写调用者给的局部副本，从不写按键采样状态。** `avoid.cpp` 里唯一的写回发生在 `FinishInput()`（327-334）：

```cpp
if(Drives)
{
    m_DroveTick = Tick;
    m_YieldTick = -1;
    *pInput = m_LastOverride;
    return INPUT_DRIVEN;
}
```

四个理由（每个都能在代码里核对）：

1. `m_aInputData` 是**按键采样状态**：`controls.cpp:144-152` 把方向/跳跃/钩索直接绑到按键状态指针上，它又被复制到 `m_aLastData` 作为“是否要发包”的比较基准。写它等于把机器人的输出变成下一 tick 的“玩家意图”。
2. 所有代理都以 `Ctx.m_Input`（来自采样缓冲区，也就是玩家的真实输入）作为“人类意图”基准：Basic 的基线测试、Blatant 的 kick-in 迟滞、Legit 的 `|DirDiff-2|` / `|HookDiff-1|` 拟人权重全靠它。一旦被污染，模块会把自己的上一次输出当成玩家意图并自我强化。
3. 这块缓冲区被别的功能共用：TAS 回放（`tas.cpp:912/918/1012/1020` 写入）、TAS 录制（现在从这里读，见 3.4）、快速换枪（`bestclient.cpp:49-63`）、dummy 交换（`gameclient.cpp:622-626`、`6228-6232`）、Fast Practice 的中立输入（`fast_practice.cpp:692`）。Avoid 写它会把机器人决策泄漏进这些功能。
4. 松开按键必须能立即收回控制权。只改这一份局部副本时，下一 tick 的 `Sampled` 又是玩家真实按键；改采样状态则会让机器人的方向“粘”在输入里。

### 3.3 每 tick 只决策一次、`m_LastOverride` 重放与 driving → yielded 交接

`FinishInput()` 是全部状态收口的地方（`avoid.cpp:327-345`）：

```cpp
CAvoid::EInputResult CAvoid::FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick)
{
    if(Drives)
    {
        m_DroveTick = Tick;
        m_YieldTick = -1;
        *pInput = m_LastOverride;
        return INPUT_DRIVEN;
    }

    // 代理刚停下的下一 tick 仍然要把玩家自己的输入送上网，因为采样器只是拿自己的原始状态和
    // 自己的原始状态比，根本看不见机器人改过什么。记住这个 tick 之后，同一个 tick 的重发也成立。
    if(m_DroveTick >= 0 && Tick == m_DroveTick + 1)
        m_YieldTick = Tick;
    m_DroveTick = -1;
    return m_YieldTick == Tick ? INPUT_YIELDED : INPUT_IDLE;
}
```

* **交接为什么必要**：采样器决定“要不要发包”时比较的是它自己的原始状态（`m_aInputData` vs `m_aLastData`），而机器人的改写只存在于发给服务端的数据包里。如果代理在 tick T 驱动、在 T+1 松手，而玩家自己的按键在 T→T+1 没有变化，采样器会认为“没变化、不需要发”，服务端就会继续沿用 T 的机器人输入。`INPUT_YIELDED` 强制在 T+1 发一次包，把玩家的真实输入送出去。
* **只交接一次**：`m_YieldTick` 只在 `Tick == m_DroveTick + 1` 时被设置；`m_DroveTick` 随即被清成 −1，所以同一次松手不会连续发两个交接包。同一个 tick 被重发时（控制器会重问 `OnSnapInput`），`m_DroveTick` 已经是 −1 但 `m_YieldTick == Tick` 仍然成立 → 依旧返回 `INPUT_YIELDED`（340 行的注释写明了这一点）。
* **早退分支也会交接**：`ApplyInput` 的三个提前返回（没有本地角色 `357-364`、总开关关闭 `366-376`、AFK 刚触发 `378-390`）全部走 `FinishInput(false, pInput, Ctx.m_Tick)`，所以“代理被关掉/人物消失”的那一 tick 同样会把玩家输入送出去。
* **重置会清掉交接状态**：`OnReset()`（`avoid.cpp:90-103`）与 `OnMapLoad()`（`105-118`）除了清遥测/路径/决策 tick 之外，也把 `m_DroveTick` 与 `m_YieldTick` 归为 −1（96-97、111-112），所以换图或重置之后不会凭空产生一个交接包。
* **`SetEnabled()` 故意保留两个 tick 字段**（`avoid.cpp:170-173` 的注释）：如果代理正在驱动时被关掉，下一 tick 仍需交接；其余情况下过期的值因为“必须相邻”这条判断而无害。
* 每 tick 只决策一次：`Ctx.m_Tick != m_LastDecisionTick` 才调用 `GetAction`（`395-428`），同一 tick 的重发只重放结论（`m_LastOverrideActive` + `m_LastOverride`）。规划器的切片计数、`m_CandidateIndex`、`m_Generation` 因此每 tick 至多前进一次；决策/接管/NSIF 三个计数器也是每 tick 至多 +1；`m_CostMs` 在重放路径上不刷新。
* `CheckAfkProtection` 在 tick 守卫**之前**，每 tick 都会跑：空闲计数按 50 次/秒增长，与 `50 * Set.m_AfkTime` 的阈值一致；命中时它调用 `SetEnabled(false)`，紧接着的第二次 `IsEnabled()` 检查保证**同一 tick** 就停止接管（`378-390` 的注释写明了这一点）。
* `SetAgent()` 会**同时重置旧代理和新代理**（`avoid.cpp:142-156`）：新代理不能继承任何状态，旧代理要释放它建好的导航网格（本模块最大的分配）。

### 3.4 与 TAS、Fast Practice、dummy 的关系

| 机制 | 行为 | 位置 |
| :--- | :--- | :--- |
| TAS 回放 | `m_Tas.IsPlaybackActive()` 为真时 `OnSnapInput` 直接返回 TAS 的输入，Avoid 完全旁路 | `gameclient.cpp:632-638` |
| TAS 录制 | 录制读的是**采样缓冲区**（`Sampled`），而不是 `SnapInput` 可能没填过的 `pData`；录到的是玩家原始输入，回放轨道保持 bit-exact | `gameclient.cpp:651-654` |
| Fast Practice | 录制被 `!m_FastPractice.Enabled()` 关掉；推演改用练习沙盒世界（`ActiveCore()` / `GetActiveWorld()`） | `gameclient.cpp:653`、`avoid.cpp:285-304`、`avoid_engine.cpp:791-802` |
| dummy | `ApplyInput` 只在 `!Dummy` 分支被调用；它没有 `Dummy` 形参，tick 一律取 `g_Config.m_ClDummy` | `gameclient.cpp:643`、`avoid.cpp:347-355` |
| 菜单入口 | TAS& 页面第二个子页签 “Avoid”（`g_Config.m_BcTasTab == 1`） | `menus_tas.cpp:595-621` |

---

## 4. 前向推演引擎

### 4.1 `CGameWorld::CopyWorldClean`

推演的起点是克隆世界（`avoid_engine.cpp:179`、`396`、`1631`、`2014`）：

```cpp
m_World.CopyWorldClean(pBaseWorld);
m_World.m_WorldConfig.m_PredictEvents = false;
```

`CopyWorldClean`（`src/game/client/prediction/gameworld.cpp:669-716`）与 `CopyWorld`（同文件 718 起）的区别只有一处，但很关键：`CopyWorld` 会把副本挂进预测世界的父子链（`m_pParent` / `m_pChild`，并把链上旧副本标记为失效），`CopyWorldClean` 做的是**脱离链条的完整深拷贝**。
模拟器必须要“干净”的克隆：一个 tick 里可能同时存在多个克隆（Fentbot 的 `m_pSnapshot` 加 `CSimSession` 的世界），它们绝不能互相把对方标记成失效，也不能干扰客户端自己的 `m_PredictedWorld`。这是有意选择，见 §12.2。
`m_PredictEvents = false` 关掉克隆里的预测事件重放，避免推演时触发副作用。

### 4.2 单 tick 步进与推演主循环

`CForwardSim`（`avoid_engine.cpp:166-212`）持有克隆世界、本地角色指针、碰撞指针和本地 client id：

```cpp
m_World.CopyWorldClean(pBaseWorld);                                  // 179
m_pChar = m_World.GetCharacterById(m_LocalClientId);                 // 183
...
void SetPredictPlayers(bool PredictPlayers)                          // 193-202
{
    if(PredictPlayers) return;
    for(int i = 0; i < MAX_CLIENTS; ++i)
        if(i != m_LocalClientId)
            m_World.m_Core.m_apCharacters[i] = nullptr;              // 其他 tee 不再挡路
}
void Step(const CNetObj_PlayerInput &Input)                          // 205-211
{
    m_pChar->OnDirectInput(&Input);
    m_pChar->OnPredictedInput(&Input);
    m_World.Tick();
    m_pChar = m_World.GetCharacterById(m_LocalClientId);             // 角色可能在 Tick 里被销毁
}
```

`SimulatePlan()` 在 `Begin()` 之后调用一次 `Sim.SetPredictPlayers(Flags.m_PredictPlayers)`（376）；`CSimSession::Begin()` 里对着自己的克隆做同样的清理（`408-415`）。这就是 `bc_avoid_player_prediction` 的全部实现，见 §12.6。

`RunPlan()`（215-264）是 `SimulatePlan` 与 `CSimSession` 共用的循环体：

* 可选记录轨迹：先把起点压入 `pvPath`，每 tick 再压一次当前位置；调用方用 `bc_avoid_draw_path` 决定是否要路径。
* 可选流场打分：`FlowScore += (Vel.x*Dir.x + Vel.y*Dir.y) * pFlow->m_Scale`，索引取自 `GetPureMapIndex(位置)`。
* 危险判定在**步进之后**：`if(TickHitHazard(...)) break;`，返回值为“完整活下来的 tick 数”。
* 活满整个窗口 → 返回 `SIMULATION_SAFE_CONSTANT`。

输入取法（`InputAt`，158-163）：`pInputs[min(Tick, NumInputs - 1)]`。因此 `SimulateCandidate`（单输入）等于“把这个输入按住整个前瞻窗口”，`SimulatePlan`（多输入）等于“先按序列走完，再用最后一个输入补满”。

失败即安全：`SimulatePlan()` 在 `pInputs` 为空 / `NumInputs <= 0` / `CheckTicks <= 0`，或者克隆世界或本地角色不存在时，直接返回 `SIMULATION_SAFE_CONSTANT`（370-375）。这是有意的失败安全策略，副作用写在 §12.9。

### 4.3 危险判定谓词（`TickHitHazard`，97-156）

判定顺序固定：**死亡 → 冻结 → 传送 → 解冻**。死亡排在冻结之前是有意的：同一格可能一层是 freeze、另一层是 death，浅冻豁免绝不能把这种情况判成安全（105-107 的注释）。

| 谓词 | 代码条件 | 参考出处 | 开关 |
| :--- | :--- | :--- | :--- |
| 死亡 | 位置周围 5 点（中心 + 半径 `GetProximityRadius()/3.0f` 的四个对角）上，任一 `GetCollisionAt(...) & TILE_DEATH` **或** `GetFrontCollisionAt(...) & TILE_DEATH`（111-120） | spec §4.1 第 4 步（`Tile & TILE_DEATH`）；角点几何与引擎自己的死亡探测一致（`character.cpp:1207-1214`），本实现把两层都查了，见 §12.1 | `Flags.m_AvoidDeath` |
| 冻结（永远生效） | `pChar->m_FreezeTime > 0 \|\| pChar->m_FrozenLastTick \|\| pCore->m_IsInFreeze \|\| pCore->m_DeepFrozen \|\| pCore->m_LiveFrozen` | spec §4.1 第 3 步（`0x14036a98b-0x14036a9a4`：`m_FreezeTime > 0`、`m_FrozenLastTick`、`m_DeepFrozen`）；本实现额外判 `m_IsInFreeze` / `m_LiveFrozen`，见 §12.1 | `Flags.m_AvoidFreeze`；所有代理都硬编码为 `true` |
| 浅冻豁免 | 上面的冻结判定只有在 `Flags.m_AllowLightFreeze && Flags.m_pNav && !m_DeepFrozen && !m_LiveFrozen && length(m_Vel) > LIGHT_FREEZE_MIN_SPEED && m_pNav->IsLightTile(Pos)` 全真时才放行（133-138） | 无（本实现把 Fentbot 的浅冻规则同步给模拟器，并加了速度门槛，见 §12.1/§12.6） | 仅 Fentbot 打开：`Flags.m_AllowLightFreeze = Set.m_FentLightTile; Flags.m_pNav = &m_Nav;`（1545-1546） |
| 传送 | `GetPureMapIndex(位置)` 属于 `IsTeleport` / `IsEvilTeleport` / `IsCheckTeleport` / `IsCheckEvilTeleport` / `IsTeleCheckpoint` | spec §4.1 第 4 步（只点名了 `GetTeleCheckpoint`），见 §12.1 | `Flags.m_AvoidTeles` |
| 解冻 | 仅当 `Tick < Flags.m_UnfreezeTicks` 时，`IsUnfreezeTile`（`TILE_UNFREEZE`，含 front 层） | spec §2 的 `krx_avoid_tile_legit/blatant_unfreeze_tile(_ticks)` | `Flags.m_AvoidUnfreeze` + `Flags.m_UnfreezeTicks` |

速度门槛的意义：`LIGHT_FREEZE_MIN_SPEED = 1.5f`（px/tick，`avoid_engine.cpp:31`）。没有它，停在浅冻瓦片上的 tee 会被判成“活着”，搜索就会把“停在冻结里”当成满分手牌；有了它，只有仍在移动地穿过浅冻才算过关（123-128 的注释）。

补充：引擎自己会把“站在 FREEZE/DFREEZE/LFREEZE/DEATH 瓦片上”或“角点碰到 death 瓦片”写进 `m_Core.m_IsInFreeze`（`character.cpp:1185-1214`），所以本实现用 `m_IsInFreeze` 判冻结与游戏的实际手感一致；这也是 §12.1 里那条“故意更严格”的来源。

`SIMULATION_SAFE_CONSTANT = 9999` 定义在 `avoid_engine.h:30`（对应 `0x270f`），它只是**推演层的返回值**：每个代理在报 `m_SurvivalTicks` 时都会把它换算成自己实际要看的 tick 数（Basic → 6、Blatant/Legit → `CheckTicks`、两个规划器 → 护栏窗口），所以**没有任何代理会把 9999 交给 UI**，状态栏与 HUD 的存活读数永远是一个 tick 数（见 §7.1、§7.5）。

### 4.4 `CSimSession`（可恢复推演）

`CSimSession`（`avoid_engine.h:209-249`，实现 `avoid_engine.cpp:381-500`）是同一个物理内核的分片版本，只有两个规划器用它：

| 接口 | 语义 |
| :--- | :--- |
| `Begin(client, world, pInputs, NumInputs, CheckTicks, Flags, pFlow)` | 先 `Abort()`，再 `new CGameWorld` + `CopyWorldClean(pBaseWorld)`，解析本地角色与碰撞，按 `Flags.m_PredictPlayers` 清空其他 tee；失败时释放并返回 `false`。成功时把 `m_Outcome.m_EndPos` 预置为起点 |
| `Step(MaxSteps)` | 最多推进 `MaxSteps` 个仿真 tick，返回实际推进数；中途角色消失或命中危险都会置 `m_Finished` 并写 `m_Outcome.m_SurvivalTicks` |
| `Finished()` | 本候选是否算完 |
| `Outcome()` | `m_SurvivalTicks`（活满窗口则为 9999）、`m_FlowScore`、`m_EndPos` |
| `Abort()` | `delete m_pWorld`，清空全部指针与结果；`m_Finished = true`。析构与 `OnReset()` 都会调用 |

与 `SimulatePlan` 的差异：多返回 `m_EndPos`（Pilot 用来算距离惩罚）、多累加 `m_FlowScore`、按 tick 分片、每个候选一个 `CGameWorld` 堆对象。
调用方的循环模式是固定的（`avoid_engine.cpp:1650-1690`、`2039-2071`）：`Finished()` → `Begin()` 下一个候选 → `Step(StepsLeft)` → `StepsLeft -= Taken` → 若 `Finished()` 则结算 fitness。`Taken <= 0` 时跳出（防死循环）。

**指针生存期规则（改这里最容易踩）。**
会话同时持有两类外部指针，它们都指向 `CNavigator` 的内部缓冲区：

```cpp
const std::vector<vec2> &vFlow = m_Nav.Flow();
m_SessionFlow.m_pDir = vFlow.empty() ? nullptr : vFlow.data();   // 1639-1642 / 2031-2034
Flags.m_pNav = &m_Nav;                                           // 1546（浅冻判定用整个网格）
```

* 只要不调用 `CNavigator::Reset()` / `Rebuild()`，地址就不变：构建阶段只在原地写元素（`ClassifyTiles`、`StepMarkLight`、`BuildGradient`），`FlowAt()` / `IsLightTile()` 也只读。因此“搜索进行中同时建网格”是安全的。
* `Rebuild()` 走 `Reset()`，会把 `m_vGrid` / `m_vLightSeen` / `m_vDist` / `m_vFlow` 全部 `clear()` 后重新 `assign`，缓冲区必然换地址。**任何可能重建导航器的分支，都必须先 `m_Session.Abort()`**：Fentbot 的 `1552-1557`、Pilot 的 `1946-1951`，以及计划过期分支（Fentbot `1731-1742`）。
* `SFlowField` 是值成员（`m_SessionFlow`，`avoid_engine.h:396` / `435`，注释里写明了原因），`SSimFlags::m_pNav` 也活在整个会话期间，不要改成局部变量或临时对象。

### 4.5 流场打分

`RunPlan` 与 `CSimSession::Step` 用同一式子：每 tick 取当前速度与所在瓦片流场单位向量的点积，乘以 `pFlow->m_Scale` 累加。
`m_Scale` 由调用方定：Fentbot 用 `FENT_FLOW_WEIGHT / 50.0f = 35`（`avoid_engine.cpp:1645`，注释说明速度单位是 px/tick，所以按参考的浮点常量做了同比缩放），Pilot 用 `1.0f`（`2035`）并在 fitness 里再乘 4（`2062`）。

---

## 5. 五个代理

选中的代理由 `bc_avoid_agent` 决定，`CAvoid::Agent()` 会把它夹到 `[0, NUM_AGENTS-1]`；`SetAgent()` 会重置新旧两个代理的状态并丢弃路径（`avoid.cpp:124-156`）。

### 5.1 Basic（`CBasicAgent`，`avoid_engine.cpp:808-872`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §5（Basic Agent 1:1） |
| 前瞻 | 固定 6 tick：`BASIC_CHECK_TICKS = 6`（`avoid_engine.h:33`），故意没有 CVar |
| 候选空间 | `{0, -1, 1}`，正好这个顺序（`s_aCandidateDirs[3] = {0, -1, 1}`，`avoid_engine.cpp:835`） |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`（硬编码，对应 spec §5 里 `avoid-teles is false` 的注释） |
| 读取的参数 | `bc_avoid_player_prediction`（→ `Flags.m_PredictPlayers`）、`bc_avoid_draw_path`（只为填 `Out.m_vPath`） |
| 跨 tick 状态 | **无**。`CBasicAgent` 没有任何成员变量 |

算法（顺序即语义）：

1. 先推演玩家原输入 6 tick。返回 9999 → `m_Active = 0`，reason `"player input safe"`，`Out.m_SurvivalTicks = 6`。
2. 否则以基线为 `BestScore`，按 `{0, -1, 1}` 依次推演，**只在严格更优时**替换（`Score > BestScore`），一旦某个候选返回 9999 立即 `break`（843-849）。因此“同时可行”时以枚举顺序里最先达到 9999 的候选为准。
3. `BestScore > Baseline` → 只改 `Out.m_Input.m_Direction`，`m_Active = 1`；reason 按方向给 `"brake before hazard"` / `"steer left before hazard"` / `"steer right before hazard"`；开了 `bc_avoid_draw_path` 时再推一次用于画线。
4. 没有任何候选更优 → reason `"no safer plan"`，原输入照发。

`m_SurvivalTicks` 在 Basic 下最多是 6（9999 被归一化成 `BASIC_CHECK_TICKS`，827、856），所以 HUD 的 Safe 行在 Basic 接管时显示 “6 tick”。（事实上所有代理都不会把 9999 报给 UI，Safe 行永远是 tick 数，见 §7.1。）跳跃、钩索、准星一概不动。

### 5.2 Blatant（`CBlatantAgent`，`avoid_engine.cpp:878-1192`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §6（kick-in 迟滞 6.1、候选空间 6.2、并发贪心 6.3、NSIF 6.4） |
| 危险开关 | `m_AvoidFreeze = true` + `m_AvoidDeath = m_BlatantDeath` + `m_AvoidTeles = m_BlatantTeles` + `m_AvoidUnfreeze = m_BlatantUnfreeze` / `m_UnfreezeTicks = m_BlatantUnfreezeTicks` |
| 读取的参数 | `bc_avoid_kick_in_ticks`、`bc_avoid_blatant_check_ticks`、`bc_avoid_blatant_direction`、`bc_avoid_blatant_hook`、`bc_avoid_blatant_teles/death/unfreeze/unfreeze_ticks`、`bc_avoid_nsif`、`bc_avoid_track_point`、`bc_avoid_safe_aim_tracking`、`bc_avoid_auto_drag`、`bc_avoid_aimbot`、`bc_avoid_aimbot_fov`、`bc_avoid_aimbot_segments`、`bc_avoid_auto_aim`、`bc_avoid_aim_assist`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_TrackPointValid` / `m_TrackPointPos`（锁定瞄准点）、`m_SavedSafeSequence`（NSIF 缓存）。`OnReset()`（878-883）清空三者 |

执行顺序：

1. **kick-in 迟滞**（936-945）：用 `bc_avoid_kick_in_ticks`（默认 26）推演玩家原输入。返回 9999 → 清空 `m_SavedSafeSequence`，`m_Active = 0`，reason `"player input safe"`，`Out.m_SurvivalTicks = KickInTicks`。只要玩家自己能活够 kick-in 窗口，机器人就让路，即使 check-ticks 窗口更长的搜索能找到别的活路。
2. **track point**（916-934）：开着 `bc_avoid_track_point` 时，用 `IsHookable()` 沿玩家当前准星方向做钩索射线；命中可钩瓦片就把命中点记成 `m_TrackPointPos`（`m_TrackPointValid = true`）。关掉该参数时立即失效。有效时写进 `Out.m_TrackPoint`（HUD 世界覆盖层用）。
3. **候选动作**（949-968）：`{0, -1, 1} × {0, 1}`（方向 × 钩索），参数关掉时对应维度只留 1 个取值——即“保持玩家当前值”。每个候选都用 `bc_avoid_blatant_check_ticks` 推演，`Survival` 取“9999 → CheckTicks”，保留**第一个**严格最大值（`Survival > BestSurvival`），任一候选返回 9999 就置 `FoundSafe = true`。
4. **内部瞄准层**（989-1136，仅当 `bc_avoid_aimbot` 打开）：分两步。
   * 先收集候选瞄准方向 `vAims`：track point（`bc_avoid_track_point` 且 `m_TrackPointValid`；开 `bc_avoid_safe_aim_tracking` 时先做安全检查，1005-1015）；auto drag（`bc_avoid_auto_drag` 且 `bc_avoid_player_prediction`，取 `HookLength` 内最近的另一名玩家，1023-1043）；然后是 FOV 扇形扫描 —— 以玩家准星角 `atan2(PlayerAim)` 为中心、`bc_avoid_aimbot_fov`（10–360°）为扇角、`bc_avoid_aimbot_segments`（1–64）条采样，只有 `IsHookable()` 命中的方向才继续，并对每个方向做一次 `hook = 1` 探针（1062-1094）。
   * **safe aim tracking 的判据是“同一窗口上比玩家输入活得久”**：被跟踪的瞄准与玩家自己的输入都用 `CheckTicks` 推演一次，`Usable = Res == 9999 || Res > PlayerCheck`（1007-1014）。两边量的是同一个窗口，不会出现“被跟踪的瞄准死得晚、却拿去和短窗口的基线比”这种偏差。
   * 再从这些候选里**选择**真正要评估的瞄准：`FixedAims`（track point + auto drag）无条件入选；`bc_avoid_auto_aim` 选“探针存活最久”的扇区；`bc_avoid_aim_assist` 选“探针全安全且离准星最近”的扇区（`AngleDiff` 取扇区偏移，1082-1091）。`AddAim` 用 `dot(Existing, Dir) > 0.9999f` 去重，避免为同一个方向重复付费（1097-1107）。
   * 最后对每个入选瞄准 × 每个动作做一次推演（1118-1135）：严格更优才取代（`Survival > BestSurvival`），并置 `BestFromAimbot = true`；命中 9999 同样置 `FoundSafe`。
5. **NSIF**（1138-1156）：
   * `FoundSafe` → `m_SavedSafeSequence.clear(); push_back(BestAction);`
   * 否则若 `bc_avoid_nsif` 且缓存非空 → 取出 `front()` 并 **`erase(begin())`**（与 spec §6.4 一致：这一步被消费掉了，不是无限重放），然后用同一个 check 窗口重推它来得到**真实的**存活 tick（1153-1154）并置 `UsedFallback = true`。
6. **接管判定**（1158-1189）：
   * gate 是 `if(BestSurvival > 0 || UsedFallback)`，忠实于 spec §6.4 第 4 步：**只有搜索真的产出了一条序列（某个候选至少活过 1 tick）才应用计划**。当所有候选都在第 0 tick 就死掉时，`BestSurvival == 0`，玩家保留自己的输入，走 `else` 分支给出 reason `"no safer plan"` 与 `Out.m_SurvivalTicks = KickSafety`。这个分支现在是活的，不再是理论情况。
   * 接管时 `Out.m_Input = BestAction`、`m_Active = 1`、`Out.m_SurvivalTicks = BestSurvival`（NSIF 回退时就是刚测出来的真实值，不伪造 9999）；reason 优先级为 NSIF（`"NSIF: replay saved safe input"`）→ 瞄准变化（`"hook the safe aim"` / `"aim clear of the hazard"`）→ 钩索变化（`"hook to safety"` / `"release hook"`）→ 方向（`"brake before hazard"` / `"steer away from hazard"`）。
   * **`Out.m_AimTarget` 只在接管分支内发布**（1167-1172）：瞄准层算过什么不影响 `m_AimTarget`，只有真正被采纳的那次瞄准才会点亮 HUD 覆盖层。因此关掉/未采用的瞄准永远不会在画面上出现。
   * 路径只在 `bc_avoid_draw_path` 且非 NSIF 回退时重算（1182-1183）。

`STATE_NSIF` 只在这个回退路径上产生（`avoid.cpp:457`）。由于每个安全序列只存一步、回退即消费，NSIF 通常只持续一个 tick。

### 5.3 Legit（`CLegitAgent`，`avoid_engine.cpp:1198-1376`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §7（MCTS 节点 7.1、UCT 公式 7.2、四阶段 7.3） |
| 前瞻 | `bc_avoid_legit_check_ticks`（默认 6） |
| 迭代数 | `bc_avoid_legit_iterations`（默认 100，上限 1000） |
| 危险开关 | `m_AvoidFreeze = true` + `m_AvoidDeath = m_LegitDeath` + `m_AvoidTeles = m_LegitTeles` + `m_AvoidUnfreeze = m_LegitUnfreeze` / `m_UnfreezeTicks = m_LegitUnfreezeTicks` |
| 读取的参数 | 上面 6 个 + `bc_avoid_legit_direction` / `bc_avoid_legit_hook` / `bc_avoid_legit_direction_weight` / `bc_avoid_legit_lifespan_weight` / `bc_avoid_legit_hook_weight` / `bc_avoid_legit_exploration` / `bc_avoid_player_prediction` / `bc_avoid_draw_path` |
| 跨 tick 状态 | **无**。搜索树在单次 `GetAction` 里建立、选完即 `delete pRoot` |

UCT 启发式（1244-1252，逐字对应 spec §7.2）：

```
DirDiff   = |(float)Action.m_Direction - (float)Ctx.m_Input.m_Direction|
DirScore  = |DirDiff - 2.0f|  × (WeightDir      × 0.01f)
HookDiff  = |(float)Action.m_Hook      - (float)Ctx.m_Input.m_Hook|
HookScore = |HookDiff - 1.0f| × (WeightHook     × 0.01f)
LifeScore = SurvivalTicks     × (WeightLifespan × 0.01f)
Heuristic = DirScore + HookScore + LifeScore          // WEIGHT_SCALE = 0.01f
```

注意方向项的形状：候选与玩家方向相同时 `DirDiff = 0` → `|0-2| = 2`（满分），相差 2（左↔右）时得 0 分。

四阶段（每次迭代）：

1. **Selection**（1268-1296）：从根往下，选 `Score` 最大的子节点；`Score = Exploitation + Exploration + Heuristic(child)`，其中 `Exploitation = m_TotalValue / m_Visits`，`Exploration = ExplorationC × sqrt(log(max(1, parentVisits)) / childVisits)`，`ExplorationC = bc_avoid_legit_exploration`（整数直接当 double 用）。未访问过的子节点得 `3.402823466e+38`（FLT_MAX），保证每个子节点至少被走一次。
2. **Expansion**（1298-1321）：当前节点非终局且 `m_Visits > 0` 时展开 `{-1, 0, 1} × {0, 1}`（Legit 的展开顺序与 Basic/Blatant 的 `{0,-1,1}` **不同**，这是 spec §7.3 的顺序），`bc_avoid_legit_direction` / `bc_avoid_legit_hook` 关掉时对应维度只留 1 个取值；随后用**全局 `rand()`** 随机挑一个孩子继续。
3. **Rollout**（1323-1328）：`SimulateCandidate(m_pClient, pWorld, pCurr->m_Action, CheckTicks, Flags)`；`m_LifespanTicks = (9999 ? CheckTicks : Survival)`；奖励 `1.0`（全安全）或 `Survival / CheckTicks`；`Survival < CheckTicks` 时把该节点标记为终局。
4. **Backpropagation**（1330-1335）：沿父链把 `m_Visits++` 与 `m_TotalValue += Reward`。

墙钟护栏：`LEGIT_DEADLINE_MS = 8.0`，循环开头每 8 次迭代检查一次（`if((Iter & 7) == 0 && time_get() >= Deadline)`，1262-1266），命中就 `break` 并记住 `DeadlineHit`。菜单的 Priority 页签有一条提示专门说明这件事（`menus_avoid.cpp:625-627`）。

最终决策（1338-1372）：选**访问次数最多**的根子节点；只要它的 `m_Direction` 或 `m_Hook` 与玩家输入不同就接管（spec §7.3 的“只要与玩家按键不同就应用”），reason 依次是 `"hook and steer to safety"` / `"hook to safety"` / `"release hook"` / `"brake before hazard"` / `"steer to safety"`；没有可用子节点或与玩家输入一致时，`Out.m_SurvivalTicks = CheckTicks`，reason 为 `"search budget reached"`（护栏命中）或 `"player input safe"`。

### 5.4 Fentbot（`CFentbotAgent`，`avoid_engine.cpp:1382-1768`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §8（档位表 8.1、流场点积 8.2） |
| 类型 | 分片规划器：`CNavigator` 流场 + 遗传微调 + `CSimSession` |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`，外加浅冻豁免 `m_AllowLightFreeze = Set.m_FentLightTile` / `m_pNav = &m_Nav`（1537-1546） |
| 读取的参数 | `bc_avoid_fent_quality`、`bc_avoid_fent_advanced`、`bc_avoid_fent_ticks`、`bc_avoid_fent_tweaker_actions`、`bc_avoid_fent_tweaker_ticks`、`bc_avoid_fent_tweaker_dosage`、`bc_avoid_fent_light_tile`、`bc_avoid_fent_light_tile_radius`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_Nav`、`m_pSnapshot` + `m_SnapshotValid`、`m_vCandidates` + `m_vFitness`、`m_CandidateCount`、`m_CandidateLength`、`m_CandidateIndex`、`m_Generation`、`m_Cooldown`、`m_SessionFlow`、`m_Session`、`m_vPlan`、`m_vPendingPlan` + `m_PlanPending`、`m_PlanIndex`、`m_BestFitness`；`OnReset()`（1388-1408）逐个复位 |

**档位表（`ResolveFentPreset`，267-301，对应 `0x1403356ba`）**：`bc_avoid_fent_advanced == 0` 时档位覆盖这四个值，四档的 horizon 都是 10000、hold 都是 8 tick：

| `bc_avoid_fent_quality` | 档位 | `m_FentActions` | `m_FentDosage` | `m_FentHoldTicks` | `m_FentHorizon` |
| :---: | :--- | ---: | ---: | ---: | ---: |
| 0 | Low | 88 | 88 | 8 | 10000 |
| 1 | Mid | 160 | 160 | 8 | 10000 |
| 2 | Max | 1000 | 300 | 8 | 10000 |

`bc_avoid_fent_advanced == 1` 时改为读取自定义值并在引擎里再夹一次：`actions = clamp(tweaker_actions,50,5000)`、`dosage = clamp(tweaker_dosage,1,500)`、`hold = clamp(tweaker_ticks,1,30)`、`horizon = clamp(fent_ticks,1000,10000)`（294-300）。菜单里档位选择器与高级滑条互斥显示（`menus_avoid.cpp:762-792`）。

每次 `GetAction` 的四段（1515-1768）：

1. **网格与流场**（1548-1559）：地图尺寸变化、浅冻设置变化（`LightTile()` / `LightRadius()` 与当前参数不一致）、或既没就绪也没在构建时 → `m_Session.Abort()` + `m_Nav.Rebuild(pCollision, Set.m_FentLightTile, Set.m_FentLightTileRadius)` + `m_SnapshotValid = false`；未就绪则 `m_Nav.Update(pCollision, NAV_WORK_PER_TICK)`。`Length = clamp(m_FentHoldTicks,1,30)`、`Horizon = clamp(m_FentHorizon,1,10000)`。
2. **保底动作**（`FallbackAction`，1565-1618）：任何时刻都有输出。`Guard = clamp(Length, PLAN_GUARD_TICKS, 10)`（故意便宜：它可能连续跑很多 tick）；先推玩家原输入作为基线，再按流场方向决定方向槽的顺序，枚举 `3 方向 × 2 跳跃 × 2 钩索 = 12` 个候选（钩索时准星对准流场方向），严格更优才替换。reason：接管则 `"survival fallback while searching"`，否则 `"player input safe"`。
   **方向槽是一个真正的排列**（1573-1590）：`aDirs[0] = 0`（不动）、`aDirs[1] = -1`（左）、`aDirs[2] = 1`（右）；`aOrder` 是 `{1,2,0}`（流场不可用时：左、右、不动），有右向流场时 `{2,0,1}`（右、不动、左），有左向流场时 `{1,0,2}`（左、不动、右）。三个槽各被枚举一次，流场最可能想要的那个排在最前，所以最可能的候选最先被推演。
3. **搜索切片**（1623-1704）：`m_Cooldown` 每 tick 递减；为 0 时执行
   * 还没有快照 → `m_Session.Abort()`、`CopyWorldClean(pWorld)` 到 `m_pSnapshot`、`SeedGeneration()`；
   * 刷新 `m_SessionFlow`（`m_pDir = m_Nav.Flow().data()`、宽高取自 `m_Nav`、`m_Scale = 1750.0f / 50.0f`）；
   * `StepsLeft = PLANNER_STEPS_PER_TICK`；候选逐个 `Begin`→`Step`，结算 `Fitness = Survival + FlowScore − 2 × DistanceAt(m_EndPos)`（`Survival` 把 9999 换成 `Horizon`；导航距离为 −1 时不扣）；
   * **随时发布（anytime publication，1679-1688）**：某个候选的 `Fitness > m_BestFitness` 时，把这一条基因组写进 **`m_vPendingPlan`** 并置 `m_PlanPending = true`（不直接改正在执行的 `m_vPlan`）。`m_BestFitness` 在 `SeedGeneration` 里重置（1421），因此它在一整轮（多代）内单调递增，跨代继承“目前最好”。
   * 整代算完（`m_CandidateIndex >= Count`）→ `m_Generation + 1 < max(1, m_FentDosage)` 时 `Breed()`，否则 `m_SnapshotValid = false; m_Cooldown = SEARCH_COOLDOWN_TICKS`（睡 10 tick 后开新一轮）。
* 种子生成（`SeedGeneration`，1410-1462）：`Count = clamp(m_FentActions,1,5000)`、`Length = clamp(m_FentHoldTicks,1,30)`；碱基是玩家输入但清空跳跃与钩索；偶数个体跟流场走（`Flow.x > 0.25` → 右，`< -0.25` → 左；`Flow.y < -0.25` → 跳），`i % 4 == 1` 的个体方向取 `(i % 3) - 1`；只有在 `i % 2 == 1 || t > 0` 时才做突变（`rand()` 位测试：方向 1/4、跳跃 1/16、钩索 1/16）。也就是说“偶数个体的第 0 个基因”正好是干净的流场跟随动作。
* 繁殖（`Breed`，1464-1513）：`Keep = clamp(Count/4, 1, Count)`；按 fitness 稳定降序排序；前 `Keep` 个精英原样保留（`ParentA = vOrder[i % Keep]`），其余逐个基因从两个父本（`rand() & 1`）取，再按 1/5 方向、1/11 跳跃、1/13 钩索突变；之后 `m_Generation++`、fitness 重置为 `-1e30`、`m_CandidateIndex = 0`。**注意**：种群形状以正在跑的这一代为准，参数变化只在下一次 `SeedGeneration` 生效。
4. **执行与闭环护栏**（1706-1767）：
   * 没有计划就直接返回保底动作（`1707-1708`）。
   * **换挡是一次原子交换**（1710-1720）：只有当 `m_PlanPending` 为真**且**当前基因组已经走到最后一步（`m_PlanIndex >= LastStep`）时，才 `m_vPlan.swap(m_vPendingPlan)`、清空暂存、`m_PlanPending = false`、`m_PlanIndex = 0`。这条规则的作用是：**永远不会把旧基因组的尾巴和新基因组的头连着开**（那会把同一个跳跃/钩索重新触发一遍）；没有更新的基因组时，最后一个输入被一直保持。
   * 用 `PLAN_GUARD_TICKS = 6` 推演这一步：不是 9999 时再推玩家原输入，如果“计划 ≤ 玩家”就认定计划过期 → 清空计划与暂存、`m_PlanPending = false`、`m_PlanIndex = 0`、`m_SnapshotValid = false`、`m_Cooldown = 0`、`m_CandidateCount/Length = 0`、`m_Session.Abort()`，返回保底动作（`1724-1744`）。
   * 计划可用则 `m_PlanIndex++`（只在未到末尾时）、`m_Active = 1`、reason `"fentbot plan"`、`m_SurvivalTicks = (9999 ? 6 : PlanSafety)`。`bc_avoid_draw_path` 时沿流场走 40 步、每步 24 px 生成可视化折线。

网格分类（`CNavigator`，506-785）：`NAV_BLOCKED=0 / NAV_OPEN=1 / NAV_GOAL=2 / NAV_LIGHT=3`。实心或死亡瓦片一律 blocked；deep freeze 永远 blocked；普通 freeze 先一律 blocked（566-571），unfreeze 瓦片在分类阶段就被收集成浅冻 BFS 的种子（575-578）。`StepMarkLight`（588-636）从每个 unfreeze 种子向外做最多 `m_LightRadius` 步的四邻 BFS：穿过可走瓦片，遇到 blocked 且是普通 freeze 的瓦片就标成 `NAV_LIGHT`（可通行），**实心、deep freeze 与死亡瓦片永不通行**（623-627）；每个瓦片只访问一次（`m_vLightSeen`），深度到 `Radius` 就停。目标瓦片优先取 finish（`NAV_GOAL`），地图没有 finish 时退化为 unfreeze 瓦片。阶段顺序 `PHASE_CLASSIFY → PHASE_MARK_LIGHT → PHASE_FLOOD → PHASE_GRADIENT → PHASE_IDLE`，每帧总工作量受 `NAV_WORK_PER_TICK` 限制，`Ready()` 之前 `NavigatorReady()` 为假（菜单状态栏显示 `grid: building`）。
浅冻规则不只影响寻路：`Flags.m_AllowLightFreeze` + `m_pNav->IsLightTile()` 让模拟器也接受这些瓦片（§4.3），并且要求角色仍在移动，否则规划会把“停在冻结里”当成满分。

### 5.5 Pilot（`CPilotAgent`，`avoid_engine.cpp:1774-2174`）

| 项 | 内容 |
| :--- | :--- |
| 复现的参考章节 | spec §9（模式与四个种群参数） |
| 类型 | 分片规划器：种群序列搜索 + `CNavigator` 流场 + `CSimSession` |
| 危险开关 | `m_AvoidFreeze = true`、`m_AvoidDeath = true`、`m_AvoidTeles = false`、`m_AvoidUnfreeze = false`（硬编码，1936-1941） |
| 读取的参数 | `bc_avoid_pilot_mode`、`bc_avoid_pilot_population`、`bc_avoid_pilot_depth`、`bc_avoid_pilot_top_k`、`bc_avoid_pilot_sequence`、`bc_avoid_player_prediction`、`bc_avoid_draw_path` |
| 跨 tick 状态 | `m_Nav`、`m_pSnapshot` + `m_SnapshotValid`、`m_vPopulation`、`m_vFitness`、`m_vNext`（头文件注释已更正为 breeding scratch buffer，1810-1862 的 `SeedPopulation()` 不再从它继承任何东西）、`m_IndividualCount`、`m_IndividualDepth`、`m_Individual`、`m_Rng`、`m_SessionFlow`、`m_Session`、`m_vPlan`、`m_PlanIndex`、`m_PlanTick`、`m_PlanEpoch`、`m_HeldEpoch`、`m_Target`、`m_TargetValid`；`OnReset()`（1787-1808）全部复位，`m_Rng` 回到 `0x1f123bb5` |

* **随机数**：`NextRand()`（1780-1785）是自带 LCG `m_Rng = m_Rng * 1103515245 + 12345`，返回值 `((unsigned)m_Rng >> 16) & 0x7fff`。Pilot 不用全局 `rand()`，所以在相同输入下是唯一可复现的搜索代理。
* **导航网格**（1943-1953）：地图尺寸变化、或导航器当前是按浅冻规则建的（`m_Nav.LightTile()`），就 `m_Session.Abort()` + `m_Nav.Rebuild(pCollision, false, 0)` + `m_SnapshotValid = false`。Pilot 没有浅冻开关，它的网格永远不把 freeze 当可通行。
* **导航模式**（1955-2001）：
  * `0` 自主：`m_Nav.Ready() && GoalTiles() > 0` 时 `m_Target = Pos + FlowAt(Pos) × 256.0f`；
  * `1` 跟随准星：`m_Target = m_Controls.m_aMousePos[g_Config.m_ClDummy]`；
  * `2` 跟随玩家：取最近的另一名 tee 位置。
  目标有效时一并写进 `Out.m_AimTarget`（所以 `bc_avoid_draw_aimbot` 也画 Pilot 的目标点）。
* **种群循环（2003-2081）**——这一段的规则是“快照可以换代，种群不轻易重建”：
  * `ShapeChanged` 为真当且仅当：还没有种群（`m_IndividualCount <= 0` / `m_IndividualDepth <= 0`）、`bc_avoid_pilot_population` / `bc_avoid_pilot_depth` 与当前种群形状不一致、或缓冲区尺寸不匹配（2005-2008）。
  * 只有在 `!m_SnapshotValid || ShapeChanged` 时才做一次 `CopyWorldClean(pWorld)`（2011-2016）。
  * **取完新快照之后**：`ShapeChanged`（或种群为空）→ 调 `SeedPopulation()` 建全新的初始种群；**否则保留同一批后代**，只把 `m_vFitness` 重置为 `-1e30`、`m_Individual = 0`，让**同一批个体**在新快照上重新评估（2017-2027）。这条是 `bc_avoid_pilot_top_k` 与交叉真正起作用的关键：如果每个新快照都重新播种，`Breed()` 产出的后代会被丢掉，搜索只是在反复评估父母。
  * 逐个体 `Begin(..., GenomeDepth, GenomeDepth, ...)`（每个个体的仿真深度与采纳计划长度都是 `bc_avoid_pilot_depth`）→ `Step(StepsLeft)`；结算 `Fitness = Survival × 100 + FlowScore × 4 − distance(m_EndPos, m_Target)（目标有效时） − DistanceAt(m_EndPos)（网格就绪时）`，`Survival` 把 9999 换成 `GenomeDepth`（2039-2071）。
  * 整代评估完 → `Breed()`、`m_PlanEpoch++`、`m_SnapshotValid = false`（2073-2080），下一 tick 取一张新快照并按上面的规则继续（形状没变就还是这批后代）。
  * 播种（`SeedPopulation`，1810-1862）：`Count = clamp(population,1,8192)`、`Depth = clamp(depth,1,50)`；**只在种群为空或形状变化时调用**（1815-1818 的注释写明了原因），会完整重建 `Count × Depth` 个输入。`i % 4 == 0` 跟流场、`i % 4 == 1` 朝目标（x 阈值 ±8，y < −8 时跳）、其余随机；只有在 `i % 4 >= 2 || t > 0` 时突变，概率 1/4 方向、1/9 跳跃、1/17 钩索。**没有**从 `m_vNext` 继承精英的步骤。
  * 繁殖（`Breed`，1864-1911）：`TopK = clamp(m_PilotTopK,1,min(100,Count))`；按 fitness 稳定降序；前 TopK 精英原样保留，其余做**均匀交叉**（每个基因随机取父 A 或父 B）+ 1/7 方向、1/23 跳跃、1/29 钩索突变；然后 `m_vPopulation.swap(m_vNext)`、fitness 重置、`m_Individual = 0`。精英主义在种群内部延续（TopK 被抄进新种群），但没有任何“跨快照继承”。
* **计划采纳与执行**（2083-2134）：
  * `NewerGeneration = m_PlanEpoch != m_HeldEpoch`；`WantPlan = 计划为空 || (NewerGeneration && (m_PlanTick >= bc_avoid_pilot_sequence || m_PlanIndex + 1 >= (int)m_vPlan.size()))`。`m_PlanIndex` 会饱和在最后一步，所以“走完”必须用“下一步就越界”来判（2089-2092 的注释）。
  * 采纳时要求种群与 fitness 的尺寸自洽，取**当前种群**里 fitness 最大的个体（未评估的个体是 `-1e30`，不会中选）作为新计划，并把 `m_HeldEpoch = m_PlanEpoch`。
  * 执行时用 `PLAN_GUARD_TICKS = 6` 推演这一步，**并且把玩家自己的输入也在同一窗口推演一次**（2115-2116）：`PlanUsable = PlanSafety == 9999 || PlanSafety > PlayerSafety`。计划与玩家输入的比较因此是对称的。
  * 可用则 `m_Active = 1`、`m_SurvivalTicks = (9999 ? 6 : PlanSafety)`、reason `"pilot plan"`、`m_PlanIndex++`（未到末尾时）/ `m_PlanTick++`；否则丢弃计划（清空 + 计数与 `m_HeldEpoch` 归位）。
  * 两次世代之间当前计划继续向前走、走到末尾就保持最后一个输入，所以同一个动作不会被重启。
* **保底**（2136-2156）：没有接管时推玩家原输入作基线，再按 `{0, -1, 1}` 找严格更优的方向，接管则 reason `"survival fallback while evolving"`，否则 `"player input safe"`。
* **路径可视化**（2158-2171）：与 Fentbot 相同的“沿流场走 40 步、每步 24 px”，仅在 `bc_avoid_draw_path` 且已接管时填充。

---

## 6. 参数全表

### 6.1 全表（50 个，顺序与 `config_variables_bestclient.h:616-670` 一致）

“读取方”一列是引擎里的真实读取点；“可观察影响”只描述代码支持的效果。

| # | 脚本名 | C++ 成员 | 默认 | min | max | 读取方 | 可观察影响 |
| ---: | :--- | :--- | ---: | ---: | ---: | :--- | :--- |
| 1 | `bc_avoid_enabled` | `m_BcAvoidEnabled` | 0 | 0 | 1 | `CAvoid`（`IsEnabled`/`SetEnabled`/`ApplyInput`） | 关→状态 OFF、完全不介入、不额外发包（除松手交接那一 tick）；开→每 tick 决策一次，接管时由钩子索要数据包 |
| 2 | `bc_avoid_agent` | `m_BcAvoidAgent` | 0 | 0 | 4 | `CAvoid::Agent`/`SetAgent` | 切换代理（0…4）；HUD 标题、菜单页签、读取的参数集随之改变；`SetAgent` 重置新旧两个代理 |
| 3 | `bc_avoid_afk_protection` | `m_BcAvoidAfkProtection` | 0 | 0 | 1 | `CAvoid::CheckAfkProtection` | 打开后，连续无操作到 `afk_time` 秒时自动关闭总开关、状态变 AFK 并打印一行（当 tick 生效） |
| 4 | `bc_avoid_afk_time` | `m_BcAvoidAfkTime` | 5 | 5 | 300 | `CAvoid::CheckAfkProtection` | 触发自动关闭所需的空闲秒数（空闲按 ApplyInput 调用计数，≈50 次/秒） |
| 5 | `bc_avoid_player_prediction` | `m_BcAvoidPlayerPrediction` | 1 | 0 | 1 | 五个代理（`SSimFlags.m_PredictPlayers`） | 关→克隆世界里其他 tee 的 core 条目被清空，推演不再被它们挡住；开→考虑其他玩家 |
| 6 | `bc_avoid_draw_path` | `m_BcAvoidDrawPath` | 1 | 0 | 1 | `CAvoid` 覆盖层 + 五个代理 | 世界坐标里画蓝色预测折线；关掉时代理不再重算路径（省掉一次推演） |
| 7 | `bc_avoid_draw_track_point` | `m_BcAvoidDrawTrackPoint` | 0 | 0 | 1 | 仅 `CAvoid::RenderWorldOverlay` | 画出到 Blatant 锁定瞄准点的连线 + 十字/叉（数据来自 `bc_avoid_track_point` 打开时的 Blatant） |
| 8 | `bc_avoid_draw_aimbot` | `m_BcAvoidDrawAimbot` | 0 | 0 | 1 | 仅 `CAvoid::RenderWorldOverlay` | 画出橙色瞄准目标（Blatant 真正采纳的那次瞄准，或 Pilot 的模式目标） |
| 9 | `bc_avoid_legit_direction_weight` | `m_BcAvoidLegitDirectionWeight` | 170 | 1 | 1000 | Legit（`Heuristic`） | UCT 启发式里的方向一致性权重，越大越倾向保持玩家方向 |
| 10 | `bc_avoid_legit_lifespan_weight` | `m_BcAvoidLegitLifespanWeight` | 160 | 1 | 1000 | Legit（`Heuristic`） | 存活 tick 的奖励权重，越大越优先活得久 |
| 11 | `bc_avoid_legit_hook_weight` | `m_BcAvoidLegitHookWeight` | 260 | 1 | 1000 | Legit（`Heuristic`） | 钩索状态一致性权重，越大越少改钩索 |
| 12 | `bc_avoid_legit_exploration` | `m_BcAvoidLegitExploration` | 4 | 1 | 1000 | Legit（UCT 探索项） | UCT 探索常数：越大越常试没走过的分支 |
| 13 | `bc_avoid_legit_iterations` | `m_BcAvoidLegitIterations` | 100 | 1 | 1000 | Legit（迭代上限） | 每次决策的 MCTS 迭代数；直接决定耗时，受 8 ms 护栏压制（§9.3） |
| 14 | `bc_avoid_legit_check_ticks` | `m_BcAvoidLegitCheckTicks` | 6 | 1 | 50 | Legit（单次推演深度） | 每个模拟动作必须活过的 tick 数；也决定 HUD 的 Safe 上限 |
| 15 | `bc_avoid_legit_direction` | `m_BcAvoidLegitDirection` | 1 | 0 | 1 | Legit（候选空间/接管） | 关→Legit 不改方向 |
| 16 | `bc_avoid_legit_hook` | `m_BcAvoidLegitHook` | 1 | 0 | 1 | Legit（候选空间/接管） | 关→Legit 不改钩索 |
| 17 | `bc_avoid_legit_teles` | `m_BcAvoidLegitTeles` | 0 | 0 | 1 | Legit（`m_AvoidTeles`） | 打开→把传送瓦片算作失败 |
| 18 | `bc_avoid_legit_death` | `m_BcAvoidLegitDeath` | 0 | 0 | 1 | Legit（`m_AvoidDeath`） | 打开→把死亡瓦片算作失败 |
| 19 | `bc_avoid_legit_unfreeze` | `m_BcAvoidLegitUnfreeze` | 0 | 0 | 1 | Legit（`m_AvoidUnfreeze`） | 打开→解冻瓦片在前 N tick 内算作失败 |
| 20 | `bc_avoid_legit_unfreeze_ticks` | `m_BcAvoidLegitUnfreezeTicks` | 5 | 1 | 30 | Legit（`m_UnfreezeTicks`） | 上面那条的前瞻窗口 |
| 21 | `bc_avoid_blatant_check_ticks` | `m_BcAvoidBlatantCheckTicks` | 26 | 1 | 50 | Blatant（候选推演深度） | 每个候选动作必须活过的 tick 数；也是 HUD Safe 的上限，还是 safe aim tracking 两侧比较用的窗口 |
| 22 | `bc_avoid_kick_in_ticks` | `m_BcAvoidKickInTicks` | 26 | 1 | 50 | Blatant（迟滞窗口） | 玩家原输入能活够这么多 tick 就完全不介入；越大越“不抢手” |
| 23 | `bc_avoid_blatant_direction` | `m_BcAvoidBlatantDirection` | 1 | 0 | 1 | Blatant（候选空间） | 关→候选只剩“保持玩家方向” |
| 24 | `bc_avoid_blatant_hook` | `m_BcAvoidBlatantHook` | 1 | 0 | 1 | Blatant（候选空间） | 关→候选只剩“保持玩家钩索状态” |
| 25 | `bc_avoid_blatant_teles` | `m_BcAvoidBlatantTeles` | 0 | 0 | 1 | Blatant（`m_AvoidTeles`） | 打开→传送瓦片算作失败 |
| 26 | `bc_avoid_blatant_death` | `m_BcAvoidBlatantDeath` | 0 | 0 | 1 | Blatant（`m_AvoidDeath`） | 打开→死亡瓦片算作失败 |
| 27 | `bc_avoid_blatant_unfreeze` | `m_BcAvoidBlatantUnfreeze` | 0 | 0 | 1 | Blatant（`m_AvoidUnfreeze`） | 打开→解冻瓦片算作失败 |
| 28 | `bc_avoid_blatant_unfreeze_ticks` | `m_BcAvoidBlatantUnfreezeTicks` | 26 | 0 | 30 | Blatant（`m_UnfreezeTicks`） | 解冻判定的前瞻窗口（可以是 0，等于关闭时间条件） |
| 29 | `bc_avoid_nsif` | `m_BcAvoidNsif` | 1 | 0 | 1 | Blatant（NSIF 分支） | 打开→没有全安全方案时消费缓存的安全输入（状态变 NSIF，计数器 +1） |
| 30 | `bc_avoid_track_point` | `m_BcAvoidTrackPoint` | 0 | 0 | 1 | Blatant（track point） | 打开→记住最后一次可钩到瓦片的准星方向并据此瞄准 |
| 31 | `bc_avoid_safe_aim_tracking` | `m_BcAvoidSafeAimTracking` | 0 | 0 | 1 | Blatant（瞄准过滤/探针） | 打开→锁定瞄准必须与玩家输入在**同一个 check 窗口**上比较，且活得更久才采用 |
| 32 | `bc_avoid_auto_drag` | `m_BcAvoidAutoDrag` | 0 | 0 | 1 | Blatant（瞄准候选） | 打开（且 `player_prediction` 打开）→把最近的其他玩家当作瞄准候选 |
| 33 | `bc_avoid_aimbot` | `m_BcAvoidAimbot` | 0 | 0 | 1 | Blatant（瞄准层开关） | 关→整个瞄准层（track point/auto drag/扇形扫描/auto aim/aim assist）都不产生候选；菜单会用琥珀色提示这一点 |
| 34 | `bc_avoid_aimbot_fov` | `m_BcAvoidAimbotFov` | 90 | 10 | 360 | Blatant（扇形扫描） | 以玩家准星为中心的扫描扇角（度） |
| 35 | `bc_avoid_aimbot_segments` | `m_BcAvoidAimbotSegments` | 5 | 1 | 64 | Blatant（扇形扫描） | 扇角内的采样条数；每条可钩方向额外一次探针（§9.1） |
| 36 | `bc_avoid_auto_aim` | `m_BcAvoidAutoAim` | 0 | 0 | 1 | Blatant（瞄准选择） | 打开→把“探针存活最久”的扇区加入待评估瞄准 |
| 37 | `bc_avoid_aim_assist` | `m_BcAvoidAimAssist` | 1 | 0 | 1 | Blatant（瞄准选择） | 打开→把“探针全安全且离准星最近”的扇区加入待评估瞄准 |
| 38 | `bc_avoid_fent_quality` | `m_BcAvoidFentQuality` | 0 | 0 | 2 | `ResolveFentPreset` | 高级关时决定 88/160/1000 个体、88/160/300 代、8 tick、10000 horizon |
| 39 | `bc_avoid_fent_advanced` | `m_BcAvoidFentAdvanced` | 0 | 0 | 1 | `ResolveFentPreset` | 打开→档位失效，改用下面四个自定义值（菜单同时切换显示） |
| 40 | `bc_avoid_fent_ticks` | `m_BcAvoidFentTicks` | 1000 | 1000 | 10000 | Fentbot（`m_FentHorizon`） | 每个候选基因的推演深度（高级模式生效） |
| 41 | `bc_avoid_fent_tweaker_actions` | `m_BcAvoidFentTweakerActions` | 50 | 50 | 5000 | Fentbot（`m_FentActions`） | 每代候选输入序列数量（高级模式生效） |
| 42 | `bc_avoid_fent_tweaker_ticks` | `m_BcAvoidFentTweakerTicks` | 1 | 1 | 30 | Fentbot（`m_FentHoldTicks`） | 一次优化周期覆盖多少个输入 tick（高级模式生效） |
| 43 | `bc_avoid_fent_tweaker_dosage` | `m_BcAvoidFentTweakerDosage` | 1 | 1 | 500 | Fentbot（`m_FentDosage`） | 每轮搜索的遗传代数（高级模式生效） |
| 44 | `bc_avoid_fent_light_tile` | `m_BcAvoidFentLightTile` | 0 | 0 | 1 | `CNavigator` + `SSimFlags.m_AllowLightFreeze` | 打开→unfreeze 半径内的 freeze 瓦片变成可通行（NAV_LIGHT），模拟器以“仍在移动”为条件接受它们（死亡瓦片永不放行） |
| 45 | `bc_avoid_fent_light_tile_radius` | `m_BcAvoidFentLightTileRadius` | 1 | 0 | 20 | `CNavigator`（BFS 步数） | 浅冻可通行的半径；改动会触发导航器重建；0 等于关闭该规则 |
| 46 | `bc_avoid_pilot_mode` | `m_BcAvoidPilotMode` | 0 | 0 | 2 | Pilot（目标选择） | 0 自主（流向 finish/unfreeze）、1 跟随准星、2 跟随最近的玩家 |
| 47 | `bc_avoid_pilot_population` | `m_BcAvoidPilotPopulation` | 2048 | 128 | 8192 | Pilot（`SeedPopulation`） | 每代个体数；乘上深度就是每代的仿真 tick 总量；改动会重建种群形状 |
| 48 | `bc_avoid_pilot_depth` | `m_BcAvoidPilotDepth` | 17 | 5 | 50 | Pilot（计划长度 + 推演深度） | 每个个体被仿真的 tick 数，也是采纳计划的最大长度；改动会重建种群形状 |
| 49 | `bc_avoid_pilot_top_k` | `m_BcAvoidPilotTopK` | 10 | 1 | 100 | Pilot（`Breed`） | 直接进入下一代的精英数，也是交叉时的父本池 |
| 50 | `bc_avoid_pilot_sequence` | `m_BcAvoidPilotSequence` | 5 | 1 | 20 | Pilot（计划刷新） | 一个有新世代可用时，计划最多被执行多少 tick 后被替换 |

### 6.2 表的可信度

* 表里的 name/default/min/max 与头文件逐字一致，并且被自检第 2 步用一个硬编码的 50 元组契约钉死（`avoid_selfcheck.sh:28-92`）。
* 自检第 3 步还会断言：每个参数都在 `src/**/*.cpp|h`（头文件本身除外）里被读到（名字或 `m_BcAvoidXxx` 成员出现），并且菜单里必须有对应控件；只有 `bc_avoid_enabled` / `bc_avoid_agent` 例外，它们分别由 `Avoid.SetEnabled(` / `Avoid.SetAgent(` 提供入口（`avoid_selfcheck.sh:111-146`）。
* 运行时代码还会再夹一次范围（`CAvoid::ReadSettings`，`avoid.cpp:215-265`），与 CVar 的 min/max 完全一致。即使有人用脚本把配置写成越界值，引擎侧也只会用夹紧后的值；两个规划器在 `SeedGeneration` / `SeedPopulation` 里还会再夹一次（种群 1–5000 / 1–8192，长度 1–30 / 1–50）。

### 6.3 与代理的对应（与菜单 defaults 表同一划分）

| 代理 | 专属参数 | defaults 表条目 |
| :--- | :--- | ---: |
| Basic | 无（只用 `player_prediction` 与 `draw_path`） | 1（`bc_avoid_player_prediction`） |
| Legit | 12 个 `bc_avoid_legit_*` | 12 |
| Blatant | 17 个（`blatant_*`、`kick_in_ticks`、`nsif`、`track_point`、`safe_aim_tracking`、`auto_drag`、`aimbot*`、`auto_aim`、`aim_assist`） | 17 |
| Fentbot | 8 个 `bc_avoid_fent_*` | 8 |
| Pilot | 5 个 `bc_avoid_pilot_*` | 5 |
| 共用（“All bots” 框 + 状态栏） | `bc_avoid_enabled`、`bc_avoid_agent`、`bc_avoid_afk_*`、`bc_avoid_player_prediction`、`bc_avoid_draw_*` | 7 |

### 6.4 与参考的实现差异（有意为之，详见 §12）

1. 参数名换了前缀（`krx_*` / `cl_avoid_*` → `bc_avoid_*`），数值不变；参考里成对的参数（Legit/Blatant 各一套）在本实现里也是分开的两个 CVar。
2. 危险判定比 spec §4.1 更严格（死亡先判且两层都查，冻结含 `m_IsInFreeze` / `m_LiveFrozen`，浅冻豁免带速度门槛，传送覆盖全部传送类型），见 §12.1。
3. Legit 多了一个 spec 没有的 8 ms 墙钟护栏；Fentbot/Pilot 是时间切片规划器而不是同步算完，见 §12.4/§12.5。
4. Fentbot 的计划采用“暂存 + 走到末尾整体换挡”，Pilot 的计划按 `sequence` 节奏换挡并把玩家输入纳入同窗口比较；spec 没有描述这些记账字段，但它们只影响“什么时候换计划/是否采用”，不改变候选与评分。

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

---

## 7. 界面规格

入口：主菜单 → TAS& → 第二个子页签 **Avoid**（`menus_tas.cpp:595-621`，`g_Config.m_BcTasTab == 1` → `CMenus::RenderSettingsAvoid`，实现从 `menus_avoid.cpp:292` 开始）。

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

因此：**面板里已经没有给说明文字用的固定高度槽位**（旧的 `HSplitTop(46/56/60)` 全部删除），页面里也不存在直接调用 `AvoidHint` 的地方 —— 每一处尾注都是 `AvoidHintBottom`（488、508、609、612、631、657、660、678、692、722、751、774、790、809、827、858、877），唯一的例外是 Blatant 的琥珀色警告：它自己量高、自己设 `m_MaxWidth`（`menus_avoid.cpp:725-734`：`AvoidHintHeight` → `HSplitTop(...)` 预留 `AimHint` → `AimProps.m_MaxWidth = AimHint.w` → `DoLabel`）。两个语言文件的文案长度不同时面板高度会跟着变，这是有意的。自检第 5 步把这条契约钉死了（§11）。

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

* 状态栏：`MainView.HSplitTop(52.0f, &StatusBar, ...)`、间隔 8、左右栏 `VSplitMid(..., 14.0f)`（302-305）。
* **状态栏的三段宽度按比例夹紧**（312-321）：以 `Inner.w` 为 `BarWidth`，状态徽章 `min(104, BarWidth*0.13)`、代理徽章 `min(140, BarWidth*0.18)`、右侧块 `min(210, BarWidth*0.28)`。原因是 `VSplitLeft`/`VSplitRight` **不做夹紧**：在 4:3 窗口或调高 `cl_ui_scale` 时，原来的固定宽度会把中间的文字区压成 0，标签直接画到按钮上（312-314 的注释写明了这一点）。
* 状态徽章显示 `OFF` / `WATCH` / `ASSIST` / `NSIF` / `AFK`（`CAvoid::StateName`，走本地化），颜色由 `AvoidStateColor` 决定（NSIF 红、ASSIST 橙、WATCH 绿、AFK 灰）。总开关关闭时一律显示 `OFF`（323-328）。
* 代理徽章显示 `AVOID: <代理名>`，颜色每个代理一套（`AvoidAgentColor`）（330-334）。
* 两行信息（336-338）：第一行 `Plan: <reason>`；第二行**永远是纯 tick 计数** `safe %d | cost %.2f ms | took over %d/%d`（345-349），选中的是 Fentbot 或 Pilot 时再追加 `grid: ready|building`（350-356）。这里没有 9999 分支 —— 没有任何代理会把 `SIMULATION_SAFE_CONSTANT` 报成 `m_SurvivalTicks`（345-346 的注释写明了这一点），所以那句话删掉了，对应的本地化词条也一起删了（§8.3）。
* 左栏三个盒子**没有固定高度**（389-426）：`BOX_GAP = 6.0f`；首选高度由**实测文案高度**推导 —— `AgentCopyHeight = AvoidHintHeight(..., (LeftColumn.w - 16.0f) * 0.5f)`（392-398，按半栏宽度量，是保守上界），`GeneralCopyHeight = AvoidHintHeight(..., LeftColumn.w - 16.0f)`（399-401），`aPreferred = {max(120, 120 + AgentCopyHeight), max(126, 110 + GeneralCopyHeight), 126}`（402-405）；最小高度 `aMinimum = {106, 106, 106}`（406）。可用高度不足时三个盒子按同一个比例因子在最小与首选之间线性收缩（410-420），Visuals 盒再额外夹到剩余高度（426）——所以窗口变矮时是先裁说明文字，而不是把复选框挤出屏幕。
* 代理说明文字不再有固定 56 高的槽位：由 `AvoidHintBottom` 在盒子底部按实测高度预留（488）；“All bots” 的尾注同样（508-509）；“Visuals” 盒只有四个复选框、没有尾注（512-535）。
* 代理选择分两行（Basic/Legit/Blatant 一行，Fentbot/Pilot 一行），点击调用 `Avoid.SetAgent()`（443-462）。

### 7.3 StatusBar 右侧

* `Enable` / `Disable` 按钮 → `Avoid.SetEnabled()`（等价于 `bc_avoid_enabled` 取反）（368-370）。
* `Reset counters` → `Avoid.ResetCounters()`（只清零 decisions / overrides / nsif 三个计数器并打印一行，不动任何搜索状态）（371-373）。
* 绑定提示固定为字符串 `bind X toggle bc_avoid_enabled 1 0`（不翻译，因为它是命令本身）（375-377）。

### 7.4 页签、右栏头部与每个页签暴露的参数

**右栏头部是统一的**（`menus_avoid.cpp:547-595`）：无论选中的是哪个代理，都先切出一条 22 高的 `NavBar` 并在右侧留出 92 宽的 `Defaults` 按钮（552-558）；`PanelCount > 1` 时 `NavBar` 画页签，否则画一行标题 —— 单页签画该页签名，**Basic（0 页签）画代理名**（560-578）。也就是说 Basic 也有可达的 “Defaults” 按钮（它重置 `bc_avoid_player_prediction` + 共用表），`g_aBasicDefaultParams` 不再是死代码。

页签选择按代理记忆（`static int s_aPanelByAgent[CAvoid::NUM_AGENTS]`，`menus_avoid.cpp:542`）。

| 代理 | 页签数 | 页签 | 暴露的参数 |
| :--- | ---: | :--- | :--- |
| Basic | 0 | （无页签，标题是代理名；内容只有两段 `AvoidHintBottom` 说明，`605-615`） | 无（只重置共用表 + Basic 表） |
| Legit | 3 | Settings（`618`） | `bc_avoid_legit_direction`、`bc_avoid_legit_hook`、`bc_avoid_legit_check_ticks` |
| | | Priority（`637`） | `bc_avoid_legit_iterations`（Quality）、`bc_avoid_legit_exploration`（Randomness）、`bc_avoid_legit_direction_weight`、`bc_avoid_legit_hook_weight`、`bc_avoid_legit_lifespan_weight`；另有一条固定提示说明 8 ms 护栏（660-661） |
| | | Tiles（`666`） | `bc_avoid_legit_death`、`bc_avoid_legit_teles`、`bc_avoid_legit_unfreeze`、`bc_avoid_legit_unfreeze_ticks` |
| Blatant | 4 | Avoid（`684`） | `bc_avoid_nsif` |
| | | Settings（`698`） | `bc_avoid_blatant_direction`、`bc_avoid_blatant_hook`、`bc_avoid_blatant_check_ticks`、`bc_avoid_kick_in_ticks`、`bc_avoid_track_point`、`bc_avoid_safe_aim_tracking`、`bc_avoid_auto_drag`；当 `bc_avoid_aimbot` 关闭时用琥珀色（`0.95,0.72,0.30`）提示“这些开关需要先到 Aimbot 页签打开内部瞄准”（725-734） |
| | | Tiles（`739`） | `bc_avoid_blatant_death`、`bc_avoid_blatant_teles`、`bc_avoid_blatant_unfreeze`、`bc_avoid_blatant_unfreeze_ticks` |
| | | Aimbot（`757`） | `bc_avoid_aimbot`、`bc_avoid_aimbot_segments`、`bc_avoid_aimbot_fov`、`bc_avoid_auto_aim`、`bc_avoid_aim_assist` |
| Fentbot | 1 | Calculation（`780`） | `bc_avoid_fent_light_tile`、`bc_avoid_fent_light_tile_radius`、`bc_avoid_fent_advanced`，以及互斥显示的两组：高级关 → `bc_avoid_fent_quality`（Low/Mid/Max 分段控件，799-810）；高级开 → `bc_avoid_fent_ticks`、`bc_avoid_fent_tweaker_actions`、`bc_avoid_fent_tweaker_ticks`、`bc_avoid_fent_tweaker_dosage`（812-828） |
| Pilot | 2 | Main（`834`） | `bc_avoid_pilot_mode`（Autonomous / Follow cursor / Follow player 分段控件 + 自动量高的模式说明，841-858） |
| | | Settings（`863`） | `bc_avoid_pilot_population`、`bc_avoid_pilot_depth`、`bc_avoid_pilot_top_k`、`bc_avoid_pilot_sequence` |
| 所有代理 | — | 左栏 “All bots”（`491-510`） | `bc_avoid_player_prediction`、`bc_avoid_afk_protection`、`bc_avoid_afk_time` |
| 所有代理 | — | 左栏 “Visuals”（`512-535`） | HUD 开关（`HudLayout`）+ `bc_avoid_draw_path`、`bc_avoid_draw_track_point`、`bc_avoid_draw_aimbot` |

### 7.5 “Defaults” 按钮的行为

* 统一头部保证**每个代理**都有这个按钮（Basic 也算了），可见 `menus_avoid.cpp:580-594`。
* 点击后遍历两张表：该代理的 defaults 表 + `g_aGeneralDefaultParams`，对每个名字调用 `IConfigManager::Reset(name)`（587-590），也就是**从 CVar 定义处恢复默认值**，代码里没有第二份默认值副本可以漂移。
* 表总条目 50 条，正好覆盖全部参数：general 7、Basic 1、Legit 12、Blatant 17、Fentbot 8、Pilot 5（自检第 3 步断言 6 张表、无重复、名字都已声明）。
* 反馈：按钮文字在 2 秒内变成“已恢复”（`Client()->LocalTime()` 比较），并在控制台打印 `Avoid: parameters restored to their defaults` 的本地化字符串（592-593）。

### 7.6 HUD 模块接线

| 项 | 值 | 出处 |
| :--- | :--- | :--- |
| 枚举 | `HudLayout::MODULE_AVOID` | `hud_layout.h:40` |
| 持久化 id | `"avoid"` | `hud_layout.cpp:81` |
| 显示名 | `"Avoid"`（经 `BcLocalize`） | `hud_layout.cpp:109`、`520` |
| 默认布局 | X=286, Y=52, scale=100, 位置模式=左上, **enabled=false**, background=true, color=0x66000000, alpha=100 | `hud_layout.cpp:28-53`（索引 24） |
| 持久化位置 | `BestClient/hud_layout.cfg`（配置域 `HUDLAYOUT`） | `config_domains.h:11` |
| 编辑器 | 在白名单里；取 `m_Avoid.GetHudEditorRect()`；把模块加入可视化列表；`HudLayout::SetEnabled` 写同一个状态 | `hud_layout.cpp:475-503`、`hud_editor.cpp:528-531`、`587`、`950` |
| 菜单入口 | 左栏 “Visuals” → “Status HUD” 复选框，`HudLayout::SetEnabled(MODULE_AVOID, !HudEnabled)` | `menus_avoid.cpp:527-530` |
| 可见性判定 | `CAvoid::IsHudVisible()` = `HudLayout::IsEnabled(MODULE_AVOID)` | `avoid.cpp:486-489` |

因此“HUD 开不开”只有一份状态：菜单复选框与 HUD 编辑器改的是同一个 `HudLayout` 值；`bc_avoid_enabled` 只决定状态栏里显示什么内容（关闭时状态徽章为 `OFF`）。

面板几何（`avoid.cpp:535-610`）：基准尺寸 122×62（HUD 画布 500×300，常量在 `avoid.cpp:30-39`），scale 夹到 0.25–3.0，alpha 夹到 0.05–1.0；头部 9 高、每行 7.5 高、标题字号 5.5、行字号 5.0；徽章宽 34、标签列宽 40；四行固定为 `Plan:` / `Safe:` / `Cost:` / `Override:`，值分别为 reason、存活 tick（`%d tick`，**没有 9999 分支**，585-594）、`X.XX ms`、`Overrides / Decisions`。
面板背景跟随模块设置：`if(Layout.m_BackgroundEnabled) Canvas.Draw(ColorRGBA(0.06f,0.08f,0.12f,0.85f × Alpha), ...)`（550-552）——背景开关由 HUD 编辑器统一控制，底色仍用面板自己的深色常量；位置、缩放、位置模式与 alpha 同样来自 `HudLayout`。
HUD 编辑器预览走 `GetHudEditorRect()`（强制取矩形）+ `RenderPreview()`（`ForcePreview = true`，此时固定显示 Basic 代理、`WATCH` 状态、`player input safe`、`26 tick`、`0.00 ms`、`0 0`）。

### 7.7 三个世界覆盖层（`avoid.cpp:612-697`）

前提：`IsEnabled()` 且三个 `draw_*` 里至少一个打开（`avoid.cpp:475`）；渲染时把屏幕切到以相机中心/缩放构造的世界坐标，画完恢复。

| 覆盖层 | 开关 | 数据 | 画法 |
| :--- | :--- | :--- | :--- |
| 预测路径 | `bc_avoid_draw_path` | `m_vLastPath`（代理在 `bc_avoid_draw_path` 打开时填充） | 至少 2 个点时逐段画线，颜色 `(0.40,0.80,1.00,0.85)`；跳过非有限坐标（629-651） |
| Blatant 锁定瞄准点 | `bc_avoid_draw_track_point` | `m_Telemetry.m_TrackPoint`（Blatant 在 `bc_avoid_track_point` 打开且钩索射线命中时写入） | 从角色到目标一条连线 + 十字 + 叉（5 条线段），颜色 `(0.36,0.68,1.00,0.90)`（653-673） |
| 瞄准目标 | `bc_avoid_draw_aimbot` | `m_Telemetry.m_AimTarget`：Blatant **真正采纳**的那次瞄准（只在接管分支里发布），或 Pilot 的模式目标 | 连线 + 十字（3 条线段），颜色 `(0.98,0.62,0.16,0.90)`（675-692） |

两个标记都要求与角色的距离 > 1.0，避免在脚下画出一团噪声。

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
3. 文件格式：可选的 `#` 注释行；`origin` 行后面紧跟一行以 `== ` 开头的译文；上下文行是 `[BestClient]`。加载器对格式错误会打印 `log_error` 并跳过，所以自检第 4 步用同构的 Python 解析器复刻了 `CLocalizationDatabase::Load` 的规则（`avoid_selfcheck.sh:169-199`），格式错误会直接让自检失败。
4. 约定上每条词条都单独带一行 `[BestClient]` 上下文（现有两个文件就是这样组织的，各 866 条）。

### 8.3 当前规模与校验方式

* 参与契约的源文件（与自检第 4 步一致）：`avoid.cpp`、`avoid_engine.cpp`、`menus_avoid.cpp`、`menus_tas.cpp`、`hud_layout.cpp`。
* 当前唯一 key 数 **177**（自检断言 `> 120`；旧的状态栏 “safe for the whole lookahead” 词条已随该死分支一起删除）；两个语言文件各 **866** 条，缺失 **0**（写作时按自检同样的解析器逐条复核）。
* 引擎侧的 reason 文本**不是**本地化标签：`avoid_engine.cpp` 里 0 处 `BcLocalize`；代理写的是英文常量，HUD 与菜单原样显示 `m_aReason`。当前共 22 个取值：

  `player input safe`、`brake before hazard`、`steer left before hazard`、`steer right before hazard`、`steer away from hazard`、`steer to safety`、`hook to safety`、`release hook`、`hook and steer to safety`、`hook the safe aim`、`aim clear of the hazard`、`no safer plan`、`no world`、`no character`、`agent unavailable`、`NSIF: replay saved safe input`、`search budget reached`、`survival fallback while searching`、`survival fallback while evolving`、`fentbot plan`、`pilot plan`、`AFK protection disabled the bot`。

  控制台/界面上它们出现在 `Plan: <reason>`（HUD 与菜单状态栏）与 `[avoid] last plan: <reason>`（`avoid_status`）。要改这些文本就等于改行为可读性，注意别把它们塞进 `BcLocalize` 除非同时补两份语言文件。

### 8.4 新增一种语言

1. 把语言写进语言索引（`data/languages/index.txt`；TClient 的词条索引在 `data/tclient/languages/index.txt`）：文件名、母语名、ISO 3166-1 数字国家码、RFC 3066 标签，各占一行，后三者以 `== ` 开头。
2. 复制一份 `data/BestClient/languages/<name>.txt`（例如以 `russian.txt` 为模板），把全部 `[BestClient]` 词条的 `== ` 行换成新语言；不要动 key 行。
3. 缺的词条会被 `BcLocalize` 退化到 `Localize()`，再退化到英文原文——界面不会崩，但自检第 4 步会因为 key 缺失而失败，所以必须补齐。
4. 游戏内 `cl_languagefile "languages/<name>.txt"` 切换（或在设置里选语言）；切换会重放三层加载（`gameclient.cpp:1625-1647`）。
5. 跑 `./scripts/avoid_selfcheck.sh`，第 4 步通过即表示契约完整。

---

## 9. 性能与预算

### 9.1 Basic / Blatant 由参数直接限界（同步）

| 代理 | 每次决策的推演次数（上限） | 每次推演的 tick 数 | 备注 |
| :--- | :--- | :--- | :--- |
| Basic | 4（1 基线 + 3 候选） | 6（`BASIC_CHECK_TICKS`，写死） | 基线安全时 1 次就返回；候选命中 9999 立即 break |
| Blatant | 1（kick-in）+ ≤6（方向×钩索）+ ≤2（safe aim tracking 的两侧）+ ≤`aimbot_segments`（FOV 探针，每个可钩扇区一次）+ ≤24（入选瞄准 ≤4 个 × 动作 ≤6）+ ≤1（画路径） ≈ ≤98 | `kick_in_ticks` / `blatant_check_ticks`（≤50） | 瞄准层默认关闭（`bc_avoid_aimbot = 0`）时约 ≤8；入选瞄准是 `FixedAims`（track point + auto drag，≤2）+ auto aim + aim assist，并用 `dot > 0.9999` 去重 |
| Legit | ≤ `legit_iterations`（默认 100，≤1000）+ 1（画路径） | `legit_check_ticks`（≤50） | 每次迭代 1 次推演 + 建树/回传；受 8 ms 墙钟护栏限制 |

三者都在**单次 `GetAction` 里把活干完**，而且每推演一次就 `CopyWorldClean` 一份世界（Basic 最多 4 份、Blatant 最坏几十份），这是本模块 CPU 占用的主要来源。Blatant 的瞄准层默认是关闭的，把 `aimbot_segments` 调大之前先看 `Cost` 读数。

### 9.2 Blatant 没有墙钟护栏

只有 Legit 有 deadline。Blatant 的最坏成本随 `aimbot_segments × blatant_check_ticks` 线性增长（§9.1 的公式）；慢机器上把 `bc_avoid_aimbot_segments` 调小、把 `bc_avoid_blatant_check_ticks` 调小，或者关掉 `bc_avoid_aimbot`。
接管 gate 的收紧（`BestSurvival > 0 || UsedFallback`，§5.2）只影响“是否应用计划”，不改变推演次数：所有候选仍然要算完才知道谁活过第 0 tick。

### 9.3 Legit 的 8 ms 护栏

* `LEGIT_DEADLINE_MS = 8.0`，检查点是 `(Iter & 7) == 0`，即每 8 次迭代看一次表：护栏是**检查点**而不是硬中断，最坏会多跑 7 次迭代。
* 命中护栏时 `break`，用“目前为止访问次数最多的根子节点”继续，行为不断档。
* 参考默认值（100 次迭代 / 6 tick）在任何现代机器上都碰不到这个护栏（`avoid_engine.cpp:33-36` 的注释也这么写）；把 `bc_avoid_legit_iterations` 拉到 1000 且 `check_ticks` 拉到 50 时才会稳定命中，此时 HUD 的 `Cost` 会停在 8 ms 附近（见 §12.4）。菜单的 Priority 页签已经把这件事写给用户了（`menus_avoid.cpp:625-627`）。

### 9.4 Fentbot / Pilot 是分片规划器

| 常量 | 值 | 含义 |
| :--- | ---: | :--- |
| `PLANNER_STEPS_PER_TICK` | 600 | 每个渲染 tick 内，两个规划器最多推进的**仿真 tick** 总数（跨候选共享一个预算，`StepsLeft` 递减） |
| `NAV_WORK_PER_TICK` | 20000 | 每帧网格构建（分类 / 浅冻标记 / 洪水 / 梯度）最多处理的瓦片数 |
| `SEARCH_COOLDOWN_TICKS` | 10 | Fentbot 一轮结束后的休眠 tick 数 |
| `PLAN_GUARD_TICKS` | 6 | 执行计划前的闭环护栏推演深度 |

* 每 tick 的固定额外开销：护栏推演 —— Fentbot 1 次（6 tick，必要时再加 1 次玩家输入）、Pilot 2 次（计划 + 玩家输入，同窗口比较）；没有可用计划时再加一次保底搜索 —— Fentbot 12 个候选（`Guard = clamp(Length,6,10)`）、Pilot 3 个候选（6 tick）。
* Fentbot 的量级（Low 档，`actions = 88`、`horizon = 10000`、`dosage = 88`）：单代 88 × 10000 = 880,000 仿真 tick ÷ 600 ≈ 1467 个渲染 tick ≈ **29 秒**一代；一轮 88 代是小时级。候选一碰到危险就提前结束，所以实际远小于这个上界，但“开箱即用地秒出结果”不是这个模块的行为。Mid/Max 档更大（Max：1000 × 10000 = 10,000,000 tick/代）。计划是**随时暂存**的：第一个候选算完（`Fitness > -1e30`）就会有一条可执行的暂存计划，只要当前基因组走到末尾就整体换上去。
* Pilot 的量级（默认 `population = 2048`、`depth = 17`）：一代 2048 × 17 = 34,816 仿真 tick ÷ 600 ≈ 58 个渲染 tick ≈ **1.2 秒**一代。快照失效时**不会**重建种群：形状没变就把同一批后代的 fitness 清空、在新快照上重新评估（同样约 1.2 秒），只有种群为空或形状（`population` / `depth`）变化时才调 `SeedPopulation()` 重建 `Count × Depth` 个输入（`avoid_engine.cpp:2003-2028`）。计划在新世代完成且 `bc_avoid_pilot_sequence`（默认 5）tick 的节奏到达时才换。
* 两个规划器都**没有后台线程**：源码里没有 `std::async` / `std::thread` / `std::future`（spec §6.3 的并发贪心在移植时被替换为分片）。

### 9.5 HUD 与菜单的 Cost 读数

* 采样点：`ApplyInput` 里 `StartTime` 取在 `GetAction` 之前（`avoid.cpp:401`），`CostMs = (time_get()-StartTime) × 1000 / time_freq()`（414）。它**只包含这一次决策**（导航器构建 + 播种 + 切片搜索 + 护栏/保底推演），不含渲染、组包、其他组件。
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

### 10.1 Basic

* 准备：`bc_avoid_agent 0`、`bc_avoid_enabled 1`。
* 动作：在普通冻结房（或任何有连续冻结块的图）按住 D 撞向冻结块。
* 必须看到：
  * 状态徽章在贴近冻结块前从 `WATCH` 变 `ASSIST`，`Plan` 变成 `brake before hazard` 或 `steer left/right before hazard`；
  * `Safe` 显示 `6 tick`（Basic 的前瞻就是 6；所有代理的 Safe 行都是 tick 数，见 §5.1/§7.1）；
  * tee 在冻结块前沿悬停、绝不冻结；松开按键后立刻恢复玩家控制（松手那一 tick 会额外发一个数据包，见 §3.3）；
  * 在安全路段行走时 `Plan: player input safe`、`Override` 计数不增长。
* 反向验证：把 `bc_avoid_player_prediction` 关掉，在有两名玩家并排的窄通道里，Basic 会更容易给出 `no safer plan`（克隆世界里其他 tee 不再阻挡）。

### 10.2 Legit

* 准备：`bc_avoid_agent 1`、`bc_avoid_enabled 1`（默认 `iterations = 100`、`check_ticks = 6`、权重 170/260/160）。
* 动作 A：在安全路段正常起跳、飞行、钩索。
* 必须看到：`Plan: player input safe`，`Override` 几乎不增长，`Cost` 是零点几毫秒量级；操作手感不被改写（方向与钩索状态都保留）。
* 动作 B：朝冻结墙跳过去。
* 必须看到：接管时 reason 是 `steer to safety` / `hook to safety` / `release hook` / `hook and steer to safety` 之一，`Safe` 显示 `6 tick`（把 `bc_avoid_legit_check_ticks` 调大后显示对应值）；方向与钩索在最后几个可救 tick 内被改掉。
* 动作 C（护栏）：`bc_avoid_legit_iterations 1000`、`bc_avoid_legit_check_ticks 50`。
* 必须看到：`Cost` 上升到 8 ms 附近并稳定（护栏生效）；没有接管时 reason 可能变成 `search budget reached`；把两个值调回默认后 Cost 回落。

### 10.3 Blatant

* 准备：`bc_avoid_agent 2`、`bc_avoid_enabled 1`、`bc_avoid_nsif 1`（默认 `kick_in_ticks = 26`、`blatant_check_ticks = 26`）。
* 动作 A（迟滞）：保持自己的输入安全（在平地上正常走）。
* 必须看到：状态停在 `WATCH`，`Plan: player input safe`，`Safe: 26 tick`，`Override` 不增长——这就是 kick-in 迟滞。
* 动作 B（贪心 + 钩索）：从高处落进狭长冻结池。
* 必须看到：状态变 `ASSIST`，reason 出现 `hook to safety` / `release hook` / `brake before hazard` / `steer away from hazard`，`Override` 增长；开着 `bc_avoid_draw_path` 时能看到预测折线。
* 动作 C（无解情形）：找一个所有候选都在第 0 tick 就死掉的处境（例如已经贴在致死/冻结面上且没有任何方向能撑过一 tick）。
* 必须看到：reason 回到 `no safer plan`、状态仍是 `WATCH`、玩家输入保持原样——gate 是 `BestSurvival > 0`（§5.2 第 6 点），不会再无条件接管。
* 动作 D（NSIF）：同一条命里先制造一次“找到全安全方案”的接管（例如让它先躲开一次冻结），再落进无法全安全的处境。
* 必须看到：状态徽章变红 `NSIF`，`Plan: NSIF: replay saved safe input`，计数器的 `nsif` 增长；`Safe` 显示的是**重新推演出来的真实存活 tick**（不是伪造的 9999）。
  注意：每个安全序列只存一步、回退时会被 `erase` 消费，所以 NSIF 通常只持续一个 tick；`m_SavedSafeSequence` 还会在重置、地图加载和 kick-in 安全时被清空，必须在**同一条命**里先攒到一次安全序列。
* 动作 E（瞄准层）：`bc_avoid_aimbot 1`、`bc_avoid_draw_aimbot 1`、`bc_avoid_auto_aim 1`。
* 必须看到：橙色瞄准标记出现并指向一个能活过整个 check 窗口的方向；`Plan` 里出现 `hook the safe aim` / `aim clear of the hazard`；把 `bc_avoid_aimbot_segments` 从 5 调到 32 后 `Cost` 明显上升（每条可钩扇区都要一次探针）。
* 反向验证 1：在 Aimbot 页签关掉 `bc_avoid_aimbot`，回到 Settings 页签会看到琥珀色提示——此时 `bc_avoid_track_point` / `bc_avoid_safe_aim_tracking` / `bc_avoid_auto_drag` 不再产生任何候选。
* 反向验证 2：只在“瞄准层算出了候选、但最终没有被采纳”的 tick 观察 `bc_avoid_draw_aimbot`：标记不会亮，因为 `m_AimTarget` 只在接管分支里发布（§5.2 第 6 点）。

### 10.4 Fentbot

* 准备：`bc_avoid_agent 3`、`bc_avoid_enabled 1`；为了快速看到结果，建议先 `bc_avoid_fent_advanced 1`、`bc_avoid_fent_tweaker_actions 50`、`bc_avoid_fent_ticks 1000`、`bc_avoid_fent_tweaker_dosage 1`。
* 动作：进入一张有冻结段和 finish/unfreeze 目标的图，先站着不动观察。
* 必须看到：
  * 状态栏网格指示从 `grid: building` 变 `grid: ready`（导航器分片构建完成）；
  * 第一个候选算完之前，reason 是 `survival fallback while searching`（保底贪心）或 `player input safe`；
  * 第一个候选算完之后就变成 `fentbot plan`（随时暂存），`Safe: 6 tick`（护栏窗口），路径覆盖层沿流场画出一条折线；
  * 更好的基因组会在当前基因组走到末尾时整体换上（不会出现“旧尾巴接新头”的重复动作）；`avoid_status` 的第二行 `last plan` 与 HUD 一致。
* 预期时间：默认档位是分钟到小时级（§9.4），不要用默认档做“秒级”验收；要看效果就把 `dosage` 设成 1、`actions` 设成 50。
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

### 10.6 共用项

* AFK 保护：`bc_avoid_afk_protection 1`、`bc_avoid_afk_time 5`，松开所有按键 5 秒以上 → 状态变 `AFK`，总开关自动关闭，控制台打印 `Avoid: AFK protection disabled the bot`；触发的那一 tick 就已经不再接管，并且会走一次交接（§3.3）。
* 特效范围：`bc_avoid_draw_track_point` / `bc_avoid_draw_aimbot` 只在 Blatant 或 Pilot 有对应数据时才画出东西；`bc_avoid_draw_path` 对五个代理都有效。
* 代理切换：切到 Fentbot/Pilot 时新旧代理的状态都被重置（网格重新构建、种群清空），HUD 标题与菜单页签同步变化。
* 参数恢复：任何代理（**包括 Basic**）点 “Defaults” → 控制台打印恢复提示，滑条/复选框回到头文件里的默认值。
* HUD 背景：在 HUD 编辑器里关掉 Avoid 模块的 background，面板底色消失但文字仍在（§7.5）。

### 10.7 控制台命令与推荐绑定

| 命令 | 注册处 | 行为 |
| :--- | :--- | :--- |
| `avoid_toggle` | `avoid.cpp:76` | 等价于 `bc_avoid_enabled` 取反；打印 `Avoid: enabled` / `Avoid: disabled` |
| `avoid_status` | `avoid.cpp:77` | 打印一行状态（`[avoid] state … \| agent … \| enabled … \| safe N ticks \| cost X.XXX ms \| decisions N \| overrides N \| nsif N`）+ 一行 `[avoid] last plan: <reason>` |
| `avoid_reset` | `avoid.cpp:78` | 清零 decisions / overrides / nsif 三个计数器，打印 `Avoid: counters reset` |

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
| 2 | `[2/7] bc_avoid_* parameter contract (name, default, min, max)` | 用一个硬编码的 50 元组契约表与头文件里的 `MACRO_CONFIG_INT(…, bc_avoid_*, …)` 做**集合相等**比较；多一个、少一个、默认值/最小值/最大值任意一项不同都会打印差异并失败。通过时打印 `ok: N parameters, every default/min/max matches the reference` |
| 3 | `[3/7] every parameter is actually wired` | (a) 每个声明的参数要么名字、要么它的 `m_BcAvoidXxx` 成员必须出现在除头文件以外的 `src/**/*.cpp\|h` 里（“死参数”即失败）；(b) 每个参数必须能在 `menus_avoid.cpp` 里找到它的成员名，例外是 `bc_avoid_enabled` / `bc_avoid_agent`，它们必须分别出现 `Avoid.SetEnabled(` / `Avoid.SetAgent(`；(c) 恰好 6 张 defaults 表（每个代理一张 + 共用一张），每张非空、无重复、所有名字都已声明，并打印总条目数（当前 50） |
| 4 | `[4/7] localization follows the client language` | 从 `avoid.cpp`、`avoid_engine.cpp`、`menus_avoid.cpp`、`menus_tas.cpp`、`hud_layout.cpp` 收集全部唯一的 `BcLocalize("…")` 字面量并断言数量 > 120（当前 177）；用与 `CLocalizationDatabase::Load` 同构的解析器读两个语言文件（上下文行、`== ` 行格式错误都会失败），然后断言每个 key 都以 `BestClient` 为上下文存在 |
| 5 | `[5/7] no placeholder copy left in the module` + UI 排版契约 | (a) 脚本里硬编码的禁止词表（检索字面量为 `not implemented`、`Not implemented`、`尚未实现`、`Planned`、`planned`、`is missing`、`arrives with the agent`、`reserved for upcoming`）不得出现在 Avoid 相关文件的任何 `BcLocalize` 字面量里、不得出现在 `menus_avoid.cpp` 正文里、也不得作为语言文件里这些 key 的译文出现；注释不受限。(b) 排版契约（235-244）：菜单里必须仍有 `SLabelProperties Props;` 与 `Props.m_MaxWidth = Rect.w;`（否则说明文字会被压到 5 px 下限）、必须仍调用 `AvoidHintBottom(` 与 `TextBoundingBox(`（否则面板不再为换行后的文案预留高度）；`AVOID_TEXT_VALUE` 与 `SIMULATION_SAFE_CONSTANT` 必须**不**出现在菜单里（前者已删除，后者对应的分支永远不会运行） |
| 6 | `[6/7] engine contract` | 对源码做字符串断言：`SimulateCandidate` + `CopyWorldClean` + `SIMULATION_SAFE_CONSTANT = 9999`；Basic 的 `BASIC_CHECK_TICKS = 6` 与 `s_aCandidateDirs[3] = {0, -1, 1}`；Legit 的 `WEIGHT_SCALE = 0.01f`、`std::abs(DirDiff - 2.0f)`、`std::abs(HookDiff - 1.0f)`、`3.402823466e+38`、`s_aDirs[3] = {-1, 0, 1}`；Blatant 的 `Set.m_KickInTicks`、`m_SavedSafeSequence`、`s_aHooks[2] = {0, 1}`；Fentbot 的 `FENT_FLOW_WEIGHT = 1750.0f` 与档位值 88/160/1000/300；Pilot 的三个参数名；五个代理都必须有 `AvoidInput <类名>::GetAction(` 且在组件里 `new Avoid::<类名>(`；输入钩子：`avoid.h` 里有 `EInputResult ApplyInput(`，组件里有 `*pInput = m_LastOverride;` 与 `FinishInput(`，组件里**不出现** `m_Controls.m_aInputData`，`WantsEveryTickInput` 在组件与 `controls.cpp` 里都不存在，`gameclient.cpp` 里有 `m_Avoid.ApplyInput(&Input) != CAvoid::INPUT_IDLE` 与 TAS 录制那行（294-302） |
| 6b | `[6b/7] HUD module wiring and render order` | `hud_layout.h` 有 `MODULE_AVOID,`；`hud_layout.cpp` 有 `"avoid",`、`"Avoid",`、`case MODULE_AVOID:`；`hud_editor.cpp` 有 `MODULE_AVOID` 与 `m_Avoid.RenderPreview()`；渲染组件顺序必须是 `m_MapLayersForeground < m_Hud < m_Tas < m_Avoid`（打印 `ok: foreground < Hud < Tas < Avoid`） |
| 7 | `[7/7] testrunner test suite` | `ninja -C build testrunner` 后运行 `./build/testrunner`，即整个引擎/游戏 gtest 套件 |

关于第 7 步的一句实话：`src/test/` 下当前**没有** Avoid 专属用例（源码里搜不到 `CAvoidSimulatorTest`）。仓库根目录残留的一批 `CAvoidSimulatorTest.*.tmp` 空目录是历史遗留的 gtest 临时目录，不是回归的一部分，也不要据此以为存在单元测试。Avoid 的行为回归靠第 2–6b 步的契约断言 + §10 的实机验收。

改动前的自查顺序建议：`avoid_selfcheck.sh` → 按 §10 手动过一遍受影响的代理 → 若改了参数集，确认第 2 步的契约表也同步更新（它和头文件必须同时改，否则脚本会报 `parameter set drift`）。

---

## 12. 已知限制与有意偏离

以下每条都是当前代码的事实，不是待办事项。前六条是**有意偏离 spec 字面**的设计，改动前先确认你不是在“修好”一个刻意为之的行为。

1. **`TickHitHazard` 比 spec §4.1 更严格、顺序也不同，这是故意的。** 死亡排在冻结之前，并且在角色位置的 5 个点（中心 + `GetProximityRadius()/3` 的四个角）上同时查 `GetCollisionAt` 与 `GetFrontCollisionAt`（`avoid_engine.cpp:108-121`）——同一格一层是 freeze、另一层是 death 时，浅冻豁免永远盖不住它；角点几何与引擎自己的死亡探测一致（`character.cpp:1207-1214`，引擎也是两层都查）。冻结判定除 spec 的三个条件外还包含 `m_IsInFreeze` 与 `m_LiveFrozen`（130-131）——游戏自己就是用 `m_IsInFreeze` 表示“站在冻结/死亡瓦片上”（`character.cpp:1185-1198`），只用 spec 的三条会漏判。浅冻豁免额外要求角色**仍在移动**（`length(m_Vel) > LIGHT_FREEZE_MIN_SPEED = 1.5f`，133-136），否则停在浅冻里也会被判成安全、搜索会把“停在冻结里”当成满分手牌；导航器也拒绝把死亡瓦片提升成 `NAV_LIGHT`（623-627）。传送判定覆盖 `IsTeleport` / `IsEvilTeleport` / `IsCheckTeleport` / `IsCheckEvilTeleport` / `IsTeleCheckpoint`（89-94、141-146），而 spec 只点名了 checkpoint getter。全部是“宁可多判一次危险”。
2. **克隆用 `CopyWorldClean` 而不是 `CopyWorld`，这是故意的。** `CopyWorld` 会把副本挂进实时预测世界的父子链（`gameworld.cpp:718-726`），而模拟器一个 tick 里会有多个克隆（Fentbot 的快照 + 会话世界），互相标记失效会破坏客户端自己的 `m_PredictedWorld`。详见 §4.1。
3. **`CSimSession` 持有指向 `CNavigator` 内部缓冲区的指针（流场 + 网格），这是设计约束。** `m_SessionFlow.m_pDir` 指向 `Flow()` 的 `data()`，`SSimFlags::m_pNav` 指向导航器本体；所有重建路径（地图尺寸变化、浅冻设置变化、计划过期重来）都必须先 `m_Session.Abort()`：Fentbot `1552-1557` / `1731-1742`，Pilot `1946-1951`。改这一带代码时先读 §4.4 的规则。
4. **Legit 有一个 spec 没有的 8 ms 每 tick 墙钟护栏（`LEGIT_DEADLINE_MS`）。** 参考实现在 MCTS 预算被调得离谱时会掉帧；本实现选择让出并返回当前最优（`avoid_engine.cpp:33-36`、`1260-1266`）。代价是 `bc_avoid_legit_iterations` 调到很大时质量被护栏封顶：`1000` 次迭代 + `check_ticks 50` 会稳定命中，`Cost` 停在 8 ms 附近；只有 `DeadlineHit` 且计划与玩家输入一致时 reason 才会显示 `search budget reached`。
5. **Fentbot / Pilot 是实时预算内的分片规划器，不是后台异步求解器，模块也没有 Tile Editor / 隧道编辑 UI。** 搜索切片在 `GetAction` 里执行（`PLANNER_STEPS_PER_TICK = 600` 个仿真 tick/帧），源码里没有线程、`std::async` 或 `std::future`；参考档位下 Fentbot 的一代是几十秒级、一轮是小时级（§9.4），一轮结束还会休眠 `SEARCH_COOLDOWN_TICKS = 10` 个 tick；Pilot 的一代约 1.2 秒，但快照每次过期都要把整代重新评估一遍（不重建种群）。寻路只消费当前地图 `CCollision` 的瓦片分类（finish、unfreeze、freeze、deep freeze、death、teleport、实心）与 CVar 开关，`CNavigator` 的网格是运行时从碰撞层构建的；仓库里没有任何“标注隧道/危险区”的数据或界面（`grep -i tunnel` 在 `components/bestclient/` 下无结果），想改变寻路行为只能改 `bc_avoid_fent_light_tile*` 与地图本身。
6. **两条参数的工作方式与 spec 的自然读法不同，但都是刻意实现。** `bc_avoid_player_prediction` 不是“不预测其他玩家”，而是把其他 tee 从克隆世界的 core 字符表里清空（`CForwardSim::SetPredictPlayers`，`193-202`；`CSimSession::Begin`，`408-415`）——它们不再挡路，也就没有计划需要绕开；`bc_avoid_fent_light_tile` 不仅让导航器把半径内的 freeze 标成 `NAV_LIGHT`（死亡瓦片除外），还通过 `SSimFlags::m_AllowLightFreeze` + `m_pNav` 让模拟器接受这些瓦片，条件是角色仍在移动（1537-1546、`TickHitHazard` 133-138）。
7. **NSIF 是“一次性”的，不是无尽重放。** 与 spec §6.4 一致：每个保存的安全序列只有一步，回退时 `erase` 掉并重新推演它来报告真实存活 tick（`avoid_engine.cpp:1138-1156`）。所以 NSIF 徽章通常只亮一个 tick，之后缓存为空、状态回到 `ASSIST` 或 `no safer plan`。验收时必须先在同一条命里攒到一次安全序列（§10.3）。
8. **Basic 永远不能用跳跃和钩索救命。** 它只改 `m_Direction`（§5.1），所以需要跳跃或钩索才能脱险的地图它救不回来。这类图用 Legit / Blatant。
9. **“推演失败”会被当成“安全”。** `SimulatePlan` / `SimulateCandidate` 在没有世界、没有角色或 `CheckTicks <= 0` 时返回 9999（`avoid_engine.cpp:370-375`），于是代理不介入；由于每个代理都会把 9999 换算成自己的窗口长度（§4.3），HUD 与状态栏此时显示的仍是一个**正常的 tick 数**，看上去和“确认安全”没有区别。这保证了异常时不会乱改输入，代价是读数本身区分不出这两种情况。定位这类情况看 `Plan` 的 reason（`no world` / `no character` / `agent unavailable`）。
10. **Blatant 没有墙钟护栏。** 它的单次决策成本随 `bc_avoid_aimbot_segments`、`bc_avoid_blatant_check_ticks`、`bc_avoid_kick_in_ticks` 线性增长（§9.1 的公式，最坏不到百次世界克隆）；收紧后的接管 gate（`BestSurvival > 0`）不减少推演次数，只减少“算完不用”的情况。慢机器上先降低这几个值。
11. **Legit 与 Fentbot 使用全局 `rand()`，不具备逐位可复现性。** Legit 用 `rand()` 选展开的子节点（`1320`），Fentbot 用 `rand()` 做播种与变异（`1449`、`1494`、`1496`）：同一场景两次运行会得到不同（但同类）的搜索过程。Basic / Blatant 不用随机数；Pilot 用自带 LCG（`m_Rng` 固定种子 `0x1f123bb5`），是唯一在相同输入下逐位可复现的搜索代理。
12. **Legit 的 8 ms 护栏是墙钟检查点，不是硬实时保证。** 它每 8 次迭代才看一次表（§9.3），因此单次决策仍可能略微超过 8 ms（最坏多跑 7 次迭代）；它保证的是“不会因为迭代数拉到 1000 就整帧卡住”，不是“一定在 8 ms 内结束”。
13. **Blatant 的 `m_AimTarget` 只在接管时发布，因此覆盖层可能“什么都不显示”。** 这是有意的（只显示真正被采用的瞄准，§5.2/§7.6）：当 kick-in 成功、或所有候选都在第 0 tick 死亡时，即使瞄准层内部算出过候选，橙色标记也不会亮。排查“瞄准层是否在工作”要看 `bc_avoid_aimbot_segments` 对 `Cost` 的影响，而不是看标记。
