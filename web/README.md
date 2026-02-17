# 📡 Pet‑Tracker — Hybrid IoT Backend (MQTT + HTTP)

A lightweight, fully self‑hosted IoT backend for MC60‑based projects using a Hybrid Architecture:

- **MQTT** for device data ingestion
- **HTTP** for REST API and dashboard access (simple Leaflet frontend)


---

## 🗂️ Project Structure
```text
web/
├── backend/
│   ├── node_modules/
│   ├── public/
│   │   └── index.html          # Frontend (Leaflet / Map)
│   ├── package.json
│   ├── package-lock.json
│   └── server.js               # Node.js Hybrid Backend (MQTT + HTTP)
│
├── mock/
│   ├── mock_device.py          # MQTT mock device
│   └── requirements.txt        # Python dependencies
│
└── README.md
```

### Install Mosquitto
```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

### Run Backend (Node.js)
```bash
cd backend
npm install
node server.js
```
you can access frontend via 0.0.0.0:3000 after that

### Run mock script
```bash
cd mock
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 mock_device.py
```

