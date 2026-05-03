# All-Clusters-App Auto-Enable Experiment

## Goal

Make `examples/all-clusters-app` actually enable **every** cluster /
attribute / event / command that this branch's ZAP catalog knows about,
without hand-toggling each one in the ZAP GUI. The `.zap` file should be
regenerated from a script so the same approach works on v1.4 / v1.5 / v1.6.

Endpoint layout target:
- **EP0 (root)**: only the clusters that are spec-mandated to live on the
  root node.
- **EP1**: every other cluster, server side, with all attributes/commands/
  events enabled.

This is for fuzz coverage — we want maximum SDK code reachable in the
target binary.

## Why a new workspace

This is an experimental rewrite of `all-clusters-app.zap`. Doing it in
`/Volumes/Workspace/matterFuzzerCode-portable-flatten-c3b398f` would mix
generated-code churn with the fuzzer's portability work. This worktree is
branched off clean upstream `v1.5-branch` so the diff is auditable and
the result can be cherry-picked back when it builds.

## Workspace setup (already done)

- Worktree path: `/Volumes/Workspace/matter-allclusters-experiment/`
- Branch: `allclusters-experiment` (from `v1.5-branch`, HEAD `40c1eeb909`)
- Created with: `git -C /Volumes/Workspace/connectedhomeip-v1.5-branch worktree add -b allclusters-experiment /Volumes/Workspace/matter-allclusters-experiment v1.5-branch`
- Submodules: **not yet initialized** — first thing the new session must do.

## Where things live (verified in the source workspace)

### Cluster catalog (per-branch source of truth for ZAP)
- `src/app/zap-templates/zcl/zcl.json` — index of XMLs to load.
- `src/app/zap-templates/zcl/data-model/chip/*-cluster.xml` — 141 XML
  files in v1.5; each defines one cluster's attributes / commands / events
  with full metadata. These XMLs are what ZAP actually reads. On 1.4 / 1.6
  the file set differs — that's why the script must walk `zcl.json`
  rather than hardcoding cluster lists.
- `data_model/1.5/clusters/*.xml` is a **different** thing — it's the spec
  PDF/XML extract used by the fuzzer's `fsm-build` skill. Do not confuse
  the two.

### Target file to rewrite
- `examples/all-clusters-app/all-clusters-common/all-clusters-app.zap`
  (~56k lines, JSON). Current state in v1.5:
  - 6 endpoint types: EP0 MA-rootdevice (97 clusters), EP1 MA-onofflight
    (130), EP2 MA-onofflight (7), EP3/4 MA-genericswitch (3 each),
    EP65534 anonymous.
  - Each cluster has `enabled`, each attribute `included`, each command
    `isEnabled`. Top-level keys to preserve: `fileFormat`, `featureLevel`,
    `keyValuePairs`, `package`.

### ZAP toolchain
- `scripts/setup/zap.version` — pinned `v2025.10.23-nightly`.
- `scripts/tools/zap/zap_download.py` — downloads a release zip or clones
  the source repo. On arm64 macOS, **must** use `--zap SOURCE` (no arm64
  release artifact).
- `scripts/tools/zap/zap_bootstrap.sh` — `npm ci`, version stamp, build
  SPA. Required after a SOURCE download.
- `scripts/tools/zap/generate.py` — runs ZAP on a `.zap` file to refresh
  the `.matter` IDL and per-app codegen headers.
- `scripts/tools/zap/run_zaptool.sh` — launches the ZAP GUI for spot
  inspection (optional).

## Plan

### 1. Init submodules
```bash
cd /Volumes/Workspace/matter-allclusters-experiment
git submodule update --init --recursive
```
Long-running. The previous session was killed mid-init; safe to retry.

### 2. Bootstrap Pigweed + ZAP source
```bash
cd /Volumes/Workspace/matter-allclusters-experiment
source scripts/activate.sh
eval $(python3 scripts/tools/zap/zap_download.py --zap SOURCE)
./scripts/tools/zap/zap_bootstrap.sh -c
```
`--zap SOURCE` clones `https://github.com/project-chip/zap.git` at the
pinned tag into `.zap/zap-<version>-src/` and exports
`ZAP_DEVELOPMENT_PATH`. The bootstrap step does `npm ci`, version
stamping, canvas rebuild, SPA build.

### 3. Write the rewriter
Path: `scripts/tools/zap/enable_all_clusters_in_zap.py`

Inputs:
- `--zap` path to the `.zap` file (default: `examples/all-clusters-app/all-clusters-common/all-clusters-app.zap`).
- `--zcl` path to `zcl.json` (default: `src/app/zap-templates/zcl/zcl.json`).
- `--ep0-only` optional path to a JSON / text override of the EP0 cluster id allowlist.
- `--skip` optional denylist of cluster ids that fail to coexist (start empty, grow as build surfaces conflicts).

Behavior:
1. Parse `zcl.json` → for each `xmlFile` entry, parse the XML in
   `xmlRoot[1]` (`./data-model/chip`). Build a catalog: cluster id, name,
   server/client side, attributes (code, name, type, default, side),
   commands (code, name, source, isIncoming), events (code, name).
2. Skip clusters in `--skip`.
3. Hard-coded **EP0 allowlist** (top-of-file constant, edit-friendly):
   - 0x001D Descriptor
   - 0x001E Binding (debatable; revisit)
   - 0x001F AccessControl
   - 0x0028 BasicInformation
   - 0x0029 OtaSoftwareUpdateProvider
   - 0x002A OtaSoftwareUpdateRequestor
   - 0x002B LocalizationConfiguration
   - 0x002C TimeFormatLocalization
   - 0x002D UnitLocalization
   - 0x002E PowerSourceConfiguration
   - 0x0030 GeneralCommissioning
   - 0x0031 NetworkCommissioning
   - 0x0032 DiagnosticLogs
   - 0x0033 GeneralDiagnostics
   - 0x0034 SoftwareDiagnostics
   - 0x0035 ThreadNetworkDiagnostics
   - 0x0036 WifiNetworkDiagnostics
   - 0x0037 EthernetNetworkDiagnostics
   - 0x0038 TimeSynchronization
   - 0x003C AdministratorCommissioning
   - 0x003E OperationalCredentials
   - 0x003F GroupKeyManagement
   - 0x0046 IcdManagement
   - 0x0062 ProductAppearance? — verify per spec
   (Verify and tighten against `data_model/1.5/device_types/MA-rootnode.xml`
   in the fuzzer workspace if uncertain.)
4. Rewrite the `.zap` JSON:
   - Preserve `fileFormat`, `featureLevel`, `creator`, `keyValuePairs`, `package`.
   - Replace `endpointTypes` with two entries:
     - **EP0 type** (`name`: `MA-rootdevice`, deviceType `MA-rootdevice`
       code `0x0016`): one cluster entry per EP0-allowlisted cluster
       (server side), all attrs/cmds/events enabled.
     - **EP1 type** (`name`: `MA-onofflight` or generic — pick a device
       type that doesn't constrain conformance; `MA-onofflight` 0x0100 is
       a safe default since the existing file uses it): one cluster entry
       per non-EP0 cluster server side, plus globals.
   - `endpoints[]`: just EP0 (id 0) and EP1 (id 1). Drop EP2–EP4 and the
     anonymous EP65534 unless build needs them — record the decision in
     the script comments.
5. Each cluster entry follows the existing file's conventions exactly:
   `enabled: 1`, attributes `included: 1`, `storageOption: "External"`,
   `singleton: 0`, `bounded: 0`, `defaultValue: null`, `reportable: 1`,
   `minInterval: 1`, `maxInterval: 65534`, `reportableChange: 0`.
   Commands `isIncoming: 1`/`isEnabled: 1` (server-incoming = client-source).
   Always include the six globals: `GeneratedCommandList` (0xFFF8),
   `AcceptedCommandList` (0xFFF9), `EventList` (0xFFFA), `AttributeList`
   (0xFFFB), `FeatureMap` (0xFFFC), `ClusterRevision` (0xFFFD).
6. Write back via `json.dump(..., indent=2)` with `sort_keys=False` to
   match ZAP's formatting; pass through `scripts/tools/zap/zapfile_formatter.py`
   if it exists and matches what ZAP writes itself.

### 4. Apply + regenerate
```bash
python3 scripts/tools/zap/enable_all_clusters_in_zap.py
python3 scripts/tools/zap/generate.py \
    examples/all-clusters-app/all-clusters-common/all-clusters-app.zap
```
This refreshes `all-clusters-app.matter` and the per-app generated
headers under `zzz_generated/`.

### 5. Build to verify
```bash
./scripts/build/build_examples.py \
    --target darwin-arm64-all-clusters-clang-no-ble-no-wifi-no-thread \
    build
```
Plain non-instrumented build — fastest feedback. Iterate the script's
`--skip` denylist on each compile failure (singletons, mutually
exclusive features, missing C++ glue). Document each skipped cluster in
the script header with a one-line reason.

### 6. (Later) Cherry-pick into the fuzzer workspace
Once the all-clusters-app builds clean here, apply the same rewriter
under `matterFuzzerCode-portable-flatten-c3b398f`, regen, and rebuild
with the ASAN+libFuzzer target. Per the fuzzer repo's CLAUDE.md, the
script and EP0 allowlist live in different buckets:
- Script (`enable_all_clusters_in_zap.py`) → portable runtime tooling.
- EP0 allowlist constant → adapter / matter-1.5 profile bucket.

## Caveats / known unknowns

- **Some clusters can't coexist** at codegen or build time (singletons,
  mutually exclusive features, conflicting C++ glue). Don't pre-guess —
  let the build tell us, then add to `--skip` with a comment.
- **Embedded targets won't fit.** This script is for the host fuzzer
  target only; don't propose this `.zap` for nrf/silabs/esp32 builds.
- **EP0 allowlist is best-effort.** Verify against the v1.5
  RootNode device type before declaring done. Some entries above (e.g.
  Descriptor, Binding) may belong on every endpoint, not just EP0.
- **Generated `.matter` diff will be huge.** That's expected and the
  whole point. Commit the `.zap`, the script, and the regenerated
  `.matter` together.
- **ZAP source build on arm64 macOS** depends on `canvas` (native build).
  If `npm rebuild canvas --update-binary` fails, Pigweed's pkg-config
  paths are usually the culprit — `source scripts/activate.sh` first.

## Resume hints for the next session

When you start fresh in this workspace, read this doc first, then:
1. `git status` — confirm you're on `allclusters-experiment`, clean tree.
2. `git submodule status | head` — if anything shows `-` prefix, run init.
3. Pick up at the first incomplete plan step.

Source workspace (for reference, not for editing):
`/Volumes/Workspace/matterFuzzerCode-portable-flatten-c3b398f/`

Clean upstream v1.5 (parent of this worktree):
`/Volumes/Workspace/connectedhomeip-v1.5-branch/`
