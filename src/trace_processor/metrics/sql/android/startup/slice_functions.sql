--
-- Copyright 2022 The Android Open Source Project
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

SELECT IMPORT('android.startup.startups');

-- Helper function to build a Slice proto from a duration.
CREATE PERFETTO FUNCTION startup_slice_proto(dur INT)
RETURNS PROTO AS
SELECT AndroidStartupMetric_Slice(
  "dur_ns", $dur,
  "dur_ms", $dur / 1e6
);

-- View containing all the slices for all launches. Generally, this view
-- should not be used. Instead, one of the helper functions below which wrap
-- this view should be used.
DROP VIEW IF EXISTS thread_slices_for_all_launches;
CREATE VIEW thread_slices_for_all_launches AS
SELECT * FROM android_thread_slices_for_all_startups;

-- Given a launch id and GLOB for a slice name, returns the startup slice proto,
-- summing the slice durations across the whole startup.
CREATE PERFETTO FUNCTION dur_sum_slice_proto_for_launch(startup_id LONG, slice_name STRING)
RETURNS PROTO AS
SELECT NULL_IF_EMPTY(
  startup_slice_proto(
    android_sum_dur_for_startup_and_slice($startup_id, $slice_name)
  )
);

-- Same as |dur_sum_slice_proto_for_launch| except only counting slices happening
-- on the main thread.
CREATE PERFETTO FUNCTION dur_sum_main_thread_slice_proto_for_launch(startup_id LONG, slice_name STRING)
RETURNS PROTO AS
SELECT NULL_IF_EMPTY(
  startup_slice_proto(
    android_sum_dur_on_main_thread_for_startup_and_slice($startup_id, $slice_name)
  )
);

-- Given a launch id and GLOB for a slice name, returns the startup slice proto by
-- taking the duration between the start of the launch and start of the slice.
-- If multiple slices match, picks the latest one which started during the launch.
CREATE PERFETTO FUNCTION launch_to_main_thread_slice_proto(startup_id INT, slice_name STRING)
RETURNS PROTO AS
SELECT NULL_IF_EMPTY(startup_slice_proto(MAX(slice_ts) - startup_ts))
FROM android_thread_slices_for_all_startups s
JOIN thread t USING (utid)
WHERE
  s.slice_name GLOB $slice_name AND
  s.startup_id = $startup_id AND
  s.is_main_thread AND
  (t.end_ts IS NULL OR t.end_ts >= s.startup_ts_end);

-- Given a lauch id, returns the total time spent in GC
CREATE PERFETTO FUNCTION total_gc_time_by_launch(startup_id LONG)
RETURNS INT AS
SELECT SUM(slice_dur)
FROM android_thread_slices_for_all_startups slice
WHERE
  slice.startup_id = $startup_id AND
  (
    slice_name GLOB "*semispace GC" OR
    slice_name GLOB "*mark sweep GC" OR
    slice_name GLOB "*concurrent copying GC"
  );

-- Given a launch id and package name, returns if baseline or cloud profile is missing.
CREATE PERFETTO FUNCTION missing_baseline_profile_for_launch(startup_id LONG, pkg_name STRING)
RETURNS BOOL AS
SELECT (COUNT(slice_name) > 0)
FROM (
  SELECT *
  FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(
    $startup_id,
    "location=* status=* filter=* reason=*"
  )
  ORDER BY slice_name
)
WHERE
  -- when location is the package odex file and the reason is "install" or "install-dm",
  -- if the compilation filter is not "speed-profile", baseline/cloud profile is missing.
  SUBSTR(STR_SPLIT(slice_name, " status=", 0), LENGTH("location=") + 1)
    GLOB ("*" || $pkg_name || "*odex")
  AND (STR_SPLIT(slice_name, " reason=", 1) = "install"
    OR STR_SPLIT(slice_name, " reason=", 1) = "install-dm")
  AND STR_SPLIT(STR_SPLIT(slice_name, " filter=", 1), " reason=", 0) != "speed-profile";

-- Given a launch id, returns if there is a main thread run-from-apk slice.
-- Add an exception if "split_config" is in parent slice's name(b/277809828).
-- TODO: remove the exception after Sep 2023 (b/78772867)
CREATE PERFETTO FUNCTION run_from_apk_for_launch(launch_id LONG)
RETURNS BOOL AS
SELECT EXISTS(
  SELECT slice_name, startup_id, is_main_thread
  FROM android_thread_slices_for_all_startups s
  JOIN slice ON slice.id = s.slice_id
  LEFT JOIN slice parent ON slice.parent_id = parent.id
  WHERE
    startup_id = $launch_id AND is_main_thread AND
    slice_name GLOB "location=* status=* filter=* reason=*" AND
    STR_SPLIT(STR_SPLIT(slice_name, " filter=", 1), " reason=", 0)
      GLOB ("*" || "run-from-apk" || "*") AND
    (parent.name IS NULL OR
      parent.name NOT GLOB ("OpenDexFilesFromOat(*split_config*apk)"))
);

CREATE PERFETTO FUNCTION summary_for_optimization_status(
  loc STRING,
  status STRING,
  filter_str STRING,
  reason STRING
)
RETURNS STRING AS
SELECT
CASE
  WHEN
    $loc GLOB "*/base.odex" AND $loc GLOB "*==/*-*"
  THEN STR_SPLIT(STR_SPLIT($loc, "==/", 1), "-", 0) || "/.../"
  ELSE ""
END ||
CASE
  WHEN $loc GLOB "*/*"
    THEN REVERSE(STR_SPLIT(REVERSE($loc), "/", 0))
  ELSE $loc
END || ": " || $status || "/" || $filter_str || "/" || $reason;

SELECT CREATE_VIEW_FUNCTION(
  'BINDER_TRANSACTION_REPLY_SLICES_FOR_LAUNCH(startup_id INT, threshold DOUBLE)',
  'name STRING',
  '
    SELECT reply.name AS name
    FROM ANDROID_BINDER_TRANSACTION_SLICES_FOR_STARTUP($startup_id, $threshold) request
    JOIN following_flow(request.id) arrow
    JOIN slice reply ON reply.id = arrow.slice_in
    WHERE reply.dur > $threshold AND request.is_main_thread
  '
);

-- Given a launch id, return if unlock is running by systemui during the launch.
CREATE PERFETTO FUNCTION is_unlock_running_during_launch(startup_id LONG)
RETURNS BOOL AS
SELECT EXISTS(
  SELECT slice.name
  FROM slice, android_startups launches
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread USING(utid)
  JOIN process USING(upid)
  WHERE launches.startup_id = $startup_id
  AND slice.name = "KeyguardUpdateMonitor#onAuthenticationSucceeded"
  AND process.name = "com.android.systemui"
  AND slice.ts >= launches.ts
  AND (slice.ts + slice.dur) <= launches.ts_end
);
