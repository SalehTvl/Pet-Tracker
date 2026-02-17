import json
import math
import random
import time
import uuid
import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883

DEVICE_ID = "device_001"
TOPIC = f"devices/{DEVICE_ID}/telemetry"

# Tehran bounds
TEHRAN_LAT_MIN, TEHRAN_LAT_MAX = 35.60, 35.80
TEHRAN_LON_MIN, TEHRAN_LON_MAX = 51.30, 51.55

def meters_to_lat(m):
    return m / 111_111.0

def meters_to_lon(m, lat_deg):
    return m / (111_111.0 * math.cos(math.radians(lat_deg)))

lat = random.uniform(TEHRAN_LAT_MIN, TEHRAN_LAT_MAX)
lon = random.uniform(TEHRAN_LON_MIN, TEHRAN_LON_MAX)
alt = random.uniform(1100, 1500)

battery = 100.0

client = mqtt.Client(client_id=str(uuid.uuid4()))
client.connect(BROKER, PORT, 60)
client.loop_start()

print("Mock publishing to:", BROKER, PORT, TOPIC)
print("device_id:", DEVICE_ID)

while True:
    # Move ~1 meter each second in random direction
    theta = random.uniform(0, 2 * math.pi)

    lat += meters_to_lat(1.0) * math.sin(theta)
    lon += meters_to_lon(1.0, lat) * math.cos(theta)

    # clamp inside Tehran-ish range
    lat = max(TEHRAN_LAT_MIN, min(TEHRAN_LAT_MAX, lat))
    lon = max(TEHRAN_LON_MIN, min(TEHRAN_LON_MAX, lon))

    hdop = random.uniform(0.6, 1.6)
    battery = max(0.0, battery - random.uniform(0.0005, 0.003))

    payload = {
        "device_id": DEVICE_ID,
        "timestamp": int(time.time()),  # seconds like 1708098000
        "gps": {
            "lat": lat,
            "lon": lon,
            "alt": round(alt, 1),
            "hdop": round(hdop, 2)
        },
        "battery": round(battery, 2)
    }

    client.publish(TOPIC, json.dumps(payload), qos=0)
    time.sleep(5)
