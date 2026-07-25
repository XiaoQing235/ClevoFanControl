# Runtime Logic Corrections Design

## Goal

Correct the reviewed runtime defects in single-instance enforcement, preset fallback, update-timer reconfiguration, one-shot force cooling, and Task Scheduler XML generation. Wire the existing logic tests into the Visual Studio solution and add focused regression coverage without changing the EC protocol or replacing the current MFC architecture.

## Confirmed Decisions

- Single-instance enforcement is machine-wide because the embedded controller is a machine-wide hardware resource.
- Automatic preset switching may immediately discard unsaved draft values. The application does not prompt, pause, or preserve those values.
- When no process matches an automatic preset, the global configuration is restored.
- The no-match behavior above supersedes the earlier decision in `2026-07-25-multi-preset-design.md` that retained the last automatic preset.
- Disabling automatic switching only stops future scans; it does not independently restore the global configuration.
- Force cooling is a one-shot action. When both monitored temperatures fall below the configured threshold, the runtime state, checkbox, and current draft all become disabled.
- The existing MFC dialog, multimedia timer, `schtasks` integration, and EC DLL boundary remain in place.
- The existing standalone tests become a first-class solution project rather than remaining a manually compiled source file.

## Scope

### In scope

- Replace window-title-based duplicate detection with a machine-wide named mutex.
- Reconfigure the multimedia update timer after `UpdateInterval` changes at runtime.
- Restore the global configuration when automatic process matching returns no preset.
- Preserve the documented behavior that automatic switching discards unsaved drafts.
- Reliably synchronize completion of one-shot force cooling from the core thread to the UI draft.
- Generate valid UTF-8 Task Scheduler XML and escape the executable path.
- Add and register a Win32 console test project for the existing and new focused tests.

### Out of scope

- Replacing `schtasks` with the Task Scheduler COM API.
- Preserving or prompting for drafts during automatic preset switching.
- Changing behavior when automatic switching is manually disabled.
- Redesigning the EC DLL interface, fan-count assumptions, curves, or soft-control algorithm.
- Editing README content that is being changed independently.

## Architecture

### Machine-wide single instance

Add a small Win32 `SingleInstanceGuard` that owns a mutex handle for the process lifetime. `CClevoFanControlApp::InitInstance` acquires `Global\ClevoFanControl.EcController` before creating the dialog.

The acquisition result distinguishes:

- `Acquired`: continue startup and retain the handle until application destruction.
- `AlreadyRunning`: show an informational message and return without creating a second controller.
- `Unavailable`: show the Windows error and fail closed rather than running without protection.

The guard closes only its own handle and never releases or deletes an object owned by another process. A focused test uses a process-specific mutex name to verify that the first guard acquires it and a simultaneous second guard reports `AlreadyRunning`.

### Runtime timer reconfiguration

Keep timer ownership on the core worker thread. Factor timer creation and stopping into focused `CCore` methods so callbacks are never reconfigured from the UI thread.

`CCore::ApplyConfig` continues to copy a validated configuration and set `m_bForcedRefresh`. After the worker handles that immediate refresh, it compares the configured interval with the active multimedia-timer interval. A changed interval causes the worker to stop the old timer and create a new one with the same `TIMECAPS` bounds. Timer creation uses `TIME_KILL_SYNCHRONOUS` so no callback can outlive timer shutdown.

If initial timer creation or runtime recreation fails, the worker stops the multimedia timer and enters the existing `RunOriginal` loop. That loop already reads `UpdateInterval` after each update, so control remains active with dynamic timing rather than terminating or retaining a stale interval.

### Automatic preset fallback

`ScanPresetProcesses` resolves every successful process scan to one desired active index:

- the first matching preset index; or
- `-1` when no preset matches.

If the desired index equals the current active index, no action is taken. A different non-negative index calls the existing automatic preset application path. A desired index of `-1` calls the global-configuration application path without a modal error. Both paths intentionally replace the current draft and clear its dirty state, matching the confirmed discard behavior.

Process enumeration failure is different from a successful scan with no match: failures retain the current configuration and remain rate-limited diagnostics.

### One-shot force-cooling completion

Add a pure `ShouldCompleteForcedCooling` predicate for the enabled flag, two current temperatures, and threshold. `CCore::Work` uses this predicate instead of duplicating the condition, so the threshold semantics can be tested without loading the hardware DLL.

Add an interlocked completion sequence to `CCore`, initialized to zero. When `Work` automatically disables force cooling because `ShouldCompleteForcedCooling` returns true, it increments this sequence exactly once while clearing the runtime flag.

Expose the current sequence through `CCoreStatusSnapshot`. The dialog tracks the last observed value. During UI timer processing it always advances its observed sequence, but clears the checkbox and `m_draft.ForceCooling` only when:

- the completion sequence changed; and
- the current core snapshot is no longer force cooling.

This prevents a prior completion event from clearing a newly applied force-cooling preset. A completed action marks the draft dirty so the next normal save persists `false`. Direct user unchecking does not increment the completion sequence.

### Task Scheduler XML

Add a non-MFC `TaskXml` helper with a narrow responsibility:

```cpp
bool BuildTaskXmlUtf8(
    const std::wstring& targetPath,
    std::string* output,
    std::string* diagnostic);
```

The helper validates output arguments, XML-escapes `&`, `<`, `>`, `"`, and `'`, converts the resulting wide string to UTF-8, and emits an `encoding="UTF-8"` declaration. UTF-8 does not require a byte-order mark.

`CreateTaskXml` converts the existing ANSI executable path to a wide string using the active Windows code page, calls the helper, and writes the exact UTF-8 bytes through Win32 file APIs. It verifies create, complete write, flush, and close operations. Any failure returns `FALSE`, allowing the existing autorun save transaction to roll back.

The generated XML retains the existing trigger, principal, settings, and task name. Only serialization and file writing change.

## Error Handling

- Mutex creation failures stop startup and include the Windows error code in the message.
- An existing mutex is not treated as an exceptional crash and does not create a dialog.
- A failed process snapshot never restores global configuration because it is not evidence that no process matches.
- A failed automatic preset or global apply retains the previously effective core configuration and active index, with rate-limited diagnostics and no timer-driven modal dialog.
- Timer recreation failure falls back to `RunOriginal` after the multimedia timer is fully stopped.
- Force-cooling UI synchronization only reacts to a completion sequence transition, not a transient unready core snapshot.
- Task XML conversion and file operations return actionable diagnostics; temporary XML cleanup remains best effort after `schtasks` completes.

## Test Integration

Add `ClevoFanControlTests.vcxproj` as a Win32 console application with Debug and Release configurations and register it in `ClevoFanControl.sln`. It compiles the existing `FanCurveModelTests.cpp` harness, the model/store/matcher sources it already exercises, and the new platform helpers that do not depend on MFC.

Extend the harness with focused tests for:

- first and duplicate acquisition of a uniquely named global `SingleInstanceGuard`;
- UTF-8 Task XML declaration and absence of a contradictory UTF-16 declaration;
- XML escaping of an executable path containing `&`, quotes, and non-ASCII text;
- successful automatic matching and the existing `-1` no-match result used for global fallback;
- the force-cooling completion predicate at temperatures above, equal to, and below the threshold.

The timer-owner and MFC-dialog transitions remain integration-tested through compilation and targeted code-path review because they depend on a running message loop, multimedia callbacks, and the hardware worker.

## Acceptance Criteria

- Starting a second instance on the machine does not create a second main dialog or core worker.
- Changing `UpdateInterval` through save or preset application changes the active worker interval without restarting the application.
- Exiting the last matching process restores the global configuration on the next successful scan.
- Automatic switching continues to discard unsaved drafts without prompting.
- Completing force cooling unchecks the UI, changes the draft to `ForceCooling == false`, and leaves it dirty until saved.
- Autorun task XML parses as UTF-8 and remains valid when the executable path contains XML-sensitive characters.
- `ClevoFanControl.sln` builds Debug and Release for Win32, including the test project.
- The test executable reports `FanCurveModelTests: PASS`.
- `git diff --check` reports no whitespace errors in the implementation changes.

## Non-goals

- No new service, driver, background process, or external dependency.
- No change to persisted JSON schemas.
- No migration of existing preset or global configuration files.
- No unrelated UI layout or documentation rewrite.
