# Release Action v1.0.0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (\`- [ ]\`) syntax for tracking.

**Goal:** Repair release-tag validation, rebuild and test the Win32 Release configuration, then publish \`v1.0.0\` with the four required runtime files.

**Architecture:** Keep the workflow's existing manual-dispatch, build, test, package, and publish stages. Replace only the release-existence probe with a typed PowerShell HTTP request that treats HTTP 404 as absent and all other failures as fatal; retain the remote-tag check and publish against the workflow SHA.

**Tech Stack:** GitHub Actions, PowerShell 7, GitHub REST API, GitHub CLI, MSBuild, Visual Studio C++/MFC, Win32.

---

### Task 1: Replace the brittle release-existence check

**Files:**
- Modify: \`.github/workflows/release.yml:27-53\`

- [ ] **Step 1: Replace text-based 404 matching with typed HTTP status handling**

Replace the current \`$releaseLookup = gh api ...\` block with:

~~~powershell
          $releaseTag = $env:RELEASE_TAG.Trim()
          $tagPattern = '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$'
          if (-not [System.Text.RegularExpressions.Regex]::IsMatch($releaseTag, $tagPattern)) {
            throw "Release tag '$releaseTag' must use a version such as v1.2.3 or v1.2.3-beta.1."
          }
          Write-Output "Release tag validation passed for '$releaseTag'."

          $releaseUri = "https://api.github.com/repos/$env:GITHUB_REPOSITORY/releases/tags/$releaseTag"
          $releaseRequestHeaders = @{
            Accept = 'application/vnd.github+json'
            Authorization = "Bearer $env:GH_TOKEN"
            'X-GitHub-Api-Version' = '2022-11-28'
          }

          $releaseStatusCode = 0
          try {
            Invoke-RestMethod -Method Get -Uri $releaseUri -Headers $releaseRequestHeaders -SkipHttpErrorCheck -StatusCodeVariable releaseStatusCode | Out-Null
          }
          catch {
            throw "Unable to check whether GitHub Release '$releaseTag' exists: $($_.Exception.Message)"
          }

          Write-Output "Release lookup returned HTTP $releaseStatusCode for '$releaseTag'."
          if ($releaseStatusCode -eq 200) {
            throw "GitHub Release '$releaseTag' already exists."
          }
          if ($releaseStatusCode -ne 404) {
            throw "Unable to check whether GitHub Release '$releaseTag' exists: HTTP $releaseStatusCode."
          }
~~~

Keep the \`git ls-remote\` check immediately after this block. The build and test steps remain unchanged. Package and publish steps must normalize the same input before using it:

~~~powershell
$releaseTag = $env:RELEASE_TAG.Trim()
$packageName = "ClevoFanControl-$releaseTag-win32"
$arguments = @('release', 'create', $releaseTag, $env:RELEASE_ARCHIVE)
~~~

The workflow also prints the normalized tag and HTTP status without exposing the token, so a hosted-runner failure identifies the validation phase.

- [ ] **Step 2: Check the workflow diff and whitespace**

Run:

~~~powershell
git diff --check
git diff -- .github/workflows/release.yml
~~~

Expected: no whitespace errors; only the release-existence validation block changes; the existing tag, MSBuild, test, package, and publish stages remain present.

### Task 2: Verify the validation logic locally

**Files:**
- Verify: \`.github/workflows/release.yml:27-70\`

- [ ] **Step 1: Run the absent-release and absent-tag checks against GitHub**

Run:

~~~powershell
$ErrorActionPreference = 'Stop'
$env:GITHUB_REPOSITORY = 'XiaoQing235/ClevoFanControl'
$env:RELEASE_TAG = 'v1.0.0'
$releaseTag = $env:RELEASE_TAG.Trim()
$tagPattern = '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$'
if (-not [System.Text.RegularExpressions.Regex]::IsMatch($releaseTag, $tagPattern)) {
  throw "Invalid release tag: $releaseTag"
}
$releaseUri = "https://api.github.com/repos/$env:GITHUB_REPOSITORY/releases/tags/$releaseTag"
$releaseRequestHeaders = @{
  Accept = 'application/vnd.github+json'
  Authorization = "Bearer $env:GH_TOKEN"
  'X-GitHub-Api-Version' = '2022-11-28'
}
$releaseStatusCode = 0
try {
  Invoke-RestMethod -Method Get -Uri $releaseUri -Headers $releaseRequestHeaders -SkipHttpErrorCheck -StatusCodeVariable releaseStatusCode | Out-Null
}
catch {
  throw "Request failed for '$releaseTag': $($_.Exception.Message)"
}
Write-Output "Release lookup returned HTTP $releaseStatusCode for '$releaseTag'."
if ($releaseStatusCode -eq 200) { throw "Release already exists" }
if ($releaseStatusCode -ne 404) { throw "Unexpected release status: $releaseStatusCode" }
$remoteTagLookup = @(git ls-remote --tags origin "refs/tags/$releaseTag" 2>&1)
$tagLookupExitCode = $LASTEXITCODE
Write-Output "Remote tag lookup exited $tagLookupExitCode with $($remoteTagLookup.Count) output line(s)."
if ($tagLookupExitCode -ne 0) { throw "Unable to check remote tag: $($remoteTagLookup -join ' ')" }
if ($remoteTagLookup.Count -gt 0) { throw "Remote tag already exists: $($remoteTagLookup -join ' ')" }
Write-Output 'VALIDATION_OK'
~~~

Expected: \`VALIDATION_OK\`; the API returns 404 for the absent release and \`git ls-remote --tags\` returns exit code 0 with no output for the absent tag.

- [ ] **Step 2: Confirm the workflow still has the required release inputs and outputs**

Run:

~~~powershell
Select-String -Path .github/workflows/release.yml -Pattern 'workflow_dispatch|tag:|prerelease:|msbuild ClevoFanControl.sln|ClevoFanControlTests.exe|Compress-Archive|gh @arguments'
~~~

Expected: all six workflow responsibilities are found.

### Task 3: Rebuild and run the Release Win32 tests

**Files:**
- Verify: \`ClevoFanControl.sln\`
- Verify: \`ClevoFanControl\\ClevoFanControl.vcxproj\`
- Verify: \`ClevoFanControl\\ClevoFanControlTests.vcxproj\`

- [ ] **Step 1: Locate the installed Visual Studio MSBuild command**

Run from a Visual Studio developer PowerShell, or initialize the installed developer environment before running the next step:

~~~powershell
Get-Command msbuild
~~~

Expected: \`msbuild.exe\` resolves to the installed Visual Studio toolchain. If the ordinary PowerShell does not resolve it, run the build from the installed \`VsDevCmd.bat\` environment.

- [ ] **Step 2: Rebuild both Release Win32 projects**

Run:

~~~powershell
msbuild ClevoFanControl.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=Win32
~~~

Expected: exit code 0 and \`Release\\ClevoFanControl.exe\` plus \`Release\\ClevoFanControlTests.exe\` are produced.

- [ ] **Step 3: Run the test executable**

Run:

~~~powershell
& .\Release\ClevoFanControlTests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
~~~

Expected: exit code 0.

### Task 4: Commit, push, and dispatch the formal release

**Files:**
- Commit: \`.github/workflows/release.yml\`
- Preserve: \`README.md\`

- [ ] **Step 1: Review status and stage only the workflow**

Run:

~~~powershell
git status --short
git diff --check
git add .github/workflows/release.yml
git diff --cached --check
git diff --cached -- .github/workflows/release.yml
~~~

Expected: only \`.github/workflows/release.yml\` is staged; the pre-existing \`README.md\` modification is unstaged and untouched.

- [ ] **Step 2: Commit the workflow fix**

Run:

~~~powershell
git commit -m "fix: make release validation handle missing releases"
~~~

Expected: one new commit containing only the workflow fix.

- [ ] **Step 3: Push the repaired main branch**

Run:

~~~powershell
git push origin main
~~~

Expected: origin/main advances to the workflow-fix commit; no README changes are pushed.

- [ ] **Step 4: Dispatch v1.0.0 as a non-prerelease**

Run:

~~~powershell
gh workflow run release.yml --repo XiaoQing235/ClevoFanControl --ref main -f tag=v1.0.0 -f prerelease=false
~~~

Expected: GitHub accepts the manual dispatch.

- [ ] **Step 5: Wait for the dispatched run and preserve its result**

Run:

~~~powershell
$run = gh run list --repo XiaoQing235/ClevoFanControl --workflow release.yml --limit 1 --json databaseId,status,conclusion,headSha,url | ConvertFrom-Json
gh run watch $run.databaseId --repo XiaoQing235/ClevoFanControl --exit-status
~~~

Expected: the run completes successfully; if it fails, run \`gh run view $run.databaseId --repo XiaoQing235/ClevoFanControl --log-failed\` before changing anything.

### Task 5: Verify the published Release and package contents

**Files:**
- Verify: GitHub Release \`v1.0.0\`
- Verify: \`ClevoFanControl-v1.0.0-win32.zip\`

- [ ] **Step 1: Inspect the Release metadata and asset list**

Run:

~~~powershell
gh release view v1.0.0 --repo XiaoQing235/ClevoFanControl --json tagName,targetCommitish,isDraft,isPrerelease,assets,url
~~~

Expected: tag name \`v1.0.0\`, \`isDraft=false\`, \`isPrerelease=false\`, and asset \`ClevoFanControl-v1.0.0-win32.zip\`.

- [ ] **Step 2: Download and inspect the release archive**

Run:

~~~powershell
$verificationDirectory = Join-Path $env:TEMP 'ClevoFanControl-v1.0.0-verification'
New-Item -ItemType Directory -Force -Path $verificationDirectory | Out-Null
gh release download v1.0.0 --repo XiaoQing235/ClevoFanControl --pattern 'ClevoFanControl-v1.0.0-win32.zip' --dir $verificationDirectory --clobber
$extractDirectory = Join-Path $verificationDirectory 'extracted'
Expand-Archive -LiteralPath (Join-Path $verificationDirectory 'ClevoFanControl-v1.0.0-win32.zip') -DestinationPath $extractDirectory -Force
$requiredFiles = @('ClevoFanControl.exe', 'NVGPU_DLL.dll', 'NTPortDrvSetup.exe', 'ClevoEcInfo.dll')
foreach ($requiredFile in $requiredFiles) {
  if (-not (Test-Path -LiteralPath (Join-Path $extractDirectory $requiredFile) -PathType Leaf)) {
    throw "Missing release file: $requiredFile"
  }
}
Write-Output 'PACKAGE_OK'
~~~

Expected: \`PACKAGE_OK\` and all four required runtime files exist in the archive.

- [ ] **Step 3: Verify the final worktree boundary**

Run:

~~~powershell
git status --short
git log -3 --oneline --decorate
~~~

Expected: the workflow-fix commit is on \`main\`; only the pre-existing \`README.md\` remains as an unrelated local modification.
