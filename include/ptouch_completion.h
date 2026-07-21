/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Jack Hosemans
 *
 * Pure state machine for Brother automatic print-status notifications.
 * Keeping this independent from ESP-IDF USB calls makes the completion
 * contract host-testable.
 */
#ifndef PTOUCH_COMPLETION_H
#define PTOUCH_COMPLETION_H

#include <stdbool.h>
#include "ptouch_model.h"
#include "ptouch_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTOUCH_COMPLETION_WAITING = 0,
    PTOUCH_COMPLETION_CONFIRMED,
    PTOUCH_COMPLETION_PRINTER_ERROR,
} ptouch_completion_observation_t;

typedef struct {
    bool armed;
    bool saw_printing_phase;
    bool saw_printing_completed;
    ptouch_completion_strategy_t strategy;
} ptouch_completion_tracker_t;

void ptouch_completion_tracker_init(ptouch_completion_tracker_t *tracker);

/* Arm a new receive epoch only after an idle status-request reply has drained
 * all automatic notifications from earlier jobs. */
void ptouch_completion_tracker_arm(ptouch_completion_tracker_t *tracker);

/* Select a model's completion contract. JOB_STATUS accepts a fresh
 * PRINTING_COMPLETED frame directly; the preflight RX drain is its stale-frame
 * boundary. STRICT_NOTIFICATIONS retains P710's ordered three-frame proof. */
void ptouch_completion_tracker_arm_for_strategy(
    ptouch_completion_tracker_t *tracker,
    ptouch_completion_strategy_t strategy);

/* Observe one 32-byte status frame after the current print stream has been
 * transferred. Completion is confirmed only by the ordered notification
 * sequence documented by Brother:
 *
 *   phase change -> printing
 *   printing completed
 *   phase change -> editing/reception possible
 *
 * Requiring the printing phase prevents stale completion notifications from a
 * previous job from acknowledging the current physical label.
 */
ptouch_completion_observation_t ptouch_completion_observe(
    ptouch_completion_tracker_t *tracker,
    const ptouch_status_t *status);

/* Error bits are authoritative even in a reply/notification frame. An
 * ERROR_OCCURRED or TURNED_OFF status is also an error when Brother does not
 * set one of the documented error bits. */
bool ptouch_status_reports_error(const ptouch_status_t *status);

/* A supported operator clear or print preflight may proceed only after an
 * uncached status request proves idle, error-free state with media present. */
bool ptouch_status_is_idle_ready(const ptouch_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* PTOUCH_COMPLETION_H */
