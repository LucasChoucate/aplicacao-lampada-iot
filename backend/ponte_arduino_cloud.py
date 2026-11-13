"""Ponte Banco de Dados <-> Arduino IoT Cloud.

Esta ponte é um "dispositivo manual" (Python) cadastrado no Arduino IoT Cloud.
- A cada PONTE_INTERVALO_S segundos ela lê a leitura mais recente do banco e publica
  as variáveis ldr e refletancia na Thing -> os gráficos do dashboard atualizam sozinhos.
- Quando alguém muda a variável leitura_ativa no dashboard, ela grava o valor no banco;
  o ESP32 recebe esse valor na resposta da API.

Roda de duas formas:
1. Dentro da API (app.py inicia em uma thread) -> é o que acontece no Render.
2. Sozinha:  python ponte_arduino_cloud.py   (usando DATABASE_URL do banco do Render).
"""
import logging
import os
import threading
import time

from arduino_iot_cloud import ArduinoCloudClient, Task

from database import Leitura, SessionLocal, criar_tabelas, salvar_config

log = logging.getLogger("ponte")

DEVICE_ID = os.getenv("ARDUINO_DEVICE_ID", "")
SECRET_KEY = os.getenv("ARDUINO_SECRET_KEY", "")
INTERVALO_S = float(os.getenv("PONTE_INTERVALO_S", "5"))

# Nomes das variáveis criadas na Thing "Ponte_BD" do Arduino IoT Cloud.
# Precisam ser IGUAIS aos "Variable name" do Cloud.
VARIAVEIS_LEITURA = ("ldr", "refletancia")
VARIAVEL_COMANDO = "leitura_ativa"

_ultimo_id_enviado = None


def configurada() -> bool:
    return bool(DEVICE_ID and SECRET_KEY)


def _definir(client, nome, valor):
    """Atribui o valor mantendo o tipo que a variável já tem (a biblioteca não aceita trocar o tipo)."""
    atual = client[nome]
    if atual is not None and type(valor) is not type(atual):
        valor = type(atual)(valor)
    client[nome] = valor


def enviar_do_banco(client, args=None):
    """Task periódica: banco -> Cloud. Só publica quando chega uma leitura nova."""
    global _ultimo_id_enviado
    try:
        with SessionLocal() as db:
            leitura = db.query(Leitura).order_by(Leitura.id.desc()).first()
        if leitura is None or leitura.id == _ultimo_id_enviado:
            return
        _definir(client, "ldr", int(leitura.ldr))
        _definir(client, "refletancia", bool(leitura.refletancia))
        _ultimo_id_enviado = leitura.id
        log.info("enviado ao Cloud: id=%s ldr=%s refletancia=%s",
                 leitura.id, leitura.ldr, leitura.refletancia)
    except Exception:
        # Se a exceção escapar, a biblioteca descarta esta Task para sempre.
        log.exception("falha ao enviar leitura ao Arduino Cloud")


def recebeu_leitura_ativa(client, valor):
    """Chamado quando o dashboard muda leitura_ativa (Cloud -> banco)."""
    try:
        with SessionLocal() as db:
            salvar_config(db, VARIAVEL_COMANDO, bool(valor))
        log.info("Cloud mudou %s para %s", VARIAVEL_COMANDO, bool(valor))
    except Exception:
        log.exception("falha ao salvar %s no banco", VARIAVEL_COMANDO)


def criar_cliente() -> ArduinoCloudClient:
    client = ArduinoCloudClient(device_id=DEVICE_ID, username=DEVICE_ID, password=SECRET_KEY)
    for nome in VARIAVEIS_LEITURA:
        client.register(nome, value=None)
    client.register(VARIAVEL_COMANDO, value=None, on_write=recebeu_leitura_ativa)
    client.register(Task("loop_banco", on_run=enviar_do_banco, interval=INTERVALO_S))
    return client


def rodar_para_sempre():
    """Mantém a ponte rodando; se a conexão cair, recria o cliente."""
    while True:
        try:
            log.info("conectando ao Arduino IoT Cloud (device %s)...", DEVICE_ID)
            criar_cliente().start()
        except Exception:
            log.exception("ponte caiu; tentando de novo em 10 s")
        time.sleep(10)


def iniciar_em_thread():
    if not configurada():
        log.warning("ARDUINO_DEVICE_ID/ARDUINO_SECRET_KEY não definidos: ponte com o Arduino Cloud desligada.")
        return None
    thread = threading.Thread(target=rodar_para_sempre, name="ponte-arduino-cloud", daemon=True)
    thread.start()
    return thread


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    if not configurada():
        raise SystemExit("Defina ARDUINO_DEVICE_ID e ARDUINO_SECRET_KEY antes de rodar a ponte.")
    criar_tabelas()
    rodar_para_sempre()
