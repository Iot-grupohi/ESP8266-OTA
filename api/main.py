from fastapi import FastAPI, HTTPException, Security
from fastapi.security.api_key import APIKeyHeader
from pydantic import BaseModel
from typing import Literal, Optional
import paho.mqtt.client as mqtt
import threading
import json
import os
from dotenv import load_dotenv

load_dotenv()

app = FastAPI(
    title="MQTT Gateway API — LAV60",
    description="API to control washers, dryers, AC and dosers via MQTT",
    version="2.0.0"
)

# ── MQTT Settings ───────────────────────────────────
MQTT_BROKER     = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT       = int(os.getenv("MQTT_PORT", 1883))
MQTT_USER       = os.getenv("MQTT_USER")
MQTT_PASSWORD   = os.getenv("MQTT_PASSWORD")
CONFIRM_TIMEOUT = int(os.getenv("CONFIRM_TIMEOUT", 5))
PING_TIMEOUT    = int(os.getenv("PING_TIMEOUT", 20))  # ping all devices can take longer

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
    type: Literal["softener0", "softener1", "softener2", "am01-1", "am01-2", "am02-1", "am02-2", "eepromread"]


# ── MQTT publish with confirmation ──────────────────
def send_and_wait(topic_cmd: str, topic_status: str, payload: str) -> bool:
    confirmed = threading.Event()

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(topic_status)

    def on_message(client, userdata, msg):
        confirmed.set()

    client = mqtt.Client()
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(MQTT_BROKER, MQTT_PORT)
        client.loop_start()
        client.publish(topic_cmd, payload)
        received = confirmed.wait(timeout=CONFIRM_TIMEOUT)
        client.loop_stop()
        client.disconnect()
        return received
    except Exception as e:
        raise HTTPException(status_code=503, detail=f"Failed to connect to MQTT broker: {str(e)}")


def publish_and_confirm(store: str, topic_suffix: str, payload: str, description: str):
    topic_cmd    = f"{store}/{topic_suffix}"
    topic_status = f"{store}/{topic_suffix}/status"

    confirmed = send_and_wait(topic_cmd, topic_status, payload)

    if not confirmed:
        raise HTTPException(
            status_code=400,
            detail=f"ESP32 at store '{store}' did not respond within {CONFIRM_TIMEOUT}s. Check if it is online."
        )

    return {
        "store": store,
        "topic": topic_cmd,
        "payload": payload,
        "message": f"{description} — confirmed by ESP32"
    }


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
            "message": f"Doser {body.am} + Washer {machine} — confirmed by ESP32"
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
# DOSER  →  {store}/doser/{machine}
# Machines: 321, 432, 543, 654
# Types: softener0/1/2, am01-1/2, am02-1/2, eepromread
# ══════════════════════════════════════════════════════
@app.post("/{store}/doser/{machine}")
def trigger_doser(store: str, machine: str, body: DoserCommand, _: None = Security(verify_token)):
    valid_machines = ["321", "432", "543", "654"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")
    return publish_and_confirm(store, f"doser/{machine}", body.type, f"Doser {machine} — {body.type}")


# ══════════════════════════════════════════════════════
# STATUS / PING  →  {store}/ping/{device}
# ESP8266 faz TCP connect na porta 80 de cada dispositivo
# ══════════════════════════════════════════════════════

def ping_and_wait(store: str, topic_suffix: str, timeout: int = None) -> dict:
    """Publica ping e aguarda resposta JSON do ESP8266."""
    if timeout is None:
        timeout = CONFIRM_TIMEOUT

    result = {"data": None, "event": threading.Event()}

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(f"{store}/{topic_suffix}/status")

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
        client.publish(f"{store}/{topic_suffix}", "ping")
        result["event"].wait(timeout=timeout)
        client.loop_stop()
        client.disconnect()
    except Exception as e:
        raise HTTPException(status_code=503, detail=f"Failed to connect to MQTT broker: {str(e)}")

    if result["data"] is None:
        raise HTTPException(
            status_code=400,
            detail=f"ESP8266 at store '{store}' did not respond within {timeout}s."
        )
    return result["data"]


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
    valid_machines = ["321", "432", "543", "654"]
    if machine not in valid_machines:
        raise HTTPException(status_code=400, detail=f"Invalid machine. Use: {valid_machines}")
    return ping_and_wait(store, f"ping/doser/{machine}")
