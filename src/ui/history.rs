// Live telemetry: timestamped samples, three explicitly labelled scales.
use super::*;
use clevo_fan_control::hardware::{Reading, Sample};
use std::collections::VecDeque;
const TEMP: Color = Color::from_rgbi(22);
const RPM: Color = Color::from_rgbi(23);
const DUTY: Color = Color::from_rgbi(24);
pub(super) struct History {
    points: VecDeque<Option<Sample>>,
    capacity: usize,
}
impl History {
    pub fn new(capacity: usize) -> Self {
        Self {
            points: VecDeque::new(),
            capacity,
        }
    }
    pub fn set_capacity(&mut self, capacity: usize) {
        self.capacity = capacity;
        while self.points.len() > capacity {
            self.points.pop_front();
        }
    }
    // Only sampling events advance history, including failures; UI snapshots do not.
    pub fn push(&mut self, sample: Option<Sample>) {
        self.points.push_back(sample);
        self.set_capacity(self.capacity);
    }
    fn metric_points(&self, ch: usize, metric: usize) -> impl Iterator<Item = (usize, f64)> + '_ {
        self.points
            .iter()
            .enumerate()
            .filter_map(move |(index, sample)| {
                Some((index, value(sample.as_ref()?.channels.get(ch)?, metric)?))
            })
    }
    fn rpm_max(&self, ch: usize) -> u32 {
        self.points
            .iter()
            .filter_map(|s| s.as_ref()?.channels.get(ch)?.rpm)
            .max()
            .unwrap_or(0)
            .div_ceil(1000)
            .max(6)
            * 1000
    }
}
fn point_x(index: usize, length: usize, capacity: usize, left: i32, width: i32) -> i32 {
    left + width
        - ((length - 1 - index) as f64 * width as f64 / (capacity - 1) as f64).round() as i32
}
fn sample_index(x: i32, length: usize, capacity: usize, left: i32, width: i32) -> Option<usize> {
    if width <= 0 || x < left || x > left + width {
        return None;
    }
    let offset =
        ((left + width - x) as f64 * (capacity - 1) as f64 / width as f64).round() as usize;
    length.checked_sub(offset + 1)
}
pub(super) fn colors(dark: bool) {
    let values = if dark {
        [0xffbc70, 0x78a7ff, 0x67d9ac]
    } else {
        [0xa34e00, 0x245edb, 0x007b54]
    };
    for (color, value) in [TEMP, RPM, DUTY].into_iter().zip(values) {
        let (r, g, b) = Color::from_hex(value).to_rgb();
        app::set_color(color, r, g, b);
    }
}
pub(super) fn chart(ch: usize, history: Rc<RefCell<History>>) -> Frame {
    let mut frame = Frame::default();
    let hover = Rc::new(Cell::new(None::<(i32, i32)>));
    let draw_hover = hover.clone();
    frame.draw(move |w| {
        draw::push_clip(w.x(), w.y(), w.w(), w.h());
        draw::draw_rect_fill(w.x(), w.y(), w.w(), w.h(), SURFACE);
        let history = history.borrow();
        let rpm_max = history.rpm_max(ch);
        draw::set_font(Font::Helvetica, UI_TEXT_SIZE.get().clamp(12, 14));
        let latest = history
            .points
            .back()
            .and_then(|s| s.as_ref())
            .and_then(|s| s.channels.get(ch));
        let metrics = [
            (0, TEMP, 125.0, "°C"),
            (1, RPM, rpm_max as f64, " RPM"),
            (2, DUTY, 100.0, "%"),
        ];
        let labels: Vec<_> = metrics
            .iter()
            .filter_map(|&(metric, color, max, unit)| {
                let value = value(latest?, metric)?;
                Some((color, max, value, format!("{value:.0}{unit}")))
            })
            .collect();
        let label_width = labels
            .iter()
            .map(|(_, _, _, text)| draw::measure(text, false).0)
            .chain([draw::measure("20000 RPM", false).0])
            .max()
            .unwrap_or(0);
        let left = w.x() + 12;
        let top = w.y() + 32;
        let legend_width = w.w() - 24;
        let width = legend_width - label_width - 14;
        let height = w.h() - 58;
        if width <= 0 || height <= 0 {
            draw::pop_clip();
            return;
        }
        draw::set_font(Font::Helvetica, UI_TEXT_SIZE.get().clamp(12, 14));
        for (i, (color, text)) in [
            (TEMP, "温度 0–125°C".to_string()),
            (RPM, format!("RPM 0–{rpm_max}")),
            (DUTY, "实际 0–100%".to_string()),
        ]
        .into_iter()
        .enumerate()
        {
            let x = left + i as i32 * legend_width / 3;
            draw::set_draw_color(color);
            draw::draw_rect_fill(x, w.y() + 14, 12, 3, color);
            draw::draw_text2(
                &text,
                x + 17,
                w.y() + 5,
                legend_width / 3 - 17,
                22,
                Align::Left | Align::Inside,
            );
        }
        draw::set_draw_color(GRID);
        for i in 0..=4 {
            let y = top + height * i / 4;
            draw::draw_line(left, y, left + width, y);
        }
        for i in 0..=6 {
            let x = left + width * i / 6;
            draw::draw_line(x, top, x, top + height);
        }
        draw::set_draw_color(MUTED);
        draw::draw_text2(
            &format!("最近 {} 点", history.capacity),
            left,
            top + height + 4,
            100,
            20,
            Align::Left | Align::Inside,
        );
        draw::draw_text2(
            "最新",
            left + width - 65,
            top + height + 4,
            65,
            20,
            Align::Right | Align::Inside,
        );
        draw::push_clip(left, top, width + 1, height + 1);
        for (metric, color, max) in [(0, TEMP, 125.0), (1, RPM, rpm_max as f64), (2, DUTY, 100.0)] {
            draw::set_draw_color(color);
            draw::set_line_style(draw::LineStyle::Solid, 2);
            let mut previous: Option<(usize, i32, i32)> = None;
            for (index, value) in history.metric_points(ch, metric) {
                let x = point_x(index, history.points.len(), history.capacity, left, width);
                let y = top + height - (value / max * height as f64).round() as i32;
                if let Some((previous_index, px, py)) = previous {
                    draw::set_line_style(
                        if index > previous_index + 1 {
                            draw::LineStyle::Dash
                        } else {
                            draw::LineStyle::Solid
                        },
                        2,
                    );
                    draw::draw_line(px, py, x, y);
                }
                draw::draw_rect_fill(x - 1, y - 1, 3, 3, color);
                previous = Some((index, x, y));
            }
        }
        draw::set_line_style(draw::LineStyle::Solid, 0);
        draw::pop_clip();
        // Keep nearby values readable while linking each label to its actual endpoint.
        let label_height = draw::height() + 2;
        let mut labels: Vec<_> = labels
            .into_iter()
            .map(|(color, max, value, text)| {
                let y = top + height - (value / max * height as f64).round() as i32;
                (color, text, y, y.clamp(top, top + height))
            })
            .collect();
        labels.sort_by_key(|label| label.3);
        for i in 1..labels.len() {
            labels[i].3 = labels[i].3.max(labels[i - 1].3 + label_height);
        }
        if let Some(last) = labels.last_mut() {
            last.3 = last.3.min(top + height);
        }
        for i in (0..labels.len().saturating_sub(1)).rev() {
            labels[i].3 = labels[i].3.min(labels[i + 1].3 - label_height);
        }
        for (color, text, endpoint_y, label_y) in labels {
            draw::set_draw_color(color);
            draw::draw_line(left + width, endpoint_y, left + width + 10, label_y);
            draw::draw_text2(
                &text,
                left + width + 14,
                label_y - label_height / 2,
                label_width,
                label_height,
                Align::Left | Align::Inside,
            );
        }
        if !history
            .points
            .iter()
            .any(|s| s.as_ref().is_some_and(|s| s.channels.get(ch).is_some()))
        {
            draw::set_draw_color(MUTED);
            draw::draw_text2("等待采样…", left, top, width, height, Align::Center);
        }
        if let Some((mx, my)) = draw_hover.get() {
            let (mx, my) = (w.x() + mx, w.y() + my);
            if (top..=top + height).contains(&my) && (left..=left + width).contains(&mx) {
                let index = sample_index(mx, history.points.len(), history.capacity, left, width);
                let reading = index
                    .and_then(|i| history.points[i].as_ref())
                    .and_then(|s| s.channels.get(ch));
                let x = index
                    .map(|i| point_x(i, history.points.len(), history.capacity, left, width))
                    .unwrap_or(mx);
                draw::set_draw_color(MUTED);
                draw::set_line_style(draw::LineStyle::Dash, 1);
                draw::draw_line(x, top, x, top + height);
                draw::set_line_style(draw::LineStyle::Solid, 0);
                let text = match reading {
                    Some(r) => format!(
                        "{} °C\n{} RPM\n{} %",
                        r.temperature,
                        r.rpm.map(|v| v.to_string()).unwrap_or_else(|| "—".into()),
                        r.duty
                    ),
                    None => "— °C\n— RPM\n— %".into(),
                };
                hover_box(w, mx, my, &text);
            }
        }
        draw::pop_clip();
    });
    frame.handle(move |w, event| match event {
        Event::Enter | Event::Move | Event::Drag => {
            hover.set(Some((app::event_x() - w.x(), app::event_y() - w.y())));
            w.redraw();
            true
        }
        Event::Leave | Event::Hide => {
            hover.set(None);
            w.redraw();
            true
        }
        _ => false,
    });
    frame
}
fn value(r: &Reading, metric: usize) -> Option<f64> {
    match metric {
        0 => Some(r.temperature as f64),
        1 => r.rpm.map(|v| v as f64),
        _ => Some(r.duty as f64),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hover_selects_nearest_sample_without_filling_missing_history() {
        assert_eq!(sample_index(910, 5, 10, 10, 900), Some(4));
        assert_eq!(sample_index(510, 5, 10, 10, 900), Some(0));
        assert_eq!(sample_index(559, 5, 10, 10, 900), Some(0));
        assert_eq!(sample_index(561, 5, 10, 10, 900), Some(1));
        assert_eq!(sample_index(410, 5, 10, 10, 900), None);
        assert_eq!(sample_index(910, 0, 10, 10, 900), None);
        assert_eq!(sample_index(911, 5, 10, 10, 900), None);
        assert_eq!(sample_index(9, 5, 10, 10, 900), None);
        for i in 0..20 {
            let x = point_x(i, 20, 20, 12, 387);
            assert_eq!(sample_index(x, 20, 20, 12, 387), Some(i));
        }
    }
    #[test]
    fn failed_samples_occupy_positions_and_metrics_bridge_gaps() {
        let s = Sample {
            at: Instant::now(),
            channels: vec![Reading {
                temperature: 65,
                duty: 40,
                rpm: Some(2000),
            }],
        };
        let mut h = History::new(10);
        h.push(Some(s.clone()));
        h.push(None);
        h.push(None);
        let mut no_rpm = s.clone();
        no_rpm.channels[0].rpm = None;
        h.push(Some(no_rpm));
        h.push(Some(s.clone()));
        assert_eq!(h.points.len(), 5);
        assert_eq!(
            h.metric_points(0, 0).map(|(i, _)| i).collect::<Vec<_>>(),
            vec![0, 3, 4]
        );
        assert_eq!(
            h.metric_points(0, 1).map(|(i, _)| i).collect::<Vec<_>>(),
            vec![0, 4]
        );
        assert_eq!(point_x(0, 1, 10, 10, 900), 910);
        assert_eq!(point_x(0, 5, 10, 10, 900), 510);
        h.set_capacity(20);
        for _ in 0..20 {
            h.push(None);
        }
        assert_eq!(h.points.len(), 20);
        assert_eq!(h.metric_points(0, 0).count(), 0);
        h.set_capacity(10);
        h.push(Some(s));
        assert_eq!(h.points.len(), 10);
        assert_eq!(h.metric_points(0, 0).next().unwrap().0, 9);
    }
}
