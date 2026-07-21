/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 */
#include "ptouch_completion.h"

#include <string.h>

bool ptouch_status_reports_error(const ptouch_status_t *status)
{
    if (!status || !status->valid) return false;
    return status->error_information_1 != 0 ||
           status->error_information_2 != 0 ||
           status->status_type == PTOUCH_ST_ERROR_OCCURRED ||
           status->status_type == PTOUCH_ST_TURNED_OFF ||
           (status->status_type == PTOUCH_ST_NOTIFICATION &&
            status->notification_number == 0x01) || /* cover opened */
           (status->status_type == PTOUCH_ST_PHASE_CHANGE &&
            status->phase_type == 0x01 &&
            status->phase_number == 0x0014); /* cover open while receiving */
}

bool ptouch_status_is_idle_ready(const ptouch_status_t *status)
{
    return status && status->valid &&
           status->status_type == PTOUCH_ST_REPLY_TO_REQUEST &&
           !ptouch_status_reports_error(status) &&
           status->media_width_mm > 0;
}

void ptouch_completion_tracker_init(ptouch_completion_tracker_t *tracker)
{
    if (tracker) memset(tracker, 0, sizeof *tracker);
}

void ptouch_completion_tracker_arm(ptouch_completion_tracker_t *tracker)
{
    ptouch_completion_tracker_arm_for_strategy(
        tracker, PTOUCH_COMPLETION_STRICT_NOTIFICATIONS);
}

void ptouch_completion_tracker_arm_for_strategy(
    ptouch_completion_tracker_t *tracker,
    ptouch_completion_strategy_t strategy)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof *tracker);
    tracker->armed = true;
    tracker->strategy = strategy;
}

ptouch_completion_observation_t ptouch_completion_observe(
    ptouch_completion_tracker_t *tracker,
    const ptouch_status_t *status)
{
    if (!tracker || !tracker->armed || !status || !status->valid) {
        return PTOUCH_COMPLETION_WAITING;
    }
    if (ptouch_status_reports_error(status)) {
        return PTOUCH_COMPLETION_PRINTER_ERROR;
    }

    if (status->status_type == PTOUCH_ST_PHASE_CHANGE) {
        if (status->phase_type == 0x01 &&
            status->phase_number == 0x0000) { /* printing */
            tracker->saw_printing_phase = true;
            tracker->saw_printing_completed = false;
        } else if (status->phase_type == 0x00 &&
                   status->phase_number == 0x0000 &&
                   tracker->saw_printing_phase &&
                   tracker->saw_printing_completed) {
            return PTOUCH_COMPLETION_CONFIRMED;
        }
    } else if (status->status_type == PTOUCH_ST_PRINTING_COMPLETED) {
        if (tracker->strategy == PTOUCH_COMPLETION_JOB_STATUS) {
            return PTOUCH_COMPLETION_CONFIRMED;
        }
        if (tracker->saw_printing_phase) {
            tracker->saw_printing_completed = true;
        }
    }

    return PTOUCH_COMPLETION_WAITING;
}
