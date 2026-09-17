## Task: Improve GPS speed accuracy, stationary detection and GPX logging

The current tripTracker receives GPS updates at approximately 10 Hz. The displayed speed is not stable enough and the GPX logging appears to write multiple trackpoints per second.

Analysis of a recorded GPX file showed:

- The GPS itself provides reasonably stable speed values while driving.
- During a constant-speed section with cruise control set to approximately 95 km/h, GPS speed was generally around 90–92 km/h. This difference is acceptable and does NOT need calibration.
- The display sometimes varies much more than expected.
- When stationary, tripTracker can still display approximately 6–8 km/h.
- The GPX file contains multiple `<trkpt>` entries with identical timestamps.
- GPS acquisition, speed processing, display updates and GPX logging therefore need to be clearly separated.

Do NOT redesign the entire application. Modify the existing implementation with the smallest reasonable changes.

### 1. GPS acquisition must remain at full rate

Process every valid GPS update received from the GPS module.

The GPS currently provides approximately 10 updates per second.

Do NOT reduce GPS parsing to 1 Hz.

Internally, latitude, longitude, speed, course and fix information should therefore continue to be updated at the full GPS rate.

### 2. Use GPS Speed Over Ground

For the current vehicle speed, use the speed reported directly by the GNSS receiver (Speed Over Ground / SOG), for example from the NMEA RMC data.

Do NOT calculate the displayed speed from:

    distance between consecutive latitude/longitude positions / elapsed time

GPS SOG is normally much more accurate for this purpose than position-derived speed.

Before making changes, inspect the existing code and determine exactly where the currently displayed speed comes from.

If tripTracker already uses GPS SOG, keep that source and apply the filtering described below.

### 3. Add speed filtering

Process every valid GPS speed sample at the GPS update rate.

Add a simple low-pass/IIR filter:

    filteredSpeed += alpha * (rawGpsSpeed - filteredSpeed)

Start with:

    alpha = 0.25

All speeds used in this calculation must use the same unit internally. Prefer m/s internally if that is already how the GPS library represents speed, otherwise km/h is acceptable.

Do not round values during filtering.

Only round when formatting the value for the display.

The display must use `filteredSpeed`, NOT an independently calculated speed.

### 4. Handle initialization correctly

Do not initialize the filter by slowly ramping from zero when the first valid GPS speed becomes available.

For the first valid speed sample:

    filteredSpeed = rawGpsSpeed

After that, apply the IIR filter normally.

Also reset/reinitialize the filter appropriately after GPS fix loss if the existing architecture requires this.

### 5. Add stationary detection

GPS position jitter must not cause a non-zero displayed speed while the device is stationary.

Implement stationary detection with hysteresis.

Initial values:

    stationary threshold: 2.0 km/h
    stationary time:      3 seconds

If GPS speed remains below 2.0 km/h for at least 3 seconds:

    stationary = true
    displayed speed = 0.0 km/h

To leave stationary mode:

    moving threshold: 3.0 km/h
    moving time:      1 second

If GPS speed remains above 3.0 km/h for at least 1 second:

    stationary = false

Use hysteresis deliberately. Do not switch state continuously around a single threshold.

The stationary/moving decision should preferably use raw GPS SOG or a very lightly filtered GPS SOG, rather than latitude/longitude displacement.

### 6. Do not accumulate distance while stationary

When:

    stationary == true

GPS position jitter must NOT be added to the trip distance.

Do not accumulate distance merely because latitude/longitude changed slightly.

When transitioning back to moving, make sure that the displacement accumulated during the stationary period is not suddenly added as one large distance increment.

Reset/rebase the previous distance reference position when leaving stationary mode if necessary.

### 7. Separate GPS processing from GPX logging

GPS processing and GPX logging are different tasks.

Required behaviour:

    GPS acquisition/parsing:   approximately 10 Hz
    speed processing/filter:   approximately 10 Hz
    display update:            independent
    GPX logging:               maximum 1 trackpoint/second

Receiving a new GPS fix must NOT automatically cause a GPX `<trkpt>` to be written.

Introduce or use a separate GPX logging timer.

Write at most one trackpoint per second.

### 8. Fix GPX timestamps

The analysed GPX file contains many trackpoints with exactly the same timestamp, for example multiple points at:

    08:48:00
    08:48:01
    08:48:02

This must not happen when GPX logging is configured for 1 Hz.

At 1-Hz logging there should normally be:

    08:48:00
    08:48:01
    08:48:02
    08:48:03
    ...

Do not simply suppress duplicate timestamps after creating the trackpoint.

Fix the scheduling/logging logic so only one trackpoint is generated per logging interval.

### 9. Keep the 10-Hz GPS data useful

Although GPX is logged at only 1 Hz, the intermediate GPS measurements must NOT be discarded.

They are useful for:

- speed filtering;
- stationary detection;
- determining current position;
- course/heading processing;
- rejecting bad fixes.

The architecture should effectively be:

    GPS receiver (~10 Hz)
             |
             v
        GPS parser
             |
             v
      current GPS fix
             |
        +----+----------------+
        |                     |
        v                     v
    speed filter        position processing
        |
        v
    stationary detection
        |
        +----------+
        |          |
        v          v
     display     GPX logger
                  max 1 Hz

### 10. Add temporary diagnostic logging

For testing, add a compact diagnostic log approximately once per second showing at least:

    raw GPS speed
    filtered speed
    displayed speed
    stationary/moving state
    GPS fix validity
    number of GPS speed samples received during the previous second

For example:

    GPS: raw=25.42m/s filtered=25.31m/s display=91.1km/h state=MOVING samples=10 fix=VALID

and while stationary:

    GPS: raw=0.21m/s filtered=0.35m/s display=0.0km/h state=STATIONARY samples=10 fix=VALID

This logging is temporary/debug functionality and must not influence timing.

### 11. Do not add a speed calibration factor

Do NOT try to make:

    GPS 91 km/h

become:

    car speedometer 95 km/h

by multiplying the GPS speed by a correction factor.

The vehicle speedometer and GPS speed are allowed to differ.

The objective is:

- stable speed;
- accurate GPS-derived speed;
- fast enough response during acceleration/deceleration;
- 0 km/h when genuinely stationary.

### 12. Important: investigate the 6–8 km/h stationary problem

Before hiding the problem with filtering, inspect the existing code to determine where the reported 6–8 km/h while stationary originates.

Determine whether it comes from:

1. GPS SOG itself;
2. distance between consecutive coordinates;
3. an incorrectly calculated time interval;
4. stale GPS speed data;
5. display code using an old value;
6. another speed calculation elsewhere in the application.

If GPS updates stop or become invalid, an old speed value must NOT remain indefinitely on the display.

Handle stale/invalid GPS data explicitly.

### Acceptance criteria

After implementation:

- GPS input continues to be processed at approximately 10 Hz.
- Approximately 10 valid samples/sec should be visible in diagnostics when reception is good.
- GPX contains at most one trackpoint per second.
- GPX trackpoints no longer contain repeated whole-second timestamps caused by multiple writes in the same second.
- At constant driving speed, the displayed speed should be substantially more stable.
- When stationary for several seconds, the display must become exactly `0 km/h`.
- GPS drift while stationary must not increase trip distance.
- Normal acceleration and braking must still be visible without excessive lag.
- No arbitrary calibration against the vehicle speedometer is added.

First inspect the existing implementation and identify the relevant source files/functions.

Then make the changes while preserving the existing project architecture and coding style.

Finally, report:

1. which files were changed;
2. where the old displayed speed came from;
3. why 6–8 km/h could remain visible while stationary;
4. how speed filtering is now implemented;
5. how stationary detection works;
6. how GPX logging has been limited to 1 Hz;
7. any assumptions you had to make.