"""API da Lâmpada Inteligente.

Fluxo:  ESP32 --(HTTP POST /leituras)--> API --> PostgreSQL --> ponte --> Arduino IoT Cloud (dashboard)
                                                        ^                      |
                                                        +---- leitura_ativa ---+
"""
import logging
import os
import secrets
import time
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import List, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Query, status
from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy.orm import Session

import ponte_arduino_cloud
from database import (
    Leitura,
    SessionLocal,
    agora_utc,
    apagar_leituras_antigas,
    criar_tabelas,
    ler_config,
)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("api")

API_KEY = os.getenv("API_KEY", "")                       # vazio = sem autenticação (só para testes locais)
RETENCAO_DIAS = int(os.getenv("RETENCAO_DIAS", "30"))    # 0 = guardar para sempre
TIMESTAMP_MINIMO = 1_704_067_200                         # 01/01/2024: abaixo disso o relógio do ESP32 não está sincronizado

_ultima_limpeza = 0.0


@asynccontextmanager
async def lifespan(_app: FastAPI):
    criar_tabelas()
    _limpar_se_preciso(forcar=True)
    ponte_arduino_cloud.iniciar_em_thread()
    yield


app = FastAPI(
    title="API Lâmpada Inteligente IoT",
    description="Recebe leituras do ESP32 (1 LDR + sensor de refletância), guarda no banco "
                "e sincroniza com o Arduino IoT Cloud.",
    version="3.0.0",
    lifespan=lifespan,
)


# ---------------- dependências ----------------
def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


def exigir_api_key(x_api_key: Optional[str] = Header(None)):
    if API_KEY and not secrets.compare_digest(x_api_key or "", API_KEY):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "X-API-Key inválida ou ausente")


def _limpar_se_preciso(forcar: bool = False):
    global _ultima_limpeza
    if not forcar and time.monotonic() - _ultima_limpeza < 3600:
        return
    _ultima_limpeza = time.monotonic()
    with SessionLocal() as db:
        apagadas = apagar_leituras_antigas(db, RETENCAO_DIAS)
    if apagadas:
        log.info("retenção: %s leituras com mais de %s dias apagadas", apagadas, RETENCAO_DIAS)


# ---------------- schemas ----------------
class LeituraEntrada(BaseModel):
    ldr: int = Field(..., ge=0, le=4095, description="Valor bruto do LDR (ADC 0..4095)")
    refletancia: bool = Field(..., description="Sensor de refletância: true = detectou")
    timestamp: Optional[int] = Field(None, description="Epoch em segundos (NTP). Se omitido, usa a hora do servidor.")
    dispositivo: str = Field("esp32-lampada", max_length=64)


class LeituraSaida(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    timestamp: datetime
    recebido_em: datetime
    dispositivo: str
    ldr: int
    refletancia: bool


class Config(BaseModel):
    leitura_ativa: bool


class RespostaLeitura(BaseModel):
    leitura: LeituraSaida
    config: Config


# ---------------- rotas ----------------
@app.get("/")
def raiz():
    return {
        "projeto": "Lâmpada Inteligente IoT",
        "documentacao": "/docs",
        "rotas": ["POST /leituras", "GET /leituras", "GET /leituras/ultima", "GET /config", "GET /health"],
        "ponte_arduino_cloud": "ligada" if ponte_arduino_cloud.configurada() else "desligada",
    }


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/leituras", response_model=RespostaLeitura, status_code=status.HTTP_201_CREATED,
          dependencies=[Depends(exigir_api_key)])
def criar_leitura(entrada: LeituraEntrada, db: Session = Depends(get_db)):
    dados = entrada.model_dump()
    ts = dados.pop("timestamp")
    agora = agora_utc()
    dados["timestamp"] = datetime.fromtimestamp(ts, timezone.utc) if ts and ts >= TIMESTAMP_MINIMO else agora
    dados["recebido_em"] = agora

    leitura = Leitura(**dados)
    db.add(leitura)
    db.commit()
    db.refresh(leitura)

    _limpar_se_preciso()
    return {"leitura": leitura, "config": ler_config(db)}


@app.get("/leituras", response_model=List[LeituraSaida])
def listar_leituras(limite: int = Query(100, ge=1, le=1000), db: Session = Depends(get_db)):
    return db.query(Leitura).order_by(Leitura.id.desc()).limit(limite).all()


@app.get("/leituras/ultima", response_model=LeituraSaida)
def ultima_leitura(db: Session = Depends(get_db)):
    leitura = db.query(Leitura).order_by(Leitura.id.desc()).first()
    if leitura is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "Nenhuma leitura registrada ainda")
    return leitura


@app.get("/config", response_model=Config)
def obter_config(db: Session = Depends(get_db)):
    return ler_config(db)
