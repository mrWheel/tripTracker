# AT6668

The AT6668 (often included in the M5Stack Unit-GPS v1.1 or similar GNSS modules with an ATGM336H/AT6668 chip) returns NMEA 0183 (version 4.1) positioning and navigation data through a UART interface (115200 bps by default).

Data types (NMEA sentences / data output)

- Coordinates: 
    - Latitude
    - Longitude
- Altitude: Height above sea level.
- Time and Date: UTC time synchronized via satellites.
- Speed and Course: Ground movement and direction of travel.
- Satellite status: Active satellites, signal strength (SNR), and FIX status (3D/2D fix).

Supported GNSS systems

The chip receives signals from multiple constellations simultaneously:
- GPS (United States)
- GLONASS (Russia)
- GALILEO (Europe)
- BDS / BeiDou (BD2/BD3, China)
- QZSS (Japan)

## Technical Specifications

Accuracy: < 1.5 meters (CEP50)Channels: 50 channelsUpdate rate: Up to 10 HzSensitivity: -162 dBm (tracking)

The AT6668 returns data in the standard `NMEA 0183 format`. 
These are plain-text lines (known as sentences) that all begin with a $ sign and end with a checksum for error checking.

Here is an example of what a sequence of consecutive messages (a "data stream") looks like when reading the GPS directly:
```
text$GNGGA,123456.00,5213.1234,N,00514.5678,E,1,08,0.9,12.4,M,47.1,M,,*5A
$GNRMC,123456.00,A,5213.1234,N,00514.5678,E,022.4,180.5,170926,,,A*7C
$GNVTG,180.5,T,,M,022.4,N,041.5,K,A*3F
$GNGSA,A,3,01,02,03,04,05,,,,,,,,1.2,0.9,0.8*1D
```

Meaning of a message (Example: $GNRMC)The $GNRMC sentence is the most important because it contains the minimum navigation data. 
Here is the exact breakdown of the line above:
```
$GNRMC,123456.00,A,5213.1234,N,00514.5678,E,022.4,180.5,170926,,,A*7C$GN
```

This means that the data comes from multiple satellite systems combined (Global Navigation).- RMC: The message type (Recommended Minimum Navigation Information).
- 123456.00: The UTC time (12:34:56).
- A: GPS status (A = Active/Valid, V = Invalid).
- 5213.1234, N: Latitude (52° 13.1234' north latitude).
- 00514.5678, E: Longitude (05° 14.5678' east longitude).
- 022.4: Speed over ground in knots (22.4 knots).
- 180.5: Course over ground in degrees (180.5°, due south).
- 170926: The date (17 September 2026).
- *7C: The checksum (to verify that the message arrived intact).

Other common messages in the stream:

- $GNGGA: Provides the exact altitude (in meters above sea level) and the number of connected satellites.
- $GNVTG: Provides the speed directly in kilometers per hour (km/h) instead of knots.
- $GNGSA: Provides information about accuracy (DOP values) and which satellites are actively used for the calculation.