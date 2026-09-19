# Task: redesign tripTracker speed processing for an accurate 3–150 km/h GPS reference speedometer

The tripTracker is not only a trip logger. It must also function as an accurate GPS reference speedometer that can be used to check/calibrate a car speedometer.

The GNSS receiver supplies approximately 10 speed samples per second.

The GPX file is intentionally logged at a lower rate, but ALL GNSS speed samples must be used internally.

Do not calculate speed from latitude/longitude differences. Continue using GNSS Speed Over Ground (`gps->speed_kmh`).

## Main problem

The existing single IIR filter with alpha=0.25 is not sufficient.

At constant cruise-control speed the actual vehicle speed varies by less than approximately 1 km/h, but the displayed GPS speed can vary by approximately 5 km/h.

At standstill the display can also remain at several km/h for up to 30 seconds.

The current stationary algorithm requires raw speed to remain continuously below the threshold. A single noisy sample above the threshold resets the timer. Replace this behaviour.

## 1. Keep a rolling GNSS speed sample buffer

Maintain approximately 3 seconds of valid GNSS SOG samples.

At 10 Hz this is approximately:

    30 samples

Do not assume exactly 10 Hz. Use sample timestamps where appropriate.

## 2. Reject outliers before filtering

Do not directly feed every raw sample into the displayed-speed filter.

Calculate a robust speed estimate from the recent samples.

Use a median-based method so that isolated GNSS speed spikes cannot significantly change the displayed speed.

A suitable implementation is:

    median = median of recent short-term samples

Reject samples whose deviation from the median is clearly inconsistent with normal vehicle acceleration.

Do not blindly reject real acceleration or braking.

Keep the implementation simple and deterministic for ESP32.

## 3. Produce two speed values

Maintain:

    instant_speed_kmh
    reference_speed_kmh

`instant_speed_kmh` is intended to react reasonably quickly during acceleration and braking.

`reference_speed_kmh` is intended to be extremely stable when the vehicle is travelling at constant speed and is the value shown on the normal speedometer display.

The reference speed must be based on multiple GNSS SOG samples, not a single sample.

## 4. Adaptive filtering

Do not use one fixed filter response over the entire 3–150 km/h range.

Use less filtering at low speeds and progressively stronger filtering at normal road speeds.

Target behaviour:

    3–15 km/h:
        quick response

    15–50 km/h:
        moderate filtering

    50–150 km/h:
        strong filtering

At motorway speed, stability is more important than sub-second response.

A 2–3 second settling time after a real speed change is acceptable.

During constant cruise-control driving, the objective is that the reference speed varies as little as reasonably possible, preferably only a few tenths of a km/h when GNSS data quality permits.

Do NOT artificially quantize the result to whole km/h.

Keep the internal value as float and allow at least 0.1 km/h resolution.

## 5. Detect constant-speed driving

Add detection for a stable-speed condition.

For example, examine the robust GNSS speed estimates over the recent 2–3 seconds.

If their spread/standard deviation is small and there is no consistent acceleration/deceleration trend:

    speed_stable = true

When `speed_stable == true`, apply stronger smoothing to `reference_speed_kmh`.

When genuine acceleration or braking is detected, temporarily reduce smoothing so the displayed speed follows the vehicle promptly.

The algorithm must distinguish:

    random GNSS speed noise

from:

    a consistent sequence of increasing/decreasing speeds

Do not interpret every individual speed change as acceleration.

## 6. Completely redesign stationary detection

Do NOT require every raw GPS speed sample to remain continuously below 2 km/h for three seconds.

Instead use the recent speed sample window.

For example, classify the unit as stationary when, during approximately the last 2 seconds:

    median speed < 1.5 km/h

OR

    at least 80% of samples are below 2.0 km/h

The exact constants should be defined in one place so they can easily be tuned after road testing.

When stationary is detected:

    stationary = true
    instant_speed_kmh = 0
    reference_speed_kmh = 0
    display_speed_kmh = 0

Also reset/reinitialize the speed filters to zero.

Do not allow an old filtered value to decay slowly toward zero.

## 7. Moving hysteresis

Do not leave stationary mode because of one noisy GPS sample.

Require convincing movement, for example several consecutive/majority samples above approximately 3 km/h.

Once genuine movement has been established:

    stationary = false

Reinitialize the speed filter from the current robust GNSS speed estimate so the display does not slowly ramp up from zero.

## 8. GPS stale handling

Retain the existing stale-GPS protection.

If valid GPS speed updates stop, the old speed must never remain indefinitely on screen.

## 9. Reference-speed display

The primary speedometer display must show:

    reference_speed_kmh

This value is intended for checking a vehicle speedometer.

Keep at least 0.1 km/h internal/display resolution if the UI has sufficient space.

Do NOT add any correction factor to make GPS speed agree with the car speedometer.

If GPS reports 91.2 km/h while the vehicle speedometer indicates 95 km/h, display 91.2 km/h.

That difference is exactly what the tripTracker is intended to measure.

## 10. Diagnostics

Extend diagnostics to print once per second:

    raw GNSS SOG
    median/robust speed
    instant speed
    reference speed
    displayed speed
    stationary/moving
    stable/not-stable
    number of GNSS samples
    satellites/fix status

Example:

    GPS raw=91.7 robust=91.3 instant=91.4 reference=91.2 display=91.2 MOVING STABLE samples=10 sats=14

This information is important for tuning the filter after a road test.

## 11. Preserve trip logging

Do not unnecessarily modify unrelated GPX/trip functionality.

Speed filtering for the display and GPX logging frequency are separate concerns.

Continue processing every GNSS speed update even if only one GPX trackpoint per second is stored.

## Acceptance tests

### Stationary test

After stopping:

- display should reach exactly 0 km/h within approximately 2–3 seconds;
- occasional GNSS noise must not restart the displayed speed;
- no 20–30 second decay toward zero is acceptable.

### 5 km/h walking test

At a genuine steady speed around 5 km/h:

- do not force the value to zero;
- speed should remain responsive;
- stationary detection must not suppress genuine walking.

### 50 km/h cruise-control test

At constant vehicle speed:

- reference speed should be significantly more stable than the current implementation;
- random sample-to-sample GNSS variations should be strongly suppressed.

### 90–100 km/h cruise-control test

At constant cruise-control speed:

- reference speed should settle within a few seconds;
- once settled, aim for variations of only a few tenths km/h when GNSS reception permits;
- real changes in cruise-control speed must still become visible within approximately 2–3 seconds.

### 130–150 km/h

Use the same high-speed filtering principles. Do not introduce any special calibration or scaling.

## Important

Before changing the code, inspect the complete existing GPS data path and determine:

1. the exact GNSS receiver update rate;
2. whether `gps->speed_kmh` really contains GNSS SOG;
3. whether every 10-Hz GNSS speed sample reaches `speedometer_update()`;
4. whether the display uses `display_speed_kmh` directly;
5. whether any other code performs additional speed averaging or rounding.

Do not guess these points.

Implement the changes with the smallest reasonable modification to the existing architecture and coding style.

After implementation, explain exactly:

- what caused the unstable speed;
- what caused the slow/failed transition to 0 km/h;
- how the new robust speed estimate works;
- how constant-speed detection works;
- how stationary detection works;
- all tuning constants and where they are defined.