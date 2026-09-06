import uuid
from datetime import datetime

from sqlalchemy import DateTime, ForeignKey, String, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.extensions import db


class Observation(db.Model):
    """Represents a physiological measurement event."""

    __tablename__ = "observations"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        default=uuid.uuid4,
    )

    patient_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("patients.id"),
        nullable=False,
        index=True,
    )

    sensor_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("sensors.id"),
        nullable=False,
        index=True,
    )

    measurement_type_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("measurement_types.id"),
        nullable=False,
        index=True,
    )

    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        index=True,
    )

    quality: Mapped[str | None] = mapped_column(
        String(30),
        nullable=True,
    )

    source: Mapped[str] = mapped_column(
        String(30),
        nullable=False,
        default="DEVICE",
    )

    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        nullable=False,
    )

    patient: Mapped["Patient"] = relationship(
        back_populates="observations",
    )

    sensor: Mapped["Sensor"] = relationship(
        back_populates="observations",
    )

    measurement_type: Mapped["MeasurementType"] = relationship(
        back_populates="observations",
    )

    value: Mapped["ObservationValue | None"] = relationship(
        back_populates="observation",
        uselist=False,
        cascade="all, delete-orphan",
    )

    alerts: Mapped[list["Alert"]] = relationship(
        back_populates="observation",
    )