#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ptouch_completion.h"

static ptouch_status_t frame(uint8_t type, uint8_t phase_type,
                             uint16_t phase_number)
{
    ptouch_status_t status;
    memset(&status, 0, sizeof status);
    status.valid = true;
    status.status_type = type;
    status.phase_type = phase_type;
    status.phase_number = phase_number;
    return status;
}

static void normal_sequence_requires_all_three_notifications(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    status = frame(PTOUCH_ST_REPLY_TO_REQUEST, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    status = frame(PTOUCH_ST_PRINTING_COMPLETED, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0001); /* feeding */
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_CONFIRMED);
}

static void stale_completion_and_idle_frames_cannot_acknowledge(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_PRINTING_COMPLETED, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
    status = frame(PTOUCH_ST_REPLY_TO_REQUEST, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
}

static void legacy_job_status_uses_the_preflight_epoch(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);

    ptouch_status_t completed =
        frame(PTOUCH_ST_PRINTING_COMPLETED, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &completed) ==
           PTOUCH_COMPLETION_WAITING);

    ptouch_completion_tracker_arm_for_strategy(
        &tracker, PTOUCH_COMPLETION_JOB_STATUS);
    assert(ptouch_completion_observe(&tracker, &completed) ==
           PTOUCH_COMPLETION_CONFIRMED);

    ptouch_completion_tracker_arm_for_strategy(
        &tracker, PTOUCH_COMPLETION_JOB_STATUS);
    completed.error_information_1 = PTOUCH_ERR1_NO_MEDIA;
    assert(ptouch_completion_observe(&tracker, &completed) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);
}

static void a_new_printing_phase_invalidates_an_old_completion(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    ptouch_completion_observe(&tracker, &status);
    status = frame(PTOUCH_ST_PRINTING_COMPLETED, 0x00, 0x0000);
    ptouch_completion_observe(&tracker, &status);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    ptouch_completion_observe(&tracker, &status);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
}

static void every_error_status_is_terminal(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_ERROR_OCCURRED, 0x00, 0x0000);
    assert(status.error_information_1 == 0);
    assert(status.error_information_2 == 0);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);

    status = frame(PTOUCH_ST_REPLY_TO_REQUEST, 0x00, 0x0000);
    status.error_information_1 = PTOUCH_ERR1_CUTTER_JAM;
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);

    status = frame(PTOUCH_ST_TURNED_OFF, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);

    status = frame(PTOUCH_ST_NOTIFICATION, 0x00, 0x0000);
    status.notification_number = 0x01; /* cover opened */
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);
}

static void invalid_and_incomplete_sequences_remain_uncertain(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);
    ptouch_completion_tracker_arm(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    status.valid = false;
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    ptouch_completion_tracker_arm(&tracker);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0014); /* cover open */
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_PRINTER_ERROR);
}

static void stale_full_sequence_before_preflight_epoch_cannot_acknowledge(void)
{
    ptouch_completion_tracker_t tracker;
    ptouch_completion_tracker_init(&tracker);

    ptouch_status_t status = frame(PTOUCH_ST_PHASE_CHANGE, 0x01, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
    status = frame(PTOUCH_ST_PRINTING_COMPLETED, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
    status = frame(PTOUCH_ST_PHASE_CHANGE, 0x00, 0x0000);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);

    ptouch_completion_tracker_arm(&tracker);
    assert(ptouch_completion_observe(&tracker, &status) ==
           PTOUCH_COMPLETION_WAITING);
}

static void only_a_fresh_idle_reply_is_ready_for_operator_clear(void)
{
    ptouch_status_t status = frame(PTOUCH_ST_REPLY_TO_REQUEST, 0x00, 0x0000);
    status.media_width_mm = 12;
    assert(ptouch_status_is_idle_ready(&status));

    status.status_type = PTOUCH_ST_PRINTING_COMPLETED;
    assert(!ptouch_status_is_idle_ready(&status));

    status.status_type = PTOUCH_ST_REPLY_TO_REQUEST;
    status.error_information_2 = PTOUCH_ERR2_COVER_OPEN;
    assert(!ptouch_status_is_idle_ready(&status));

    status.error_information_2 = 0;
    status.media_width_mm = 0;
    assert(!ptouch_status_is_idle_ready(&status));

    status.media_width_mm = 12;
    status.valid = false;
    assert(!ptouch_status_is_idle_ready(&status));
    assert(!ptouch_status_is_idle_ready(NULL));
}

static void status_level_errors_never_format_as_ready(void)
{
    char error[64];
    ptouch_status_t status = frame(PTOUCH_ST_ERROR_OCCURRED, 0x00, 0x0000);
    assert(ptouch_status_error_string(&status, error, sizeof error) == 1);
    assert(strstr(error, "status 0x02") != NULL);

    status = frame(PTOUCH_ST_TURNED_OFF, 0x00, 0x0000);
    assert(ptouch_status_error_string(&status, error, sizeof error) == 1);
    assert(strstr(error, "turned off") != NULL);

    status = frame(PTOUCH_ST_NOTIFICATION, 0x00, 0x0000);
    status.notification_number = 0x01;
    assert(ptouch_status_error_string(&status, error, sizeof error) == 1);
    assert(strstr(error, "cover open") != NULL);
}

int main(void)
{
    normal_sequence_requires_all_three_notifications();
    stale_completion_and_idle_frames_cannot_acknowledge();
    legacy_job_status_uses_the_preflight_epoch();
    a_new_printing_phase_invalidates_an_old_completion();
    every_error_status_is_terminal();
    invalid_and_incomplete_sequences_remain_uncertain();
    stale_full_sequence_before_preflight_epoch_cannot_acknowledge();
    only_a_fresh_idle_reply_is_ready_for_operator_clear();
    status_level_errors_never_format_as_ready();
    puts("ptouch completion tests: PASS");
    return 0;
}
