--
-- Copyright 2019 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

-- Create the base tables and views containing the launch spans.
SELECT IMPORT('android.startup.startups');
SELECT RUN_METRIC('android/process_metadata.sql');

-- Define the helper functions which will be used throught the remainder
-- of the metric.
SELECT RUN_METRIC('android/startup/slice_functions.sql');
SELECT IMPORT('common.timestamps');

-- Run all the HSC metrics.
SELECT RUN_METRIC('android/startup/hsc.sql');

-- Define some helper functions related to breaking down thread state
-- for launches.
SELECT RUN_METRIC('android/startup/thread_state_breakdown.sql');

-- Define helper functions to break down slices/threads by thread
-- state.
SELECT RUN_METRIC('android/startup/mcycles_per_launch.sql');

-- Define helper functions for GC slices.
SELECT RUN_METRIC('android/startup/gc_slices.sql');

-- Define helper functions for system state.
SELECT RUN_METRIC('android/startup/system_state.sql');

-- Returns the slices for forked processes. Never present in hot starts.
-- Prefer this over process start_ts, since the process might have
-- been preforked.
SELECT CREATE_VIEW_FUNCTION(
  'ZYGOTE_FORK_FOR_LAUNCH(startup_id INT)',
  'ts INT, dur INT',
  '
    SELECT slice.ts, slice.dur
    FROM android_startups l
    JOIN slice ON (
      l.ts < slice.ts AND
      slice.ts + slice.dur < l.ts_end AND
      STR_SPLIT(slice.name, ": ", 1) = l.package
    )
    WHERE l.startup_id = $startup_id AND slice.name GLOB "Start proc: *"
  '
);

-- Returns the fully drawn slice proto given a launch id.
CREATE PERFETTO FUNCTION report_fully_drawn_for_launch(startup_id INT)
RETURNS PROTO AS
SELECT
  startup_slice_proto(report_fully_drawn_ts - launch_ts)
FROM (
  SELECT
    launches.ts AS launch_ts,
    min(slice.ts) AS report_fully_drawn_ts
  FROM android_startups launches
  JOIN android_startup_processes ON (launches.startup_id = android_startup_processes.startup_id)
  JOIN thread USING (upid)
  JOIN thread_track USING (utid)
  JOIN slice ON (slice.track_id = thread_track.id)
  WHERE
    slice.name GLOB "reportFullyDrawn*" AND
    slice.ts >= launches.ts AND
    launches.startup_id = $startup_id
);

-- Given a launch id and GLOB for a slice name, returns the N longest slice name and duration.
SELECT CREATE_VIEW_FUNCTION(
  'GET_LONG_SLICES_FOR_LAUNCH(startup_id INT, slice_name STRING, top_n INT)',
  'slice_name STRING, slice_dur INT',
  '
    SELECT slice_name, slice_dur
    FROM android_thread_slices_for_all_startups s
    WHERE s.startup_id = $startup_id AND s.slice_name GLOB $slice_name
    ORDER BY slice_dur DESC
    LIMIT $top_n
  '
);

-- Define the view
DROP VIEW IF EXISTS startup_view;
CREATE VIEW startup_view AS
SELECT
  AndroidStartupMetric_Startup(
    'startup_id',launches.startup_id,
    'startup_type', (
      SELECT lp.startup_type
      FROM android_startup_processes lp
      WHERE lp.startup_id =launches.startup_id
      LIMIT 1
    ),
    'package_name', launches.package,
    'process_name', (
      SELECT p.name
      FROM android_startup_processes lp
      JOIN process p USING (upid)
      WHERE lp.startup_id =launches.startup_id
      LIMIT 1
    ),
    'process', (
      SELECT m.metadata
      FROM process_metadata m
      JOIN android_startup_processes p USING (upid)
      WHERE p.startup_id =launches.startup_id
      LIMIT 1
    ),
    'activities', (
      SELECT RepeatedField(AndroidStartupMetric_Activity(
        'name', (SELECT STR_SPLIT(s.slice_name, ':', 1)),
        'method', (SELECT STR_SPLIT(s.slice_name, ':', 0)),
        'ts_method_start', s.slice_ts
        ))
      FROM thread_slices_for_all_launches s
      WHERE
        s.startup_id =launches.startup_id
        AND (s.slice_name GLOB 'performResume:*' OR s.slice_name GLOB 'performCreate:*')
    ),
    'long_binder_transactions', (
      SELECT RepeatedField(
        AndroidStartupMetric_BinderTransaction(
          "duration", startup_slice_proto(s.slice_dur),
          "thread", s.thread_name,
          "destination_thread", EXTRACT_ARG(s.arg_set_id, "destination name"),
          "destination_process", s.process,
          "flags", EXTRACT_ARG(s.arg_set_id, "flags"),
          "code", EXTRACT_ARG(s.arg_set_id, "code"),
          "data_size", EXTRACT_ARG(s.arg_set_id, "data_size")
        )
      )
      FROM ANDROID_BINDER_TRANSACTION_SLICES_FOR_STARTUP(launches.startup_id, 2e7) s
    ),
    'zygote_new_process', EXISTS(SELECT TRUE FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)),
    'activity_hosting_process_count', (
      SELECT COUNT(1) FROM android_startup_processes p
      WHERE p.startup_id =launches.startup_id
    ),
    'event_timestamps', AndroidStartupMetric_EventTimestamps(
      'intent_received', launches.ts,
      'first_frame', launches.ts_end
    ),
    'to_first_frame', AndroidStartupMetric_ToFirstFrame(
      'dur_ns', launches.dur,
      'dur_ms', launches.dur / 1e6,
      'main_thread_by_task_state', AndroidStartupMetric_TaskStateBreakdown(
        'running_dur_ns', IFNULL(
          main_thread_time_for_launch_and_state(launches.startup_id, 'Running'), 0
        ),
        'runnable_dur_ns', IFNULL(
          main_thread_time_for_launch_in_runnable_state(launches.startup_id), 0
        ),
        'uninterruptible_sleep_dur_ns', IFNULL(
          main_thread_time_for_launch_and_state(launches.startup_id, 'D*'), 0
        ),
        'interruptible_sleep_dur_ns', IFNULL(
          main_thread_time_for_launch_and_state(launches.startup_id, 'S'), 0
        ),
        'uninterruptible_io_sleep_dur_ns', IFNULL(
          main_thread_time_for_launch_state_and_io_wait(launches.startup_id, 'D*', TRUE), 0
        ),
        'uninterruptible_non_io_sleep_dur_ns', IFNULL(
          main_thread_time_for_launch_state_and_io_wait(launches.startup_id, 'D*', FALSE), 0
        )

      ),
      'mcycles_by_core_type', NULL_IF_EMPTY(AndroidStartupMetric_McyclesByCoreType(
        'little', mcycles_for_launch_and_core_type(launches.startup_id, 'little'),
        'big', mcycles_for_launch_and_core_type(launches.startup_id, 'big'),
        'bigger', mcycles_for_launch_and_core_type(launches.startup_id, 'bigger'),
        'unknown', mcycles_for_launch_and_core_type(launches.startup_id, 'unknown')
      )),
      'to_post_fork',
      launch_to_main_thread_slice_proto(launches.startup_id, 'PostFork'),
      'to_activity_thread_main',
      launch_to_main_thread_slice_proto(launches.startup_id, 'ActivityThreadMain'),
      'to_bind_application',
      launch_to_main_thread_slice_proto(launches.startup_id, 'bindApplication'),
      'time_activity_manager', (
        SELECT startup_slice_proto(l.ts - launches.ts)
        FROM internal_startup_events l
        WHERE l.ts BETWEEN launches.ts AND launches.ts + launches.dur
      ),
      'time_post_fork',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'PostFork'),
      'time_activity_thread_main',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'ActivityThreadMain'),
      'time_bind_application',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'bindApplication'),
      'time_activity_start',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'activityStart'),
      'time_activity_resume',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'activityResume'),
      'time_activity_restart',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'activityRestart'),
      'time_choreographer',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'Choreographer#doFrame*'),
      'time_inflate',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'inflate'),
      'time_get_resources',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'ResourcesManager#getResources'),
      'time_dex_open',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'OpenDexFilesFromOat*'),
      'time_verify_class',
      dur_sum_slice_proto_for_launch(launches.startup_id, 'VerifyClass*'),
      'time_gc_total', (
        SELECT NULL_IF_EMPTY(startup_slice_proto(total_gc_time_by_launch(launches.startup_id)))
      ),
      'time_dex_open_thread_main',
      dur_sum_main_thread_slice_proto_for_launch(
        launches.startup_id,
        'OpenDexFilesFromOat*'),
      'time_dlopen_thread_main',
      dur_sum_main_thread_slice_proto_for_launch(
        launches.startup_id,
        'dlopen:*.so'),
      'time_lock_contention_thread_main',
      dur_sum_main_thread_slice_proto_for_launch(
        launches.startup_id,
        'Lock contention on*'
      ),
      'time_monitor_contention_thread_main',
      dur_sum_main_thread_slice_proto_for_launch(
        launches.startup_id,
        'Lock contention on a monitor*'
      ),
      'time_before_start_process', (
        SELECT startup_slice_proto(ts - launches.ts)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)
      ),
      'time_jit_thread_pool_on_cpu', NULL_IF_EMPTY(startup_slice_proto(
        thread_time_for_launch_state_and_thread(
         launches.startup_id,
          'Running',
          'Jit thread pool'
        )
      )),
      'time_gc_on_cpu', (
        SELECT startup_slice_proto(sum_dur)
        FROM running_gc_slices_materialized
        WHERE launches.startup_id = startup_id
      ),
      'time_during_start_process', (
        SELECT startup_slice_proto(dur)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)
      ),
      'jit_compiled_methods', (
        SELECT IIF(COUNT(1) = 0, NULL, COUNT(1))
        FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(launches.startup_id, 'JIT compiling*')
        WHERE thread_name = 'Jit thread pool'
      ),
      'other_processes_spawned_count', (
        SELECT COUNT(1)
        FROM process
        WHERE
          process.start_ts BETWEEN launches.ts AND launches.ts + launches.dur
          AND process.upid NOT IN (
            SELECT upid FROM android_startup_processes
            WHERE android_startup_processes.startup_id =launches.startup_id
          )
      )
    ),
    'hsc', NULL_IF_EMPTY(AndroidStartupMetric_HscMetrics(
      'full_startup', (
        SELECT startup_slice_proto(h.ts_total)
        FROM hsc_based_startup_times h
        WHERE h.id =launches.startup_id
      )
    )),
    'report_fully_drawn', NULL_IF_EMPTY(report_fully_drawn_for_launch(launches.startup_id)),
    'optimization_status', (
      SELECT RepeatedField(AndroidStartupMetric_OptimizationStatus(
        'location', SUBSTR(STR_SPLIT(slice_name, ' status=', 0), LENGTH('location=') + 1),
        'odex_status', STR_SPLIT(STR_SPLIT(slice_name, ' status=', 1), ' filter=', 0),
        'compilation_filter', STR_SPLIT(STR_SPLIT(slice_name, ' filter=', 1), ' reason=', 0),
        'compilation_reason', STR_SPLIT(slice_name, ' reason=', 1),
        'summary',
        summary_for_optimization_status(
          SUBSTR(STR_SPLIT(slice_name, ' status=', 0), LENGTH('location=') + 1),
          STR_SPLIT(STR_SPLIT(slice_name, ' status=', 1), ' filter=', 0),
          STR_SPLIT(STR_SPLIT(slice_name, ' filter=', 1), ' reason=', 0),
          STR_SPLIT(slice_name, ' reason=', 1))
        ))
      FROM (
        SELECT *
        FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(
         launches.startup_id,
          'location=* status=* filter=* reason=*'
        )
        ORDER BY slice_name
      )
    ),
    'verify_class', (
      SELECT RepeatedField(AndroidStartupMetric_VerifyClass(
        'name', STR_SPLIT(slice_name, "VerifyClass ", 1),
        'dur_ns', slice_dur))
      FROM GET_LONG_SLICES_FOR_LAUNCH(launches.startup_id, "VerifyClass *", 5)
    ),
    'startup_concurrent_to_launch', (
      SELECT RepeatedField(package)
      FROM android_startups l
      WHERE l.startup_id != launches.startup_id
        AND is_spans_overlapping(l.ts, l.ts_end, launches.ts, launches.ts_end)
    ),
    'dlopen_file', (
      SELECT RepeatedField(STR_SPLIT(slice_name, "dlopen: ", 1))
      FROM android_thread_slices_for_all_startups s
      WHERE startup_id = launches.startup_id AND slice_name GLOB "dlopen: *.so"
    ),
    'system_state', AndroidStartupMetric_SystemState(
      'dex2oat_running',
      dur_of_process_running_concurrent_to_launch(launches.startup_id, '*dex2oat64') > 0,
      'installd_running',
      dur_of_process_running_concurrent_to_launch(launches.startup_id, '*installd') > 0,
      'broadcast_dispatched_count',
      count_slices_concurrent_to_launch(launches.startup_id, 'Broadcast dispatched*'),
      'broadcast_received_count',
      count_slices_concurrent_to_launch(launches.startup_id, 'broadcastReceiveReg*'),
      'most_active_non_launch_processes',
      n_most_active_process_names_for_launch(launches.startup_id),
      'installd_dur_ns',
      dur_of_process_running_concurrent_to_launch(launches.startup_id, '*installd'),
      'dex2oat_dur_ns',
      dur_of_process_running_concurrent_to_launch(launches.startup_id, '*dex2oat64')
    ),
    'slow_start_reason', (SELECT RepeatedField(slow_cause)
      FROM (
        SELECT 'No baseline or cloud profiles' AS slow_cause
        WHERE missing_baseline_profile_for_launch(launches.startup_id, launches.package)

        UNION ALL
        SELECT 'Optimized artifacts missing, run from apk'
        WHERE  run_from_apk_for_launch(launches.startup_id)

        UNION ALL
        SELECT 'Unlock running during launch'
        WHERE is_unlock_running_during_launch(launches.startup_id)

        UNION ALL
        SELECT 'App in debuggable mode'
        WHERE is_process_debuggable(launches.package)

        UNION ALL
        SELECT 'GC Activity'
        WHERE total_gc_time_by_launch(launches.startup_id) > 0

        UNION ALL
        SELECT 'dex2oat running during launch' AS slow_cause
        WHERE
          dur_of_process_running_concurrent_to_launch(launches.startup_id, '*dex2oat64') > 0

        UNION ALL
        SELECT 'installd running during launch' AS slow_cause
        WHERE
          dur_of_process_running_concurrent_to_launch(launches.startup_id, '*installd') > 0

        UNION ALL
        SELECT 'Main Thread - Time spent in Runnable state'
          AS slow_cause
        WHERE
          main_thread_time_for_launch_in_runnable_state(launches.startup_id) > launches.dur * 0.15

        UNION ALL
        SELECT 'Main Thread - Time spent in interruptible sleep state'
          AS slow_cause
        WHERE main_thread_time_for_launch_and_state(launches.startup_id, 'S') > 2900e6

        UNION ALL
        SELECT 'Main Thread - Time spent in Blocking I/O'
        WHERE main_thread_time_for_launch_state_and_io_wait(launches.startup_id, 'D*', TRUE) > 450e6

        UNION ALL
        SELECT 'Main Thread - Time spent in OpenDexFilesFromOat*'
          AS slow_cause
        WHERE android_sum_dur_on_main_thread_for_startup_and_slice(
          launches.startup_id, 'OpenDexFilesFromOat*') > launches.dur * 0.2

        UNION ALL
        SELECT 'Time spent in bindApplication'
          AS slow_cause
        WHERE android_sum_dur_for_startup_and_slice(launches.startup_id, 'bindApplication') > 1250e6

        UNION ALL
        SELECT 'Time spent in view inflation'
          AS slow_cause
        WHERE android_sum_dur_for_startup_and_slice(launches.startup_id, 'inflate') > 450e6

        UNION ALL
        SELECT 'Time spent in ResourcesManager#getResources'
          AS slow_cause
        WHERE android_sum_dur_for_startup_and_slice(
          launches.startup_id, 'ResourcesManager#getResources') > 130e6

        UNION ALL
        SELECT 'Time spent verifying classes'
          AS slow_cause
        WHERE
          android_sum_dur_for_startup_and_slice(launches.startup_id, 'VerifyClass*')
            > launches.dur * 0.15

        UNION ALL
        SELECT 'Potential CPU contention with another process' AS slow_cause
        WHERE main_thread_time_for_launch_in_runnable_state(launches.startup_id) > 100e6
          AND most_active_process_for_launch(launches.startup_id) IS NOT NULL

        UNION ALL
        SELECT 'JIT Activity'
          AS slow_cause
        WHERE thread_time_for_launch_state_and_thread(
          launches.startup_id,
          'Running',
          'Jit thread pool'
        ) > 100e6

        UNION ALL
        SELECT 'Main Thread - Lock contention'
          AS slow_cause
        WHERE android_sum_dur_on_main_thread_for_startup_and_slice(
          launches.startup_id,
          'Lock contention on*'
        ) > launches.dur * 0.2

        UNION ALL
        SELECT 'Main Thread - Monitor contention'
          AS slow_cause
        WHERE android_sum_dur_on_main_thread_for_startup_and_slice(
          launches.startup_id,
          'Lock contention on a monitor*'
        ) > launches.dur * 0.15

        UNION ALL
        SELECT 'JIT compiled methods'
        WHERE (
          SELECT COUNT(1)
          FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(launches.startup_id, 'JIT compiling*')
          WHERE thread_name = 'Jit thread pool'
        ) > 65

        UNION ALL
        SELECT 'Broadcast dispatched count'
        WHERE count_slices_concurrent_to_launch(
          launches.startup_id,
          'Broadcast dispatched*'
        ) > 15

        UNION ALL
        SELECT 'Broadcast received count'
        WHERE count_slices_concurrent_to_launch(
          launches.startup_id,
          'broadcastReceiveReg*'
        ) > 50

        UNION ALL
        SELECT 'Startup running concurrent to launch'
        WHERE EXISTS(
          SELECT package
          FROM android_startups l
          WHERE l.startup_id != launches.startup_id
            AND is_spans_overlapping(l.ts, l.ts_end, launches.ts, launches.ts_end)
        )

        UNION ALL
        SELECT 'Main Thread - Binder transactions blocked'
        WHERE (
          SELECT COUNT(1)
          FROM BINDER_TRANSACTION_REPLY_SLICES_FOR_LAUNCH(launches.startup_id, 2e7)
        ) > 0

      )
    )
  ) AS startup
FROM android_startups launches;

DROP VIEW IF EXISTS android_startup_event;
CREATE VIEW android_startup_event AS
SELECT
  'slice' AS track_type,
  'Android App Startups' AS track_name,
  l.ts AS ts,
  l.dur AS dur,
  l.package AS slice_name
FROM android_startups l;

DROP VIEW IF EXISTS android_startup_output;
CREATE VIEW android_startup_output AS
SELECT
  AndroidStartupMetric(
    'startup', (
      SELECT RepeatedField(startup) FROM startup_view
    )
  );
