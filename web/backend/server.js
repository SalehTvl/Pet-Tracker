const express = require("express");
const cors = require("cors");
const mqtt = require("mqtt");
const path = require("path");

const app = express();
app.use(cors());
app.use(express.json({ limit: "256kb" }));


const latestByDeviceId = new Map();


const MQTT_BROKER_URL = "mqtt://0.0.0.0:1883";
const MQTT_TOPIC = "mc60/data";

const mqttClient = mqtt.connect(MQTT_BROKER_URL, {
  // username: "nima",
  // password: "password",
});

mqttClient.on("connect", () => {
  console.log("✅ MQTT connected");
  mqttClient.subscribe(MQTT_TOPIC);
});

mqttClient.on("message", (_, message) => {
  try {
    const b = JSON.parse(message.toString());

    if (!b.device_id || !b.gps) return;

    const tsSec =
      typeof b.timestamp === "number"
        ? b.timestamp
        : Math.floor(Date.now() / 1000);

    const normalized = {
      device_id: String(b.device_id),
      timestamp_sec: tsSec,
      timestamp_ms: tsSec * 1000,
      gps: {
        lat: Number(b.gps.lat),
        lon: Number(b.gps.lon),
        alt: b.gps.alt ?? null,
        hdop: b.gps.hdop ?? null,
      },
      battery: b.battery ?? null,
      calories: b.calories ?? null,
      received_at_ms: Date.now(),
    };

    latestByDeviceId.set(normalized.device_id, normalized);
  } catch (e) {
    console.error("❌ MQTT parse error", e.message);
  }
});

app.get("/api/latest/:device_id", (req, res) => {
  const data = latestByDeviceId.get(req.params.device_id);
  if (!data) return res.status(404).json({ error: "No data yet" });
  res.json(data);
});


app.use(express.static(path.join(__dirname, "public")));
app.get("/", (_, res) =>
  res.sendFile(path.join(__dirname, "public/index.html"))
);


const PORT = process.env.PORT || 3000;
app.listen(PORT, "0.0.0.0", () =>
  console.log(`✅ HTTP server: http://0.0.0.0:${PORT}`)
);
