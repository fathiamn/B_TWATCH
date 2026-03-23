<?php
/* © 2026 Monterro · Fathia & Bintang. All rights reserved. */

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET');

// ── Configuration ─────────────────────────────────────────────────────────────
define('DB_HOST',       'localhost');
define('DB_NAME',       'monterro');
define('DB_USER',       'monterro');
define('DB_PASS',       'monterro_pass');  
define('CONNECTED_TTL', 30);              
define('HISTORY_MAX',   10);
// ─────────────────────────────────────────────────────────────────────────────

// ── DB connect ────────────────────────────────────────────────────────────────
$conn = new mysqli(DB_HOST, DB_USER, DB_PASS, DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db: ' . $conn->connect_error]);
    exit;
}

// ── Live state ────────────────────────────────────────────────────────────────
$live = [
    'steps'          => 0,
    'distance'       => 0,
    'duration'       => 0,
    'calories'       => 0,
    'session_active' => false,
];
$connected = false;

$r = $conn->query(
    'SELECT steps, distance, duration, calories, received_at
     FROM live_data ORDER BY id DESC LIMIT 1'
);
if ($r && $row = $r->fetch_assoc()) {
    $age       = time() - strtotime($row['received_at']);
    $is_fresh  = ($age < CONNECTED_TTL);
    $live      = [
        'steps'          => (int)$row['steps'],
        'distance'       => (int)$row['distance'],
        'duration'       => (int)$row['duration'],
        'calories'       => (int)$row['calories'],
        'session_active' => $is_fresh,
    ];
    $connected = $is_fresh;
}

// ── Last finished session ─────────────────────────────────────────────────────
$last_session = ['error' => 'no session yet'];

$r = $conn->query(
    "SELECT steps, distance, duration, calories,
            DATE_FORMAT(ended_at, '%Y-%m-%dT%TZ') AS ended_at
     FROM sessions ORDER BY id DESC LIMIT 1"
);
if ($r && $row = $r->fetch_assoc()) {
    $last_session = [
        'steps'    => (int)$row['steps'],
        'distance' => (int)$row['distance'],
        'duration' => (int)$row['duration'],
        'calories' => (int)$row['calories'],
        'ended_at' => $row['ended_at'],
    ];
}

// ── History (newest-first, up to HISTORY_MAX) ─────────────────────────────────
$history = [];

$r = $conn->query(
    "SELECT steps, distance, duration, calories,
            DATE_FORMAT(ended_at, '%Y-%m-%dT%TZ') AS ended_at
     FROM sessions ORDER BY id DESC LIMIT " . HISTORY_MAX
);
if ($r) {
    while ($row = $r->fetch_assoc()) {
        $history[] = [
            'steps'    => (int)$row['steps'],
            'distance' => (int)$row['distance'],
            'duration' => (int)$row['duration'],
            'calories' => (int)$row['calories'],
            'ended_at' => $row['ended_at'],
        ];
    }
}

$conn->close();

// ── Respond ───────────────────────────────────────────────────────────────────
echo json_encode([
    'live'         => $live,
    'last_session' => $last_session,
    'history'      => $history,
    'connected'    => $connected,
], JSON_UNESCAPED_UNICODE);
