from flask import Flask, render_template
from sqlalchemy import text

from app.config import Config
from app.extensions import db, migrate
from app.dashboard.state import get_devices




def create_app(config_class=Config):
    """Create and configure the Flask application."""

    app = Flask(__name__)

    app.config.from_object(config_class)

    db.init_app(app)
    migrate.init_app(app,db,directory=app.config["MIGRATION_DIRECTORY"],)

    from app import models

    @app.get("/health")
    def health_check():
        """Check application and database health."""

        try:
            db.session.execute(text("SELECT 1"))
            database_status = "connected"
        except Exception:
            database_status = "disconnected"

        return {
            "status": "ok",
            "service": "wireless-oximeter-backend",
            "database": database_status,
        }


    @app.get("/api/devices")
    def devices_api():
        return get_devices()

    @app.get("/")
    def dashboard():
        return render_template("index.html")

    return app