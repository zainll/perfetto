-- A simple table that checks the time between VSync (this can be used to
-- determine if we're refreshing at 90 FPS or 60 FPS).
--
-- Note: In traces without the "Java" category there will be no VSync
--       TraceEvents and this table will be empty.
DROP TABLE IF EXISTS vsync_intervals;
CREATE PERFETTO TABLE vsync_intervals AS
SELECT
  slice_id,
  ts,
  dur,
  track_id,
  LEAD(ts) OVER(PARTITION BY track_id ORDER BY ts) - ts AS time_to_next_vsync
FROM slice
WHERE name = "VSync"
ORDER BY track_id, ts;

-- Function: compute the average Vysnc interval of the
-- gesture (hopefully this would be either 60 FPS for the whole gesture or 90
-- FPS but that isnt always the case) on the given time segment.
-- If the trace doesnt contain the VSync TraceEvent we just fall back on
-- assuming its 60 FPS (this is the 1.6e+7 in the COALESCE which
-- corresponds to 16 ms or 60 FPS).
--
-- begin_ts: segment start time
-- end_ts: segment end time
CREATE PERFETTO FUNCTION calculate_avg_vsync_interval(
  begin_ts LONG,
  end_ts LONG
)
-- Returns: the average Vysnc interval on this time segment
-- or 1.6e+7, if trace doesnt contain the VSync TraceEvent.
RETURNS FLOAT AS
SELECT
  COALESCE((
    SELECT
      CAST(AVG(time_to_next_vsync) AS FLOAT)
    FROM vsync_intervals in_query
    WHERE
      time_to_next_vsync IS NOT NULL AND
      in_query.ts > $begin_ts AND
      in_query.ts < $end_ts
  ), 1e+9 / 60);
