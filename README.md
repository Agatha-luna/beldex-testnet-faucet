# Beldex Testnet Faucet

A testnet faucet backend and frontend for Beldex, built in C++ with Crow, CPR, SQLite, and other dependencies included as submodules.

## Requirements

Install the following system libraries before building:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libasio-dev libsqlite3-dev libcurl4-openssl-dev
```

| Library | Purpose |
| --- | --- |
| `build-essential` | C++ compiler and build tools |
| `cmake` (>= 3.14) | Build system |
| `libssl-dev` | OpenSSL, required by CPR (HTTP client) |
| `libasio-dev` | Standalone Asio, required by Crow |
| `libsqlite3-dev` | SQLite3, used for rate-limit tracking |
| `libcurl4-openssl-dev` | libcurl, required by CPR (system curl backend) |

## How to Clone the Repository

Clone the main repository and initialize submodules:

```bash
git clone --recurse-submodules https://github.com/MogamboPuri/beldex-testnet-faucet.git
cd beldex-testnet-faucet
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init --recursive
```

## Configuration

The backend reads its configuration from a `.env` file in the project root. Copy the example file and fill in real values:

```bash
cp .env.example .env
```

| Variable | Description |
| --- | --- |
| `WALLET_URL` | Beldex wallet RPC endpoint (`/json_rpc`) used to validate addresses, verify signatures and send faucet funds |
| `FAUCET_AMOUNT` | Amount sent per request, in atomic units (e.g. `150000000000` = 150 BDX) |
| `FAUCET_DATABASE` | SQLite database file path used for rate-limit tracking |
| `FAUCET_SIGN_SECRET` | Secret used to HMAC-sign wallet-ownership challenges (see below). Generate with `openssl rand -hex 32` |

All four are required — the server logs an error and refuses to process requests if any are missing.

## Proof of wallet ownership

Before paying out, the faucet requires proof that the requester actually controls the private key for the address they're claiming — not just a syntactically valid address. This closes the gap where rotating IPs and spraying addresses nobody owns (e.g. harvested from a block explorer) could otherwise bypass the per-address/per-IP rate limits:

1. `GET /challenge?address=<testnet address>` — mints a short-lived (5 minute), HMAC-signed challenge string bound to that address. Stateless: no database row, just an HMAC the server can re-check later.
2. The client signs that exact challenge string with the requesting wallet (e.g. the Beldex Wallet browser extension's `bdx_signMessage`, or `beldex-wallet-cli`'s `sign_value`).
3. `POST /transfer` now requires `{ "address", "challenge", "signature" }`. The server re-validates the challenge (shape, address binding, not expired), then asks the wallet RPC's native `verify` method whether `signature` really was produced by `address`'s spend key over `challenge` — no signature cryptography is implemented in this codebase itself.

This doesn't stop a determined attacker willing to script real wallet creation per request (generating a Beldex keypair is free), but it does stop the much cheaper attack of reusing addresses the requester never held keys for.

## Build Instructions

1. Create the build directory:

```bash
mkdir build
cd build
```

2. Run CMake and build:

```bash
cmake ..
make
```

## Run the Backend

The binary reads `.env` relative to its **current working directory**, so run it from the project root, not from `build/`:

```bash
cd beldex-testnet-faucet   # project root
./build/Beldex-faucet
```

The server starts on `http://localhost:5000`, exposing `POST /transfer`.

## Run the Frontend Locally

The frontend is static HTML/CSS/JS — no build step required. Serve it with any static file server:

```bash
cd frontend
python3 -m http.server 8000
```

Then open `http://localhost:8000` in your browser. Make sure `frontend/js/script.js` points at your backend URL (`http://localhost:5000/transfer` for a local backend).
