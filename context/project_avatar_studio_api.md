---
name: avatar-studio-api
description: Avatar Studio production API (aeonixinnovations) — Jetson device protocol (WS events + REST sync)
metadata: 
  node_type: memory
  type: reference
  originSessionId: c340df7a-1285-4391-94c3-c966be977f05
---

**Avatar Studio API v2.0.0** — production backend for the [[jetson-qt-kiosk]] avatar app. Express server. Docs: https://avatarstudioapi.aeonixinnovations.com/docs/ (spec embedded in `/docs/swagger-ui-init.js`, NOT at /openapi.json).

**Base:** `https://avatarstudioapi.aeonixinnovations.com/`
**WS:** `ws://avatarstudioapi.aeonixinnovations.com//ws?device_id={device_id}` (double slash as documented).
**Auth:** NO global scheme. Each device gets a `token` after claim; pass `{device_id, token}` in body of heartbeat + all /sync/* calls.

**Jetson device flow:**
1. Boot → `POST /device/init {device_id}` → `{status: pairing|already_paired, qr_payload, ws_url}`. If pairing, show QR (qr_payload) on screen.
2. Open WS `/ws?device_id=...`.
3. App scans QR → server WS `device_claimed` → Jetson `GET /device/me?device_id=...` → `{user_id, token, device_name, status}`. Store token.
4. `POST /device/heartbeat {device_id, token}` every 60s.
5. WS event → REST sync: `avatar_sync`→`POST /sync/avatars`; `alarm_sync`→`POST /sync/alarms`; `new_message`→`POST /sync/messages`; `hotword_start`/`hotword_stop`→arm/disarm local wake-word for avatar_id; `device_revoked`→wipe+show QR.
6. Jetson→server WS: `ping` q30s; `hotword_started`/`hotword_stopped` ACKs. Reconnect q5s, full sync on reconnect.

**KEY: `POST /sync/avatars {token, device_id}`** → `{active_avatar, listening_avatar_id, avatars[]}` where each avatar = `{avatar_id, avatar_name, avatar_persona, avatar_voice_link, avatar_idle_video_url, avatar_lip_movement_url}`. **idle_video = loop when idle; lip_movement = play when talking (lipsync); active_avatar = which to show.** This is the data our Jetson UI renders (idle loop center, swap to lip video on response).

**Other endpoints (mobile-app side, not Jetson):** Avatar: `POST /avatar/submit`, `GET /avatar/user-avatars`, `GET|PUT|DELETE /avatar/{request_id}`. Alarm: `POST /alarm/create`, `GET /alarm/user`, `GET /alarm/avatar/{avatar_id}`, `PUT|DELETE /alarm/{alarm_id}`. Message: `POST /message/create`, `GET /message/user`, `GET /message/avatar/{avatar_id}`, `GET|DELETE /message/{message_id}`. `POST /webhook/heygen-video-complete`. `GET /health`. Alarms/messages carry their own `video_url`.

**NOTE:** the WSL replica (E:\voice-ai-replica, [[project-voice-ai-replica]]) used a DIFFERENT vendored API where dev_4d8cf6bfdc83/user_d927e149 was claimed. THIS production API needs a fresh pairing. Spec saved local: `C:\Users\HP\jetson-kiosk\swagger-init.js`.
