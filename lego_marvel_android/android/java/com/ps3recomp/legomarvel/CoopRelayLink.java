package com.ps3recomp.legomarvel;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.Locale;

import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Unified Online Relay Transport (zero external dependencies).
 *
 * Supports BOTH:
 *   1. WebSocket over TLS / plain TCP (wss:// / ws://) for free cloud relays
 *      (Render.com Singapore, Cloudflare Workers, Koyeb, Railway, Fly.io).
 *   2. Multi-Port Raw TCP Relay (tcp://host:port) with 44-byte LMRL handshake
 *      for self-hosted VPS / PC / Termux servers.
 *
 * All video (H.264/JPEG), audio (24 kHz Stereo IMA-ADPCM), controller inputs (LMP2),
 * keepalives, and room status messages travel over ONE single outbound connection
 * (100% firewall/CGNAT friendly, SHA-256 room password isolation).
 */
public class CoopRelayLink implements java.io.Closeable {
    public static final int MAGIC_LMV1 = 0x4C4D5631; // "LMV1"
    public static final int MAGIC_LMRL = 0x4C4D524C; // "LMRL"

    public static final int PKT_CSD    = 1;
    public static final int PKT_H264   = 2;
    public static final int PKT_JPEG   = 3;
    public static final int PKT_AUDIO  = 4;
    public static final int PKT_PING   = 5;
    public static final int PKT_INPUT  = 6;
    public static final int PKT_STATUS = 7;

    public static class Packet {
        public int type;
        public int flags;
        public int w;
        public int h;
        public int len;
        public byte[] data = new byte[256 * 1024];

        public void ensureCapacity(int need) {
            if (need > data.length) {
                data = new byte[Math.max(need + 16384, data.length * 2)];
            }
        }
    }

    public static class EndpointSpec {
        public final boolean useWebSocket;
        public final boolean useTls;
        public final String host;
        public final int connectPort;
        public final int roomPort;
        public final String wsPath;

        EndpointSpec(boolean useWebSocket, boolean useTls, String host, int connectPort, int roomPort, String wsPath) {
            this.useWebSocket = useWebSocket;
            this.useTls = useTls;
            this.host = host;
            this.connectPort = connectPort;
            this.roomPort = roomPort;
            this.wsPath = wsPath;
        }
    }

    private final Socket socket;
    private final DataInputStream dis;
    private final OutputStream rawOut;
    private final boolean useWebSocket;
    private final Object txLock = new Object();
    private final SecureRandom rng = new SecureRandom();
    private byte[] txBuf = new byte[64 * 1024];
    private volatile boolean closed = false;

    private CoopRelayLink(Socket socket, DataInputStream dis, OutputStream rawOut, boolean useWebSocket) {
        this.socket = socket;
        this.dis = dis;
        this.rawOut = rawOut;
        this.useWebSocket = useWebSocket;
    }

    public static int parsePortOrDefault(String portStr, int defPort) {
        if (portStr == null) return defPort;
        try {
            int p = Integer.parseInt(portStr.trim());
            if (p >= 1 && p <= 65535) return p;
        } catch (NumberFormatException ignored) {
        }
        return defPort;
    }

    private static boolean looksLikeIpv4(String h) {
        if (h == null || h.isEmpty()) return false;
        String[] parts = h.split("\\.");
        if (parts.length != 4) return false;
        for (String p : parts) {
            if (p.isEmpty() || p.length() > 3) return false;
            for (int i = 0; i < p.length(); i++) {
                char c = p.charAt(i);
                if (c < '0' || c > '9') return false;
            }
        }
        return true;
    }

    public static EndpointSpec parseTarget(String rawServer, String roomPortStr) {
        String s = (rawServer != null) ? rawServer.trim() : "";
        int roomPort = parsePortOrDefault(roomPortStr, 31161);

        boolean explicitWs = false;
        boolean explicitTcp = false;
        boolean useTls = false;

        String low = s.toLowerCase(Locale.US);
        if (low.startsWith("wss://") || low.startsWith("https://")) {
            explicitWs = true;
            useTls = true;
            s = s.substring(s.indexOf("://") + 3);
        } else if (low.startsWith("ws://") || low.startsWith("http://")) {
            explicitWs = true;
            useTls = false;
            s = s.substring(s.indexOf("://") + 3);
        } else if (low.startsWith("tcp://")) {
            explicitTcp = true;
            useTls = false;
            s = s.substring(6);
        }

        // Strip trailing path (keep custom path if provided, otherwise /ws)
        String wsPath = "/ws";
        int slash = s.indexOf('/');
        if (slash >= 0) {
            String pathPart = s.substring(slash).trim();
            s = s.substring(0, slash);
            if (pathPart.length() > 1 && !pathPart.equals("/")) {
                wsPath = pathPart.endsWith("/ws") ? pathPart : (pathPart.endsWith("/") ? pathPart + "ws" : pathPart);
            }
        }

        String host = s;
        int explicitPort = -1;
        int colon = s.lastIndexOf(':');
        if (colon > 0 && s.indexOf(']') < colon) {
            try {
                explicitPort = Integer.parseInt(s.substring(colon + 1));
                host = s.substring(0, colon);
            } catch (NumberFormatException ignored) {
            }
        }

        if (explicitTcp) {
            int cPort = (explicitPort > 0) ? explicitPort : roomPort;
            return new EndpointSpec(false, false, host, cPort, roomPort, wsPath);
        }
        if (explicitWs) {
            int cPort = (explicitPort > 0) ? explicitPort : (useTls ? 443 : 80);
            return new EndpointSpec(true, useTls, host, cPort, roomPort, wsPath);
        }

        // Auto-detect: bare IPv4 -> Raw TCP multi-port relay; domain -> TLS WebSocket (443)
        if (looksLikeIpv4(host) || "localhost".equalsIgnoreCase(host)) {
            int cPort = (explicitPort > 0) ? explicitPort : roomPort;
            return new EndpointSpec(false, false, host, cPort, roomPort, wsPath);
        } else {
            int cPort = (explicitPort > 0) ? explicitPort : 443;
            boolean tls = (cPort == 443 || explicitPort <= 0);
            return new EndpointSpec(true, tls, host, cPort, roomPort, wsPath);
        }
    }

    public static byte[] sha256Bytes(String password) {
        String p = (password != null) ? password : "";
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return md.digest(("LMCOOP_BLUS31161:" + p).getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    public static String sha256Hex(String password) {
        byte[] hash = sha256Bytes(password);
        StringBuilder sb = new StringBuilder(64);
        for (byte b : hash) {
            sb.append(String.format(Locale.US, "%02x", b & 0xFF));
        }
        return sb.toString();
    }

    /**
     * Connects to the Online Relay Server as either Host ("host") or Client ("client"),
     * authenticated by (roomPort, password).
     */
    public static CoopRelayLink connect(String serverAddr, String roomPortStr, String password,
                                        boolean isHost, int connectTimeoutMs, int readTimeoutMs) throws IOException {
        EndpointSpec spec = parseTarget(serverAddr, roomPortStr);
        if (spec.host == null || spec.host.isEmpty()) {
            throw new IOException("Alamat server kosong");
        }

        Socket sock = null;
        try {
            if (spec.useWebSocket && spec.useTls) {
                SSLSocketFactory sf = (SSLSocketFactory) SSLSocketFactory.getDefault();
                Socket raw = new Socket();
                raw.connect(new InetSocketAddress(spec.host, spec.connectPort), connectTimeoutMs);
                SSLSocket ssl = (SSLSocket) sf.createSocket(raw, spec.host, spec.connectPort, true);
                try {
                    SSLParameters params = ssl.getSSLParameters();
                    params.setServerNames(java.util.Collections.singletonList(new SNIHostName(spec.host)));
                    ssl.setSSLParameters(params);
                } catch (Throwable ignored) {
                }
                ssl.setSoTimeout(connectTimeoutMs);
                ssl.startHandshake();
                sock = ssl;
            } else {
                sock = new Socket();
                sock.connect(new InetSocketAddress(spec.host, spec.connectPort), connectTimeoutMs);
            }
            sock.setTcpNoDelay(true);
            sock.setSendBufferSize(128 * 1024);
            sock.setReceiveBufferSize(256 * 1024);
            sock.setSoTimeout(connectTimeoutMs);

            DataInputStream dis = new DataInputStream(new BufferedInputStream(sock.getInputStream(), 65536));
            OutputStream out = new BufferedOutputStream(sock.getOutputStream(), 65536);

            if (spec.useWebSocket) {
                performWebSocketHandshake(dis, out, spec, isHost, password);
            } else {
                performRawTcpHandshake(dis, out, spec, isHost, password);
            }

            sock.setSoTimeout(readTimeoutMs);
            return new CoopRelayLink(sock, dis, out, spec.useWebSocket);
        } catch (IOException e) {
            closeQuietly(sock);
            throw e;
        } catch (Throwable t) {
            closeQuietly(sock);
            throw new IOException(t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName(), t);
        }
    }

    private static void performWebSocketHandshake(DataInputStream dis, OutputStream out,
                                                  EndpointSpec spec, boolean isHost, String password) throws IOException {
        byte[] nonce = new byte[16];
        new SecureRandom().nextBytes(nonce);
        String wsKey = Base64.getEncoder().encodeToString(nonce);

        String role = isHost ? "host" : "client";
        String passHex = sha256Hex(password);
        String fullPath = spec.wsPath
                + "?role=" + role
                + "&room=" + URLEncoder.encode(String.valueOf(spec.roomPort), "UTF-8")
                + "&pass=" + passHex;
        String hostHdr = ((spec.useTls && spec.connectPort == 443) || (!spec.useTls && spec.connectPort == 80))
                ? spec.host : (spec.host + ":" + spec.connectPort);

        String req = "GET " + fullPath + " HTTP/1.1\r\n"
                + "Host: " + hostHdr + "\r\n"
                + "Upgrade: websocket\r\n"
                + "Connection: Upgrade\r\n"
                + "Sec-WebSocket-Key: " + wsKey + "\r\n"
                + "Sec-WebSocket-Version: 13\r\n"
                + "User-Agent: LegoMarvelPS3-Android/0.3\r\n\r\n";
        out.write(req.getBytes(StandardCharsets.US_ASCII));
        out.flush();

        String statusLine = readAsciiLine(dis);
        if (statusLine == null || statusLine.isEmpty()) {
            throw new IOException("Server menutup koneksi saat handshake");
        }
        int statusCode = 0;
        String[] parts = statusLine.split(" ");
        if (parts.length >= 2) {
            try { statusCode = Integer.parseInt(parts[1].trim()); } catch (NumberFormatException ignored) {}
        }
        String customErr = null;
        for (;;) {
            String line = readAsciiLine(dis);
            if (line == null || line.isEmpty()) break;
            int idx = line.indexOf(':');
            if (idx > 0) {
                String k = line.substring(0, idx).trim();
                if ("x-coop-error".equalsIgnoreCase(k)) {
                    customErr = line.substring(idx + 1).trim();
                }
            }
        }

        if (statusCode != 101) {
            if (customErr != null && !customErr.isEmpty()) {
                throw new IOException(customErr);
            }
            if (statusCode == 403) throw new IOException("Password salah!");
            if (statusCode == 404) throw new IOException("Host HP 1 belum online di Port " + spec.roomPort);
            if (statusCode == 409) throw new IOException("Port " + spec.roomPort + " dipakai orang lain — ganti nomor Port!");
            if (statusCode == 502 || statusCode == 503) throw new IOException("Server sedang bangun (tunggu 15 detik lalu coba lagi)");
            throw new IOException("HTTP " + statusCode + " (" + statusLine + ")");
        }
    }

    private static void performRawTcpHandshake(DataInputStream dis, OutputStream out,
                                               EndpointSpec spec, boolean isHost, String password) throws IOException {
        byte[] hdr = new byte[44];
        hdr[0] = 'L'; hdr[1] = 'M'; hdr[2] = 'R'; hdr[3] = 'L';
        hdr[4] = 1; // version
        hdr[5] = (byte) (isHost ? 1 : 2);
        hdr[6] = (byte) ((spec.roomPort >>> 8) & 0xFF);
        hdr[7] = (byte) (spec.roomPort & 0xFF);
        byte[] ph = sha256Bytes(password);
        System.arraycopy(ph, 0, hdr, 8, 32);
        out.write(hdr);
        out.flush();

        byte[] code = new byte[2];
        dis.readFully(code);
        int msgLen = dis.readUnsignedShort();
        String msg = "";
        if (msgLen > 0 && msgLen <= 1024) {
            byte[] mb = new byte[msgLen];
            dis.readFully(mb);
            msg = new String(mb, StandardCharsets.UTF_8);
        }
        String c = new String(code, StandardCharsets.US_ASCII);
        if (!"OK".equals(c)) {
            if (!msg.isEmpty()) throw new IOException(msg);
            if ("PW".equals(c)) throw new IOException("Password salah!");
            if ("NO".equals(c)) throw new IOException("Host HP 1 belum online di Port " + spec.roomPort);
            if ("IN".equals(c)) throw new IOException("Port " + spec.roomPort + " sedang dipakai orang lain!");
            throw new IOException("Server menolak (" + c + ")");
        }
    }

    private static String readAsciiLine(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder(128);
        int b;
        while ((b = in.read()) != -1) {
            if (b == '\n') break;
            if (b != '\r') sb.append((char) b);
            if (sb.length() > 4096) throw new IOException("HTTP header too long");
        }
        if (b == -1 && sb.length() == 0) return null;
        return sb.toString();
    }

    /**
     * Quick reachability check for the "Tes Server" button in LauncherActivity.
     */
    public static String testServerHealth(String serverAddr, String roomPortStr) throws IOException {
        EndpointSpec spec = parseTarget(serverAddr, roomPortStr);
        if (spec.host == null || spec.host.isEmpty()) {
            throw new IOException("Alamat server belum diisi");
        }
        long t0 = System.currentTimeMillis();
        Socket sock = null;
        try {
            if (spec.useWebSocket && spec.useTls) {
                SSLSocketFactory sf = (SSLSocketFactory) SSLSocketFactory.getDefault();
                Socket raw = new Socket();
                raw.connect(new InetSocketAddress(spec.host, spec.connectPort), 8000);
                SSLSocket ssl = (SSLSocket) sf.createSocket(raw, spec.host, spec.connectPort, true);
                try {
                    SSLParameters params = ssl.getSSLParameters();
                    params.setServerNames(java.util.Collections.singletonList(new SNIHostName(spec.host)));
                    ssl.setSSLParameters(params);
                } catch (Throwable ignored) {
                }
                ssl.setSoTimeout(8000);
                ssl.startHandshake();
                sock = ssl;
            } else {
                sock = new Socket();
                sock.connect(new InetSocketAddress(spec.host, spec.connectPort), 6000);
                sock.setSoTimeout(6000);
            }
            if (spec.useWebSocket) {
                String req = "GET /health HTTP/1.1\r\nHost: " + spec.host + "\r\nConnection: close\r\n\r\n";
                sock.getOutputStream().write(req.getBytes(StandardCharsets.US_ASCII));
                sock.getOutputStream().flush();
                String status = readAsciiLine(sock.getInputStream());
                long ms = System.currentTimeMillis() - t0;
                if (status != null && status.contains("200")) {
                    return "ONLINE (" + ms + " ms • WSS)";
                }
                throw new IOException("Respon server: " + status);
            } else {
                long ms = System.currentTimeMillis() - t0;
                return "ONLINE (" + ms + " ms • TCP :" + spec.connectPort + ")";
            }
        } finally {
            closeQuietly(sock);
        }
    }

    // ---- Packet Framing (Thread-Safe TX, Streaming RX) ----

    public void sendPacket(int type, int flags, int w, int h, byte[] data, int off, int len) throws IOException {
        if (closed) throw new IOException("Relay link closed");
        int payloadLen = (data != null && len > 0) ? len : 0;
        int total = 14 + payloadLen;
        synchronized (txLock) {
            if (closed) throw new IOException("Relay link closed");
            if (txBuf.length < total + 16) {
                txBuf = new byte[Math.max(total + 4096, txBuf.length * 2)];
            }
            int pos = 0;
            if (useWebSocket) {
                // Masked RFC 6455 Binary Frame (client -> server MUST be masked)
                txBuf[pos++] = (byte) 0x82; // FIN | Binary
                if (total < 126) {
                    txBuf[pos++] = (byte) (0x80 | total);
                } else if (total < 65536) {
                    txBuf[pos++] = (byte) (0x80 | 126);
                    txBuf[pos++] = (byte) ((total >>> 8) & 0xFF);
                    txBuf[pos++] = (byte) (total & 0xFF);
                } else {
                    txBuf[pos++] = (byte) (0x80 | 127);
                    txBuf[pos++] = 0; txBuf[pos++] = 0; txBuf[pos++] = 0; txBuf[pos++] = 0;
                    txBuf[pos++] = (byte) ((total >>> 24) & 0xFF);
                    txBuf[pos++] = (byte) ((total >>> 16) & 0xFF);
                    txBuf[pos++] = (byte) ((total >>> 8) & 0xFF);
                    txBuf[pos++] = (byte) (total & 0xFF);
                }
                int maskOff = pos;
                int m = rng.nextInt();
                txBuf[pos++] = (byte) (m & 0xFF);
                txBuf[pos++] = (byte) ((m >>> 8) & 0xFF);
                txBuf[pos++] = (byte) ((m >>> 16) & 0xFF);
                txBuf[pos++] = (byte) ((m >>> 24) & 0xFF);

                int pStart = pos;
                writeLmvHeader(txBuf, pos, type, flags, w, h, payloadLen);
                pos += 14;
                if (payloadLen > 0) {
                    System.arraycopy(data, off, txBuf, pos, payloadLen);
                    pos += payloadLen;
                }
                byte m0 = txBuf[maskOff], m1 = txBuf[maskOff + 1], m2 = txBuf[maskOff + 2], m3 = txBuf[maskOff + 3];
                for (int i = 0; i < total; i++) {
                    int idx = pStart + i;
                    switch (i & 3) {
                        case 0: txBuf[idx] ^= m0; break;
                        case 1: txBuf[idx] ^= m1; break;
                        case 2: txBuf[idx] ^= m2; break;
                        default: txBuf[idx] ^= m3; break;
                    }
                }
                rawOut.write(txBuf, 0, pos);
                rawOut.flush();
            } else {
                writeLmvHeader(txBuf, 0, type, flags, w, h, payloadLen);
                if (payloadLen > 0) {
                    System.arraycopy(data, off, txBuf, 14, payloadLen);
                }
                rawOut.write(txBuf, 0, total);
                rawOut.flush();
            }
        }
    }

    private void sendWsPong(byte[] body, int len) {
        synchronized (txLock) {
            if (closed || !useWebSocket) return;
            try {
                int l = Math.min(len, 125);
                byte[] frame = new byte[2 + 4 + l];
                frame[0] = (byte) 0x8A; // FIN | Pong
                frame[1] = (byte) (0x80 | l);
                int m = rng.nextInt();
                frame[2] = (byte) (m & 0xFF);
                frame[3] = (byte) ((m >>> 8) & 0xFF);
                frame[4] = (byte) ((m >>> 16) & 0xFF);
                frame[5] = (byte) ((m >>> 24) & 0xFF);
                for (int i = 0; i < l; i++) {
                    frame[6 + i] = (byte) (body[i] ^ frame[2 + (i & 3)]);
                }
                rawOut.write(frame);
                rawOut.flush();
            } catch (Throwable ignored) {
            }
        }
    }

    private static void writeLmvHeader(byte[] dst, int off, int type, int flags, int w, int h, int len) {
        dst[off]      = 'L';
        dst[off + 1]  = 'M';
        dst[off + 2]  = 'V';
        dst[off + 3]  = '1';
        dst[off + 4]  = (byte) (type & 0xFF);
        dst[off + 5]  = (byte) (flags & 0xFF);
        dst[off + 6]  = (byte) ((w >>> 8) & 0xFF);
        dst[off + 7]  = (byte) (w & 0xFF);
        dst[off + 8]  = (byte) ((h >>> 8) & 0xFF);
        dst[off + 9]  = (byte) (h & 0xFF);
        dst[off + 10] = (byte) ((len >>> 24) & 0xFF);
        dst[off + 11] = (byte) ((len >>> 16) & 0xFF);
        dst[off + 12] = (byte) ((len >>> 8) & 0xFF);
        dst[off + 13] = (byte) (len & 0xFF);
    }

    private byte[] wsScratch = new byte[64 * 1024];

    public boolean readPacket(Packet out) throws IOException {
        if (!useWebSocket) {
            int magic = dis.readInt();
            if (magic != MAGIC_LMV1) throw new IOException("Bad stream magic");
            out.type = dis.readUnsignedByte();
            out.flags = dis.readUnsignedByte();
            out.w = dis.readUnsignedShort();
            out.h = dis.readUnsignedShort();
            out.len = dis.readInt();
            if (out.len < 0 || out.len > 4 * 1024 * 1024) throw new IOException("Invalid pkt len " + out.len);
            if (out.len > 0) {
                out.ensureCapacity(out.len);
                dis.readFully(out.data, 0, out.len);
            }
            return true;
        }

        // WebSocket mode: read next binary frame (automatically handle Ping/Pong/Close/Fragmentation)
        for (;;) {
            int b0 = dis.readUnsignedByte();
            int b1 = dis.readUnsignedByte();
            boolean fin = (b0 & 0x80) != 0;
            int opcode = b0 & 0x0F;
            boolean masked = (b1 & 0x80) != 0;
            long len = b1 & 0x7F;
            if (len == 126) {
                len = dis.readUnsignedShort();
            } else if (len == 127) {
                len = dis.readLong();
            }
            if (len < 0 || len > 4 * 1024 * 1024) throw new IOException("WS frame too large: " + len);
            int flen = (int) len;
            byte[] mask = null;
            if (masked) {
                mask = new byte[4];
                dis.readFully(mask);
            }
            if (wsScratch.length < flen) wsScratch = new byte[flen + 16384];
            if (flen > 0) {
                dis.readFully(wsScratch, 0, flen);
                if (masked) {
                    for (int i = 0; i < flen; i++) wsScratch[i] ^= mask[i & 3];
                }
            }

            if (opcode == 0x08) { // Close
                throw new EOFException("Server closed room connection");
            }
            if (opcode == 0x09) { // Ping -> Pong
                sendWsPong(wsScratch, flen);
                continue;
            }
            if (opcode == 0x0A) { // Pong
                continue;
            }

            // Reassemble fragmented binary frames if needed
            byte[] full = wsScratch;
            int fullLen = flen;
            if (!fin) {
                java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream(flen * 2);
                bos.write(wsScratch, 0, flen);
                boolean done = false;
                while (!done) {
                    int fb0 = dis.readUnsignedByte();
                    int fb1 = dis.readUnsignedByte();
                    done = (fb0 & 0x80) != 0;
                    boolean fmask = (fb1 & 0x80) != 0;
                    long clen = fb1 & 0x7F;
                    if (clen == 126) clen = dis.readUnsignedShort();
                    else if (clen == 127) clen = dis.readLong();
                    if (clen < 0 || clen > 4 * 1024 * 1024) throw new IOException("WS frag too large");
                    int iclen = (int) clen;
                    byte[] fm = null;
                    if (fmask) { fm = new byte[4]; dis.readFully(fm); }
                    if (wsScratch.length < iclen) wsScratch = new byte[iclen + 16384];
                    if (iclen > 0) {
                        dis.readFully(wsScratch, 0, iclen);
                        if (fmask) for (int i = 0; i < iclen; i++) wsScratch[i] ^= fm[i & 3];
                        bos.write(wsScratch, 0, iclen);
                    }
                }
                full = bos.toByteArray();
                fullLen = full.length;
            }

            if (fullLen < 14) continue;
            int magic = ((full[0] & 0xFF) << 24) | ((full[1] & 0xFF) << 16)
                    | ((full[2] & 0xFF) << 8) | (full[3] & 0xFF);
            if (magic != MAGIC_LMV1) continue;
            out.type = full[4] & 0xFF;
            out.flags = full[5] & 0xFF;
            out.w = ((full[6] & 0xFF) << 8) | (full[7] & 0xFF);
            out.h = ((full[8] & 0xFF) << 8) | (full[9] & 0xFF);
            out.len = ((full[10] & 0xFF) << 24) | ((full[11] & 0xFF) << 16)
                    | ((full[12] & 0xFF) << 8) | (full[13] & 0xFF);
            if (out.len < 0 || 14 + out.len > fullLen) continue;
            if (out.len > 0) {
                out.ensureCapacity(out.len);
                System.arraycopy(full, 14, out.data, 0, out.len);
            }
            return true;
        }
    }

    public boolean isClosed() {
        return closed || socket.isClosed();
    }

    @Override
    public void close() {
        if (closed) return;
        closed = true;
        closeQuietly(socket);
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c == null) return;
        try { c.close(); } catch (Throwable ignored) {}
    }
}
