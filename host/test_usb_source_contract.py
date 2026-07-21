#!/usr/bin/env python3
"""Regression checks for safety properties that depend on ESP-IDF wiring."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "ptouch_usb.c").read_text()
HEADER = (ROOT / "include" / "ptouch_usb.h").read_text()


def section(start: str, end: str) -> str:
    begin = SOURCE.index(start)
    finish = SOURCE.index(end, begin)
    return SOURCE[begin:finish]


cancel = section("static xfer_result_t cancel_timed_out_transfer", 
                 "static xfer_result_t submit_and_wait")
assert "portMAX_DELAY" not in cancel
assert "PTOUCH_TRANSFER_RECLAIM_GRACE_MS" in cancel
assert "fault_transport" in cancel

emit = section("static void emit(", "static bool profile_output_allowed")
assert "xQueueSend" in emit
assert "s_cfg.on_event(" not in emit

event_worker = section("static void event_task(", "/* Queueing is safe")
assert "s_cfg.on_event(" in event_worker

bulk = section("static ptouch_usb_transfer_result_t bulk_out_locked", 
               "int ptouch_usb_bulk_out")
assert "output_quarantined()" in bulk
assert "transport_faulted()" in bulk

bitmap = section("ptouch_usb_print_result_t ptouch_usb_print_bitmap(",
                 "static const char *status_txn_locked")
assert "!opts" in bitmap

start = SOURCE[SOURCE.index("esp_err_t ptouch_usb_start"):]
assert "ESP_ERR_INVALID_STATE" in start
assert "s_start_called" in start
assert start.index("xQueueCreate") < start.index("xTaskCreate(usb_task")

public_surface = SOURCE + HEADER
assert "Itembase" not in public_surface
assert "can_cloud_print" not in public_surface

print("ptouch USB source-contract tests: PASS")
