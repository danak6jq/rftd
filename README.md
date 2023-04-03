# rftd
RAK10701 Field Tester daemon for ChirpStack v3/v4

Runs on ChirpStack server using MQTT to access uplinks from RAK10701 in "Mapper-App"

Requires paho.mqtt.c

Definitely a science-fair experiment at the moment.

Getting started:

- Install paho.mqtt.c on build/target system
- Build rftd with 'sh make.sh'
- Create "Mapper-App" application (exactly this name) on ChirpStack
- Create device-profile for RAK10701 using RAK-provided decoder
- Create RAK10701 devices in Mapper-App using RAK10701 device-profile
- Run rftd (currently with 'nohup rftd &'; make into systemd service)

Things to do:
- Parameterize (MQTT URI at least)
~~- Figure out how to properly sleep~~
- Make into systemd service with proper logging
- Link the cJSON repo rather than include the files

