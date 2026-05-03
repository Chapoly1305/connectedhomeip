#!/usr/bin/env python3
"""Rewrite all-clusters-app.zap so every server cluster known to the
branch's ZAP catalog is enabled on EP1, with all attributes/commands/events
turned on. EP0 (root node) is preserved verbatim from the input file so the
existing root-only cluster set keeps building. EP2..EP65534 are dropped.

This is for fuzz coverage — we want maximum SDK code reachable in the
host all-clusters-app target binary. Embedded targets won't fit the result.

Usage:
    python3 scripts/tools/zap/enable_all_clusters_in_zap.py
        [--zap PATH] [--zcl PATH] [--skip 0xCODE,0xCODE,...] [--dry-run]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]

DEFAULT_ZAP = REPO_ROOT / "examples/all-clusters-app/all-clusters-common/all-clusters-app.zap"
DEFAULT_ZCL = REPO_ROOT / "src/app/zap-templates/zcl/zcl-with-test-extensions.json"

# Spec-mandated root-only clusters. EP1 will not include these — they live
# only on EP0 (preserved from the input file). Anything else from the catalog
# may appear on EP1, even if it also appears on EP0 (e.g. Descriptor, which
# every endpoint must carry).
EP0_ONLY_CLUSTER_IDS: set[int] = {
    0x001F,  # Access Control
    0x0028,  # Basic Information
    0x0029,  # OTA Software Update Provider (client-side)
    0x002A,  # OTA Software Update Requestor
    0x002B,  # Localization Configuration
    0x002C,  # Time Format Localization
    0x002D,  # Unit Localization
    0x002E,  # Power Source Configuration
    0x0030,  # General Commissioning
    0x0031,  # Network Commissioning
    0x0032,  # Diagnostic Logs
    0x0033,  # General Diagnostics
    0x0034,  # Software Diagnostics
    0x0035,  # Thread Network Diagnostics
    0x0036,  # Wi-Fi Network Diagnostics
    0x0037,  # Ethernet Network Diagnostics
    0x0038,  # Time Synchronization
    0x003C,  # Administrator Commissioning
    0x003E,  # Operational Credentials
    0x003F,  # Group Key Management
    0x0046,  # ICD Management
    # Groupcast: CodegenIntegration.cpp asserts the cluster, if present,
    # MUST live on the root endpoint. Excluding from EP1 leaves count=0
    # which satisfies the assert.
    0x0065,
}

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
}

# Per-cluster command codes to skip even if the cluster itself stays. Use
# this when only a handful of commands lack callbacks and skipping the
# whole cluster would lose too much fuzz coverage.
SKIP_COMMANDS_BY_CLUSTER: dict[int, set[int]] = {
    # Level Control: MoveToClosestFrequency (FQ feature) has no callback in
    # the host all-clusters-app — skip just that command.
    0x0008: {0x08},
}


def parse_code(s: str) -> int:
    return int(s, 0)


@dataclass
class ClusterDef:
    code: int
    name: str
    define: str
    has_server: bool = False
    has_client: bool = False
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
        "defaultValue": a.attrib.get("default") if "default" in a.attrib else None,
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
            side_elem = cl.find("server")  # not used by these XMLs
            # The cluster itself doesn't have a side attr — the cluster XML
            # always represents one cluster definition. Side per element comes
            # from each attribute/command/event's own attribs (defaulting
            # server). Treat the cluster definition as both-sides for catalog
            # purposes; we'll only emit server entries for EP1 below.
            c = catalog.setdefault(code, ClusterDef(code=code, name=name, define=define))
            if not c.name:
                c.name = name
            if not c.define:
                c.define = define
            c.has_server = True  # cluster definitions list both sides via element attribs
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


def build_ep1_cluster_entry(c: ClusterDef) -> dict:
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

    # Preserve EP0 exactly. The existing endpointTypes[0] is EP0's type.
    if not zap["endpointTypes"]:
        sys.exit("input zap has no endpointTypes")
    ep0_type = zap["endpointTypes"][0]
    ep0_cluster_codes = {c["code"] for c in ep0_type["clusters"]}

    # Take EP1 metadata from existing endpointTypes[1] if present, otherwise
    # synthesize a minimal MA-onofflight type.
    if len(zap["endpointTypes"]) >= 2:
        ep1_type = json.loads(json.dumps(zap["endpointTypes"][1]))
    else:
        ep1_type = {
            "id": 2,
            "name": "MA-onofflight",
            "deviceTypeRef": {
                "code": 256, "profileId": 259, "label": "MA-onofflight",
                "name": "MA-onofflight", "deviceTypeOrder": 0,
            },
            "deviceTypes": [{
                "code": 256, "profileId": 259, "label": "MA-onofflight",
                "name": "MA-onofflight", "deviceTypeOrder": 0,
            }],
            "deviceVersions": [1],
            "deviceIdentifiers": [256],
            "deviceTypeName": "MA-onofflight",
            "deviceTypeCode": 256,
            "deviceTypeProfileId": 259,
            "clusters": [],
        }

    new_ep1_clusters: list[dict] = []
    included: list[str] = []
    skipped_root: list[str] = []
    skipped_user: list[str] = []
    skipped_empty: list[str] = []
    for code in sorted(catalog):
        cdef = catalog[code]
        if code in EP0_ONLY_CLUSTER_IDS:
            skipped_root.append(f"{code:#06x} {cdef.name}")
            continue
        if code in skip:
            skipped_user.append(f"{code:#06x} {cdef.name}")
            continue
        if not (cdef.attributes or cdef.commands or cdef.events):
            skipped_empty.append(f"{code:#06x} {cdef.name}")
            continue
        new_ep1_clusters.append(build_ep1_cluster_entry(cdef))
        included.append(f"{code:#06x} {cdef.name}")

    ep1_type["clusters"] = new_ep1_clusters

    # Replace endpointTypes with [EP0, EP1] and endpoints with [0, 1].
    zap["endpointTypes"] = [ep0_type, ep1_type]
    zap["endpoints"] = [
        {
            "endpointTypeName": ep0_type.get("name", "MA-rootdevice"),
            "endpointTypeIndex": 0,
            "profileId": 259,
            "endpointId": 0,
            "networkId": 0,
            "parentEndpointIdentifier": None,
        },
        {
            "endpointTypeName": ep1_type.get("name", "MA-onofflight"),
            "endpointTypeIndex": 1,
            "profileId": 259,
            "endpointId": 1,
            "networkId": 0,
            "parentEndpointIdentifier": None,
        },
    ]

    print(f"EP0 preserved: {len(ep0_cluster_codes)} clusters")
    print(f"EP1 rebuilt: {len(new_ep1_clusters)} clusters")
    print(f"  root-only excluded: {len(skipped_root)}")
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
        help="comma-separated cluster ids (hex or decimal) to exclude from EP1",
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
