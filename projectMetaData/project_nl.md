# tripTracker

Een native ESP-IDF tripcomputer voor de M5Stack Core Basic met de M5Stack GPS-module. De actuele snelheid en de afgelegde afstand worden getoond op grote zevensegments-cijfers, GPS-fixes worden verwerkt met 10 Hz en er kan geschakeld worden tussen de SPEED- en TRIP-weergave.

Elke rit wordt automatisch opgeslagen op de SD-kaart als GPX- en CSV-bestand, met onder andere positie, hoogte, snelheid, koers, aantal satellieten en de cumulatieve afstand. Een ingebouwde WiFi-webserver met een browsergebaseerde bestandsbeheerder maakt het mogelijk opgenomen ritten - inclusief afstand, duur en gemiddelde snelheid - rechtstreeks vanaf telefoon of computer te downloaden, te verwijderen en te bekijken.
