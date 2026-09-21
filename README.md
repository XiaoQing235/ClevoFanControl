# ClevoFanControl

面向 蓝天 / Clevo 笔记本的 Windows x64 原生风扇控制工具，使用 **Rust + FLTK** 编写，提供可视化风扇曲线、实时监控和按进程切换的配置预设。

项目灵感来自 **myfancontrol**，但当前实现与其基本无关：界面、控制逻辑和硬件通信均已重新实现。新版通过已安装的厂商 ACPI 组件与 EC 通信，**不再使用或分发 `NTPortDrvSetup.exe`、`ClevoEcInfo.dll`、`NVGPU_DLL.dll`**，也不提供旧版 GPU 限频功能。

> 本项目由 **gpt-6-astra@medium** 开发。**Use at your own risk.** 风扇控制直接影响散热；请自行确认曲线和设备兼容性。本项目不保证所有 Clevo 机型可用，也不保证异常退出后能够恢复固件自动控制。

## 语言与技术栈

| 用途 | 技术 |
| --- | --- |
| 开发语言 | Rust，Edition 2024；工具链固定为 1.97.1 |
| UI 界面 | fltk-rs / fltk-sys 1.5.23，使用 `fltk-bundled` 预编译 FLTK 库 |
| 配置序列化 | Serde、serde_json |
| Windows 集成 | windows-sys：托盘、单实例、进程枚举、系统外观和电源事件等 |
| 厂商接口加载与校验 | libloading、sha2 |
| 并发与调度 | Rust 标准库线程与消息通道，独立风扇控制工作线程 |
| 构建与发布 | Cargo、winresource、MSVC / Windows SDK、GitHub Actions |

依赖版本以 [Cargo.lock](Cargo.lock) 为准，工具链见 [rust-toolchain.toml](rust-toolchain.toml)。

## 主要功能

- **可视化曲线编辑**：CPU / GPU 独立曲线，每条支持 2–16 个节点；支持拖动、双击插入、键盘微调和数字编辑。
- **控制策略**：阶梯或线性曲线、降温滞回、平滑调速；支持软件接管与归还固件自动控制。~~第三风扇使用 GPU 目标值~~。
- **一次性强制冷却**：以 95% 占空比运行，直到温度低于设定阈值后结束；优先于普通曲线控制，不保存为常驻开关。
- **实时监控**：显示温度、RPM、实际与目标占空比。CPU / GPU 历史图按采样点推进，支持 10–300 点，默认 60 点；缺测保留空位，恢复后以虚线连接。
- **可调采样**：200–10000 ms，默认 2000 ms；实际周期受硬件调用耗时影响。
- **预设与自动切换**：最多 32 个预设，支持手动应用、排序和按运行进程匹配；规则支持 `*` / `?` 通配符，按优先顺序选取首个匹配，无匹配时回到全局配置。自动切换不覆盖正在编辑的草稿。
- **桌面集成与外观**：托盘显示 / 隐藏 / 退出、关闭转托盘、任务计划器登录启动；浅色、深色或跟随系统主题；系统字体、已安装字体选择和 8–16pt 字号。
- **配置与故障反馈**：JSON 原子保存、旧配置导入；采样或写入失败后停止曲线写入并尝试恢复自动，界面显示错误。正常退出及睡眠暂停时也尝试恢复自动。

## 已验证环境

以下为本机验证环境，**不是通用兼容性列表**。
| 项目 | 版本 / 型号 | 说明 |
| --- | --- | --- |
| 操作系统 | Windows 11 专业工作站版，24H2，26100.7462，x64 | 本机系统查询 |
| 主板 / 机型 | Notebook `NP5x_6x_7x_SNx` | 本机 SMBIOS；未提供有效的主板修订版本 |
| BIOS | `1.07.27RTR6` | 本机 SMBIOS |
| EC | `1.07.07TR3` |本机 EC |
| Control Center | Control Center Package / ControlCenter 3.0 Package `v6.021` | 本机安装记录 |
| FnKey / 热键与 OSD 包 | `CLEVOCO.FnhotkeysandOSD` `7.88.1.0` | 应用包与 `FnKey.exe` 文件版本一致 |
| ACPI Bridge 驱动 | `AcpiBridge.sys` `1.0.0.9` | 设备 `ACPI\CLV0001\1` |
| 实际验证的通信 DLL | FnKey 目录中的 x64 `InsydeDCHU.dll` `1.0.0.1` | 二进制由 SHA256 白名单识别 |
| 物理风扇 | CPU / GPU 双风扇 | 第三风扇仅经过模拟测试 |

实际硬件验证使用的 FnKey 版 `InsydeDCHU.dll` SHA256：

```text
75a47020d3a9d052e94dcb4e3ad61fb69f20177dd46e20c9a20883d92b83981b
```

相同文件版本不代表相同二进制。程序优先查找已安装 FnKey 包中的 DLL，其次查找 Control Center 目录；只有匹配代码白名单的 DLL 才会加载。更新官方组件后，可能需要重新核对协议和哈希，不能仅凭 `1.0.0.1` 判断兼容。

既有实机记录已验证双风扇指定占空比写入、读回实际值和恢复自动；测试期间 FnKey / Control Center 后台保持运行。其他机型、BIOS、第三物理风扇，以及真实睡眠 / 恢复、注销关机、实际登录触发、Explorer 重启、跨屏 DPI 和长期稳定性，仍需对应场景验证。

## 使用

1. 安装与本机匹配的官方 Control Center / FnKey 及 ACPI Bridge 驱动；程序不附带这些厂商组件。(一般系统都有了）
2. 准备 Microsoft Visual C++ x64 运行库（VCRUNTIME140.dll / UCRT）。将程序放在可写目录，以便保存配置。（一般也都有了）
3. 运行 `clevo-fan-control.exe`。Release 版会请求管理员权限。
4. 首次启动默认不接管；编辑曲线并点击“保存并应用”，再根据需要启用“软件接管”。关闭接管后归还固件自动控制。

占空比不等于恒定 RPM。使用前应按设备散热能力调整。

主界面的编辑草稿与正在运行的配置相互独立。“保存并应用”提交编辑内容；接管开关不会顺带提交草稿；自动预设切换也不会覆盖草稿。“恢复全局”使用内存中已保存的全局配置，“重新加载”从磁盘读取配置。

### 配置与迁移

配置位于程序同目录的 `ClevoFanControl.x64.json`，采用 `schemaVersion: 1`，全局设置与预设存放于同一个文件。文件大小限制为 64 KiB，拒绝未知字段、重复字段、错误类型和越界数据。

“导入旧配置”可选择旧版 `ClevoFanControl.json`，并读取同目录的 `ClevoFanControl.presets.json`（如果存在）。导入不会修改原文件，只更新草稿，并关闭导入配置的接管、自启动和自动匹配，不恢复旧强冷开关；保存后才生效。

## 工作原理

```text
FLTK 界面：编辑草稿、提交命令、展示快照与历史
    ↕ Rust 消息通道
控制工作线程：采样、预设匹配、曲线计算、写入与读回检查
    ↓
已安装的 x64 InsydeDCHU.dll
    ↓
AcpiBridge.sys → ACPI DCHU._DSM → PK04 / EC.ECMD → 风扇
```

程序使用完整路径和受限 DLL 搜索加载通过哈希校验的厂商库。通过 `0x0C` / `0x0D` 获取风扇数量、温度、实际占空比和转速计数器；RPM 按官方 FanSpeedSetting 界面的计数器公式换算。

写入通过 `PK04(1)` 逐通道发送 EC `C1` 命令，避免使用会同时影响四个通道的 `0x68` 批量写入；归还固件自动控制使用 `0x69` 位掩码。算法与界面不直接操作底层 I/O 端口，也不安装自写内核驱动。

**程序不独占 EC。** FnKey / Control Center 仍可能在启动、恢复或配置变化时覆盖目标值；控制线程通过后续采样检查实际占空比并纠正偏差。这不等于能够阻止其他程序写入，也不保证崩溃、强杀或驱动挂起时恢复自动。协议与控制权调查见 [硬件研究记录](CONTROL_OWNERSHIP_RESEARCH.md)。

## 构建与测试

需要 Windows x64、Rust / rustup，以及带 MSVC C++ 工具链和 Windows SDK 的 Build Tools。工具链由 `rust-toolchain.toml` 固定；首次构建需要下载 Rust 依赖及 `fltk-bundled` 预编译库。

在项目根目录使用 PowerShell 执行：

```powershell
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked
cargo build --locked --release --target x86_64-pc-windows-msvc
```

产物位于 `target/x86_64-pc-windows-msvc/release/clevo-fan-control.exe`。GitHub Actions 使用同一组检查；手动发布工作流要求 tag 与 `Cargo.toml` 的版本一致。发布包包含程序、README、第三方声明和许可证目录，不包含厂商 DLL / 驱动。

当前自动化测试覆盖曲线与滞回、强冷、第三风扇跟随、故障恢复、外部覆写后的纠正、取消接管、配置校验与导入、历史图和部分界面外观行为。它们使用模拟硬件，不能替代真实硬件验收。

开发诊断命令：

```powershell
cargo run -- --demo                 # 模拟硬件，打开完整界面
cargo run -- --demo --smoke         # 模拟界面启动、临时截图与退出流程
cargo run -- --probe                # 真实硬件只读采样，输出 DLL 路径、哈希和遥测
cargo run -- --hardware-check       # 真实写入 40%，等待 2 秒读回，再尝试恢复自动
```

Debug 使用 `asInvoker`，Release 使用 `requireAdministrator`；真实硬件诊断可能需要从管理员终端运行。`--hardware-check` 会实际改变风扇占空比，不属于普通测试命令。`--smoke` 将截图写入系统临时目录；既有记录中曾出现退出超时，不能视为所有 GUI 场景均已通过。普通运行不生成错误日志或截图文件。

## 引用与感谢

感谢当前实现使用的技术栈及其维护者：

- [Rust / Cargo](https://www.rust-lang.org/)
- [FLTK](https://www.fltk.org/) 与 [fltk-rs](https://github.com/fltk-rs/fltk-rs)
- [Serde](https://serde.rs/) 与 [serde_json](https://github.com/serde-rs/json)
- [windows-rs / windows-sys](https://github.com/microsoft/windows-rs)
- [libloading](https://github.com/nagisa/rust_libloading)
- [RustCrypto / sha2](https://github.com/RustCrypto/hashes)
- [winresource](https://github.com/BenjaminRi/winresource)

保留原项目的引用与感谢：

1. [zuyan9 / RLECViewer](https://github.com/zuyan9/RLECViewer/tree/master)
2. [wangsihan158 / myfancontrol](https://github.com/wangsihan158/myfancontrol)
3. [xl-Synapse / MyFanControl](https://github.com/xl-Synapse/MyFanControl)
4. [百度贴吧原帖](https://tieba.baidu.com/p/5971634018)

沿用的图标位于 `assets/`。旧 MFC 源码及旧二进制已从当前源码树移除，可在 Git branch 'codex/backup-before-rewrite' 中查阅。第三方许可及厂商组件的分发边界见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 [licenses/](licenses/)。
