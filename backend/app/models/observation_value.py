import uuid
from datetime import datetime

from sqlalchemy import DateTime, Float, Integer, ForeignKey, Text, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.extensions import db


class ObservationValue(db.Model):
    """Stores the actual value associated with an observation."""

    __tablename__ = "observation_values"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        default=uuid.uuid4,
    )

    observation_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("observations.id", ondelete="CASCADE"),
        nullable=False,
        unique=True,
        index=True,
    )

    numeric_value: Mapped[float | None] = mapped_column(
        Float,
        nullable=True,
    )

    integer_value: Mapped[int | None] = mapped_column(
        Integer,
        nullable=True,
    )

    text_value: Mapped[str | None] = mapped_column(
        Text,
        nullable=True,
    )

    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        nullable=False,
    )

    observation: Mapped["Observation"] = relationship(
        back_populates="value",
    )