# ESP8266-Based Lock with Home Assistant and RFID Support

## Project Description

This project is a smart lock based on the ESP8266 microcontroller. It integrates with the **Home Assistant** smart home system, allowing control through its interface. Additionally, the lock supports unlocking with RFID cards via a built-in RFID module with an antenna.

## Key Features

- **Integration with Home Assistant** for controlling the lock via a mobile app or web interface.
- **MQTT Support** for interaction with other smart home systems.
- **RFID Card Unlocking** for convenient access.
- **Remote Control** for locking and unlocking over the internet.
- **Custom Automation Scenarios** for tailored smart home functionality.

## Components

- **ESP8266**: The microcontroller managing the lock's operations.
- **KY-012**: Simple buzzer for sound feedback. 
- **125Khz RFID Reader with antenna**: For reading RFID card data for authentication.
- **LY-03**: Solenoid locker for 12v which operates over a relay.
- **SRD-05VDC-SL-C**: A simple 5v relay (you can use any that operates under 3-12v).
- **Home Assistant**: The smart home automation system.
- **MQTT Broker**: For message exchange between devices.

## Installation and Setup

### 1. Assemble the Device
- Connect the RFID module to the ESP8266 according to the wiring diagram.
- Ensure the device is powered according to the required specifications.

### 2. Flash the ESP8266
1. Download the project repository.
2. Setup your personal data within sketch file:
   - Your Wi-Fi network credentials (L28-L29).
   - The MQTT broker address (L33-L36).
3. Flash the firmware to the ESP8266 using **Arduino IDE** or **PlatformIO**.
4. Long single beep will indicate that WiFi and MQTT broker has been connected successfully.
5. The first RFID card or chip will be remembered as a master.

### 3. Integrate with Home Assistant
- Start Home Assistant.
- Install Mosquitto with the MQTT broker and start it.
- Add MQTT integration which connects to your broker.
- Start the ESP device.
- The device will send a discovery message and a new device will appear in your HA interface.

### 4. Configure RFID Cards
- Register cards by touching the antenna with your card or implanted chip.
- Add unique card IDs for authorization via Home Assistant interface.

## Usage
Control the lock via:
- The Home Assistant interface.
- Automation scenarios (e.g., unlock when arriving home).
- By presenting an RFID card or implanted chip to the module.

## Requirements
- 12v with a DC power supplier to 5v output or a direct USB 5v output.
- ESP8266 (NodeMCU or equivalent with WiFi).
- 125Khz RFID Reader module (or you can use your own).
- Home Assistant with a configured MQTT broker.
- Wi-Fi Network.

## Notes
- Ensure the device is protected against unauthorized access.
- Regularly update the firmware to fix potential vulnerabilities.

## License
This project is licensed under the Apache 2.0 License.

## Contact
If you have questions or suggestions, feel free to open an Issue in the repository.
