use crate::config::{ControlConfig, Point};
#[derive(Clone, Copy, Debug, Default)]
pub struct ChannelState {
    pub level: usize,
    pub effective: Option<i32>,
}
pub fn linear(curve: &[Point], t: i32) -> i32 {
    if t <= curve[0].temperature {
        return curve[0].duty;
    }
    for p in curve.windows(2) {
        if t <= p[1].temperature {
            return (p[0].duty as f64
                + (p[1].duty - p[0].duty) as f64 * (t - p[0].temperature) as f64
                    / (p[1].temperature - p[0].temperature) as f64)
                .round() as i32;
        }
    }
    curve.last().unwrap().duty
}
impl ChannelState {
    pub fn evaluate(&mut self, curve: &[Point], t: i32, c: &ControlConfig) -> i32 {
        if c.linear {
            let e = self
                .effective
                .unwrap_or(t)
                .max(t)
                .min(t + c.transition_temp);
            self.effective = Some(e);
            self.level = curve.iter().rposition(|p| p.temperature <= e).unwrap_or(0);
            linear(curve, e)
        } else {
            self.level = self.level.min(curve.len() - 1);
            while self.level + 1 < curve.len() && t >= curve[self.level + 1].temperature {
                self.level += 1;
            }
            while self.level > 0 && t < curve[self.level].temperature - c.transition_temp {
                self.level -= 1;
            }
            curve[self.level].duty
        }
    }
}
pub fn cool_complete(temps: &[i32], threshold: i32) -> bool {
    !temps.is_empty() && temps.iter().all(|t| *t < threshold)
}
pub fn ramp(current: i32, target: i32) -> i32 {
    current + (target - current).signum()
}
pub fn to_raw(percent: i32) -> u8 {
    ((percent.clamp(0, 100) * 255 + 50) / 100) as u8
}
pub fn to_percent(raw: u8) -> i32 {
    (raw as i32 * 100 + 127) / 255
}
