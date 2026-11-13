"""Camada de banco de dados (SQLAlchemy).

- Em produção (Render) usa o PostgreSQL definido em DATABASE_URL.
- Localmente, sem DATABASE_URL, usa um arquivo SQLite (lampada_iot.db).
"""
import os
from datetime import datetime, timedelta, timezone

from sqlalchemy import Boolean, Column, DateTime, Integer, String, create_engine, delete
from sqlalchemy.orm import Session, declarative_base, sessionmaker


def _montar_url() -> str:
    url = os.getenv("DATABASE_URL", "sqlite:///./lampada_iot.db")
    # O Render entrega "postgres://..." / "postgresql://..."; o SQLAlchemy precisa do driver explícito.
    for prefixo in ("postgres://", "postgresql://"):
        if url.startswith(prefixo):
            return "postgresql+psycopg://" + url[len(prefixo):]
    return url


DATABASE_URL = _montar_url()
_connect_args = {"check_same_thread": False} if DATABASE_URL.startswith("sqlite") else {}
engine = create_engine(DATABASE_URL, connect_args=_connect_args, pool_pre_ping=True)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)
Base = declarative_base()


def agora_utc() -> datetime:
    return datetime.now(timezone.utc)


class Leitura(Base):
    """Uma leitura enviada pelo ESP32."""

    __tablename__ = "leituras"

    id = Column(Integer, primary_key=True)
    timestamp = Column(DateTime(timezone=True), nullable=False, default=agora_utc, index=True)
    recebido_em = Column(DateTime(timezone=True), nullable=False, default=agora_utc)
    dispositivo = Column(String(64), nullable=False, default="esp32-lampada")
    ldr1 = Column(Integer, nullable=False)          # 0..4095 (ADC 12 bits do ESP32)
    ldr2 = Column(Integer, nullable=False)          # 0..4095
    refletancia = Column(Boolean, nullable=False)   # True = objeto/superfície detectada


class Configuracao(Base):
    """Comandos/configurações vindos do painel do Arduino IoT Cloud (chave -> valor)."""

    __tablename__ = "configuracoes"

    nome = Column(String(64), primary_key=True)
    valor = Column(String(255), nullable=False)


# Valores iniciais das configurações. Todas são booleanas por enquanto.
CONFIG_PADRAO = {"leitura_ativa": True}


def criar_tabelas() -> None:
    Base.metadata.create_all(engine)
    with SessionLocal() as db:
        for nome, valor in CONFIG_PADRAO.items():
            if db.get(Configuracao, nome) is None:
                db.add(Configuracao(nome=nome, valor=_para_texto(valor)))
        db.commit()


def _para_texto(valor) -> str:
    return "true" if valor is True else "false" if valor is False else str(valor)


def ler_config(db: Session) -> dict:
    config = dict(CONFIG_PADRAO)
    for item in db.query(Configuracao).all():
        if item.nome in CONFIG_PADRAO:
            config[item.nome] = item.valor.lower() == "true"
    return config


def salvar_config(db: Session, nome: str, valor) -> None:
    item = db.get(Configuracao, nome)
    if item is None:
        db.add(Configuracao(nome=nome, valor=_para_texto(valor)))
    else:
        item.valor = _para_texto(valor)
    db.commit()


def apagar_leituras_antigas(db: Session, dias: int) -> int:
    """Política de retenção: remove leituras mais antigas que `dias`. Retorna quantas foram apagadas."""
    if dias <= 0:
        return 0
    limite = agora_utc() - timedelta(days=dias)
    resultado = db.execute(delete(Leitura).where(Leitura.timestamp < limite))
    db.commit()
    return resultado.rowcount or 0
