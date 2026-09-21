// Hallmark · bounded native dialogs; shared Cobalt tokens and system font.
use super::*;
use fltk::text::{TextBuffer, TextDisplay, WrapMode};

struct DialogView {
    window: Window,
    result: Rc<Cell<Option<usize>>>,
}
impl DialogView {
    fn new(title: &str, body: &str, details: Option<&str>, buttons: &[&str]) -> Self {
        let size = UI_TEXT_SIZE.get();
        let (_, _, screen_w, screen_h) = app::screen_work_area(0);
        let width = (if details.is_some() { 720 } else { 580 }).min(screen_w - 48);
        let height = (if details.is_some() {
            440 + size * 4
        } else {
            240 + size * 3
        })
        .min(screen_h - 72);
        let mut window = Window::default()
            .with_size(width, height)
            .with_label(title)
            .center_screen();
        window.set_color(PAPER);
        let mut column = Flex::default_fill().column();
        column.set_margin(24);
        column.set_pad(16);
        let mut heading = label(title);
        heading.set_label_font(Font::HelveticaBold);
        heading.set_label_size(size + 5);
        column.fixed(&heading, size + 16);
        let mut content = TextDisplay::default();
        content.set_frame(FrameType::NoBox);
        content.set_color(PAPER);
        content.set_text_color(INK);
        content.set_text_font(Font::Helvetica);
        content.set_text_size(size);
        content.wrap_mode(WrapMode::AtBounds, 0);
        let mut body_buffer = TextBuffer::default();
        body_buffer.set_text(body);
        content.set_buffer(body_buffer);
        if let Some(details) = details {
            column.fixed(&content, size * 4 + 12);
            let mut caption = section("硬件与配置信息");
            caption.set_label_size(size);
            column.fixed(&caption, size + 6);
            let mut info = TextDisplay::default();
            info.set_frame(FrameType::DownBox);
            info.set_color(SURFACE);
            info.set_text_color(MUTED);
            info.set_text_font(Font::Helvetica);
            info.set_text_size(size);
            info.wrap_mode(WrapMode::AtBounds, 0);
            let mut buffer = TextBuffer::default();
            buffer.set_text(details);
            info.set_buffer(buffer);
        }
        let mut row = Flex::default().row();
        row.set_pad(12);
        if let Some(details) = details {
            let mut copy = Button::default().with_label("复制信息");
            style_button(&mut copy, false);
            copy.set_label_size(size);
            row.fixed(&copy, (size * 6).max(112));
            let text = details.to_string();
            copy.set_callback(move |b| {
                app::copy(&text);
                b.set_label("已复制");
            });
        }
        Frame::default();
        let result = Rc::new(Cell::new(None));
        for (i, text) in buttons.iter().enumerate().filter(|(_, s)| !s.is_empty()) {
            // Plain Button deliberately avoids ReturnButton's decorative return-arrow glyph.
            let mut button = Button::default().with_label(text);
            style_button(&mut button, i == 1 || buttons.len() == 1);
            button.set_label_size(size);
            row.fixed(&button, (text.chars().count() as i32 * size + 32).max(104));
            if buttons.len() == 1 {
                button.set_shortcut(Shortcut::None | Key::Enter);
            }
            let answer = result.clone();
            let mut w = window.clone();
            button.set_callback(move |_| {
                answer.set(Some(i));
                w.hide();
            });
        }
        row.end();
        column.fixed(&row, 40);
        column.end();
        window.end();
        window.resizable(&column);
        window.size_range(width.min(480), height.min(300), 0, 0);
        window.make_modal(true);
        window.set_callback(|w| w.hide());
        Self { window, result }
    }
    fn show(&mut self) {
        self.window.show();
        platform::set_window_dark(self.window.raw_handle() as _, DARK.get().unwrap_or(false));
    }
    fn run(
        mut self,
        native: &mpsc::Receiver<NativeEvent>,
        parent: app::Sender<Action>,
    ) -> Option<usize> {
        self.show();
        while self.window.shown() {
            app::wait_for(0.05).ok();
            refresh_system_appearance();
            while let Ok(event) = native.try_recv() {
                match event {
                    NativeEvent::Exit => {
                        self.window.hide();
                        parent.send(Action::Exit);
                    }
                    NativeEvent::Show => self.window.show(),
                    _ => {}
                }
            }
        }
        let answer = self.result.get();
        Window::delete(self.window);
        answer
    }
}
pub(super) fn alert(native: &mpsc::Receiver<NativeEvent>, parent: app::Sender<Action>, body: &str) {
    DialogView::new("操作未完成", body, None, &["关闭"]).run(native, parent);
}
pub(super) fn confirm(
    native: &mpsc::Receiver<NativeEvent>,
    parent: app::Sender<Action>,
    body: &str,
    cancel: &str,
    accept: &str,
    alternate: &str,
) -> Option<usize> {
    DialogView::new("确认操作", body, None, &[cancel, accept, alternate]).run(native, parent)
}
pub(super) fn about(
    details: &str,
    native: &mpsc::Receiver<NativeEvent>,
    parent: app::Sender<Action>,
) {
    DialogView::new(
        "关于 ClevoFanControl",
        "2.0.0 · Windows x64\n自定义风扇曲线，通过原厂 ACPI 接口控制。",
        Some(details),
        &["关闭"],
    )
    .run(native, parent);
}
// Used only by the explicitly requested --smoke UI check; never opens a real hardware connection.
pub(super) fn capture_preview(details: &str) {
    let mut view = DialogView::new(
        "关于 ClevoFanControl",
        "2.0.0 · Windows x64\n自定义风扇曲线，通过原厂 ACPI 接口控制。",
        Some(details),
        &["关闭"],
    );
    view.show();
    app::check();
    if let Ok(image) = draw::capture_window(&mut view.window) {
        let mut bytes = format!("P6\n{} {}\n255\n", image.data_w(), image.data_h()).into_bytes();
        bytes.extend_from_slice(&image.to_rgb_data());
        let _ = std::fs::write(std::env::temp_dir().join("clevo-fltk-about.ppm"), bytes);
    }
    view.window.hide();
    Window::delete(view.window);
}

#[cfg(test)]
pub(super) fn verify_dialog_contract() {
    fn find_button(group: &Group, label: &str) -> Option<fltk::widget::Widget> {
        for i in 0..group.children() {
            let w = group.child(i).unwrap();
            if w.label() == label {
                return Some(w);
            }
            if let Some(g) = w.as_group()
                && let Some(w) = find_button(&g, label)
            {
                return Some(w);
            }
        }
        None
    }
    UI_TEXT_SIZE.set(21);
    let dialog = DialogView::new(
        "确认操作",
        "有未保存修改。",
        None,
        &["取消", "保存后退出", "不保存退出"],
    );
    let mut button = find_button(&dialog.window.as_group().unwrap(), "保存后退出").unwrap();
    assert!(button.x() >= 0 && button.x() + button.w() <= dialog.window.w());
    assert!(button.w() >= 5 * 21);
    button.do_callback();
    assert_eq!(dialog.result.get(), Some(1));
    Window::delete(dialog.window);
    let mut dialog = DialogView::new("操作未完成", "测试消息", None, &["关闭"]);
    dialog.window.do_callback();
    assert_eq!(dialog.result.get(), None);
    Window::delete(dialog.window);
    UI_TEXT_SIZE.set(13);
}
