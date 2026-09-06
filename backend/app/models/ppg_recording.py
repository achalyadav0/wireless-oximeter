import uuid
from datetime import datetime

from sqlalchemy import DateTime, Float, ForeignKey, Integer, String, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.extensions import db


class PPGRecording(db.Model):
    """Represents a captured segment of raw PPG data."""

    __tablename__ = "ppg_recordings"

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

    started_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        index=True,
    )

    duration_ms: Mapped[int] = mapped_column(
        Integer,
        nullable=False,
    )

    sample_rate_hz: Mapped[float] = mapped_column(
        Float,
        nullable=False,
    )

    channel: Mapped[str] = mapped_column(
        String(20),
        nullable=False,
        default="RED",
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        nullable=False,
    )
    patient: Mapped["Patient"] = relationship(
        back_populates="ppg_recordings",
    )
    sensor: Mapped["Sensor"] = relationship(
        back_populates="ppg_recordings",
    )
    samples: Mapped[list["PPGSample"]] = relationship(
        back_populates="recording",
        cascade="all, delete-orphan",
        order_by="PPGSample.sample_index",
    )
    inferences: Mapped[list["MLInference"]] = relationship(
        back_populates="recording",
    )