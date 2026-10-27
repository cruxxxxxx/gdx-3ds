#!/usr/bin/env python3
"""Writes the Grafana dashboard and alert rules for the rendezvous server:

    grafana/dashboards/gdx-rendezvous.json         (provisioned dashboard)
    grafana/provisioning/alerting/gdx-rules.json   (provisioned alert rules)

Edit this file, run it (python3 tools/mp-rendezvous/monitoring/gen_grafana.py), commit both
outputs. Datasource uids: gdx-prom (Prometheus), gdx-loki (Loki), set in
grafana/provisioning/datasources/gdx.yaml.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PROM = {"type": "prometheus", "uid": "gdx-prom"}
LOKI = {"type": "loki", "uid": "gdx-loki"}

_ids = [0]


def pid():
    _ids[0] += 1
    return _ids[0]


def target(expr, legend="", ref="A", ds=PROM, instant=False):
    t = {"datasource": ds, "expr": expr, "refId": ref, "legendFormat": legend}
    if instant:
        t["instant"] = True
        t["range"] = False
    return t


def stat(title, expr, x, y, w=4, h=4, unit="short", legend="", decimals=None, thresholds=None):
    p = {"id": pid(), "type": "stat", "title": title, "datasource": PROM,
         "gridPos": {"x": x, "y": y, "w": w, "h": h},
         "targets": [target(expr, legend, instant=True)],
         "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
                     "colorMode": "value", "graphMode": "none", "textMode": "auto"},
         "fieldConfig": {"defaults": {"unit": unit, "thresholds": thresholds or {
             "mode": "absolute", "steps": [{"color": "green", "value": None}]}}, "overrides": []}}
    if decimals is not None:
        p["fieldConfig"]["defaults"]["decimals"] = decimals
    return p


def ts(title, targets, x, y, w=12, h=8, unit="short", stack=False, desc=""):
    return {"id": pid(), "type": "timeseries", "title": title, "description": desc, "datasource": PROM,
            "gridPos": {"x": x, "y": y, "w": w, "h": h}, "targets": targets,
            "fieldConfig": {"defaults": {"unit": unit, "custom": {
                "drawStyle": "line", "lineWidth": 1, "fillOpacity": 10 if stack else 0,
                "stacking": {"mode": "normal" if stack else "none", "group": "A"},
                "showPoints": "never"}}, "overrides": []},
            "options": {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
                        "tooltip": {"mode": "multi", "sort": "desc"}}}


def row(title, y):
    return {"id": pid(), "type": "row", "title": title, "collapsed": False, "gridPos": {"x": 0, "y": y, "w": 24, "h": 1},
            "panels": []}


def dashboard():
    red_below_1 = {"mode": "absolute", "steps": [{"color": "red", "value": None}, {"color": "green", "value": 1}]}
    p = []
    y = 0
    p.append(row("Overview", y))
    y += 1
    p += [
        stat("Server up", 'sum(up{job="gdx-rendezvous"}) or vector(0)', 0, y, w=3, thresholds=red_below_1),
        stat("Probe OK", 'min(gdx_rv_probe_success) or vector(0)', 3, y, w=3, thresholds=red_below_1),
        stat("Probe RTT", 'max(gdx_rv_probe_rtt_seconds)', 6, y, w=3, unit="s", decimals=3),
        stat("Rooms", 'sum(gdx_rv_rooms{state!="unconfirmed"})', 9, y, w=3),
        stat("Players", 'sum(gdx_rv_members) - sum(gdx_rv_members_unconfirmed)', 12, y, w=3),
        stat("Relaying", 'sum(gdx_rv_members_relaying)', 15, y, w=3),
        stat("Uptime", 'time() - max(gdx_rv_start_time_seconds)', 18, y, w=3, unit="s"),
        stat("Version", 'max by (version) (gdx_rv_build_info)', 21, y, w=3, legend="{{version}}"),
    ]
    p[-1]["options"]["textMode"] = "name"
    y += 4
    p.append(row("Traffic", y))
    y += 1
    p += [
        ts("Datagrams in / s by type", [target('sum by (type, proto) (rate(gdx_rv_rx_packets_total[$__rate_interval]))',
                                              "{{type}} v{{proto}}")], 0, y, stack=True),
        ts("Datagrams out / s by type", [target('sum by (type) (rate(gdx_rv_tx_packets_total[$__rate_interval]))',
                                               "{{type}}")], 12, y, stack=True),
    ]
    y += 8
    p += [
        ts("Bandwidth", [target('sum(rate(gdx_rv_rx_bytes_total[$__rate_interval]))', "in"),
                         target('sum(rate(gdx_rv_tx_bytes_total[$__rate_interval]))', "out", "B"),
                         target('sum(rate(gdx_rv_relay_bytes_total[$__rate_interval]))', "relayed", "C")],
           0, y, unit="Bps"),
        ts("Event loop", [target('histogram_quantile(0.5, sum by (le) (rate(gdx_rv_handle_seconds_bucket[$__rate_interval])))', "handle p50"),
                          target('histogram_quantile(0.99, sum by (le) (rate(gdx_rv_handle_seconds_bucket[$__rate_interval])))', "handle p99", "B"),
                          target('max(gdx_rv_loop_lag_seconds)', "loop lag", "C")], 12, y, unit="s"),
    ]
    y += 8
    p.append(row("Rooms and players", y))
    y += 1
    p += [
        ts("Rooms by state", [target('sum by (state) (gdx_rv_rooms)', "{{state}}")], 0, y, stack=True),
        ts("Room activity / min", [target('sum by (proto) (rate(gdx_rv_rooms_created_total[$__rate_interval])) * 60', "created v{{proto}}"),
                                   target('sum by (proto) (rate(gdx_rv_joins_total[$__rate_interval])) * 60', "joined v{{proto}}", "B"),
                                   target('sum by (why) (rate(gdx_rv_rooms_closed_total[$__rate_interval])) * 60', "closed: {{why}}", "C")],
           12, y),
    ]
    y += 8
    p += [
        ts("Players that never needed the relay (punch success, 1 h window)",
           [target('sum(increase(gdx_rv_member_sessions_total{relay="no"}[1h])) / sum(increase(gdx_rv_member_sessions_total[1h]))',
                   "direct share")], 0, y, unit="percentunit",
           desc="Of the players that stayed at least 20 s, the share whose datagrams never went through the relay."),
        ts("Members leaving / min by reason", [target('sum by (why) (rate(gdx_rv_members_left_total[$__rate_interval])) * 60',
                                                      "{{why}}")], 12, y, stack=True),
    ]
    y += 8
    p.append(row("Abuse and limits", y))
    y += 1
    p += [
        ts("Drops / s by reason", [target('sum by (reason) (rate(gdx_rv_drops_total[$__rate_interval]))', "{{reason}}")],
           0, y, stack=True, desc="reason=auth: keepalives with a wrong member key (expired members, junk)"),
        ts("Rate-limit hits / s", [target('sum by (limit) (rate(gdx_rv_ratelimit_total[$__rate_interval]))', "{{limit}}")],
           12, y, stack=True),
    ]
    y += 8
    p += [
        ts("Blocklist, unconfirmed rooms, cookies",
           [target('sum(gdx_rv_ips_blocked)', "blocked addresses"),
            target('sum(gdx_rv_rooms{state="unconfirmed"})', "unconfirmed rooms", "B"),
            target('sum(rate(gdx_rv_unconfirmed_evicted_total[$__rate_interval]))', "evicted / s", "C"),
            target('sum(rate(gdx_rv_cookies_issued_total[$__rate_interval]))', "cookies / s", "D"),
            target('sum(rate(gdx_rv_cookies_rejected_total[$__rate_interval]))', "bad cookies / s", "E")], 0, y),
        ts("Errors sent / min by code", [target('sum by (code) (rate(gdx_rv_errors_sent_total[$__rate_interval])) * 60',
                                                "{{code}}")], 12, y, stack=True),
    ]
    y += 8
    p.append(row("Host", y))
    y += 1
    p += [
        ts("CPU", [target('sum by (mode) (rate(gdx_rv_host_cpu_seconds{mode!="idle"}[$__rate_interval])) / scalar(sum(rate(gdx_rv_host_cpu_seconds[$__rate_interval])))',
                          "{{mode}}"),
                   target('rate(gdx_rv_process_cpu_seconds[$__rate_interval])', "server process (cores)", "B")],
           0, y, unit="percentunit", stack=False,
           desc="t4g instances are burstable: sustained use above the baseline (5% for nano) spends CPU credits."),
        ts("Memory", [target('max by (kind) (gdx_rv_host_memory_bytes)', "{{kind}}"),
                      target('max(gdx_rv_process_resident_bytes)', "server RSS", "B")], 12, y, unit="bytes"),
    ]
    y += 8
    p += [
        ts("Probe", [target('max(gdx_rv_probe_rtt_seconds)', "rtt"),
                     target('min(gdx_rv_probe_success)', "success", "B")], 0, y, unit="s"),
        ts("Log shipping", [target('sum by (outcome) (rate(gdx_rv_loki_lines_total[$__rate_interval]))', "{{outcome}}"),
                            target('sum by (event) (rate(gdx_rv_log_suppressed_total[$__rate_interval]))', "suppressed {{event}}", "B")],
           12, y),
    ]
    y += 8
    p.append(row("Access log (Loki)", y))
    y += 1
    p.append({"id": pid(), "type": "timeseries", "title": "Events / 5 min", "datasource": LOKI,
              "gridPos": {"x": 0, "y": y, "w": 24, "h": 7},
              "targets": [target('sum by (event) (count_over_time({job="gdx-rendezvous", event!="stats"}[5m]))',
                                 "{{event}}", ds=LOKI)],
              "fieldConfig": {"defaults": {"custom": {"drawStyle": "bars", "stacking": {"mode": "normal"},
                                                      "fillOpacity": 60}}, "overrides": []}})
    y += 7
    p.append({"id": pid(), "type": "logs", "title": "Room events (create, join, leave, close, refuse, block)",
              "datasource": LOKI, "gridPos": {"x": 0, "y": y, "w": 24, "h": 12},
              "targets": [target('{job="gdx-rendezvous", event=~"$event"}', ds=LOKI)],
              "options": {"showTime": True, "wrapLogMessage": True, "sortOrder": "Descending",
                          "enableLogDetails": True, "prettifyLogMessage": False}})
    return {
        "uid": "gdx-rendezvous", "title": "G-Diffuser rendezvous", "tags": ["gdx"], "timezone": "browser",
        "schemaVersion": 39, "version": 1, "editable": True, "refresh": "30s",
        "time": {"from": "now-6h", "to": "now"},
        "templating": {"list": [{
            "name": "event", "label": "Log events", "type": "custom", "multi": True, "includeAll": False,
            "query": "create,join,rejoin,rehost,leave,orphan,room-close,refuse,block,abuse,drain,listen,stop,stats",
            "current": {"selected": True, "text": ["create", "join", "leave", "room-close", "refuse", "block"],
                        "value": ["create", "join", "leave", "room-close", "refuse", "block"]},
            "options": []}]},
        "annotations": {"list": []},
        "panels": p,
    }


def rule(uid, title, expr, op, threshold, for_, severity, summary, no_data="Alerting"):
    return {
        "uid": uid, "title": title, "condition": "C", "for": for_, "noDataState": no_data, "execErrState": "Error",
        "labels": {"severity": severity, "service": "gdx-rendezvous"},
        "annotations": {"summary": summary},
        "data": [
            {"refId": "A", "relativeTimeRange": {"from": 600, "to": 0}, "datasourceUid": "gdx-prom",
             "model": {"refId": "A", "expr": expr, "instant": True, "range": False}},
            {"refId": "B", "datasourceUid": "__expr__",
             "model": {"refId": "B", "type": "reduce", "expression": "A", "reducer": "last", "settings": {"mode": "dropNN"}}},
            {"refId": "C", "datasourceUid": "__expr__",
             "model": {"refId": "C", "type": "threshold", "expression": "B",
                       "conditions": [{"evaluator": {"type": op, "params": [threshold]}}]}},
        ],
    }


def alerts():
    rules = [
        rule("gdx-down", "Rendezvous server down", 'sum(up{job="gdx-rendezvous"}) or vector(0)', "lt", 1, "2m",
             "critical", "Prometheus cannot scrape the rendezvous server's /metrics."),
        rule("gdx-probe-fail", "Rendezvous probe failing", 'min(gdx_rv_probe_success) or vector(0)', "lt", 1, "3m",
             "critical", "The UDP probe gets no correct answer from the server's public address (what consoles use)."),
        rule("gdx-drops-high", "Rendezvous dropping a lot", 'sum(rate(gdx_rv_drops_total[5m])) or vector(0)', "gt", 200,
             "10m", "warning", "Over 200 dropped datagrams/s for 10 min: junk, a flood, or a broken client.",
             no_data="OK"),
        rule("gdx-relay-throttled", "Players hit the relay budget",
             'sum(rate(gdx_rv_ratelimit_total{limit=~"member_relay|room_relay_bytes"}[5m])) or vector(0)', "gt", 1, "10m",
             "warning", "Real relay traffic is being rate-limited (--relay-pps / --room-bps too low, or abuse).",
             no_data="OK"),
        rule("gdx-cpu-high", "Rendezvous host CPU high",
             '(sum(rate(gdx_rv_host_cpu_seconds{mode!="idle"}[5m])) / sum(rate(gdx_rv_host_cpu_seconds[5m]))) or vector(0)',
             "gt", 0.3, "15m", "warning", "CPU above 30% for 15 min: a burstable instance spends its credits.",
             no_data="OK"),
        rule("gdx-mem-low", "Rendezvous host memory low",
             'min(gdx_rv_host_memory_bytes{kind="available"}) or vector(1e12)', "lt", 50e6, "10m", "warning",
             "Less than 50 MB available on the server host.", no_data="OK"),
        rule("gdx-logs-failing", "Access log shipping failing",
             'sum(rate(gdx_rv_loki_lines_total{outcome=~"failed|dropped_queue"}[10m])) or vector(0)', "gt", 0, "15m",
             "warning", "The server cannot push event lines to Loki.", no_data="OK"),
    ]
    return {"apiVersion": 1, "groups": [{"orgId": 1, "name": "gdx-rendezvous", "folder": "gdx", "interval": "1m",
                                         "rules": rules}]}


def main():
    out = os.path.join(HERE, "grafana", "dashboards", "gdx-rendezvous.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(dashboard(), f, indent=1, sort_keys=True)
        f.write("\n")
    out = os.path.join(HERE, "grafana", "provisioning", "alerting", "gdx-rules.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(alerts(), f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
