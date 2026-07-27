# HDF5 Concurrency Audit

## Status and decision

Current status: retain the conservative global `Hdf5IO` lock. No interface is
approved for per-file concurrent use. This document records source-level
evidence only; no build, runtime probe, regression test, or stress test has
been executed in this phase.

The evidence required to replace the global lock with path and file-identity
read/write locks is incomplete. In particular, `getRuntimeInfo` and
`getRuntimeModulePath` have not yet been called and recorded by a test harness,
the lifecycle implementation and harness have not yet been built or run, and
no concurrency or cross-process tests have been run.

## Static findings

- Direct HDF5 API calls are confined to `Hdf5IO/Hdf5IO.cpp`. `FormatConversion`
  and `Utils` use `Hdf5IO` rather than a second HDF5 handle layer.
- Every current `Hdf5IO` public operation enters `ScopedHdf5Lock`. Its named
  mutex is `Local\\InSAR.Hdf5IO.v1`; `Local` reaches processes in the same
  Windows logon session, but this is an implementation detail and does not
  establish a complete cross-process protocol.
- `ReadSession` and `WriteSession` own an HDF5 file ID in an opaque DLL object.
  They are not global handles, but they also have no thread-affinity, reference
  count, or close-versus-use coordination. A session is therefore single-owner
  and must not be used or closed concurrently by multiple Workers.
- No HDF5 error callback registration (`H5E*`) was found. `Hdf5IO` contains no
  global or static HDF5 file/dataset IDs, cached `cv::Mat`, mutable temporary
  path, or shared data buffer.
- `FormatConversion` owns a mutable Sentinel diagnostic map, protected by
  `g_sentinel_diagnostic_mutex`, plus thread-local diagnostic state. This state
  is not an HDF5 handle cache, but its callback and cancellation paths remain
  part of the full DLL concurrency audit.
- `copyDatasetsIfPresent` intentionally skips missing source datasets, but now
  propagates invalid dataset names and individual delete/copy failures. A
  failure can still follow prior successful copies, so its partial-update
  semantics require targeted cancellation and integrity testing before any
  reduction of locking.

### Export and state audit

This is a source inspection record, not runtime evidence. `Hdf5IO.cpp` is the
only translation unit with direct `H5*` calls. Every path-created file ID uses
`ScopedAuditedH5File`; datasets, spaces, types, attributes, groups, and
properties use local `ScopedH5Id` values. No global or static `hid_t` was
found. There is no `H5E*` callback or error-stack operation in the repository.

The mutable state that can affect an HDF5-related workflow is limited to:

- `ScopedHdf5Lock`, a named Windows mutex retained as the only active HDF5
  serialization mechanism. It is process-session scoped, not a replacement
  for a documented distributed or crash-safe file-lock protocol.
- The opt-in `g_audit` queue, its mutex, generation, and atomic handle counter.
  It is diagnostic state, never an HDF5 handle cache. `g_auditOperationId` is
  thread-local.
- Opaque `ReadSession` and `WriteSession` objects, each owning one file ID.
  They have no reference count, thread affinity, or close-versus-use protocol;
  a session is single-owner and may not cross worker boundaries.
- `FormatConversion`'s `g_sentinel_diagnostic_state`, protected by
  `g_sentinel_diagnostic_mutex`, plus thread-local diagnostic state. Its GDAL
  initialization uses `std::once_flag`. These are not HDF5 resources, but its
  callback/cancellation behavior remains outside any future claim of full
  workflow parallelism. `Deflat` also serializes its topography callback state;
  `Utils` has a GDAL/projection `once_flag`.

The high-level direct callers found by this pass are `FormatConversion`
(including `SARDataReader` and `SARReaders`), `Utils` (`S1Merge`, `UtilsAoi`,
and `UtilsBaseline`), and `Registration`. `Deflat`, `SBAS`, and `simulation`
contain no direct HDF5 API call or `Hdf5IO::` call in the inspected sources;
they may still enter HDF5 indirectly through their public workflow
dependencies. Every direct caller either uses path APIs or retains sessions in
`unique_ptr` with the corresponding close deleter. This proves neither
application-level cancellation correctness nor a lack of all non-HDF5 shared
state, so it is not authority to relax serialization.

## Per-export concurrency contract

Every export that can enter HDF5 remains **process-session globally serial**.
The named mutex is acquired by the export or by a caller-held `BatchLock`; a
Windows mutex is recursive only for its owning thread, so a batch lock may not
be passed to another worker. `H5is_library_threadsafe=true`, if observed later,
does not change this table by itself.

| Exports (all overloads named) | Current contract | Source reason and caller restriction |
| --- | --- | --- |
| `getRuntimeInfo`, `getRuntimeModulePath` | Global serial | Invoke HDF5 runtime APIs under `ScopedHdf5Lock`; results have not been recorded from a loaded DLL. |
| `enableAuditDiagnostics`, `disableAuditDiagnostics`, `setAuditOperationId`, `getAuditStatus`, `pollAuditEvent` | Diagnostic control only; no HDF5 parallelism granted | Queue state is separately synchronized; enable reads HDF5 version under the global lock; operation ID is thread-local. |
| `areSameExistingFile`, `validateDistinctFilePaths` | Global serial | Windows identity preflight is serialized with HDF5 work; path identity is not a future path-lock key. |
| `acquireBatchLock`, `getBatchLockStatus`, `releaseBatchLock` | Global serial for the owning thread | Opaque lock guards a caller-defined sequence; it does not make sessions shareable. |
| `createFile` | Global serial | Truncates/creates a file through audited RAII; no same-file or path-level write contract is approved. |
| `getDatasetDims`, path `readArray`, `readDouble`, `readInt`, `readString`, path `readSubarray` | Global serial | Each uses a local file handle but HDF5 runtime, DLL state, and cross-process behavior are not yet evidenced. |
| `openReadSession`, `closeReadSession`, both session `readArray` overloads, session `readSubarray`, `readInterleavedComplexFloat`, `readStringAttribute`, `readArrayAttribute` | Global serial; one owner per session | File ID remains opaque and local to the session, but no reference-count/close-versus-use protocol exists. |
| `openWriteSession`, `closeWriteSession`, session `writeArray`, session `writeArrayReplace`, session `createString` | Global serial; one owner per session | Same session rule; close failure invalidates audit evidence in the harness. |
| path `writeArray`, `writeArrayReplace`, `createEmptyDataset`, `createZeroDataset`, `writeSubarray`, `writeDouble`, `writeInt`, `writeString`, path `createString`, `removeDatasetIfPresent` | Global serial | All mutate metadata or data and may expose partial state between public calls. |
| `copyDatasetIfPresent`, `copyDatasetsIfPresent` | Global serial | Both source and destination are opened by path; partial update after an earlier copy remains possible. |

`FormatConversion::Copy_para_from_h5_2_h5` retains the same global contract:
it holds a batch lock across the two copy groups and rejects same-identity
source/output before HDF5 copying. No export is presently permitted to use a
path-shared lock, path-exclusive lock, or a per-file concurrency claim. The
future path/identity-lock design below remains a proposal only.

## Four-part completion status

| Audit part | Source-level status | Runtime evidence status | Current conclusion |
| --- | --- | --- | --- |
| DLL and HDF5 build evidence | `getRuntimeInfo`, `getRuntimeModulePath`, and audit-enabled harness source are present | Ordinary and historical audit-enabled builds completed, but no current opt-in audit build or run has produced version, thread-safety, module-path, or event evidence | Incomplete. A future `threadsafe=true` result is necessary evidence only, and the CRT conflict must first be resolved or justified. |
| Export and shared-state audit | Direct `H5*` use, direct callers, HDF5 ownership, and observable shared state are recorded above | No dynamic cancellation/error-path trace | Complete as a bounded source inspection; incomplete as a full workflow-concurrency proof. |
| Per-export concurrency contract | Every `Hdf5IO` export is assigned above | No evidence supports any lock narrowing | Complete for the current conservative policy: all HDF5-capable exports stay globally serial. |
| Stress tests and pass/fail criteria | Harness source covers independent files, same-target writes/copies, reverse copies, read/write overlap, cross-process rows, lifecycle pairing, and watchdogs | Built but not run; cancellation, replacement, hash, object inventory, and crash recovery cases are absent | Incomplete. No stress claim or concurrency-safety claim may be made. |

Accordingly, the four parts are not all complete: only the bounded source
inspection and conservative contract are recorded. The build/runtime and stress
evidence required to validate a concurrency conclusion remain outstanding.

### Recorded build evidence

The user-provided Visual Studio Debug x64 solution build completed in 64.005
seconds with 10 projects successful and no failures. It rebuilt and linked
`Hdf5IO_d.dll`, plus `Utils`, `Registration`, `FormatConversion`, `Deflat`,
`SBAS`, `Unwrap`, `Dem`, `Evaluation`, and `simulation`. The solution test
projects `test`, `test2`, and `test3` were skipped by the selected solution
configuration.

This is useful integration evidence for the conservative source baseline, but
it is **not** an audit-enabled harness build: at the time of the supplied log,
`Hdf5AuditHarness` was not yet included in the solution, and the log does not
establish that `Hdf5AuditDiagnostics=true` was passed to `Hdf5IO`. The harness
is now a solution project with an explicit `Hdf5IO` project reference, but is
excluded from all solution `Build.0` mappings; normal solution builds neither
build the harness nor enable diagnostics unless explicitly requested.
No lifecycle event,
runtime-version, HDF5 thread-safety, module-path, or stress result was
produced. The `Hdf5IO` link also emitted `LNK4098` for a default-library
`MSVCRT` conflict. That runtime-library mismatch must be resolved or given a
documented compatibility justification before treating a later runtime harness
run as dependable evidence.

Static dependency inspection after the warning found that the local
`D:\SRC\HDF5\lib` directory has only un-suffixed HDF5 library names, while
the Debug `Hdf5IO` configuration uses `/MDd` and links OpenCV's Debug library.
The HDF5 bin directory similarly contains the release CRT redistributables,
not a debug HDF5 package. This does not by itself prove the CRT settings of
every HDF5 object, but it is consistent with the recorded `MSVCRT` warning and
does not establish compatibility. Do not add `/NODEFAULTLIB` merely to hide
the warning. Before a harness run is admitted as audit evidence, either supply
HDF5 and all of its static dependencies built for the Debug CRT, or document a
reviewed ABI and memory-ownership argument that justifies the exact mixed-CRT
configuration.

After the harness was added to the solution, a Debug x64 harness build reached
compilation but failed at link with unresolved OpenCV `cv::Mat` symbols
(`cv::fastFree`, `cv::error`, `cv::countNonZero`, and related methods). The
cause was a missing link dependency in `Hdf5AuditHarness.vcxproj`, not an HDF5
or audit API failure. The project now links
`D:\SRC\opencv\build\x64\vc15\lib\opencv_world450d.lib` for Debug and
`opencv_world450.lib` for Release, matching the Hdf5IO project. The libraries
exist locally. The following build record verifies this repair.

The follow-up Debug x64 build completed successfully in 8.800 seconds:
`Hdf5AuditHarness_d.exe` was produced with 1 project successful and no
failures. This verifies the solution entry, `Hdf5IO` project reference, and
OpenCV link repair. It remains a normal Debug build rather than recorded proof
of an audit-enabled build, because the supplied log does not show
`Hdf5AuditDiagnostics=true`; no harness execution occurred.

The subsequent user-provided Debug x64 build at 12:37 completed with both
`Hdf5IO` and `Hdf5AuditHarness` successful (6.110 seconds). Before that build,
the Debug x64 `Hdf5IO` configuration was explicitly given the
`HDF5IO_ENABLE_AUDIT_DIAGNOSTICS` preprocessor definition through Visual
Studio. The rebuilt `Hdf5IO_d.dll` therefore contains the lifecycle audit
channel, and the rebuilt `Hdf5AuditHarness_d.exe` is compatible with it. This
is historical audit-enabled build evidence only: the harness has not run and
no runtime, event, or stress-test evidence exists. The direct Debug macro was
removed afterwards because it broke the opt-in contract. A fresh audit-enabled
artifact must be built with `Hdf5AuditDiagnostics=true` before any run. The
`LNK4098` for the `MSVCRT` default-library conflict was still emitted by the
`Hdf5IO` link and remains unresolved; no harness execution may be treated as
reliable audit evidence until it is resolved or its CRT compatibility is
formally established.

## Self-copy contract

`Copy_para_from_h5_2_h5(A, B)` now validates identity before any HDF5 copy:

- Empty inputs return `Hdf5IO::kInvalidArgument`.
- Existing source and target files are opened using normal Windows path
  resolution. Their volume serial number and file index are compared, covering
  path aliases, reparse-point resolution, and hard links.
- If both paths resolve to the same physical file, the call returns
  `Hdf5IO::kInvalidArgument` without opening either file through HDF5 or
  registering copy/cleanup work.
- The conservative global lock is held during identity validation and both
  copy batches. This serialization does not make self-copy valid; self-copy is
  rejected as an input error.
- Every active caller now checks the copy result before its next write or
  recovery action. The caller must propagate this error as terminal for the
  current workflow.
- Every active workflow that later invokes this copy API now calls
  `validate_distinct_h5_output(source, output)` before its first
  `creat_new_h5(output)`. The preflight permits a missing output path but
  rejects an existing output with the same Windows file identity as the source.

The source and destination must already exist for this API. Failure to obtain
either file identity fails the operation rather than guessing from path text.
This baseline check does not eliminate a time-of-check/time-of-use race with an
uncoordinated file replacement after the Windows identity handles are closed
and before HDF5 reopens the paths. The future path-lock and cross-process
protocol is required before that stronger guarantee can be claimed.

This contract applies to the copy interface itself and its current high-level
callers. Any new workflow that creates, truncates, or writes its output before
invoking the copy API must perform the same distinct-file preflight before its
first output mutation; the copy API cannot undo a prior mutation.

## Required evidence before lock narrowing

1. Call `getRuntimeInfo` and `getRuntimeModulePath` in the test harness and
   record a successful `H5get_libversion` plus
   `H5is_library_threadsafe(&isThreadSafe)` result, together with the module
   path containing the linked HDF5 runtime symbol. A true thread-safety result
   is necessary evidence only; it is not a parallel-throughput claim.
2. Complete the export-by-export audit, including callers in
   `FormatConversion`, `Utils`, `Deflat`, `SBAS`, `Registration`, and
   `simulation`.
3. Build and run the test-build DLL lifecycle diagnostics with operation ID, thread ID,
   request path, open mode/result, post-open path-resolved diagnostic identity, HDF5
   version, monotonic timestamps, and paired open/close handle IDs. The
   callback must not re-enter HDF5 or wait for a GUI. Queue overflow must mark
   the test evidence incomplete and fail that test run.
4. Define and verify cross-process coordination for every process that can
   touch the same file. The existing `Local` mutex alone is not that protocol.
5. Run the specified single-process and cross-process stress, cancellation,
   retry, atomic-replacement, byte-hash, object/attribute inventory, and
   reopen-integrity tests.

Until every item is complete and passing, the fallback conclusion is unchanged:
all HDF5 operations remain protected by the conservative global lock.

## Future path and identity lock contract

This contract is not enabled by the current implementation. It becomes
eligible only after all required evidence passes.

- A normalized absolute path is only a candidate comparison and a temporary
  lock key. It is never proof that two existing files differ.
- Existing HDF5 files are identified after normal path resolution by the DLL's
  opened Windows handle: volume serial number plus file index. This recognizes
  hard links and follows reparse points by default.
- A not-yet-created target uses its normalized path as a temporary key. While
  retaining that path lock, creation resolves and registers the identity key.
- The sole lock hierarchy is path locks followed by identity locks. No code may
  request a path lock while holding an identity lock. Multiple locks of either
  class are acquired in a stable total order.
- For a DLL API that receives paths and may reopen them internally, both path
  and identity locks remain held through DLL work, result validation, and
  rollback. Release identity locks first and path locks second. Releasing the
  path lock after identity resolution is forbidden because an atomic replace
  could rebind the path before the DLL's later open.
- A future `Copy_para_from_h5_2_h5(A, B)` takes a shared source identity lock
  and exclusive destination identity lock, after the path-lock stage. If both
  paths identify the same file, it rejects with `kInvalidArgument`; it never
  relies on an exclusive lock to make self-copy valid. An initial conservative
  implementation may take both locks exclusively.
- Every in-process caller must use the same manager. Any independent process
  that can access the same HDF5 file must follow a separately documented
  cross-process protocol. A named mutex, lock file, or Windows file lock is
  acceptable only after its crash recovery and replacement semantics are
  specified and tested.
- `hid_t` may cross the application/DLL boundary only when both modules use a
  compatible HDF5 runtime and ABI and the interface defines ownership, close
  responsibility, and lifetime. Otherwise the DLL must retain the handle and
  expose lifecycle diagnostics instead.

## Test-build lifecycle diagnostic contract

This is the implemented design for the opt-in DLL audit build. It is separate
from the existing general-purpose `InSARDiagnosticEvent` callback because that
that callback borrows string pointers and does not provide HDF5 handle pairing.

The `Hdf5IO` project defaults `Hdf5AuditDiagnostics` to `false`. The lifecycle
channel is compiled only when explicitly selected with
`/p:Hdf5AuditDiagnostics=true`; `_DEBUG` alone must never enable it. Normal
Debug and Release builds keep the channel absent/default-off. Enabling
diagnostics does not change `ScopedHdf5Lock` acquisition or release behavior.

Each event queued by `Hdf5IO` must own copies of all string and scalar fields:

| Field | Open event | Close event |
| --- | --- | --- |
| Operation ID | Required, non-zero, supplied before the first HDF5 operation | Same non-zero ID as its open event |
| File-handle ID | Fresh non-zero ID only after a successful HDF5 open | Same ID as its open event |
| Thread ID | Required | Required |
| Requested path and open mode | Required | Required or copied from the opening record |
| Result | Success or failure | Close result |
| File identity | Best-effort path-resolved diagnostic only; not proof of HDF5's opened object | Identity copied from open record |
| HDF5 runtime version | Required | Required or copied from open record |
| Timestamp | Monotonic clock | Same comparable monotonic clock |

Failed HDF5 opens have no file-handle ID and never generate a later close
event. The event sink is a bounded asynchronous queue. A queue overflow marks
the evidence incomplete, increments a counter, and retains the first and last
dropped operation IDs plus monotonic timestamps in `Hdf5AuditStatus`. It does
not wait for a GUI or log consumer. An inability to read the Windows file
identity immediately after a successful HDF5 open also marks evidence
incomplete, so an all-zero identity is never accepted as audit evidence.
Likewise, an event with `operationId == 0` is retained for diagnosis but marks
the evidence range incomplete. The harness also rejects it directly. Callers
must set a non-zero operation ID before the first path or session open/create
in an audited operation; zero is not an "unattributed but valid" value.

The lifecycle identity is intentionally weaker than the Windows-handle identity
used by `areSameExistingFile` and `validateDistinctFilePaths`. After `H5Fopen`
or `H5Fcreate` succeeds, the diagnostic code opens the requested path a second
time with `CreateFileA` to obtain volume serial number and file index. An
uncoordinated replacement in that interval can make the event describe the
replacement rather than HDF5's already-open object. It is therefore useful for
best-effort path-event correlation only; it cannot prove the HDF5 object
identity, authorize lock narrowing, or close the replacement race.

The event producer and callback must never call HDF5. They must not hold an
application lock while waiting, and callers must treat the callback as
non-reentrant and `noexcept`. Application lock acquire/release records use the
same operation ID and a comparable monotonic clock so that path-lock,
identity-lock, open, close, validation, and rollback intervals can be audited
in one timeline.

## Implementation record

Completed in the current source baseline:

- Conservative `ScopedHdf5Lock` remains the only active HDF5 concurrency
  policy.
- `Copy_para_from_h5_2_h5` rejects a same-identity source/output and propagates
  copy failures; all active high-level copy workflows preflight before their
  first output file creation.
- `getRuntimeInfo` and `getRuntimeModulePath` provide the values that the
  future harness must record.
- `Hdf5AuditDiagnostics` is an explicit, default-off `Hdf5IO` build property.
  `_DEBUG` does not enable audit diagnostics.
- `ScopedAuditedH5File` is the sole owner for every path-based `H5Fopen`,
  `H5Fcreate`, and `H5Fclose` in `Hdf5IO`. It emits failed-open records without
  a handle ID and emits close records only for successful opens. Session
  construction failure, ordinary stack unwinding, copy source/destination
  opens, and session close use that same owner.
- Successful open records use a fresh handle ID and obtain a best-effort,
  path-resolved volume serial number plus file index through a second normal
  Windows `CreateFile` handle. The value is retained with the requested path,
  mode, and runtime version for close-event correlation, but is not identity
  proof for HDF5's already-open object.
- `Hdf5AuditHarness/Hdf5AuditHarness.vcxproj` provides source for runtime
  reporting, event-pair validation, single-process multi-thread pressure, and
  cross-process same-file subarray writes with final-content validation.
- This document now contains the source-level direct-caller/shared-state record
  and a per-export concurrency contract. The result is deliberately conservative:
  every HDF5-capable export remains globally serial.
- The harness source also contains same-target writer competition, same-file
  reader/writer competition, two-source-to-one-target copy competition, and
  simultaneous `A -> B` / `B -> A` copy competition. The contention set runs in
  a child process with a 120-second watchdog so a deadlock produces a non-zero
  parent result instead of indefinitely blocking the runner.

Still pending:

- Building the audit-enabled DLL and harness, then recording its runtime
  evidence from an actual DLL process.
- Running the harness and the broader cancellation, retry, atomic-replacement,
  hash, object/attribute-inventory, and reopen-integrity stress suites.

No runtime evidence has been claimed from source inspection alone.

## Harness procedure

`Hdf5AuditHarness` is deliberately an opt-in console project and is not part of
the existing `test`, `test2`, or `test3` executables. Build the harness and its
`Hdf5IO` project reference with the same global property, for example:

```powershell
msbuild .\Hdf5AuditHarness\Hdf5AuditHarness.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:Hdf5AuditDiagnostics=true
```

To enable the same property from Visual Studio without adding the diagnostic
macro directly to the Debug configuration, use `View` > `Other Windows` >
`Property Manager`. Expand `Hdf5IO` > `Debug | x64`, right-click `Debug | x64`,
and select `Add New Project Property Sheet`. Store the temporary sheet outside
the repository, then right-click that new sheet and select `Properties`.
Under `Common Properties` > `User Macros`, add the following macro:

| Name | Value |
| --- | --- |
| `Hdf5AuditDiagnostics` | `true` |

The project has a Debug x64 `PropertySheets` import point before its audit
property is evaluated, so this user macro selects the same opt-in build path as
the command line. Rebuild `Hdf5IO` first, then explicitly build
`Hdf5AuditHarness`; the harness remains excluded from normal solution builds.
After collecting evidence, remove the property sheet from `Debug | x64`, delete
the temporary sheet, and rebuild `Hdf5IO` to restore the ordinary default-off
DLL. Do not add `HDF5IO_ENABLE_AUDIT_DIAGNOSTICS` to the project's direct
preprocessor definitions.

The resulting `bin\Hdf5AuditHarness_d.exe` first reports the HDF5 runtime
version, thread-safety response, and containing module path. It then performs:

- Eight single-process workers against distinct temporary files.
- A watchdog-supervised child process with two concurrent writers to one
  dataset, a writer/readers overlap on one file, two sources copying to one
  target, and simultaneous `A -> B` / `B -> A` copies.
- Four child processes writing disjoint rows in one shared temporary file.

Each process requires an enabled, complete, non-overflowed audit queue;
successful opens must have a non-zero handle ID and a path-resolved diagnostic
identity, every close must succeed and match its open path/mode/identity, and
every checked matrix must reopen with its expected shape/type and contain one
complete writer value rather than a mixed row. The cross-process matrix must
contain the final value from each child row. A child that exceeds 120 seconds
is terminated and causes a harness failure; this detects a hang but does not
by itself prove the absence of every deadlock class.

The harness exits non-zero when diagnostics were omitted from the Hdf5IO build,
any HDF5 operation fails, event evidence is incomplete, a handle is unpaired,
a close fails, a watchdog expires, or an integrity check fails. It has been
built but not run in this audit phase. It does not yet cover cancellation
injection, atomic replacement racing, byte hashes,
full object/attribute inventories, or recovery after a forced process exit;
those remain required before lock narrowing.
