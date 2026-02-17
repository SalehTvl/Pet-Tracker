import math
import random
import time
import json
import paho.mqtt.client as mqtt

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "mc60/data"

DEVICE_ID = "device_001"

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
calories = 0.0

print("📡 MQTT mock device:", DEVICE_ID)


client = mqtt.Client(client_id=DEVICE_ID)


client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
client.loop_start()

while True:
    theta = random.uniform(0, 2 * math.pi)
    lat += meters_to_lat(1.0) * math.sin(theta)
    lon += meters_to_lon(1.0, lat) * math.cos(theta)

    lat = max(TEHRAN_LAT_MIN, min(TEHRAN_LAT_MAX, lat))
    lon = max(TEHRAN_LON_MIN, min(TEHRAN_LON_MAX, lon))

    hdop = random.uniform(0.6, 1.6)
    calories += random.uniform(0.02, 0.08)
    battery = max(0.0, battery - random.uniform(0.0005, 0.003))

    payload = {
        "device_id": DEVICE_ID,
        "timestamp": int(time.time()),  # seconds
        "gps": {
            "lat": round(lat, 6),
            "lon": round(lon, 6),
            "alt": round(alt, 1),
            "hdop": round(hdop, 2),
        },
        "battery": round(battery, 2),
        "calories": round(calories, 2),
    }

    client.publish(MQTT_TOPIC, json.dumps(payload), qos=0)

    print("✅ published:", payload)
    time.sleep(1)
