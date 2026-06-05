#!/bin/bash
set -euo pipefail

APP_DIR="/opt/mqtt-api/app"
VENV_DIR="/opt/mqtt-api"
REPO_DIR="${REPO_DIR:-/root/ESP8266-OTA}"
SERVICE="mqtt-api"

echo "=== Atualizando MQTT Gateway API ==="

if [ -d "$REPO_DIR/.git" ]; then
  echo ">> Git pull em $REPO_DIR"
  git -C "$REPO_DIR" pull origin main
  cp "$REPO_DIR/api/main.py" "$APP_DIR/"
  cp "$REPO_DIR/api/requirements.txt" "$APP_DIR/"
  cp "$REPO_DIR/api/README.md" "$APP_DIR/" 2>/dev/null || true
else
  echo ">> Repositório não encontrado em $REPO_DIR"
  echo "   Copie manualmente api/main.py para $APP_DIR"
  exit 1
fi

echo ">> Instalando dependências"
source "$VENV_DIR/bin/activate"
pip install -r "$APP_DIR/requirements.txt"

echo ">> Reiniciando serviço"
systemctl daemon-reload
systemctl restart "$SERVICE"
systemctl status "$SERVICE" --no-pager

echo ""
echo "=== API atualizada ==="
echo "Docs: https://gateway.lav60.com/docs"
