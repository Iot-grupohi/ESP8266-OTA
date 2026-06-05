from fastapi import FastAPI, HTTPException, Security
from fastapi.security.api_key import APIKeyHeader
from pydantic import BaseModel, Field
from typing import Literal, Optional, Union
import paho.mqtt.client as mqtt
import threading
import json
import os
from dotenv import load_dotenv

load_dotenv()

app = FastAPI(
    title="MQTT Gateway API — LAV60",
    description="API to control washers, dryers, AC and dosers via MQTT",
    version="2.1.0"
)

# ── MQTT Settings ───────────────────────────────────
MQTT_BROKER            = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT              = int(os.getenv("MQTT_PORT", 1883))
MQTT_USER              = os.getenv("MQTT_USER")
MQTT_PASSWORD          = os.getenv("MQTT_PASSWORD")
CONFIRM_TIMEOUT        = int(os.getenv("CONFIRM_TIMEOUT", 5))
PING_TIMEOUT           = int(os.getenv("PING_TIMEOUT", 20))
DOSER_CONSULTA_TIMEOUT = int(os.getenv("DOSER_CONSULTA_TIMEOUT", 15))

DOSER_MACHINES = ["321", "432", "543", "654"]

# ── Auth ────────────────────────────────────────────
API_TOKEN    = os.getenv("API_TOKEN")
token_header = APIKeyHeader(name="X-Token", auto_error=False)

def verify_token(token: str = Security(token_header)):
    if not API_TOKEN or token != API_TOKEN:
        raise HTTPException(status_code=401, detail="Invalid or missing X-Token header.")


# ── Models ──────────────────────────────────────────
class LedCommand(BaseModel):
    command: Literal["ON", "OFF"]

class WasherCommand(BaseModel):
    am: Optional[Literal["am01-1", "am01-2", "am02-1", "am02-2"]] = None

class DryerCommand(BaseModel):
    minutes: Literal[15, 30, 45]

class AcCommand(BaseModel):
    temperature: Literal["18", "22", "off"]

class DoserCommand(BaseModel):
    type: Literal[
        "softener0", "softener1", "softener2", "softener3",
        "am01-1", "am01-2", "am02-1", "am02-2",
        "rele1on", "rele2on", "rele3on",
        "consultasb01", "consultaam01", "consultaam02",
        "eepromread", "status"
    ]

class DoserAmacianteCommand(BaseModel):
    number: Optional[Literal[1, 2, 3]] = None
    endpoint: Optional[str] = None

class DoserDosagemCommand(BaseModel):
    endpoint: Optional[Literal["am01-1", "am01-2", "am02-1", "am02-2"]] = None

class DoserBombaCommand(BaseModel):
    pump: Literal[1, 2, 3]

class DoserSetTimeCommand(BaseModel):
    rele: Literal[1, 2, 3]
    seconds: float = Field(gt=0, le=3600)

class DoserTimeAdjustCommand(BaseModel):
    seconds: float = Field(gt=0, le=3600)


def normalize_tempo_seconds(value) -> float:
    """Converte valor do dispositivo (ms) para segundos. Valores < 100 já são segundos."""
    num = float(str(value).strip())
    if num >= 100:
        num /= 1000.0
    return round(num, 2)


def format_tempo_output(seconds: float):
    if seconds == int(seconds):
        return int(seconds)
    return seconds


def convert_tempos_to_seconds(tempos: dict) -> dict:
    return {
        key: format_tempo_output(normalize_tempo_seconds(val))
        for key, val in tempos.items()
    }


def format_seconds_payload(seconds: float) -> str:
    if seconds == int(seconds):
        return str(int(seconds))
    return str(seconds)


# ── MQTT helpers ────────────────────────────────────
def mqtt_publish_and_get_response(
    topic_cmd: str,
    topic_status: str,
    payload: str = "ping",
    timeout: int = None
) -> Union[dict, str, None]:
    if timeout is None:
        timeout = CONFIRM_TIMEOUT

    result = {"data": None, "event": threading.Event()}

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(topic_status)

    def on_message(client, userdata, msg):
        try:
            result["data"] = json.loads(msg.payload.decode())
        except Exception:
            result["data"] = msg.payload.decode()
        result["event"].set()

    client = mqtt.Client()
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(MQTT_BROKER, MQTT_PORT)
        client.loop_start()
        client.publish(topic_cmd, payload)
        result["event"].wait(timeout=timeout)
        client.loop_stop()
        client.disconnect()
    except Exception as e:
        raise HTTPException(status_code=503, detail=f"Failed to connect to MQTT broker: {str(e)}")

    return result["data"]


def send_and_wait(topic_cmd: str, topic_status: str, payload: str, timeout: int = None) -> bool:
    return mqtt_publish_and_get_response(topic_cmd, topic_status, payload, timeout) is not None


def validate_doser_machine(machine: str) -> None:
    if machine not in DOSER_MACHINES:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {DOSER_MACHINES}")


def publish_and_confirm(
    store: str,
    topic_suffix: str,
    payload: str,
    description: str,
    timeout: int = None,
    status_suffix: str = None,
):
    topic_cmd = f"{store}/{topic_suffix}"
    status_topic = f"{store}/{status_suffix or topic_suffix}/status"

    response = mqtt_publish_and_get_response(topic_cmd, status_topic, payload, timeout)

    if response is None:
        raise HTTPException(
            status_code=400,
            detail=f"ESP8266 at store '{store}' did not respond within {timeout or CONFIRM_TIMEOUT}s. Check if it is online."
        )

    if response == "error":
        raise HTTPException(status_code=400, detail=f"{description} failed at store '{store}'.")

    return {
        "store": store,
        "topic": topic_cmd,
        "payload": payload,
        "response": response,
        "message": f"{description} — confirmed by ESP8266"
    }


def publish_doser_action(store: str, machine: str, action: str, payload: str, description: str, timeout: int = None):
    # ESP8266 sempre publica resposta em {store}/doser/{machine}/status
    return publish_and_confirm(
        store,
        f"doser/{machine}/{action}",
        payload,
        description,
        timeout,
        status_suffix=f"doser/{machine}",
    )


# ══════════════════════════════════════════════════════
# LED
# ══════════════════════════════════════════════════════
@app.get("/")
def root():
    return {"message": "MQTT Gateway API LAV60 online", "broker": MQTT_BROKER}

@app.post("/{store}/led/on")
def led_on(store: str, _: None = Security(verify_token)):
    return publish_and_confirm(store, "led", "ON", "LED turned on")

@app.post("/{store}/led/off")
def led_off(store: str, _: None = Security(verify_token)):
    return publish_and_confirm(store, "led", "OFF", "LED turned off")

@app.post("/{store}/led")
def led_command(store: str, body: LedCommand, _: None = Security(verify_token)):
    return publish_and_confirm(store, "led", body.command, f"LED {body.command}")


# ══════════════════════════════════════════════════════
# WASHERS  →  {store}/washer/{machine}
# Machines: 321, 432, 543, 654
# Body (optional): { "am": "am01-1" } → triggers doser first
# ══════════════════════════════════════════════════════
@app.post("/{store}/washer/{machine}")
def release_washer(store: str, machine: str, body: WasherCommand = WasherCommand(), _: None = Security(verify_token)):
    valid_machines = ["321", "432", "543", "654"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")

    if body.am:
        # Step 1: trigger doser AM
        ok_doser = send_and_wait(
            topic_cmd    = f"{store}/doser/{machine}",
            topic_status = f"{store}/doser/{machine}/status",
            payload      = body.am
        )
        if not ok_doser:
            raise HTTPException(
                status_code=400,
                detail=f"Doser for machine '{machine}' did not respond within {CONFIRM_TIMEOUT}s. Washer was not released."
            )

        # Step 2: release washer
        ok_wash = send_and_wait(
            topic_cmd    = f"{store}/washer/{machine}",
            topic_status = f"{store}/washer/{machine}/status",
            payload      = "start"
        )
        if not ok_wash:
            raise HTTPException(
                status_code=400,
                detail=f"Doser OK, but washer '{machine}' did not respond within {CONFIRM_TIMEOUT}s."
            )

        return {
            "store": store,
            "machine": machine,
            "doser": body.am,
            "washer": "released",
            "message": f"Doser {body.am} + Washer {machine} — confirmed by ESP8266"
        }

    return publish_and_confirm(store, f"washer/{machine}", "start", f"Washer {machine} released")


# ══════════════════════════════════════════════════════
# DRYERS  →  {store}/dryer/{machine}
# Machines: 765, 876, 987, 210 | Minutes: 15, 30, 45
# ══════════════════════════════════════════════════════
@app.post("/{store}/dryer/{machine}")
def release_dryer(store: str, machine: str, body: DryerCommand, _: None = Security(verify_token)):
    valid_machines = ["765", "876", "987", "210"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")
    return publish_and_confirm(store, f"dryer/{machine}", str(body.minutes), f"Dryer {machine} started for {body.minutes} min")


# ══════════════════════════════════════════════════════
# AC  →  {store}/ac
# Temperatures: 18, 22, off
# ══════════════════════════════════════════════════════
@app.post("/{store}/ac")
def control_ac(store: str, body: AcCommand, _: None = Security(verify_token)):
    description = "AC turned off" if body.temperature == "off" else f"AC set to {body.temperature}°C"
    return publish_and_confirm(store, "ac", body.temperature, description)


# ══════════════════════════════════════════════════════
# DOSER  →  {store}/doser/{machine}[/{action}]
# Machines: 321, 432, 543, 654
# ══════════════════════════════════════════════════════
@app.post("/{store}/doser/{machine}")
def trigger_doser(store: str, machine: str, body: DoserCommand, _: None = Security(verify_token)):
    validate_doser_machine(machine)
    return publish_and_confirm(store, f"doser/{machine}", body.type, f"Doser {machine} — {body.type}")


@app.post("/{store}/doser/{machine}/amaciante")
def doser_amaciante(
    store: str,
    machine: str,
    body: DoserAmacianteCommand = DoserAmacianteCommand(),
    _: None = Security(verify_token)
):
    validate_doser_machine(machine)
    if body.endpoint:
        payload = body.endpoint
    elif body.number is not None:
        payload = str(body.number)
    else:
        payload = ""
    return publish_doser_action(
        store, machine, "amaciante", payload,
        f"Doser {machine} — amaciante"
    )


@app.post("/{store}/doser/{machine}/dosagem")
def doser_dosagem(
    store: str,
    machine: str,
    body: DoserDosagemCommand = DoserDosagemCommand(),
    _: None = Security(verify_token)
):
    validate_doser_machine(machine)
    payload = body.endpoint or ""
    return publish_doser_action(
        store, machine, "dosagem", payload,
        f"Doser {machine} — dosagem"
    )


@app.post("/{store}/doser/{machine}/bomba")
def doser_bomba(store: str, machine: str, body: DoserBombaCommand, _: None = Security(verify_token)):
    validate_doser_machine(machine)
    return publish_doser_action(
        store, machine, "bomba", str(body.pump),
        f"Doser {machine} — bomba {body.pump}"
    )


@app.get("/{store}/doser/{machine}/consulta")
def doser_consulta(store: str, machine: str, _: None = Security(verify_token)):
    """Consulta tempos de sabão, floral e sport na dosadora."""
    validate_doser_machine(machine)
    response = mqtt_publish_and_get_response(
        topic_cmd=f"{store}/doser/{machine}/consulta",
        topic_status=f"{store}/doser/{machine}/status",
        payload="ping",
        timeout=DOSER_CONSULTA_TIMEOUT
    )

    if response is None:
        raise HTTPException(
            status_code=400,
            detail=f"ESP8266 at store '{store}' did not respond within {DOSER_CONSULTA_TIMEOUT}s."
        )

    if response == "error" or not isinstance(response, dict):
        raise HTTPException(status_code=400, detail=f"Doser consulta failed for machine '{machine}'.")

    return {
        "store": store,
        "machine": machine,
        "tempos": convert_tempos_to_seconds(response)
    }


@app.post("/{store}/doser/{machine}/settime")
def doser_settime(store: str, machine: str, body: DoserSetTimeCommand, _: None = Security(verify_token)):
    validate_doser_machine(machine)
    payload = f"{body.rele}:{format_seconds_payload(body.seconds)}"
    return publish_doser_action(
        store, machine, "settime", payload,
        f"Doser {machine} — settime rele {body.rele} ({body.seconds}s)"
    )


@app.post("/{store}/doser/{machine}/settime/sabao")
def doser_settime_sabao(
    store: str,
    machine: str,
    body: DoserTimeAdjustCommand,
    _: None = Security(verify_token)
):
    validate_doser_machine(machine)
    return publish_doser_action(
        store, machine, "settime", f"1:{format_seconds_payload(body.seconds)}",
        f"Doser {machine} — tempo sabão ({body.seconds}s)"
    )


@app.post("/{store}/doser/{machine}/settime/floral")
def doser_settime_floral(
    store: str,
    machine: str,
    body: DoserTimeAdjustCommand,
    _: None = Security(verify_token)
):
    validate_doser_machine(machine)
    return publish_doser_action(
        store, machine, "settime", f"2:{format_seconds_payload(body.seconds)}",
        f"Doser {machine} — tempo floral ({body.seconds}s)"
    )


@app.post("/{store}/doser/{machine}/settime/sport")
def doser_settime_sport(
    store: str,
    machine: str,
    body: DoserTimeAdjustCommand,
    _: None = Security(verify_token)
):
    validate_doser_machine(machine)
    return publish_doser_action(
        store, machine, "settime", f"3:{format_seconds_payload(body.seconds)}",
        f"Doser {machine} — tempo sport ({body.seconds}s)"
    )


@app.get("/{store}/doser/{machine}/device-status")
def doser_device_status(store: str, machine: str, _: None = Security(verify_token)):
    """Verifica conectividade HTTP da dosadora via GET /status no dispositivo."""
    validate_doser_machine(machine)
    response = mqtt_publish_and_get_response(
        topic_cmd=f"{store}/doser/{machine}/status",
        topic_status=f"{store}/doser/{machine}/status",
        payload="ping"
    )

    if response is None:
        raise HTTPException(
            status_code=400,
            detail=f"ESP8266 at store '{store}' did not respond within {CONFIRM_TIMEOUT}s."
        )

    return {
        "store": store,
        "machine": machine,
        "online": response == "online"
    }


# ══════════════════════════════════════════════════════
# STATUS / PING  →  {store}/ping/{device}
# ESP8266 verifica dispositivos via ICMP (dosadoras: HTTP /status + ICMP)
# ══════════════════════════════════════════════════════

def ping_and_wait(store: str, topic_suffix: str, timeout: int = None) -> dict:
    """Publica ping e aguarda resposta JSON do ESP8266."""
    if timeout is None:
        timeout = CONFIRM_TIMEOUT

    data = mqtt_publish_and_get_response(
        topic_cmd=f"{store}/{topic_suffix}",
        topic_status=f"{store}/{topic_suffix}/status",
        payload="ping",
        timeout=timeout
    )

    if data is None:
        raise HTTPException(
            status_code=400,
            detail=f"ESP8266 at store '{store}' did not respond within {timeout}s."
        )
    return data


@app.get("/{store}/status")
def status_all(store: str, _: None = Security(verify_token)):
    """Consulta o status de todos os dispositivos da loja."""
    return ping_and_wait(store, "ping/all", timeout=PING_TIMEOUT)


@app.get("/{store}/status/washer/{machine}")
def status_washer(store: str, machine: str, _: None = Security(verify_token)):
    valid_machines = ["321", "432", "543", "654"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")
    return ping_and_wait(store, f"ping/washer/{machine}")


@app.get("/{store}/status/dryer/{machine}")
def status_dryer(store: str, machine: str, _: None = Security(verify_token)):
    valid_machines = ["765", "876", "987", "210"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")
    return ping_and_wait(store, f"ping/dryer/{machine}")


@app.get("/{store}/status/ac")
def status_ac(store: str, _: None = Security(verify_token)):
    return ping_and_wait(store, "ping/ac")


@app.get("/{store}/status/doser/{machine}")
def status_doser(store: str, machine: str, _: None = Security(verify_token)):
    validate_doser_machine(machine)
    return ping_and_wait(store, f"ping/doser/{machine}")
