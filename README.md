# ClevoFanControl — Rust / FLTK x64

Windows 原生风扇控制器。新版使用 Rust + fltk-rs，复用已安装的官方 InsydeDCHU.dll / AcpiBridge.sys，不需要旧版 NTPort、ClevoEcInfo.dll 或 NVGPU_DLL.dll。

## 使用

运行 `clevo-fan-control.exe`（Release 版请求管理员权限）。运行需要 Microsoft Visual C++ x64 运行库（VCRUNTIME140.dll / UCRT）。首次启动读取状态，默认不接管。勾选“软件接管”开始使用已提交曲线，取消勾选归还固件自动。**按本次测试要求，CPU/GPU 初始曲线在全温区均为 40% 占空比**；这不是闭环恒定 RPM，实际转速随硬件响应变化。

- CPU/GPU 各有按采样点推进的实时历史图：橙色温度、蓝色 RPM、绿色实际占空比。每个采样周期（含失败）向左移动一格，最新点位于最右侧；等待期间位置不变。历史长度可设为 10–300 点（默认 60），不按时间删除。图例分别标明量程（温度 0–125°C、实际 0–100%、RPM 按历史峰值取整，至少 6000）。缺测占空点、不补零，恢复后用虚线跨过缺测点连接上一个有效点，连续有效点用实线；历史仅保存在内存。点数立即预览，保存后记住。窗口内容可垂直滚动。
- 采样间隔支持 200–10000 ms，界面按 1 ms 调整，默认 2000 ms；保存并应用后生效，随全局/预设控制配置切换。实际间隔受硬件调用耗时影响。
- CPU/GPU 独立 2–16 点曲线；阶梯/线性、温度滞回、每约 100ms 一个百分点的平滑调速。
- 拖动节点、双击插入、方向键微调、Shift ×5、Delete 删除；节点编号和温度/占空比数字编辑。
- 第三风扇跟随 GPU 目标；显示温度、实际占空比、目标、档位及按官方公式换算的 RPM。
- 95% 强制冷却优先于普通接管；所有相关有效温度严格低于阈值后结束。它是一次性操作，不保存开关。
- 最多 32 个预设，支持创建、重命名、编辑进程规则、删除、排序与手动应用。规则按所有进程的文件名匹配，支持 `*`、Unicode 字符 `?`，大小写不敏感；按列表顺序选择首个匹配，无匹配回到 Global。扫描失败不当作无匹配。
- 自动切换每约 500ms 检查，不替换正在编辑的草稿。“编辑配置”和“运行”分别显示两者；点击保存并应用才提交曲线。
- 外观：字体默认跟随 Windows 界面字体，也可选择本机已安装字体，字号保持 8–16pt 可调。主题可选“跟随系统 / 浅色 / 深色”，默认跟随系统应用模式；选择立即预览，点击“保存并应用”持久保存。运行中约每秒检查系统外观变化。旧配置缺少外观字段时自动使用跟随系统。
- 托盘显示/隐藏/退出、温度和占空比提示、Explorer 重启重建、关闭转托盘、退出保存询问、任务计划器登录启动、8–16pt 字号。
- 正常退出及睡眠暂停尝试恢复自动；恢复后先重新采样。采样或写入失败会停止曲线写入、尝试自动，并在界面反馈。修复故障后重连或重新请求接管。
- 主界面按控制设置、窗口与外观分组；弹窗使用统一中文按钮、主题和字体。关于窗口中的长路径自动换行，可滚动查看并复制硬件与配置信息；Windows 支持时同步原生标题栏深浅色。
- 不生成错误日志文件。诊断命令仅向终端输出。

“恢复全局”使用内存中的已保存全局配置；“重新加载”读取新版配置文件；“重置曲线”恢复当前通道的 40% 测试默认曲线。接管开关只改变当前运行策略，不连带应用曲线草稿。管理预设的“确定”保存预设，不顺带应用主界面的未保存曲线；“使用选中项”在保存成功后应用该预设，自动规则仍可随后覆盖它。

## 配置与迁移

新版配置：exe 同目录 `ClevoFanControl.x64.json`，`schemaVersion: 1`。全局和预设在一个文件中原子保存，限制 64 KiB，严格拒绝未知字段、重复字段、错误类型与越界数据。目录需要写权限。

“导入旧配置”选择原来的 `ClevoFanControl.json`，同时读取同目录的 `ClevoFanControl.presets.json`（如果存在）。保留原文件；导入仅修改草稿，关闭导入内容的接管、强冷、自启动和自动匹配，点击保存后生效。旧工程留在仓库中作对照，备份分支为 `codex/backup-before-rewrite`。

## 硬件支持与验证边界

目前已验证本机 `NP5x_6x_7x_SNx` / BIOS `1.07.27RTR6` / AcpiBridge `1.0.0.9`。加载已安装 FnKey 包中的 DLL，使用完整路径、受限 DLL 搜索及已检查二进制的 SHA256 白名单。未知 DLL 会明确拒绝；更新官方组件后需要重新核对协议与哈希。

调用链：`Rust → InsydeDCHU.dll → AcpiBridge.sys → DCHU._DSM → PK04 / EC.ECMD`。逐通道发送 EC C1，不使用会连带写四个通道的 0x68；恢复自动使用 0x69 位掩码。温度/占空比/风扇数量通过 0x0C / 0x0D 获取。RPM 使用官方 FanSpeedSetting UI 的计数器换算，而非把计数器直接显示成 RPM。

本机已完成指定占空比、读回实际值、恢复自动的真实闭环，测试期间 FnKey 和 CC 后台仍在运行。它们在启动、恢复和配置变化时仍可能覆盖设置；新版通过后续采样检测偏差并纠正，不声称独占 EC。测试记录见 `REWRITE_ROADMAP.md`。

第三风扇路径经过模拟测试，本机只有两个风扇。睡眠/注销、任务计划器实际登录、Explorer 重启、跨屏 DPI 与长期稳定性仍需要对应场景验证；进程强杀/崩溃不保证恢复自动。不承诺所有 Clevo 机型兼容。

## 构建与测试

需要 Windows x64、MSVC C++ Build Tools / Windows SDK 和 Rust。工具链固定于 `rust-toolchain.toml`。fltk-rs 1.5.23 使用 `fltk-bundled` 官方预编译库，首次构建需网络；依赖锁定在 Cargo.lock。

```powershell
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked
cargo build --locked --release --target x86_64-pc-windows-msvc
```

产物：`target/x86_64-pc-windows-msvc/release/clevo-fan-control.exe`。GitHub Actions 使用同一构建方式。手动 release 工作流要求 tag 与 Cargo.toml 版本一致，保留重复 tag/release 检查，发布包不含厂商 DLL/驱动。

开发验证：

```powershell
cargo run -- --demo                 # 模拟硬件，完整 UI
cargo run -- --demo --smoke         # 模拟 UI 启动/截图/退出，不接触 EC
cargo run -- --probe                # 只读硬件采样，输出 DLL 路径/哈希
cargo run -- --hardware-check       # 真实写入 40%，2 秒后读回，再恢复自动
```

Debug 使用 asInvoker 便于 UI 测试；Release 使用 requireAdministrator。`--hardware-check` 是显式调速测试，不在启动时自动运行。`--smoke` 会把 UI 截图写入系统临时目录；普通使用不生成截图或日志。

第三方许可证及厂商组件边界见 `THIRD_PARTY_NOTICES.md` 和 `licenses/`。
