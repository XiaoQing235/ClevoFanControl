# Runtime Logic Corrections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix machine-wide duplicate execution, stale automatic presets, stale update intervals, one-shot force-cooling state divergence, and invalid Task Scheduler XML while making the existing logic tests part of normal solution builds.

**Architecture:** Keep the existing MFC dialog, EC DLL boundary, multimedia timer, and `schtasks` workflow. Add three focused non-MFC helpers (`SingleInstance`, `TaskXml`, and `FanControlLogic`), keep timer ownership inside `CCore`, and use a completion sequence to transfer one-shot force-cooling completion to the UI without losing events.

**Tech Stack:** C++17, Win32, MFC, Visual Studio v145, MSBuild, existing console test harness.

---

## File Map

- Create `ClevoFanControl/SingleInstance.h` and `ClevoFanControl/SingleInstance.cpp`: own and classify a named Win32 mutex.
- Create `ClevoFanControl/TaskXml.h` and `ClevoFanControl/TaskXml.cpp`: XML escaping and UTF-8 Task Scheduler document serialization.
- Create `ClevoFanControl/FanControlLogic.h` and `ClevoFanControl/FanControlLogic.cpp`: pure force-cooling and interval-change decisions shared by `CCore` and tests.
- Create `ClevoFanControl/ClevoFanControlTests.vcxproj`: build the existing console harness and non-MFC helpers in Debug and Release.
- Modify `ClevoFanControl.sln`: register the test project for both Win32 configurations.
- Modify `ClevoFanControl/ClevoFanControl.vcxproj`: compile and display the new application helpers.
- Modify `ClevoFanControl/ClevoFanControl.h` and `ClevoFanControl/ClevoFanControl.cpp`: hold the machine-wide single-instance guard and remove title matching.
- Modify `ClevoFanControl/Core.h` and `ClevoFanControl/Core.cpp`: track force-cooling completion and own dynamic multimedia-timer recreation.
- Modify `ClevoFanControl/ClevoFanControlDlg.h` and `ClevoFanControl/ClevoFanControlDlg.cpp`: restore global configuration after no match, synchronize completed force cooling, and write valid task XML.
- Modify `ClevoFanControl/PresetMatcher.h` and `ClevoFanControl/PresetMatcher.cpp`: expose one desired automatic preset index, including `-1` for global.
- Modify `ClevoFanControl/FanCurveModelTests.cpp`: add focused regression tests to the existing harness.

### Task 1: Wire the Existing Test Harness into the Solution

**Files:**
- Create: `ClevoFanControl/ClevoFanControlTests.vcxproj`
- Modify: `ClevoFanControl.sln`

- [ ] **Step 1: Record the current manual-test baseline**

Run the already built harness before project changes:

```powershell
& '.\Release\FanCurveModelTests.exe'
```

Expected: `FanCurveModelTests: PASS`.

- [ ] **Step 2: Create the Win32 console test project**

Create `ClevoFanControl/ClevoFanControlTests.vcxproj` with project GUID `{2BCACB2D-5D01-47D9-9E01-6CBB3F59270D}`, Debug/Release Win32 configurations, `PlatformToolset` `v145`, no precompiled header, console subsystem, and these initial compile items:

```xml
<ItemGroup>
  <ClCompile Include="FanCurveModelTests.cpp" />
  <ClCompile Include="FanCurveModel.cpp" />
  <ClCompile Include="FanConfig.cpp" />
  <ClCompile Include="ConfigStore.cpp" />
  <ClCompile Include="JsonValue.cpp" />
  <ClCompile Include="PresetMatcher.cpp" />
  <ClCompile Include="PresetStore.cpp" />
</ItemGroup>
```

Set the output properties for both configurations so the executable is predictable:

```xml
<OutDir>$(SolutionDir)$(Configuration)\</OutDir>
<IntDir>$(Configuration)\$(ProjectName)\</IntDir>
<TargetName>ClevoFanControlTests</TargetName>
```

- [ ] **Step 3: Register the project in the solution**

Add this project entry after the application project:

```text
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "ClevoFanControlTests", "ClevoFanControl\ClevoFanControlTests.vcxproj", "{2BCACB2D-5D01-47D9-9E01-6CBB3F59270D}"
EndProject
```

Add `ActiveCfg` and `Build.0` mappings for Debug and Release Win32 under `ProjectConfigurationPlatforms`.

- [ ] **Step 4: Build and run the solution-owned baseline**

Run:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe' 'ClevoFanControl.sln' /m /t:Build /p:Configuration=Release /p:Platform=Win32 /v:minimal
& '.\Release\ClevoFanControlTests.exe'
```

Expected: both `ClevoFanControl.exe` and `ClevoFanControlTests.exe` are produced; the test process prints `FanCurveModelTests: PASS`.

- [ ] **Step 5: Commit the test-project baseline**

```powershell
git add -- ClevoFanControl.sln ClevoFanControl/ClevoFanControlTests.vcxproj
git commit -m "test: wire logic tests into solution"
```

### Task 2: Enforce a Machine-Wide Single Instance

**Files:**
- Create: `ClevoFanControl/SingleInstance.h`
- Create: `ClevoFanControl/SingleInstance.cpp`
- Modify: `ClevoFanControl/FanCurveModelTests.cpp`
- Modify: `ClevoFanControl/ClevoFanControlTests.vcxproj`
- Modify: `ClevoFanControl/ClevoFanControl.vcxproj`
- Modify: `ClevoFanControl/ClevoFanControl.h`
- Modify: `ClevoFanControl/ClevoFanControl.cpp`

- [ ] **Step 1: Write the failing guard test**

Add includes and a test that uses a process-specific global name:

```cpp
#include "SingleInstance.h"

void TestSingleInstanceGuard()
{
    const std::wstring name = L"Global\\ClevoFanControl.Tests." +
        std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
    SingleInstanceGuard first;
    SingleInstanceGuard second;
    Expect(first.Acquire(name.c_str()) == SingleInstanceStatus::Acquired,
        "the first single-instance guard should acquire its mutex");
    Expect(second.Acquire(name.c_str()) == SingleInstanceStatus::AlreadyRunning,
        "a second guard should detect the existing mutex");
    Expect(second.ErrorCode() == ERROR_ALREADY_EXISTS,
        "duplicate acquisition should preserve ERROR_ALREADY_EXISTS");
}
```

Call `TestSingleInstanceGuard()` from `main()`.

- [ ] **Step 2: Build to verify the test fails**

Run the Release solution build. Expected: compilation fails because `SingleInstance.h` does not exist.

- [ ] **Step 3: Implement the guard**

Define:

```cpp
enum class SingleInstanceStatus
{
    Acquired,
    AlreadyRunning,
    Unavailable
};

class SingleInstanceGuard
{
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();
    SingleInstanceStatus Acquire(const wchar_t* name);
    DWORD ErrorCode() const;
private:
    HANDLE handle_;
    DWORD errorCode_;
};
```

Delete copy construction and assignment. `Acquire` calls `CreateMutexW(nullptr, FALSE, name)`, keeps the handle only for a first acquisition, closes the duplicate handle when `GetLastError() == ERROR_ALREADY_EXISTS`, and returns `Unavailable` for null/empty names or other Win32 failures. The destructor closes `handle_` when present.

- [ ] **Step 4: Integrate the guard into application startup**

Add `SingleInstanceGuard m_singleInstance;` to `CClevoFanControlApp`. At the start of `InitInstance`, acquire `L"Global\\ClevoFanControl.EcController"` before constructing the dialog. Replace the old `FindWindow` block with:

```cpp
const SingleInstanceStatus singleInstance =
    m_singleInstance.Acquire(L"Global\\ClevoFanControl.EcController");
if (singleInstance == SingleInstanceStatus::AlreadyRunning)
{
    AfxMessageBox(_T("ClevoFanControl is already running."), MB_ICONINFORMATION);
    return FALSE;
}
if (singleInstance != SingleInstanceStatus::Acquired)
{
    CString message;
    message.Format(_T("Unable to establish single-instance protection. (Windows error %lu)"),
        static_cast<unsigned long>(m_singleInstance.ErrorCode()));
    AfxMessageBox(message, MB_ICONERROR);
    return FALSE;
}
```

Register `SingleInstance.h/.cpp` in the application project and `SingleInstance.cpp` in the test project.

- [ ] **Step 5: Run the focused tests**

Build Release and run `Release/ClevoFanControlTests.exe`. Expected: `FanCurveModelTests: PASS`, including the first/duplicate mutex assertions.

- [ ] **Step 6: Commit**

```powershell
git add -- ClevoFanControl/SingleInstance.h ClevoFanControl/SingleInstance.cpp ClevoFanControl/FanCurveModelTests.cpp ClevoFanControl/ClevoFanControlTests.vcxproj ClevoFanControl/ClevoFanControl.vcxproj ClevoFanControl/ClevoFanControl.h ClevoFanControl/ClevoFanControl.cpp
git commit -m "fix: enforce machine-wide single instance"
```

### Task 3: Generate Valid UTF-8 Task XML

**Files:**
- Create: `ClevoFanControl/TaskXml.h`
- Create: `ClevoFanControl/TaskXml.cpp`
- Modify: `ClevoFanControl/FanCurveModelTests.cpp`
- Modify: `ClevoFanControl/ClevoFanControlTests.vcxproj`
- Modify: `ClevoFanControl/ClevoFanControl.vcxproj`
- Modify: `ClevoFanControl/ClevoFanControlDlg.cpp`

- [ ] **Step 1: Write failing XML serialization tests**

Add `#include "TaskXml.h"` and:

```cpp
void TestTaskXmlSerialization()
{
    std::string xml;
    std::string diagnostic;
    const std::wstring target = L"C:\\R&D\\\u6e05\"Fan'Control.exe";
    Expect(BuildTaskXmlUtf8(target, &xml, &diagnostic),
        "task XML should serialize a Unicode target path");
    Expect(diagnostic.empty(), "successful task XML serialization should clear diagnostics");
    Expect(xml.find("encoding=\"UTF-8\"") != std::string::npos,
        "task XML should declare UTF-8");
    Expect(xml.find("UTF-16") == std::string::npos,
        "task XML should not declare UTF-16");
    Expect(xml.find("R&amp;D") != std::string::npos,
        "task XML should escape ampersands");
    Expect(xml.find("&quot;Fan&apos;Control.exe") != std::string::npos,
        "task XML should escape quotes and apostrophes");
    Expect(xml.find("\xe6\xb8\x85") != std::string::npos,
        "task XML should encode non-ASCII text as UTF-8");
    Expect(!BuildTaskXmlUtf8(target, nullptr, &diagnostic),
        "task XML should reject a null output");
}
```

Call it from `main()`.

- [ ] **Step 2: Build to verify the test fails**

Expected: missing `TaskXml.h` or unresolved `BuildTaskXmlUtf8`.

- [ ] **Step 3: Implement the serializer**

Expose the signature from the approved design. In `TaskXml.cpp`, build a wide XML document with the existing trigger/principal/settings/action structure, escape all five XML-sensitive characters in the command text, and convert the complete document with `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)`. Clear `diagnostic` on success and report null output, excessive input, or conversion errors on failure.

- [ ] **Step 4: Replace narrow text-file generation**

In `CreateTaskXml`:

1. Convert `strTargetPath` from `CP_ACP` to `std::wstring` with `MultiByteToWideChar`.
2. Call `BuildTaskXmlUtf8`.
3. Create `strXmlPath` with `CreateFileA(..., CREATE_ALWAYS, ...)`.
4. Write all bytes, handling sizes larger than one `DWORD` chunk.
5. Call `FlushFileBuffers` and `CloseHandle`.
6. Delete the partial file and return `FALSE` after any create/write/flush/close error.

Remove the UTF-16 literal, `sprintf_s`, `fopen_s`, `fputs`, and `fclose` implementation. Register the helper in both projects.

- [ ] **Step 5: Run tests and parser verification**

Build Release and run the test executable. Then use a PowerShell `XmlDocument` stream parse over a representative UTF-8 byte string generated by the test helper or a temporary task file. Expected: tests print `PASS`, and the XML parser accepts the document without an encoding error.

- [ ] **Step 6: Commit**

```powershell
git add -- ClevoFanControl/TaskXml.h ClevoFanControl/TaskXml.cpp ClevoFanControl/FanCurveModelTests.cpp ClevoFanControl/ClevoFanControlTests.vcxproj ClevoFanControl/ClevoFanControl.vcxproj ClevoFanControl/ClevoFanControlDlg.cpp
git commit -m "fix: generate valid task scheduler XML"
```

### Task 4: Synchronize One-Shot Force Cooling

**Files:**
- Create: `ClevoFanControl/FanControlLogic.h`
- Create: `ClevoFanControl/FanControlLogic.cpp`
- Modify: `ClevoFanControl/FanCurveModelTests.cpp`
- Modify: `ClevoFanControl/ClevoFanControlTests.vcxproj`
- Modify: `ClevoFanControl/ClevoFanControl.vcxproj`
- Modify: `ClevoFanControl/Core.h`
- Modify: `ClevoFanControl/Core.cpp`
- Modify: `ClevoFanControl/ClevoFanControlDlg.h`
- Modify: `ClevoFanControl/ClevoFanControlDlg.cpp`

- [ ] **Step 1: Write failing force-cooling predicate tests**

Add `#include "FanControlLogic.h"` and:

```cpp
void TestForceCoolingCompletion()
{
    Expect(!ShouldCompleteForcedCooling(false, 40, 40, 50),
        "disabled force cooling should never complete");
    Expect(!ShouldCompleteForcedCooling(true, 50, 40, 50),
        "temperature equal to the threshold should keep force cooling active");
    Expect(!ShouldCompleteForcedCooling(true, 49, 50, 50),
        "either fan temperature at the threshold should keep force cooling active");
    Expect(ShouldCompleteForcedCooling(true, 49, 49, 50),
        "both temperatures below the threshold should complete force cooling");
}
```

- [ ] **Step 2: Build to verify the test fails**

Expected: missing `FanControlLogic.h` or unresolved predicate.

- [ ] **Step 3: Implement the pure predicate**

```cpp
bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
    int gpuTemperature, int threshold)
{
    return enabled && cpuTemperature < threshold && gpuTemperature < threshold;
}
```

Register the files in both projects.

- [ ] **Step 4: Add an interlocked completion sequence to the core**

Add `LONG forceCoolingCompletionSequence` to `CCoreStatusSnapshot` and `volatile LONG m_nForceCoolingCompletionSequence` to `CCore`. Initialize it to zero. In `GetStatusSnapshot`, read it with `InterlockedCompareExchange`.

In `Work`, retain the existing full-speed branch, but replace the below-threshold `else` condition with `ShouldCompleteForcedCooling`. When it returns true, clear `m_bForcedCooling` and call `InterlockedIncrement(&m_nForceCoolingCompletionSequence)` exactly once.

- [ ] **Step 5: Synchronize the completion into the dialog draft**

Add `LONG m_nLastForceCoolingCompletionSequence`, initialize it to zero, and add:

```cpp
void CClevoFanControlDlg::SyncForceCoolingCompletion(
    const CCoreStatusSnapshot& status)
{
    if (status.forceCoolingCompletionSequence ==
        m_nLastForceCoolingCompletionSequence)
    {
        return;
    }
    m_nLastForceCoolingCompletionSequence =
        status.forceCoolingCompletionSequence;
    if (!status.forcedCooling && m_draft.ForceCooling)
    {
        m_draft.ForceCooling = false;
        m_ctlForcedCooling.SetCheck(BST_UNCHECKED);
        SetDraftDirty(TRUE);
    }
}
```

Call it in `OnTimer` immediately after obtaining a valid status snapshot and before refreshing status controls. Always advance the observed sequence even when a newly applied preset is currently force cooling, so an old completion cannot clear the new action later.

- [ ] **Step 6: Run the focused tests and Release build**

Expected: test executable prints `PASS`; Release application compiles with the widened status snapshot and dialog method.

- [ ] **Step 7: Commit**

```powershell
git add -- ClevoFanControl/FanControlLogic.h ClevoFanControl/FanControlLogic.cpp ClevoFanControl/FanCurveModelTests.cpp ClevoFanControl/ClevoFanControlTests.vcxproj ClevoFanControl/ClevoFanControl.vcxproj ClevoFanControl/Core.h ClevoFanControl/Core.cpp ClevoFanControl/ClevoFanControlDlg.h ClevoFanControl/ClevoFanControlDlg.cpp
git commit -m "fix: synchronize force cooling completion"
```

### Task 5: Restore Global Configuration after the Last Match Exits

**Files:**
- Modify: `ClevoFanControl/PresetMatcher.h`
- Modify: `ClevoFanControl/PresetMatcher.cpp`
- Modify: `ClevoFanControl/FanCurveModelTests.cpp`
- Modify: `ClevoFanControl/ClevoFanControlDlg.cpp`

- [ ] **Step 1: Write failing desired-index tests**

Add a new matcher API:

```cpp
int ResolveAutomaticPresetIndex(
    const PresetCollection& collection,
    const std::vector<std::string>& processNames);
```

Before implementing it, add assertions to `TestPresetMatching`:

```cpp
std::vector<std::string> automaticRunning;
automaticRunning.push_back("game_dx12.exe");
Expect(ResolveAutomaticPresetIndex(collection, automaticRunning) == 0,
    "automatic resolution should return the first matching preset");
automaticRunning.clear();
automaticRunning.push_back("notepad.exe");
Expect(ResolveAutomaticPresetIndex(collection, automaticRunning) == -1,
    "automatic resolution should select global when no process matches");
```

- [ ] **Step 2: Build to verify the test fails**

Expected: unresolved `ResolveAutomaticPresetIndex`.

- [ ] **Step 3: Implement desired-index resolution**

Implement it by calling `FindMatchingPresetIndex`, returning the matched index or `-1`. Keep `FindMatchingPresetIndex` unchanged for existing callers and tests.

- [ ] **Step 4: Apply the desired index in the dialog**

After a successful process snapshot, replace the conditional-only match path with:

```cpp
const int selectedIndex = ResolveAutomaticPresetIndex(m_presets, processNames);
if (selectedIndex == m_nActivePreset)
{
    return;
}
if (selectedIndex >= 0)
{
    ApplyPresetAt(selectedIndex, FALSE, TRUE);
    return;
}
if (!ApplyGlobalConfiguration(FALSE))
{
    static ULONGLONG lastGlobalTraceTick = 0;
    const ULONGLONG traceTick = GetTickCount64();
    if (lastGlobalTraceTick == 0 || traceTick - lastGlobalTraceTick >= 5000)
    {
        TRACE0("Automatic global configuration restoration failed\n");
        lastGlobalTraceTick = traceTick;
    }
}
```

Do not check `m_bDraftDirty`; automatic changes deliberately replace the draft. Do not restore global configuration when process enumeration itself fails.

- [ ] **Step 5: Run tests**

Build Release and run the test executable. Expected: `PASS`, including the `-1` global-selection assertion.

- [ ] **Step 6: Commit**

```powershell
git add -- ClevoFanControl/PresetMatcher.h ClevoFanControl/PresetMatcher.cpp ClevoFanControl/FanCurveModelTests.cpp ClevoFanControl/ClevoFanControlDlg.cpp
git commit -m "fix: restore global config after preset exit"
```

### Task 6: Reconfigure the Core Timer after Interval Changes

**Files:**
- Modify: `ClevoFanControl/FanControlLogic.h`
- Modify: `ClevoFanControl/FanControlLogic.cpp`
- Modify: `ClevoFanControl/FanCurveModelTests.cpp`
- Modify: `ClevoFanControl/Core.h`
- Modify: `ClevoFanControl/Core.cpp`

- [ ] **Step 1: Write failing interval-decision tests**

Add:

```cpp
Expect(!ShouldRestartUpdateTimer(2, 2),
    "equal update intervals should keep the active timer");
Expect(ShouldRestartUpdateTimer(2, 1),
    "a changed update interval should restart the active timer");
```

Declare `bool ShouldRestartUpdateTimer(int activeInterval, int configuredInterval);` in `FanControlLogic.h` before implementing it.

- [ ] **Step 2: Build to verify the test fails**

Expected: unresolved `ShouldRestartUpdateTimer`.

- [ ] **Step 3: Implement the interval decision**

```cpp
bool ShouldRestartUpdateTimer(int activeInterval, int configuredInterval)
{
    return activeInterval != configuredInterval;
}
```

- [ ] **Step 4: Encapsulate multimedia timer ownership**

Add `int m_nTimerIntervalSeconds`, initialize it to zero, and add private methods:

```cpp
BOOL StartUpdateTimer(const TIMECAPS& caps, int intervalSeconds);
void StopUpdateTimer();
```

`StartUpdateTimer` first calls `StopUpdateTimer`, clamps `intervalSeconds * 1000` to `caps.wPeriodMin/wPeriodMax`, then calls `timeSetEvent` with:

```cpp
TIME_PERIODIC | TIME_CALLBACK_FUNCTION | TIME_KILL_SYNCHRONOUS
```

On success it records the validated seconds; on failure both timer fields remain zero. `StopUpdateTimer` calls `timeKillEvent`, then clears the timer ID and recorded seconds. Use it in the destructor and all worker exit paths.

- [ ] **Step 5: Reconfigure from the worker loop**

After each forced refresh is atomically consumed and `Work` completes, load the current config. If `ShouldRestartUpdateTimer(m_nTimerIntervalSeconds, config.UpdateInterval)` is true, restart the timer on the worker thread. If restart fails, break from the multimedia loop, fully stop it, and call `RunOriginal` while `GetExitState() == 0`.

Consume refresh requests with `InterlockedExchange(..., FALSE)` before `Work` instead of clearing afterward, so a timer callback arriving during `Work` remains pending for the next loop rather than being lost.

- [ ] **Step 6: Run focused and full configuration builds**

Run the test executable, then build Debug and Release Win32. Expected: tests print `PASS`; both configurations produce application and test executables without compiler errors.

- [ ] **Step 7: Commit**

```powershell
git add -- ClevoFanControl/FanControlLogic.h ClevoFanControl/FanControlLogic.cpp ClevoFanControl/FanCurveModelTests.cpp ClevoFanControl/Core.h ClevoFanControl/Core.cpp
git commit -m "fix: reconfigure runtime update timer"
```

### Task 7: Full Verification and Review Closure

**Files:**
- Verify all implementation files above.
- Do not stage or modify `README.md`.

- [ ] **Step 1: Build Debug Win32 from scratch**

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe' 'ClevoFanControl.sln' /m /t:Rebuild /p:Configuration=Debug /p:Platform=Win32 /v:minimal
```

Expected: application and test projects succeed.

- [ ] **Step 2: Build Release Win32 from scratch**

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe' 'ClevoFanControl.sln' /m /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /v:minimal
```

Expected: application and test projects succeed.

- [ ] **Step 3: Run the complete logic harness**

```powershell
& '.\Release\ClevoFanControlTests.exe'
```

Expected: `FanCurveModelTests: PASS` with exit code 0.

- [ ] **Step 4: Run repository checks**

```powershell
git diff --check
git status --short --branch
```

Expected: no whitespace errors; README remains the user's only unrelated working-tree modification.

- [ ] **Step 5: Re-review the seven original findings**

Confirm from the final code that:

1. No `FindWindow` title guard remains.
2. No-match automatic scans call the global-configuration path.
3. Task XML declares UTF-8 and is written as UTF-8 bytes.
4. The command path is XML-escaped.
5. Applied `UpdateInterval` changes restart the worker timer.
6. Automatic preset switching still intentionally replaces drafts.
7. Force-cooling completion updates core state, UI state, and the draft.

- [ ] **Step 6: Commit any verification-only project corrections**

Only if verification required project-file or test corrections, stage those exact files and commit:

```powershell
git commit -m "test: complete runtime correction coverage"
```

Do not create an empty commit.
