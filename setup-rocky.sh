#!/bin/bash
# setup-rocky.sh — Download Prometheus & Grafana binaries for Rocky Linux
# Run this once from your project directory.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PROM_VERSION="2.55.0"
GRAFANA_VERSION="11.1.0"

echo "========================================"
echo "Setting up MAWMAW monitoring stack"
echo "Rocky Linux / RHEL / Fedora compatible"
echo "========================================"

# --- Prometheus --------------------------------------------------------------
if [ -d "./prometheus" ] && [ -f "./prometheus/prometheus" ]; then
    echo "[OK] Prometheus already installed."
else
    echo "[1/2] Downloading Prometheus v${PROM_VERSION}..."
    mkdir -p prometheus
    cd prometheus
    wget -q --show-progress "https://github.com/prometheus/prometheus/releases/download/v${PROM_VERSION}/prometheus-${PROM_VERSION}.linux-amd64.tar.gz"
    tar xzf "prometheus-${PROM_VERSION}.linux-amd64.tar.gz" --strip-components=1
    rm "prometheus-${PROM_VERSION}.linux-amd64.tar.gz"
    mkdir -p data
    cd ..
    echo "[OK] Prometheus installed in ./prometheus/"
fi

# --- Grafana -----------------------------------------------------------------
if [ -d "./grafana" ] && [ -f "./grafana/bin/grafana-server" ]; then
    echo "[OK] Grafana already installed."
else
    echo "[2/2] Downloading Grafana v${GRAFANA_VERSION}..."
    mkdir -p grafana
    cd grafana
    wget -q --show-progress "https://dl.grafana.com/oss/release/grafana-${GRAFANA_VERSION}.linux-amd64.tar.gz"
    tar xzf "grafana-${GRAFANA_VERSION}.linux-amd64.tar.gz" --strip-components=1
    rm "grafana-${GRAFANA_VERSION}.linux-amd64.tar.gz"
    mkdir -p data logs
    cd ..
    echo "[OK] Grafana installed in ./grafana/"
fi

echo ""
echo "========================================"
echo "Setup complete!"
echo ""
echo "Next steps:"
echo "  1. Ensure mawmaw binary is in this directory: ./mawmaw"
echo "  2. Ensure AP_plugin.so is in this directory: ./AP_plugin.so"
echo "  3. Ensure prometheus_exporter.py is in: ./scripts/prometheus_exporter.py"
echo "  4. Run:  python3 run.py"
echo ""
echo "Services:"
echo "  mawmaw     -> localhost:8080/metrics  (ESP32 metrics endpoint)"
echo "  prometheus -> http://localhost:9090   (Prometheus UI)"
echo "  grafana    -> http://localhost:3000   (Grafana UI, admin/admin)"
echo "========================================"
