# Beldex Testnet Faucet

A testnet faucet backend and frontend for Beldex, built in C++ with Crow, CPR, SQLite, and other dependencies included as submodules.

## Requirements

Install the following system libraries before building:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libboost-dev libsqlite3-dev libcurl4-openssl-dev
```

| Library | Purpose |
| --- | --- |
| `build-essential` | C++ compiler and build tools |
| `cmake` (>= 3.15) | Build system |
| `libssl-dev` | OpenSSL, required by CPR (HTTP client) |
| `libboost-dev` | Header-only Boost.Asio, required by Crow |
| `libsqlite3-dev` | SQLite3, used for rate-limit tracking |
| `libcurl4-openssl-dev` | libcurl, required by CPR (system curl backend) |

On macOS with Homebrew:

```bash
brew install cmake openssl boost sqlite curl
```

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
| `CAPTCHA_SECRET_KEY` | Cloudflare Turnstile secret key used to verify every faucet request |
| `FAUCET_SIGN_SECRET` | Secret used to HMAC-sign wallet-ownership challenges. Generate with `openssl rand -hex 32` |

All five are required to process faucet requests.

## CAPTCHA setup

Create a Cloudflare Turnstile widget and allow your frontend hostname (including
`localhost` for local development). Put its public site key in
`frontend/js/config.js` as `captchaSiteKey`, and its secret key in the root `.env`
as `CAPTCHA_SECRET_KEY`. Restart the backend after changing `.env`.

The frontend submits `captcha_token` with `address`. The backend verifies it with
Cloudflare before any wallet RPC or database reservation and requires the `faucet`
action. The backend requires Cloudflare's verified `challenge_ts` to be less than
60 seconds old, enforcing a 1-minute limit. Missing, invalid or future timestamps
are rejected. Keep the backend server clock synchronized.
Missing, rejected, expired or reused tokens cannot request funds; verification
outages also block transfers. The widget resets after each request.

See https://developers.cloudflare.com/turnstile/get-started/ for key setup.

## Proof of wallet ownership

Before paying out, the faucet also requires proof that the requester controls the
private key for the requested address:

1. `GET /challenge?address=<testnet address>` returns a short-lived (5 minute), HMAC-signed challenge bound to that address.
2. The client signs that exact challenge with the connected Beldex Wallet.
3. `POST /transfer` requires `{ "address", "captcha_token", "challenge", "signature" }`. The backend validates the CAPTCHA and challenge, then uses the wallet RPC `verify` method to verify the signature.

This prevents requests using harvested addresses whose private keys the requester
does not control. It does not prevent someone from creating and controlling many
new wallets.

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

Then open `http://localhost:8000` in your browser. `frontend/js/config.js` routes
localhost/127.0.0.1/[::1] on any local frontend port to the backend on port 5000. For hosted deployments,
it defaults to `/transfer`; configure your reverse proxy to forward that route to
the backend, or set `apiUrl` to your public backend endpoint. HTTPS pages require
an HTTPS backend endpoint.

If you receive a non-JSON HTTP 501 response, refresh the page to load the latest
frontend configuration and check that the request goes to port 5000, rather than
the static server on port 8000. Start the backend from the project root as shown
above. Complete CAPTCHA and click **Get Faucet** to submit; completing CAPTCHA
alone does not send a request. A failed request remains visible while you verify
again. The backend also requires `CAPTCHA_SECRET_KEY` in `.env`.
