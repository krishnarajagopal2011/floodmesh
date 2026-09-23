# First range test: 350 m, no line of sight

Tags: #trials #feedback
Channels: X (single post plus one reply), Instagram reel
Visual: phone clip from the car of the OLED showing the received alarm, then
a map with the two points marked. If there is no clip, a photo of the unit on
the dashboard and the map.

## Facts

- Sender in a moving car, receiver fixed, no line of sight. Test notes.
- Alarms received to about 350 m, nothing past that. Test notes.
- RSSI not logged. Test notes.
- Radio: 866.5 MHz, SF7, BW 125 kHz. README.
- Next steps: log RSSI, 22 dBm, SF8 selectable. Range roadmap decisions.

## X

#trials #feedback

We are building an off-grid pager for floods. First range test today, and it
was humbling.

Sender in a car, receiver fixed, no line of sight through a built-up area.
Alarms landed out to about 350 m. Nothing past that.

Stock antenna, 866 MHz, SF7, 125 kHz. No RSSI logged, which was a mistake.

[clip or photo]

**Reply:**
Next: log RSSI per packet, try 22 dBm, and test SF8. Anyone who has pushed
LoRa through Indian apartment blocks, what worked for you?

github.com/krishnarajagopal2011/floodmesh

## Instagram reel script (30 s)

- 0 to 2 s, text on screen: "350 m. Then nothing."
- 2 to 8 s: unit on the dashboard, OLED showing the received alarm, road
  passing outside. Text: "First range test. No line of sight."
- 8 to 16 s: map with both points and the 350 m line. Text: "Alarms landed
  to 350 m through buildings. Past that, silence."
- 16 to 24 s: close-up of the board. Text: "Stock antenna. SF7. We did not
  log signal strength. Lesson learned."
- 24 to 30 s: text: "Next: log RSSI, more power, SF8. Results here, good or
  bad." End card with the GitHub link.

## Instagram caption

#trials #feedback

We are building an off-grid pager for floods. First range test: about 350 m
with no line of sight, then nothing.

That is far short of what it needs to be. The next round logs signal
strength on every packet, raises transmit power to the legal limit, and
tries a slower spreading factor. Every result gets posted.

If you have pushed LoRa through dense Indian apartment blocks, what antenna
and settings worked for you?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware, built in public: link in bio.

#floodmesh #lora #rangetest #meshnetwork #disastertech #openhardware #esp32 #buildinpublic #chennai
