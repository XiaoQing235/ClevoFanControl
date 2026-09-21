# Control Center 风扇控制权研究

研究日期：2026-09-21。针对当前机器和 `codex/fltk-rs-rewrite` 分支；代码基线 `57a12f768954c9b24198f568bc4dc90e87b8e4d8`。

历史引用说明：旧 MFC 工程及厂商二进制已从当前源码树移除；本文中的旧代码路径与行号对应上述基线提交，可用 `git show 57a12f7:<路径>` 查阅。

## 结论与推荐

原版没有比 Control Center（CC）更高的控制优先级，也没有把 CC 锁在控制器外。它调用厂商 DLL 写入目标占空比；部分控制路径会在后续采样发现差异时再写入，但这不等于禁止其他程序写入。

“换成 x64”“管理员运行”“直接访问 EC”解决的分别是架构、访问权限和通信路径问题，都不会自动产生独占控制权。严格阻止 CC 覆写，需要消除写入者、限制写入权限，或在其命令到达固件之前进行过滤。

建议按需求选择：

1. **允许替代 CC 后台：保留原厂 ACPI 驱动，停用实际执行控制的 CC 组件，由 Rust 后台成为唯一日常控制者。** 这是复杂度最低的产品路线；需要识别和补齐用户仍需要的热键、灯光、性能配置功能。
2. **允许 CC 部分硬件功能失效：优先验证设备实例 `Exclusive` 属性或设备 ACL，加一个持久持有设备句柄的 Rust 后台。** 可能不需要自写内核驱动，但隔离的是整个 Bridge，不能天然只隔离风扇。
3. **要求 CC 继续运行且保留非风扇功能：以设备专用 KMDF upper filter 为正式方案候选。** 在 `AcpiBridge.sys` 前解析和过滤风扇相关请求；用户态 DLL 拦截可作为先行实验。需要覆盖批量命令及实际使用的旁路。
4. **上述路径不满足需求时：继续评估替代功能驱动、ACPI 方法改写、EC 固件修改或独立硬件控制。** 这些路线可以改变控制边界，但成本明显高于普通软件重写，且本次没有完成机型适配或运行验证。

以上是源码、二进制静态分析及只读系统检查得到的方案结论。没有执行风扇写入、停止服务、修改设备属性、安装过滤驱动、刷写固件或测试 CC 对抗行为。不能把“Windows 支持该机制”写成“本机已经验证接管成功”。

## 1. 原版究竟怎样“接管”

代码入口位于 `ClevoFanControl/Core.cpp`：

| 位置 | 实际作用 |
| --- | --- |
| `Init()`，174 行 | 动态加载 `ClevoEcInfo.dll`，解析 `InitIo`、`SetFanDuty`、`SetFanDutyAuto` 和遥测函数 |
| `TimerCallback()`，441 行 | 触发工作循环；采样周期来自配置 |
| `Work()`，685 行 | 先 `Update()`；普通接管路径调用 `Control(config)`，取消接管则 `ResetFan()` |
| `Update()`，761 行 | 读取温度、当前占空比及可选转速 |
| `Control(config)`，843 行 | 根据温度、曲线和滞回计算目标；直接写入或交给平滑线程 |
| `ResetFan()`，955 行 | 对实际风扇调用 `SetFanDutyAuto(fanId)`，恢复固件自动模式 |
| `SetFanDuty()`，984 行 | 当前值与目标不同时，将百分比换算到 0–255，调用 DLL；第三风扇跟随 GPU 目标 |
| `SoftControlDuty()`，1024 行 | 逐步改变软件内的平滑占空比；只有软件内的值发生变化时才调用写入 |

需要修正“原版持续抢控制权”的说法：

- 普通非平滑路径会在后续采样发现占空比不同后尝试纠正，期间 CC 的写入仍可能生效。
- 平滑线程到达目标后，`changed` 为假；仅发生外部覆写，并不必然触发重新写入。
- 强制冷却分支也有基于内部目标值的写入条件，不能视为外部覆写检测器。
- `m_bTakeOverStatus` 是进程内状态；`m_csEcApi` 是本程序内的串行化；单实例 Mutex 只限制本软件重复启动。它们都不是 CC 认可的控制权协议。
- `THREAD_PRIORITY_ABOVE_NORMAL` 仅影响 CPU 调度，不会阻止 CC 的设备 I/O。

原版 `ClevoEcInfo.dll` 的静态反汇编显示：

```text
应用 SetFanDuty(fanId, duty)
  -> ClevoEcInfo.dll
  -> NTPort 提供的端口访问
  -> 等待 0x66 状态端口的输入缓冲区空闲
  -> 向 0x66 发送 0x99
  -> 向 0x62 发送 fanId，再发送 duty

恢复自动：0x99 -> 0xFF -> fanId
```

这里没有可见的所有者身份、持久租约或“拒绝下一位写入者”的逻辑。EC 内部对不同模式的优先级仍须以该机型固件或运行实验确认。

## 2. 本机证据

### 2.1 机器和实际运行组件

- 型号/主板：`Notebook NP5x_6x_7x_SNx`。
- BIOS：`1.07.27RTR6`。
- 正在运行：`FnKey.exe`，来自 `CLEVOCO.FnhotkeysandOSD_7.88.1.0_x64__6h6z29zh29qx0`。
- 已通过进程模块列表确认：该进程实际加载自身目录中的 `InsydeDCHU.dll`，不是仅在磁盘上发现同名文件。
- 应用包 Manifest 将 FnKey 声明为 `Windows.FullTrustApplication`，并请求 `runFullTrust`；不能因为它位于 WindowsApps 就把它当作受普通 UWP 沙箱限制的进程。
- `CCDCHUService` 正在运行，自动启动、LocalSystem；实际程序在 DriverStore 的 `acpibridge1.inf_amd64_0a24f05be94f40ec` 目录。
- `HKClipSvc` 也在运行；不能仅凭名字认定它是风扇写入者。
- 实际驱动：`C:\Windows\System32\drivers\AcpiBridge.sys`，版本 `1.0.0.9`，设备 `ACPI\CLV0001\1`，安装 INF 为 `oem77.inf`。
- `ACPI\CLV0002\1` 也存在；本机 DSDT 中的 DCHP 只含标识和状态方法，不能误称为已确认的第二套风扇命令桥。

`CCDCHUService.OnStart()` 会启动 FnKey，并建立 `DCHUSERVICE` 管道。服务程序集还包含 WMI/DCHU 调用代码。它不是一个已确认可单独摘除的“纯风扇服务”；停服务也不能假设已经存在的 FnKey 进程随之退出。

### 2.2 正在使用的 DLL/驱动

FnKey 中的 DLL 为 x64，导出包括：

```text
GetDCHU_Data_Integer
GetDCHU_Data_Buffer
SetDCHU_Data
SetDCHU_DataEx
ReadAppSettings
WriteAppSettings
```

观察到的开设备代码使用 `GENERIC_READ | GENERIC_WRITE` 和 `FILE_SHARE_READ | FILE_SHARE_WRITE`。普通命令经 `DeviceIoControl(0x322400)`，AppSettings 经 `0x32240C`。

`SetDCHU_DataEx` 也使用 `0x322400`，所以只 Hook `SetDCHU_Data` 会遗漏一个调用入口。读写共用 IOCTL，过滤策略必须看命令及载荷，不能仅按 IOCTL 编号、函数名称或句柄的读/写权限分类。

在实际驱动的设备创建路径中，观察到 `WdfDeviceCreate` 和设备接口注册；所查路径没有设置 `WdfDeviceInitSetExclusive`。安装 INF 未声明 `Exclusive`；设备属性查询没有返回一个显式的独占值。这些是静态/配置证据，尚未通过双句柄实验验证实际独占行为。

设备接口 GUID 为 `86994c74-ad43-4812-b7e7-0c420b5c5fd7`。驱动将请求转换为 ACPI 方法求值；这是通信桥，不是自动拥有最高优先级的风扇仲裁器。

### 2.3 FnKey 确实具有风扇写入代码

对实际安装的 FnKey 做静态 IL 分析，其混淆方法仍可按 metadata token 跟踪：

- `06000806` 把子命令写入四字节载荷的最高字节，再进入通用调用函数。
- `06000819` 分派到 `SetDCHU_Data` 或 `SetDCHU_DataEx`。
- `0600000A`、`0600000D`、`0600000E` 等方法存在 `121 / 子命令 1` 的风扇模式写入。
- 还有 `121 / 14` 的风扇偏移和 `121 / 34` 的默认配置恢复路径。

这是存在真实调用路径的证据，不代表已测得它每隔多少秒写一次，也不代表所有条件分支在本机都会执行。

## 3. 本机 ACPI 指令和过滤范围

使用 Windows `GetSystemFirmwareTable` 只读导出 DSDT，再用 ACPICA iASL `20260408` 反编译。没有调用这些 ACPI 方法。

本机 DSDT 中：

```text
InsydeDCHU.dll -> AcpiBridge.sys -> DCHU._DSM -> SCMD / CC30 / CPKG
WMI 方法入口                  -> WMI.WMBB -> SCMD / CC20 / CPKG
                                                    |
                                          EC.ECMD 或 EC 字段写入
```

`DCHU._DSM` 使用 UUID `93f224e4-fbdc-4bbf-add6-db71bdc0afad`。WMI 的 `_WDG` 包含方法 GUID `abbc0f6d-8ea1-11d1-00a0-c90629100000`。

本次查询 `root\WMI` 没有找到 `CLEVO_GET` 类。因此，**固件具有 WMI 入口**与**当前 Windows 中可通过该类调用**必须分开表述；没有证明当前 CC 正在使用 WMI 旁路。

| 命令/路径 | 本机静态行为 | 对隔离的影响 |
| --- | --- | --- |
| `0x68 / 104` | 经 EC `0xC1` 向四个风扇通道依次发送载荷中的四个字节 | 不可把它封装成只影响一个风扇、其余补零的接口 |
| `0x69 / 105` | 位 0–3 选择通道，发送 EC `0xC1, 0xFF, channel` | 是选定通道恢复自动的路径，不是已证实的“获取独占锁” |
| `0x79 / 121, sub=1` | 根据模式值发送 EC `0xD7` 子命令 | 必须纳入风扇模式保护 |
| `0x79, sub=0x0E` | 写入 EC `GFOF` | 风扇偏移路径 |
| `0x79, sub=0x22` | 参数 bit0 为 1 时发送 EC `0xD9` | 与应用的恢复默认路径对应 |
| `0x0E / 14` | `PK0E` 写 `F1T2/F1D2` 等曲线字段及转速相关字段 | 只拦占空比仍会漏掉曲线改写 |
| `0x02 / CPKG` | 批量包内包含四通道占空比和恢复自动分支 | 必须解析包内选择位及写入位，或明确拒绝相关混合包 |
| `0x04 / PK04` | 包含以输入字段组装 EC 命令的分支 | 不能笼统视为无副作用的读取包 |
| `0x6B + 0x75` | 设置索引，再写 EC 映射区域；覆盖命令字段时还会提交命令 | 通用写入路径需要单独约束 |
| `0x6A / 106` | 把参数两字节交给 EC `0xBA`；服务命名为 `SetECResumerTimer` | EC 内部计时、单位和失效行为尚不清楚，不能拿名字推断成锁或安全看门狗 |
| `0x79` 的其他子命令 | 还涉及性能、无线等功能 | 整体屏蔽 `121` 会伤及非风扇功能 |

`CPKG` 的占空比分支检查 `WQW2/WRW2` 的 bit `0x27`，取包内 `0xB6` 的 DWORD；恢复自动检查 bit `0x28`，取 `0xBA`。这只是静态分支定位，运行前还需核对包类型和编码，不能直接当已验证的发送格式。

这里看到的是事务串行化：`_DSM`/部分方法 Serialized，`ECMD` 使用 `PATM` Mutex。**事务锁只防止同时执行，不阻止另一程序在事务完成后修改状态。** 长期占住 Mutex、伪造固件刷写忙状态等做法会干扰其他 EC 事务，不适合用作风扇控制权机制。

本次 iASL 对外部方法解析有告警；以上结论限于已检查的本地方法体。没有完整逆向 EC 内部固件，不能排除固件自身的保护、超时或模式优先级。

## 4. 方案矩阵

“可强制阻止”均指覆盖的普通 CC 写入路径，不包含对抗一个主动修改系统配置的管理员，也不意味着禁止固件保护动作。

| 方案 | 能否阻止覆写 | CC 其他功能 | 判断 |
| --- | --- | --- | --- |
| 厂商手动模式/固定占空比 | 未发现排他保证 | 大体可保留，可能竞争 | 先验证实际共存行为；不要把模式当锁 |
| 停用/移除实际写入组件，保留驱动 | 写入者确实不再运行时可以 | 混合后台功能可能需重写 | 默认产品路线最简单 |
| CC 官方关闭风扇自动管理的设置 | 若确实存在且覆盖所有事件则可行 | 最好 | 本机静态检查未确认这样的完整开关 |
| 禁止风扇模块启动、选择性修改 CC 逻辑 | 覆盖完整调用点时可行 | 有机会保留 | FnKey 混合职责、版本升级和恢复流程增加维护成本 |
| 设备实例 Exclusive + 持久句柄 | 设备栈正确执行独占时可挡其他打开 | 整个 Bridge 相关功能受影响 | 有 Windows 官方机制，值得做低成本原型 |
| 设备 ACL + 独立服务身份 | 可禁止未授权主体打开设备 | 同样是整设备粒度 | 比抢启动顺序更明确；不能按 EXE 名自然区分 |
| DLL 代理/Detours 拦截 | 对被覆盖进程中的调用可行 | 可按语义保留 | 适合实验；需处理多份 DLL、Ex 导出、升级、加载与应用包限制 |
| AcpiBridge 设备 upper filter | 可在请求进入原驱动前强制拒绝 | 可做语义级保留 | 严格共存的主要正式候选，需开发和签名内核驱动 |
| 对实际 WMI 路径补充过滤 | 可补主 Bridge 之外的入口 | 取决于过滤粒度 | 仅在证实使用后增加；勿全局破坏 WMI |
| 替换 AcpiBridge 功能驱动 | 可内建权限和命令仲裁 | 要兼容事件、缓存和协议 | 工作量大于设备过滤，后备路线 |
| 让 CC 使用本软件生成的配置/曲线 | 减少策略冲突，不是强制禁止 | 较好 | 可作为需求折中；用户在 CC 中改模式仍可冲突 |
| PawnIO/自有 x64 端口驱动直通 EC | 单独使用不能 | 仍然竞争 | 是访问层替代方案，不是仲裁方案 |
| 禁用 CC 使用的 Bridge，自己改走独立 EC 通道 | 可切断被禁用设备上的 CC 通路 | 原厂相关控制功能失效 | 可与 PawnIO 等组合；必须检查 WMI/其他驱动旁路，不能同时还依赖被禁用的 Bridge |
| 将 CC 放入受限环境，硬件访问经自有代理 | 覆盖其所有硬件访问时可行 | 需代理保留功能 | 相当于重建一套中介协议，复杂度通常高于直接过滤 |
| ACPI 方法改写/表覆盖 | 可在覆盖的 AML 入口统一阻断旧写入路径 | 需逐方法保留 | 开发实验路线；本机完整表重编译、引导兼容均未验证 |
| 修改 EC 固件/适配开源 EC | 可从控制器内部定义优先级 | 需重新适配全部主板职责 | 真正改变最终执行规则，但不是通用 DLL 替换 |
| Hypervisor 截获 I/O/MMIO | 理论上可在覆盖路径实现 | 高复杂度 | 需处理多种访问、VBS/Hyper-V/SMM 边界，不适合当前产品 |
| 独立风扇控制硬件 | 可绕开原 EC 对风扇输出的支配 | 需硬件适配 | 最终替代路线，已超出纯软件重写 |
| 高频覆盖、提高进程优先级、开机先启动 | 不能保证；仍有竞争窗口 | 表面保留 | 不满足“禁止覆写”验收条件 |
| 挂起整个 CC 进程 | 挂起期间该进程不执行 | 其后台功能暂停 | 诊断用途；不等于可维护的选择性隔离 |
| 注册表只读、阻止配置文件保存 | 不能阻止已经发出的硬件命令 | 可能造成配置不一致 | 不能作为控制权边界 |

### 4.1 Exclusive 的具体实现边界

Windows 的 `DEVPKEY_Device_Exclusive / SPDRP_EXCLUSIVE` 可由管理员安装程序设置。它不同于单次 `CreateFile` 的共享参数；实际排他性必须落实到设备栈的命名设备对象。

候选实现：设备实例配置独占属性 → 重新建立设备栈使设置生效 → 自有后台获得并长期保留唯一句柄 → 所有 UI 操作通过后台提交。需要实测是否要求重启设备/系统、原厂驱动的内部打开是否受影响，以及 CC 打开失败后的行为。

现成 Insyde DLL 的调用会管理自己的打开/关闭过程，不能假设它会复用后台占住的句柄。采用这条路线时，可能需要在 Rust 中直接实现已核实的 Bridge IOCTL 封装，而不是继续原样调用 DLL。

实际 FnKey DLL 的 `SetDCHU_Data` 提供了重写通信层的静态依据：请求大小 `0x420`，输出缓冲区 `0x40C`，命令 IOCTL 为 `0x322400`。请求中 `+0x00` 是 UUID、`+0x10` 是 revision、`+0x14` 是 `_DSM`、`+0x18` 是命令号；`+0x1C` 的包长度字段为 `0x104`，后面是包含 256 字节缓冲区的参数编码。后续原型应先复现已确认的读取请求，再验证写入和返回值语义；这些字段不是任意版本 DLL 都通用的稳定公开 ABI。

仅靠“先打开”存在启动顺序竞争；若要确定主体，需结合独立服务身份和合适 ACL。普通用户与 CC 同一账户时，按用户授权无法区分两个应用；给 LocalSystem 一概放行也可能同时放行厂商服务。

即使成功独占 CLV0001，也只覆盖该设备入口，不自动封住 WMI、其他内核驱动或 EC 自身行为。

### 4.2 精确过滤的最小设计

```text
fltk-rs UI -> Rust 控制后台 -> 受限控制入口/受授权句柄
                                       |
CC 各进程 -> 原厂 DLL -> 设备过滤驱动 -> 原厂 AcpiBridge -> ACPI -> EC
                         |
                         +-- 非所有者的风扇改写请求：拒绝
                         +-- 已确认的非风扇操作：转发
```

- 首先做观察模式，确认实际命令、发起进程、触发事件和批量载荷；不能仅凭静态存在就认定持续执行。
- 对所有者写入和 CC 请求进行同一处串行化，切换接管状态前处理已在途请求，防止先放行后到达的旧请求覆写新目标。
- 策略按命令语义、子命令和包内字段执行。对未理解的可写包不能一边全放行，一边宣称“严格禁止风扇覆写”。
- 批量包可以拒绝整包，也可以在完全理解协议后移除风扇写入部分；后一种做法必须保持返回语义和其余字段。
- 调用者识别绑定受授权服务/文件对象，不仅比较进程名，也不依赖队列回调当时的线程属于谁。
- 明确 CC 被拒绝后是显示失败、重试还是退出。伪造成功会导致其界面与硬件状态不一致，不能未经验证默认采用。
- 应用退出、后台崩溃、句柄清理、睡眠恢复和驱动移除需要明确释放与恢复自动策略。不能只依赖 UI 正常退出时调用恢复函数。
- 这是一个设备专用过滤驱动，不应为了该任务 Hook 系统内核、修改 `Acpi.sys` 或对整个 ACPI/WMI 类做无差别阻断。

## 5. 发散探索得到的补充路线

### PawnIO 保留旧协议

公开 `LpcACPIEC.p` 提供对 `0x62/0x66` 的读写，并要求调用者使用 `Access_EC` Mutex 协调。理论上可以用它在 x64 中重写旧 DLL 的端口发送逻辑，摆脱 x86 `ClevoEcInfo.dll` 和 NTPort。

这只是可评估的协议迁移方案：旧 EC `0x99` 命令是否适用本机、端口事务与 ACPI 的协同、模块/驱动部署，都需要验证。该 Mutex 只约束参与者；没有证据表明 CC 或 Windows ACPI 执行器使用它，所以不能把它当跨系统锁。

### 与 CC 共享策略而非竞争

原厂 FanSpeedSetting 的 `SetFanMode` 会写固件命令，同时保存 AppSettings，再通知其他组件。本软件如果只写硬件、不维护 CC 读取的状态，后续恢复配置时更容易出现策略分歧。

如果接受使用固件支持的有限曲线点，可以研究让 CC 和本软件使用同一份配置。它可以减少非预期恢复，但不能阻止用户在 CC 中主动改模式，也不等价于当前软件任意曲线/进程预设的全部功能。

### ACPI 与 EC 固件

ACPI 表改写可以让既有厂商入口忽略受保护的风扇写入，并为自己的后台保留另外的执行路径。AML 本身并不知道 Windows 调用者的 PID，不能只增加一个“判断是哪个 EXE”的条件。还须覆盖 WMI、批量包和通用写入路径。

Windows 有官方开发用 ACPI table-load 机制，但不是普通应用的稳定部署接口。EC 固件则更深入：System76 提供公开 EC 源码可研究控制架构，但本次没有确认其可直接支持这块主板，不能跨型号刷入。

独立硬件控制还要确定风扇是 PWM 控制还是电压控制、供电和测速反馈等，不能只假设增加一根 PWM 线即可接管。

## 6. 下一阶段验证顺序

本节是后续计划，没有在本次研究中执行。

1. **先判明覆写来源。** 分别在 CC 写入组件不运行、正常运行、切换模式、接通/拔除电源、睡眠恢复等情形检查目标、模式、占空比和 RPM。区分 CC 命令、EC 自动恢复以及 RPM 正常变化；RPM 变化本身不证明寄存器被覆写。
2. **验证单一控制者。** 保留厂商驱动，让自己的 x64 后台完成读、设定、恢复自动。确认后台崩溃后的硬件行为，避免把未知的 `SetECResumerTimer` 当作已验证的恢复机制。
3. **决定 CC 保留边界。** 若不要求保留后台，停在替代路线；若可接受整桥隔离，验证 Exclusive/ACL；若必须保留其他控制功能，进入命令过滤。
4. **为过滤建立实际命令清单。** 除单独命令外，确认 CPKG、PK04、通用写入和配置恢复是否实际出现；检查当前未暴露的 WMI 类是否会因驱动/CC 更新改变。
5. **验证对抗和恢复。** CC 发起改变风扇的操作时，请求应在硬件执行前被阻止；非风扇功能仍可用；接管释放后恢复自动；CC 更新、服务重启、睡眠和后台退出不遗留错误控制状态。

验收“严格阻止覆写”不能只观察最终读数符合目标。至少要证明被阻止的 CC 请求没有到达受保护写入路径，同时核对硬件状态。

## 7. 证据定位和公开资料

本机临时研究目录：`C:\Users\Xiao Qing\AppData\Local\Temp\clevo-x64-dependency-research`。包含静态 IL、反汇编和 ACPI 导出；这些临时材料未作为项目依赖引入。

| 文件 | 定位 |
| --- | --- |
| `acpi/DSDT.dsl` | `WMBB` 103700；`ECMD` 104585；`DCHU` 106106；`PK0E` 107290；`CPKG` 107483；手动/自动分支 110633/110668；`0x79` 111066 |
| `FnKey-token.il.txt` | 按上文 metadata token 搜索，避免混淆方法名无法区分 |
| `ActualDCHU-0.asm.txt` | 实际 FnKey DLL；`CreateFileW` 调用 VA `0x18000237E` |
| `AcpiBridge.sys.asm.txt` | 实际 v1.0.0.9；设备创建路径从 VA `0x140006DF8` 开始 |
| `FanSpeedSetting-all.il.txt` | 官方应用包静态分析；`FAN.SetFanMode`、`FAN.SetFanInfo` 等 |

关键 SHA-256：

```text
AcpiBridge.sys 1.0.0.9:
5c01679117aab5629526dab211eb0d57b1035631cd498710d2450290dcf80f35
实际 FnKey.exe:
d9968396dda8080d112cfe011b4a73f834e3302da18feb4558a747b0387103af
实际 FnKey/InsydeDCHU.dll:
75a47020d3a9d052e94dcb4e3ad61fb69f20177dd46e20c9a20883d92b83981b
DriverStore/InsydeDCHU.dll:
22fecadff27f4bf08cb4a17fe455ae490107b409e55827210f821947d65a9d47
本机 DSDT.dat:
c7062c7817f081bbd38b51910cd5236f824c9810dec41801864a6b599c8f3b58
```

公开一手资料：

- [Windows 设备独占语义](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/specifying-exclusive-access-to-device-objects)
- [安装后修改设备属性，包括 SPDRP_EXCLUSIVE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/setting-device-object-registry-properties-after-installation)
- [DEVPKEY_Device_Exclusive](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-exclusive)
- [KMDF 设备访问权限](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/controlling-device-access-in-kmdf-drivers)
- [Windows 设备过滤驱动安装机制](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-a-filter-driver)
- [WDF DeviceIoControl 请求处理](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nc-wdfio-evt_wdf_io_queue_io_device_control)
- [Windows 内核驱动签名要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-requirements--windows-vista-and-later-)
- [Microsoft Detours](https://github.com/microsoft/Detours)
- [Microsoft ASL Compiler / ACPI 表覆盖](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/microsoft-asl-compiler)
- [PawnIO LpcACPIEC 源码](https://github.com/namazso/PawnIO.Modules/blob/master/LpcACPIEC.p)
- [PawnIO 模块集成与许可说明](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules)
- [System76 开源 EC](https://github.com/system76/ec)
- [TUXEDO Clevo 风扇控制源码](https://github.com/tuxedocomputers/tuxedo-drivers/blob/main/src/tuxedo_io/tuxedo_io.c)
- [clevo-thermald 的 NH5xAx 逆向记录](https://github.com/samoylenkodmitry/clevo-thermald/blob/main/docs/REVERSE-ENGINEERING.md)：另一机型的参照，不能代替本机验证。
- [RLECViewer](https://github.com/zuyan9/RLECViewer)：README 要求先卸载 Control Center，不能作为已经解决 CC 共存优先级的证据。

网上另有项目把 `SetFanAutoDuty(1)` 解释为取得手动控制；这与本机 DSDT 的通道自动恢复分支不一致。重写应以本机固件和验证结果为准，不复制跨机型的命令说明。
