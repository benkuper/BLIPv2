"""Check real string replies and full trees on an already reachable test board."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import sys
import urllib.request
from websockets.sync.client import connect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import Client
from blip_led_network_hil import osc_message, decode_osc


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    files = {Path(__file__), ROOT / "v2/tools/control/blip_wasm_hil.py",
             ROOT / "v2/tools/control/blip_serial_control.py", ROOT / "v2/tools/control/blip_led_network_hil.py",
             ROOT / "v2/firmware/CMakeLists.txt", ROOT / "v2/firmware/main/main.cpp"}
    for name in ("blip_core", "blip_oscquery", "blip_network", "blip_transport"):
        files.update(p for p in (ROOT / f"v2/components/{name}").rglob("*")
                     if p.is_file() and p.suffix in (".hpp", ".cpp", ".txt"))
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            for p in sorted(files)}


def read_text(path):
    data = path.read_bytes()
    return data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def nodes(tree):
    yield tree
    for child in tree.get("CONTENTS", {}).values():
        yield from nodes(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    macs = re.findall(r"MAC:\s*([0-9a-f:]+)", read_text(args.flash_log), re.I)
    if not macs or macs[-1].lower() != args.mac.lower():
        parser.error("Flashed MAC differs from expected test board")
    report = {"board": args.board, "port": args.port, "flashed_mac": args.mac.lower(),
              "source_snapshot": sources(), "flash_log_sha256": sha(args.flash_log),
              "artifacts": {name: sha(args.build / name) for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")},
              "pc_network_changed": False, "test_firmware_left_installed": True,
              "guest_defined_controls_exercised": False, "checks": [], "passed": False}
    client = None
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition:
            raise RuntimeError(name)
    try:
        client = Client(args.port)
        ip = client.get("ip_address", "blip.transport.wifi")
        check("serial-owned-ip-string", isinstance(ip, str) and ip not in ("0.0.0.0", ""))
        report["ip"] = ip
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        for cycle in range(3):
            with opener.open(f"http://{ip}/?config=1", timeout=5) as response:
                data = response.read(131073)
                tree = json.loads(data)
                check(f"http-complete-json-{cycle}", response.status == 200 and len(data) <= 131072 and "CONTENTS" in tree)
            component = next(n for n in nodes(tree) if n.get("BLIP_COMPONENT_ID") == "blip.transport.wifi")
            ip_node = component["CONTENTS"]["ip_address"]
            check(f"http-owned-ip-string-{cycle}", ip_node["VALUE"] == [ip])
            password = component["CONTENTS"]["pass"]
            check(f"http-write-only-password-{cycle}", "VALUE" not in password and not password["BLIP_READABLE"])
        report["tree_bytes"] = len(data)
        path = ip_node["FULL_PATH"]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(3)
            for cycle in range(3):
                udp.sendto(osc_message(path, []), (ip, 9000))
                packet, address = udp.recvfrom(1025)
                returned, values = decode_osc(packet)
                check(f"udp-owned-ip-string-{cycle}", address[0] == ip and returned == path and values[-1:] == [ip])
        with connect(f"ws://{ip}/", open_timeout=5, close_timeout=2, proxy=None) as websocket:
            for cycle in range(3):
                websocket.send(osc_message(path, []))
                packet = websocket.recv(timeout=5)
                returned, values = decode_osc(packet)
                check(f"websocket-owned-ip-string-{cycle}", returned == path and values == [ip])
        http_margin = client.get("http_stack_headroom", "blip.oscquery")
        udp_margin = client.get("task_stack_headroom", "blip.oscquery")
        report["stack_headroom"] = {"http_bytes": http_margin, "udp_bytes": udp_margin}
        check("http-stack-margin", http_margin >= 1024)
        check("udp-stack-margin", udp_margin >= 1024)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if client:
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(report["checks"]), "stack_headroom": report["stack_headroom"]}))


if __name__ == "__main__":
    main()
