import json
import os

from flask import Flask, jsonify, render_template

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TEMPLATE_DIR = os.path.join(BASE_DIR, "app", "templates")
DATA_FILE = os.path.join(BASE_DIR, "latest_devices.json")

app = Flask(__name__, template_folder=TEMPLATE_DIR)


@app.get("/")
def dashboard():
    return render_template("index.html")


@app.get("/api/devices")
def get_devices():
    if not os.path.exists(DATA_FILE):
        return jsonify({})

    try:
        with open(DATA_FILE, "r") as file:
            data = json.load(file)
    except (json.JSONDecodeError, OSError):
        return jsonify({})

    return jsonify(data)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
