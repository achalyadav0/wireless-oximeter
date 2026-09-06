import uuid
from datetime import datetime

from sqlalchemy import DateTime, Float, ForeignKey, Integer, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.extensions import db


class PPGSample(db.Model):
    """Represents one raw PPG sample inside a recording."""

    __tablename__ = "ppg_samples"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        default=uuid.uuid4,
    )

    recording_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("ppg_recordings.id", ondelete="CASCADE"),
        nullable=False,
        index=True,
    )

    sample_index: Mapped[int] = mapped_column(
        Integer,
        nullable=False,
    )

    timestamp_offset_ms: Mapped[int] = mapped_column(
        Integer,
        nullable=False,
    )

    value: Mapped[float] = mapped_column(
        Float,
        nullable=False,
    )

    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        nullable=False,
    )

    recording: Mapped["PPGRecording"] = relationship(
        back_populates="samples",
    )