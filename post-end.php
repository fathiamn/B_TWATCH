<?php
/**
 * post-end.php — Monterro session-end endpoint
 * Place at: /var/www/html/monterro/post-end.php
 *
 * Called by activity.cpp → post_session_end() when Stop is pressed.
 * Form fields:  api_key, steps, distance, duration
 *               history_N_steps, history_N_distance, history_N_duration,
 *               history_N_calories   (N = 0 … HISTORY_MAX-1, newest first)
 *
 * Actions:
 *   1. Validate API key
 *   2. Insert into sessions table
 *   3. Broadcast session_end to Supabase with history array
 */

// ── Configuration (keep in sync with post-live.php) ─────────────────────────
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
define('HISTORY_MAX',   10);
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

// ── Parse current session ─────────────────────────────────────────────────────
$steps    = intval($_POST['steps']    ?? 0);
$distance = intval($_POST['distance'] ?? 0);
$duration = intval($_POST['duration'] ?? 0);
$calories = intval($steps * 4 / 100);

// ── Parse history array ───────────────────────────────────────────────────────
// Watch sends history_0_steps, history_0_distance … history_9_calories
// newest-first (index 0 = most recent session, which is the one just ended).
$history = [];
for ($i = 0; $i < HISTORY_MAX; $i++) {
    $hs = intval($_POST["history_{$i}_steps"]    ?? -1);
    if ($hs < 0) break;   // no more entries
    $history[] = [
        'steps'    => $hs,
        'distance' => intval($_POST["history_{$i}_distance"] ?? 0),
        'duration' => intval($_POST["history_{$i}_duration"] ?? 0),
        'calories' => intval($_POST["history_{$i}_calories"] ?? 0),
    ];
}

// ── Insert session into MySQL ─────────────────────────────────────────────────
$conn = new mysqli(DB_HOST, DB_USER, DB_PASS, DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db connect: ' . $conn->connect_error]);
    exit;
}

$stmt = $conn->prepare(
    'INSERT INTO sessions (steps, distance, duration, calories)
     VALUES (?, ?, ?, ?)'
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
$ended_at = gmdate('Y-m-d\TH:i:s\Z');

$sb_payload = [
    'messages' => [[
        'topic'   => 'realtime:' . SB_CHANNEL,
        'event'   => 'session_end',
        'payload' => [
            'steps'    => $steps,
            'distance' => $distance,
            'duration' => $duration,
            'calories' => $calories,
            'ended_at' => $ended_at,
            'history'  => $history,
        ],
    ]]
];

$broadcast_url = SUPABASE_URL
    . '/realtime/v1/api/broadcast?apikey='
    . SUPABASE_KEY;

$ch = curl_init($broadcast_url);
curl_setopt_array($ch, [
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => json_encode($sb_payload),
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

// ── Mirror session to Supabase DB ─────────────────────────────────────────
$sb_session = json_encode([
    'steps'    => $steps,
    'distance' => $distance,
    'duration' => $duration,
    'calories' => $calories,
    'ended_at' => $ended_at,
]);
$ch2 = curl_init(SUPABASE_URL . '/rest/v1/session_history');
curl_setopt_array($ch2, [
    CURLOPT_CUSTOMREQUEST  => 'POST',
    CURLOPT_POSTFIELDS     => $sb_session,
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT        => 3,
    CURLOPT_HTTPHEADER     => [
        'Content-Type: application/json',
        'apikey: '         . SUPABASE_KEY,
        'Authorization: Bearer ' . SUPABASE_KEY,
    ],
]);
curl_exec($ch2);
curl_close($ch2);

echo json_encode([
    'ok'               => true,
    'history_received' => count($history),
    'supabase'         => $sb_code,
]);