use crate::{
    config::{Settings, wildcard},
    control::{ChannelState, cool_complete, ramp},
    hardware::{Hardware, Sample},
};
use std::{
    sync::mpsc::{self, Receiver, Sender},
    thread,
    time::{Duration, Instant},
};
#[derive(Clone, Debug)]
pub struct Snapshot {
    pub sample: Option<Sample>,
    pub targets: Vec<i32>,
    pub levels: Vec<usize>,
    pub active: Option<String>,
    pub mode: String,
    pub error: Option<String>,
    pub force: bool,
    pub description: String,
}

pub enum Command {
    Settings(Settings),
    Select(Option<String>),
    Takeover(bool),
    Force(bool),
    AutoSwitch(bool),
    Suspend,
    Resume,
    Retry,
    Exit,
}
pub enum Event {
    Sample(Option<Sample>),
    Snapshot(Snapshot),
    CoolingDone,
    Stopped(std::result::Result<(), String>),
}
pub struct Worker {
    pub tx: Sender<Command>,
    pub rx: Receiver<Event>,
    join: Option<thread::JoinHandle<()>>,
}
impl Worker {
    pub fn start(settings: Settings, demo: bool) -> Self {
        let (tx, rx) = mpsc::channel();
        let (etx, erx) = mpsc::channel();
        let join = thread::spawn(move || run(settings, open(demo), rx, etx));
        Self {
            tx,
            rx: erx,
            join: Some(join),
        }
    }
    pub fn join(&mut self) {
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}
fn open(demo: bool) -> std::result::Result<Box<dyn Hardware>, String> {
    if demo {
        Ok(Box::new(crate::hardware::Demo::default()))
    } else {
        Ok(Box::new(crate::hardware::Insyde::open(None)?))
    }
}
fn run(
    mut settings: Settings,
    initial: std::result::Result<Box<dyn Hardware>, String>,
    rx: Receiver<Command>,
    tx: Sender<Event>,
) {
    let mut hardware = None;
    let mut error = None;
    let demo = initial
        .as_ref()
        .is_ok_and(|h| h.description().contains("演示模式"));
    match initial {
        Ok(h) => hardware = Some(h),
        Err(e) => error = Some(e),
    }
    let mut active: Option<String> = None;
    let mut cfg = settings.global.clone();
    let mut states = [ChannelState::default(); 2];
    let mut force = false;
    let mut owned = false;
    let mut suspended = false;
    let mut fault = false;
    let mut sample: Option<Sample> = None;
    let mut targets = vec![];
    let mut output = vec![];
    let mut last_written = vec![];
    let mut pending = None;
    let mut next_sample = Instant::now();
    let mut next_scan = Instant::now();
    let mut next_restore = Instant::now();
    let mut last_emit = Instant::now() - Duration::from_secs(1);
    loop {
        let cmd = match pending
            .take()
            .map_or_else(|| rx.recv_timeout(Duration::from_millis(100)), Ok)
        {
            Ok(c) => Some(c),
            Err(mpsc::RecvTimeoutError::Timeout) => None,
            Err(_) => Some(Command::Exit),
        };
        let mut selected = false;
        let mut changed = false;
        if let Some(cmd) = cmd {
            match cmd {
                Command::Exit => {
                    let result = if owned {
                        hardware.as_mut().map_or(Ok(()), |h| h.automatic())
                    } else {
                        Ok(())
                    };
                    let _ = tx.send(Event::Stopped(result));
                    return;
                }
                Command::Settings(s) => {
                    settings = s;
                    if active
                        .as_ref()
                        .is_some_and(|a| !settings.presets.iter().any(|p| &p.name == a))
                    {
                        active = None;
                    }
                    selected = true;
                }
                Command::Select(a) => {
                    active = a;
                    selected = true;
                }
                Command::Takeover(v) => {
                    cfg.take_over = v;
                    fault = false;
                    changed = true;
                }
                Command::Force(v) => {
                    force = v;
                    fault = false;
                    changed = true;
                }
                Command::AutoSwitch(v) => {
                    settings.auto_switch = v;
                    next_scan = Instant::now();
                }
                Command::Suspend => {
                    suspended = true;
                    sample = None;
                    if owned {
                        match hardware.as_mut().unwrap().automatic() {
                            Ok(()) => owned = false,
                            Err(e) => error = Some(e),
                        }
                    }
                }
                Command::Resume => {
                    suspended = false;
                    changed = true;
                    sample = None;
                }
                Command::Retry => {
                    if !owned {
                        match open(demo) {
                            Ok(h) => {
                                hardware = Some(h);
                                error = None;
                                fault = false;
                                changed = true
                            }
                            Err(e) => error = Some(e),
                        }
                    }
                }
            }
        }
        if settings.auto_switch && Instant::now() >= next_scan && !suspended {
            next_scan = Instant::now() + Duration::from_millis(500);
            match crate::platform::processes() {
                Ok(names) => {
                    let a = settings
                        .presets
                        .iter()
                        .find(|p| names.iter().any(|n| wildcard(&p.process_pattern, n)))
                        .map(|p| p.name.clone());
                    if a != active {
                        active = a;
                        selected = true;
                    }
                }
                Err(e) => error = Some(e),
            }
        }
        if selected {
            let desired = active
                .as_ref()
                .and_then(|a| settings.presets.iter().find(|p| &p.name == a))
                .map(|p| p.control.clone())
                .unwrap_or_else(|| settings.global.clone());
            if desired != cfg {
                cfg = desired;
                changed = true;
                fault = false;
            }
        }
        if changed {
            states = [ChannelState::default(); 2];
            next_sample = Instant::now();
            output.clear();
            last_written.clear();
        }
        if !suspended && hardware.is_none() && Instant::now() >= next_sample {
            next_sample = Instant::now() + Duration::from_secs_f64(cfg.update_interval);
            let _ = tx.send(Event::Sample(None));
        }
        if !suspended && let Some(h) = hardware.as_mut() {
            if Instant::now() >= next_sample {
                next_sample = Instant::now() + Duration::from_secs_f64(cfg.update_interval);
                match h.sample() {
                    Ok(s) => {
                        // A sample jump is retried once before it can drive the next control decision.
                        let jump = sample.as_ref().is_some_and(|old| {
                            old.channels
                                .iter()
                                .zip(&s.channels)
                                .any(|(a, b)| (a.temperature - b.temperature).abs() > 30)
                        });
                        let result = if jump {
                            thread::sleep(Duration::from_secs(1));
                            h.sample()
                        } else {
                            Ok(s)
                        };
                        match result {
                            Ok(s) => {
                                if force
                                    && cool_complete(
                                        &s.channels
                                            .iter()
                                            .map(|c| c.temperature)
                                            .collect::<Vec<_>>(),
                                        cfg.force_temp,
                                    )
                                {
                                    force = false;
                                    let _ = tx.send(Event::CoolingDone);
                                }
                                targets = s
                                    .channels
                                    .iter()
                                    .take(2)
                                    .enumerate()
                                    .map(|(i, c)| {
                                        if force {
                                            95
                                        } else {
                                            let idx = i.min(1);
                                            let curve = if idx == 0 {
                                                &cfg.cpu_curve
                                            } else {
                                                &cfg.gpu_curve
                                            };
                                            states[idx].evaluate(curve, c.temperature, &cfg)
                                        }
                                    })
                                    .collect();
                                if s.channels.len() == 3 {
                                    targets.push(targets[1]);
                                }
                                if output.len() != targets.len() {
                                    output = s.channels.iter().map(|c| c.duty).collect();
                                }
                                // Readback, rather than only the last target, detects external CC changes.
                                if s.channels.iter().map(|c| c.duty).ne(output.iter().copied()) {
                                    last_written.clear();
                                }
                                sample = Some(s);
                                if !fault {
                                    error = None;
                                }
                            }
                            Err(e) => {
                                error = Some(e);
                                sample = None;
                                fault = true;
                            }
                        }
                    }
                    Err(e) => {
                        error = Some(e);
                        sample = None;
                        fault = true;
                    }
                }
                let _ = tx.send(Event::Sample(sample.clone()));
            }
            // Process newly queued commands before any actuator operation, including restore.
            if let Ok(c) = rx.try_recv() {
                pending = Some(c);
                continue;
            }
            let stale = sample.as_ref().is_none_or(|s| {
                s.at.elapsed() > Duration::from_secs_f64(cfg.update_interval + 3.0)
            });
            if (fault || stale || (!cfg.take_over && !force))
                && owned
                && Instant::now() >= next_restore
            {
                next_restore = Instant::now() + Duration::from_secs(2);
                match h.automatic() {
                    Ok(()) => {
                        owned = false;
                        last_written.clear()
                    }
                    Err(e) => {
                        error = Some(e);
                        fault = true;
                    }
                }
            }
            if !fault && !stale && (cfg.take_over || force) {
                for (i, t) in targets.iter().enumerate() {
                    output[i] = if cfg.soft_control && !force {
                        ramp(output[i], *t)
                    } else {
                        *t
                    };
                }
                if output != last_written {
                    owned = true;
                    match h.write(&output) {
                        Ok(()) => last_written = output.clone(),
                        Err(e) => {
                            error = Some(e);
                            fault = true;
                            if let Err(e) = h.automatic() {
                                error =
                                    Some(format!("{}；{e}", error.as_deref().unwrap_or_default()))
                            } else {
                                owned = false;
                            }
                        }
                    }
                }
            }
        }
        if last_emit.elapsed() >= Duration::from_millis(100) {
            last_emit = Instant::now();
            let mode = if suspended {
                "休眠暂停"
            } else if hardware.is_none() || fault {
                "不可用"
            } else if force {
                "强制冷却 95%"
            } else if cfg.take_over {
                "软件曲线"
            } else {
                "固件自动"
            };
            let _ = tx.send(Event::Snapshot(Snapshot {
                sample: sample.clone(),
                targets: targets.clone(),
                levels: states.iter().map(|s| s.level + 1).collect(),
                active: active.clone(),
                mode: mode.into(),
                error: error.clone(),
                force,
                description: hardware
                    .as_ref()
                    .map(|h| h.description())
                    .unwrap_or_default(),
            }));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hardware::Reading;
    use std::sync::{Arc, Mutex};
    #[derive(Default)]
    struct State {
        writes: Vec<Vec<i32>>,
        restores: usize,
        temps: Vec<i32>,
        duties: Vec<i32>,
        fail: bool,
    }
    struct Mock(Arc<Mutex<State>>);
    impl Hardware for Mock {
        fn sample(&mut self) -> Result<Sample, String> {
            let s = self.0.lock().unwrap();
            if s.fail {
                return Err("sample unavailable".into());
            }
            Ok(Sample {
                at: Instant::now(),
                channels: s
                    .temps
                    .iter()
                    .zip(&s.duties)
                    .map(|(t, d)| Reading {
                        temperature: *t,
                        duty: *d,
                        rpm: Some(2000),
                    })
                    .collect(),
            })
        }
        fn write(&mut self, d: &[i32]) -> Result<(), String> {
            let mut s = self.0.lock().unwrap();
            s.duties = d.to_vec();
            s.writes.push(d.to_vec());
            Ok(())
        }
        fn automatic(&mut self) -> Result<(), String> {
            self.0.lock().unwrap().restores += 1;
            Ok(())
        }
        fn description(&self) -> String {
            "test".into()
        }
    }
    fn start(
        mut settings: Settings,
        temps: Vec<i32>,
    ) -> (
        Arc<Mutex<State>>,
        Sender<Command>,
        Receiver<Event>,
        thread::JoinHandle<()>,
    ) {
        settings.global.update_interval = 1.0;
        let state = Arc::new(Mutex::new(State {
            duties: vec![30; temps.len()],
            temps,
            ..State::default()
        }));
        let h = Box::new(Mock(state.clone()));
        let (tx, rx) = mpsc::channel();
        let (etx, erx) = mpsc::channel();
        let join = thread::spawn(move || run(settings, Ok(h), rx, etx));
        (state, tx, erx, join)
    }
    fn until(rx: &Receiver<Event>, predicate: impl Fn(&Event) -> bool) {
        let end = Instant::now() + Duration::from_secs(5);
        while Instant::now() < end {
            if let Ok(e) = rx.recv_timeout(Duration::from_millis(200))
                && predicate(&e)
            {
                return;
            }
        }
        panic!("worker condition timed out")
    }
    fn stop(tx: Sender<Command>, rx: Receiver<Event>, join: thread::JoinHandle<()>) {
        tx.send(Command::Exit).unwrap();
        until(&rx, |e| matches!(e, Event::Stopped(Ok(()))));
        join.join().unwrap();
    }
    #[test]
    fn sampling_events_include_failures_but_not_status_refreshes() {
        let (state, tx, rx, j) = start(Settings::default(), vec![50, 60]);
        until(&rx, |e| matches!(e, Event::Sample(Some(_))));
        state.lock().unwrap().fail = true;
        let deadline = Instant::now() + Duration::from_millis(350);
        let mut snapshots = 0;
        while Instant::now() < deadline {
            match rx.recv_timeout(Duration::from_millis(100)) {
                Ok(Event::Sample(_)) => panic!("status refresh advanced history"),
                Ok(Event::Snapshot(_)) => snapshots += 1,
                _ => {}
            }
        }
        assert!(snapshots > 0);
        until(&rx, |e| matches!(e, Event::Sample(None)));
        state.lock().unwrap().fail = false;
        until(&rx, |e| matches!(e, Event::Sample(Some(_))));
        stop(tx, rx, j);
    }
    #[test]
    fn third_fan_follows_gpu_and_exit_restores() {
        let mut s = Settings::default();
        s.global.take_over = true;
        for p in &mut s.global.cpu_curve {
            p.duty = 35;
        }
        let (state, tx, rx, j) = start(s, vec![50, 60, 80]);
        until(
            &rx,
            |e| matches!(e,Event::Snapshot(s) if s.mode=="软件曲线"),
        );
        assert_eq!(state.lock().unwrap().writes[0], vec![35, 40, 40]);
        stop(tx, rx, j);
        assert_eq!(state.lock().unwrap().restores, 1);
    }
    #[test]
    fn sample_failure_returns_automatic_and_latches_fault() {
        let mut s = Settings::default();
        s.global.take_over = true;
        let (state, tx, rx, j) = start(s, vec![50, 60]);
        until(
            &rx,
            |e| matches!(e,Event::Snapshot(s) if s.mode=="软件曲线"),
        );
        state.lock().unwrap().fail = true;
        until(
            &rx,
            |e| matches!(e,Event::Snapshot(s) if s.mode=="不可用"&&s.sample.is_none()),
        );
        assert_eq!(state.lock().unwrap().restores, 1);
        let writes = state.lock().unwrap().writes.len();
        state.lock().unwrap().fail = false;
        until(
            &rx,
            |e| matches!(e,Event::Snapshot(s) if s.sample.is_some()),
        );
        assert_eq!(state.lock().unwrap().writes.len(), writes);
        stop(tx, rx, j);
    }
    #[test]
    fn readback_detects_external_override_after_target_is_stable() {
        let mut s = Settings::default();
        s.global.take_over = true;
        s.global.soft_control = true;
        let (state, tx, rx, j) = start(s, vec![60, 60]);
        until(&rx, |_| state.lock().unwrap().duties == vec![40, 40]);
        let count = state.lock().unwrap().writes.len();
        state.lock().unwrap().duties = vec![55, 55];
        until(&rx, |_| state.lock().unwrap().writes.len() > count);
        assert_eq!(state.lock().unwrap().duties, vec![40, 40]);
        stop(tx, rx, j);
    }
    #[test]
    fn forced_cooling_overrides_takeover_and_completes_once() {
        let (state, tx, rx, j) = start(Settings::default(), vec![60, 60]);
        tx.send(Command::Force(true)).unwrap();
        until(&rx, |_| state.lock().unwrap().duties == vec![95, 95]);
        state.lock().unwrap().temps = vec![49, 49];
        until(&rx, |e| matches!(e, Event::CoolingDone));
        until(
            &rx,
            |e| matches!(e,Event::Snapshot(s) if s.mode=="固件自动"),
        );
        assert_eq!(state.lock().unwrap().restores, 1);
        stop(tx, rx, j);
    }
    #[test]
    fn cancel_queued_during_sampling_prevents_stale_write() {
        struct Blocking {
            inner: Mock,
            ready: Option<Sender<()>>,
            release: Receiver<()>,
        }
        impl Hardware for Blocking {
            fn sample(&mut self) -> Result<Sample, String> {
                if let Some(t) = self.ready.take() {
                    t.send(()).unwrap();
                    self.release.recv_timeout(Duration::from_secs(2)).unwrap();
                }
                self.inner.sample()
            }
            fn write(&mut self, d: &[i32]) -> Result<(), String> {
                self.inner.write(d)
            }
            fn automatic(&mut self) -> Result<(), String> {
                self.inner.automatic()
            }
            fn description(&self) -> String {
                "blocking test".into()
            }
        }
        let state = Arc::new(Mutex::new(State {
            temps: vec![60, 60],
            duties: vec![30, 30],
            ..State::default()
        }));
        let (ready_tx, ready_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let (tx, rx) = mpsc::channel();
        let (etx, erx) = mpsc::channel();
        let h = Blocking {
            inner: Mock(state.clone()),
            ready: Some(ready_tx),
            release: release_rx,
        };
        let mut settings = Settings::default();
        settings.global.take_over = true;
        let j = thread::spawn(move || run(settings, Ok(Box::new(h)), rx, etx));
        ready_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        tx.send(Command::Takeover(false)).unwrap();
        release_tx.send(()).unwrap();
        until(
            &erx,
            |e| matches!(e,Event::Snapshot(s) if s.mode=="固件自动"),
        );
        assert!(state.lock().unwrap().writes.is_empty());
        stop(tx, erx, j);
    }
}
