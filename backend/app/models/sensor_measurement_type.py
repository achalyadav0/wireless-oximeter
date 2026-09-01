from sqlalchemy import Column, ForeignKey, Table
from sqlalchemy.dialects.postgresql import UUID

from app.extensions import db


sensor_measurement_types = Table(
    "sensor_measurement_types",
    db.metadata,

    Column(
        "sensor_id",
        UUID(as_uuid=True),
        ForeignKey("sensors.id", ondelete="CASCADE"),
        primary_key=True,
    ),

    Column(
        "measurement_type_id",
        UUID(as_uuid=True),
        ForeignKey("measurement_types.id", ondelete="CASCADE"),
        primary_key=True,
    ),
)