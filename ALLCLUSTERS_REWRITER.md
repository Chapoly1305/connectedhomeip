# Auto-enable everything in `all-clusters-app.zap`

`scripts/tools/zap/enable_all_clusters_in_zap.py` rewrites
`examples/all-clusters-app/all-clusters-common/all-clusters-app.zap` so
every server cluster known to this branch's ZAP catalog is enabled on a
single endpoint (EP0), with all attributes / commands / events turned on.
EP1..EP65534 are dropped. It is intended for fuzzing the host
all-clusters-app binary — embedded targets do not have the flash to fit
the result.

This document covers:
1. The data flow from cluster XML → `.zap` → `.matter` → C++ → linked binary.
2. Where the ground truth for clusters / attributes / commands / events lives.
3. How the script enables them, and what it deliberately omits.
4. How attribute default values are normalised between XML and IDL.
5. How to verify nothing was missed silently, and where each failure mode
   would surface.

---

## 1. The pipeline at a glance

```
src/app/zap-templates/zcl/zcl-with-test-extensions.json
        │  (lists which XMLs make up the catalog)
        ▼
src/app/zap-templates/zcl/data-model/{chip,test}/*.xml
        │  (per-cluster definitions: attributes, commands, events)
        ▼
[enable_all_clusters_in_zap.py]
        │  reads the XMLs, rebuilds EP0's cluster set in the .zap file
        ▼
examples/all-clusters-app/all-clusters-common/all-clusters-app.zap
        │  (one endpoint with every cluster enabled)
        ▼
[scripts/tools/zap/generate.py + ZAP source build]
        │  matter-idl-server.json template renders Clusters.matter
        ▼
examples/all-clusters-app/all-clusters-common/all-clusters-app.matter
        │  (typed IDL with only the enabled cluster shapes)
        ▼
[gn gen → ninja build, codegen at build time]
        │  src/app/zap_cluster_list.{json,py} maps cluster defines
        │  to source dirs under src/app/clusters/<dir>
        ▼
zzz_generated/.../IMClusterCommandHandler.cpp + per-cluster source_set
        │  static dispatch table to emberAf<Cluster><Cmd>Callback
        │  and Matter<Cluster>PluginServerInitCallback symbols
        ▼
ld → chip-all-clusters-app
```

Each arrow is a place where a misalignment between layers can fail. The
script only touches the first arrow; the rest of the pipeline is
unmodified, which is what makes the verification story tractable.

---

## 2. Ground truth: where each thing is defined

### 2.1 Cluster catalog index

`src/app/zap-templates/zcl/zcl-with-test-extensions.json`

This is the **package** that the `.zap` file references in its
`package[].path`. The .zap file specifies it explicitly:

```json
"package": [
  { "path": "../../../src/app/zap-templates/zcl/zcl-with-test-extensions.json",
    "type": "zcl-properties" }
]
```

Important: the all-clusters-app uses the **`-with-test-extensions`**
variant, not the plain `zcl.json`. The two differ in:
- An extra `xmlRoot` entry: `./data-model/test`.
- One additional XML: `mode-select-extensions.xml` (adds a manufacturer
  attribute and command to ModeSelect).

The script's `--zcl` flag defaults to `zcl-with-test-extensions.json`
**because that is what the input .zap declares as its package**. Using
`zcl.json` instead would produce a smaller catalog that disagrees with
what ZAP itself uses to render the `.matter` file.

The two relevant fields in the JSON:
- `xmlRoot` — list of directories searched for each XML file. Currently
  `[".", "./data-model/chip", "./data-model/test"]` (paths relative to
  the JSON itself).
- `xmlFile` — ordered list of XML filenames. The script walks every
  entry and finds it under the first matching `xmlRoot`.

### 2.2 Per-cluster XML

`src/app/zap-templates/zcl/data-model/chip/*.xml`
`src/app/zap-templates/zcl/data-model/test/*.xml`

There are **141 cluster XMLs** in v1.5 (142 entries total in
`xmlFile` — one is `clusters-extensions.xml` which only carries
`<clusterExtension>` elements, no top-level `<cluster>`).

Each XML is the **single source of truth for one cluster's surface**:

```xml
<configurator>
  <cluster>
    <name>Access Control</name>
    <code>0x001F</code>
    <define>ACCESS_CONTROL_CLUSTER</define>

    <attribute side="server" code="0x0000" name="ACL"
               type="array" entryType="AccessControlEntryStruct" .../>
    <attribute side="server" code="0x0002"
               name="SubjectsPerAccessControlEntry" type="int16u" .../>

    <command code="0x00" source="client" name="ReviewFabricRestrictions" .../>
    <command code="0x01" source="server" name="ReviewFabricRestrictionsResponse" .../>

    <event side="server" code="0x0000" name="AccessControlEntryChanged"
           priority="info" isFabricSensitive="true">
      <description>...</description>
      <field id="1" name="AdminNodeID" type="node_id" isNullable="true"/>
    </event>
  </cluster>
</configurator>
```

Notes about the schema:
- The `<code>` element on `<cluster>` is the cluster ID. Hex literals
  (`0x001F`) are allowed.
- `<attribute>`, `<command>`, `<event>` carry their codes as **XML
  attributes** (`code="0x0000"`), not child elements.
- Each attribute / command / event has an explicit `side` (`"server"`
  or `"client"`).
- A `<configurator>` may also contain `<clusterExtension code="0xXXXX">`
  blocks — these layer additional attributes/commands/events onto a
  cluster defined elsewhere. `clusters-extensions.xml` and
  `mode-select-extensions.xml` use this.
- Manufacturer-specific cluster IDs and element IDs use a 32-bit code
  with the upper 16 bits set to a vendor ID (e.g., `0xFFF1FC05` =
  vendor `0xFFF1` + id `0xFC05`). The script treats any **element**
  (attribute / command / event) code `> 0xFFFF` as mfg-specific and
  skips it. Mfg-specific *clusters* (whole-cluster code `> 0xFFFF`,
  e.g. `Fault Injection 0xFFF1FC06`) are still included if their
  callbacks exist.

### 2.3 Server-side C++ implementation map

`src/app/zap_cluster_list.json`

Maps each `<define>` (e.g., `ACCESS_CONTROL_CLUSTER`) to a list of
directories under `src/app/clusters/`. `chip_data_model.gni` walks this
map, adds `src/app/clusters/<dir>` as a build dep for each enabled
cluster, and imports each dir's `app_config_dependent_sources.gni` to
pull in extra source files.

There are two valid shapes for the value list:
- `[]` — no per-cluster sources are needed.
- `["dirname"]` or `["dirname", "another"]` — one or more source
  directories under `src/app/clusters/`.

A cluster define that is missing from this JSON entirely **breaks
`gn gen`** with: `Unhandled server cluster: XXX_CLUSTER (hint: add to
src/app/zap_cluster_list.json)`.

### 2.4 Ember command callback contract

For every cluster command enabled in the .zap file, the ZAP-generated
`zzz_generated/.../IMClusterCommandHandler.cpp` emits a static dispatch
that calls:

```cpp
emberAf<ClusterName>Cluster<CommandName>Callback(
    chip::app::CommandHandler*,
    const chip::app::ConcreteCommandPath&,
    const Commands::<CommandName>::DecodableType&);
```

Each cluster's server source dir (or the all-clusters-app glue) must
**provide a definition for every such callback**. If the server impl
is the new "server-cluster" style (a `source_set` in BUILD.gn that
returns its own `ServerClusterInterface`), no ember callbacks are
generated for that cluster — instead the dispatch goes through the
`ServerClusterInterfaceRegistry`. Mixing models within one cluster is
where most missing-callback link errors come from: the XML enables a
new command, the dispatch table grows, but the ember-style server impl
never adds a matching callback.

Similarly, every cluster gets a one-time
`Matter<ClusterName>PluginServerInitCallback()` on startup and per
endpoint `Matter<ClusterName>ClusterInit(EndpointId)` /
`...ClusterShutdown(EndpointId)`. These have weak default
implementations in `src/app/util/util.cpp` for legacy clusters but **not
for new server-cluster-style clusters** — those must define the symbol
(typically via the cluster's CodegenIntegration.cpp).

### 2.5 What `.zap` actually stores per cluster

The minimum shape an `endpointTypes[i].clusters[j]` entry needs (verified
empirically against the input file before rewriting):

```jsonc
{
  "name": "<human name>",
  "code": <int>,             // cluster id
  "mfgCode": null,
  "define": "<UPPER_SNAKE>",  // matches zap_cluster_list.json key
  "side": "server",
  "enabled": 1,
  "attributes": [ {
      "name": "...", "code": <int>, "mfgCode": null, "side": "server",
      "type": "<zcl type>",
      "included": 1,
      "storageOption": "External",  // see §3.4
      "singleton": 0, "bounded": 0,
      "defaultValue": <string|null>,  // see §4
      "reportable": 1, "minInterval": 1, "maxInterval": 65534,
      "reportableChange": 0
  } ],
  "commands":  [ {
      "name": "...", "code": <int>, "mfgCode": null,
      "source": "client" | "server",
      "isIncoming": 0|1,            // see §3.3
      "isEnabled": 1
  } ],
  "events":    [ {
      "name": "...", "code": <int>, "mfgCode": null,
      "side": "server", "included": 1
  } ]
}
```

These fields and defaults were chosen by reading what ZAP's GUI emits
for the existing all-clusters-app entries — not from documentation.

---

## 3. How the script enables everything

### 3.1 Single endpoint

The Matter spec splits clusters into "root-node-only" (must live on
endpoint 0) and "anywhere else", but the SDK enforces only the
"must be on EP0" half via build-time `static_assert` (Groupcast,
BasicInformation, GeneralCommissioning, AdministratorCommissioning).
A grep across `src/app/clusters/` finds **zero** asserts in the
opposite direction. So the simplest possible layout — one endpoint
with everything on it — is buildable, and each cluster is reachable
through one endpoint id, simplifying fuzz reproducers.

The script does:
- **EP0**: keeps the existing `endpointTypes[0]` metadata
  (`deviceTypeRef`, `deviceTypes`, `deviceVersions`, `deviceIdentifiers`,
  etc.) so commissioning-relevant fields stay correct. Existing
  *client*-side clusters on EP0 (the OTA Software Update Provider
  binding) are preserved verbatim. Existing *server* clusters are
  discarded and replaced with fresh fully-enabled entries from the
  catalog.
- **EP1..EP65534**: dropped entirely. The original file's secondary
  light, generic switches, and anonymous network-interface endpoints
  add no fuzz coverage that EP0's full catalog doesn't already give.
- `endpoints[]` is rewritten to a single entry: `endpointId: 0`,
  `endpointTypeIndex: 0`.

### 3.2 Skip lists

`DEFAULT_SKIP` records clusters that the build can't currently coexist
with (each entry has a one-line reason). Iteration loop: build, read
the failure, add the cluster code with a comment, re-run. Currently:

| Code | Cluster | Reason |
| ---- | ------- | ------ |
| 0x0039 | Bridged Device Basic Information | missing `KeepActiveCallback` |
| 0x0047 | Timer | no impl dir on this branch |
| 0x0065 | Groupcast | upstream `gServer.Create()` lacks required `BitFlags<Feature>` arg |
| 0x0079 | Water Tank Level Monitoring | not in `zap_cluster_list.json` |
| 0x050F | Content Control | missing several block-window callbacks |
| 0x0551 | Camera AV Stream Management | missing PluginServerInit / ClusterInit / ClusterShutdown |
| 0x0554 | WebRTC Transport Requestor | missing PluginServerInit |
| 0x0752 | Joint Fabric Datastore | missing many callbacks |
| 0xFFF1FC05 | Unit Testing | many test command callbacks missing in app glue |

`SKIP_COMMANDS_BY_CLUSTER` is the per-command escape hatch when only a
handful of commands lack callbacks and dropping the whole cluster would
lose too much coverage:

| Cluster | Command | Reason |
| ------- | ------- | ------ |
| 0x0008 LevelControl | 0x08 MoveToClosestFrequency | FQ feature, no callback in host glue |
| 0x0046 ICDManagement | 0x00 RegisterClient | callback missing |
| 0x0046 ICDManagement | 0x02 UnregisterClient | callback missing |

### 3.3 Reading XML, writing JSON

`collect_cluster_catalog()` runs in two passes:

1. **First pass — clusters.** For each `<cluster>` element, take
   `<code>`, `<name>`, `<define>` to seed a `ClusterDef`, then
   `merge_into(side="server", elem)` consumes every `<attribute>`,
   `<command>`, `<event>` child. Keys in the per-cluster dicts are
   `(side, code)` so a later extension cannot accidentally collide
   with an existing element.
2. **Second pass — extensions.** Re-walks the same files looking for
   `<clusterExtension code="0xXXXX">`. The matching `ClusterDef` is
   extended in place. A `<clusterExtension>` whose target cluster is
   not in the catalog logs `WARN:` and is skipped (rather than
   crashing) — this is how a stale `clusters-extensions.xml` would be
   flagged.

Several details matter:

- **Side defaulting.** XML elements without an explicit `side` attrib
  default to `"server"`. The xml-to-zap conversion functions encode
  this in a single place so the JSON always has `side` set.
- **`isIncoming`.** On a server cluster, an XML
  `<command source="client">` is incoming (the server receives it) and
  `source="server"` is outgoing (a response). `cmd_xml_to_zap` derives
  this from the XML `source` attribute and the cluster side.
- **Manufacturer-specific filtering.** Inside `merge_into`,
  `is_mfg_specific(code) := code > 0xFFFF` rejects vendor-prefixed
  attribute / command / event ids before they enter the catalog. This
  exists because the codegen for these requires the `.zap` to set
  `mfgCode = 0xFFF1` (or whatever vendor) on each entry, and the C++
  cluster-objects header doesn't declare a
  `Commands::FooMfg::DecodableType` struct for them — the generated
  dispatch table fails to compile. The mode-select
  `SampleMfgExtensionCommand` is the canonical example.

### 3.4 Why `storageOption: "External"`

Each attribute in the .zap declares a storage class:
- `"RAM"` — value lives in static RAM, ember manages it.
- `"NVM"` — value lives in non-volatile storage, ember manages it.
- `"External"` — application provides read/write callbacks; ember calls
  them.

`"External"` is the safest blanket choice for fuzzing because:
- No static RAM cost is added per attribute.
- The application's `ExternalAttributeAccessInterface` (or default
  weak implementations) handle every attribute access, returning a
  zero-initialised value if no specific impl is registered.
- Many attributes (arrays, structs, lists) are too large or too
  variable-shaped for `"RAM"` storage and would fail to compile if not
  marked external.

Sticking to one option keeps the script's output deterministic across
input shapes.

### 3.5 Reporting parameters

`reportable: 1`, `minInterval: 1`, `maxInterval: 65534`,
`reportableChange: 0`. These match the values the ZAP GUI emits when
you tick "reportable" with no other tweaks. They make every attribute
subscribable (which expands attribute-read code paths reachable by
fuzz inputs) without requiring per-attribute thought.

### 3.6 What the script does NOT do

- It does not enumerate or enable client-side clusters from the
  catalog. The only client-side entry on EP0 is whatever was already
  in the input `.zap` (typically OTA Software Update Provider).
- It does not touch `keyValuePairs`, `package`, `creator`, `fileFormat`,
  or `featureLevel` on the .zap. Those are passed through unchanged.
- It does not edit `zap_cluster_list.json` or any C++ glue. When a
  build failure points at one of those, the failure is recorded in
  `DEFAULT_SKIP` / `SKIP_COMMANDS_BY_CLUSTER` with a one-line reason,
  and the user decides whether to fix the upstream gap or accept the
  loss.

---

## 4. Default values: how XML defaults map to IDL defaults

Attribute defaults are the one place where the XML, the `.zap`, the
matter-idl grammar, and the C++ codegen all see the data slightly
differently. The script reconciles them in a single function,
`safe_default_value()`.

### 4.1 What the XML carries

Across all 142 cluster XMLs in v1.5, **368 attributes** declare a
`default=` in their XML element. The shapes that appear (audited
mechanically):

| Shape | Count | Example values |
| ----- | ----- | -------------- |
| Decimal integer (signed) | 222 | `0`, `1`, `-5`, `100` |
| Hex integer | 141 | `0x00`, `0x0019`, `0x00000000` |
| Boolean | 4 | `true`, `false` |
| Symbolic enum name | 1 | `Unknown` |

The grammar at `scripts/py_matter_idl/matter/idl/matter_grammar.lark:97` is:

```
default_value: "default" "=" (integer | ESCAPED_STRING | bool_default)
integer:        positive_integer | "-" positive_integer
positive_integer: /\d+/ | /0x[0-9a-fA-F]+/
bool_default:   "true" | "false"
ESCAPED_STRING                                            // quoted string
```

So the parser will accept any decimal/hex integer (signed),
`true`/`false`, or a quoted string — but **not** an unquoted symbolic
identifier like `Unknown`.

### 4.2 What the renderer template does

`src/app/zap-templates/templates/app/MatterIDL_Server.zapt:46` is the
Handlebars template that turns each `.zap` attribute into one line of
the `.matter` IDL:

```
default = {{#if (isString type)}}"{{defaultValue}}"{{else}}{{defaultValue}}{{/if}}
```

The renderer only auto-adds quotes for *string-typed* attributes
(`char_string`, etc.). For an `int8u`, `enum8`, or `boolean` attribute,
it emits `defaultValue` **bare**. So the script gets to choose:
- Store `"defaultValue": "0"` → renderer emits `default = 0` →
  parser reads as `integer`, codegen uses 0 as a real numeric default.
- Store `"defaultValue": "Unknown"` → renderer emits
  `default = Unknown` → parser **fails** with `UnexpectedCharacters`.
- Store `"defaultValue": "\"Unknown\""` (literal quote chars in the
  string) → renderer emits `default = "Unknown"` → parser reads as
  `ESCAPED_STRING`. The C++ codegen sees a string default for a
  non-string type and emits `ZAP_EMPTY_DEFAULT()` — same as if the
  default was nulled — but the original spec intent is preserved in
  the `.matter` as documentation.

### 4.3 What `safe_default_value()` does

```python
_INTEGER_RE = re.compile(r"-?(\d+|0x[0-9a-fA-F]+)")

def safe_default_value(raw):
    if raw is None: return None
    s = raw.strip()
    if s == "":                          return ""
    if s.lower() in ("true", "false"):   return s.lower()
    if _INTEGER_RE.fullmatch(s):         return s
    return f'"{s}"'                       # quote-on-fail
```

Mapping for the four shapes seen in the corpus:

| XML default | Stored in .zap | Rendered .matter line | C++ effect |
| ----------- | -------------- | --------------------- | ---------- |
| `0` (int) | `"0"` | `default = 0` | numeric default 0 |
| `0x0019` (hex) | `"0x0019"` | `default = 0x0019` | numeric default 25 |
| `true` (bool) | `"true"` | `default = true` | bool default true |
| `Unknown` (enum) | `"\"Unknown\""` | `default = "Unknown"` | `ZAP_EMPTY_DEFAULT()` |

The integer regex matches the grammar exactly (`-?(\d+|0x[0-9a-fA-F]+)`),
so anything that *would* be a valid integer literal stays bare; anything
else gets quoted. The fallout is documented loss-of-information for
symbolic enum defaults — the C++ falls back to zero-init, which for
`UpdateState=Unknown` happens to be the same value the spec intends
(`Unknown == 0`). For other future symbolic defaults, the same fallback
applies; the `.matter` carries the original intent for human readers.

### 4.4 Why this is mostly decorative anyway

`storageOption: "External"` short-circuits the static default at runtime.
The C++ static cluster table's default is read only when no
`ExternalAttributeAccessInterface` is registered for that attribute;
the all-clusters-app does register one (or relies on the weak default
that returns zero), so attribute reads route through callbacks and the
default in the static table is observable only briefly during init.
For fuzz coverage the value of the default doesn't matter at all — the
fuzzer mutates inputs anyway.

---

## 5. Verification: did we actually enable everything?

The strongest answer is operational, not declarative: each layer of the
pipeline reports what it sees, and the layers check each other.

### 5.1 Script self-report

Running the script prints a summary:

```
loaded 142 XML files from zcl-with-test-extensions.json
catalog: 142 clusters
EP0 rebuilt: 129 server clusters + 1 client clusters preserved
  user --skip: 9 -> ['0x0039 Bridged Device Basic Information',
                     '0x0047 Timer',
                     '0x0065 Groupcast',
                     '0x0079 Water Tank Level Monitoring',
                     '0x050f Content Control',
                     '0x0551 Camera AV Stream Management',
                     '0x0554 WebRTC Transport Requestor',
                     '0x0752 Joint Fabric Datastore',
                     '0xfff1fc05 Unit Testing']
  empty/extension-only excluded: 4 -> ['0x001c Pulse Width Modulation',
                                       '0x0042 Proxy Configuration',
                                       '0x0043 Proxy Discovery',
                                       '0x0044 Proxy Valid']
```

The arithmetic is `EP0 server count + user-skip + empty == catalog size`.
Any cluster that disappeared silently would break this identity, which
is why each excluded bucket is listed by code and name.

The script also logs `WARN:` for:
- An xmlFile referenced by `zcl.json` but not found in any `xmlRoot`
  (e.g., a deleted XML).
- A `<clusterExtension code="...">` whose target cluster is not in the
  catalog.

These are normally silent; if they fire, the catalog is incomplete.

### 5.2 Cross-check the .zap with ZAP itself

`scripts/tools/zap/generate.py <path>` runs the real ZAP CLI against
the rewritten file. If the .zap is malformed, ZAP rejects it with a
JSON schema or feature-level error before any C++ codegen runs. If a
referenced cluster id, attribute id, command id, or event id does not
exist in the catalog ZAP loads, ZAP errors out with a missing-symbol
message. Successful generation produces:

- `examples/all-clusters-app/all-clusters-common/all-clusters-app.matter`
  — refreshed IDL.

ZAP also emits structural warnings that you can grep for, even on
success. Example:

```
Application has missing response commands for enabled commands as follows:
  - On endpoint 0, cluster: OTA Software Update Provider client,
    incoming command: QueryImageResponse should be enabled as it is
    the response to the enabled outgoing command: QueryImage.
```

A *new* warning after a script change should be investigated; existing
ones for preserved client clusters can be ignored.

### 5.3 Cross-check `.matter` against the .zap

The `.matter` file is a typed IDL — every cluster, attribute, command,
event mentioned in it must be in the .zap. After running the script:

```
git diff --stat examples/all-clusters-app/all-clusters-common/
```

should show the `.zap` and `.matter` both grew by tens of thousands of
lines together. If only one of them grew, ZAP did not actually
regenerate (likely cause: forgot to set `ZAP_DEVELOPMENT_PATH` or the
ZAP source build is stale).

A coarser check — count clusters per endpoint:

```python
import json
z = json.load(open('examples/all-clusters-app/all-clusters-common/all-clusters-app.zap'))
for et in z['endpointTypes']:
    s = sum(1 for c in et['clusters'] if c['side']=='server')
    print(et['name'], 'server clusters:', s)
```

This should match the script's `EP0 rebuilt: N server clusters` line.

### 5.4 Cross-check at build time

Six failure modes are caught here, and the diagnostics tell you which:

1. **Cluster define not in `zap_cluster_list.json`.**
   `gn gen` raises:
   `Unhandled server cluster: XXX_CLUSTER (hint: add to src/app/zap_cluster_list.json)`.
   Either add the cluster to the JSON (with the right
   `src/app/clusters/<dir>` value) or add the cluster code to the
   script's `DEFAULT_SKIP`.

2. **Cluster mapped to a non-existent dir.**
   `gn gen` raises:
   `Unable to load ".../src/app/clusters/<dir>/app_config_dependent_sources.gni"`.
   The fix is one of: correct the JSON path, create the missing dir, or
   skip the cluster.

3. **`.gni` exists but does not declare the variable.**
   `gn gen` raises:
   `No value named "app_config_dependent_sources" in scope "cluster_entry"`.
   The cluster's `app_config_dependent_sources.gni` is missing its
   `app_config_dependent_sources = [...]` line. Add the declaration
   (`= []` if the source_set in BUILD.gn supplies all sources).

4. **Static asserts about endpoint placement.**
   These compile-time errors carry an explicit message:
   `Groupcast cluster MUST be on endpoint 0`. With the single-endpoint
   layout these are normally satisfied automatically (everything is on
   EP0), but a cluster's CodegenIntegration could still fail in the
   constructor (e.g., Groupcast itself: `gServer.Create()` requires a
   `BitFlags<Feature>` arg the integration doesn't pass) — those land
   in `DEFAULT_SKIP`.

5. **Codegen produces references to nonexistent cluster-objects.**
   Compilation errors like:
   `no member named 'SampleMfgExtensionCommand' in namespace
   'chip::app::Clusters::ModeSelect::Commands'`.
   The cluster-objects header (rendered from the same .matter file)
   doesn't declare a struct for the command. This is the symptom that
   triggered the `is_mfg_specific(code > 0xFFFF)` filter. If a
   non-mfg-specific command triggers this, something deeper is wrong.

6. **Linker missing-symbol errors.**
   `ld64.lld: error: undefined symbol: emberAfXxxClusterYyyCallback(...)`
   means a command was enabled that has no matching server-side
   callback. Two responses:
   - Skip the command (`SKIP_COMMANDS_BY_CLUSTER[code] |= {0x...}`).
   - Skip the cluster (`DEFAULT_SKIP |= {0x...}`).

   For `MatterXxxPluginServerInitCallback` / `ClusterInitCallback` /
   `ClusterShutdownCallback`, the cluster lacks per-endpoint lifecycle
   hooks; in practice this means the cluster needs its
   server-cluster-style glue or else it should be skipped.

The build's exit code is non-zero on any of these, so a green build is
the strongest single signal that the rewriter's output is consistent
with the rest of the SDK.

### 5.5 Counting at the binary level

After a successful build, you can read back what the binary actually
declares:

```bash
# Number of cluster instances in the EmberAf static cluster list.
strings out/.../chip-all-clusters-app | grep -E 'CLUSTER$' | sort -u | wc -l

# Per-cluster command callbacks present in the binary:
nm out/.../chip-all-clusters-app | grep -E ' T emberAf.+Cluster.+Callback' | wc -l
```

For a fuzz harness the second number is the most directly useful — it
is a lower bound on the number of distinct command-dispatch entry
points the fuzzer can reach.

### 5.6 What you cannot detect this way

Some omissions are silent and require inspection:

- **Optional commands / attributes / events that the cluster XML does
  not declare at all.** The catalog only knows what the XML knows. If
  the spec defines an attribute that nobody wrote into the XML, the
  script cannot enable it. (The two sources are not synchronised:
  `data_model/1.5/clusters/*.xml` is a separate spec PDF extract used
  by other tooling, not the ZAP catalog.)
- **Behavioural correctness of the enabled callbacks.** The build
  proves a callback is *linked*, not that it does anything sensible.
  Most callbacks in the all-clusters-app are stub responders; that is
  fine for fuzz coverage, but do not assume a green binary implements
  correct cluster semantics.
- **Endpoint composition rules.** The Matter spec restricts which
  device types may declare which clusters. Putting Light Sensor on the
  same endpoint as Microwave Oven Control is odd, but as long as no
  static_assert says otherwise, the build will succeed. The fuzzer
  doesn't care; a conformance test would.

---

## 6. Reproducing on v1.4 / v1.6

The script is not pinned to v1.5. It only depends on:
- The `.zap` file having a parseable `endpointTypes[0]` to take metadata
  and client clusters from.
- The `zcl.json` (or `-with-test-extensions.json`) being a valid
  package descriptor that points at a directory of cluster XMLs.

Both v1.4 and v1.6 carry the same descriptor + per-cluster XML layout.
What differs across branches:
- The set of cluster XMLs (new clusters added, old ones removed).
- The set of clusters in `zap_cluster_list.json` and which dirs exist.
- The set of clusters whose CodegenIntegration is incomplete.
- The set of clusters whose ember callbacks are missing.

So the workflow is:

```bash
git checkout v1.6-branch     # or v1.4-branch
python3 scripts/tools/zap/enable_all_clusters_in_zap.py
python3 scripts/tools/zap/generate.py \
    examples/all-clusters-app/all-clusters-common/all-clusters-app.zap
./scripts/build/build_examples.py \
    --target darwin-arm64-all-clusters-clang-no-ble-no-wifi-no-thread build
```

Iterate `DEFAULT_SKIP` and `SKIP_COMMANDS_BY_CLUSTER` based on the build
output until the link succeeds. Each entry should record *why* it is on
the list (typo, no impl dir, missing callback) so the next branch
upgrade can revisit.

---

## 7. File reference

| Path | Role |
| ---- | ---- |
| `scripts/tools/zap/enable_all_clusters_in_zap.py` | The rewriter. |
| `examples/all-clusters-app/all-clusters-common/all-clusters-app.zap` | Output target. |
| `examples/all-clusters-app/all-clusters-common/all-clusters-app.matter` | Regenerated by `generate.py`. |
| `src/app/zap-templates/zcl/zcl-with-test-extensions.json` | Catalog index. |
| `src/app/zap-templates/zcl/data-model/chip/*.xml` | Cluster definitions. |
| `src/app/zap-templates/zcl/data-model/test/*.xml` | Test-only extensions. |
| `src/app/zap-templates/templates/app/MatterIDL_Server.zapt` | Renders `.zap` → `.matter` (line 46 controls default-quoting). |
| `scripts/py_matter_idl/matter/idl/matter_grammar.lark` | IDL grammar (line 97 = default value rule). |
| `src/app/zap_cluster_list.json` | Cluster define → source dir map. |
| `src/app/chip_data_model.gni` | GN code that consumes the .zap and the cluster list. |
| `src/app/clusters/<name>/app_config_dependent_sources.gni` | Per-cluster source list pulled into the chip_data_model target. |
| `scripts/tools/zap/zap_download.py` | Bootstrap ZAP source / binaries. |
| `scripts/tools/zap/zap_bootstrap.sh -c` | Build the ZAP SPA + native deps. |
| `scripts/tools/zap/generate.py` | Run ZAP against a `.zap` file to refresh `.matter`. |

---

## 8. Summary

- **Ground truth**: `zcl-with-test-extensions.json` enumerates the XML
  files; each XML is the source of truth for one cluster's surface.
- **Layout**: single endpoint (EP0). Existing client clusters preserved;
  every catalog server cluster rebuilt fresh with full enablement.
  No build-time assert prohibits clusters from EP0, so this is the
  simplest layout that covers the catalog.
- **Default values**: integer/bool literals stored bare so they render
  as IDL literals; symbolic values (e.g., enum names) wrapped in quote
  characters so they render as IDL `ESCAPED_STRING` and the C++ codegen
  silently falls back to `ZAP_EMPTY_DEFAULT()` while the `.matter`
  preserves the original intent.
- **Coverage check**: the script prints catalog size, EP0 cluster count,
  and every excluded code by category. Their sum equals the catalog size.
- **Failure detection**: ZAP rejects malformed .zap files; `gn gen`
  rejects missing cluster-list mappings or .gni declarations;
  C++ compilation rejects missing struct shapes; the linker rejects
  missing callbacks and init hooks. A successful build is therefore a
  strong end-to-end check that every enabled element has a matching
  implementation; a failed build's first error tells you which layer
  disagrees.
