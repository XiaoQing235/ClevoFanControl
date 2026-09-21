use clevo_fan_control::{config::*, control::*, hardware::decode_rpm};
fn curve() -> Vec<Point> {
    vec![
        Point {
            temperature: 40,
            duty: 20,
        },
        Point {
            temperature: 60,
            duty: 50,
        },
        Point {
            temperature: 80,
            duty: 90,
        },
    ]
}
#[test]
fn initial_test_curve_is_flat_40() {
    let s = Settings::default();
    for c in [&s.global.cpu_curve, &s.global.gpu_curve] {
        assert!(c.iter().all(|p| p.duty == 40));
        for t in 0..130 {
            assert_eq!(linear(c, t), 40);
        }
    }
}
#[test]
fn step_hysteresis_is_strict_and_crosses_multiple_nodes() {
    let c = ControlConfig::default();
    let mut s = ChannelState::default();
    let p = curve();
    assert_eq!(s.evaluate(&p, 85, &c), 90);
    assert_eq!(s.evaluate(&p, 77, &c), 90);
    assert_eq!(s.evaluate(&p, 76, &c), 50);
    assert_eq!(s.evaluate(&p, 30, &c), 20);
}
#[test]
fn linear_tracks_rising_temperature_and_holds_falling_temperature() {
    let c = ControlConfig {
        linear: true,
        ..ControlConfig::default()
    };
    let mut s = ChannelState::default();
    let p = curve();
    assert_eq!(s.evaluate(&p, 60, &c), 50);
    assert_eq!(s.evaluate(&p, 59, &c), 50);
    assert_eq!(s.evaluate(&p, 55, &c), 47);
    assert_eq!(s.evaluate(&p, 70, &c), 70);
}
#[test]
fn decreasing_duty_and_endpoint_clamp_are_supported() {
    let p = vec![
        Point {
            temperature: 40,
            duty: 80,
        },
        Point {
            temperature: 80,
            duty: 20,
        },
    ];
    validate_curve(&p).unwrap();
    assert_eq!(linear(&p, 60), 50);
    assert_eq!(linear(&p, 10), 80);
    assert_eq!(linear(&p, 110), 20);
}
#[test]
fn force_requires_all_valid_channels_strictly_below_threshold() {
    assert!(!cool_complete(&[], 50));
    assert!(!cool_complete(&[49, 50], 50));
    assert!(cool_complete(&[49], 50));
    assert!(cool_complete(&[48, 49], 50));
    assert_eq!(ramp(40, 45), 41);
    assert_eq!(ramp(40, 35), 39);
    assert_eq!(ramp(40, 40), 40);
}
#[test]
fn unicode_glob_and_case_sensitive_preset_names() {
    assert!(wildcard("?.exe", "İ.exe"));
    assert!(wildcard("游戏?.EXE", "游戏猫.exe"));
    assert!(!wildcard("游戏?.exe", "游戏猫狗.exe"));
    assert!(wildcard("*render*", "BlenderRender.exe"));
    assert!(!wildcard("game.exe", "othergame.exe"));
    let mut s = Settings::default();
    s.presets = vec![
        Preset {
            name: "A".into(),
            process_pattern: "*".into(),
            control: s.global.clone(),
        },
        Preset {
            name: "a".into(),
            process_pattern: "?".into(),
            control: s.global.clone(),
        },
    ];
    s.validate().unwrap();
    s.presets[1].name = "A".into();
    assert!(s.validate().is_err());
}
#[test]
fn strict_json_rejects_extra_duplicate_missing_and_invalid_fields() {
    let s = Settings::default();
    let json = serde_json::to_string(&s).unwrap();
    assert_eq!(serde_json::from_str::<Settings>(&json).unwrap(), s);
    assert!(serde_json::from_str::<Settings>(&json.replacen("{", "{\"extra\":1,", 1)).is_err());
    assert!(
        serde_json::from_str::<Settings>(&json.replacen("{", "{\"schemaVersion\":1,", 1)).is_err()
    );
    assert!(serde_json::from_str::<Settings>("{}").is_err());
    let mut s = s;
    s.global.update_interval = 0.0;
    assert!(s.validate().is_err());
}
#[test]
fn raw_duty_and_official_rpm_conversion() {
    for p in 0..=100 {
        assert_eq!(to_percent(to_raw(p)), p);
    }
    assert_eq!(to_raw(40), 102);
    assert_eq!(decode_rpm(357), Some(6040));
    assert_eq!(decode_rpm(0), Some(0));
    assert_eq!(decode_rpm(1), None);
}

#[test]
fn appearance_settings_roundtrip_and_old_config_defaults() {
    let mut s = Settings::default();
    assert_eq!(s.theme, Theme::System);
    assert_eq!(s.font_family, None);
    for theme in [Theme::System, Theme::Light, Theme::Dark] {
        s.theme = theme;
        s.font_family = Some("Microsoft YaHei UI".into());
        s.validate().unwrap();
        let bytes = serde_json::to_vec(&s).unwrap();
        assert_eq!(serde_json::from_slice::<Settings>(&bytes).unwrap(), s);
    }
    let mut old = serde_json::to_value(&s).unwrap();
    old.as_object_mut().unwrap().remove("theme");
    old.as_object_mut().unwrap().remove("fontFamily");
    let migrated: Settings = serde_json::from_value(old.clone()).unwrap();
    assert_eq!(migrated.theme, Theme::System);
    assert_eq!(migrated.font_family, None);
    old["theme"] = serde_json::json!("unknown");
    assert!(serde_json::from_value::<Settings>(old).is_err());
    s.font_family = Some("\0invalid".into());
    assert!(s.validate().is_err());
}
#[test]
fn atomic_save_preserves_other_files_and_reports_replace_failure() {
    let dir = std::env::temp_dir().join(format!("clevo-config-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join("配置.json");
    let s = Settings::default();
    s.save(&p).unwrap();
    assert_eq!(Settings::load(&p).unwrap(), s);
    let blocked = dir.join("directory.json");
    std::fs::create_dir_all(&blocked).unwrap();
    assert!(s.save(&blocked).is_err());
    assert_eq!(Settings::load(&p).unwrap(), s);
    std::fs::remove_dir_all(dir).unwrap();
}
#[test]
fn legacy_import_disarms_control_without_touching_original() {
    let dir = std::env::temp_dir().join(format!("clevo-import-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join("ClevoFanControl.json");
    let c = ControlConfig::default();
    let mut v = serde_json::to_value(c).unwrap();
    let o = v.as_object_mut().unwrap();
    o.insert("updateInterval".into(), 2.into());
    o.insert("forceCooling".into(), true.into());
    o.insert("takeOver".into(), true.into());
    o.insert("autoRun".into(), true.into());
    o.insert("closeToTray".into(), true.into());
    o.insert("fontSize".into(), 10.into());
    let original = serde_json::to_vec(&v).unwrap();
    std::fs::write(&p, &original).unwrap();
    let s = import_legacy(&p).unwrap();
    assert!(!s.global.take_over && !s.auto_run && !s.auto_switch);
    assert!(s.close_to_tray);
    assert_eq!(std::fs::read(&p).unwrap(), original);
    std::fs::remove_dir_all(dir).unwrap();
}

#[test]
fn sampling_interval_and_history_count_accept_old_config_and_new_ranges() {
    let mut s = Settings::default();
    let mut old = serde_json::to_value(&s).unwrap();
    old.as_object_mut().unwrap().remove("historyPoints");
    old["global"]["updateInterval"] = 2.into();
    let loaded: Settings = serde_json::from_value(old).unwrap();
    assert_eq!(loaded.history_points, 60);
    assert_eq!(loaded.global.update_interval, 2.0);
    for interval in [0.2, 0.5, 2.0, 10.0] {
        s.global.update_interval = interval;
        s.validate().unwrap();
        assert_eq!(
            serde_json::from_slice::<Settings>(&serde_json::to_vec(&s).unwrap()).unwrap(),
            s
        );
    }
    for interval in [0.0, 0.1, 10.1, f64::NAN, f64::INFINITY] {
        s.global.update_interval = interval;
        assert!(s.validate().is_err());
    }
    s.global.update_interval = 0.2;
    for points in [10, 60, 300] {
        s.history_points = points;
        s.validate().unwrap();
    }
    for points in [0, 9, 301] {
        s.history_points = points;
        assert!(s.validate().is_err());
    }
}
