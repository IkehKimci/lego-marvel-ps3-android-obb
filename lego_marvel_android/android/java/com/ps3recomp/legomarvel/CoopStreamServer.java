package com.ps3recomp.legomarvel;

import android.content.Context;
import android.graphics.Bitmap;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Dual-Screen 2-Phone Co-op Host Server (runs on Phone 1).
 *
 * Supports TWO transport modes:
 *   1. ONLINE RELAY ("relay"): Connects outbound via CoopRelayLink (TLS WebSocket or Multi-Port TCP)
 *      to a free cloud/VPS relay server using (Room Port, SHA-256 Password). Streams H.264 video +
 *      24 kHz Stereo IMA-ADPCM audio OUT and receives Player 2 controller packets (PKT_INPUT) IN
 *      over ONE single connection — 100% firewall/CGNAT friendly!
 *   2. LAN DIRECT ("lan"): Direct TCP :31161 + UDP :31160 discovery on local Wi-Fi / Hotspot.
 */
public class CoopStreamServer {
    private static final String TAG = "CoopStreamServer";
    public static final int DISCOVERY_UDP_PORT = 31160;
    public static final int VIDEO_TCP_PORT     = 31161;
    public static final int INPUT_UDP_PORT     = 31162;

    public static final int HELLO_PRBE = 0x50524245; // "PRBE"
    public static final int HELLO_PLAY = 0x504C4159; // "PLAY"

    private static final int STREAM_W = 640;
    private static final int STREAM_H = 360;

    public static final int MAGIC_LMV1 = CoopRelayLink.MAGIC_LMV1;
    public static final int PKT_CSD    = CoopRelayLink.PKT_CSD;
    public static final int PKT_H264   = CoopRelayLink.PKT_H264;
    public static final int PKT_JPEG   = CoopRelayLink.PKT_JPEG;
    public static final int PKT_AUDIO  = CoopRelayLink.PKT_AUDIO;
    public static final int PKT_PING   = CoopRelayLink.PKT_PING;
    public static final int PKT_INPUT  = CoopRelayLink.PKT_INPUT;
    public static final int PKT_STATUS = CoopRelayLink.PKT_STATUS;

    public static class LanEndpoint {
        public final String ifaceName;
        public final String ip;
        public final String broadcast;
        public final int priority;

        LanEndpoint(String ifaceName, String ip, String broadcast, int priority) {
            this.ifaceName = ifaceName;
            this.ip = ip;
            this.broadcast = broadcast;
            this.priority = priority;
        }
    }

    private interface PacketSink {
        void send(int type, int flags, int w, int h, byte[] data, int off, int len) throws IOException;
        boolean isAlive();
    }

    private final Context appCtx;
    private final boolean useRelay;
    private final String relayServer;
    private final String relayPort;
    private final String relayPass;
    private final int streamBitrate;
    private final int streamFps;
    private final int jpegQuality;
    private final boolean streamAudio;

    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile boolean serverListening = false;
    private volatile String serverBindError = "";
    private volatile String relayHudState = "";
    private volatile boolean relayPeerOnline = false;

    // LAN mode handles
    private ServerSocket serverSocket;
    private DatagramSocket discoveryRxSocket;
    private volatile Socket activeClient;
    private WifiManager.MulticastLock multicastLock;

    // Relay mode handle
    private volatile CoopRelayLink activeRelayLink;

    public CoopStreamServer(Context ctx) {
        this(ctx, "lan", "", "31161", "", "wan", true);
    }

    public CoopStreamServer(Context ctx, String netMode, String relayServer, String relayPort,
                            String relayPass, String quality, boolean streamAudio) {
        this.appCtx = (ctx != null) ? ctx.getApplicationContext() : null;
        this.useRelay = "relay".equalsIgnoreCase(netMode);
        this.relayServer = (relayServer != null) ? relayServer.trim() : "";
        this.relayPort = (relayPort != null && !relayPort.trim().isEmpty()) ? relayPort.trim() : "31161";
        this.relayPass = (relayPass != null) ? relayPass : "";
        this.streamAudio = streamAudio;

        if ("low".equalsIgnoreCase(quality)) {
            this.streamBitrate = 800_000;
            this.streamFps = 24;
            this.jpegQuality = 42;
        } else if ("high".equalsIgnoreCase(quality) || "lan".equalsIgnoreCase(quality)) {
            this.streamBitrate = 3_500_000;
            this.streamFps = 30;
            this.jpegQuality = 70;
        } else { // "wan" default (Online Stabil)
            this.streamBitrate = 1_500_000;
            this.streamFps = 30;
            this.jpegQuality = 54;
        }
    }

    public boolean isRelayMode() {
        return useRelay;
    }

    public String getRelayPortLabel() {
        return relayPort;
    }

    public boolean isServerListening() {
        return serverListening;
    }

    public String getServerBindError() {
        return serverBindError;
    }

    public String getRelayHudState() {
        return relayHudState;
    }

    public boolean isRelayPeerOnline() {
        return relayPeerOnline;
    }

    // ---- Android-safe LAN / Wi-Fi / Hotspot network helpers ----

    private static boolean isIgnoredInterface(String name) {
        if (name == null) return false;
        String n = name.toLowerCase(Locale.US);
        return n.startsWith("lo")
                || n.startsWith("rmnet")
                || n.startsWith("r_rmnet")
                || n.startsWith("v4-rmnet")
                || n.startsWith("ccmni")
                || n.startsWith("pdp")
                || n.startsWith("seth_lte")
                || n.startsWith("wwan")
                || n.startsWith("clat")
                || n.startsWith("dummy")
                || n.startsWith("tun")
                || n.startsWith("tap")
                || n.startsWith("sit")
                || n.startsWith("ip6tnl")
                || n.startsWith("epdg")
                || n.startsWith("ims");
    }

    public static boolean isPrivateLanIpv4(String ip) {
        if (ip == null) return false;
        if (ip.startsWith("192.168.") || ip.startsWith("10.")) return true;
        if (ip.startsWith("172.")) {
            String[] p = ip.split("\\.");
            if (p.length == 4) {
                try {
                    int b = Integer.parseInt(p[1]);
                    return b >= 16 && b <= 31;
                } catch (NumberFormatException ignored) {
                }
            }
        }
        return false;
    }

    private static int ifacePriority(String name, String ip) {
        String n = (name != null) ? name.toLowerCase(Locale.US) : "";
        if (n.equals("wlan0")) return 10;
        if (n.startsWith("ap") || n.startsWith("swlan") || n.startsWith("softap") || n.startsWith("wlan")) return 15;
        if (n.startsWith("eth") || n.startsWith("rndis") || n.startsWith("usb")) return 25;
        if (isPrivateLanIpv4(ip)) return 40;
        return 80;
    }

    private static String fallbackBroadcast(String ip) {
        if (ip == null) return "255.255.255.255";
        int dot = ip.lastIndexOf('.');
        if (dot > 0) return ip.substring(0, dot + 1) + "255";
        return "255.255.255.255";
    }

    public static Network findWifiNetwork(Context ctx) {
        if (ctx == null) return null;
        try {
            ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return null;
            Network[] nets = cm.getAllNetworks();
            if (nets == null) return null;
            for (Network net : nets) {
                NetworkCapabilities caps = cm.getNetworkCapabilities(net);
                if (caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    return net;
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    public static List<LanEndpoint> getLanEndpoints(Context ctx) {
        Map<String, LanEndpoint> byIp = new LinkedHashMap<>();
        if (ctx != null) {
            try {
                ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
                if (cm != null) {
                    Network[] nets = cm.getAllNetworks();
                    if (nets != null) {
                        for (Network net : nets) {
                            NetworkCapabilities caps = cm.getNetworkCapabilities(net);
                            if (caps == null) continue;
                            boolean isWifi = caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI);
                            boolean isEth  = caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET);
                            if (!isWifi && !isEth) continue;
                            LinkProperties lp = cm.getLinkProperties(net);
                            if (lp == null) continue;
                            String ifName = lp.getInterfaceName();
                            if (ifName == null || ifName.isEmpty()) ifName = isWifi ? "wlan0" : "eth0";
                            for (LinkAddress la : lp.getLinkAddresses()) {
                                InetAddress a = la.getAddress();
                                if (a instanceof Inet4Address && !a.isLoopbackAddress() && !a.isLinkLocalAddress()) {
                                    String ip = a.getHostAddress();
                                    if (ip != null && !byIp.containsKey(ip)) {
                                        byIp.put(ip, new LanEndpoint(ifName, ip, fallbackBroadcast(ip), isWifi ? 5 : 20));
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (Throwable ignored) {
            }
        }

        try {
            Enumeration<NetworkInterface> nics = NetworkInterface.getNetworkInterfaces();
            while (nics != null && nics.hasMoreElements()) {
                NetworkInterface nic = nics.nextElement();
                if (!nic.isUp() || nic.isLoopback()) continue;
                String name = nic.getName();
                if (isIgnoredInterface(name)) continue;

                List<InterfaceAddress> iaddrs = nic.getInterfaceAddresses();
                if (iaddrs != null) {
                    for (InterfaceAddress ia : iaddrs) {
                        InetAddress addr = ia.getAddress();
                        if (addr instanceof Inet4Address && !addr.isLoopbackAddress() && !addr.isLinkLocalAddress()) {
                            String ip = addr.getHostAddress();
                            if (ip == null) continue;
                            InetAddress bcastAddr = ia.getBroadcast();
                            String bcast = (bcastAddr instanceof Inet4Address)
                                    ? bcastAddr.getHostAddress() : fallbackBroadcast(ip);
                            int prio = ifacePriority(name, ip);
                            LanEndpoint prev = byIp.get(ip);
                            if (prev == null || prio < prev.priority) {
                                byIp.put(ip, new LanEndpoint(name, ip, bcast, prio));
                            }
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        }

        List<LanEndpoint> out = new ArrayList<>(byIp.values());
        out.sort((a, b) -> Integer.compare(a.priority, b.priority));
        return out;
    }

    public static String getLocalIpv4(Context ctx) {
        List<LanEndpoint> eps = getLanEndpoints(ctx);
        if (!eps.isEmpty()) return eps.get(0).ip;
        return "";
    }

    public static String getLocalIpv4() {
        return getLocalIpv4(null);
    }

    public static String getLocalIpv4Summary(Context ctx) {
        List<LanEndpoint> eps = getLanEndpoints(ctx);
        if (eps.isEmpty()) return "Belum terhubung Wi-Fi / Hotspot";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < Math.min(eps.size(), 2); i++) {
            if (i > 0) sb.append("  |  ");
            sb.append(eps.get(i).ip).append(" (").append(eps.get(i).ifaceName).append(")");
        }
        return sb.toString();
    }

    private static String sameSubnetWifiPrefix(Context ctx, Network wifi) {
        if (ctx == null || wifi == null) return null;
        try {
            ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
            LinkProperties lp = (cm != null) ? cm.getLinkProperties(wifi) : null;
            if (lp != null) {
                for (LinkAddress la : lp.getLinkAddresses()) {
                    InetAddress a = la.getAddress();
                    if (a instanceof Inet4Address && !a.isLoopbackAddress() && !a.isLinkLocalAddress()) {
                        String ip = a.getHostAddress();
                        int dot = (ip != null) ? ip.lastIndexOf('.') : -1;
                        if (dot > 0) return ip.substring(0, dot + 1);
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    public static Socket connectLanTcp(Context ctx, String hostIp, int port, int timeoutMs) throws IOException {
        IOException wifiErr = null;
        if (isPrivateLanIpv4(hostIp)) {
            Network wifi = findWifiNetwork(ctx);
            if (wifi != null) {
                String wifiPref = sameSubnetWifiPrefix(ctx, wifi);
                boolean sameSubnet = (wifiPref != null && hostIp.startsWith(wifiPref));
                Socket ws = null;
                try {
                    ws = wifi.getSocketFactory().createSocket();
                    ws.connect(new InetSocketAddress(hostIp, port), timeoutMs);
                    return ws;
                } catch (IOException e) {
                    wifiErr = e;
                    closeQuietly(ws);
                    if (sameSubnet) {
                        throw new IOException("[WiFi] " + e.getMessage(), e);
                    }
                } catch (Throwable t) {
                    closeQuietly(ws);
                    if (sameSubnet) {
                        throw new IOException("[WiFi] " + t.getMessage(), t);
                    }
                }
            }
        }
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(hostIp, port), timeoutMs);
            return s;
        } catch (IOException e) {
            closeQuietly(s);
            if (wifiErr != null) {
                throw new IOException("[WiFi] " + wifiErr.getMessage() + " / [Def] " + e.getMessage(), wifiErr);
            }
            throw e;
        }
    }

    public static DatagramSocket createLanUdpSocket(Context ctx, String targetIp) throws IOException {
        DatagramSocket ds = new DatagramSocket();
        if (targetIp != null && isPrivateLanIpv4(targetIp)) {
            Network wifi = findWifiNetwork(ctx);
            if (wifi != null) {
                try { wifi.bindSocket(ds); } catch (Throwable ignored) {}
            }
        }
        return ds;
    }

    public static WifiManager.MulticastLock acquireMulticastLock(Context ctx, String tag) {
        if (ctx == null) return null;
        try {
            WifiManager wm = (WifiManager) ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                WifiManager.MulticastLock lock = wm.createMulticastLock(tag);
                lock.setReferenceCounted(false);
                lock.acquire();
                return lock;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    public static void releaseMulticastLock(WifiManager.MulticastLock lock) {
        if (lock == null) return;
        try {
            if (lock.isHeld()) lock.release();
        } catch (Throwable ignored) {
        }
    }

    private static synchronized void coopLog(String msg) {
        Log.i(TAG, msg);
        try {
            java.io.File dir = new java.io.File("/storage/emulated/0/PS3/BLUS31161/logs");
            if (!dir.exists()) dir.mkdirs();
            java.io.File f = new java.io.File(dir, "coop_host.log");
            String ts = new java.text.SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new java.util.Date());
            try (java.io.FileWriter fw = new java.io.FileWriter(f, true)) {
                fw.write("[" + ts + "] " + msg + "\n");
            }
        } catch (Throwable ignored) {
        }
    }

    // ---- Server Lifecycle ----

    public void start() {
        if (!running.compareAndSet(false, true)) return;
        serverListening = false;
        serverBindError = "";
        relayPeerOnline = false;
        try {
            java.io.File f = new java.io.File("/storage/emulated/0/PS3/BLUS31161/logs/coop_host.log");
            if (f.exists() && f.length() > 128 * 1024) f.delete();
        } catch (Throwable ignored) {
        }

        if (useRelay) {
            coopLog("=== ONLINE RELAY HOST start: server=" + relayServer + " port=" + relayPort
                    + " bitrate=" + (streamBitrate / 1000) + "kbps fps=" + streamFps + " audio=" + streamAudio + " ===");
            relayHudState = "RELAY MENYAMBUNG...";
            Thread t = new Thread(this::runOnlineRelayHostLoop, "coop-relay-host");
            t.setDaemon(true);
            t.start();
        } else {
            coopLog("=== LAN HOST start: interfaces=" + getLocalIpv4Summary(appCtx)
                    + " bitrate=" + (streamBitrate / 1000) + "kbps audio=" + streamAudio + " ===");
            multicastLock = acquireMulticastLock(appCtx, "LegoMarvelCoopHost");

            Thread btx = new Thread(this::runDiscoveryBeaconTx, "coop-beacon-tx");
            btx.setDaemon(true);
            btx.start();

            Thread brx = new Thread(this::runDiscoveryProbeResponder, "coop-beacon-rx");
            brx.setDaemon(true);
            brx.start();

            Thread acc = new Thread(this::runLanAcceptLoop, "coop-video-srv");
            acc.setDaemon(true);
            acc.start();
        }
    }

    public void stop() {
        if (!running.compareAndSet(true, false)) return;
        serverListening = false;
        relayPeerOnline = false;
        coopLog("CoopStreamServer.stop()");
        setNativeStreamingActive(false);
        CoopRelayLink rl = activeRelayLink;
        activeRelayLink = null;
        closeQuietly(rl);
        closeQuietly(activeClient);
        activeClient = null;
        closeQuietly(serverSocket);
        serverSocket = null;
        if (discoveryRxSocket != null) {
            try { discoveryRxSocket.close(); } catch (Throwable ignored) {}
            discoveryRxSocket = null;
        }
        releaseMulticastLock(multicastLock);
        multicastLock = null;
    }

    private void setNativeStreamingActive(boolean active) {
        try {
            GameActivity.nativeCoopSetClientConnected(active ? 1 : 0);
        } catch (Throwable ignored) {
        }
        try {
            GameActivity.nativeCoopSetAudioEnabled((active && streamAudio) ? 1 : 0);
        } catch (Throwable ignored) {
        }
        if (!active) {
            try {
                GameActivity.nativeCoopPushP2Input(-1, 128, 128, 128, 128, 0, 0);
            } catch (Throwable ignored) {
            }
        }
    }

    // ---- ONLINE RELAY MODE (Outbound Connection — Zero Firewall / NAT / Hotspot Issues!) ----

    private void runOnlineRelayHostLoop() {
        while (running.get()) {
            if (relayServer.isEmpty()) {
                relayHudState = "RELAY: ISI ALAMAT SERVER";
                try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
                continue;
            }
            CoopRelayLink link = null;
            Thread streamThread = null;
            final AtomicBoolean sessionAlive = new AtomicBoolean(true);
            try {
                relayHudState = "RELAY MENYAMBUNG...";
                link = CoopRelayLink.connect(relayServer, relayPort, relayPass, true, 10_000, 45_000);
                activeRelayLink = link;
                serverListening = true;
                serverBindError = "";
                relayPeerOnline = false;
                relayHudState = "ONLINE ROOM " + relayPort + " (TUNGGU P2)";
                coopLog("Online Relay HOST connected! Room/Port=" + relayPort);

                final CoopRelayLink fLink = link;
                PacketSink sink = new PacketSink() {
                    @Override
                    public void send(int type, int flags, int w, int h, byte[] data, int off, int len) throws IOException {
                        fLink.sendPacket(type, flags, w, h, data, off, len);
                    }
                    @Override
                    public boolean isAlive() {
                        return running.get() && sessionAlive.get() && !fLink.isClosed();
                    }
                };

                // Streamer thread waits until relayPeerOnline == true, then streams H.264 + Audio!
                streamThread = new Thread(() -> runRelayMediaLoop(sink, sessionAlive), "coop-relay-tx");
                streamThread.setDaemon(true);
                streamThread.start();

                // RX loop: receives PKT_STATUS (P2 join/leave) and PKT_INPUT (Player 2 controller!)
                CoopRelayLink.Packet pkt = new CoopRelayLink.Packet();
                while (running.get() && !link.isClosed()) {
                    link.readPacket(pkt);
                    if (pkt.type == PKT_STATUS) {
                        boolean peerOn = (pkt.flags & 4) != 0;
                        String info = (pkt.len > 0) ? new String(pkt.data, 0, pkt.len, StandardCharsets.UTF_8) : "";
                        coopLog("Relay STATUS: peerOnline=" + peerOn + " msg=" + info);
                        if (peerOn && !relayPeerOnline) {
                            relayPeerOnline = true;
                            setNativeStreamingActive(true);
                            relayHudState = "ONLINE P2 • ROOM " + relayPort;
                        } else if (!peerOn && relayPeerOnline) {
                            relayPeerOnline = false;
                            setNativeStreamingActive(false);
                            relayHudState = "ONLINE ROOM " + relayPort + " (TUNGGU P2)";
                        }
                    } else if (pkt.type == PKT_INPUT && pkt.len >= 16) {
                        if (pkt.data[0] == 'L' && pkt.data[1] == 'M' && pkt.data[2] == 'P' && pkt.data[3] == '2') {
                            int btns = (pkt.data[8] & 0xFF) | ((pkt.data[9] & 0xFF) << 8);
                            int lx = pkt.data[10] & 0xFF;
                            int ly = pkt.data[11] & 0xFF;
                            int rx = pkt.data[12] & 0xFF;
                            int ry = pkt.data[13] & 0xFF;
                            int l2 = pkt.data[14] & 0xFF;
                            int r2 = pkt.data[15] & 0xFF;
                            try {
                                GameActivity.nativeCoopPushP2Input(btns, lx, ly, rx, ry, l2, r2);
                            } catch (Throwable ignored) {
                            }
                            if (!relayPeerOnline) {
                                relayPeerOnline = true;
                                setNativeStreamingActive(true);
                                relayHudState = "ONLINE P2 • ROOM " + relayPort;
                            }
                        }
                    }
                }
            } catch (Throwable t) {
                String msg = (t.getMessage() != null && !t.getMessage().isEmpty())
                        ? t.getMessage() : t.getClass().getSimpleName();
                coopLog("Online Relay HOST disconnected/error: " + msg);
                serverBindError = msg;
                relayHudState = "RELAY: " + (msg.length() > 36 ? msg.substring(0, 36) : msg);
            } finally {
                sessionAlive.set(false);
                serverListening = false;
                relayPeerOnline = false;
                setNativeStreamingActive(false);
                if (activeRelayLink == link) activeRelayLink = null;
                closeQuietly(link);
                if (streamThread != null) {
                    try { streamThread.join(1000); } catch (InterruptedException ignored) {}
                }
            }
            if (running.get()) {
                try { Thread.sleep(2500); } catch (InterruptedException ignored) {}
            }
        }
    }

    private void runRelayMediaLoop(PacketSink sink, AtomicBoolean sessionAlive) {
        long lastIdlePing = System.currentTimeMillis();
        while (running.get() && sessionAlive.get() && sink.isAlive()) {
            if (!relayPeerOnline) {
                long now = System.currentTimeMillis();
                if (now - lastIdlePing >= 12_000L) {
                    try {
                        sink.send(PKT_PING, 0, STREAM_W, STREAM_H, null, 0, 0);
                        lastIdlePing = now;
                    } catch (IOException e) {
                        break;
                    }
                }
                try { Thread.sleep(80); } catch (InterruptedException ignored) {}
                continue;
            }

            // Peer is online! Start Audio thread + H.264/JPEG Video encoder loop until peer leaves or link dies
            final AtomicBoolean peerStreaming = new AtomicBoolean(true);
            Thread audioThread = null;
            if (streamAudio) {
                audioThread = new Thread(() -> runAudioStreamLoop(sink, peerStreaming), "coop-audio-tx");
                audioThread.setDaemon(true);
                audioThread.start();
            }
            try {
                sink.send(PKT_PING, 4, STREAM_W, STREAM_H, null, 0, 0);
                PacketSink peerSink = new PacketSink() {
                    @Override
                    public void send(int type, int flags, int w, int h, byte[] data, int off, int len) throws IOException {
                        sink.send(type, flags, w, h, data, off, len);
                    }
                    @Override
                    public boolean isAlive() {
                        return running.get() && sessionAlive.get() && relayPeerOnline && sink.isAlive();
                    }
                };
                if (!streamH264(peerSink)) {
                    if (peerSink.isAlive()) {
                        coopLog("Hardware H.264 unavailable; falling back to JPEG stream");
                        streamJpegFallback(peerSink);
                    }
                }
            } catch (Throwable t) {
                coopLog("Relay media stream stopped: " + t.getMessage());
            } finally {
                peerStreaming.set(false);
                if (audioThread != null) {
                    try { audioThread.join(500); } catch (InterruptedException ignored) {}
                }
            }
        }
    }

    private void runAudioStreamLoop(PacketSink sink, AtomicBoolean activeFlag) {
        byte[] adpcmBuf = new byte[544];
        while (running.get() && activeFlag.get() && sink.isAlive()) {
            try {
                int n = GameActivity.nativeCoopReadAudioAdpcm(adpcmBuf);
                if (n > 0) {
                    sink.send(PKT_AUDIO, 2, 24000, 512, adpcmBuf, 0, n);
                } else {
                    Thread.sleep(10);
                }
            } catch (IOException e) {
                break;
            } catch (Throwable ignored) {
                try { Thread.sleep(25); } catch (InterruptedException ie) { break; }
            }
        }
    }

    // ---- LAN DIRECT MODE (Local Wi-Fi / Hotspot) ----

    private void runDiscoveryBeaconTx() {
        InetAddress globalBcast;
        try {
            globalBcast = InetAddress.getByName("255.255.255.255");
        } catch (Throwable t) {
            return;
        }
        while (running.get()) {
            List<LanEndpoint> eps = getLanEndpoints(appCtx);
            for (LanEndpoint ep : eps) {
                DatagramSocket s = null;
                try {
                    s = new DatagramSocket(new InetSocketAddress(InetAddress.getByName(ep.ip), 0));
                    s.setBroadcast(true);
                    String msg = "LMCOOP1|" + ep.ip + "|" + VIDEO_TCP_PORT + "|" + INPUT_UDP_PORT;
                    byte[] data = msg.getBytes(StandardCharsets.UTF_8);
                    try {
                        s.send(new DatagramPacket(data, data.length, InetAddress.getByName(ep.broadcast), DISCOVERY_UDP_PORT));
                    } catch (Throwable ignored) {}
                    try {
                        s.send(new DatagramPacket(data, data.length, globalBcast, DISCOVERY_UDP_PORT));
                    } catch (Throwable ignored) {}
                } catch (Throwable ignored) {
                } finally {
                    if (s != null) try { s.close(); } catch (Throwable ignored) {}
                }
            }
            try { Thread.sleep(800); } catch (InterruptedException ignored) {}
        }
    }

    private void runDiscoveryProbeResponder() {
        DatagramSocket sock = null;
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.setBroadcast(true);
            sock.bind(new InetSocketAddress(InetAddress.getByAddress(new byte[]{0, 0, 0, 0}), DISCOVERY_UDP_PORT));
            sock.setSoTimeout(1000);
            discoveryRxSocket = sock;
            coopLog("UDP discovery responder listening on 0.0.0.0:" + DISCOVERY_UDP_PORT);

            byte[] buf = new byte[256];
            DatagramPacket pkt = new DatagramPacket(buf, buf.length);
            while (running.get()) {
                try {
                    sock.receive(pkt);
                    String req = new String(pkt.getData(), 0, pkt.getLength(), StandardCharsets.UTF_8).trim();
                    if (req.startsWith("LMCOOP_PROBE")) {
                        String ip = getLocalIpv4(appCtx);
                        if (!ip.isEmpty()) {
                            String reply = "LMCOOP1|" + ip + "|" + VIDEO_TCP_PORT + "|" + INPUT_UDP_PORT;
                            byte[] rdata = reply.getBytes(StandardCharsets.UTF_8);
                            sock.send(new DatagramPacket(rdata, rdata.length, pkt.getSocketAddress()));
                        }
                    }
                } catch (java.net.SocketTimeoutException ignored) {
                } catch (Throwable ignored) {
                }
            }
        } catch (Throwable t) {
            coopLog("UDP discovery responder failed: " + t.getMessage());
        } finally {
            if (sock != null) try { sock.close(); } catch (Throwable ignored) {}
        }
    }

    private void runLanAcceptLoop() {
        try {
            serverSocket = new ServerSocket();
            serverSocket.setReuseAddress(true);
            serverSocket.bind(new InetSocketAddress(InetAddress.getByAddress(new byte[]{0, 0, 0, 0}), VIDEO_TCP_PORT), 16);
            serverListening = true;
            serverBindError = "";
            coopLog("LAN TCP server LISTENING on 0.0.0.0:" + VIDEO_TCP_PORT + " (" + getLocalIpv4Summary(appCtx) + ")");

            while (running.get()) {
                final Socket client = serverSocket.accept();
                if (!running.get()) {
                    closeQuietly(client);
                    break;
                }
                Thread t = new Thread(() -> handleLanAcceptedSocket(client), "coop-lan-conn");
                t.setDaemon(true);
                t.start();
            }
        } catch (Throwable t) {
            serverListening = false;
            serverBindError = (t.getMessage() != null) ? t.getMessage() : t.getClass().getSimpleName();
            if (running.get()) coopLog("LAN TCP server failed: " + serverBindError);
        } finally {
            serverListening = false;
            closeQuietly(serverSocket);
        }
    }

    private void handleLanAcceptedSocket(Socket client) {
        try {
            client.setTcpNoDelay(true);
            client.setSoTimeout(2500);
            DataInputStream dis = new DataInputStream(client.getInputStream());
            int hello = dis.readInt();
            if (hello == HELLO_PRBE) {
                String ip = getLocalIpv4(appCtx);
                String resp = "LMCOOP1|" + ip + "|" + VIDEO_TCP_PORT + "|" + INPUT_UDP_PORT + "\n";
                client.getOutputStream().write(resp.getBytes(StandardCharsets.UTF_8));
                client.getOutputStream().flush();
                closeQuietly(client);
                return;
            }
            if (hello != HELLO_PLAY) {
                closeQuietly(client);
                return;
            }
        } catch (Throwable t) {
            closeQuietly(client);
            return;
        }

        Socket prev = activeClient;
        if (prev != null && prev != client) closeQuietly(prev);
        activeClient = client;

        try {
            client.setTcpNoDelay(true);
            client.setSendBufferSize(256 * 1024);
            client.setSoTimeout(5000);
        } catch (Throwable ignored) {}

        coopLog("LAN Phone 2 PLAY connected from " + client.getRemoteSocketAddress());
        final Object txLock = new Object();
        final AtomicBoolean lanAlive = new AtomicBoolean(true);
        Thread audioThread = null;
        try {
            final DataOutputStream dos = new DataOutputStream(client.getOutputStream());
            PacketSink sink = new PacketSink() {
                @Override
                public void send(int type, int flags, int w, int h, byte[] data, int off, int len) throws IOException {
                    synchronized (txLock) {
                        dos.writeInt(MAGIC_LMV1);
                        dos.writeByte(type);
                        dos.writeByte(flags);
                        dos.writeShort(w);
                        dos.writeShort(h);
                        dos.writeInt(len);
                        if (len > 0 && data != null) dos.write(data, off, len);
                        dos.flush();
                    }
                }
                @Override
                public boolean isAlive() {
                    return running.get() && lanAlive.get() && activeClient == client && !client.isClosed();
                }
            };

            setNativeStreamingActive(true);
            sink.send(PKT_PING, 4, STREAM_W, STREAM_H, null, 0, 0);
            if (streamAudio) {
                audioThread = new Thread(() -> runAudioStreamLoop(sink, lanAlive), "coop-lan-audio");
                audioThread.setDaemon(true);
                audioThread.start();
            }
            if (!streamH264(sink)) {
                if (sink.isAlive()) {
                    coopLog("Hardware H.264 unavailable; falling back to JPEG");
                    streamJpegFallback(sink);
                }
            }
        } catch (Throwable t) {
            coopLog("LAN Phone 2 stream ended: " + t.getMessage());
        } finally {
            lanAlive.set(false);
            if (audioThread != null) {
                try { audioThread.join(400); } catch (InterruptedException ignored) {}
            }
            if (activeClient == client) {
                activeClient = null;
                setNativeStreamingActive(false);
            }
            closeQuietly(client);
        }
    }

    // ---- Shared Hardware H.264 Encoder + JPEG Fallback ----

    private static byte[] extractFormatCsd(MediaFormat fmt) {
        if (fmt == null) return null;
        ByteBuffer csd0 = fmt.getByteBuffer("csd-0");
        ByteBuffer csd1 = fmt.getByteBuffer("csd-1");
        if (csd0 == null && csd1 == null) return null;
        ByteArrayOutputStream bos = new ByteArrayOutputStream(128);
        if (csd0 != null) {
            ByteBuffer dup = csd0.duplicate();
            byte[] b = new byte[dup.remaining()];
            dup.get(b);
            bos.write(b, 0, b.length);
        }
        if (csd1 != null) {
            ByteBuffer dup = csd1.duplicate();
            byte[] b = new byte[dup.remaining()];
            dup.get(b);
            bos.write(b, 0, b.length);
        }
        return bos.size() > 0 ? bos.toByteArray() : null;
    }

    private boolean streamH264(PacketSink sink) {
        MediaCodec encoder = null;
        try {
            MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, STREAM_W, STREAM_H);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, streamBitrate);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, streamFps);
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, useRelay ? 2 : 1);
            try {
                fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            } catch (Throwable ignored) {}
            try {
                fmt.setInteger("low-latency", 1);
            } catch (Throwable ignored) {}

            encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encoder.start();
            coopLog("H.264 encoder started (" + STREAM_W + "x" + STREAM_H + " @ " + streamFps + "fps, "
                    + (streamBitrate / 1000) + " kbps)");
        } catch (Throwable t) {
            coopLog("H.264 encoder init failed: " + t.getMessage());
            if (encoder != null) try { encoder.release(); } catch (Throwable ignored) {}
            return false;
        }

        int[] meta = new int[3];
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] outScratch = new byte[256 * 1024];
        long frameIdx = 0L;
        int lastSplit = 0;
        boolean sawImage = false;
        boolean sentCsd = false;
        long lastSyncMs = System.currentTimeMillis();
        long lastSendMs = lastSyncMs;
        long lastCapMs = 0L;
        final long minFrameIntervalMs = (streamFps < 30) ? (1000L / streamFps) : 0L;
        int pendingInIdx = -1;
        int pendingRawCap = 0;
        Image pendingImg = null;

        try {
            while (sink.isAlive()) {
                if (pendingInIdx < 0) {
                    int inIdx = encoder.dequeueInputBuffer(15_000);
                    if (inIdx >= 0) {
                        int rawCap = 0;
                        try {
                            ByteBuffer raw = encoder.getInputBuffer(inIdx);
                            if (raw != null) rawCap = raw.capacity();
                        } catch (Throwable ignored) {}
                        Image img = null;
                        try {
                            img = encoder.getInputImage(inIdx);
                        } catch (Throwable ignored) {}
                        if (img == null) {
                            if (!sawImage) return false;
                        } else {
                            sawImage = true;
                            pendingInIdx = inIdx;
                            pendingRawCap = rawCap;
                            pendingImg = img;
                        }
                    }
                }
                if (pendingInIdx >= 0 && pendingImg != null) {
                    long nowCap = System.currentTimeMillis();
                    if (minFrameIntervalMs > 0 && (nowCap - lastCapMs) < minFrameIntervalMs) {
                        try { Thread.sleep(Math.min(12, minFrameIntervalMs - (nowCap - lastCapMs))); } catch (InterruptedException ignored) {}
                    } else {
                        Image.Plane[] planes = pendingImg.getPlanes();
                        if (planes == null || planes.length < 3) {
                            pendingImg.close();
                            pendingImg = null;
                            pendingInIdx = -1;
                            return false;
                        }
                        int yStride = planes[0].getRowStride();
                        int rc = GameActivity.nativeCoopFillYuvPlanes(
                                planes[0].getBuffer(), yStride,
                                planes[1].getBuffer(), planes[1].getRowStride(),
                                planes[2].getBuffer(), planes[2].getRowStride(),
                                planes[1].getPixelStride(),
                                meta, 60);
                        if (rc < 0) {
                            if (!sink.isAlive()) break;
                            try { Thread.sleep(80); } catch (InterruptedException ignored) {}
                        } else if (rc > 0) {
                            lastCapMs = System.currentTimeMillis();
                            pendingImg.close();
                            pendingImg = null;
                            lastSplit = meta[2];
                            long ptsUs = (frameIdx++) * (1_000_000L / streamFps);
                            int yuvSize = Math.max((STREAM_W * STREAM_H * 3) / 2, (yStride * STREAM_H * 3) / 2);
                            if (pendingRawCap > 0 && yuvSize > pendingRawCap) yuvSize = pendingRawCap;
                            encoder.queueInputBuffer(pendingInIdx, 0, yuvSize, ptsUs, 0);
                            pendingInIdx = -1;
                        }
                    }
                }

                long nowMs = System.currentTimeMillis();
                if (nowMs - lastSendMs >= 2000L) {
                    sink.send(PKT_PING, (lastSplit != 0 ? 2 : 0), STREAM_W, STREAM_H, null, 0, 0);
                    lastSendMs = nowMs;
                }
                if (nowMs - lastSyncMs >= (useRelay ? 2500L : 1500L)) {
                    lastSyncMs = nowMs;
                    try {
                        Bundle b = new Bundle();
                        b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                        encoder.setParameters(b);
                    } catch (Throwable ignored) {}
                }

                for (;;) {
                    int outIdx = encoder.dequeueOutputBuffer(info, 2_000);
                    if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) break;
                    if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        if (!sentCsd) {
                            byte[] csd = extractFormatCsd(encoder.getOutputFormat());
                            if (csd != null) {
                                sink.send(PKT_CSD, (lastSplit != 0 ? 2 : 0), STREAM_W, STREAM_H, csd, 0, csd.length);
                                sentCsd = true;
                                lastSendMs = System.currentTimeMillis();
                            }
                        }
                        continue;
                    }
                    if (outIdx < 0) break;

                    ByteBuffer outBuf = encoder.getOutputBuffer(outIdx);
                    if (outBuf != null && info.size > 0) {
                        if (info.size > outScratch.length) outScratch = new byte[info.size + 16384];
                        outBuf.position(info.offset);
                        outBuf.limit(info.offset + info.size);
                        outBuf.get(outScratch, 0, info.size);

                        boolean isCsd = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                        boolean isKey = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                        if (isCsd) sentCsd = true;
                        int flags = (isKey ? 1 : 0) | (lastSplit != 0 ? 2 : 0);
                        sink.send(isCsd ? PKT_CSD : PKT_H264, flags, STREAM_W, STREAM_H, outScratch, 0, info.size);
                        lastSendMs = System.currentTimeMillis();
                    }
                    encoder.releaseOutputBuffer(outIdx, false);
                }
            }
            return true;
        } catch (IOException ioe) {
            return true;
        } catch (Throwable t) {
            coopLog("H.264 stream error: " + t.getMessage());
            return sawImage;
        } finally {
            if (pendingImg != null) try { pendingImg.close(); } catch (Throwable ignored) {}
            try { encoder.stop(); } catch (Throwable ignored) {}
            try { encoder.release(); } catch (Throwable ignored) {}
        }
    }

    private void streamJpegFallback(PacketSink sink) throws IOException {
        ByteBuffer rgbaBuf = ByteBuffer.allocateDirect(STREAM_W * STREAM_H * 4);
        Bitmap bmp = Bitmap.createBitmap(STREAM_W, STREAM_H, Bitmap.Config.ARGB_8888);
        ByteArrayOutputStream jpegOut = new ByteArrayOutputStream(64 * 1024);
        int[] meta = new int[3];
        long lastSendMs = System.currentTimeMillis();

        while (sink.isAlive()) {
            int rc = GameActivity.nativeCoopFillRgba(rgbaBuf, meta, 150);
            if (rc <= 0) {
                if (rc < 0) {
                    if (!sink.isAlive()) break;
                    try { Thread.sleep(80); } catch (InterruptedException ignored) {}
                }
                long now = System.currentTimeMillis();
                if (now - lastSendMs >= 2000L) {
                    sink.send(PKT_PING, 0, STREAM_W, STREAM_H, null, 0, 0);
                    lastSendMs = now;
                }
                continue;
            }
            rgbaBuf.rewind();
            bmp.copyPixelsFromBuffer(rgbaBuf);
            jpegOut.reset();
            bmp.compress(Bitmap.CompressFormat.JPEG, jpegQuality, jpegOut);
            byte[] jpeg = jpegOut.toByteArray();
            int flags = (meta[2] != 0) ? 2 : 0;
            sink.send(PKT_JPEG, flags, STREAM_W, STREAM_H, jpeg, 0, jpeg.length);
            lastSendMs = System.currentTimeMillis();
        }
        bmp.recycle();
    }

    public static void closeQuietly(java.io.Closeable c) {
        if (c == null) return;
        try { c.close(); } catch (Throwable ignored) {}
    }
}
