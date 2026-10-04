# DDNet KRX Avoid 模块完整复现与工程实现技术规范书

## 目录
1. [模块概述与架构设计](#1-模块概述与架构设计)
2. [配置变量系统 (CVars) 规范](#2-配置变量系统-cvars-规范)
3. [核心数据结构与接口定义](#3-核心数据结构与接口定义)
4. [物理世界克隆与前向推演引擎 (Forward Simulation)](#4-物理世界克隆与前向推演引擎-forward-simulation)
5. [Basic Agent (基础避障) 算法实现细节](#5-basic-agent-基础避障-算法实现细节)
6. [Blatant Agent (激进避障) 算法实现细节](#6-blatant-agent-激进避障-算法实现细节)
7. [Legit Agent (拟人 MCTS 避障) 算法实现细节](#7-legit-agent-拟人-mcts-避障-算法实现细节)
8. [Fentbot Agent (流场与遗传轨迹优化) 算法实现细节](#8-fentbot-agent-流场与遗传轨迹优化-算法实现细节)
9. [DDNet 工程集成与编译指南](#9-ddnet-工程集成与编译指南)

---

## 1. 模块概述与架构设计

### 1.1 背景与目标
在 Teeworlds / DDNet（尤其是 Gores / DDRace 模式）中，地图上分布着大量的危险 Tile（如普通冻结块 Freeze、深度冻结 DeepFreeze、死亡块 Death、以及传送门 Teleport）。KRX 客户端的 **Avoid 模块** 是一个本地客户端的实时动作干预与轨迹推演系统。它在客户端向服务器发送每帧网络输入包前进行拦截，利用本地预测物理世界进行多步前向模拟，寻找最优的安全输入序列，从而防止玩家角色（Tee）意外冻结或死亡。

### 1.2 系统架构拓扑
```
               [ 玩家硬件输入 / Mouse / Keyboard ]
                               │
                               ▼
                    [ CControls 组件输入采样 ]
                               │
               (拦截点: CControls::OnMessage / OnRender)
                               │
                               ▼
                    ┌──────────────────────┐
                    │       BLAvoid        │ (主控制器)
                    │  (krx_avoidfreeze)   │
                    └──────────┬───────────┘
                               │
       ┌───────────────────────┼───────────────────────┐
       ▼                       ▼                       ▼
  [BasicAgent]           [BlatantAgent]           [LegitAgent]       [FentAgent]
 (枚举3向单步)        (并发贪心+NSIF回退)     (MCTS树搜索+拟人加权)  (流场+遗传微调)
       │                       │                       │                  │
       └───────────────────────┼───────────────────────┘                  │
                               ▼                                          ▼
                [ CGameWorld 本地克隆物理推演 ]                     [ 全局流场生成 ]
                (多 Tick 步进 + Freeze/Death 检测)
                               │
                               ▼
                     [ 生成 AvoidInput 结构 ]
                               │
                 (是否生效: m_Active == 1)
                               │
                ┌──────────────┴──────────────┐
              Yes                             No
                │                              │
                ▼                              ▼
    [ 覆盖 CNetObj_PlayerInput ]     [ 保持玩家原操作原样发送 ]
                │                              │
                └──────────────┬───────────────┘
                               ▼
                   [ Client()->SendInput() ]
```

---

## 2. 配置变量系统 (CVars) 规范

所有 Avoid 模块的配置项均需注册到 DDNet 的配置管理器中（对应 `src/engine/shared/config_variables.h`）：

```c
// 主控制开关与 Agent 选择
MACRO_CONFIG_INT(KrxAvoidFreeze, krx_avoidfreeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid freeze on/off")
MACRO_CONFIG_INT(KrxAvoidAgentType, krx_avoid_agent_type, 0, 0, 4, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid agent type (0: Basic, 1: Legit, 2: Blatant, 3: Fentbot, 4: PilotBot)")

// AFK 保护
MACRO_CONFIG_INT(KrxAvoidTileAfkProtection, krx_avoid_tile_afk_protection, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid tile afk protection on/off")
MACRO_CONFIG_INT(KrxAvoidTileAfkTime, krx_avoid_tile_afk_time, 5, 5, 300, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid tile afk timeout in seconds")

// 可视化渲染
MACRO_CONFIG_INT(KrxDrawAvoidTrackPoint, krx_drawavoidtrackpoint, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draw avoid trackpoint visual")
MACRO_CONFIG_INT(KrxDrawAvoidAimbot, krx_drawavoidaimbot, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draw avoid aimbot visual")
MACRO_CONFIG_INT(KrxDrawAvoidPath, krx_drawavoidpath, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draw avoid predicted path")

// Basic & 通用预测配置
MACRO_CONFIG_INT(KrxAvoidTilePlayerPrediction, krx_avoidtileplayerprediction, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Predict other players movement in avoidance")

// Blatant 激进模式参数
MACRO_CONFIG_INT(KrxAvoidTileBlatantCheckTicks, krx_avoid_tile_blatant_check_ticks, 26, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Blatant forward simulation lookahead ticks")
MACRO_CONFIG_INT(KrxAvoidTileKickInTicks, krx_avoid_tile_kick_in_ticks, 20, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Intervene only if player input safe ticks less than this")
MACRO_CONFIG_INT(KrxAvoidTileBlatantDirection, krx_avoid_tile_blatant_direction, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Blatant direction assistance")
MACRO_CONFIG_INT(KrxAvoidTileBlatantHook, krx_avoid_tile_blatant_hook, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Blatant hook assistance")
MACRO_CONFIG_INT(KrxAvoidTileBlatantTeles, krx_avoid_tile_blatant_teles, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid teleporter tiles")
MACRO_CONFIG_INT(KrxAvoidTileBlatantDeath, krx_avoid_tile_blatant_death, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Avoid death tiles")
MACRO_CONFIG_INT(KrxAvoidTileBlatantUnfreezeTile, krx_avoid_tile_blatant_unfreeze_tile, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Blatant seek unfreeze tile when possible")
MACRO_CONFIG_INT(KrxAvoidTileBlatantUnfreezeTileTicks, krx_avoid_tile_blatant_unfreeze_tile_ticks, 15, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Lookahead ticks for unfreeze seek")
MACRO_CONFIG_INT(KrxAvoidTileNsif, krx_avoid_tile_nsif, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "No Safe Input Found (NSIF) fallback mechanism")
MACRO_CONFIG_INT(KrxAvoidTileTrackPoints, krx_avoid_tile_track_points, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Track last hookable surface point")
MACRO_CONFIG_INT(KrxAvoidTileSafeAimTracking, krx_avoid_tile_safe_aim_tracking, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Only track point if target direction remains safe")
MACRO_CONFIG_INT(KrxAvoidTileAutoDrag, krx_avoid_tile_auto_drag, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Auto aim and hook nearby safe players")

// Legit 拟人 MCTS 模式参数
MACRO_CONFIG_INT(ClAvoidNumIterations, cl_avoid_num_iterations, 80, 5, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "MCTS simulation iterations count")
MACRO_CONFIG_INT(KrxAvoidTileExplorationConstant, krx_avoid_tile_exploration_constant, 140, 1, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "MCTS UCT exploration coefficient (scaled by 0.01)")
MACRO_CONFIG_INT(KrxAvoidTileLegitCheckTicks, krx_avoid_tile_legit_check_ticks, 18, 1, 60, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit forward simulation lookahead ticks")
MACRO_CONFIG_INT(KrxAvoidTileDirectionWeight, krx_avoid_tile_direction_weight, 170, 1, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Weight prioritizing player intended direction")
MACRO_CONFIG_INT(KrxAvoidTileLifespanWeight, krx_avoid_tile_lifespan_weight, 160, 1, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Weight prioritizing survival duration")
MACRO_CONFIG_INT(KrxAvoidTileHookWeight, krx_avoid_tile_hook_weight, 120, 1, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Weight prioritizing maintaining player hook state")
MACRO_CONFIG_INT(KrxAvoidTileLegitDirection, krx_avoid_tile_legit_direction, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit directional assist toggle")
MACRO_CONFIG_INT(KrxAvoidTileLegitHook, krx_avoid_tile_legit_hook, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit hook assist toggle")
MACRO_CONFIG_INT(KrxAvoidTileLegitTeles, krx_avoid_tile_legit_teles, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit avoid teleport tiles")
MACRO_CONFIG_INT(KrxAvoidTileLegitDeath, krx_avoid_tile_legit_death, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit avoid death tiles")
MACRO_CONFIG_INT(KrxAvoidTileLegitUnfreezeTile, krx_avoid_tile_legit_unfreeze_tile, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit seek unfreeze tile")
MACRO_CONFIG_INT(KrxAvoidTileLegitUnfreezeTileTicks, krx_avoid_tile_legit_unfreeze_tile_ticks, 12, 1, 60, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Legit lookahead for unfreeze seek")

// Fentbot 参数
MACRO_CONFIG_INT(KrxFentTicks, krx_fent_ticks, 120, 10, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Fentbot forward path simulation ticks")
MACRO_CONFIG_INT(KrxFentDosage, krx_fent_dosage, 30, 5, 200, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Genetic algorithm population count")
MACRO_CONFIG_INT(KrxFentTweakerTicks, krx_fent_tweaker_ticks, 8, 2, 32, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Consecutive input gene length")
```

---

## 3. 核心数据结构与接口定义

### 3.1 基础结构
```cpp
#pragma once
#include <base/vmath.h>
#include <generated/protocol.h>
#include <vector>

// 避障动作输出包装结构体
struct AvoidInput
{
    CNetObj_PlayerInput m_Input; // 修饰后的网络输入（40字节）
    int m_Active;                // 0: 保持玩家输入; 1: 机器人干预生效
};

// 贪心搜索候选结果
struct UGreedySearchResult
{
    int m_SurvivalTicks;         // 存活时长 (9999 代表全程安全)
    std::vector<CNetObj_PlayerInput> m_Sequence; // 成功存活的输入序列
    bool m_FoundSafe;            // 是否找到无损方案
};
```

### 3.2 Agent 基类抽象接口 (`BLAgent`)
```cpp
class CGameClient;
class CGameWorld;

class BLAgent
{
protected:
    CGameClient *m_pClient;

public:
    BLAgent(CGameClient *pClient) : m_pClient(pClient) {}
    virtual ~BLAgent() = default;

    // 虚表 [1]: 核心决策入口
    virtual AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) = 0;

    // 虚表 [2]: 可视化调试渲染 (世界坐标投影)
    virtual void OnRender() {}

    // 虚表 [3]: 状态重置 (地图切换/死亡)
    virtual void OnReset() {}

    // 虚表 [4]: 准星/动态相机平滑修正
    virtual void OnUpdateCamera(vec2 &TargetPos) {}
};
```

### 3.3 主控制器类定义 (`BLAvoid`)
```cpp
#include <game/client/component.h>

class BLAvoid : public CComponent
{
private:
    std::vector<BLAgent *> m_apAgents;
    int64_t m_LastActiveTime; // 用于 AFK 判定
    CNetObj_PlayerInput m_LastPlayerInput;

public:
    BLAvoid();
    virtual ~BLAvoid();

    virtual void OnInterfacesInit(CGameClient *pClient) override;
    virtual void OnRender() override;
    virtual void OnReset() override;

    // 输入拦截核心钩子函数
    void ProcessInput(CNetObj_PlayerInput *pInput);

    bool IsAfk() const;
    void UpdateAfkTimer(const CNetObj_PlayerInput *pInput);
};
```

---

## 4. 物理世界克隆与前向推演引擎 (Forward Simulation)

Avoid 模块的判断基石是能够在**不影响客户端实际渲染与真实物理状态**的前提下，对未来若干帧的本地角色进行确定性步进预测。

### 4.1 物理模拟核心推演函数 `SimulateCandidate`
在反编译器中对应的符号为 `func_0x00014036a8d0`。实现原理如下：

```cpp
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>

static constexpr int SIMULATION_SAFE_CONSTANT = 9999;

int SimulateCandidate(
    CGameClient *pClient,
    CGameWorld *pBaseWorld,
    const CNetObj_PlayerInput &CandidateInput,
    int CheckTicks,
    bool PredictPlayers,
    bool AvoidTeles,
    bool AvoidDeath)
{
    if(!pBaseWorld || CheckTicks <= 0)
        return SIMULATION_SAFE_CONSTANT;

    // 1. 在栈上或局部堆上创建 CGameWorld 克隆副本
    CGameWorld ClonedWorld;
    ClonedWorld.CopyWorld(pBaseWorld);

    int LocalClientId = pClient->m_Snap.m_LocalClientId;
    CCharacter *pChar = ClonedWorld.GetCharacterById(LocalClientId);
    if(!pChar)
        return SIMULATION_SAFE_CONSTANT;

    // 2. 步进前向模拟
    for(int Tick = 0; Tick < CheckTicks; ++Tick)
    {
        // 注入推演输入
        pChar->OnDirectInput(&CandidateInput);
        pChar->OnPredictedInput(&CandidateInput);

        // 推进一物理帧 (默认 50Hz, 20ms)
        ClonedWorld.Tick();

        // 3. 危险瓦片及状态检测
        // A. 冻结瓦片检测
        if(pChar->m_FreezeTime > 0 || pChar->m_FrozenLastTick)
            return Tick; // 触碰冻结块，返回存活 tick 数

        // B. 死亡瓦片检测 (当配置开启时)
        if(AvoidDeath)
        {
            vec2 Pos = pChar->Core()->m_Pos;
            int Tile = ClonedWorld.Collision()->GetCollisionAt(Pos.x, Pos.y);
            if(Tile & TILE_DEATH)
                return Tick;
        }

        // C. 传送门检测 (当配置开启且不希望进入未知传送门时)
        if(AvoidTeles)
        {
            vec2 Pos = pChar->Core()->m_Pos;
            int TeleTile = ClonedWorld.Collision()->GetTeleCheckpoint(Pos.x, Pos.y);
            if(TeleTile > 0)
                return Tick;
        }

        // D. 角色碰撞交互检测
        if(!PredictPlayers)
        {
            // 若禁用多玩家推演，关闭角色碰撞反馈计算
            pChar->Core()->m_Colliding = false;
        }
    }

    // 全程安全未触冻/未死亡
    return SIMULATION_SAFE_CONSTANT;
}
```

---

## 5. Basic Agent (基础避障) 算法实现细节

`BasicAgent` 是最轻量的规避算法，设计原则是**只在必然触冻时用最小干预修正方向**，不修改钩索和准星。

### 5.1 算法流程
1. 读取玩家当前输入 `CurrentInput`。
2. 使用 `CheckTicks = 6` 调用 `SimulateCandidate`。
3. 若返回值 `== 9999`，说明玩家当前操作在未来 6 帧内绝对安全，直接返回 `m_Active = 0`。
4. 若返回值 `< 9999`，生成 3 种离散方向候选：
   - 动作 0: `m_Direction = -1` (向左)
   - 动作 1: `m_Direction = 0`  (松开)
   - 动作 2: `m_Direction = 1`  (向右)
   其他字段保持玩家原始输入不变。
5. 分别推演 3 个动作，按 `SurvivalTicks` 降序排列；如果存活帧数相同，优先选择最接近玩家原始输入方向的动作。
6. 取最优动作输出，设置 `m_Active = 1`。

### 5.2 核心代码实现
```cpp
class BasicAgent : public BLAgent
{
public:
    BasicAgent(CGameClient *pClient) : BLAgent(pClient) {}

    virtual AvoidInput GetAction(const CNetObj_PlayerInput *pCurrentInput) override
    {
        AvoidInput Result;
        Result.m_Input = *pCurrentInput;
        Result.m_Active = 0;

        CGameWorld *pWorld = m_pClient->GetPredictionWorld();
        if(!pWorld)
            return Result;

        // 1. 验证当前操作是否已经安全
        int CurrentSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, 6,
            g_Config.m_KrxAvoidTilePlayerPrediction, false, true);

        if(CurrentSafety == SIMULATION_SAFE_CONSTANT)
            return Result; // 无需干预

        // 2. 穷举三向候选
        const int Directions[3] = { -1, 0, 1 };
        int BestScore = -1;
        int BestDir = pCurrentInput->m_Direction;

        for(int Dir : Directions)
        {
            CNetObj_PlayerInput Candidate = *pCurrentInput;
            Candidate.m_Direction = Dir;

            int Score = SimulateCandidate(m_pClient, pWorld, Candidate, 6,
                g_Config.m_KrxAvoidTilePlayerPrediction, false, true);

            if(Score > BestScore)
            {
                BestScore = Score;
                BestDir = Dir;
            }
        }

        if(BestScore > CurrentSafety)
        {
            Result.m_Input.m_Direction = BestDir;
            Result.m_Active = 1;
        }

        return Result;
    }
};
```

---

## 6. Blatant Agent (激进避障) 算法实现细节

`BlatantAgent` 面向极端难度地图，拥有全面的自救策略（包含钩索、移动方向、解冻块搜寻以及前缀序列缓存回退机制）。

### 6.1 迟滞介入阈值 (`KickInTicks`)
为了避免在玩家正常飞行或悬空时频繁抖动改键，Blatant 设置了 `KickInTicks` 迟滞过滤器：
- 只有当当前玩家输入能够维持安全的 Tick 数 **低于** `g_Config.m_KrxAvoidTileKickInTicks`（默认 20）时，搜索算法才会启动干预。

### 6.2 候选动作空间构建 (`GenerateCandidateActions`)
对应反编译函数 `0x14032e530`：
每个候选动作是方向与钩索的组合：
$$\mathcal{A} = \{(dir, hook) \mid dir \in \{-1, 0, 1\}, hook \in \{0, 1\}\}$$
当配置中禁用了 `Hook Assistance` 时，$hook$ 强制锁定为玩家当前钩索状态。

### 6.3 多线程并发贪心搜索 (`GreedySearch`)
对应反编译函数 `0x14032eb50`：
```cpp
UGreedySearchResult GreedySearch(
    CGameClient *pClient,
    CGameWorld *pWorld,
    const CNetObj_PlayerInput &BaseInput,
    int CheckTicks)
{
    UGreedySearchResult Result;
    Result.m_FoundSafe = false;
    Result.m_SurvivalTicks = 0;

    auto Candidates = GenerateCandidateActions(BaseInput);
    std::vector<std::future<int>> Futures;

    // 利用线程池并发推进每个候选动作的模拟
    for(const auto &Act : Candidates)
    {
        Futures.push_back(pClient->GetThreadPool()->SubmitTask([=]() {
            return SimulateCandidate(pClient, pWorld, Act, CheckTicks,
                g_Config.m_KrxAvoidTilePlayerPrediction,
                g_Config.m_KrxAvoidTileBlatantTeles,
                g_Config.m_KrxAvoidTileBlatantDeath);
        }));
    }

    int BestIndex = -1;
    for(size_t i = 0; i < Futures.size(); ++i)
    {
        int Score = Futures[i].get();
        if(Score > Result.m_SurvivalTicks)
        {
            Result.m_SurvivalTicks = Score;
            BestIndex = i;
        }
    }

    if(BestIndex != -1)
    {
        Result.m_Sequence.push_back(Candidates[BestIndex]);
        if(Result.m_SurvivalTicks == SIMULATION_SAFE_CONSTANT)
            Result.m_FoundSafe = true;
    }

    return Result;
}
```

### 6.4 NSIF (No Safe Input Found) 容错机制
在极其险峻的陷阱中，当前帧所有候选动作都无法在全长 `Check Ticks` 内存活：
- 如果 `krx_avoid_tile_nsif == 1`：
  从上一帧推演出的有效存活路径队列 `m_SavedSafeSequence` 中提取下一个残余动作，保持执行，以争取更多时间等待物理状态发生位移改变。

### 6.5 轨道点与自动拉扯 (`TrackPoint` & `AutoDrag`)
1. **Track Point**：维护一个玩家此前成功钩中实心 Tile 的矢量点 `m_TrackPoint`。
   - 若 `g_Config.m_KrxAvoidTileTrackPoints == 1`：在搜索时，测试将准星 `(TargetX, TargetY)` 强行对准该轨道的推演安全性；若其存活帧数更长，优先覆盖准星。
2. **Auto Drag**：遍历 $360^\circ$ FOV 内部的队友 Tee。如果瞄准并勾住该队友不会导致自身与队友触冻，则自动对准其边界。

---

## 7. Legit Agent (拟人 MCTS 避障) 算法实现细节

`LegitAgent` 使用基于 **UCT 的蒙特卡洛树搜索 (Monte Carlo Tree Search)**，核心特点是在价值评价函数中引入了**多目标拟人加权惩罚项**，使得机器人动作平滑、无机械抖动，外表几乎看不出外挂辅助痕迹。

### 7.1 MCTS 节点结构定义
每个节点大小对应反编译中的 `0x58` (88 字节)：
```cpp
struct MCTSNode
{
    MCTSNode *m_pParent = nullptr;
    std::vector<MCTSNode *> m_vChildren;
    
    CNetObj_PlayerInput m_Action; // 抵达该节点所采用的操作
    int m_Visits = 0;             // 访问次数 n
    double m_TotalValue = 0.0;    // 累积价值 Q
    int m_LifespanTicks = 0;      // 存活时长统计
    bool m_IsTerminal = false;    // 是否已冻结

    ~MCTSNode()
    {
        for(auto *pChild : m_vChildren)
            delete pChild;
    }
};
```

### 7.2 UCT 打分与拟人加权公式 (核心反编译还原)
对应反编译函数 `0x140338260`。在标准 UCT 的基础上叠加三项输入偏差惩罚：

$$\text{Score}(child) = \underbrace{\frac{Q_i}{n_i} + c \cdot \sqrt{\frac{\ln N_{parent}}{n_i}}}_{\text{标准 UCT 探索利用项}} + \underbrace{H(child)}_{\text{拟人加权启发项}}$$

启发项 $H(child)$ 的确切实现：
```cpp
double CalculateHeuristicScore(
    const MCTSNode *pNode,
    const CNetObj_PlayerInput *pPlayerInput,
    double WeightDir,
    double WeightLife,
    double WeightHook)
{
    // 1. 方向偏离惩罚 (避免频繁反向拉扯)
    double DirDiff = std::abs(pNode->m_Action.m_Direction - pPlayerInput->m_Direction);
    double DirPenalty = - DirDiff * WeightDir * 0.001;

    // 2. 钩索保持惩罚 (避免反常的反复松放钩索)
    double HookDiff = std::abs(pNode->m_Action.m_Hook - pPlayerInput->m_Hook);
    double HookPenalty = - HookDiff * WeightHook * 0.001;

    // 3. 存活周期奖励
    double LifeReward = pNode->m_LifespanTicks * WeightLife * 0.001;

    return DirPenalty + HookPenalty + LifeReward;
}
```

### 7.3 MCTS 四阶段循环实现 (`MCTSSearch`)
对应反编译函数 `0x1403390d0`：
```cpp
MCTSNode* MCTSSearch(
    CGameClient *pClient,
    CGameWorld *pWorld,
    const CNetObj_PlayerInput &HumanInput,
    int Iterations,
    double ExplorationC,
    int CheckTicks)
{
    MCTSNode *pRoot = new MCTSNode();
    pRoot->m_Action = HumanInput;

    for(int iter = 0; iter < Iterations; ++iter)
    {
        // 1. Selection (根据 UCT + Heuristic 沿树向下挑选最优叶子)
        MCTSNode *pCurr = pRoot;
        while(!pCurr->m_vChildren.empty())
        {
            MCTSNode *pBestChild = nullptr;
            double BestScore = -1e9;
            for(MCTSNode *pChild : pCurr->m_vChildren)
            {
                double Uct = 0.0;
                if(pChild->m_Visits == 0)
                {
                    Uct = 1e5; // 优先访问未探索节点
                }
                else
                {
                    double Exploitation = pChild->m_TotalValue / pChild->m_Visits;
                    double Exploration = ExplorationC * std::sqrt(std::log(pCurr->m_Visits) / pChild->m_Visits);
                    double Heuristic = CalculateHeuristicScore(pChild, &HumanInput,
                        g_Config.m_KrxAvoidTileDirectionWeight,
                        g_Config.m_KrxAvoidTileLifespanWeight,
                        g_Config.m_KrxAvoidTileHookWeight);
                    Uct = Exploitation + Exploration + Heuristic;
                }

                if(Uct > BestScore)
                {
                    BestScore = Uct;
                    pBestChild = pChild;
                }
            }
            pCurr = pBestChild;
        }

        // 2. Expansion (如果非终止态，展开可能动作)
        if(!pCurr->m_IsTerminal && pCurr->m_Visits > 0)
        {
            for(int d = -1; d <= 1; ++d)
            {
                for(int h = 0; h <= 1; ++h)
                {
                    MCTSNode *pNewChild = new MCTSNode();
                    pNewChild->m_pParent = pCurr;
                    pNewChild->m_Action = pCurr->m_Action;
                    pNewChild->m_Action.m_Direction = d;
                    pNewChild->m_Action.m_Hook = h;
                    pCurr->m_vChildren.push_back(pNewChild);
                }
            }
            if(!pCurr->m_vChildren.empty())
                pCurr = pCurr->m_vChildren[rand() % pCurr->m_vChildren.size()];
        }

        // 3. Rollout / Simulation (前向模拟物理)
        int Survival = SimulateCandidate(pClient, pWorld, pCurr->m_Action, CheckTicks,
            g_Config.m_KrxAvoidTilePlayerPrediction,
            g_Config.m_KrxAvoidTileLegitTeles,
            g_Config.m_KrxAvoidTileLegitDeath);

        double Reward = (Survival == SIMULATION_SAFE_CONSTANT) ? 1.0 : (double)Survival / CheckTicks;
        pCurr->m_LifespanTicks = Survival;
        if(Survival < CheckTicks)
            pCurr->m_IsTerminal = true;

        // 4. Backpropagation (反向回溯更新 Q 与 n)
        while(pCurr)
        {
            pCurr->m_Visits++;
            pCurr->m_TotalValue += Reward;
            pCurr = pCurr->m_pParent;
        }
    }

    // 选取根节点下访问量最大（或综合价值最高）的子分支
    MCTSNode *pSelected = nullptr;
    int MostVisits = -1;
    for(MCTSNode *pChild : pRoot->m_vChildren)
    {
        if(pChild->m_Visits > MostVisits)
        {
            MostVisits = pChild->m_Visits;
            pSelected = pChild;
        }
    }

    return pSelected; // 调用者获取其 m_Action 后释放整个树
}
```

---

## 8. Fentbot Agent (流场与遗传轨迹优化) 算法实现细节

`FentAgent` 是针对长距离 King of Gores (KoG) 地图开发的实验性全局规划器。它使用 **流场 (Flow Field) 引导结合遗传算法 (Genetic Tweaker)**。

### 8.1 二维流场通道 (Tunnel & FlowField)
在初始化或切换地图时，Fentbot 会对地图瓦片进行广度优先搜索 (BFS) 或 Dijkstra 扫描，从终点（或未冻结安全通道）开始逆向扩散，计算每个瓦片 $(x, y)$ 指向下一个安全格的最佳单位向量 $\vec{D}(x, y)$。

### 8.2 轨迹适应度函数 (`EvaluateActions`)
对应反编译函数 `0x1403342c0`：
在推演连续输入序列时，每个 Tick 统计角色速度 $\vec{v}$ 与流场方向的点积：

$$\text{Fitness} = \sum_{t=0}^{T} \left( \vec{v}_t \cdot \vec{D}(\text{Tile}(x_t, y_t)) \right) \cdot W_{flow} - \lambda \cdot \text{Distance}(x_T, Target)$$

- 若角色在中途触碰冻结或离开有效 Tunnel，立即中断推演，该基因个体被赋予极低适应度负分。

### 8.3 遗传微调 (Tweaker)
- **个体编码**：由连续若干个 Tick 的输入组成的基因片段（长度为 `krx_fent_tweaker_ticks`）。
- **种群繁殖**：并发运行 `krx_fent_dosage` 个个体测试，应用单点变异（如随机翻转某一帧的 Jump 或 Direction）。
- **异步求解**：计算全部在后台线程进行，求解出的有效路径存入队列；主线程的 `GetAction` 直接按 Tick 依序弹出执行。

---

## 9. DDNet 工程集成与编译指南

为使其他 AI 或开发者能够将此 Avoid 系统无缝植入原版 DDNet，请遵循以下工程集成步骤。

### 9.1 源码目录组织
在 `ddnet/src/game/client/components/` 下新建 `avoid/` 子目录：
```
src/game/client/components/avoid/
├── avoid.h                  // BLAvoid 主组件定义
├── avoid.cpp                // BLAvoid 框架与输入调度
├── agent_base.h             // BLAgent 虚基类
├── agent_basic.cpp          // BasicAgent 实现
├── agent_blatant.cpp        // BlatantAgent 实现
├── agent_legit.cpp          // LegitAgent 实现 (MCTS)
├── agent_fent.cpp           // FentAgent 实现 (流场)
└── simulation.h             // SimulateCandidate 物理模拟推演核心
```

### 9.2 输入管道挂载点 (Hook Integration)
在 `src/game/client/components/controls.cpp` 的 `CControls::OnMessage` 或输入准备函数中挂载调用：

```cpp
// 在 controls.cpp 顶部包含
#include <game/client/components/avoid/avoid.h>

// 在 CControls::OnMessage 中，拷贝输入数据前添加拦截逻辑：
int CControls::OnMessage(int Msg, void *pRawMsg)
{
    // ... 原有输入更新逻辑 ...

    // [KRX AVOID 模块拦截钩子]
    if(g_Config.m_KrxAvoidFreeze && GameClient()->m_pAvoid)
    {
        GameClient()->m_pAvoid->ProcessInput(&m_aInputData[g_Config.m_ClDummy]);
    }

    // 将最终修饰后的输入复制并发送
    mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
    return sizeof(m_aInputData[0]);
}
```

### 9.3 组件注册 (`CGameClient`)
在 `src/game/client/gameclient.h` 中增加指针变量：
```cpp
class CBAvoid *m_pAvoid;
```
在 `src/game/client/gameclient.cpp` 中初始化并加入组件列表：
```cpp
#include "components/avoid/avoid.h"

// 在 CGameClient 构造函数或组件注册处：
m_pAvoid = new BLAvoid();
m_All.add(m_pAvoid);
```

### 9.4 CMake 构建配置更新
在 `ddnet/CMakeLists.txt` 中添加新源码路径：
```cmake
set(CLIENT_AVOID_SRC
    src/game/client/components/avoid/avoid.cpp
    src/game/client/components/avoid/agent_basic.cpp
    src/game/client/components/avoid/agent_blatant.cpp
    src/game/client/components/avoid/agent_legit.cpp
    src/game/client/components/avoid/agent_fent.cpp
)

target_sources(DDNet PRIVATE ${CLIENT_AVOID_SRC})
```

---

## 10. 验证与测试清单

完成复现后，可通过以下步骤在 DDNet 客户端控制台（F1）进行功能验证：

1. **基本启闭测试**：
   在控制台输入 `bind x toggle krx_avoidfreeze 1 0`，确认按键可切换避障状态。
2. **Basic 模式验证**：
   设置 `krx_avoid_agent_type 0`，向冻结池走动，观察角色是否在临近冻结时自动反向回缩。
3. **Legit MCTS 拟人测试**：
   设置 `krx_avoid_agent_type 1`，调整 `cl_avoid_num_iterations 100`，观察在复杂障碍物跳跃时，角色移动是否顺滑无突变。
4. **Blatant 激进测试**：
   设置 `krx_avoid_agent_type 2`，开启 `krx_avoid_tile_track_points 1` 与 `krx_avoid_tile_blatant_hook 1`，在极端冻结下落场景中确认钩索与方向自动抢救成功率。
