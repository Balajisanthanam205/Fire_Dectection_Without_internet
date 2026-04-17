-- ============================================================
--  Supabase Database Setup — Fire Detection System
--  Run this in: Supabase Dashboard → SQL Editor → New Query
-- ============================================================


-- ── 1. Create the sensor readings table ─────────────────────
CREATE TABLE IF NOT EXISTS sensor_readings (
  id             BIGSERIAL PRIMARY KEY,
  created_at     TIMESTAMPTZ DEFAULT NOW() NOT NULL,
  temperature    NUMERIC(5,1),          -- °C from DHT11
  humidity       NUMERIC(5,1),          -- % from DHT11
  gas_level      SMALLINT,              -- 0-100% from MQ2
  flame_level    SMALLINT,              -- 0-100% from Flame sensor
  soil_moisture  SMALLINT,              -- 0-100% from soil sensor
  danger_level   SMALLINT NOT NULL      -- 0=Safe 1=Caution 2=Warning 3=Fire
                 CHECK (danger_level BETWEEN 0 AND 3),
  rssi           SMALLINT,              -- LoRa signal strength dBm
  packet_id      INTEGER                -- TX-side packet counter
);

-- ── 2. Index on created_at for fast time-range queries ──────
CREATE INDEX IF NOT EXISTS idx_readings_created_at
  ON sensor_readings (created_at DESC);

-- ── 3. Index on danger_level for fast alert queries ─────────
CREATE INDEX IF NOT EXISTS idx_readings_danger
  ON sensor_readings (danger_level)
  WHERE danger_level >= 2;  -- Only index warning/fire events


-- ── 4. Enable Row Level Security (RLS) ──────────────────────
ALTER TABLE sensor_readings ENABLE ROW LEVEL SECURITY;

-- Allow anonymous inserts (your ESP32 uses the anon key)
CREATE POLICY "allow_anon_insert"
  ON sensor_readings
  FOR INSERT
  TO anon
  WITH CHECK (true);

-- Allow anyone to read (change to authenticated if needed)
CREATE POLICY "allow_public_read"
  ON sensor_readings
  FOR SELECT
  TO anon
  USING (true);


-- ── 5. Useful views ─────────────────────────────────────────

-- Latest reading
CREATE OR REPLACE VIEW latest_reading AS
  SELECT *
  FROM   sensor_readings
  ORDER  BY created_at DESC
  LIMIT  1;

-- Alert events only (danger >= 2)
CREATE OR REPLACE VIEW fire_alerts AS
  SELECT id,
         created_at,
         temperature,
         humidity,
         gas_level,
         flame_level,
         danger_level,
         rssi
  FROM   sensor_readings
  WHERE  danger_level >= 2
  ORDER  BY created_at DESC;

-- Hourly averages for the last 24h
CREATE OR REPLACE VIEW hourly_averages AS
  SELECT DATE_TRUNC('hour', created_at) AS hour,
         ROUND(AVG(temperature)::NUMERIC, 1)   AS avg_temp,
         ROUND(AVG(humidity)::NUMERIC, 1)       AS avg_humidity,
         ROUND(AVG(gas_level)::NUMERIC, 1)      AS avg_gas,
         ROUND(AVG(flame_level)::NUMERIC, 1)    AS avg_flame,
         MAX(danger_level)                       AS max_danger,
         COUNT(*)                                AS reading_count
  FROM   sensor_readings
  WHERE  created_at >= NOW() - INTERVAL '24 hours'
  GROUP  BY 1
  ORDER  BY 1 DESC;


-- ── 6. Auto-cleanup: keep only last 7 days (optional) ───────
-- Uncomment if you want automatic data pruning via cron:
--
-- SELECT cron.schedule(
--   'cleanup-old-readings',
--   '0 2 * * *',   -- runs daily at 2am UTC
--   $$
--     DELETE FROM sensor_readings
--     WHERE  created_at < NOW() - INTERVAL '7 days';
--   $$
-- );


-- ── 7. Verify your setup ────────────────────────────────────
SELECT 'sensor_readings table created' AS status;

-- Test insert (run this to confirm ESP32 will be able to insert):
INSERT INTO sensor_readings
  (temperature, humidity, gas_level, flame_level, soil_moisture, danger_level, rssi, packet_id)
VALUES
  (28.5, 60.0, 5, 2, 70, 0, -78, 1);

SELECT * FROM sensor_readings ORDER BY created_at DESC LIMIT 5;
