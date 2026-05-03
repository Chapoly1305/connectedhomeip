#!/usr/bin/env python3
"""Rewrite all-clusters-app.zap so every server cluster known to the
branch's ZAP catalog is enabled on a single endpoint (EP0), with all
attributes/commands/events turned on. EP1..EP65534 are dropped.

Single-endpoint design rationale: the SDK has build-time static_asserts
in the "MUST be on endpoint 0" direction (Groupcast, BasicInformation,
GeneralCommissioning, AdministratorCommissioning) but none in the
"MUST NOT be on endpoint 0" direction. Putting everything on EP0 is
buildable and simplifies fuzz reproducers — every cluster is reachable
through one endpoint id.

Existing client-side clusters on EP0 (e.g., the OTA Software Update
Provider binding) are preserved untouched. Every server cluster found
in the catalog is enabled fresh — any prior partial enablement is
overwritten.

This is for fuzz coverage; embedded targets won't fit the result.

Usage:
    python3 scripts/tools/zap/enable_all_clusters_in_zap.py
        [--zap PATH] [--zcl PATH] [--skip 0xCODE,0xCODE,...] [--dry-run]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

DEFAULT_ZAP = REPO_ROOT / "examples/all-clusters-app/all-clusters-common/all-clusters-app.zap"
DEFAULT_ZCL = REPO_ROOT / "src/app/zap-templates/zcl/zcl-with-test-extensions.json"

# Clusters the build can't currently coexist with the rest. Start empty and
# grow this on each compile failure with a one-line reason.
DEFAULT_SKIP: set[int] = {
    # No server impl in src/app/zap_cluster_list.json (and no ember-only
    # path either). gn gen: "Unhandled server cluster:
    # WATER_TANK_LEVEL_MONITORING_CLUSTER".
    0x0079,
    # Timer: zap_cluster_list.json points at src/app/clusters/timer-server/
    # which doesn't exist on this branch. No impl shipped.
    0x0047,
    # Bridged Device Basic Information: server impl missing
    # emberAf...KeepActiveCallback for the KeepActive command.
    0x0039,
    # Content Control: server impl missing several block/window callbacks
    # (Add/RemoveBlockChannels, Set/RemoveBlockContentTimeWindow, etc.).
    0x050F,
    # Joint Fabric Datastore: server impl missing many callbacks
    # (AddACLToNode, RemoveNode, AddBindingToEndpoint, etc.).
    0x0752,
    # Camera AV Stream Management: missing MatterCameraAvStreamManagement{
    # PluginServerInit, ClusterInit, ClusterShutdown}Callback.
    0x0551,
    # WebRTC Transport Requestor: missing
    # MatterWebRTCTransportRequestorPluginServerInitCallback.
    0x0554,
    # Unit Testing (mfg-specific test cluster, code 0xFFF1FC05): many test
    # commands lack callbacks in the all-clusters-app glue.
    0xFFF1FC05,
    # Groupcast: src/app/clusters/groupcast/CodegenIntegration.cpp calls
    # gServer.Create() with no args, but GroupcastCluster's only ctor takes
    # BitFlags<Feature>. Upstream bug — disable until fixed.
    0x0065,
}

# Per-cluster command codes to skip even if the cluster itself stays. Use
# this when only a handful of commands lack callbacks and skipping the
# whole cluster would lose too much fuzz coverage.
SKIP_COMMANDS_BY_CLUSTER: dict[int, set[int]] = {
    # Level Control: MoveToClosestFrequency (FQ feature) has no callback in
    # the host all-clusters-app — skip just that command.
    0x0008: {0x08},
    # ICD Management: RegisterClient / UnregisterClient ember callbacks are
    # not provided by the host glue. Other ICD commands work.
    0x0046: {0x00, 0x02},
}


def parse_code(s: str) -> int:
    return int(s, 0)


@dataclass
class ClusterDef:
    code: int
    name: str
    define: str
    # keyed by (side, code) so server/client extensions don't collide
    attributes: dict[tuple[str, int], dict] = field(default_factory=dict)
    commands: dict[tuple[str, int], dict] = field(default_factory=dict)
    events: dict[tuple[str, int], dict] = field(default_factory=dict)


def load_xml_files(zcl_json: Path) -> list[Path]:
    with zcl_json.open() as f:
        data = json.load(f)
    roots = [(zcl_json.parent / r).resolve() for r in data["xmlRoot"]]
    files: list[Path] = []
    for name in data["xmlFile"]:
        for root in roots:
            p = root / name
            if p.exists():
                files.append(p)
                break
        else:
            print(f"WARN: xml file not found in any xmlRoot: {name}", file=sys.stderr)
    return files


_INTEGER_RE = re.compile(r"-?(\d+|0x[0-9a-fA-F]+)")


def safe_default_value(raw: str | None) -> str | None:
    """Map an XML default into something MatterIDL_Server.zapt + the matter-idl
    parser can both consume.

    The IDL grammar (matter_grammar.lark:97) accepts:
        default = (integer | ESCAPED_STRING | bool_default)
    where integer is `-?\\d+` or `-?0x[0-9a-f]+`, bool_default is true/false,
    and ESCAPED_STRING is a quoted string.

    The renderer template (MatterIDL_Server.zapt:46) only adds quotes for
    string-typed attributes. For a non-string-typed attribute (int, enum,
    bool) the renderer emits `defaultValue` bare. So:
      * integer/bool literals must be stored bare so the renderer emits
        them bare and the parser reads them as `integer`/`bool_default`.
      * Symbolic values (e.g., "Unknown") would render unquoted and break
        the parser. Wrapping them in literal quote chars makes the renderer
        emit `default = "Unknown"`, which the parser accepts as
        ESCAPED_STRING. The C++ codegen sees a string default for a
        non-string type and emits ZAP_EMPTY_DEFAULT() — same as nulling —
        but the original spec intent stays in the .matter as documentation.
    """
    if raw is None:
        return None
    s = raw.strip()
    if s == "":
        return ""
    if s.lower() in ("true", "false"):
        return s.lower()
    if _INTEGER_RE.fullmatch(s):
        return s
    return f'"{s}"'


def attr_xml_to_zap(a: ET.Element) -> dict:
    return {
        "name": a.attrib.get("name") or (a.text or "").strip(),
        "code": parse_code(a.attrib["code"]),
        "mfgCode": None,
        "side": a.attrib.get("side", "server"),
        "type": a.attrib.get("type", ""),
        "included": 1,
        "storageOption": "External",
        "singleton": 0,
        "bounded": 0,
        "defaultValue": safe_default_value(a.attrib.get("default")),
        "reportable": 1,
        "minInterval": 1,
        "maxInterval": 65534,
        "reportableChange": 0,
    }


def cmd_xml_to_zap(c: ET.Element, cluster_side: str = "server") -> dict:
    source = c.attrib.get("source", "client")
    # On a server cluster: client-source commands are incoming, server-source
    # commands (responses) are outgoing.
    is_incoming = 1 if (cluster_side == "server" and source == "client") else 0
    if cluster_side == "client":
        is_incoming = 1 if source == "server" else 0
    return {
        "name": c.attrib["name"],
        "code": parse_code(c.attrib["code"]),
        "mfgCode": None,
        "source": source,
        "isIncoming": is_incoming,
        "isEnabled": 1,
    }


def event_xml_to_zap(e: ET.Element) -> dict:
    return {
        "name": e.attrib["name"],
        "code": parse_code(e.attrib["code"]),
        "mfgCode": None,
        "side": e.attrib.get("side", "server"),
        "included": 1,
    }


def collect_cluster_catalog(xml_files: list[Path]) -> dict[int, ClusterDef]:
    """Walk every XML, build a code->ClusterDef map.

    A `<cluster>` defines a cluster (and one side of it). A
    `<clusterExtension code="0xXXXX">` adds attributes/commands/events to an
    already-defined cluster — used by clusters-extensions.xml etc.
    """
    catalog: dict[int, ClusterDef] = {}

    # Skip manufacturer-specific (vendor-prefixed) attribute/command/event
    # codes — the .zap field "mfgCode" would have to be set, and the C++
    # cluster-objects codegen doesn't emit structs for these, so the
    # generated IMClusterCommandHandler fails to compile.
    def is_mfg_specific(code: int) -> bool:
        return code > 0xFFFF

    def merge_into(c: ClusterDef, side: str, elem: ET.Element):
        for a in elem.findall("attribute"):
            zap = attr_xml_to_zap(a)
            if is_mfg_specific(zap["code"]):
                continue
            if "side" not in a.attrib:
                zap["side"] = side
            catalog_key = (zap["side"], zap["code"])
            c.attributes[catalog_key] = zap
        for cm in elem.findall("command"):
            zap = cmd_xml_to_zap(cm, side)
            if is_mfg_specific(zap["code"]):
                continue
            c.commands[(zap["source"], zap["code"])] = zap
        for ev in elem.findall("event"):
            zap = event_xml_to_zap(ev)
            if is_mfg_specific(zap["code"]):
                continue
            if "side" not in ev.attrib:
                zap["side"] = side
            c.events[(zap["side"], zap["code"])] = zap

    # First pass: real clusters.
    for path in xml_files:
        try:
            tree = ET.parse(path)
        except ET.ParseError as e:
            print(f"WARN: parse failed {path.name}: {e}", file=sys.stderr)
            continue
        for cl in tree.getroot().findall("cluster"):
            code_text = cl.findtext("code")
            if code_text is None:
                continue
            code = parse_code(code_text.strip())
            name = (cl.findtext("name") or "").strip()
            define = (cl.findtext("define") or "").strip()
            c = catalog.setdefault(code, ClusterDef(code=code, name=name, define=define))
            if not c.name:
                c.name = name
            if not c.define:
                c.define = define
            merge_into(c, "server", cl)

    # Second pass: extensions.
    for path in xml_files:
        try:
            tree = ET.parse(path)
        except ET.ParseError:
            continue
        for ext in tree.getroot().findall("clusterExtension"):
            code = parse_code(ext.attrib["code"])
            c = catalog.get(code)
            if c is None:
                print(f"WARN: clusterExtension for unknown cluster {hex(code)} in {path.name}", file=sys.stderr)
                continue
            merge_into(c, "server", ext)

    return catalog


def build_full_cluster_entry(c: ClusterDef) -> dict:
    server_attrs = sorted(
        (a for k, a in c.attributes.items() if k[0] == "server"),
        key=lambda a: a["code"],
    )
    server_events = sorted(
        (e for k, e in c.events.items() if k[0] == "server"),
        key=lambda e: e["code"],
    )
    drop_cmds = SKIP_COMMANDS_BY_CLUSTER.get(c.code, set())
    server_cmds = sorted(
        (cm for cm in c.commands.values() if cm["code"] not in drop_cmds),
        key=lambda cm: (cm["source"], cm["code"]),
    )
    return {
        "name": c.name,
        "code": c.code,
        "mfgCode": None,
        "define": c.define,
        "side": "server",
        "enabled": 1,
        "commands": server_cmds,
        "attributes": server_attrs,
        "events": server_events,
    }


def rewrite_zap(zap_path: Path, zcl_json: Path, skip: set[int], dry_run: bool) -> None:
    with zap_path.open() as f:
        zap = json.load(f)

    xml_files = load_xml_files(zcl_json)
    print(f"loaded {len(xml_files)} XML files from {zcl_json.name}")

    catalog = collect_cluster_catalog(xml_files)
    print(f"catalog: {len(catalog)} clusters")

    if not zap["endpointTypes"]:
        sys.exit("input zap has no endpointTypes")

    # Take all metadata (deviceTypeRef, deviceTypes, deviceVersions, etc.)
    # from the existing endpointTypes[0] so commissioning-relevant fields
    # stay correct. Only the cluster set is replaced.
    ep0_type = json.loads(json.dumps(zap["endpointTypes"][0]))

    # Carry client-side cluster entries forward (e.g., OTA Software Update
    # Provider, which the all-clusters-app uses as a client to bind to a
    # remote provider). Server-side entries are dropped — every server
    # cluster is rebuilt from the catalog with full enablement.
    preserved_clients = [c for c in ep0_type["clusters"] if c.get("side") == "client"]

    rebuilt_servers: list[dict] = []
    skipped_user: list[str] = []
    skipped_empty: list[str] = []
    for code in sorted(catalog):
        cdef = catalog[code]
        if code in skip:
            skipped_user.append(f"{code:#06x} {cdef.name}")
            continue
        if not (cdef.attributes or cdef.commands or cdef.events):
            skipped_empty.append(f"{code:#06x} {cdef.name}")
            continue
        rebuilt_servers.append(build_full_cluster_entry(cdef))

    ep0_type["clusters"] = preserved_clients + rebuilt_servers

    # Single endpoint at id 0. Drop EP1+ entirely.
    zap["endpointTypes"] = [ep0_type]
    zap["endpoints"] = [
        {
            "endpointTypeName": ep0_type.get("name", "MA-rootdevice"),
            "endpointTypeIndex": 0,
            "profileId": ep0_type.get("deviceTypeProfileId", 259),
            "endpointId": 0,
            "networkId": 0,
            "parentEndpointIdentifier": None,
        },
    ]

    print(f"EP0 rebuilt: {len(rebuilt_servers)} server clusters + "
          f"{len(preserved_clients)} client clusters preserved")
    if skipped_user:
        print(f"  user --skip: {len(skipped_user)} -> {skipped_user}")
    if skipped_empty:
        print(f"  empty/extension-only excluded: {len(skipped_empty)} -> {skipped_empty}")

    if dry_run:
        print("dry-run: not writing.")
        return

    with zap_path.open("w") as f:
        json.dump(zap, f, indent=2)
        f.write("\n")
    print(f"wrote {zap_path}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zap", type=Path, default=DEFAULT_ZAP)
    ap.add_argument("--zcl", type=Path, default=DEFAULT_ZCL)
    ap.add_argument(
        "--skip", default="",
        help="comma-separated cluster ids (hex or decimal) to exclude",
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    skip = set()
    for s in args.skip.split(","):
        s = s.strip()
        if s:
            skip.add(parse_code(s))
    skip |= DEFAULT_SKIP

    if not args.zap.exists():
        sys.exit(f"zap file not found: {args.zap}")
    if not args.zcl.exists():
        sys.exit(f"zcl json not found: {args.zcl}")

    rewrite_zap(args.zap, args.zcl, skip, args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
