# Release Action v1.0.0 Design

## Goal

Fix the release workflow's false failure while checking that the requested GitHub Release does not already exist, then build, test, package, and publish `v1.0.0` from the repaired workflow.

## Current Evidence

- The repository is a Visual Studio C++/MFC solution with a Release Win32 configuration and a console test executable.
- The workflow at `.github/workflows/release.yml` is manually dispatched with a release tag and prerelease flag.
- Run `30192284758` rejected `v1.0` at the version-format check, as expected for a non-semver three-part release tag.
- Run `30192335198` accepted `v1.0.0` but failed inside `Validate release tag` before MSBuild ran.
- The repository has no existing `v1.0.0` tag or release. The local GitHub API check returns HTTP 404 and `git ls-remote --tags` returns exit code 0 with no output for the absent tag.
- `README.md` has an unrelated uncommitted user change and must remain outside this work.

## Chosen Approach

Replace the release-existence check's text matching with a typed PowerShell HTTP request. `Invoke-RestMethod` will query the release-by-tag endpoint using the workflow token. A 200 response means the release exists and the workflow stops. A 404 response means it is safe to continue. Any other status, transport failure, or malformed response is fatal and includes the original error.

Keep the tag validation as a separate `git ls-remote --tags` check without `--exit-code`. A successful command with no output means the remote tag is absent; any output means the tag exists; any nonzero command exit is an infrastructure or repository error. This avoids relying on a runner-specific no-match exit code while preserving the protection against publishing a requested version against an unexpected existing tag.

Normalize the dispatch input with `Trim()` once before validation and reuse the normalized value for the release lookup, package name, and publish command. Emit the normalized tag and HTTP status as non-secret diagnostics so hosted-runner failures identify the validation phase without exposing the token.

Leave the build, test, packaging, and `gh release create --target $GITHUB_SHA` steps unchanged. After the repair is pushed to `main`, manual dispatch with `tag=v1.0.0` and `prerelease=false` will build the pushed commit. GitHub CLI will create the new tag from the workflow SHA when the release is published.

## Workflow Data Flow

1. Checkout the complete repository history.
2. Validate the tag syntax.
3. Query the release endpoint and classify only HTTP 404 as "not found".
4. Query the remote tag and classify only a successful empty response as "not found".
5. Rebuild both solution projects as Release Win32 with MSBuild.
6. Run `Release\\ClevoFanControlTests.exe` and preserve its exit code.
7. Verify and stage the four runtime files, then create `ClevoFanControl-v1.0.0-win32.zip`.
8. Publish the archive as the `v1.0.0` GitHub Release, targeting the workflow SHA.

## Failure Handling

- Existing release: fail before compilation with an explicit duplicate-release message.
- Existing tag: fail before compilation with an explicit duplicate-tag message.
- GitHub API status other than 404: fail rather than treating an outage or permission error as a missing release.
- Missing build output, test failure, or missing runtime dependency: fail before publication.
- Publication failure: leave the build artifacts in the failed runner for diagnostics; do not retry or delete remote state automatically.

## Verification

- Run `git diff --check` and inspect the workflow diff.
- Run the Release Win32 solution rebuild with the same MSBuild properties used by CI.
- Run the produced test executable and record its exit code.
- Execute the release-check logic locally against the absent `v1.0.0` release and tag, including the 404 and exit-code branches.
- After pushing, inspect the GitHub Actions run and its failed log if it fails.
- After a successful run, verify the `v1.0.0` release, tag target, archive name, and all four archive assets.

## Scope

Only `.github/workflows/release.yml` and this design document are part of the implementation. No application source, dependency binaries, or unrelated `README.md` changes are included.
