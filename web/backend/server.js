const path = require("path");
const express = require("express");
const cors = require("cors");
const mqtt = require("mqtt");

const app = express();
app.use(cors());
app.use(express.json({ limit: "256kb" }));

// Serve frontend
app.use(express.static(path.join(__dirname, "public")));
app.get("/", (req, res) => {
  res.sendFile(path.join(__dirname, "public/index.html"));
});

// =====================
// Config
// =====================
const MQTT_URL = process.env.MQTT_URL || "mqtt://0.0.0.0:1883";
const MQTT_TOPIC = process.env.MQTT_TOPIC || "devices/+/telemetry";

const C = Number(process.env.CAL_C || 1.0);

const MAX_METERS_PER_SEC = Number(process.env.MAX_MPS || 10);

// =====================
// In-memory storage
// =====================
// device_id -> latest payload for frontend
const latestByDeviceId = new Map();

// device_id -> running state for daily tracking
// {
//   dayKey, lastLat, lastLon, lastTsMs,
//   distanceTodayKm, weight, caloriesToday,
//   lastSpeedMps, lastDtSec
// }
const deviceState = new Map();

// =====================
// Helpers
// =====================
function haversineMeters(lat1, lon1, lat2, lon2) {
  const R = 6371000;
  const toRad = (d) => (d * Math.PI) / 180;

  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);

  const a =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;

  return 2 * R * Math.asin(Math.sqrt(a));
}

function iranDayKeyFromMs(tsMs) {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Tehran",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(new Date(tsMs));
}

function normalizeTimestampSec(input) {
  // device sends seconds like 1708098000
  if (typeof input === "number" && Number.isFinite(input)) return input;
  return Math.floor(Date.now() / 1000);
}

function recomputeCaloriesForDevice(id) {
  const st = deviceState.get(id);
  if (!st) return;

  if (st.weight != null && Number.isFinite(st.weight)) {
    st.caloriesToday = st.distanceTodayKm * st.weight * C;
  } else {
    st.caloriesToday = null;
  }
  deviceState.set(id, st);

  const latest = latestByDeviceId.get(id);
  if (latest) {
    latest.weight = st.weight;
    latest.distance_today_km = Number(st.distanceTodayKm.toFixed(5));
    latest.calories_today =
      st.caloriesToday != null ? Number(st.caloriesToday.toFixed(2)) : null;
    latest.day_key_iran = st.dayKey;
    latestByDeviceId.set(id, latest);
  }
}

// =====================
// REST API
// =====================
app.get("/api/latest/:device_id", (req, res) => {
  const id = String(req.params.device_id);
  const data = latestByDeviceId.get(id);
  if (!data) return res.status(404).json({ error: "No data for this device_id yet" });
  res.json(data);
});

app.post("/api/device/:device_id/weight", (req, res) => {
  const id = String(req.params.device_id);
  const w = Number(req.body?.weight);

  if (!Number.isFinite(w) || w <= 0 || w > 500) {
    return res.status(400).json({ error: "weight must be a number between 0 and 500" });
  }

  const prev = deviceState.get(id);
  if (!prev) {
    const nowMs = Date.now();
    deviceState.set(id, {
      dayKey: iranDayKeyFromMs(nowMs),
      lastLat: null,
      lastLon: null,
      lastTsMs: nowMs,
      distanceTodayKm: 0,
      weight: w,
      caloriesToday: 0,
      lastSpeedMps: null,
      lastDtSec: null,
    });
  } else {
    prev.weight = w;
    deviceState.set(id, prev);
  }

  recomputeCaloriesForDevice(id);
  res.json({ ok: true, device_id: id, weight: w });
});

// =====================
// MQTT subscriber
// =====================
const mqttClient = mqtt.connect(MQTT_URL);

mqttClient.on("connect", () => {
  console.log("✅ MQTT connected:", MQTT_URL);
  mqttClient.subscribe(MQTT_TOPIC, (err) => {
    if (err) console.error("❌ MQTT subscribe error:", err.message);
    else console.log("📡 Subscribed to:", MQTT_TOPIC);
  });
});

mqttClient.on("message", (_, message) => {
  try {
    const b = JSON.parse(message.toString());
    if (!b.device_id || !b.gps) return;

    const id = String(b.device_id);

    const tsSec = normalizeTimestampSec(b.timestamp);
    const tsMs = tsSec * 1000;

    const lat = Number(b.gps.lat);
    const lon = Number(b.gps.lon);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return;

    const alt = b.gps.alt ?? null;
    const hdop = b.gps.hdop ?? null;

    const dayKey = iranDayKeyFromMs(tsMs);
    const prev = deviceState.get(id);

    let st;

    if (!prev || prev.dayKey !== dayKey) {
      st = {
        dayKey,
        lastLat: lat,
        lastLon: lon,
        lastTsMs: tsMs,
        distanceTodayKm: 0,
        weight: prev?.weight ?? null, // keep stored weight
        caloriesToday: 0,
        lastSpeedMps: null,
        lastDtSec: null,
      };
      if (prev && prev.dayKey) {
        console.log(
            `Day closed for ${deviceId} (${stateToLog.dayKey}) | distance_km=${Number(stateToLog.distanceTodayKm.toFixed(3))} | calories=${stateToLog.caloriesToday ?? "null"}`
        );
      }
    } else {
      st = { ...prev };

      if (st.lastLat == null || st.lastLon == null) {
        st.lastLat = lat;
        st.lastLon = lon;
        st.lastTsMs = tsMs;
        st.lastSpeedMps = null;
        st.lastDtSec = null;
      } else {
        const meters = haversineMeters(st.lastLat, st.lastLon, lat, lon);
        const dtSec = Math.max(1, Math.round((tsMs - st.lastTsMs) / 1000));

        st.lastDtSec = dtSec;

        if (meters / dtSec <= MAX_METERS_PER_SEC) {
          st.distanceTodayKm += meters / 1000;

          st.lastSpeedMps = meters / dtSec;
        } else {
          st.lastSpeedMps = null;
        }

        st.lastLat = lat;
        st.lastLon = lon;
        st.lastTsMs = tsMs;
      }
    }

    if (st.weight != null && Number.isFinite(st.weight)) {
      st.caloriesToday = st.distanceTodayKm * st.weight * C;
    } else {
      st.caloriesToday = null;
    }

    deviceState.set(id, st);

    const speedMps = st.lastSpeedMps;
    const speedKmh = speedMps != null ? speedMps * 3.6 : null;

    const normalized = {
      device_id: id,
      timestamp_sec: tsSec,
      timestamp_ms: tsMs,
      gps: { lat, lon, alt, hdop },
      battery: b.battery ?? null,

      weight: st.weight,
      distance_today_km: Number(st.distanceTodayKm.toFixed(5)),
      calories_today: st.caloriesToday != null ? Number(st.caloriesToday.toFixed(2)) : null,
      day_key_iran: st.dayKey,

      seconds_since_prev: st.lastDtSec ?? null,
      speed_mps: speedMps != null ? Number(speedMps.toFixed(2)) : null,
      speed_kmh: speedKmh != null ? Number(speedKmh.toFixed(2)) : null,

      // server receive time for "updated X seconds ago"
      received_at_ms: Date.now(),
    };

    latestByDeviceId.set(id, normalized);
  } catch (e) {
    console.error("❌ MQTT parse error", e.message);
  }
});

// =====================
// Start server
// =====================
const PORT = process.env.PORT || 3000;
app.listen(PORT, "0.0.0.0", () => {
  console.log(`   Web app: http://0.0.0.0:${PORT}`);
  console.log(`   MQTT_URL=${MQTT_URL}`);
  console.log(`   MQTT_TOPIC=${MQTT_TOPIC}`);
  console.log(`   CAL_C=${C}   MAX_MPS=${MAX_METERS_PER_SEC}`);
});
