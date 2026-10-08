// Public Cloudflare Turnstile site key (the secret belongs only in .env).
window.FAUCET_CONFIG = {
    // Local Python server has no API. Hosted deployments should proxy /transfer
    // to the backend, or set this to their public backend URL.
    apiUrl: ['localhost', '127.0.0.1', '[::1]'].includes(window.location.hostname)
        ? `${window.location.protocol}//${window.location.hostname}:5000/transfer`
        : '/transfer',
    captchaSiteKey: '0x4AAAAAAFRMORebBqhtG85U'
};
