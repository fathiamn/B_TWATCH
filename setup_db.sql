-- Monterro — MySQL setup
-- Run once on the Pi:
--   sudo mysql -u root -p < setup_db.sql

CREATE DATABASE IF NOT EXISTS monterro
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_general_ci;

USE monterro;

-- Latest live tick from the watch (only the most recent row matters;
-- older rows are kept for debugging but never read by the dashboard)
CREATE TABLE IF NOT EXISTS live_data (
    id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    steps      INT UNSIGNED NOT NULL DEFAULT 0,
    distance   INT UNSIGNED NOT NULL DEFAULT 0,   -- metres
    duration   INT UNSIGNED NOT NULL DEFAULT 0,   -- seconds
    calories   INT UNSIGNED NOT NULL DEFAULT 0,
    source     VARCHAR(8)   NOT NULL DEFAULT 'wifi',  -- 'wifi' | 'ble'
    received_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- One row per finished hike session
CREATE TABLE IF NOT EXISTS sessions (
    id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    steps      INT UNSIGNED NOT NULL DEFAULT 0,
    distance   INT UNSIGNED NOT NULL DEFAULT 0,
    duration   INT UNSIGNED NOT NULL DEFAULT 0,
    calories   INT UNSIGNED NOT NULL DEFAULT 0,
    ended_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Create a dedicated user so PHP never runs as root
-- Change 'monterro_pass' to a real password before running
CREATE USER IF NOT EXISTS 'monterro'@'localhost' IDENTIFIED BY 'monterro_pass';
GRANT SELECT, INSERT, UPDATE ON monterro.* TO 'monterro'@'localhost';
FLUSH PRIVILEGES;
