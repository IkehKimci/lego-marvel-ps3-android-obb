/**
 * LEGO Marvel Super Heroes (BLUS-31161) — Free Cloudflare Workers + Durable Objects Relay
 *
 * Uses Durable Objects WebSocket Hibernation API + SQLite storage (100% Free Tier,
 * Jakarta / Singapore Edge < 20 ms latency, unlimited isolated Room Ports + Passwords!).
 *
 * Endpoint: wss://<your-worker>.workers.dev/ws?role=host|client&room=<port>&pass=<sha256hex>
 */

const MAGIC_LMV1 = 0x4C4D5631;
const PKT_STATUS = 7;

function makeStatusPacket(peerConnected, text) {
    const msg = new TextEncoder().encode(text || '');
    const buf = new ArrayBuffer(14 + msg.length);
    const dv = new DataView(buf);
    dv.setUint32(0, MAGIC_LMV1, false);
    dv.setUint8(4, PKT_STATUS);
    dv.setUint8(5, peerConnected ? 4 : 0);
    dv.setUint16(6, 0, false);
    dv.setUint16(8, 0, false);
    dv.setUint32(10, msg.length, false);
    new Uint8Array(buf, 14).set(msg);
    return buf;
}

export class CoopRoom {
    constructor(ctx, env) {
        this.ctx = ctx;
        this.env = env;
        this.passHash = null;
    }

    getPeer(role) {
        const list = this.ctx.getWebSockets(role);
        return list.length > 0 ? list[0] : null;
    }

    async fetch(request) {
        const url = new URL(request.url);
        const role = (url.searchParams.get('role') || '').toLowerCase();
        const roomId = (url.searchParams.get('room') || url.searchParams.get('port') || '').trim();
        const passHash = (url.searchParams.get('pass') || '').trim().toLowerCase();

        if (role !== 'host' && role !== 'client') {
            return new Response('Invalid role', { status: 400, headers: { 'X-Coop-Error': 'Role tidak valid' } });
        }
        if (!roomId || passHash.length !== 64 || !/^[0-9a-f]{64}$/.test(passHash)) {
            return new Response('Invalid room/pass', { status: 400, headers: { 'X-Coop-Error': 'Port/Password tidak valid' } });
        }

        const existingHost = this.getPeer('host');
        if (existingHost) {
            const savedPass = existingHost.deserializeAttachment()?.passHash || this.passHash;
            if (role === 'host') {
                if (savedPass && savedPass !== passHash) {
                    return new Response('Room in use', {
                        status: 409,
                        headers: { 'X-Coop-Error': `Port/Room ${roomId} sedang dipakai orang lain — ganti nomor Port` }
                    });
                }
                try { existingHost.close(1000, 'Replaced by reconnecting host'); } catch (_) {}
            } else {
                if (savedPass && savedPass !== passHash) {
                    return new Response('Wrong password', {
                        status: 403,
                        headers: { 'X-Coop-Error': 'Password salah!' }
                    });
                }
            }
        } else if (role === 'client') {
            return new Response('Host offline', {
                status: 404,
                headers: { 'X-Coop-Error': `Host HP 1 belum online di Port ${roomId}` }
            });
        }

        if (role === 'client') {
            const existingClient = this.getPeer('client');
            if (existingClient) {
                try { existingClient.close(1000, 'Replaced by new P2'); } catch (_) {}
            }
        } else {
            this.passHash = passHash;
        }

        const pair = new WebSocketPair();
        const [clientSock, serverSock] = Object.values(pair);
        this.ctx.acceptWebSocket(serverSock, [role]);
        serverSock.serializeAttachment({ role, roomId, passHash });

        const host = role === 'host' ? serverSock : this.getPeer('host');
        const client = role === 'client' ? serverSock : this.getPeer('client');
        if (host && client) {
            try { host.send(makeStatusPacket(true, 'Player 2 terhubung!')); } catch (_) {}
            try { client.send(makeStatusPacket(true, `Terhubung ke Room ${roomId}!`)); } catch (_) {}
        } else if (role === 'host') {
            try { serverSock.send(makeStatusPacket(false, `Room ${roomId} siap — menunggu HP 2`)); } catch (_) {}
        }

        return new Response(null, { status: 101, webSocket: clientSock });
    }

    webSocketMessage(ws, message) {
        const att = ws.deserializeAttachment() || {};
        const targetRole = att.role === 'host' ? 'client' : 'host';
        const peer = this.getPeer(targetRole);
        if (peer) {
            try { peer.send(message); } catch (_) {}
        }
    }

    webSocketClose(ws) {
        this.onDisconnect(ws);
    }

    webSocketError(ws) {
        this.onDisconnect(ws);
    }

    onDisconnect(ws) {
        const att = ws.deserializeAttachment() || {};
        if (att.role === 'host') {
            this.passHash = null;
            const client = this.getPeer('client');
            if (client) {
                try { client.send(makeStatusPacket(false, 'Host HP 1 terputus')); } catch (_) {}
                try { client.close(1000, 'Host disconnected'); } catch (_) {}
            }
        } else if (att.role === 'client') {
            const host = this.getPeer('host');
            if (host) {
                try { host.send(makeStatusPacket(false, 'Player 2 keluar')); } catch (_) {}
            }
        }
    }
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        if (url.pathname === '/ws') {
            const roomId = (url.searchParams.get('room') || url.searchParams.get('port') || '').trim();
            if (!roomId || roomId.length > 32) {
                return new Response('Missing room', { status: 400, headers: { 'X-Coop-Error': 'Port/Room kosong' } });
            }
            const id = env.COOP_ROOMS.idFromName('room-' + roomId);
            return env.COOP_ROOMS.get(id).fetch(request);
        }
        if (url.pathname === '/health' || url.pathname === '/status.json') {
            return new Response(JSON.stringify({ server: 'LEGO Marvel Co-op Relay (Cloudflare Edge)', status: 'online' }), {
                headers: { 'Content-Type': 'application/json' }
            });
        }
        return new Response(
            '<!doctype html><html><body style="background:#0d1117;color:#f0f6fc;font-family:sans-serif;padding:24px">' +
            '<h2>🎮 LEGO Marvel PS3 Online Co-op Relay (Cloudflare Edge)</h2>' +
            '<p>Status: <b>🟢 ONLINE</b></p>' +
            '<p>Masukkan alamat domain ini di Launcher Android HP 1 & HP 2, pilih nomor <b>Port / Room</b> (misal <code>31161</code>, <code>31162</code>, dst) dan <b>Password</b> yang sama!</p>' +
            '</body></html>',
            { headers: { 'Content-Type': 'text/html; charset=utf-8' } }
        );
    }
};
