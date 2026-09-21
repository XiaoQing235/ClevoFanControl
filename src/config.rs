use serde::{Deserialize, Serialize};
use std::{fs, path::Path};

pub type Result<T> = std::result::Result<T, String>;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Point {
    pub temperature: i32,
    pub duty: i32,
}
pub fn default_curve() -> Vec<Point> {
    [45, 50, 55, 60, 65, 70, 75, 80, 85, 90]
        .into_iter()
        .zip([40; 10])
        .map(|(temperature, duty)| Point { temperature, duty })
        .collect()
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ControlConfig {
    pub cpu_curve: Vec<Point>,
    pub gpu_curve: Vec<Point>,
    pub update_interval: f64,
    pub transition_temp: i32,
    pub force_temp: i32,
    pub linear: bool,
    pub take_over: bool,
    pub soft_control: bool,
}
impl Default for ControlConfig {
    fn default() -> Self {
        Self {
            cpu_curve: default_curve(),
            gpu_curve: default_curve(),
            update_interval: 2.0,
            transition_temp: 3,
            force_temp: 50,
            linear: false,
            take_over: false,
            soft_control: false,
        }
    }
}
impl ControlConfig {
    pub fn validate(&self) -> Result<()> {
        for curve in [&self.cpu_curve, &self.gpu_curve] {
            validate_curve(curve)?;
        }
        if !(0.2..=10.0).contains(&self.update_interval)
            || !(0..=10).contains(&self.transition_temp)
            || !(0..=100).contains(&self.force_temp)
        {
            return Err("采样间隔应为 200–10000 ms，滞回应为 0–10°C，冷却阈值应为 0–100°C".into());
        }
        Ok(())
    }
}
pub fn validate_curve(p: &[Point]) -> Result<()> {
    if !(2..=16).contains(&p.len())
        || p.iter()
            .any(|p| !(30..=100).contains(&p.temperature) || !(0..=100).contains(&p.duty))
        || p.windows(2).any(|p| p[0].temperature >= p[1].temperature)
    {
        return Err("曲线需要 2–16 个点；温度 30–100°C 严格递增，占空比 0–100%".into());
    }
    Ok(())
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Preset {
    pub name: String,
    pub process_pattern: String,
    pub control: ControlConfig,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Theme {
    #[default]
    System,
    Light,
    Dark,
}
fn default_history_points() -> usize {
    60
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Settings {
    pub schema_version: u32,
    pub global: ControlConfig,
    pub font_size: i32,
    #[serde(default = "default_history_points")]
    pub history_points: usize,
    #[serde(default)]
    pub theme: Theme,
    /// None uses the current Windows message font; Some is an installed FLTK font name.
    #[serde(default)]
    pub font_family: Option<String>,
    pub auto_run: bool,
    pub close_to_tray: bool,
    pub auto_switch: bool,
    pub presets: Vec<Preset>,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            schema_version: 1,
            global: ControlConfig::default(),
            font_size: 10,
            history_points: default_history_points(),
            theme: Theme::System,
            font_family: None,
            auto_run: false,
            close_to_tray: false,
            auto_switch: false,
            presets: vec![],
        }
    }
}
impl Settings {
    pub fn validate(&self) -> Result<()> {
        if self.schema_version != 1
            || !(8..=16).contains(&self.font_size)
            || self.presets.len() > 32
        {
            return Err("配置版本、字号或预设数量无效".into());
        }
        if self.font_family.as_ref().is_some_and(|name| {
            name.trim().is_empty()
                || name.chars().count() > 128
                || name.chars().any(char::is_control)
        }) {
            return Err("字体名称无效".into());
        }
        if !(10..=300).contains(&self.history_points) {
            return Err("历史采样点数应为 10–300".into());
        }
        self.global.validate()?;
        for (i, p) in self.presets.iter().enumerate() {
            if p.name.trim().is_empty()
                || p.name.chars().count() > 64
                || p.name.chars().any(char::is_control)
                || self.presets[..i].iter().any(|q| q.name == p.name)
            {
                return Err("预设名称必须唯一、非空，最多 64 个字符".into());
            }
            if p.process_pattern.is_empty()
                || p.process_pattern.chars().count() > 128
                || p.process_pattern
                    .chars()
                    .any(|c| c.is_control() || "\\/:<>|\"".contains(c))
            {
                return Err("进程规则只接受文件名与 * / ? 通配符，最多 128 字符".into());
            }
            p.control.validate()?;
        }
        Ok(())
    }
    pub fn load(path: &Path) -> Result<Self> {
        if !path.exists() {
            return Ok(Self::default());
        }
        let s: Self = read_json(path)?;
        s.validate()?;
        Ok(s)
    }
    pub fn save(&self, path: &Path) -> Result<()> {
        self.validate()?;
        let data = serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?;
        atomic_write(path, &data)
    }
}
pub fn read_json<T: serde::de::DeserializeOwned>(path: &Path) -> Result<T> {
    use std::io::Read;
    let mut b = Vec::new();
    fs::File::open(path)
        .map_err(|e| format!("读取配置失败（{}）：{e}", path.display()))?
        .take(65537)
        .read_to_end(&mut b)
        .map_err(|e| e.to_string())?;
    if b.len() > 65536 {
        return Err("配置文件超过 64 KiB".into());
    }
    serde_json::from_slice(b.strip_prefix(&[239, 187, 191]).unwrap_or(&b))
        .map_err(|e| e.to_string())
}
pub fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    use std::io::Write;
    if data.len() > 65536 {
        return Err("配置文件超过 64 KiB".into());
    }
    let tmp = path.with_extension("json.tmp");
    let result = (|| {
        let mut f = fs::File::create(&tmp)
            .map_err(|e| format!("创建配置临时文件失败（{}）：{e}", tmp.display()))?;
        f.write_all(data)
            .and_then(|_| f.sync_all())
            .map_err(|e| format!("写入配置失败（{}）：{e}", tmp.display()))?;
        drop(f);
        crate::platform::replace_file(&tmp, path)
            .map_err(|e| format!("替换配置文件失败（{}）：{e}", path.display()))
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

// Exact legacy shapes retain serde's duplicate/unknown-key rejection; flatten is intentionally avoided.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct LegacyGlobal {
    cpu_curve: Vec<Point>,
    gpu_curve: Vec<Point>,
    update_interval: u64,
    transition_temp: i32,
    force_temp: i32,
    linear: bool,
    take_over: bool,
    force_cooling: bool,
    soft_control: bool,
    font_size: i32,
    auto_run: bool,
    close_to_tray: bool,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct LegacyPreset {
    name: String,
    process_pattern: String,
    cpu_curve: Vec<Point>,
    gpu_curve: Vec<Point>,
    update_interval: u64,
    transition_temp: i32,
    force_temp: i32,
    linear: bool,
    take_over: bool,
    force_cooling: bool,
    soft_control: bool,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct LegacyPresets {
    auto_switch: bool,
    presets: Vec<LegacyPreset>,
}
pub fn import_legacy(path: &Path) -> Result<Settings> {
    let a: LegacyGlobal = read_json(path)?;
    let _old_intentions = (a.take_over, a.force_cooling, a.auto_run);
    let mut s = Settings {
        global: ControlConfig {
            cpu_curve: a.cpu_curve,
            gpu_curve: a.gpu_curve,
            update_interval: a.update_interval as f64,
            transition_temp: a.transition_temp,
            force_temp: a.force_temp,
            linear: a.linear,
            take_over: false,
            soft_control: a.soft_control,
        },
        font_size: a.font_size,
        close_to_tray: a.close_to_tray,
        ..Settings::default()
    };
    let p = path.with_file_name("ClevoFanControl.presets.json");
    if p.exists() {
        let old: LegacyPresets = read_json(&p)?;
        let _ = old.auto_switch;
        for p in old.presets {
            let _ = (p.take_over, p.force_cooling);
            s.presets.push(Preset {
                name: p.name,
                process_pattern: p.process_pattern,
                control: ControlConfig {
                    cpu_curve: p.cpu_curve,
                    gpu_curve: p.gpu_curve,
                    update_interval: p.update_interval as f64,
                    transition_temp: p.transition_temp,
                    force_temp: p.force_temp,
                    linear: p.linear,
                    take_over: false,
                    soft_control: p.soft_control,
                },
            })
        }
    }
    s.validate()?;
    Ok(s)
}
pub fn wildcard(pattern: &str, name: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let n: Vec<char> = name.chars().collect();
    let mut row = vec![false; n.len() + 1];
    row[0] = true;
    for c in p {
        let mut next = vec![false; n.len() + 1];
        if c == '*' {
            next[0] = row[0];
        }
        for j in 1..=n.len() {
            next[j] = if c == '*' {
                row[j] || next[j - 1]
            } else {
                row[j - 1] && (c == '?' || c.to_lowercase().eq(n[j - 1].to_lowercase()))
            };
        }
        row = next;
    }
    row[n.len()]
}
