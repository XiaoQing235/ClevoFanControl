#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod ui;
use clevo_fan_control::{
    config::Settings,
    hardware::{Hardware, Insyde},
    platform,
    worker::Worker,
};
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let demo = args.iter().any(|a| a == "--demo" || a == "--smoke");
    let instance = match platform::SingleInstance::acquire() {
        Ok(v) => v,
        Err(e) => {
            ui::startup_error(&e);
            return;
        }
    };
    if args.iter().any(|a| a == "--probe") {
        let result = Insyde::open(None).and_then(|mut h| {
            println!("{}", h.description());
            println!("{:?}", h.sample()?);
            Ok(())
        });
        if let Err(e) = result {
            eprintln!("{e}");
            std::process::exit(1)
        }
        return;
    }
    if args.iter().any(|a| a == "--hardware-check") {
        let result = Insyde::open(None).and_then(|mut h| {
            let before = h.sample()?;
            println!("Before: {before:?}");
            let test = (|| {
                h.write(&vec![40; before.channels.len()])?;
                std::thread::sleep(std::time::Duration::from_secs(2));
                let after = h.sample()?;
                println!("40%: {after:?}");
                if after.channels.iter().any(|c| (c.duty - 40).abs() > 2) {
                    return Err("调速读回不匹配".to_string());
                }
                Ok(())
            })();
            let restore = h.automatic();
            println!("Restore automatic: {restore:?}");
            restore?;
            test?;
            std::thread::sleep(std::time::Duration::from_secs(2));
            println!("After: {:?}", h.sample()?);
            Ok(())
        });
        if let Err(e) = result {
            eprintln!("{e}");
            std::process::exit(1)
        }
        return;
    }
    let path = std::env::current_exe()
        .expect("executable path")
        .with_file_name("ClevoFanControl.x64.json");
    let (settings, error) = match Settings::load(&path) {
        Ok(s) => (s, None),
        Err(e) => (Settings::default(), Some(e)),
    };
    let worker = Worker::start(settings.clone(), demo);
    ui::run(
        settings,
        path,
        worker,
        error,
        demo,
        args.iter().any(|a| a == "--smoke"),
    );
    drop(instance);
}
