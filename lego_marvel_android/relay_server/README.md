# Server Relay Online Gratis — LEGO Marvel PS3 Co-op 2 HP

Server ini memungkinkan **2 HP bermain Co-op jarak jauh lewat internet** (beda Wi-Fi / 4G / 5G, tanpa Tailscale, tanpa port-forwarding, dan aman dipakai **banyak pasang pemain sekaligus** karena tiap pemain bisa memilih **Port / Nomor Room berbeda + Password sendiri**!).

---

## Opsi 1 (Paling Gampang & Unlimited WebSocket — 100% GRATIS Tanpa Kartu Kredit): **Render.com (Region Singapore)**

- **Gratis:** 750 jam/bulan + **100 GB bandwidth/bulan**, unlimited pesan WebSocket, server **Singapore (~15–30 ms dari Indonesia)**!
- **Cara Deploy (3 menit):**
  1. Hubungkan repository GitHub ini ke akun Render.com kamu (bisa pakai **New + → Blueprint** untuk deploy otomatis dari `render.yaml`, atau manual **New + → Web Service**).
  2. Buka [https://render.com](https://render.com) → login pakai GitHub.
  3. Klik **New +** → **Web Service** → pilih repo `lego-marvel-coop-relay` ini.
  4. Isi setting berikut:
     - **Root Directory:** *(kosongkan / default)*
     - **Runtime:** `Node`
     - **Region:** **`Singapore`**
     - **Build Command:** *(kosongkan)*
     - **Start Command:** `node server.js`
     - **Instance Type:** **`Free`**
  5. Klik **Create Web Service** → tunggu ~1 menit sampai muncul URL seperti:
     `https://lego-marvel-coop-relay.onrender.com`
  6. Di Launcher HP 1 & HP 2:
     - **Mode Koneksi:** `Server Online (Relay)`
     - **Alamat Server:** `lego-marvel-coop-relay.onrender.com`
     - **Port / Nomor Room:** bebas (misal kamu pakai `31161`, teman lain pakai `31162`, `31163`, `7777`, dst!)
     - **Password Room:** buat password bebas (HP 1 & HP 2 harus sama!).

---

## Opsi 2 (Ping Terendah Jakarta/Singapura `< 15 ms` — 100% GRATIS): **Cloudflare Workers + Durable Objects**

- **Gratis:** 100.000 request/hari (rasio WebSocket 20:1 ≈ **~7 jam main Co-op/hari gratis**), tanpa sleep/cold-start, langsung di server Edge Jakarta/Singapura!
- **Cara Deploy:**
  ```bash
  cd cloudflare
  npx wrangler login
  npx wrangler deploy
  ```
- Setelah muncul URL `https://lego-marvel-coop-relay.<akun>.workers.dev`, cukup ketik `lego-marvel-coop-relay.<akun>.workers.dev` di kolom **Alamat Server** Launcher HP 1 & HP 2!

---

## Opsi 3 (Multi-Port TCP Asli `:31161`..`:31170` + WebSocket di VPS / PC / Termux)

Jika kamu punya VPS murah, PC rumah, atau ingin menjalankan server multi-port TCP asli:
```bash
cd node
PORT=8080 TCP_PORTS=31161-31170 node server.js
```
- Port WebSocket (`8080`) + 10 Port Raw TCP asli (`31161` sampai `31170`) langsung aktif bersamaan!
- Pemain bisa menyambung via `tcp://<IP-VPS>:31161`, `tcp://<IP-VPS>:31162`, dst dengan password masing-masing!

---

## Keamanan Multi-User
- Password **tidak pernah dikirim plaintext** — langsung di-hash `SHA-256` di HP sebelum dikirim ke server.
- Jika Port/Room `31161` sedang dipakai pemain lain dengan password berbeda, server menolak pembajakan room (`409: Port/Room sedang dipakai — pilih nomor Port lain`).
- Jika HP 2 memasukkan password salah, server langsung menolak (`403: Password salah!`).
- Halaman status web (`https://<alamat-server>/`) hanya menampilkan daftar nomor Port/Room aktif dan **tidak pernah menampilkan password**.
