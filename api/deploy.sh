#!/bin/bash

echo "=== Deploy MQTT Gateway API ==="

# Instalar Python e pip
apt update
apt install -y python3 python3-pip python3-venv

# Criar ambiente virtual
cd /opt
python3 -m venv mqtt-api
source mqtt-api/bin/activate

# Copiar arquivos
mkdir -p /opt/mqtt-api/app
cp /root/api/main.py /opt/mqtt-api/app/
cp /root/api/requirements.txt /opt/mqtt-api/app/
cp /root/api/.env /opt/mqtt-api/app/

# Instalar dependências
cd /opt/mqtt-api/app
pip install -r requirements.txt

# Criar serviço systemd
cat > /etc/systemd/system/mqtt-api.service << EOF
[Unit]
Description=MQTT Gateway API
After=network.target mosquitto.service

[Service]
User=root
WorkingDirectory=/opt/mqtt-api/app
EnvironmentFile=/opt/mqtt-api/app/.env
ExecStart=/opt/mqtt-api/bin/uvicorn main:app --host ${API_HOST} --port ${API_PORT}
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Habilitar e iniciar
systemctl daemon-reload
systemctl enable mqtt-api
systemctl start mqtt-api
systemctl status mqtt-api

# Abrir porta no firewall
source /opt/mqtt-api/app/.env
ufw allow ${API_PORT}/tcp

echo ""
echo "=== API disponível em: https://gateway.lav60.com ==="
echo "=== Documentação:      https://gateway.lav60.com/docs ==="
