from app import create_app
from app.mqtt.client import MQTTClient
from app.mqtt.handlers import handle_vitals_message


def main():
    app = create_app()

    with app.app_context():
        mqtt_client = MQTTClient(
            message_handler=handle_vitals_message,
        )
        mqtt_client.start()


if __name__ == "__main__":
    main()