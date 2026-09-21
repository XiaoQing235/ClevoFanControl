fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let mut r = winresource::WindowsResource::new();
        let manifest = std::fs::read_to_string("app.manifest").expect("manifest");
        // Debug builds can exercise the UI/tests without an elevation prompt.
        let manifest = if std::env::var("PROFILE").as_deref() == Ok("debug") {
            manifest.replace("requireAdministrator", "asInvoker")
        } else {
            manifest
        };
        r.set_manifest(&manifest);
        r.set_icon("assets/ClevoFanControl.ico");
        r.compile().expect("compile Windows resources");
    }
}
