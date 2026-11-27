'use strict';

/**
 * LEGO Marvel Super Heroes (BLUS-31161) — Multi-Room Online Co-op Relay Server
 *
 * ZERO external dependencies (Node.js built-ins only: http, net, crypto, url).
 * Supports BOTH:
 *   1. WebSocket Relay (ws:// / wss:// on PORT, e.g. Render.com / Koyeb / Railway / Fly.io)
 *      with unlimited virtual Room Ports (1..65535) + SHA-256 Password isolation!
 *   2. Multi-Port Raw TCP Relay (TCP_PORTS="31161-31170" or "31161,31162,31163")
 *      for VPS / PC / Termux / LAN multi-port hosting.
 *
 * Multiple pairs of players (HP 1 + HP 2) can play online at the same time on ONE server,
 * each using a different Port/Room number and their own Password.
 */

const http = require('http');
const net = require('net');
const crypto = require('crypto');

const HTTP_PORT = parseInt(process.env.PORT || '8080', 10);
const HOST_BIND = process.env.HOST || '0.0.0.0';
const TCP_PORTS_SPEC = (process.env.TCP_PORTS || '').trim(); // e.g. "31161-31170"
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || '100', 10);
const SERVER_NAME = process.env.SERVER_NAME || 'LEGO Marvel PS3 Co-op Relay';

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const MAGIC_LMV1 = 0x4C4D5631; // "LMV1"
const MAGIC_LMRL = 0x4C4D524C; // "LMRL"
const PKT_PING   = 5;
const PKT_STATUS = 7;

const startedAt = Date.now();
let totalRelayedBytes = 0;

/**
 * Room registry keyed by roomId (e.g. "31161").
 * value: { roomId, passHashHex, host: Endpoint|null, client: Endpoint|null, createdAt, lastActivity }
 */
const rooms = new Map();

function safeHashEqual(aHex, bHex) {
    if (typeof aHex !== 'string' || typeof bHex !== 'string') return false;
    const a = Buffer.from(aHex.toLowerCase(), 'utf8');
    const b = Buffer.from(bHex.toLowerCase(), 'utf8');
    if (a.length !== b.length) return false;
    return crypto.timingSafeEqual(a, b);
}

function makeLmvPacket(type, flags, w, h, payloadBuf) {
    const payload = payloadBuf || Buffer.alloc(0);
    const out = Buffer.allocUnsafe(14 + payload.length);
    out.writeUInt32BE(MAGIC_LMV1, 0);
    out.writeUInt8(type & 0xFF, 4);
    out.writeUInt8(flags & 0xFF, 5);
    out.writeUInt16BE(w & 0xFFFF, 6);
    out.writeUInt16BE(h & 0xFFFF, 8);
    out.writeUInt32BE(payload.length >>> 0, 10);
    if (payload.length > 0) payload.copy(out, 14);
    return out;
}

function makeStatusPacket(peerConnected, text) {
    const msg = Buffer.from(text || '', 'utf8');
    return makeLmvPacket(PKT_STATUS, peerConnected ? 4 : 0, 0, 0, msg);
}

function sendRelayData(ep, buf) {
    if (!ep || ep.closed) return;
    // Drop video/audio frames if peer's socket send queue is severely backed up (>512 KB)
    // so slow 4G links never accumulate multi-second lag or exhaust RAM.
    if (ep.socket && ep.socket.writableLength > 512 * 1024) {
        if (buf.length >= 14 && buf.readUInt32BE(0) === MAGIC_LMV1) {
            const type = buf.readUInt8(4);
            const flags = buf.readUInt8(5);
            // Keep CSD(1), keyframes(flags&1), INPUT(6), STATUS(7), PING(5); drop non-key P-frames/JPEG/Audio
            if ((type === 2 && (flags & 1) === 0) || type === 3 || type === 4) {
                return;
            }
        }
    }
    totalRelayedBytes += buf.length;
    ep.sendBinary(buf);
}

function cleanupRoomIfEmpty(room) {
    if (!room) return;
    if (!room.host && !room.client) {
        rooms.delete(room.roomId);
    }
}

function detachEndpoint(room, ep) {
    if (!room || !ep) return;
    if (room.host === ep) {
        room.host = null;
        console.log(`[room ${room.roomId}] HOST disconnected`);
        if (room.client && !room.client.closed) {
            try {
                room.client.sendBinary(makeStatusPacket(false, 'Host HP 1 terputus'));
                room.client.close('Host disconnected');
            } catch (_) {}
            room.client = null;
        }
    } else if (room.client === ep) {
        room.client = null;
        console.log(`[room ${room.roomId}] CLIENT disconnected`);
        if (room.host && !room.host.closed) {
            try {
                room.host.sendBinary(makeStatusPacket(false, 'Player 2 keluar'));
            } catch (_) {}
        }
    }
    cleanupRoomIfEmpty(room);
}

function joinRoom(roomId, role, passHashHex, ep) {
    const cleanId = String(roomId).trim();
    if (!cleanId || cleanId.length > 32) {
        return { ok: false, code: 'BAD', httpCode: 400, msg: 'Nomor Port/Room tidak valid' };
    }
    const normPass = (passHashHex || '').toLowerCase().trim();
    if (normPass.length !== 64 || !/^[0-9a-f]{64}$/.test(normPass)) {
        return { ok: false, code: 'BAD', httpCode: 400, msg: 'Format password hash tidak valid' };
    }

    let room = rooms.get(cleanId);

    if (role === 'host') {
        if (room && room.host && !room.host.closed) {
            if (!safeHashEqual(room.passHashHex, normPass)) {
                return {
                    ok: false,
                    code: 'IN',
                    httpCode: 409,
                    msg: `Port/Room ${cleanId} sedang dipakai pemain lain — pilih nomor Port lain!`
                };
            }
            // Same password reconnecting host: cleanly replace old socket
            const oldHost = room.host;
            room.host = null;
            oldHost.close('Replaced by reconnecting host');
        }
        if (!room) {
            if (rooms.size >= MAX_ROOMS) {
                return { ok: false, code: 'FULL', httpCode: 503, msg: 'Server penuh — coba lagi nanti' };
            }
            room = {
                roomId: cleanId,
                passHashHex: normPass,
                host: null,
                client: null,
                createdAt: Date.now(),
                lastActivity: Date.now()
            };
            rooms.set(cleanId, room);
        } else {
            room.passHashHex = normPass;
        }
        room.host = ep;
        room.lastActivity = Date.now();
        console.log(`[room ${cleanId}] HOST online (${ep.proto}, rooms=${rooms.size})`);
        if (room.client && !room.client.closed) {
            ep.sendBinary(makeStatusPacket(true, 'Player 2 terhubung!'));
            room.client.sendBinary(makeStatusPacket(true, 'Host HP 1 terhubung!'));
        } else {
            ep.sendBinary(makeStatusPacket(false, `Room ${cleanId} siap — menunggu HP 2`));
        }
        return { ok: true, room, msg: `Room ${cleanId} aktif` };
    }

    if (role === 'client') {
        if (!room || !room.host || room.host.closed) {
            return {
                ok: false,
                code: 'NO',
                httpCode: 404,
                msg: `Host HP 1 belum online di Port/Room ${cleanId}`
            };
        }
        if (!safeHashEqual(room.passHashHex, normPass)) {
            return {
                ok: false,
                code: 'PW',
                httpCode: 403,
                msg: 'Password salah!'
            };
        }
        if (room.client && !room.client.closed) {
            const oldClient = room.client;
            room.client = null;
            oldClient.close('Replaced by new P2 connection');
        }
        room.client = ep;
        room.lastActivity = Date.now();
        console.log(`[room ${cleanId}] CLIENT joined (${ep.proto})`);
        room.host.sendBinary(makeStatusPacket(true, 'Player 2 terhubung!'));
        ep.sendBinary(makeStatusPacket(true, `Terhubung ke Room ${cleanId}!`));
        return { ok: true, room, msg: `Terhubung ke Room ${cleanId}` };
    }

    return { ok: false, code: 'BAD', httpCode: 400, msg: 'Role tidak dikenal' };
}

function forwardPacket(room, role, payloadBuf) {
    if (!room) return;
    room.lastActivity = Date.now();
    const peer = (role === 'host') ? room.client : room.host;
    if (peer && !peer.closed) {
        sendRelayData(peer, payloadBuf);
    }
}

// ---------------------------------------------------------------------------
// Minimal RFC 6455 WebSocket Framing (Zero Dependencies)
// ---------------------------------------------------------------------------

function encodeWsFrame(opcode, payload) {
    const len = payload.length;
    let hdr;
    if (len < 126) {
        hdr = Buffer.allocUnsafe(2);
        hdr[0] = 0x80 | (opcode & 0x0F);
        hdr[1] = len;
    } else if (len < 65536) {
        hdr = Buffer.allocUnsafe(4);
        hdr[0] = 0x80 | (opcode & 0x0F);
        hdr[1] = 126;
        hdr.writeUInt16BE(len, 2);
    } else {
        hdr = Buffer.allocUnsafe(10);
        hdr[0] = 0x80 | (opcode & 0x0F);
        hdr[1] = 127;
        hdr.writeUInt32BE(Math.floor(len / 0x100000000), 2);
        hdr.writeUInt32BE(len >>> 0, 6);
    }
    return Buffer.concat([hdr, payload]);
}

function attachWebSocket(socket, headBuf, roomId, role, passHashHex) {
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 15000);

    const ep = {
        proto: 'ws',
        socket,
        closed: false,
        sendBinary(buf) {
            if (this.closed || socket.destroyed) return;
            try {
                socket.write(encodeWsFrame(0x02, buf));
            } catch (_) {
                this.close();
            }
        },
        close() {
            if (this.closed) return;
            this.closed = true;
            try { socket.write(encodeWsFrame(0x08, Buffer.alloc(0))); } catch (_) {}
            try { socket.destroy(); } catch (_) {}
        }
    };

    const res = joinRoom(roomId, role, passHashHex, ep);
    if (!res.ok) {
        // Should already have been checked before 101, safeguard anyway
        ep.close();
        return;
    }
    const room = res.room;

    let rxBuf = headBuf && headBuf.length > 0 ? Buffer.from(headBuf) : Buffer.alloc(0);
    let fragChunks = [];

    function processRx() {
        while (rxBuf.length >= 2) {
            const b0 = rxBuf[0];
            const b1 = rxBuf[1];
            const fin = (b0 & 0x80) !== 0;
            const opcode = b0 & 0x0F;
            const masked = (b1 & 0x80) !== 0;
            let len = b1 & 0x7F;
            let off = 2;

            if (len === 126) {
                if (rxBuf.length < 4) return;
                len = rxBuf.readUInt16BE(2);
                off = 4;
            } else if (len === 127) {
                if (rxBuf.length < 10) return;
                const hi = rxBuf.readUInt32BE(2);
                const lo = rxBuf.readUInt32BE(6);
                len = hi * 0x100000000 + lo;
                off = 10;
            }
            if (len > 4 * 1024 * 1024) {
                ep.close();
                return;
            }
            const maskLen = masked ? 4 : 0;
            if (rxBuf.length < off + maskLen + len) return;

            let payload = rxBuf.subarray(off + maskLen, off + maskLen + len);
            if (masked) {
                const mask = rxBuf.subarray(off, off + 4);
                const unmasked = Buffer.allocUnsafe(len);
                for (let i = 0; i < len; i++) {
                    unmasked[i] = payload[i] ^ mask[i & 3];
                }
                payload = unmasked;
            } else {
                payload = Buffer.from(payload);
            }
            rxBuf = rxBuf.subarray(off + maskLen + len);

            if (opcode === 0x08) { // Close
                ep.close();
                return;
            } else if (opcode === 0x09) { // Ping -> Pong
                try { socket.write(encodeWsFrame(0x0A, payload)); } catch (_) {}
            } else if (opcode === 0x0A) { // Pong
                room.lastActivity = Date.now();
            } else if (opcode === 0x02 || opcode === 0x01 || opcode === 0x00) {
                if (!fin) {
                    fragChunks.push(payload);
                } else {
                    const msg = fragChunks.length > 0
                        ? Buffer.concat([...fragChunks, payload])
                        : payload;
                    fragChunks = [];
                    forwardPacket(room, role, msg);
                }
            }
        }
    }

    socket.on('data', (chunk) => {
        rxBuf = rxBuf.length > 0 ? Buffer.concat([rxBuf, chunk]) : chunk;
        processRx();
    });
    const onDone = () => {
        ep.closed = true;
        detachEndpoint(room, ep);
    };
    socket.on('close', onDone);
    socket.on('error', onDone);
    if (rxBuf.length > 0) processRx();
}

// ---------------------------------------------------------------------------
// HTTP Status Dashboard + WebSocket Upgrade Server
// ---------------------------------------------------------------------------

function buildStatusJson() {
    const list = [];
    for (const r of rooms.values()) {
        list.push({
            port: r.roomId,
            hostOnline: Boolean(r.host && !r.host.closed),
            player2Online: Boolean(r.client && !r.client.closed),
            ageSec: Math.floor((Date.now() - r.createdAt) / 1000)
        });
    }
    return {
        server: SERVER_NAME,
        status: 'online',
        uptimeSec: Math.floor((Date.now() - startedAt) / 1000),
        activeRooms: list.length,
        maxRooms: MAX_ROOMS,
        relayedMB: Number((totalRelayedBytes / (1024 * 1024)).toFixed(2)),
        tcpPorts: TCP_PORTS_SPEC || '(WebSocket mode)',
        rooms: list
    };
}

const httpServer = http.createServer((req, res) => {
    const u = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
    if (u.pathname === '/status.json' || u.pathname === '/health') {
        const body = JSON.stringify(buildStatusJson(), null, 2);
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(body);
        return;
    }
    const st = buildStatusJson();
    const rows = st.rooms.length === 0
        ? '<tr><td colspan="4" style="color:#8b949e;text-align:center;">Belum ada room aktif — masukkan alamat server ini di Launcher LEGO Marvel!</td></tr>'
        : st.rooms.map(r => `<tr>
            <td><b>${r.port}</b></td>
            <td>${r.hostOnline ? '🟢 Online' : '⚪ Offline'}</td>
            <td>${r.player2Online ? '🟢 P2 Bergabung' : '🟡 Menunggu P2'}</td>
            <td>${r.ageSec}s</td>
          </tr>`).join('');
    const html = `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${SERVER_NAME}</title>
<style>
  body{background:#0d1117;color:#f0f6fc;font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px}
  .card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:16px;margin-bottom:14px}
  h1{margin:0 0 6px;font-size:20px;color:#58a6ff}
  table{width:100%;border-collapse:collapse;margin-top:8px}
  th,td{padding:8px;border-bottom:1px solid #30363d;text-align:left;font-size:13px}
  code{background:#21262d;padding:2px 6px;border-radius:6px;color:#7ee787}
</style></head><body>
  <div class="card">
    <h1>🎮 ${SERVER_NAME}</h1>
    <div>Status: <b>🟢 ONLINE</b> • Uptime: <b>${st.uptimeSec}s</b> • Room Aktif: <b>${st.activeRooms}/${st.maxRooms}</b> • Trafik: <b>${st.relayedMB} MB</b></div>
  </div>
  <div class="card">
    <b>Cara Main Online 2 HP (Beda Wi-Fi / 4G / 5G):</b>
    <ol style="font-size:13px;line-height:1.6;color:#c9d1d9">
      <li>Pilih <b>Mode Koneksi → Server Online (Relay)</b> di Launcher HP 1 & HP 2.</li>
      <li>Isi <b>Alamat Server</b> dengan domain server ini (tanpa spasi).</li>
      <li>Pilih <b>Port / Nomor Room</b> bebas (misal <code>31161</code>, <code>31162</code>, <code>7777</code>) & buat <b>Password Room</b>.</li>
      <li>HP 1 masuk game, lalu HP 2 (tanpa OBB!) masukkan Port & Password yang sama lalu klik <b>GABUNG PLAYER 2</b>!</li>
    </ol>
  </div>
  <div class="card">
    <b>Daftar Room Aktif (Password Terenkripsi — Tidak Ditampilkan):</b>
    <table><tr><th>Port / Room</th><th>Host (HP 1)</th><th>Client (HP 2)</th><th>Durasi</th></tr>${rows}</table>
  </div>
</body></html>`;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
});

httpServer.on('upgrade', (req, socket, head) => {
    const u = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
    if (u.pathname !== '/ws') {
        socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
        socket.destroy();
        return;
    }
    const role = (u.searchParams.get('role') || '').toLowerCase();
    const roomId = (u.searchParams.get('room') || u.searchParams.get('port') || '').trim();
    const passHash = (u.searchParams.get('pass') || '').trim().toLowerCase();

    // Pre-validate room & password BEFORE sending 101 so Android receives exact HTTP 403/404/409 reason!
    if (role !== 'host' && role !== 'client') {
        socket.write('HTTP/1.1 400 Bad Request\r\nX-Coop-Error: Role tidak valid\r\nConnection: close\r\n\r\n');
        socket.destroy();
        return;
    }
    if (!roomId || passHash.length !== 64 || !/^[0-9a-f]{64}$/.test(passHash)) {
        socket.write('HTTP/1.1 400 Bad Request\r\nX-Coop-Error: Port/Password tidak valid\r\nConnection: close\r\n\r\n');
        socket.destroy();
        return;
    }

    const existing = rooms.get(roomId);
    if (role === 'host') {
        if (existing && existing.host && !existing.host.closed && !safeHashEqual(existing.passHashHex, passHash)) {
            socket.write('HTTP/1.1 409 Conflict\r\nX-Coop-Error: Port/Room dipakai orang lain, ganti nomor Port\r\nConnection: close\r\n\r\n');
            socket.destroy();
            return;
        }
        if (!existing && rooms.size >= MAX_ROOMS) {
            socket.write('HTTP/1.1 503 Service Unavailable\r\nX-Coop-Error: Server penuh\r\nConnection: close\r\n\r\n');
            socket.destroy();
            return;
        }
    } else {
        if (!existing || !existing.host || existing.host.closed) {
            socket.write(`HTTP/1.1 404 Not Found\r\nX-Coop-Error: Host HP 1 belum online di Port ${roomId}\r\nConnection: close\r\n\r\n`);
            socket.destroy();
            return;
        }
        if (!safeHashEqual(existing.passHashHex, passHash)) {
            socket.write('HTTP/1.1 403 Forbidden\r\nX-Coop-Error: Password salah!\r\nConnection: close\r\n\r\n');
            socket.destroy();
            return;
        }
    }

    const wsKey = req.headers['sec-websocket-key'];
    if (!wsKey) {
        socket.write('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
        socket.destroy();
        return;
    }
    const accept = crypto.createHash('sha1').update(wsKey + WS_GUID).digest('base64');
    socket.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
    );
    attachWebSocket(socket, head, roomId, role, passHash);
});

httpServer.listen(HTTP_PORT, HOST_BIND, () => {
    console.log(`[relay] HTTP + WebSocket server listening on ${HOST_BIND}:${HTTP_PORT}`);
});

// ---------------------------------------------------------------------------
// Multi-Port Raw TCP Relay (for VPS / PC / Fly.io / Termux)
// Handshake (44 bytes):
//   [0..3]  "LMRL" (0x4C4D524C)
//   [4]     version = 1
//   [5]     role (1 = HOST, 2 = CLIENT)
//   [6..7]  u16 BE roomPort (defaults to listening port if 0)
//   [8..39] 32 bytes SHA-256(password)
//   [40..43] reserved = 0
// Reply: 2-byte status ("OK"/"NO"/"PW"/"IN"/"ER") + u16 BE msgLen + UTF-8 msg
// After "OK", both sides stream length-prefixed LMV1 binary packets!
// ---------------------------------------------------------------------------

function sendTcpHandshakeReply(socket, code2, msg) {
    const m = Buffer.from(msg || '', 'utf8');
    const out = Buffer.allocUnsafe(4 + m.length);
    out.write(code2.slice(0, 2), 0, 2, 'ascii');
    out.writeUInt16BE(m.length, 2);
    m.copy(out, 4);
    socket.write(out);
}

function startRawTcpListener(listenPort) {
    const srv = net.createServer((socket) => {
        socket.setNoDelay(true);
        socket.setKeepAlive(true, 15000);

        let handshakeDone = false;
        let rxBuf = Buffer.alloc(0);
        let room = null;
        let role = null;
        const pendingSends = [];

        const ep = {
            proto: `tcp:${listenPort}`,
            socket,
            closed: false,
            sendBinary(buf) {
                if (this.closed || socket.destroyed) return;
                if (!handshakeDone) {
                    pendingSends.push(buf);
                    return;
                }
                try { socket.write(buf); } catch (_) { this.close(); }
            },
            close() {
                if (this.closed) return;
                this.closed = true;
                try { socket.destroy(); } catch (_) {}
            }
        };

        function drainLmvPackets() {
            while (rxBuf.length >= 14) {
                if (rxBuf.readUInt32BE(0) !== MAGIC_LMV1) {
                    ep.close();
                    return;
                }
                const payloadLen = rxBuf.readUInt32BE(10);
                if (payloadLen > 4 * 1024 * 1024) {
                    ep.close();
                    return;
                }
                if (rxBuf.length < 14 + payloadLen) return;
                const pkt = Buffer.from(rxBuf.subarray(0, 14 + payloadLen));
                rxBuf = rxBuf.subarray(14 + payloadLen);
                forwardPacket(room, role, pkt);
            }
        }

        socket.on('data', (chunk) => {
            rxBuf = rxBuf.length > 0 ? Buffer.concat([rxBuf, chunk]) : chunk;
            if (!handshakeDone) {
                if (rxBuf.length < 44) return;
                const magic = rxBuf.readUInt32BE(0);
                if (magic !== MAGIC_LMRL) {
                    sendTcpHandshakeReply(socket, 'ER', 'Bad relay handshake');
                    ep.close();
                    return;
                }
                const roleByte = rxBuf.readUInt8(5);
                const reqRoom = rxBuf.readUInt16BE(6) || listenPort;
                const passHex = rxBuf.subarray(8, 40).toString('hex');
                rxBuf = rxBuf.subarray(44);
                role = (roleByte === 1) ? 'host' : (roleByte === 2 ? 'client' : '');

                const res = joinRoom(`${listenPort}:${reqRoom}`, role, passHex, ep);
                if (!res.ok) {
                    sendTcpHandshakeReply(socket, res.code.slice(0, 2).padEnd(2, ' '), res.msg);
                    ep.close();
                    return;
                }
                room = res.room;
                sendTcpHandshakeReply(socket, 'OK', res.msg);
                handshakeDone = true;
                for (const b of pendingSends) ep.sendBinary(b);
                pendingSends.length = 0;
            }
            if (handshakeDone && rxBuf.length > 0) {
                drainLmvPackets();
            }
        });

        const onDone = () => {
            ep.closed = true;
            if (room) detachEndpoint(room, ep);
        };
        socket.on('close', onDone);
        socket.on('error', onDone);
    });

    srv.on('error', (err) => {
        console.error(`[relay] TCP port ${listenPort} error: ${err.message}`);
    });
    srv.listen(listenPort, HOST_BIND, () => {
        console.log(`[relay] Raw TCP room server listening on ${HOST_BIND}:${listenPort}`);
    });
}

function parsePortSpec(spec) {
    const ports = new Set();
    if (!spec) return [];
    for (const part of spec.split(',')) {
        const s = part.trim();
        if (!s) continue;
        const dash = s.indexOf('-');
        if (dash > 0) {
            const a = parseInt(s.slice(0, dash), 10);
            const b = parseInt(s.slice(dash + 1), 10);
            if (a > 0 && b >= a && b <= 65535 && (b - a) <= 100) {
                for (let p = a; p <= b; p++) ports.add(p);
            }
        } else {
            const p = parseInt(s, 10);
            if (p > 0 && p <= 65535) ports.add(p);
        }
    }
    return Array.from(ports);
}

for (const p of parsePortSpec(TCP_PORTS_SPEC)) {
    if (p !== HTTP_PORT) startRawTcpListener(p);
}

// Keepalive ping every 20s so free cloud proxies (Render / Cloudflare / Nginx) never drop idle rooms
setInterval(() => {
    const ping = makeLmvPacket(PKT_PING, 0, 0, 0, null);
    for (const r of rooms.values()) {
        if (r.host && !r.host.closed) r.host.sendBinary(ping);
        if (r.client && !r.client.closed) r.client.sendBinary(ping);
    }
}, 20000);
