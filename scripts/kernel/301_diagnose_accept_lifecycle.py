#!/usr/bin/env python3
"""Bounded Linux epoll control for B41; preserve handshake evidence on failure."""

import argparse
from collections import Counter
import errno
import json
from pathlib import Path
import select
import socket
import subprocess
import time


def counters():
    lines = Path("/proc/net/netstat").read_text().splitlines()
    keys = lines[0].split()[1:]
    values = map(int, lines[1].split()[1:])
    return dict(zip(keys, values))


def diagnose(listener, clients, accepted, events):
    for name in ("nf_conntrack_count", "nf_conntrack_max"):
        path = Path("/proc/sys/net/netfilter") / name
        print(f"{name}={path.read_text().strip()}" if path.exists() else f"{name}=unavailable", flush=True)
    states = Counter()
    details = []
    for client in clients:
        info = client.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104)
        error = client.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        states[info[0]] += 1
        details.append({"local": client.getsockname(), "state": info[0],
                        "retransmits": info[2], "so_error": error})
    ready = select.select([listener], [], [], 0)[0]
    direct = 0
    while True:
        try:
            peer, _ = listener.accept()
            accepted.append(peer)
            direct += 1
        except BlockingIOError:
            break
    print(json.dumps({"listener": listener.getsockname(), "states": states,
                      "epoll_events": events, "listener_readable": bool(ready),
                      "direct_accept_after_deadline": direct, "clients": details}), flush=True)
    result = subprocess.run(["ss", "-nto", "sport", "=",
                             str(listener.getsockname()[1])],
                            check=True, capture_output=True, text=True)
    print(result.stdout, flush=True)


def run(args):
    before = counters()
    total = 0
    start = time.monotonic()
    try:
        for run_index in range(args.runs):
            with socket.socket() as listener, select.epoll() as poll:
                listener.setblocking(False)
                listener.bind(("127.0.0.1", 0))
                listener.listen(256)
                for batch in range(args.rounds):
                    clients, accepted = [], []
                    events = 0
                    poll.register(listener.fileno(), select.EPOLLIN | select.EPOLLET)
                    try:
                        for _ in range(args.width):
                            client = socket.socket()
                            clients.append(client)
                            client.setblocking(False)
                            error = client.connect_ex(listener.getsockname())
                            if error not in (0, errno.EINPROGRESS):
                                raise OSError(error, "connect")
                        deadline = time.monotonic() + 2
                        while len(accepted) < args.width and time.monotonic() < deadline:
                            ready = poll.poll(0)
                            events += len(ready)
                            if ready:
                                while len(accepted) < args.width:
                                    try:
                                        peer, _ = listener.accept()
                                        accepted.append(peer)
                                    except BlockingIOError:
                                        break
                        if len(accepted) != args.width:
                            print(f"FAIL run={run_index} batch={batch} accepted={len(accepted)}/{args.width}", flush=True)
                            diagnose(listener, clients, accepted, events)
                            return 1
                        total += args.width
                    finally:
                        poll.unregister(listener.fileno())
                        pairs = (accepted + clients if args.close_order == "server-first"
                                 else clients + accepted)
                        for peer in pairs:
                            peer.close()
            print(f"run={run_index} total={total} elapsed={time.monotonic() - start:.3f}", flush=True)
    finally:
        after = counters()
        print(json.dumps({"counter_delta": {key: after[key] - value
                                           for key, value in before.items()
                                           if after[key] != value}}), flush=True)
    print(f"PASS total={total} close_order={args.close_order}", flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=160)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--close-order", choices=("server-first", "client-first"),
                        default="server-first")
    args = parser.parse_args()
    if min(args.runs, args.rounds, args.width) < 1:
        parser.error("runs, rounds and width must be positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
