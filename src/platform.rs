use crate::config::Result;
use std::{
    ffi::OsStr,
    os::windows::{ffi::OsStrExt, process::CommandExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{Arc, Mutex, OnceLock, mpsc},
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::*,
    Storage::FileSystem::*,
    System::{Diagnostics::ToolHelp::*, LibraryLoader::GetModuleHandleW, Threading::*},
    UI::{Shell::*, WindowsAndMessaging::*},
};
pub fn wide(s: impl AsRef<OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(Some(0)).collect()
}
/// Windows application theme; absent personalization uses the Windows light default.
pub fn system_dark_mode() -> bool {
    use windows_sys::Win32::System::Registry::*;
    let mut light = 1u32;
    let mut size = std::mem::size_of::<u32>() as u32;
    let status = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            wide(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize").as_ptr(),
            wide("AppsUseLightTheme").as_ptr(),
            RRF_RT_REG_DWORD,
            std::ptr::null_mut(),
            (&mut light as *mut u32).cast(),
            &mut size,
        )
    };
    status == ERROR_SUCCESS && light == 0
}
pub fn system_font_family() -> Option<String> {
    let mut metrics: NONCLIENTMETRICSW = unsafe { std::mem::zeroed() };
    metrics.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
    let ok = unsafe {
        SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            metrics.cbSize,
            (&mut metrics as *mut NONCLIENTMETRICSW).cast(),
            0,
        )
    };
    if ok == 0 {
        return None;
    }
    let face = &metrics.lfMessageFont.lfFaceName;
    let len = face.iter().position(|&c| c == 0).unwrap_or(face.len());
    let name = String::from_utf16_lossy(&face[..len]);
    (!name.is_empty()).then_some(name)
}
pub fn replace_file(from: &Path, to: &Path) -> Result<()> {
    unsafe {
        if MoveFileExW(
            wide(from).as_ptr(),
            wide(to).as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        ) == 0
        {
            Err(std::io::Error::last_os_error().to_string())
        } else {
            Ok(())
        }
    }
}
pub struct SingleInstance(HANDLE);
impl SingleInstance {
    pub fn acquire() -> Result<Self> {
        unsafe {
            let h = CreateMutexW(
                std::ptr::null(),
                0,
                wide("Global\\ClevoFanControl.EcController").as_ptr(),
            );
            if h.is_null() {
                return Err(std::io::Error::last_os_error().to_string());
            }
            if GetLastError() == ERROR_ALREADY_EXISTS {
                CloseHandle(h);
                return Err("已有新／旧版 ClevoFanControl 正在运行".into());
            }
            Ok(Self(h))
        }
    }
}
impl Drop for SingleInstance {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
pub fn processes() -> Result<Vec<String>> {
    unsafe {
        let h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if h == INVALID_HANDLE_VALUE {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let mut e: PROCESSENTRY32W = std::mem::zeroed();
        e.dwSize = std::mem::size_of_val(&e) as u32;
        let mut names = vec![];
        if Process32FirstW(h, &mut e) == 0 {
            CloseHandle(h);
            return Err("无法枚举进程".into());
        }
        loop {
            let end = e
                .szExeFile
                .iter()
                .position(|v| *v == 0)
                .unwrap_or(e.szExeFile.len());
            names.push(String::from_utf16_lossy(&e.szExeFile[..end]));
            if Process32NextW(h, &mut e) == 0 {
                let err = GetLastError();
                CloseHandle(h);
                if err != ERROR_NO_MORE_FILES {
                    return Err(format!("进程枚举中断: {err}"));
                }
                break;
            }
        }
        Ok(names)
    }
}
pub fn run(program: &str, args: &[&str]) -> Result<String> {
    use std::io::Read;
    let mut child = Command::new(program)
        .args(args)
        .creation_flags(CREATE_NO_WINDOW)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("启动系统命令 {program} 失败：{e}"))?;
    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let read = |stream: Box<dyn Read + Send>| {
        std::thread::spawn(move || {
            let mut b = Vec::new();
            stream.take(4 * 1024 * 1024).read_to_end(&mut b).map(|_| b)
        })
    };
    let out_reader = read(Box::new(stdout));
    let err_reader = read(Box::new(stderr));
    let start = Instant::now();
    loop {
        if child.try_wait().map_err(|e| e.to_string())?.is_some() {
            break;
        }
        if start.elapsed() > Duration::from_secs(20) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("系统命令执行超时".into());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    let stdout = out_reader
        .join()
        .map_err(|_| "读取命令输出失败")?
        .map_err(|e| e.to_string())?;
    let stderr = err_reader
        .join()
        .map_err(|_| "读取命令输出失败")?
        .map_err(|e| e.to_string())?;
    if !status.success() {
        return Err(format!(
            "{program}: {} {}",
            status,
            String::from_utf8_lossy(&stderr)
        ));
    }
    if stdout.starts_with(&[255, 254]) || stdout.starts_with(&[60, 0, 63, 0]) {
        let b = if stdout.starts_with(&[255, 254]) {
            &stdout[2..]
        } else {
            &stdout[..]
        };
        let w: Vec<u16> = b
            .chunks_exact(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .collect();
        Ok(String::from_utf16_lossy(&w))
    } else {
        match String::from_utf8(stdout) {
            Ok(s) => Ok(s),
            Err(e) => {
                use windows_sys::Win32::Globalization::{CP_OEMCP, MultiByteToWideChar};
                let b = e.into_bytes();
                unsafe {
                    let n = MultiByteToWideChar(
                        CP_OEMCP,
                        0,
                        b.as_ptr(),
                        b.len() as i32,
                        std::ptr::null_mut(),
                        0,
                    );
                    if n == 0 {
                        return Err("系统命令文本解码失败".into());
                    }
                    let mut out = vec![0u16; n as usize];
                    MultiByteToWideChar(
                        CP_OEMCP,
                        0,
                        b.as_ptr(),
                        b.len() as i32,
                        out.as_mut_ptr(),
                        n,
                    );
                    Ok(String::from_utf16_lossy(&out))
                }
            }
        }
    }
}
fn xml(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}
pub fn set_autorun(enabled: bool) -> Result<()> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    if enabled {
        let text = format!(
            r#"<?xml version="1.0" encoding="UTF-16"?><Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task"><Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers><Principals><Principal id="Author"><GroupId>S-1-5-32-545</GroupId><RunLevel>HighestAvailable</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled></Settings><Actions Context="Author"><Exec><Command>{}</Command></Exec></Actions></Task>"#,
            xml(&exe.to_string_lossy())
        );
        let p =
            std::env::temp_dir().join(format!("ClevoFanControl-task-{}.xml", std::process::id()));
        let bytes: Vec<u8> = std::iter::once(0xfeff)
            .chain(text.encode_utf16())
            .flat_map(u16::to_le_bytes)
            .collect();
        std::fs::write(&p, bytes).map_err(|e| e.to_string())?;
        let result = run(
            "schtasks.exe",
            &[
                "/Create",
                "/TN",
                "ClevoFanControl",
                "/XML",
                &p.to_string_lossy(),
                "/F",
            ],
        );
        let _ = std::fs::remove_file(p);
        result?;
        let actual = run(
            "schtasks.exe",
            &["/Query", "/TN", "ClevoFanControl", "/XML"],
        )?;
        if !actual.contains(&xml(&exe.to_string_lossy())) {
            return Err("登录任务的目标路径验证失败".into());
        }
    } else {
        // Query all tasks distinguishes an absent task from a scheduler/permission failure.
        let all = run("schtasks.exe", &["/Query", "/FO", "CSV", "/NH"])?;
        if all.to_lowercase().contains("\\clevofancontrol\"") {
            run("schtasks.exe", &["/Delete", "/TN", "ClevoFanControl", "/F"])?;
        }
        let all = run("schtasks.exe", &["/Query", "/FO", "CSV", "/NH"])?;
        if all.to_lowercase().contains("\\clevofancontrol\"") {
            return Err("登录任务删除后仍存在".into());
        }
    }
    Ok(())
}
pub fn machine() -> Result<String> {
    run(
        "powershell.exe",
        &[
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "(Get-ItemProperty 'HKLM:\\HARDWARE\\DESCRIPTION\\System\\BIOS').SystemProductName",
        ],
    )
    .map(|s| s.trim().to_string())
}
pub fn find_dll() -> Result<PathBuf> {
    let p = run(
        "powershell.exe",
        &[
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "Get-AppxPackage '*FnhotkeysandOSD*' | ForEach-Object { Join-Path $_.InstallLocation 'FnKey\\InsydeDCHU.dll' } | Select-Object -First 1",
        ],
    )?;
    let path = PathBuf::from(p.trim());
    if path.is_absolute() && path.is_file() {
        return Ok(path);
    }
    let path = PathBuf::from(r"C:\Program Files (x86)\ControlCenter\InsydeDCHU.dll");
    if path.is_file() {
        Ok(path)
    } else {
        Err("未找到官方 x64 InsydeDCHU.dll，请先安装 Control Center 驱动".into())
    }
}

#[derive(Clone, Copy, Debug)]
pub enum NativeEvent {
    Show,
    Hide,
    Exit,
}
struct TrayState {
    sender: mpsc::Sender<NativeEvent>,
    control: mpsc::Sender<crate::worker::Command>,
    tip: Arc<Mutex<String>>,
    taskbar: u32,
}
static TRAY: OnceLock<TrayState> = OnceLock::new();
const TRAY_MSG: u32 = WM_APP + 12;
const UPDATE_MSG: u32 = WM_APP + 13;
pub struct Tray {
    hwnd: usize,
    tip: Arc<Mutex<String>>,
}
impl Tray {
    pub fn start(
        sender: mpsc::Sender<NativeEvent>,
        control: mpsc::Sender<crate::worker::Command>,
    ) -> Result<Self> {
        let tip = Arc::new(Mutex::new("ClevoFanControl".into()));
        let t = tip.clone();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || unsafe {
            let class = wide("ClevoFanControl.Tray.x64");
            let instance = GetModuleHandleW(std::ptr::null());
            let w = WNDCLASSW {
                lpfnWndProc: Some(tray_proc),
                hInstance: instance,
                lpszClassName: class.as_ptr(),
                ..std::mem::zeroed()
            };
            if RegisterClassW(&w) == 0 {
                let _ = tx.send(Err(std::io::Error::last_os_error().to_string()));
                return;
            }
            let _ = TRAY.set(TrayState {
                sender,
                control,
                tip: t,
                taskbar: RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()),
            });
            let h = CreateWindowExW(
                0,
                class.as_ptr(),
                class.as_ptr(),
                0,
                0,
                0,
                0,
                0,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                instance,
                std::ptr::null(),
            );
            if h.is_null() {
                let _ = tx.send(Err(std::io::Error::last_os_error().to_string()));
                return;
            }
            notify(h, NIM_ADD);
            let _ = tx.send(Ok(h as usize));
            let mut msg = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            notify(h, NIM_DELETE);
        });
        Ok(Self {
            hwnd: rx.recv().map_err(|e| e.to_string())??,
            tip,
        })
    }
    pub fn update(&self, text: String) {
        if let Ok(mut t) = self.tip.lock() {
            *t = text;
        }
        unsafe {
            PostMessageW(self.hwnd as HWND, UPDATE_MSG, 0, 0);
        }
    }
}
impl Drop for Tray {
    fn drop(&mut self) {
        unsafe {
            PostMessageW(self.hwnd as HWND, WM_CLOSE, 0, 0);
        }
    }
}
unsafe fn notify(h: HWND, op: u32) {
    unsafe {
        let mut n: NOTIFYICONDATAW = std::mem::zeroed();
        n.cbSize = std::mem::size_of_val(&n) as u32;
        n.hWnd = h;
        n.uID = 1;
        n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        n.uCallbackMessage = TRAY_MSG;
        n.hIcon = LoadIconW(
            GetModuleHandleW(std::ptr::null()),
            std::ptr::without_provenance(1),
        );
        if n.hIcon.is_null() {
            n.hIcon = LoadIconW(std::ptr::null_mut(), IDI_APPLICATION);
        }
        if let Some(s) = TRAY.get()
            && let Ok(t) = s.tip.lock()
        {
            for (a, b) in n.szTip.iter_mut().take(127).zip(t.encode_utf16()) {
                *a = b;
            }
        }
        Shell_NotifyIconW(op, &n);
    }
}
unsafe extern "system" fn tray_proc(h: HWND, msg: u32, w: usize, l: isize) -> isize {
    unsafe {
        let Some(s) = TRAY.get() else {
            return DefWindowProcW(h, msg, w, l);
        };
        if msg == s.taskbar {
            notify(h, NIM_ADD);
            return 0;
        }
        match msg {
            UPDATE_MSG => {
                notify(h, NIM_MODIFY);
                0
            }
            TRAY_MSG => {
                match l as u32 {
                    WM_LBUTTONUP | WM_LBUTTONDBLCLK => {
                        let _ = s.sender.send(NativeEvent::Show);
                    }
                    WM_RBUTTONUP => {
                        let menu = CreatePopupMenu();
                        AppendMenuW(menu, MF_STRING, 1, wide("显示").as_ptr());
                        AppendMenuW(menu, MF_STRING, 2, wide("隐藏").as_ptr());
                        AppendMenuW(menu, MF_STRING, 3, wide("退出").as_ptr());
                        let mut p = std::mem::zeroed();
                        GetCursorPos(&mut p);
                        SetForegroundWindow(h);
                        let id = TrackPopupMenu(
                            menu,
                            TPM_RETURNCMD | TPM_RIGHTBUTTON,
                            p.x,
                            p.y,
                            0,
                            h,
                            std::ptr::null(),
                        );
                        DestroyMenu(menu);
                        let e = match id {
                            1 => Some(NativeEvent::Show),
                            2 => Some(NativeEvent::Hide),
                            3 => Some(NativeEvent::Exit),
                            _ => None,
                        };
                        if let Some(e) = e {
                            let _ = s.sender.send(e);
                        }
                    }
                    _ => {}
                }
                0
            }
            WM_POWERBROADCAST => {
                if w == 4 {
                    let _ = s.control.send(crate::worker::Command::Suspend);
                }
                if w == 18 {
                    let _ = s.control.send(crate::worker::Command::Resume);
                }
                1
            }
            WM_QUERYENDSESSION => {
                let _ = s.sender.send(NativeEvent::Exit);
                1
            }
            WM_CLOSE => {
                notify(h, NIM_DELETE);
                DestroyWindow(h);
                0
            }
            WM_DESTROY => {
                PostQuitMessage(0);
                0
            }
            _ => DefWindowProcW(h, msg, w, l),
        }
    }
}

/// Best effort native title-bar appearance on supported Windows versions.
pub fn set_window_dark(hwnd: usize, dark: bool) {
    let value: i32 = dark.into();
    unsafe {
        windows_sys::Win32::Graphics::Dwm::DwmSetWindowAttribute(
            hwnd as *mut std::ffi::c_void,
            20,
            (&value as *const i32).cast(),
            std::mem::size_of::<i32>() as u32,
        );
    }
}
