# BestClient Avoid (避障辅助) 模块全量技术与交接文档

> **文档版本**: 2.0.0  
> **适用代码分支**: `feature/tas`  
> **面向对象**: 后续维护开发者、算法工程师与 AI Agent  
> **更新时间**: 2026-10-05  

---

## 1. 模块全貌与交付目标

本模块为 BestClient 中的 **Avoid（避障 / Gores Bot）** 辅助系统，完全对齐参考客户端（KRX）技术规范（见 `docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md` 与 `docs/avoid/*.md`）。

### 1.1 核心特性与交付成果
1. **五大 Agent 算法全量落地**：
   - **基础 (Basic)**：轻量级方向制动与反转、图块角部探针、反应时间窗口控制。
   - **拟真 (Legit)**：UCT MCTS 模拟搜索、松钩前瞻判定、跳跃节流保护、其他玩家影子核心互撞预测、解冻前瞻。
   - **激进 (Blatant)**：内置射线瞄准（Auto Aim / Aim Assist）、Track Point 追踪记忆、Safe Aim Tracking、Auto Drag 队友救援、NSIF 跨 tick 记忆。
   - **Fentbot**：动态流场隧道寻路（Flowfield BFS / Tunnels）、Fent Ticks 前瞻计算、输入微调器（Input Tweaker: Dosage / Inputs / Ticks）。
   - **Pilot**：遗传进化算法（Genetic Algorithm / Evolution Strategy）、种群演化、探索深度、Top-K 挑选、动作序列模拟评分。
2. **全部 33 项 `bc_avoid_*` 参数完全生效**：涵盖辅助界面 5 个模块所对应的右侧专属参数面板（基本、图块、辅助、调参、优先级、瞄准、自救、寻路、进化等）。
3. **UI 风格与 HUD 完整保留**：保持“辅助模块”界面设计、状态指示器及拖拽式 HUD 仪表盘无任何破坏。
4. **威胁可视化崩溃修复**：彻底定位并修复了开启“威胁可视化”（Threat overlay）时在 Vulkan 渲染后端下发生的段错误（Signal 11），并完成了引擎层与渲染层的双重防御加固。

---

## 2. 系统架构与模块划分

```
┌─────────────────────────────────────────────────────────────┐
│                    游戏主循环 (GameClient)                    │
│                                                             │
│   OnSnapInput() (强制 50Hz)                 OnRender()       │
└───────────────┬─────────────────────────────┬───────────────┘
                │                             │
                ▼                             ▼
┌─────────────────────────────────────────────────────────────┐
│                       CAvoid (avoid.cpp)                    │
│  - 输入管线拦截 (ApplyInput)           - HUD 模块渲染 (仪表盘) │
│  - 状态机与遥测 (m_Telemetry)          - 威胁可视化 (世界叠加层)│
│  - Agent 分发器 (m_apAgents[5])        - 控制台指令绑定        │
└───────────────┬─────────────────────────────────────────────┘
                │
                ▼ (EvaluateBestPlan / GetAction)
┌─────────────────────────────────────────────────────────────┐
│                 IAgent 基类 (avoid_engine.h)                 │
│                                                             │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────┐ │
│  │ CBasicAgent  │ │ CLegitAgent  │ │    CBlatantAgent     │ │
│  └──────────────┘ └──────────────┘ └──────────────────────┘ │
│  ┌──────────────┐ ┌──────────────┐                          │
│  │CFentbotAgent │ │ CPilotAgent  │                          │
│  └──────────────┘ └──────────────┘                          │
└───────────────┬─────────────────────────────────────────────┘
                │
                ▼ (调用前向模拟沙箱与物理环境)
┌─────────────────────────────────────────────────────────────┐
│              前向物理沙箱 (Avoid 核心计算层)                  │
│  - CCollision / ClassifyPoint() (图块危险度分类)             │
│  - SEnvironment (私有 CWorldCore + 最多 8 名影子玩家快照)     │
│  - CCharacterCore::Tick() 物理推演                           │
│  - 碰撞射线检测 (IntersectLineTeleHook)                      │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 关键源文件说明

| 文件路径 | 职责与内容 |
| :--- | :--- |
| `src/game/client/components/bestclient/avoid.h` | `CAvoid` 客户端组件声明、UI 渲染接口、遥测结构体 |
| `src/game/client/components/bestclient/avoid.cpp` | 组件生命周期、输入挂点处理、HUD 与世界叠加层绘制、Agent 实例生命周期管理 |
| `src/game/client/components/bestclient/avoid_engine.h` | 避障数据契约（`SSettings`, `SContext`, `AvoidInput`）、`IAgent` 抽象基类、5 类 Agent 定义、物理沙箱接口 |
| `src/game/client/components/bestclient/avoid_engine.cpp` | 5 个 Agent 的核心算法实现、`ScanThreat` 危险扫描、流场寻路、遗传进化策略 |
| `src/game/client/components/bestclient/menus_avoid.cpp` | “辅助模块”菜单 UI，顶部状态条、左栏模式切换、右栏 5 模式参数面板 |
| `src/engine/client/backend/vulkan/backend_vulkan.cpp` | Vulkan 渲染后端管线绑定与保护 |

---

## 3. 五大模式算法原理与实现细节

### 3.1 基础模式 (Basic)
- **定位**：低开销、纯方向性制动。专为轻量机器或仅需紧急刹车的玩家设计。
- **算法流程**：
  1. **反应时间窗口 (`kick_in_ticks`)**：通过前向模拟玩家当前输入，若玩家存活帧数大于 `kick_in_ticks`，代理不干预。
  2. **三向扫描**：当即将进入危险时，推演 `Direction ∈ {-1, 0, 1}`（保持、松开、反向）。
  3. **角部探针**：使用 `HAZARD_CORNER_PROBE` 在玩家碰撞盒四个角点进行死亡图块与激光穿透检测。
  4. **方向与生命权重**：按 `bc_avoid_direction_weight` 与 `bc_avoid_life_weight` 综合选择最安全的方向输出，绝不主动改动跳跃、钩子或准星。

### 3.2 拟真模式 (Legit)
- **定位**：高拟真度防暴毙。动作自然，符合人类操作习惯。
- **算法流程**：
  1. **候选动作空间**：`方向 (3) × 跳跃 (2) × 钩子 (3)` 共 18 种组合，玩家当前输入排在第一位，平局时严格保留玩家意志。
  2. **UCT (Upper Confidence bounds applied to Trees) MCTS 搜索**：
     - `bc_avoid_quality` 控制搜索迭代深度与次数；
     - `bc_avoid_randomness` 控制探索常数及随机 rollout 概率；
     - 本地 PRNG 确保单 tick 内确定性可复现。
  3. **提前松钩前瞻**：若玩家当前正钩向危险物（如死亡墙/冰冻区域），以松钩后的推演序列能否存活满 `check_ticks` 作为脱钩判定依据。
  4. **跳跃节流保护**：钩索飞行或挂墙状态下严禁触发跳跃；仅在玩家自身输入存活时间 $\le 6$ 帧时，才允许跳跃救场。
  5. **其他玩家交互预测 (`bc_avoid_player_prediction`)**：
     - 在私有 `CWorldCore` 沙箱中克隆周围最多 8 个玩家影子快照；
     - 包含碰撞推挤（`TickDeferred`）与钩索拉扯模拟。
  6. **解冻前瞻 (`bc_avoid_unfreeze_ticks`)**：若附近存在解冻块且玩家处于冰冻状态，将解冻路径纳入优先评分。

### 3.3 激进模式 (Blatant)
- **定位**：极致求生。在绝境中接管准星寻找可勾支点，完成自动救场。
- **算法流程**：
  1. **内置射线瞄准 (Aimbot)**：
     - 在准星周围 `[准星 ± FOV/2]` 扇形区间内，按 `aimbot_segments` 步长发射钩索射线；
     - 利用引擎级 `CCollision::IntersectLineTeleHook()` 判断表面可勾性；
     - 模式 0 (Auto Aim)：选择仿真存活评分最高的可勾点；模式 1 (Aim Assist)：选择离玩家准星最近且能救命的可勾点。
  2. **Track Point 记忆追踪**：
     - 连续记录上一 tick 锁定的安全支点，避免准星在多个支点间剧烈抖动；
     - 在 `OnReset()` 或脱离危险后自动清空。
  3. **Safe Aim Tracking**：优先保持已验证安全的瞄准方向，在连续跳跃荡摆中维持稳定抓钩。
  4. **Auto Drag 队友救援**：检测范围内队友（依赖玩家预测），在无墙可勾时自动瞄准拉扯队友借力。
  5. **NSIF (No Safe Input Fallback) 跨 tick 状态恢复**：
     - 当没有任何输入组合能完全存活时，回放已知最安全的第一步序列，争取多存活帧数。

### 3.4 Fentbot 模式
- **定位**：基于空间连通性与流场的避障与隧道寻路导航。
- **算法流程**：
  1. **流场与隧道分析 (Flowfield & Tunnels)**：
     - `CalculateFlowField()` 以玩家当前坐标为起点，向周围无害图块进行广度优先搜索（BFS），计算安全势能场；
     - 识别地形中的安全通道（Tunnels），排斥危险图块（死亡块、冰冻块、深水等）。
  2. **Fent Ticks 前瞻计算 (`bc_avoid_fent_ticks`)**：
     - 结合流场梯度的最陡下降方向推导长周期的移动规划。
  3. **输入微调器 (Input Tweaker)**：
     - `bc_avoid_tweaker_dosage`：调节微调力度与权重；
     - `bc_avoid_tweaker_inputs` / `bc_avoid_tweaker_ticks`：在计划路线上注入微小抖动，避免在狭长隧道内卡死于拐角。
  4. **可视化接口支持**：支持路径线条和流场网格调试绘制。

### 3.5 Pilot 模式
- **定位**：基于群体演化的长期轨迹搜索（Genetic Algorithm / Evolution Strategy）。
- **算法流程**：
  1. **种群初始化**：生成规模为 `bc_avoid_population_size` 的候选动作序列，每条个体包含长度为 `bc_avoid_sequence_length` 的输入指令帧。
  2. **适应度评估 (Fitness Evaluation)**：
     - 在物理模拟沙箱中逐帧推演个体序列；
     - 适应度得分 = 存活帧数 + 终点离危险源的距离 - 输入剧烈变化惩罚。
  3. **进化与优选 (Selection & Crossover)**：
     - 保留 Top-K（`bc_avoid_top_k`）最优个体作为精英；
     - 对精英序列进行多点交叉与随机变异。
  4. **动作提取**：输出最优个体序列的第一帧动作作为当前 tick 的最终执行输入。

---

## 4. 33 项配置参数全量映射表

所有 33 项 `bc_avoid_*` 参数均定义于 `src/engine/shared/config_variables_bestclient.h`，并通过 `menus_avoid.cpp` 进行双向绑定与实时持久化：

| 参数名称 | 默认值 | 范围 | 生效 Agent 模式 | 功能说明与实现映射 |
| :--- | :--- | :--- | :--- | :--- |
| `bc_avoid_enabled` | 0 | 0/1 | 全部 | Avoid 辅助模块总开关 |
| `bc_avoid_agent` | 0 | 0~4 | 全部 | 当前模式：0=Basic, 1=Legit, 2=Blatant, 3=Fentbot, 4=Pilot |
| `bc_avoid_show_hud` | 1 | 0/1 | 全部 | 游戏内 HUD 仪表盘显示开关 |
| `bc_avoid_show_visuals` | 0 | 0/1 | 全部 | 世界空间威胁可视化（椭圆环、危险高亮、瞄准线） |
| `bc_avoid_sensing_radius` | 2 | 1~32 | 全部 | 感知半径（单位：半格，即 0.5 图块） |
| `bc_avoid_check_ticks` | 24 | 1~100 | 全部 | 前向推演检查帧数（前瞻窗口深度） |
| `bc_avoid_kick_in_ticks` | 20 | 0~100 | 全部 | 介入容忍帧数（玩家存活帧小于此时才接管） |
| `bc_avoid_direction_assist` | 1 | 0/1 | 全部 | 方向键接管许可（允许改写 `m_Direction`） |
| `bc_avoid_direction_weight` | 50 | 0~100 | 全部 | 保持玩家原始行进方向的偏好权重 |
| `bc_avoid_life_weight` | 80 | 0~100 | 全部 | 延长存活帧数的评分权重 |
| `bc_avoid_nsif` | 1 | 0/1 | 全部 | 无安全输入兜底开关 (No Safe Input Fallback) |
| `bc_avoid_afk_protect` | 0 | 0/1 | 全部 | 挂机防暴毙保护（静止无输入时检测危险） |
| `bc_avoid_afk_time` | 15 | 1~60 | 全部 | 判定为 AFK 的静止等待秒数 |
| `bc_avoid_tile_death` | 1 | 0/1 | 全部 | 启用对死亡图块（Spike / Laser）的避障 |
| `bc_avoid_tile_freeze` | 1 | 0/1 | 全部 | 启用对冰冻图块（Freeze）的避障 |
| `bc_avoid_tile_unfreeze` | 0 | 0/1 | 全部 | 启用对解冻图块的感知与主动引导 |
| `bc_avoid_tile_tele` | 0 | 0/1 | 全部 | 启用对传送图块（Teleport）的避障 |
| `bc_avoid_unfreeze_ticks` | 10 | 1~50 | 全部 | 解冻图块专用搜索深度 |
| `bc_avoid_hook_assist` | 1 | 0/1 | Legit, Blatant | 允许代理自动操作钩索（出钩/松钩） |
| `bc_avoid_hook_weight` | 50 | 0~100 | Legit, Blatant | 钩索安全性与连贯性评估权重 |
| `bc_avoid_quality` | 24 | 1~200 | Legit, Blatant | UCT 搜索迭代轮数与模拟质量 |
| `bc_avoid_randomness` | 20 | 0~100 | Legit, Blatant | UCT 探索随机性常数 |
| `bc_avoid_player_prediction` | 0 | 0/1 | Legit, Blatant | 开启多玩家影子实体物理交互预测 |
| `bc_avoid_aimbot` | 0 | 0/1 | Blatant | 允许激进模式接管准星（Aimbot） |
| `bc_avoid_aimbot_mode` | 0 | 0/1 | Blatant | 瞄准策略：0=Auto Aim (最安全), 1=Aim Assist (最贴近) |
| `bc_avoid_aimbot_segments` | 48 | 8~128 | Blatant | FOV 射线采样密度（分段数） |
| `bc_avoid_aimbot_fov` | 120 | 30~360 | Blatant | 瞄准搜索扇形视野角度（度数） |
| `bc_avoid_track_point` | 0 | 0/1 | Blatant | 锁定上一个安全挂钩点（防准星晃动） |
| `bc_avoid_safe_aim_tracking` | 0 | 0/1 | Blatant | 安全准星锁定追踪 |
| `bc_avoid_auto_drag` | 0 | 0/1 | Blatant | 绝境下拉扯队友借力自救 |
| `bc_avoid_fent_ticks` | 20 | 1~60 | Fentbot | Fentbot 模式流场前瞻步数 |
| `bc_avoid_tweaker_dosage` | 50 | 0~100 | Fentbot | 输入微调器介入剂量 (Dosage) |
| `bc_avoid_tweaker_inputs` | 1 | 0/1 | Fentbot | 微调器输入扰动开关 |
| `bc_avoid_tweaker_ticks` | 5 | 1~20 | Fentbot | 微调器动作注入周期帧长 |
| `bc_avoid_population_size` | 30 | 5~100 | Pilot | Pilot 遗传算法种群个体规模 |
| `bc_avoid_exploration_depth`| 15 | 1~50 | Pilot | Pilot 基因演化探索深度 |
| `bc_avoid_sequence_length` | 20 | 1~60 | Pilot | 候选个体控制序列帧数 |
| `bc_avoid_top_k` | 5 | 1~20 | Pilot | 演化淘汰每代保留的精英个体数 (Top-K) |

---

## 5. 威胁可视化（Threat Overlay）崩溃根因与修复

### 5.1 崩溃现象
在游戏内辅助模块设置中勾选 **“威胁可视化” (Threat overlay / `bc_avoid_show_visuals`)** 时，客户端立即闪退（Crash to Desktop），Linux 下系统产生 Signal 11 (SIGSEGV) 段错误。

### 5.2 核心根因分析
通过提取的系统 Coredump 日志：
```
#0 0x00007f582d381c1d (libnvidia-glcore.so + 0xd81c1d)
#1 0x00007f582d3adff7 (libnvidia-glcore.so + 0xdadff7)
#2 0x00005569f28edc53 CCommandProcessorFragment_Vulkan::BindPipeline
```
1. **Vulkan 缺乏带纹理的线条着色管线**：
   DDNet 在 Vulkan 初始化时，仅对无纹理线条创建了着色管线（`HasSampler = false, IsLinePipe = true`）。
   当用户在含有 UI 按钮/材质绑定的菜单界面勾选选项时，图形状态机中仍残留着先前控件的纹理（`m_State.m_Texture >= 0`）。
2. **渲染调用前未重置纹理状态**：
   旧的 `CAvoid::RenderWorldOverlay()` 直接调用 `Graphics()->LinesBegin()`。
   Vulkan 后端发现图元为 `LINES` 且 `IsTextured = true`，在 `m_StandardLinePipeline` 中检索带有纹理的线条管线，返回了空句柄 `VK_NULL_HANDLE`。驱动程序在 `vkCmdBindPipeline` 中直接解引用空指针崩溃。
3. **几何计算越界与除零隐患**：
   地图瓦片循环未对负坐标及超出地图宽高的索引做 clamp；零向量在进行 `normalize()` 时可能导致无效浮点数。

### 5.3 双重防御性修复实现

#### 修复 1：`src/game/client/components/bestclient/avoid.cpp`
- **指针及地图有效性拦截**：进入渲染前检查 `if(!Collision() || Collision()->GetWidth() <= 0) return;`。
- **瓦片边界收敛**：
  ```cpp
  const int MinTileX = std::clamp(CenterX - ScanX, 0, Collision()->GetWidth() - 1);
  const int MaxTileX = std::clamp(CenterX + ScanX, 0, Collision()->GetWidth() - 1);
  const int MinTileY = std::clamp(CenterY - ScanY, 0, Collision()->GetHeight() - 1);
  const int MaxTileY = std::clamp(CenterY + ScanY, 0, Collision()->GetHeight() - 1);
  ```
- **纯色图元纹理清理**：在每次调用 `QuadsBegin()` 与 `LinesBegin()` 之前，显式调用 `Graphics()->TextureClear()`，并在 `LinesBegin()` 之后设置图元颜色。
- **向量模长守卫**：对 Blatant 追踪向量与瞄准向量检查 `length > 0.001f`，防止零向量进入几何计算。
- **现场还原**：绘制结束后清理纹理并恢复原屏幕视口矩阵与白色着色。

#### 修复 2：`src/engine/client/backend/vulkan/backend_vulkan.cpp`
- **管线查找强制去纹理**：
  ```cpp
  VkPipeline &GetStandardPipe(bool IsLineGeometry, bool IsTextured, ...) {
      if(IsLineGeometry)
          return GetPipeline(m_StandardLinePipeline, false, BlendModeIndex, DynamicIndex);
      ...
  }
  ```
- **指令执行层去纹理**：在 `RenderStandard` 中加入 `if(IsLineGeometry) IsTextured = false;`。
- **空句柄安全门禁**：在 `BindPipeline` 中加入：
  ```cpp
  if(BindingPipe == VK_NULL_HANDLE)
      return;
  ```

---

## 6. 验证与回归自检指南

### 6.1 编译验证
```bash
# 增量编译游戏可执行文件
ninja -C build DDNet
```
确认无任何 error 且链接成功。

### 6.2 自动化测试与自检
```bash
# 执行 Avoid 专用全量契约与回归测试
./scripts/avoid_selfcheck.sh
```
预期输出：
```
[1/6] building
[2/6] bc_avoid_* config variables: ok: 33
[3/6] localization keys: ok: 120
[4/6] running regression suite: [  PASSED  ] 365 tests.
all checks passed
```

---

## 7. 后续维护与安全规范

1. **TAS 自动位移红线**：
   - 严禁在未获玩家显式授权的情况下主动朝任意方向行走（“代跑”）；
   - 避障的唯一目标是**阻止即时暴毙**；当玩家处于安全状态时，输出必须与玩家输入保持 bit-identical。
2. **50Hz 实时性与性能预算**：
   - Avoid 运行于 50Hz 物理 tick，每次决策耗时严格控制在 **1.0ms** 以内（预警上限 1.5ms）；
   - 在高负载（如地图内存在 8 名影子玩家 + 200 质量迭代）时，UCT 搜索及遗传算法内置了时间卫兵（Wall Clock Guard），超时立刻截断返回当前最优解。
3. **物理沙箱纯净性**：
   - 推演必须在克隆出来的 `CCharacterCore` / `CCollision` 上进行，严禁直接修改游戏实体的实时状态。
