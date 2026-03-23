<?php
/* © 2026 Monterro · Fathia & Bintang. All rights reserved. */

// ── Configuration ────────────────────────────────────────────────────────────
define('API_KEY',       'monterro2026');        
define('DB_HOST',       'localhost');
define('DB_NAME',       'monterro');
define('DB_USER',       'monterro');
define('DB_PASS',       'monterro_pass');      

define('SUPABASE_URL',  'https://sudlejmejjlairgxdlzi.supabase.co');
define('SUPABASE_KEY',  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9'
                       .'.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0'
                       .'.NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQo');
define('SB_CHANNEL',    'twatch-activity');
// ─────────────────────────────────────────────────────────────────────────────

header('Content-Type: application/json');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['error' => 'POST only']);
    exit;
}

// ── Validate API key ─────────────────────────────────────────────────────────
$api_key = isset($_POST['api_key']) ? trim($_POST['api_key']) : '';
if ($api_key !== API_KEY) {
    http_response_code(403);
    echo json_encode(['error' => 'bad api_key']);
    exit;
}

// ── Parse fields ─────────────────────────────────────────────────────────────
$steps    = intval($_POST['steps']    ?? 0);
$distance = intval($_POST['distance'] ?? 0);
$duration = intval($_POST['duration'] ?? 0);
$calories = intval($steps * 4 / 100);

// ── Insert into MySQL ─────────────────────────────────────────────────────────
$conn = new mysqli(DB_HOST, DB_USER, DB_PASS, DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db connect: ' . $conn->connect_error]);
    exit;
}

$stmt = $conn->prepare(
    'INSERT INTO live_data (steps, distance, duration, calories, source)
     VALUES (?, ?, ?, ?, "wifi")'
);
$stmt->bind_param('iiii', $steps, $distance, $duration, $calories);
$ok = $stmt->execute();
$stmt->close();
$conn->close();

if (!$ok) {
    http_response_code(500);
    echo json_encode(['error' => 'db insert failed']);
    exit;
}

// ── Broadcast to Supabase ─────────────────────────────────────────────────────
$payload = [
    'messages' => [[
        'topic'   => 'realtime:' . SB_CHANNEL,
        'event'   => 'live_update',
        'payload' => [
            'steps'          => $steps,
            'distance'       => $distance,
            'duration'       => $duration,
            'calories'       => $calories,
            'session_active' => true,
        ],
    ]]
];

$broadcast_url = SUPABASE_URL
    . '/realtime/v1/api/broadcast?apikey='
    . SUPABASE_KEY;

$ch = curl_init($broadcast_url);
curl_setopt_array($ch, [
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => json_encode($payload),
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT        => 4,
    CURLOPT_HTTPHEADER     => [
        'Content-Type: application/json',
        'apikey: '         . SUPABASE_KEY,
        'Authorization: Bearer ' . SUPABASE_KEY,
    ],
]);
$sb_result = curl_exec($ch);
$sb_code   = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

$sb_row = json_encode([
    'id'         => 1,  
    'steps'      => $steps,
    'distance'   => $distance,
    'duration'   => $duration,
    'calories'   => $calories,
    'updated_at' => gmdate('c'),
]);
$ch2 = curl_init(SUPABASE_URL . '/rest/v1/live_snapshot');
curl_setopt_array($ch2, [
    CURLOPT_CUSTOMREQUEST  => 'POST',
    CURLOPT_POSTFIELDS     => $sb_row,
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT        => 3,
    CURLOPT_HTTPHEADER     => [
        'Content-Type: application/json',
        'apikey: '         . SUPABASE_KEY,
        'Authorization: Bearer ' . SUPABASE_KEY,
        'Prefer: resolution=merge-duplicates',
    ],
]);
curl_exec($ch2);
$sb_db_code = curl_getinfo($ch2, CURLINFO_HTTP_CODE);
curl_close($ch2);

echo json_encode([
    'ok'         => true,
    'inserted'   => true,
    'sb_broadcast' => $sb_code,
    'sb_db_write'  => $sb_db_code,
]);
