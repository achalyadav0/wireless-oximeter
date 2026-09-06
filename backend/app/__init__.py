from flask import Flask
from sqlalchemy import text

from app.config import Config
from app.extensions import db, migrate




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
        

    return app