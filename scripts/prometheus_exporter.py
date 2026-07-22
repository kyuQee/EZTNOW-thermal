import json
import time

# Persistent registry: device_id -> metrics
_device_registry = {}
_parse_errors = 0
_total_events = 0


def build_exposition(registry):
    """Build Prometheus text exposition from the device registry."""
    lines = []
    now = time.time_ns()

    stale_threshold = 60_000_000_000  # 60 seconds in nanoseconds
    
    # Filter out stale devices
    active = {
        dev_id: m for dev_id, m in registry.items()
        if (now - m["last_seen_ns"]) < stale_threshold
    }

    lines.append("# HELP esp32_temperature_celsius Temperature reading from ESP32 sensor")
    lines.append("# TYPE esp32_temperature_celsius gauge")
    for dev_id, m in sorted(active.items()):
        lines.append(f'esp32_temperature_celsius{{device_id="{dev_id}"}} {m["temperature"]:.2f}')

    lines.append("# HELP esp32_humidity_percent Relative humidity reading from ESP32 sensor")
    lines.append("# TYPE esp32_humidity_percent gauge")
    for dev_id, m in sorted(active.items()):
        lines.append(f'esp32_humidity_percent{{device_id="{dev_id}"}} {m["humidity"]:.2f}')

    lines.append("# HELP esp32_last_seen_ns Unix timestamp (ns) of last received packet")
    lines.append("# TYPE esp32_last_seen_ns gauge")
    for dev_id, m in sorted(active.items()):
        lines.append(f'esp32_last_seen_ns{{device_id="{dev_id}"}} {m["last_seen_ns"]}')

    lines.append("# HELP esp32_packets_total Total packets received per device")
    lines.append("# TYPE esp32_packets_total counter")
    for dev_id, m in sorted(active.items()):
        lines.append(f'esp32_packets_total{{device_id="{dev_id}"}} {m["packets"]}')

    lines.append("# HELP mawmaw_exporter_parse_errors_total Total payload parse errors")
    lines.append("# TYPE mawmaw_exporter_parse_errors_total counter")
    lines.append(f"mawmaw_exporter_parse_errors_total {_parse_errors}")

    lines.append("# HELP mawmaw_exporter_events_total Total events processed")
    lines.append("# TYPE mawmaw_exporter_events_total counter")
    lines.append(f"mawmaw_exporter_events_total {_total_events}")

    return "\n".join(lines) + "\n"


def on_trigger(trigger: dict, windows: dict) -> list:
    """Called by mawmaw for every esp32_raw event.

    AP_plugin.so payload is UTF-8 JSON from the ESP32:
        {"device_id":"ESP32_...","temperature":23.50,"humidity":45.00,"timestamp":1234567890}

    We parse the JSON, extract device_id/temperature/humidity from the PAYLOAD,
    update the registry, and emit the full Prometheus exposition text.
    """
    global _device_registry, _parse_errors, _total_events

    _total_events += 1
    seq = trigger.get("sequence", 0)

    # 1. Decode payload bytes -> string
    payload = trigger.get("payload", b"")
    if isinstance(payload, (bytes, bytearray)):
        text = payload.decode("utf-8", errors="replace").strip()
    elif isinstance(payload, str):
        text = payload.strip()
    else:
        text = str(payload).strip()

    # 2. Parse JSON and extract fields FROM THE PAYLOAD
    try:
        data = json.loads(text)
        dev_id = str(data["device_id"])
        temperature = float(data["temperature"])
        humidity = float(data["humidity"])
    except Exception:
        _parse_errors += 1
        # Emit current registry even on error so Prometheus never gets empty data
        exp = build_exposition(_device_registry)
        return [{
            "stream_id": "prometheus_metrics",
            "schema_id": "prom_v1",
            "payload": exp.encode("utf-8"),
            "sequence": seq,
        }]

    # 3. Update registry for this device
    if dev_id not in _device_registry:
        _device_registry[dev_id] = {
            "temperature": 0.0,
            "humidity": 0.0,
            "last_seen_ns": 0,
            "packets": 0,
        }

    _device_registry[dev_id]["temperature"] = temperature
    _device_registry[dev_id]["humidity"] = humidity
    _device_registry[dev_id]["last_seen_ns"] = time.time_ns()
    _device_registry[dev_id]["packets"] += 1

    # 4. Build exposition and emit
    exp = build_exposition(_device_registry)

    return [{
        "stream_id": "prometheus_metrics",
        "schema_id": "prom_v1",
        "payload": exp.encode("utf-8"),
        "sequence": seq,
    }]
