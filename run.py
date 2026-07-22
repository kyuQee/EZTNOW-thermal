#!/usr/bin/env python3
"""
run.py - Launch mawmaw + Prometheus + Grafana on Rocky Linux.

Starts:
  1. mawmaw     (sudo, WiFi hotspot + ESP32 ingestor)
  2. Prometheus (scraper + TSDB + web UI on :9090)
  3. Grafana    (dashboards on :3000)

Usage:
  python3 run.py              # Start everything, open Grafana
  python3 run.py --browser prometheus
  python3 run.py --no-grafana
"""
import os
import sys
import subprocess
import time
import signal
import webbrowser
import argparse
import threading


def main():
    parser = argparse.ArgumentParser(description="Launch MAWMAW monitoring stack")
    parser.add_argument(
        "--no-grafana", action="store_true",
        help="Skip launching Grafana"
    )
    parser.add_argument(
        "--browser", default="grafana",
        choices=["prometheus", "grafana", "none"],
        help="Which UI to auto-open (default: grafana)"
    )
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    mawmaw_bin = "./mawmaw"
    prom_bin = "./prometheus/prometheus"
    graf_bin = "./grafana/bin/grafana-server"

    # Sanity checks
    if not os.path.isfile(mawmaw_bin):
        print(f"ERROR: {mawmaw_bin} not found.", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(prom_bin):
        print(
            f"ERROR: {prom_bin} not found.\n"
            "Run:  bash setup-rocky.sh",
            file=sys.stderr,
        )
        sys.exit(1)

    os.makedirs("./prometheus/data", exist_ok=True)

    procs = []

    print("=" * 64)
    print("  MAWMAW Monitoring Stack — Rocky Linux")
    print("=" * 64)

    # 1. mawmaw (sudo required for AP_plugin.so: nmcli, firewalld)
    print("\n[1/3] Starting mawmaw (sudo required)...")
    mawmaw_proc = subprocess.Popen(
        ["sudo", mawmaw_bin],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    procs.append(("mawmaw", mawmaw_proc))

    # 2. Prometheus
    print("[2/3] Starting Prometheus on http://localhost:9090 ...")
    prom_proc = subprocess.Popen(
        [
            prom_bin,
            "--config.file=./prometheus.yml",
            "--storage.tsdb.path=./prometheus/data",
            "--web.enable-lifecycle",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    procs.append(("prometheus", prom_proc))

    # 3. Grafana (optional)
    graf_proc = None
    if not args.no_grafana:
        if not os.path.isfile(graf_bin):
            print(
                f"WARNING: {graf_bin} not found. Skipping Grafana.\n"
                "Run:  bash setup-rocky.sh",
                file=sys.stderr,
            )
        else:
            print("[3/3] Starting Grafana on http://localhost:3000 ...")
            os.makedirs("./grafana/data", exist_ok=True)
            os.makedirs("./grafana/logs", exist_ok=True)

            env = os.environ.copy()
            env["GF_PATHS_DATA"] = os.path.abspath("./grafana/data")
            env["GF_PATHS_LOGS"] = os.path.abspath("./grafana/logs")
            env["GF_SECURITY_ADMIN_USER"] = "admin"
            env["GF_SECURITY_ADMIN_PASSWORD"] = "admin"

            graf_proc = subprocess.Popen(
                [graf_bin, "-homepath", os.path.abspath("./grafana")],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=env,
            )
            procs.append(("grafana", graf_proc))

    # Open browser
    if args.browser == "prometheus":
        url = "http://localhost:9090"
    elif args.browser == "grafana" and graf_proc:
        url = "http://localhost:3000"
    else:
        url = None

    if url:
        time.sleep(2)
        print(f"\nOpening browser: {url}")
        webbrowser.open(url)

    print("\n" + "=" * 64)
    print("  All services running. Press Ctrl+C to stop.")
    print("=" * 64 + "\n")

    # Signal handling
    def shutdown(signum=None, frame=None):
        print("\n[STOP] Shutting down services...")
        for name, proc in reversed(procs):
            if proc.poll() is None:
                print(f"       Stopping {name} ...")
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        print("[STOP] Done.")
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    # Tee stdout so you see logs live
    def tee_output(name, proc):
        try:
            for line in iter(proc.stdout.readline, b""):
                print(f"[{name}] {line.decode('utf-8', errors='replace').rstrip()}")
        except Exception:
            pass

    for name, proc in procs:
        t = threading.Thread(target=tee_output, args=(name, proc), daemon=True)
        t.start()

    # Monitor loop
    try:
        while True:
            for name, proc in procs:
                if proc.poll() is not None:
                    print(f"\nWARNING: {name} exited unexpectedly (code {proc.returncode}).")
                    shutdown()
            time.sleep(1)
    except Exception as e:
        print(f"\nERROR: {e}")
        shutdown()


if __name__ == "__main__":
    main()
