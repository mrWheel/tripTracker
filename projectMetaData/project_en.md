# tripTracker

A native ESP-IDF trip computer built for the M5Stack Core Basic with the M5Stack GPS module. It shows real-time speed and trip distance on large seven-segment style digits, processes GPS fixes at 10 Hz, and switches between SPEED and TRIP display modes.

Every trip is automatically logged to the SD card as GPX and CSV files, including position, altitude, speed, course, satellite count and cumulative distance. A built-in WiFi webserver with a browser-based file manager lets you download, delete and review recorded trips - including distance, duration and average speed - directly from your phone or computer.
