import os

import paho.mqtt.client as mqtt


class MQTTClient:
    """Handles the connection between the Flask backend and MQTT broker."""

    def __init__(self, message_handler):
        self.broker_host = os.getenv(
            "MQTT_BROKER_HOST",
            "localhost",
        )
        self.broker_port = int(
            os.getenv(
                "MQTT_BROKER_PORT",
                "1883",
            )
        )

        self.topic = os.getenv(
            "MQTT_VITALS_TOPIC",
            "oximeter/devices/+/vitals",
        )

        self.message_handler = message_handler

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="wireless-oximeter-backend",
        )

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        """Called when the backend connects to the MQTT broker."""

        print(
            f"Connected to MQTT broker: "
            f"{self.broker_host}:{self.broker_port}"
        )

        client.subscribe(self.topic)

        print(f"Subscribed to: {self.topic}")

    def _on_message(self, client, userdata, message):
        """Called whenever an MQTT message is received."""

        payload = message.payload.decode("utf-8")

        print()
        print("MQTT message received")
        print(f"Topic: {message.topic}")
        print(f"Payload: {payload}")

        self.message_handler(
            message.topic,
            payload,
        )

    def start(self):
        """Connect to MQTT broker and start the network loop."""

        print(
            f"Connecting to MQTT broker "
            f"{self.broker_host}:{self.broker_port}"
        )

        self.client.connect(
            self.broker_host,
            self.broker_port,
            keepalive=60,
        )

        self.client.loop_forever()