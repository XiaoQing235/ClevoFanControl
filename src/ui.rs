mod dialogs;
mod history;
// Hallmark · pre-emit critique: P4 H4 E4 S5 R5 V4.
// Hallmark:  Cobalt workbench, system or installed UI font. P4 H4 E4 S5 R5 V4.
use clevo_fan_control::{
    config::{self, ControlConfig, Point, Preset, Settings, Theme},
    control::linear,
    platform::{self, NativeEvent, Tray},
    worker::{Command, Event as WorkerEvent, Snapshot, Worker},
};
use fltk::{
    app,
    browser::HoldBrowser,
    button::{Button, CheckButton},
    dialog, draw,
    enums::{Align, Color, Event, Font, FrameType, Key, Shortcut},
    frame::Frame,
    group::{Flex, Group, Scroll, ScrollType},
    input::Input,
    menu::Choice,
    misc::Spinner,
    prelude::*,
    window::Window,
};
use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
    rc::Rc,
    sync::mpsc,
    time::{Duration, Instant},
};
// Indexed colors let existing widgets and custom canvases change together.
const PAPER: Color = Color::from_rgbi(16);
const SURFACE: Color = Color::from_rgbi(17);
const INK: Color = Color::from_rgbi(18);
const MUTED: Color = Color::from_rgbi(19);
const GRID: Color = Color::from_rgbi(20);
const ACCENT: Color = Color::from_rgbi(21);
thread_local! {
    static LAST_APPEARANCE_CHECK: Cell<Option<Instant>> = const { Cell::new(None) };
    static UI_TEXT_SIZE: Cell<i32> = const { Cell::new(13) };
    static THEME: Cell<Theme> = const { Cell::new(Theme::System) };
    static FONT_FAMILY: RefCell<Option<String>> = const { RefCell::new(None) };
    static APPLIED_FONT: RefCell<Option<String>> = const { RefCell::new(None) };
    static DARK: Cell<Option<bool>> = const { Cell::new(None) };
}
fn theme_value(theme: Theme) -> i32 {
    match theme {
        Theme::System => 0,
        Theme::Light => 1,
        Theme::Dark => 2,
    }
}
fn selected_theme(choice: &Choice) -> Theme {
    match choice.value() {
        1 => Theme::Light,
        2 => Theme::Dark,
        _ => Theme::System,
    }
}
fn apply_theme(theme: Theme) {
    THEME.set(theme);
    let dark = match theme {
        Theme::System => platform::system_dark_mode(),
        Theme::Light => false,
        Theme::Dark => true,
    };
    if DARK.replace(Some(dark)) == Some(dark) {
        return;
    }
    let colors = if dark {
        [0x181c23, 0x242b35, 0xe9eef5, 0xafbdce, 0x394555, 0x78a7ff]
    } else {
        [0xf4f6f8, 0xffffff, 0x202a38, 0x566476, 0xdce2e9, 0x245edb]
    };
    let rgb = |hex| Color::from_hex(hex).to_rgb();
    let (r, g, b) = rgb(colors[0]);
    app::background(r, g, b);
    let (r, g, b) = rgb(colors[1]);
    app::background2(r, g, b);
    let (r, g, b) = rgb(colors[2]);
    app::foreground(r, g, b);
    for (index, hex) in colors.into_iter().enumerate() {
        let (r, g, b) = rgb(hex);
        app::set_color(Color::by_index(16 + index as u8), r, g, b);
    }
    history::colors(dark);
    let (r, g, b) = ACCENT.to_rgb();
    app::set_color(Color::Selection, r, g, b);
    if let Some(windows) = app::windows() {
        for w in windows {
            platform::set_window_dark(w.raw_handle() as _, dark);
        }
    }
    app::redraw();
}
fn apply_family(family: Option<String>) {
    FONT_FAMILY.set(family.clone());
    let Some(name) = family.or_else(platform::system_font_family) else {
        return;
    };
    if APPLIED_FONT.with_borrow(|old| old.as_ref() == Some(&name)) {
        return;
    }
    Font::set_font(Font::Helvetica, &format!(" {name}"));
    Font::set_font(Font::HelveticaBold, &format!("B{name}"));
    APPLIED_FONT.set(Some(name));
    app::redraw();
}
fn load_family_choice(choice: &mut Choice, family: &Option<String>, fonts: &[String]) {
    choice.set_value(
        family
            .as_ref()
            .and_then(|name| fonts.iter().position(|f| f == name))
            .map_or(0, |i| i as i32 + 1),
    );
}
fn refresh_system_appearance() {
    if LAST_APPEARANCE_CHECK
        .get()
        .is_some_and(|last| last.elapsed() < Duration::from_secs(1))
    {
        return;
    }
    LAST_APPEARANCE_CHECK.set(Some(Instant::now()));
    if FONT_FAMILY.with_borrow(|name| name.is_none()) {
        apply_family(None);
    }
    if THEME.get() == Theme::System {
        apply_theme(Theme::System);
    }
}
#[derive(Clone, Copy)]
enum Action {
    Edit,
    Numeric(usize),
    HistoryPoints,
    Node(usize, i32),
    Save,
    SavePresets,
    Load,
    Global,
    Import,
    Presets,
    Takeover,
    Force,
    Auto,
    Font,
    Theme,
    Family,
    Exit,
    Close,
    Retry,
    About,
    Settings,
    SaveSettings,
    Select,
}
#[derive(Clone)]
struct Editor {
    draft: ControlConfig,
    selection: [usize; 2],
    temps: [Option<i32>; 2],
    dirty: bool,
    target: Option<String>,
}
impl Editor {
    fn curve(&self, ch: usize) -> &Vec<Point> {
        if ch == 0 {
            &self.draft.cpu_curve
        } else {
            &self.draft.gpu_curve
        }
    }
    fn curve_mut(&mut self, ch: usize) -> &mut Vec<Point> {
        if ch == 0 {
            &mut self.draft.cpu_curve
        } else {
            &mut self.draft.gpu_curve
        }
    }
}
fn label(text: &str) -> Frame {
    let mut f = Frame::default().with_label(text);
    f.set_align(Align::Left | Align::Inside);
    f.set_label_color(INK);
    f
}
fn control_frame(x: i32, y: i32, w: i32, h: i32, color: Color) {
    draw::draw_rect_fill(x, y, w, h, color);
    draw::set_draw_color(GRID);
    draw::draw_rect(x, y, w, h);
}
fn install_control_style() {
    for frame in [
        FrameType::UpBox,
        FrameType::DownBox,
        FrameType::ThinUpBox,
        FrameType::ThinDownBox,
    ] {
        app::set_frame_type_cb(frame, control_frame, 2, 2, 4, 4);
    }
}
fn style_button(b: &mut Button, primary: bool) {
    let base = if primary { ACCENT } else { SURFACE };
    b.set_frame(FrameType::UpBox);
    b.set_color(base);
    b.set_label_color(if primary { SURFACE } else { INK });
    b.set_selection_color(base);
    b.handle(move |b, event| {
        match event {
            Event::Enter if b.active_r() => {
                b.set_color(base.lighter());
                b.redraw();
            }
            Event::Leave => {
                b.set_color(base);
                b.redraw();
            }
            _ => {}
        }
        false
    });
}
fn section(text: &str) -> Frame {
    let mut f = label(text);
    f.set_label_font(Font::HelveticaBold);
    f.set_label_color(MUTED);
    f
}
fn button(text: &str, action: Action, tx: app::Sender<Action>) -> Button {
    let mut b = Button::default().with_label(text);
    style_button(&mut b, false);
    b.set_callback(move |_| tx.send(action));
    b
}
fn spin(min: f64, max: f64, val: f64, action: Action, tx: app::Sender<Action>) -> Spinner {
    let mut s = Spinner::default();
    s.set_color(SURFACE);
    s.set_text_color(INK);
    s.set_selection_color(ACCENT);
    s.set_range(min, max);
    s.set_step(1.);
    s.set_value(val);
    s.set_callback(move |_| tx.send(action));
    s
}
fn check(text: &str, val: bool, action: Action, tx: app::Sender<Action>) -> CheckButton {
    let mut b = CheckButton::default().with_label(text);
    b.set_down_frame(FrameType::DownBox);
    b.set_color(SURFACE);
    b.set_selection_color(ACCENT);
    b.set_value(val);
    b.set_callback(move |_| tx.send(action));
    b
}
fn apply_font(group: &mut Group, size: i32) {
    for i in 0..group.children() {
        if let Some(mut w) = group.child(i) {
            w.set_label_size(size);
            if let Some(mut g) = w.as_group() {
                apply_font(&mut g, size);
            }
        }
    }
}
fn hover_box(w: &Frame, x: i32, y: i32, text: &str) {
    draw::set_font(Font::Helvetica, UI_TEXT_SIZE.get().clamp(12, 16));
    let (tw, th) = draw::measure(text, false);
    let width = tw + 16;
    let height = th + 12;
    let x = if x + 14 + width <= w.x() + w.w() {
        x + 14
    } else {
        x - width - 14
    }
    .max(w.x());
    let y = (y - height - 10).clamp(w.y(), (w.y() + w.h() - height).max(w.y()));
    draw::push_clip(w.x(), w.y(), w.w(), w.h());
    draw::draw_rect_fill(x, y, width, height, PAPER);
    draw::set_draw_color(MUTED);
    draw::draw_rect(x, y, width, height);
    draw::set_draw_color(INK);
    draw::draw_text2(text, x + 8, y + 6, tw, th, Align::Left | Align::Inside);
    draw::pop_clip();
}
fn canvas(ch: usize, editor: Rc<RefCell<Editor>>) -> Frame {
    let mut f = Frame::default();
    f.set_frame(FrameType::FlatBox);
    f.set_color(SURFACE);
    let hover = Rc::new(Cell::new(None::<(i32, i32)>));
    let draw_hover = hover.clone();
    let e = editor.clone();
    f.draw(move |w| {
        draw::draw_rect_fill(w.x(), w.y(), w.w(), w.h(), SURFACE);
        let l = w.x() + 45;
        let t = w.y() + 20;
        let width = w.w() - 65;
        let height = w.h() - 52;
        let px = |v: i32| l + (v - 30) * width / 70;
        let py = |v: i32| t + height - v * height / 100;
        draw::set_font(Font::Helvetica, UI_TEXT_SIZE.get().clamp(12, 16));
        for temp in (30..=100).step_by(10) {
            draw::set_draw_color(GRID);
            draw::draw_line(px(temp), t, px(temp), t + height);
            draw::set_draw_color(MUTED);
            draw::draw_text2(
                &temp.to_string(),
                px(temp) - 15,
                t + height + 8,
                30,
                20,
                Align::Center,
            );
        }
        for duty in (0..=100).step_by(25) {
            draw::set_draw_color(GRID);
            draw::draw_line(l, py(duty), l + width, py(duty));
            draw::set_draw_color(MUTED);
            draw::draw_text2(
                &duty.to_string(),
                w.x() + 2,
                py(duty) - 8,
                35,
                16,
                Align::Right,
            );
        }
        let e = e.borrow();
        if let Some(temp) = e.temps[ch] {
            draw::set_draw_color(MUTED);
            draw::set_line_style(draw::LineStyle::Dash, 1);
            draw::draw_line(
                px(temp.clamp(30, 100)),
                t,
                px(temp.clamp(30, 100)),
                t + height,
            );
            draw::set_line_style(draw::LineStyle::Solid, 0);
        }
        let points = e.curve(ch);
        draw::set_draw_color(ACCENT);
        draw::set_line_style(draw::LineStyle::Solid, 2);
        draw::draw_line(
            px(30),
            py(points[0].duty),
            px(points[0].temperature),
            py(points[0].duty),
        );
        for p in points.windows(2) {
            if e.draft.linear {
                draw::draw_line(
                    px(p[0].temperature),
                    py(p[0].duty),
                    px(p[1].temperature),
                    py(p[1].duty),
                );
            } else {
                draw::draw_line(
                    px(p[0].temperature),
                    py(p[0].duty),
                    px(p[1].temperature),
                    py(p[0].duty),
                );
                draw::draw_line(
                    px(p[1].temperature),
                    py(p[0].duty),
                    px(p[1].temperature),
                    py(p[1].duty),
                );
            }
        }
        let last = points.last().unwrap();
        draw::draw_line(px(last.temperature), py(last.duty), px(100), py(last.duty));
        draw::set_line_style(draw::LineStyle::Solid, 0);
        for (i, p) in points.iter().enumerate() {
            let r = if e.selection[ch] == i { 6 } else { 4 };
            draw::draw_pie(
                px(p.temperature) - r,
                py(p.duty) - r,
                2 * r,
                2 * r,
                0.,
                360.,
            );
        }
        if w.has_focus() {
            draw::set_draw_color(ACCENT);
            draw::draw_rect(w.x() + 1, w.y() + 1, w.w() - 2, w.h() - 2);
        }
        if let Some((mx, my)) = draw_hover.get() {
            let (mx, my) = (w.x() + mx, w.y() + my);
            if let Some((i, p)) = points
                .iter()
                .enumerate()
                .find(|(_, p)| (px(p.temperature) - mx).abs() < 12 && (py(p.duty) - my).abs() < 12)
            {
                hover_box(
                    w,
                    mx,
                    my,
                    &format!("Node #{}\n{} °C\n{} %", i + 1, p.temperature, p.duty),
                );
            }
        }
    });
    let mut dragging = false;
    f.handle(move |w, event| {
        match event {
            Event::Enter | Event::Move | Event::Drag | Event::Push | Event::Released => {
                hover.set(Some((app::event_x() - w.x(), app::event_y() - w.y())));
                w.redraw();
            }
            Event::Leave | Event::Hide => {
                hover.set(None);
                w.redraw();
            }
            _ => {}
        }
        if event == Event::Push {
            let _ = w.take_focus();
        }
        let mut e = editor.borrow_mut();
        let l = w.x() + 45;
        let t = w.y() + 20;
        let width = w.w() - 65;
        let height = w.h() - 52;
        let temp = (30 + (app::event_x() - l) * 70 / width.max(1)).clamp(30, 100);
        let duty = (100 - (app::event_y() - t) * 100 / height.max(1)).clamp(0, 100);
        match event {
            Event::Enter | Event::Move | Event::Leave => true,
            Event::Push => {
                let index = e.curve(ch).iter().position(|p| {
                    let x = l + (p.temperature - 30) * width / 70;
                    let y = t + height - p.duty * height / 100;
                    (x - app::event_x()).abs() < 12 && (y - app::event_y()).abs() < 12
                });
                if let Some(i) = index {
                    e.selection[ch] = i;
                    dragging = true;
                } else if app::event_clicks()
                    && e.curve(ch).len() < 16
                    && !e.curve(ch).iter().any(|p| p.temperature == temp)
                {
                    let i = e.curve(ch).partition_point(|p| p.temperature < temp);
                    e.curve_mut(ch).insert(
                        i,
                        Point {
                            temperature: temp,
                            duty,
                        },
                    );
                    e.selection[ch] = i;
                    e.dirty = true;
                }
                w.redraw();
                true
            }
            Event::Drag if dragging => {
                let i = e.selection[ch];
                let p = e.curve(ch);
                let lo = if i == 0 { 30 } else { p[i - 1].temperature + 1 };
                let hi = if i + 1 == p.len() {
                    100
                } else {
                    p[i + 1].temperature - 1
                };
                e.curve_mut(ch)[i] = Point {
                    temperature: temp.clamp(lo, hi),
                    duty,
                };
                e.dirty = true;
                w.redraw();
                true
            }
            Event::Released => {
                dragging = false;
                true
            }
            Event::Unfocus => {
                dragging = false;
                w.redraw();
                true
            }
            Event::Focus => {
                w.redraw();
                true
            }
            Event::KeyDown => {
                let i = e.selection[ch];
                let key = app::event_key();
                let n = if app::event_state().contains(Shortcut::Shift) {
                    5
                } else {
                    1
                };
                if key == Key::Delete && e.curve(ch).len() > 2 {
                    e.curve_mut(ch).remove(i);
                    e.selection[ch] = i.min(e.curve(ch).len() - 1);
                    e.dirty = true;
                } else if [Key::Left, Key::Right, Key::Up, Key::Down].contains(&key) {
                    let p = e.curve(ch);
                    let lo = if i == 0 { 30 } else { p[i - 1].temperature + 1 };
                    let hi = if i + 1 == p.len() {
                        100
                    } else {
                        p[i + 1].temperature - 1
                    };
                    let p = &mut e.curve_mut(ch)[i];
                    if key == Key::Left {
                        p.temperature = (p.temperature - n).max(lo)
                    }
                    if key == Key::Right {
                        p.temperature = (p.temperature + n).min(hi)
                    }
                    if key == Key::Up {
                        p.duty = (p.duty + n).min(100)
                    }
                    if key == Key::Down {
                        p.duty = (p.duty - n).max(0)
                    }
                    e.dirty = true;
                } else {
                    return false;
                }
                w.redraw();
                true
            }
            _ => false,
        }
    });
    f
}
struct Fields {
    interval: Spinner,
    history_points: Spinner,
    transition: Spinner,
    threshold: Spinner,
    linear: CheckButton,
    soft: CheckButton,
    take: CheckButton,
    force: CheckButton,
    auto: CheckButton,
    autorun: CheckButton,
    close: CheckButton,
    font: Spinner,
    theme: Choice,
    family: Choice,
    installed_fonts: Vec<String>,
}
impl Fields {
    fn selected_family(&self) -> Option<String> {
        self.family
            .value()
            .checked_sub(1)
            .and_then(|i| self.installed_fonts.get(i as usize))
            .cloned()
    }
    fn appearance_dirty(&self, s: &Settings) -> bool {
        self.font.value() as i32 != s.font_size
            || self.history_points.value() as usize != s.history_points
            || selected_theme(&self.theme) != s.theme
            || self.selected_family() != s.font_family
            || self.autorun.value() != s.auto_run
            || self.close.value() != s.close_to_tray
    }
    fn text_size(&mut self, size: i32) {
        UI_TEXT_SIZE.set(size);
        self.theme.set_text_size(size);
        self.family.set_text_size(size);
        for s in [
            &mut self.interval,
            &mut self.history_points,
            &mut self.transition,
            &mut self.threshold,
            &mut self.font,
        ] {
            s.set_text_size(size);
        }
    }
    fn load(&mut self, c: &ControlConfig) {
        self.interval.set_value(c.update_interval * 1000.0);
        self.transition.set_value(c.transition_temp as f64);
        self.threshold.set_value(c.force_temp as f64);
        self.linear.set_value(c.linear);
        self.soft.set_value(c.soft_control);
        self.take.set_value(c.take_over);
    }
    fn read(&self, e: &mut Editor) {
        e.draft.update_interval = self.interval.value() / 1000.0;
        e.draft.transition_temp = self.transition.value() as i32;
        e.draft.force_temp = self.threshold.value() as i32;
        e.draft.linear = self.linear.value();
        e.draft.soft_control = self.soft.value();
    }
}
pub fn startup_error(message: &str) {
    let _app = app::App::default().with_scheme(app::Scheme::Base);
    install_control_style();
    apply_family(None);
    apply_theme(Theme::System);
    let (_native_tx, native_rx) = mpsc::channel();
    let (tx, _) = app::channel::<Action>();
    dialogs::alert(&native_rx, tx, message);
}
pub fn run(
    mut settings: Settings,
    path: PathBuf,
    mut worker: Worker,
    load_error: Option<String>,
    demo: bool,
    smoke: bool,
) {
    let _application = app::App::default().with_scheme(app::Scheme::Base);
    install_control_style();
    UI_TEXT_SIZE.set(settings.font_size * 4 / 3);
    let mut installed_fonts: Vec<String> = app::get_font_names()
        .into_iter()
        .filter_map(|name| name.strip_prefix(' ').map(str::to_owned))
        .filter(|name| !name.starts_with('@'))
        .collect();
    installed_fonts.sort();
    installed_fonts.dedup();
    // Preserve a saved choice if a font was removed, allowing Windows font substitution.
    if let Some(name) = &settings.font_family
        && !installed_fonts.contains(name)
    {
        installed_fonts.push(name.clone());
    }
    apply_family(settings.font_family.clone());
    apply_theme(settings.theme);
    app::set_visible_focus(true);
    let (tx, rx) = app::channel::<Action>();
    let editor = Rc::new(RefCell::new(Editor {
        draft: settings.global.clone(),
        selection: [0, 0],
        temps: [None, None],
        dirty: false,
        target: None,
    }));
    let mut win = Window::default()
        .with_size(1080, 850)
        .with_label("ClevoFanControl · x64")
        .center_screen();
    if let Ok(icon) = fltk::image::PngImage::from_data(include_bytes!("../assets/Fan.png")) {
        win.set_icon(Some(icon));
    }
    win.set_color(PAPER);
    let mut scroll = Scroll::default_fill();
    scroll.set_type(ScrollType::VerticalAlways);
    let mut root = Flex::default().with_size(win.w() - 20, 820).column();
    root.set_margin(24);
    root.set_pad(12);
    let mut header = Flex::default().row();
    let mut title = label("ClevoFanControl");
    title.set_label_font(Font::HelveticaBold);
    let mut mode = label(if demo {
        "演示模式"
    } else {
        "正在连接硬件…"
    });
    header.fixed(&mode, 270);
    let settings_button = button("设置", Action::Settings, tx);
    header.fixed(&settings_button, 70);
    let about = button("关于", Action::About, tx);
    header.fixed(&about, 60);
    header.end();
    root.fixed(&header, 32);
    let mut top = Flex::default().row();
    top.set_pad(10);
    label("编辑配置");
    let mut choice = Choice::default();
    top.fixed(&choice, 240);
    let manager = button("管理预设", Action::Presets, tx);
    top.fixed(&manager, 120);
    let auto = check("自动匹配进程", settings.auto_switch, Action::Auto, tx);
    top.fixed(&auto, 170);
    let mut running = label("运行：Global");
    top.end();
    root.fixed(&top, 36);
    let mut status = Flex::default().row();
    status.set_pad(24);
    let mut cpu = label("CPU    -- °C    -- %    -- RPM");
    let mut gpu = label("GPU    -- °C    -- %    -- RPM");
    status.end();
    root.fixed(&status, 52);
    let history = Rc::new(RefCell::new(history::History::new(settings.history_points)));
    let mut history_canvases = vec![];
    let mut graphs = Flex::default().row();
    graphs.set_pad(20);
    let mut canvases = vec![];
    let mut numbers = vec![];
    for ch in 0..2 {
        let mut col = Flex::default().column();
        col.set_pad(8);
        let heading = label(if ch == 0 {
            "CPU  /  温度 °C → 占空比 %"
        } else {
            "GPU  /  温度 °C → 占空比 %"
        });
        col.fixed(&heading, 25);
        canvases.push(canvas(ch, editor.clone()));
        let mut row = Flex::default().row();
        row.set_pad(5);
        let mut n = spin(1., 16., 1., Action::Numeric(ch), tx);
        n.set_tooltip("选中节点编号");
        row.fixed(&n, 65);
        let mut temp = spin(30., 100., 45., Action::Numeric(ch), tx);
        temp.set_tooltip("节点温度（°C）");
        let mut duty = spin(0., 100., 40., Action::Numeric(ch), tx);
        duty.set_tooltip("节点占空比（%）");
        let add = button("添加", Action::Node(ch, 1), tx);
        row.fixed(&add, 56);
        let del = button("删除", Action::Node(ch, -1), tx);
        row.fixed(&del, 56);
        let reset = button("重置曲线", Action::Node(ch, 0), tx);
        row.fixed(&reset, 95);
        row.end();
        col.fixed(&row, 32);
        numbers.push((n, temp, duty));
        let caption = section(if ch == 0 {
            "CPU · 实时历史"
        } else {
            "GPU · 实时历史"
        });
        col.fixed(&caption, 22);
        history_canvases.push(history::chart(ch, history.clone()));
        col.end();
    }
    graphs.end();
    let hint = label(
        "选择节点后可输入编号 / 温度 / 占空比。拖动编辑，双击插入，方向键微调，Shift ×5，Delete 删除。",
    );
    root.fixed(&hint, 24);
    let heading = section("控制设置");
    root.fixed(&heading, 22);
    let mut row = Flex::default().row();
    row.set_pad(10);
    label("温度滞回（°C）");
    let transition = spin(
        0.,
        10.,
        settings.global.transition_temp as f64,
        Action::Edit,
        tx,
    );
    row.fixed(&transition, 80);
    label("冷却至低于（°C）");
    let threshold = spin(
        0.,
        100.,
        settings.global.force_temp as f64,
        Action::Edit,
        tx,
    );
    row.fixed(&threshold, 80);
    row.end();
    root.fixed(&row, 34);
    let row = Flex::default().row();
    let lin = check("线性插值", settings.global.linear, Action::Edit, tx);
    let soft = check("平滑调速", settings.global.soft_control, Action::Edit, tx);
    let take = check("软件接管", settings.global.take_over, Action::Takeover, tx);
    let force = check("强制冷却 95%", false, Action::Force, tx);
    row.end();
    root.fixed(&row, 32);
    let mut row = Flex::default().row();
    row.set_pad(10);
    let mut save = button("保存并应用", Action::Save, tx);
    style_button(&mut save, true);
    button("重新加载", Action::Load, tx);
    button("恢复全局", Action::Global, tx);
    button("导入旧配置", Action::Import, tx);
    button("重试连接", Action::Retry, tx);
    button("退出", Action::Exit, tx);
    row.end();
    root.fixed(&row, 38);
    let mut notice = label(
        load_error
            .as_deref()
            .unwrap_or("编辑后点击保存并应用；强制冷却是一次性操作。"),
    );
    notice.set_align(Align::Left | Align::Inside | Align::Wrap);
    root.fixed(&notice, 48);
    root.end();
    scroll.end();
    win.end();
    win.resizable(&scroll);
    let mut content = root.clone();
    scroll.resize_callback(move |s, x, y, w, h| {
        let minimum = if UI_TEXT_SIZE.get() > 16 { 920 } else { 820 };
        content.resize(x, y - s.yposition(), w - 20, h.max(minimum));
    });
    win.size_range(980, 780, 0, 0);
    win.set_callback(move |_| tx.send(Action::Close));
    let mut settings_window = Window::default()
        .with_size(780, 640)
        .with_label("设置")
        .center_screen();
    settings_window.set_color(PAPER);
    let mut settings_root = Flex::default_fill().column();
    settings_root.set_margin(24);
    settings_root.set_pad(14);
    let heading = section("采样与历史");
    settings_root.fixed(&heading, 28);
    let mut row = Flex::default().row();
    row.set_pad(16);
    label("历史采样点数");
    let history_points = spin(
        10.,
        300.,
        settings.history_points as f64,
        Action::HistoryPoints,
        tx,
    );
    row.fixed(&history_points, 150);
    row.end();
    settings_root.fixed(&row, 40);
    let mut row = Flex::default().row();
    row.set_pad(16);
    label("采样间隔（ms）");
    let mut interval = spin(
        200.,
        10000.,
        settings.global.update_interval * 1000.0,
        Action::Edit,
        tx,
    );
    interval.set_step(1.);
    interval.set_tooltip("200–10000 ms/次，保存并应用后生效");
    row.fixed(&interval, 150);
    row.end();
    settings_root.fixed(&row, 40);
    let sampling_note = label("采样间隔应用于当前编辑配置；历史点数应用于全部历史图。");
    settings_root.fixed(&sampling_note, 30);
    let heading = section("窗口与外观");
    settings_root.fixed(&heading, 28);
    let row = Flex::default().row();
    let autorun = check("登录时启动", settings.auto_run, Action::Edit, tx);
    let close = check("关闭时隐藏到托盘", settings.close_to_tray, Action::Edit, tx);
    row.end();
    settings_root.fixed(&row, 40);
    let mut row = Flex::default().row();
    label("字号（pt）");
    let font = spin(8., 16., settings.font_size as f64, Action::Font, tx);
    row.fixed(&font, 150);
    row.end();
    settings_root.fixed(&row, 40);
    let mut row = Flex::default().row();
    let caption = label("界面主题");
    row.fixed(&caption, 180);
    let mut theme = Choice::default();
    theme.add_choice("跟随系统|浅色|深色");
    theme.set_value(theme_value(settings.theme));
    theme.set_callback(move |_| tx.send(Action::Theme));
    row.end();
    settings_root.fixed(&row, 40);
    let mut row = Flex::default().row();
    let caption = label("界面字体");
    row.fixed(&caption, 180);
    let mut family = Choice::default();
    family.add_choice("跟随系统");
    for name in &installed_fonts {
        family.add_choice(&name.replace(['|', '/', '\\', '&'], "_"));
    }
    load_family_choice(&mut family, &settings.font_family, &installed_fonts);
    family.set_callback(move |_| tx.send(Action::Family));
    row.end();
    settings_root.fixed(&row, 40);
    let mut note = label("返回会保留未保存修改；保存并应用会同时保存当前曲线和设置。");
    note.set_align(Align::Left | Align::Inside | Align::Wrap);
    let mut row = Flex::default().row();
    row.set_pad(12);
    let mut save_settings = button("保存并应用", Action::SaveSettings, tx);
    style_button(&mut save_settings, true);
    let mut back = Button::default().with_label("返回");
    style_button(&mut back, false);
    let mut settings_handle = settings_window.clone();
    back.set_callback(move |_| settings_handle.hide());
    row.end();
    settings_root.fixed(&row, 40);
    settings_root.end();
    settings_window.end();
    settings_window.resizable(&settings_root);
    settings_window.size_range(780, 640, 0, 0);
    settings_window.make_modal(true);
    settings_window.set_callback(|w| w.hide());
    let mut fields = Fields {
        interval,
        history_points,
        transition,
        threshold,
        linear: lin,
        soft,
        take,
        force,
        auto,
        autorun,
        close,
        font,
        theme,
        family,
        installed_fonts,
    };
    choice.set_callback(move |_| tx.send(Action::Select));
    let refresh_choices = |c: &mut Choice, s: &Settings, target: &Option<String>| {
        c.clear();
        c.add_choice("Global");
        for p in &s.presets {
            c.add_choice(&p.name.replace(['|', '/', '\\'], "_"));
        }
        let i = target
            .as_ref()
            .and_then(|n| s.presets.iter().position(|p| &p.name == n))
            .map_or(0, |i| i + 1);
        c.set_value(i as i32);
    };
    refresh_choices(&mut choice, &settings, &None);
    apply_font(
        &mut root.as_group().unwrap(),
        (settings.font_size * 4 / 3).max(11),
    );
    apply_font(
        &mut settings_root.as_group().unwrap(),
        settings.font_size * 4 / 3,
    );
    let text_size = settings.font_size * 4 / 3;
    fields.text_size(text_size);
    choice.set_text_size(text_size);
    for (n, t, d) in &mut numbers {
        n.set_text_size(text_size);
        t.set_text_size(text_size);
        d.set_text_size(text_size);
    }
    if settings.font_size > 12 {
        win.resize(win.x(), win.y(), 1200, 940);
    }
    win.show();
    platform::set_window_dark(win.raw_handle() as _, DARK.get().unwrap_or(false));
    let (ntx, nrx) = mpsc::channel();
    let tray = match Tray::start(ntx, worker.tx.clone()) {
        Ok(t) => Some(t),
        Err(e) => {
            notice.set_label(&format!("托盘不可用：{e}"));
            None
        }
    };
    let (saved_tx, saved_rx) = mpsc::channel::<(Settings, Result<(), String>)>();
    let mut saving = false;
    let mut save_draft = true;
    let mut save_selection: Option<Option<String>> = None;
    let mut store_dirty = false;
    let mut exiting = false;
    let mut exit_after_save = false;
    let mut stopped = false;
    let mut last_snapshot: Option<Snapshot> = None;
    let began = Instant::now();
    let mut last_indices = [usize::MAX; 2];
    let mut last_points = [(-1, -1); 2];
    while !stopped {
        app::wait_for(0.05).ok();
        refresh_system_appearance();
        while let Ok(e) = nrx.try_recv() {
            match e {
                NativeEvent::Show => {
                    win.show();
                    let _ = win.take_focus();
                }
                NativeEvent::Hide => {
                    settings_window.hide();
                    win.hide();
                }
                NativeEvent::Exit => tx.send(Action::Exit),
            }
        }
        while let Ok(event) = worker.rx.try_recv() {
            match event {
                WorkerEvent::Sample(sample) => {
                    history.borrow_mut().push(sample);
                    for canvas in &mut history_canvases {
                        canvas.redraw();
                    }
                }
                WorkerEvent::Snapshot(s) => {
                    running.set_label(&format!(
                        "运行：{}",
                        s.active.as_deref().unwrap_or("Global")
                    ));
                    mode.set_label(&format!("{}{}", if demo { "模拟 · " } else { "" }, s.mode));
                    if let Some(e) = &s.error {
                        notice.set_label(e)
                    }
                    let display = |ch: usize| {
                        let name = if ch == 0 { "CPU" } else { "GPU" };
                        if let Some(c) = s.sample.as_ref().and_then(|x| x.channels.get(ch)) {
                            format!(
                                "{name}  {}°C   实际 {}%   {} RPM\n目标 {}% · 节点 {}",
                                c.temperature,
                                c.duty,
                                c.rpm.map_or("--".into(), |r| r.to_string()),
                                s.targets.get(ch).map_or("--".into(), ToString::to_string),
                                s.levels.get(ch).map_or("--".into(), ToString::to_string)
                            )
                        } else {
                            format!("{name}   -- °C    -- %    -- RPM")
                        }
                    };
                    cpu.set_label(&display(0));
                    gpu.set_label(&display(1));
                    let tip = format!(
                        "ClevoFanControl · {}\n{}\n{}",
                        s.mode,
                        display(0).replace('\n', " "),
                        display(1).replace('\n', " ")
                    );
                    if let Some(t) = &tray {
                        t.update(tip)
                    }
                    fields.force.set_value(s.force);
                    {
                        let mut e = editor.borrow_mut();
                        for ch in 0..2 {
                            e.temps[ch] = s
                                .sample
                                .as_ref()
                                .and_then(|s| s.channels.get(ch))
                                .map(|c| c.temperature);
                        }
                    }
                    if win.shown() {
                        for c in canvases.iter_mut().chain(history_canvases.iter_mut()) {
                            c.redraw();
                        }
                    }
                    last_snapshot = Some(s);
                }
                WorkerEvent::CoolingDone => {
                    fields.force.set_value(false);
                    notice.set_label("强制冷却完成，已返回原运行策略。");
                }
                WorkerEvent::Stopped(result) => {
                    if let Err(e) = result {
                        dialogs::alert(&nrx, tx, &format!("退出时未能恢复自动：{e}"));
                    }
                    stopped = true;
                }
            }
        }
        if let Ok((s, result)) = saved_rx.try_recv() {
            saving = false;
            root.activate();
            match result {
                Ok(()) => {
                    settings = s;
                    if save_draft {
                        editor.borrow_mut().dirty = false;
                    }
                    store_dirty = false;
                    let _ = worker.tx.send(Command::Settings(settings.clone()));
                    if let Some(target) = save_selection.take() {
                        let _ = worker.tx.send(Command::Select(target));
                    }
                    notice.set_label("已保存并应用。");
                    if exit_after_save {
                        exiting = true;
                        let _ = worker.tx.send(Command::Exit);
                    }
                }
                Err(e) => {
                    exit_after_save = false;
                    notice.set_label(&e);
                    dialogs::alert(&nrx, tx, &e);
                }
            }
        }
        if smoke && began.elapsed() > Duration::from_secs(3) && !exiting {
            if let Ok(image) = draw::capture_window(&mut win) {
                let mut bytes =
                    format!("P6\n{} {}\n255\n", image.data_w(), image.data_h()).into_bytes();
                bytes.extend_from_slice(&image.to_rgb_data());
                let _ = std::fs::write(std::env::temp_dir().join("clevo-fltk-smoke.ppm"), bytes);
            }
            settings_window.show();
            app::check();
            if let Ok(image) = draw::capture_window(&mut settings_window) {
                let mut bytes =
                    format!("P6\n{} {}\n255\n", image.data_w(), image.data_h()).into_bytes();
                bytes.extend_from_slice(&image.to_rgb_data());
                let _ =
                    std::fs::write(std::env::temp_dir().join("clevo-settings-smoke.ppm"), bytes);
            }
            settings_window.hide();
            dialogs::capture_preview(&format!(
                "演示模式（未连接硬件）\n\n长路径换行检查\nC:\\Program Files\\WindowsApps\\CLEVOCO.FnhotkeysandOSD_7.88.1.0_x64__6h6z29zh29qx0\\FnKey\\InsydeDCHU.dll\n\n配置文件\n{}",
                path.display()
            ));
            exiting = true;
            let _ = worker.tx.send(Command::Exit);
        }
        while let Some(action) = rx.recv() {
            if exiting {
                continue;
            }
            match action {
                Action::Settings => {
                    settings_window.set_label(&format!(
                        "设置 · {}",
                        editor.borrow().target.as_deref().unwrap_or("Global")
                    ));
                    settings_window.show();
                    platform::set_window_dark(
                        settings_window.raw_handle() as _,
                        DARK.get().unwrap_or(false),
                    );
                }
                Action::SaveSettings => {
                    settings_window.hide();
                    tx.send(Action::Save);
                }
                Action::HistoryPoints => {
                    history
                        .borrow_mut()
                        .set_capacity(fields.history_points.value() as usize);
                    for chart in &mut history_canvases {
                        chart.redraw();
                    }
                }
                Action::Edit => {
                    let mut e = editor.borrow_mut();
                    fields.read(&mut e);
                    e.dirty = true;
                }
                Action::Family => {
                    apply_family(fields.selected_family());
                }
                Action::Theme => {
                    apply_theme(selected_theme(&fields.theme));
                }
                Action::Font => {
                    editor.borrow_mut().dirty = true;
                    let size = fields.font.value() as i32;
                    UI_TEXT_SIZE.set(size * 4 / 3);
                    apply_font(&mut root.as_group().unwrap(), size * 4 / 3);
                    apply_font(&mut settings_root.as_group().unwrap(), size * 4 / 3);
                    fields.text_size(size * 4 / 3);
                    choice.set_text_size(size * 4 / 3);
                    for (n, t, d) in &mut numbers {
                        n.set_text_size(size * 4 / 3);
                        t.set_text_size(size * 4 / 3);
                        d.set_text_size(size * 4 / 3);
                    }
                    if size > 12 {
                        win.resize(win.x(), win.y(), 1200, 940);
                    }
                    win.redraw();
                }
                Action::Numeric(ch) => {
                    let mut e = editor.borrow_mut();
                    let i = (numbers[ch].0.value() as usize)
                        .saturating_sub(1)
                        .min(e.curve(ch).len() - 1);
                    if i != e.selection[ch] {
                        e.selection[ch] = i;
                    } else {
                        let mut curve = e.curve(ch).clone();
                        curve[i] = Point {
                            temperature: numbers[ch].1.value() as i32,
                            duty: numbers[ch].2.value() as i32,
                        };
                        if config::validate_curve(&curve).is_ok() {
                            *e.curve_mut(ch) = curve;
                            e.dirty = true;
                        } else {
                            notice.set_label("温度必须严格递增；输入已恢复。");
                        }
                    }
                    last_indices[ch] = usize::MAX;
                }
                Action::Node(ch, op) => {
                    let mut e = editor.borrow_mut();
                    let i = e.selection[ch];
                    let p = e.curve(ch);
                    if op == 0 {
                        *e.curve_mut(ch) = config::default_curve();
                        e.selection[ch] = 0;
                        e.dirty = true;
                    } else if op < 0 && p.len() > 2 {
                        e.curve_mut(ch).remove(i);
                        e.selection[ch] = i.min(e.curve(ch).len() - 1);
                        e.dirty = true;
                    } else if op > 0 && p.len() < 16 {
                        let t = if i + 1 < p.len() {
                            (p[i].temperature + p[i + 1].temperature) / 2
                        } else {
                            p[i].temperature + 5
                        };
                        if t <= 100 && t > p[i].temperature {
                            let duty = linear(p, t);
                            e.curve_mut(ch).insert(
                                i + 1,
                                Point {
                                    temperature: t,
                                    duty,
                                },
                            );
                            e.selection[ch] = i + 1;
                            e.dirty = true;
                        } else {
                            notice.set_label("相邻温度之间没有可插入的整数节点。");
                        }
                    }
                    last_indices[ch] = usize::MAX;
                }
                Action::Takeover => {
                    let v = fields.take.value();
                    editor.borrow_mut().draft.take_over = v;
                    editor.borrow_mut().dirty = true;
                    let _ = worker.tx.send(Command::Takeover(v));
                    notice.set_label("接管开关已生效，曲线编辑仍需保存并应用。");
                }
                Action::Force => {
                    let _ = worker.tx.send(Command::Force(fields.force.value()));
                }
                Action::Auto => {
                    settings.auto_switch = fields.auto.value();
                    store_dirty = true;
                    let _ = worker.tx.send(Command::AutoSwitch(settings.auto_switch));
                }
                Action::Save | Action::SavePresets => {
                    if saving {
                        continue;
                    }
                    save_draft = matches!(action, Action::Save);
                    let e = editor.borrow();
                    if save_draft {
                        save_selection = Some(e.target.clone());
                    }
                    let mut candidate = settings.clone();
                    if save_draft {
                        candidate.font_size = fields.font.value() as i32;
                        candidate.history_points = fields.history_points.value() as usize;
                        candidate.theme = selected_theme(&fields.theme);
                        candidate.font_family = fields.selected_family();
                        candidate.auto_run = fields.autorun.value();
                        candidate.close_to_tray = fields.close.value();
                        if let Some(name) = &e.target {
                            if let Some(p) = candidate.presets.iter_mut().find(|p| &p.name == name)
                            {
                                p.control = e.draft.clone();
                            } else {
                                notice.set_label("编辑的预设已被删除，请重新选择。");
                                continue;
                            }
                        } else {
                            candidate.global = e.draft.clone();
                        }
                    }
                    drop(e);
                    if let Err(err) = candidate.validate() {
                        dialogs::alert(&nrx, tx, &err);
                        continue;
                    }
                    saving = true;
                    root.deactivate();
                    notice.set_label("正在保存设置…");
                    let result_tx = saved_tx.clone();
                    let p = path.clone();
                    let previous = settings.clone();
                    std::thread::spawn(move || {
                        let changed = candidate.auto_run != previous.auto_run;
                        let result = (|| {
                            if save_draft && (changed || candidate.auto_run) {
                                platform::set_autorun(candidate.auto_run)
                                    .map_err(|e| format!("更新登录启动任务失败：{e}"))?;
                            }
                            if let Err(e) = candidate.save(&p) {
                                if save_draft
                                    && changed
                                    && let Err(rollback) = platform::set_autorun(previous.auto_run)
                                {
                                    return Err(format!("{e}；自启动回滚失败：{rollback}"));
                                }
                                return Err(e);
                            }
                            Ok(())
                        })();
                        let _ = result_tx.send((candidate, result));
                    });
                }
                Action::Load => {
                    if (editor.borrow().dirty || fields.appearance_dirty(&settings))
                        && dialogs::confirm(
                            &nrx,
                            tx,
                            "放弃当前未保存编辑并重新加载？",
                            "取消",
                            "重新加载",
                            "",
                        ) != Some(1)
                    {
                        continue;
                    }
                    match Settings::load(&path) {
                        Ok(s) => {
                            settings = s;
                            {
                                let mut e = editor.borrow_mut();
                                e.draft = settings.global.clone();
                                e.target = None;
                                e.dirty = false;
                                e.selection = [0, 0];
                            }
                            fields.load(&settings.global);
                            fields.font.set_value(settings.font_size as f64);
                            fields
                                .history_points
                                .set_value(settings.history_points as f64);
                            history.borrow_mut().set_capacity(settings.history_points);
                            fields.text_size(settings.font_size * 4 / 3);
                            apply_font(&mut root.as_group().unwrap(), settings.font_size * 4 / 3);
                            apply_font(
                                &mut settings_root.as_group().unwrap(),
                                settings.font_size * 4 / 3,
                            );
                            fields.theme.set_value(theme_value(settings.theme));
                            apply_theme(settings.theme);
                            load_family_choice(
                                &mut fields.family,
                                &settings.font_family,
                                &fields.installed_fonts,
                            );
                            apply_family(settings.font_family.clone());
                            fields.autorun.set_value(settings.auto_run);
                            fields.close.set_value(settings.close_to_tray);
                            fields.auto.set_value(settings.auto_switch);
                            let _ = worker.tx.send(Command::Settings(settings.clone()));
                            let _ = worker.tx.send(Command::Select(None));
                            refresh_choices(&mut choice, &settings, &None);
                        }
                        Err(e) => dialogs::alert(&nrx, tx, &e),
                    }
                }
                Action::Global | Action::Select => {
                    if editor.borrow().dirty
                        && dialogs::confirm(
                            &nrx,
                            tx,
                            "切换编辑对象将放弃当前草稿。",
                            "取消",
                            "切换",
                            "",
                        ) != Some(1)
                    {
                        refresh_choices(&mut choice, &settings, &editor.borrow().target);
                        continue;
                    }
                    let target = if matches!(action, Action::Global) {
                        None
                    } else {
                        settings
                            .presets
                            .get((choice.value() - 1) as usize)
                            .map(|p| p.name.clone())
                    };
                    let cfg = target
                        .as_ref()
                        .and_then(|a| settings.presets.iter().find(|p| &p.name == a))
                        .map(|p| p.control.clone())
                        .unwrap_or_else(|| settings.global.clone());
                    {
                        let mut e = editor.borrow_mut();
                        e.target = target.clone();
                        e.draft = cfg.clone();
                        e.selection = [0, 0];
                        e.dirty = false;
                    }
                    fields.load(&cfg);
                    refresh_choices(&mut choice, &settings, &target);
                    let _ = worker.tx.send(Command::Select(target));
                }
                Action::Import => {
                    let mut chooser =
                        dialog::NativeFileChooser::new(dialog::NativeFileChooserType::BrowseFile);
                    chooser.set_filter("JSON\t*.json");
                    chooser.show();
                    let file = chooser.filename();
                    if file.is_file() {
                        match config::import_legacy(&file) {
                            Ok(s) => {
                                settings = s;
                                let mut e = editor.borrow_mut();
                                e.draft = settings.global.clone();
                                e.target = None;
                                e.selection = [0, 0];
                                e.dirty = true;
                                fields.load(&e.draft);
                                fields.autorun.set_value(false);
                                fields.auto.set_value(false);
                                fields.close.set_value(settings.close_to_tray);
                                fields.font.set_value(settings.font_size as f64);
                                fields
                                    .history_points
                                    .set_value(settings.history_points as f64);
                                history.borrow_mut().set_capacity(settings.history_points);
                                fields.text_size(settings.font_size * 4 / 3);
                                apply_font(
                                    &mut root.as_group().unwrap(),
                                    settings.font_size * 4 / 3,
                                );
                                apply_font(
                                    &mut settings_root.as_group().unwrap(),
                                    settings.font_size * 4 / 3,
                                );
                                fields.theme.set_value(theme_value(settings.theme));
                                apply_theme(settings.theme);
                                load_family_choice(
                                    &mut fields.family,
                                    &settings.font_family,
                                    &fields.installed_fonts,
                                );
                                apply_family(settings.font_family.clone());
                                refresh_choices(&mut choice, &settings, &None);
                                notice.set_label("已导入草稿；接管、强冷、自启动与自动规则均未启用。保存后生效。");
                            }
                            Err(e) => dialogs::alert(&nrx, tx, &e),
                        }
                    }
                }
                Action::Presets => {
                    let draft = editor.borrow().draft.clone();
                    if let Some((s, use_name)) = preset_manager(settings.clone(), draft, &nrx, tx) {
                        settings = s;
                        {
                            let mut e = editor.borrow_mut();
                            if e.target.as_ref().is_some_and(|name| {
                                !settings.presets.iter().any(|p| &p.name == name)
                            }) {
                                e.target = None;
                                e.draft = settings.global.clone();
                                e.selection = [0, 0];
                                fields.load(&e.draft);
                            }
                        }
                        store_dirty = true;
                        refresh_choices(&mut choice, &settings, &editor.borrow().target);
                        save_selection = use_name.map(Some);
                        tx.send(Action::SavePresets);
                    }
                }
                Action::Retry => {
                    let _ = worker.tx.send(Command::Retry);
                    notice.set_label("正在重新连接…");
                }
                Action::About => {
                    let details = format!(
                        "{}\n\n配置文件\n{}",
                        last_snapshot
                            .as_ref()
                            .map(|s| s.description.as_str())
                            .unwrap_or("硬件未连接"),
                        path.display()
                    );
                    dialogs::about(&details, &nrx, tx);
                }
                Action::Close if fields.close.value() && tray.is_some() => win.hide(),
                Action::Close | Action::Exit => {
                    settings_window.hide();
                    if saving {
                        notice.set_label("请等待保存完成再退出。");
                        continue;
                    }
                    if editor.borrow().dirty || store_dirty || fields.appearance_dirty(&settings) {
                        match dialogs::confirm(
                            &nrx,
                            tx,
                            "有未保存修改。",
                            "取消",
                            "保存后退出",
                            "不保存退出",
                        ) {
                            Some(1) => {
                                exit_after_save = true;
                                tx.send(Action::Save);
                                continue;
                            }
                            Some(2) => {}
                            _ => continue,
                        }
                    }
                    exiting = true;
                    notice.set_label("正在恢复固件自动并退出…");
                    let _ = worker.tx.send(Command::Exit);
                }
            }
        }
        {
            let e = editor.borrow();
            let title = format!(
                "ClevoFanControl · {}{}{}",
                e.target.as_deref().unwrap_or("Global"),
                if e.dirty || store_dirty || fields.appearance_dirty(&settings) {
                    " *"
                } else {
                    ""
                },
                if demo { " · 演示" } else { "" }
            );
            if win.label() != title {
                win.set_label(&title);
            }
            for ch in 0..2 {
                let i = e.selection[ch];
                let p = &e.curve(ch)[i];
                if last_indices[ch] != i || last_points[ch] != (p.temperature, p.duty) {
                    numbers[ch].0.set_value((i + 1) as f64);
                    numbers[ch].1.set_value(p.temperature as f64);
                    numbers[ch].2.set_value(p.duty as f64);
                    last_points[ch] = (p.temperature, p.duty);
                }
                last_indices[ch] = i;
            }
        }
    }
    worker.join();
    drop(tray);
    settings_window.hide();
    win.hide();
}

fn preset_manager(
    mut settings: Settings,
    draft: ControlConfig,
    native: &mpsc::Receiver<NativeEvent>,
    parent: app::Sender<Action>,
) -> Option<(Settings, Option<String>)> {
    let (tx, rx) = mpsc::channel::<i32>();
    let mut w = Window::default()
        .with_size(740, 530)
        .with_label("预设 · 从上到下匹配，第一个命中生效")
        .center_screen();
    let mut col = Flex::default_fill().column();
    col.set_margin(20);
    col.set_pad(12);
    let mut list = HoldBrowser::default();
    list.set_format_char('\0');
    list.set_text_size(settings.font_size * 4 / 3);
    let t = tx.clone();
    list.set_callback(move |_| {
        let _ = t.send(10);
    });
    let mut row = Flex::default().row();
    label("名称");
    let mut name = Input::default();
    row.fixed(&name, 520);
    row.end();
    col.fixed(&row, 32);
    let mut row = Flex::default().row();
    label("进程文件名规则");
    let mut rule = Input::default();
    name.set_text_size(settings.font_size * 4 / 3);
    rule.set_text_size(settings.font_size * 4 / 3);
    row.fixed(&rule, 520);
    row.end();
    col.fixed(&row, 32);
    let mut row = Flex::default().row();
    row.set_pad(6);
    for (i, s) in ["从当前草稿新建", "保存规则", "删除", "上移", "下移"]
        .iter()
        .enumerate()
    {
        let mut b = Button::default().with_label(s);
        style_button(&mut b, i == 0);
        let t = tx.clone();
        b.set_callback(move |_| {
            let _ = t.send(i as i32);
        });
    }
    row.end();
    col.fixed(&row, 36);
    let note = label("规则示例：game.exe、*render*、游戏?.exe。所有运行进程参与匹配。");
    col.fixed(&note, 26);
    let mut row = Flex::default().row();
    row.set_pad(10);
    for (i, s) in ["使用选中项", "确定", "取消"].iter().enumerate() {
        let mut b = Button::default().with_label(s);
        style_button(&mut b, i == 0);
        let t = tx.clone();
        b.set_callback(move |_| {
            let _ = t.send(5 + i as i32);
        });
    }
    row.end();
    col.fixed(&row, 36);
    col.end();
    w.end();
    w.resizable(&col);
    w.make_modal(true);
    let t = tx.clone();
    w.set_callback(move |_| {
        let _ = t.send(7);
    });
    apply_font(&mut col.as_group().unwrap(), settings.font_size * 4 / 3);
    w.show();
    platform::set_window_dark(w.raw_handle() as _, DARK.get().unwrap_or(false));
    let refresh = |list: &mut HoldBrowser, s: &Settings, i: i32| {
        list.clear();
        for p in &s.presets {
            list.add(&format!("{}    [{}]", p.name, p.process_pattern));
        }
        list.select(i.min(s.presets.len() as i32));
    };
    refresh(&mut list, &settings, 1);
    let _ = tx.send(10);
    while w.shown() {
        app::wait_for(0.05).ok();
        refresh_system_appearance();
        while let Ok(e) = native.try_recv() {
            match e {
                NativeEvent::Exit => {
                    w.hide();
                    parent.send(Action::Exit);
                    return None;
                }
                NativeEvent::Show => w.show(),
                _ => {}
            }
        }
        if let Ok(a) = rx.try_recv() {
            let i = list.value().saturating_sub(1) as usize;
            match a {
                0 => {
                    if settings.presets.len() >= 32 {
                        dialogs::alert(native, parent, "最多 32 个预设");
                        continue;
                    }
                    let mut n = 1;
                    while settings
                        .presets
                        .iter()
                        .any(|p| p.name == format!("Preset {n}"))
                    {
                        n += 1;
                    }
                    settings.presets.push(Preset {
                        name: format!("Preset {n}"),
                        process_pattern: "*".into(),
                        control: draft.clone(),
                    });
                    refresh(&mut list, &settings, settings.presets.len() as i32);
                    let _ = tx.send(10);
                }
                1 | 5 | 6 => {
                    if i < settings.presets.len() {
                        let old = settings.presets[i].clone();
                        settings.presets[i].name = name.value();
                        settings.presets[i].process_pattern = rule.value();
                        if let Err(e) = settings.validate() {
                            settings.presets[i] = old;
                            dialogs::alert(native, parent, &e);
                            continue;
                        }
                    }
                    if a == 1 {
                        refresh(&mut list, &settings, (i + 1) as i32)
                    } else {
                        let selected = if a == 5 {
                            settings.presets.get(i).map(|p| p.name.clone())
                        } else {
                            None
                        };
                        w.hide();
                        return Some((settings, selected));
                    }
                }
                2 => {
                    if i < settings.presets.len() {
                        settings.presets.remove(i);
                        refresh(
                            &mut list,
                            &settings,
                            ((i + 1).min(settings.presets.len())) as i32,
                        );
                        let _ = tx.send(10);
                    }
                }
                3 | 4 => {
                    let j = if a == 3 { i.saturating_sub(1) } else { i + 1 };
                    if i < settings.presets.len() && j < settings.presets.len() {
                        settings.presets.swap(i, j);
                        refresh(&mut list, &settings, (j + 1) as i32);
                        let _ = tx.send(10);
                    }
                }
                7 => {
                    w.hide();
                    return None;
                }
                10 => {
                    if let Some(p) = settings.presets.get(i) {
                        name.set_value(&p.name);
                        rule.set_value(&p.process_pattern);
                    } else {
                        name.set_value("");
                        rule.set_value("");
                    }
                }
                _ => {}
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn existing_widgets_follow_palette_and_font_changes() {
        let _app = app::App::default();
        let mut widget = Frame::default();
        widget.set_color(SURFACE);
        widget.set_label_color(INK);
        apply_theme(Theme::Light);
        assert_eq!(widget.color().to_rgb(), (255, 255, 255));
        assert_eq!(widget.label_color().to_rgb(), (32, 42, 56));
        apply_theme(Theme::Dark);
        assert_eq!(widget.color().to_rgb(), (36, 43, 53));
        assert_eq!(widget.label_color().to_rgb(), (233, 238, 245));
        apply_theme(Theme::Light);
        assert_eq!(widget.color().to_rgb(), (255, 255, 255));
        apply_theme(Theme::System);
        assert_eq!(DARK.get(), Some(platform::system_dark_mode()));
        apply_family(Some("Arial".into()));
        assert_eq!(
            APPLIED_FONT.with_borrow(Clone::clone).as_deref(),
            Some("Arial")
        );
        apply_family(None);
        dialogs::verify_dialog_contract();
        if let Some(system) = platform::system_font_family() {
            assert_eq!(APPLIED_FONT.with_borrow(Clone::clone), Some(system));
        }
    }
}
