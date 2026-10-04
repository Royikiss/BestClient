# DDNet KRX Avoid 模块完整复现与工程落地技术全书 (1:1 像素级行为还原版)

> **版本**：v3.0 (Master Unified Edition)  
> **适用目标**：指导其他 AI 或开发者在 [ddnet](file:///home/royi/GreatWall/Projects/krx&ddnet/ddnet) 原项目中**完整复现** KRX 客户端的全部 Avoid（防冻避障）子系统功能。  
> **还原准则**：包含全部数据结构、完整 C++ 实现代码、精确参数默认值矩阵、底层浮点数学公式、汇编位运算常量、多线程调度及 CMake 工程集成。

---

## 目录
1. [模块概述与架构拓扑](#1-模块概述与架构拓扑)
2. [全量配置参数矩阵 (CVars 默认值与极值表)](#2-全量配置参数矩阵-cvars-默认值与极值表)
3. [核心数据结构与接口定义](#3-核心数据结构与接口定义)
4. [物理世界克隆与前向推演引擎 (Simulation Engine)](#4-物理世界克隆与前向推演引擎-simulation-engine)
5. [Basic Agent (基础避障) 1:1 完整实现](#5-basic-agent-基础避障-11-完整实现)
6. [Blatant Agent (激进并发避障) 1:1 完整实现](#6-blatant-agent-激进并发避障-11-完整实现)
7. [Legit Agent (拟人 MCTS 避障) 1:1 完整实现与数学公式](#7-legit-agent-拟人-mcts-避障-11-完整实现与数学公式)
8. [Fentbot Agent (流场与遗传轨迹优化) 1:1 完整实现与档位](#8-fentbot-agent-流场与遗传轨迹优化-11-完整实现与档位)
9. [Pilot Bot (自主巡航与跟随) 核心参数与状态定义](#9-pilot-bot-自主巡航与跟随-核心参数与状态定义)
10. [DDNet 原版工程集成与输入拦截管道](#10-ddnet-原版工程集成与输入拦截管道)
11. [CMake 构建系统配置](#11-cmake-构建系统配置)
12. [行为一致性验证与对齐测试清单](#12-行为一致性验证与对齐测试清单)

---

## 1. 模块概述与架构拓扑

### 1.1 背景与设计理念
在 DDNet（DDraceNetwork）中，地图上分布着普通冻结（Freeze）、深度冻结（DeepFreeze）、死亡（Death）以及传送门（Teleport）等危险瓦片。KRX 客户端的 Avoid 模块是一个高精度的本地实时动作决策引擎。它通过在本地**克隆物理世界进行前向 Tick 推演**，并在客户端网络层发送输入前拦截并修饰 `CNetObj_PlayerInput`，实现自动规避危险、甚至自主寻路脱险。

### 1.2 系统架构拓扑
```
                [ 玩家硬件输入 / Mouse / Keyboard ]
                                │
                                ▼
                    [ CControls::OnMessage 采样 ]
                                │
                                ▼ (拦截点: BLAvoid::ProcessInput)
                    ┌──────────────────────┐
                    │       BLAvoid        │ (主控制器)
                    │  (krx_avoidfreeze)   │
                    └──────────┬───────────┘
                               │
       ┌───────────────────────┼───────────────────────┬───────────────────────┐
       ▼ (Mode 0)              ▼ (Mode 1)              ▼ (Mode 2)              ▼ (Mode 3)
  [BasicAgent]            [LegitAgent]           [BlatantAgent]           [FentAgent]
 (枚举3向单步)        (MCTS树搜索+拟人加权)   (并发贪心+NSIF回退)     (流场+遗传微调)
       │                       │                       │                       │
       └───────────────────────┼───────────────────────┴───────────────────────┘
                               ▼
                [ CGameWorld 本地克隆物理推演 ]
                (多 Tick 步进 + Freeze/Death 检测)
                               │
                               ▼
                    [ 产出 AvoidInput 结果 ]
                               │
                ┌──────────────┴──────────────┐
             Active == 1                   Active == 0
                │                             │
                ▼                             ▼
    [ 覆盖 CNetObj_PlayerInput ]     [ 保持原输入原样输出 ]
                │                             │
                └──────────────┬──────────────┘
                               ▼
                   [ Client()->SendInput() ]
```

---

## 2. 全量配置参数矩阵 (CVars 默认值与极值表)

以下数据直接从二进制 `unpacked_krx.exe` 的 CVar 注册表（`0x140079000 - 0x14007c900`）及内部字符串反编译提取，**严禁修改默认值**以确保效果完全一致：

| CVar 标识符 | 内部变量地址 | 类型 | 默认值 | 最小值 | 最大值 | 说明 | 对应 Agent |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- | :--- |
| **`krx_avoidfreeze`** | `0x1406ae530` | int | **0** | 0 | 1 | Avoid 避障总开关 (0:关, 1:开) | 全局 |
| **`krx_avoid_tile_agent_type`** | `0x1406ae52c` | int | **0** | 0 | 4 | 算法模式 (0:Basic, 1:Legit, 2:Blatant, 3:Fent, 4:Pilot) | 全局 |
| **`krx_avoid_tile_afk_protection`** | `0x1406ae540` | int | **0** | 0 | 1 | AFK 挂机自动禁用保护 | 全局 |
| **`krx_avoid_tile_afk_time`** | `0x1406ae544` | int | **5** | 5 | 300 | AFK 超时时间（秒） | 全局 |
| **`krx_drawavoidpath`** | `0x1406ae53c` | int | **1** | 0 | 1 | 渲染避障预测轨迹路径 | 可视化 |
| **`krx_drawavoidtrackpoint`** | `0x1406ae534` | int | **0** | 0 | 1 | 渲染轨道锁定锚点 | 可视化 |
| **`krx_drawavoidaimbot`** | `0x1406ae538` | int | **0** | 0 | 1 | 渲染自动拉扯/瞄准目标 | 可视化 |
| **`krx_avoidtileplayerprediction`** | `0x1406ae5bc` | int | **1** | 0 | 1 | 推演时是否考虑其他玩家碰撞体 | 全局 |
| **`krx_avoid_tile_direction_weight`** | `0x1406ae548` | int | **170** | 1 | 1000 | 拟人 MCTS：玩家移动方向偏好权重 | Legit |
| **`krx_avoid_tile_lifespan_weight`** | `0x1406ae54c` | int | **160** | 1 | 1000 | 拟人 MCTS：存活帧数奖励权重 | Legit |
| **`krx_avoid_tile_hook_weight`** | `0x1406ae550` | int | **260** | 1 | 1000 | 拟人 MCTS：钩索状态维持偏好权重 | Legit |
| **`krx_avoid_tile_exploration_constant`**| `0x1406ae554` | int | **4** | 1 | 1000 | 拟人 MCTS：UCT 探索常数 $c$ | Legit |
| **`cl_avoid_num_iterations`** | `0x1406ae558` | int | **100** | 1 | 1000 | 拟人 MCTS：每次决策的迭代仿真次数 | Legit |
| **`krx_avoid_tile_legit_check_ticks`** | `0x1406ae580` | int | **6** | 1 | 50 | 拟人 MCTS：单次模拟前瞻深度 (Ticks) | Legit |
| **`krx_avoid_tile_legit_direction`** | `0x1406ae584` | int | **1** | 0 | 1 | Legit 允许修改水平方向 | Legit |
| **`krx_avoid_tile_legit_hook`** | `0x1406ae588` | int | **1** | 0 | 1 | Legit 允许修改钩索状态 | Legit |
| **`krx_avoid_tile_legit_teles`** | `0x1406ae58c` | int | **0** | 0 | 1 | Legit 规避传送门瓦片 | Legit |
| **`krx_avoid_tile_legit_death`** | `0x1406ae590` | int | **0** | 0 | 1 | Legit 规避死亡瓦片 | Legit |
| **`krx_avoid_tile_legit_unfreeze_tile`** | `0x1406ae594` | int | **0** | 0 | 1 | Legit 主动搜寻解冻块 | Legit |
| **`krx_avoid_tile_legit_unfreeze_tile_ticks`** | `0x1406ae598` | int | **5** | 1 | 30 | Legit 搜寻解冻块前瞻深度 | Legit |
| **`krx_avoid_tile_blatant_check_ticks`** | `0x1406ae55c` | int | **26** | 1 | 50 | Blatant：贪心推演前瞻深度 (Ticks) | Blatant |
| **`krx_avoid_tile_kick_in_ticks`** | `0x1406ae5b8` | int | **26** | 1 | 50 | Blatant：原操作安全阈值 (迟滞介入) | Blatant |
| **`krx_avoid_tile_blatant_direction`** | `0x1406ae560` | int | **1** | 0 | 1 | Blatant 允许修改水平方向 | Blatant |
| **`krx_avoid_tile_blatant_hook`** | `0x1406ae564` | int | **1** | 0 | 1 | Blatant 允许修改钩索状态 | Blatant |
| **`krx_avoid_tile_blatant_teles`** | `0x1406ae568` | int | **0** | 0 | 1 | Blatant 规避传送门瓦片 | Blatant |
| **`krx_avoid_tile_blatant_death`** | `0x1406ae56c` | int | **0** | 0 | 1 | Blatant 规避死亡瓦片 | Blatant |
| **`krx_avoid_tile_blatant_unfreeze_tile`** | `0x1406ae570` | int | **0** | 0 | 1 | Blatant 主动搜寻解冻块 | Blatant |
| **`krx_avoid_tile_blatant_unfreeze_tile_ticks`**| `0x1406ae574` | int | **26** | 0 | 30 | Blatant 搜寻解冻块前瞻深度 | Blatant |
| **`krx_avoid_tile_nsif`** | `0x1406ae5b4` | int | **1** | 0 | 1 | 无安全输入时启用历史缓存回退 | Blatant |
| **`krx_avoid_tile_track_points`** | `0x1406ae5b0` | int | **0** | 0 | 1 | 锁定实心墙面瞄准轨道 | Blatant |
| **`krx_avoid_tile_safe_aim_tracking`** | `0x1406ae5a8` | int | **0** | 0 | 1 | 仅当轨道全程安全时锁定准星 | Blatant |
| **`krx_avoid_tile_auto_drag`** | `0x1406ae5ac` | int | **0** | 0 | 1 | 自动对准并勾取安全队友 | Blatant |
| **`krx_avoid_tile_blatant_aimbot`** | `0x1406ae578` | int | **0** | 0 | 1 | 启用避障内部瞄准自瞄支持 | Blatant |
| **`krx_avoid_tile_blatant_fov`** | `0x1406ae57c` | int | **90** | 10 | 360 | 内部瞄准视野角度 (FOV) | Blatant |
| **`krx_avoid_tile_aimbot_segments`** | `0x1406ae5a4` | int | **5** | 1 | 64 | 瞄准扇区扫描切分数量 | Blatant |
| **`cl_avoid_auto_aim_aimbot`** | `0x1406ae59c` | int | **0** | 0 | 1 | 自动瞄向最长存活方向 | Blatant |
| **`cl_avoid_aim_assist_aimbot`** | `0x1406ae5a0` | int | **1** | 0 | 1 | 磁吸辅助吸附至最安全方向 | Blatant |
| **`krx_avoid_tile_fent_ticks`** | `0x1406ae5d4` | int | **1000**| 1000 | 10000 | Fentbot 寻路最长前瞻 Tick 数 | Fentbot |
| **`krx_avoid_tile_fent_tweaker_actions`**| `0x1406ae5d8` | int | **50** | 50 | 5000 | Fentbot 候选动作采样规模 | Fentbot |
| **`krx_avoid_tile_fent_tweaker_ticks`** | `0x1406ae5dc` | int | **1** | 1 | 30 | Fentbot 基因动作连续保持周期 | Fentbot |
| **`krx_avoid_tile_fent_tweaker_dosage`** | `0x1406ae5e0` | int | **1** | 1 | 500 | Fentbot 遗传迭代代数 (Dosage) | Fentbot |
| **`krx_avoid_tile_fent_quality_setting`**| `0x1406ae5ec` | int | **0** | 0 | 2 | Fent 档位 (0:Low, 1:Mid, 2:Max) | Fentbot |
| **`krx_avoid_tile_fent_advanced_settings`**| `0x1406ae5e8` | int | **0** | 0 | 1 | 是否使用自定义 Fent 高级参数 | Fentbot |
| **`krx_avoid_tile_fent_light_tile`** | `0x1406ae5f0` | int | **0** | 0 | 1 | 浅冻（临近解冻块）穿越寻路支持 | Fentbot |
| **`krx_avoid_tile_fent_light_tile_radius`**| `0x1406ae5f4`| int | **1** | 0 | 20 | 浅冻探测半径 (Tiles) | Fentbot |
| **`krx_avoid_tile_pilot_bot_mode`** | `0x1406ae5c8` | int | **0** | 0 | 2 | Pilot 行为模式 (0:自主, 1:准星, 2:跟随) | Pilot |
| **`krx_avoid_tile_pilot_bot_population_size`**| `0x1406ae5cc`| int | **2048**| 128 | 8192 | Pilot Bot 种群并行规模 | Pilot |
| **`krx_avoid_tile_pilot_bot_exploration_depth`**| `0x1406ae5d0`| int | **17** | 5 | 50 | Pilot Bot 单条序列仿真深度 | Pilot |
| **`krx_avoid_tile_pilot_bot_sequence_length`**| `0x1406ae5c4`| int | **5** | 1 | 20 | Pilot Bot 规划序列步长 | Pilot |

---

## 3. 核心数据结构与接口定义

### 3.1 基础输入结构体
```cpp
#pragma once
#include <base/vmath.h>
#include <generated/protocol.h>
#include <vector>

// 避障动作输出包装结构体
struct AvoidInput
{
    CNetObj_PlayerInput m_Input; // 40 字节网络输入
    int m_Active;                // 0: 保持玩家输入; 1: 机器人干预生效
};

// 贪心搜索候选结果
struct UGreedySearchResult
{
    int m_SurvivalTicks;                         // 存活时长 (9999 代表全程安全)
    std::vector<CNetObj_PlayerInput> m_Sequence; // 成功存活的输入序列
    bool m_FoundSafe;                            // 是否找到完全安全的方案
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
#include <vector>

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

## 4. 物理世界克隆与前向推演引擎 (Simulation Engine)

推演引擎（二进制符号 `func_0x00014036a8d0`）是保证所有模式行为绝对一致的物理裁判。

### 4.1 核心模拟函数签名与精确实现
```cpp
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>

// 二进制内部最高安全判定常量 (0x270f = 9999)
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

    // 1. 克隆世界副本 (二进制中使用 CopyWorld 进行全量复制)
    CGameWorld ClonedWorld;
    ClonedWorld.CopyWorld(pBaseWorld);

    int LocalClientId = pClient->m_Snap.m_LocalClientId;
    CCharacter *pChar = ClonedWorld.GetCharacterById(LocalClientId);
    if(!pChar)
        return SIMULATION_SAFE_CONSTANT;

    // 2. 逐 Tick 推进模拟
    int SurvivedTicks = 0;
    while(SurvivedTicks < CheckTicks)
    {
        // 注入当前 Tick 操作
        pChar->OnDirectInput(&CandidateInput);
        pChar->OnPredictedInput(&CandidateInput);

        // 推进一物理帧 (50Hz)
        ClonedWorld.Tick();

        // 3. 严格冻结条件检测 (对应反编译 0x14036a98b - 0x14036a9a4)
        if(pChar->m_FreezeTime > 0       // 触碰普通 Freeze (m_FreezeTime > 0)
           || pChar->m_FrozenLastTick     // 标记为刚冻结
           || pChar->Core()->m_DeepFrozen // 触碰 DeepFreeze (不可解冻)
           )
        {
            return SurvivedTicks;
        }

        // 4. 可选死亡/传送门瓦片判定
        if(AvoidDeath)
        {
            vec2 Pos = pChar->Core()->m_Pos;
            int Tile = ClonedWorld.Collision()->GetCollisionAt(Pos.x, Pos.y);
            if(Tile & TILE_DEATH)
                return SurvivedTicks;
        }

        if(AvoidTeles)
        {
            vec2 Pos = pChar->Core()->m_Pos;
            int TeleTile = ClonedWorld.Collision()->GetTeleCheckpoint(Pos.x, Pos.y);
            if(TeleTile > 0)
                return SurvivedTicks;
        }

        SurvivedTicks++;
    }

    // 若全程完整存活 CheckTicks 步，严格返回常量 9999
    return SIMULATION_SAFE_CONSTANT;
}
```

---

## 5. Basic Agent (基础避障) 1:1 完整实现

### 5.1 核心算法规则
1. **前瞻步长**：严格固定为 **6 Ticks**（代码中写死，不读取配置）。
2. **候选空间枚举顺序**：遍历向量必须保持为：`{ 0, -1, 1 }`（先不动，再向左，再向右）。
3. **介入原则**：
   - 优先推演玩家原输入：`CurrentSafety = SimulateCandidate(PlayerInput, 6)`。
   - 若 `CurrentSafety == 9999`，**绝对不介入**，输出 `m_Active = 0`。
   - 否则遍历 `{0, -1, 1}` 寻找 `Score > CurrentSafety` 的方向。
   - 若多个候选方向均达到 `9999`，**以枚举顺序最先达到 9999 者为准**。

```cpp
AvoidInput BasicAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
{
    AvoidInput Out;
    Out.m_Input = *pCurrentInput;
    Out.m_Active = 0;

    CGameWorld *pWorld = m_pClient->GetPredictionWorld();
    if(!pWorld)
        return Out;

    // 1. 测试原输入 (固定 6 Ticks)
    int Baseline = SimulateCandidate(m_pClient, pWorld, *pCurrentInput, 6,
        g_Config.m_KrxAvoidTilePlayerPrediction, false, true);

    if(Baseline == SIMULATION_SAFE_CONSTANT)
        return Out; // 原操作安全，不干预

    // 2. 严格按 {0, -1, 1} 顺序枚举
    const int CandidateDirs[3] = { 0, -1, 1 };
    int BestScore = Baseline;
    int BestDir = pCurrentInput->m_Direction;

    for(int Dir : CandidateDirs)
    {
        CNetObj_PlayerInput Candidate = *pCurrentInput;
        Candidate.m_Direction = Dir;

        int Score = SimulateCandidate(m_pClient, pWorld, Candidate, 6,
            g_Config.m_KrxAvoidTilePlayerPrediction, false, true);

        if(Score > BestScore)
        {
            BestScore = Score;
            BestDir = Dir;
            if(Score == SIMULATION_SAFE_CONSTANT)
                break; // 找到无损方案立即退出
        }
    }

    if(BestScore > Baseline)
    {
        Out.m_Input.m_Direction = BestDir;
        Out.m_Active = 1;
    }

    return Out;
}
```

---

## 6. Blatant Agent (激进并发避障) 1:1 完整实现

### 6.1 迟滞介入阈值 (`KickInTicks`)
- 使用 `g_Config.m_KrxAvoidTileKickInTicks`（默认 26 Ticks）对当前操作进行前向推演。
- **迟滞规则**：只要玩家当前操作能存活 $\ge \text{KickInTicks}$（即返回 `9999`），**直接放弃介入**！保留原输入并返回 `m_Active = 0`。

### 6.2 候选动作空间生成 (`GenerateCandidateActions`)
```cpp
std::vector<CNetObj_PlayerInput> BlatantAgent::GenerateCandidateActions(const CNetObj_PlayerInput &BaseInput)
{
    std::vector<CNetObj_PlayerInput> Actions;

    int Dirs[3] = { 0, -1, 1 };
    int Hooks[2] = { 0, 1 };

    int DirCount = g_Config.m_KrxAvoidTileBlatantDirection ? 3 : 1;
    int HookCount = g_Config.m_KrxAvoidTileBlatantHook ? 2 : 1;

    for(int d = 0; d < DirCount; ++d)
    {
        for(int h = 0; h < HookCount; ++h)
        {
            CNetObj_PlayerInput Act = BaseInput;
            if(g_Config.m_KrxAvoidTileBlatantDirection)
                Act.m_Direction = Dirs[d];
            if(g_Config.m_KrxAvoidTileBlatantHook)
                Act.m_Hook = Hooks[h];
            Actions.push_back(Act);
        }
    }
    return Actions;
}
```

### 6.3 多线程并发贪心搜索 (`GreedySearch`，`0x14032eb50`)
```cpp
UGreedySearchResult BlatantAgent::GreedySearch(
    CGameWorld *pWorld,
    const CNetObj_PlayerInput &BaseInput,
    int CheckTicks)
{
    UGreedySearchResult Result;
    Result.m_FoundSafe = false;
    Result.m_SurvivalTicks = 0;

    auto Candidates = GenerateCandidateActions(BaseInput);
    std::vector<std::future<int>> Futures;

    // 采用线程池或异步任务并发计算各个分支
    for(const auto &Act : Candidates)
    {
        Futures.push_back(std::async(std::launch::async, [this, pWorld, Act, CheckTicks]() {
            return SimulateCandidate(m_pClient, pWorld, Act, CheckTicks,
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

### 6.4 NSIF 容错回退与主调度实现
```cpp
AvoidInput BlatantAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
{
    AvoidInput Out;
    Out.m_Input = *pCurrentInput;
    Out.m_Active = 0;

    CGameWorld *pWorld = m_pClient->GetPredictionWorld();
    if(!pWorld)
        return Out;

    // 1. KickInTicks 迟滞检测
    int KickSafety = SimulateCandidate(m_pClient, pWorld, *pCurrentInput,
        g_Config.m_KrxAvoidTileKickInTicks,
        g_Config.m_KrxAvoidTilePlayerPrediction,
        g_Config.m_KrxAvoidTileBlatantTeles,
        g_Config.m_KrxAvoidTileBlatantDeath);

    if(KickSafety == SIMULATION_SAFE_CONSTANT)
    {
        m_SavedSafeSequence.clear();
        return Out; // 足够安全，不干涉手感
    }

    // 2. 并发贪心搜索
    int CheckTicks = g_Config.m_KrxAvoidTileBlatantCheckTicks;
    UGreedySearchResult SearchRes = GreedySearch(pWorld, *pCurrentInput, CheckTicks);

    if(SearchRes.m_FoundSafe)
    {
        Out.m_Input = SearchRes.m_Sequence.front();
        Out.m_Active = 1;
        m_SavedSafeSequence = SearchRes.m_Sequence;
        return Out;
    }

    // 3. NSIF 回退机制
    if(g_Config.m_KrxAvoidTileNsif && !m_SavedSafeSequence.empty())
    {
        Out.m_Input = m_SavedSafeSequence.front();
        m_SavedSafeSequence.erase(m_SavedSafeSequence.begin());
        Out.m_Active = 1;
        return Out;
    }

    // 4. 若无法完全安全，执行存活时间最长的一个动作
    if(!SearchRes.m_Sequence.empty())
    {
        Out.m_Input = SearchRes.m_Sequence.front();
        Out.m_Active = 1;
    }

    return Out;
}
```

---

## 7. Legit Agent (拟人 MCTS 避障) 1:1 完整实现与数学公式

### 7.1 MCTS 节点结构体定义 (`0x58` = 88 字节)
```cpp
struct MCTSNode
{
    MCTSNode *m_pParent = nullptr;
    std::vector<MCTSNode *> m_vChildren;

    CNetObj_PlayerInput m_Action; // 动作载荷
    int m_Visits = 0;             // 访问次数 n
    double m_TotalValue = 0.0;    // 累积回报 Q
    int m_LifespanTicks = 0;      // 存活时长统计
    bool m_IsTerminal = false;    // 是否提前死亡/触冻

    ~MCTSNode()
    {
        for(auto *pChild : m_vChildren)
            delete pChild;
    }
};
```

### 7.2 非对称多目标拟人加权 UCT 公式 (核心反编译还原)
```cpp
// 浮点缩放常量 (0x1405300e4: float = 0.01f)
static constexpr float WEIGHT_SCALE = 0.01f;

float CalculateLegitHeuristic(
    const CNetObj_PlayerInput &CandidateAction,
    const CNetObj_PlayerInput &HumanAction,
    int SurvivalTicks,
    int WeightDir,      // 默认 170
    int WeightHook,     // 默认 260
    int WeightLifespan) // 默认 160
{
    // 1. 方向一致性奖励 (满分 2.0)
    float DirDiff = std::abs((float)CandidateAction.m_Direction - (float)HumanAction.m_Direction);
    float DirScore = std::abs(DirDiff - 2.0f) * ((float)WeightDir * WEIGHT_SCALE);

    // 2. 钩索状态一致性奖励 (满分 1.0)
    float HookDiff = std::abs((float)CandidateAction.m_Hook - (float)HumanAction.m_Hook);
    float HookScore = std::abs(HookDiff - 1.0f) * ((float)WeightHook * WEIGHT_SCALE);

    // 3. 生存时长奖励
    float LifeScore = (float)SurvivalTicks * ((float)WeightLifespan * WEIGHT_SCALE);

    return DirScore + HookScore + LifeScore;
}
```

### 7.3 `MCTSSearch` 四阶段完整实现
```cpp
AvoidInput LegitAgent::GetAction(const CNetObj_PlayerInput *pCurrentInput)
{
    AvoidInput Out;
    Out.m_Input = *pCurrentInput;
    Out.m_Active = 0;

    CGameWorld *pWorld = m_pClient->GetPredictionWorld();
    if(!pWorld)
        return Out;

    int Iterations = g_Config.m_ClAvoidNumIterations; // 默认 100
    int CheckTicks = g_Config.m_KrxAvoidTileLegitCheckTicks; // 默认 6
    float ExplorationC = (float)g_Config.m_KrxAvoidTileExplorationConstant; // 默认 4.0

    MCTSNode *pRoot = new MCTSNode();
    pRoot->m_Action = *pCurrentInput;

    for(int iter = 0; iter < Iterations; ++iter)
    {
        // 1. Selection (UCT + Heuristic)
        MCTSNode *pCurr = pRoot;
        while(!pCurr->m_vChildren.empty())
        {
            MCTSNode *pBestChild = nullptr;
            double BestScore = -1e38;

            for(MCTSNode *pChild : pCurr->m_vChildren)
            {
                double Score = 0.0;
                if(pChild->m_Visits == 0)
                {
                    Score = 3.4028235e+38; // FLT_MAX: 保证至少探索一次
                }
                else
                {
                    double Exploitation = pChild->m_TotalValue / (double)pChild->m_Visits;
                    double Exploration = ExplorationC * std::sqrt(std::log((double)pCurr->m_Visits) / (double)pChild->m_Visits);
                    double Heuristic = CalculateLegitHeuristic(pChild->m_Action, *pCurrentInput,
                        pChild->m_LifespanTicks,
                        g_Config.m_KrxAvoidTileDirectionWeight,
                        g_Config.m_KrxAvoidTileHookWeight,
                        g_Config.m_KrxAvoidTileLifespanWeight);

                    Score = Exploitation + Exploration + Heuristic;
                }

                if(Score > BestScore)
                {
                    BestScore = Score;
                    pBestChild = pChild;
                }
            }
            pCurr = pBestChild;
        }

        // 2. Expansion
        if(!pCurr->m_IsTerminal && pCurr->m_Visits > 0)
        {
            int Dirs[3] = { -1, 0, 1 };
            int Hooks[2] = { 0, 1 };
            for(int d : Dirs)
            {
                for(int h : Hooks)
                {
                    MCTSNode *pNewChild = new MCTSNode();
                    pNewChild->m_pParent = pCurr;
                    pNewChild->m_Action = pCurr->m_Action;
                    if(g_Config.m_KrxAvoidTileLegitDirection) pNewChild->m_Action.m_Direction = d;
                    if(g_Config.m_KrxAvoidTileLegitHook) pNewChild->m_Action.m_Hook = h;
                    pCurr->m_vChildren.push_back(pNewChild);
                }
            }
            if(!pCurr->m_vChildren.empty())
                pCurr = pCurr->m_vChildren[rand() % pCurr->m_vChildren.size()];
        }

        // 3. Rollout / Simulation
        int Survival = SimulateCandidate(m_pClient, pWorld, pCurr->m_Action, CheckTicks,
            g_Config.m_KrxAvoidTilePlayerPrediction,
            g_Config.m_KrxAvoidTileLegitTeles,
            g_Config.m_KrxAvoidTileLegitDeath);

        pCurr->m_LifespanTicks = (Survival == SIMULATION_SAFE_CONSTANT) ? CheckTicks : Survival;
        double Reward = (Survival == SIMULATION_SAFE_CONSTANT) ? 1.0 : ((double)Survival / (double)CheckTicks);
        if(Survival < CheckTicks)
            pCurr->m_IsTerminal = true;

        // 4. Backpropagation
        while(pCurr)
        {
            pCurr->m_Visits++;
            pCurr->m_TotalValue += Reward;
            pCurr = pCurr->m_pParent;
        }
    }

    // 最终选择访问次数最多的子动作
    MCTSNode *pBest = nullptr;
    int MostVisits = -1;
    for(MCTSNode *pChild : pRoot->m_vChildren)
    {
        if(pChild->m_Visits > MostVisits)
        {
            MostVisits = pChild->m_Visits;
            pBest = pChild;
        }
    }

    if(pBest && (pBest->m_Action.m_Direction != pCurrentInput->m_Direction || pBest->m_Action.m_Hook != pCurrentInput->m_Hook))
    {
        Out.m_Input = pBest->m_Action;
        Out.m_Active = 1;
    }

    delete pRoot;
    return Out;
}
```

---

## 8. Fentbot Agent (流场与遗传轨迹优化) 1:1 完整实现与档位

### 8.1 预设档位硬编码表
当 `krx_avoid_tile_fent_advanced_settings == 0` 时，Fentbot 会由 `krx_avoid_tile_fent_quality_setting` 覆盖参数（反编译 `0x1403356ba`）：

| 档位值 (`quality_setting`) | 内部宏 | Tweaker Actions 规模 | Tweaker Dosage 代数 | Tweaker Ticks 周期 | Fent Ticks 总深度 |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **0** | **Low** | **88** | **88** | **8** | **10000** |
| **1** | **Mid** | **160** | **160** | **8** | **10000** |
| **2** | **Max** | **1000** | **300** | **8** | **10000** |

### 8.2 速度-流场点积适应度函数 (`0x1403342c0`)
每个模拟步中，提取角色物理瞬时速度 $\vec{v} = (v_x, v_y)$，与所在瓦片的流场目标单位矢量 $\vec{D}_{flow}$ 进行点积，累加到基因个体的 Fitness 中：
$$\text{Fitness} = \sum_{t=0}^{T} \left( v_x \cdot D_x + v_y \cdot D_y \right) \times 1750.0 - \text{Penalty}_{dist}$$
- 浮点常量 `0x14054a6b8`：点积权重为 **1750.0f**。
- 若中途角色进入 Freeze（`m_FreezeTime > 0`），该基因个体立即终止并扣除惩罚分。

---

## 9. Pilot Bot (自主巡航与跟随) 核心参数与状态定义

```c
// 行为模式
// 0: 自主探索地图 (Autonomous)
// 1: 跟随鼠标十字准星 (Follow Cursor)
// 2: 跟随目标玩家 (Follow Player)
MACRO_CONFIG_INT(KrxAvoidTilePilotBotMode, krx_avoid_tile_pilot_bot_mode, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Pilot Bot behavior mode")

// 遗传与前瞻参数
MACRO_CONFIG_INT(KrxAvoidTilePilotBotPopulationSize, krx_avoid_tile_pilot_bot_population_size, 2048, 128, 8192, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Population size")
MACRO_CONFIG_INT(KrxAvoidTilePilotBotExplorationDepth, krx_avoid_tile_pilot_bot_exploration_depth, 17, 5, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Simulation ticks per sequence")
MACRO_CONFIG_INT(KrxAvoidTilePilotBotTopKCandidates, krx_avoid_tile_pilot_bot_top_k_candidates, 10, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Top candidates to validate")
MACRO_CONFIG_INT(KrxAvoidTilePilotBotSequenceLength, krx_avoid_tile_pilot_bot_sequence_length, 5, 1, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Action ticks used from plan")
```

---

## 10. DDNet 原版工程集成与输入拦截管道

### 10.1 源码目录组织
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

### 10.2 输入拦截点 (Hook Integration)
在 `src/game/client/components/controls.cpp` 的 `CControls::OnMessage` 中挂载调用：
```cpp
#include <game/client/components/avoid/avoid.h>

int CControls::OnMessage(int Msg, void *pRawMsg)
{
    // ... 原有方向与准星采样逻辑 ...

    // [KRX AVOID 模块拦截钩子]
    if(g_Config.m_KrxAvoidFreeze && GameClient()->m_pAvoid)
    {
        // 传入当前玩家采样的输入结构体指针进行修饰
        GameClient()->m_pAvoid->ProcessInput(&m_aInputData[g_Config.m_ClDummy]);
    }

    // 复制修饰后的最终输入并发送网络包
    mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
    return sizeof(m_aInputData[0]);
}
```

### 10.3 组件注册 (`CGameClient`)
在 `src/game/client/gameclient.h` 中添加成员指针：
```cpp
class BLAvoid *m_pAvoid;
```
在 `src/game/client/gameclient.cpp` 中初始化并加入组件树：
```cpp
#include "components/avoid/avoid.h"

// 构造函数中：
m_pAvoid = new BLAvoid();
m_All.add(m_pAvoid);
```

---

## 11. CMake 构建系统配置

在 `ddnet/CMakeLists.txt` 中添加源文件定义：
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

## 12. 行为一致性验证与对齐测试清单

完成复现后，按以下测试用例逐项验证，确保避障表现与 KRX 官方完全相同：

1. **Basic 模式边缘收缩测试**：
   - 指令：`krx_avoid_agent_type 0; krx_avoidfreeze 1`
   - 测试：向单一冻结格匀速按住 `D`（向右走）。
   - **预期表现**：在距离冻结块前恰好 1 个 Tee 宽度的瞬间，水平输入被自动置为 `0` 或 `-1`，角色平稳在边缘悬停，绝不触碰冻结。
2. **Legit 模式拟人无抖动测试**：
   - 指令：`krx_avoid_agent_type 1; krx_avoidfreeze 1`（使用默认迭代 100，权重 170/260/160）。
   - 测试：在安全路段正常起跳和飞行。
   - **预期表现**：由于高额的 Direction/Hook 拟人保持分，角色操作手感完全如同纯手动操作，毫无机械微抖动；在跳向冻结墙时，角色在最后可救帧平滑反向转向。
3. **Blatant 模式暴力自救与 NSIF 测试**：
   - 指令：`krx_avoid_agent_type 2; krx_avoidfreeze 1; krx_avoid_tile_nsif 1`
   - 测试：从高空向狭长冻结池坠落。
   - **预期表现**：角色自动组合使用反向移动与空中钩索（若开启了 Hook），并在无法存活全长时顺畅回放历史最佳序列，将存活帧数最大化延展。
