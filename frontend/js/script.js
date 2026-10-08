const statusWrap = () => document.getElementById('tx-status');
const statusRow = () => document.getElementById('tx-status-row');
const submitBtn = () => document.getElementById('submit-btn');

function escapeHtml(value) {
    return String(value).replace(/[&<>'"]/g, character => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        "'": '&#39;',
        '"': '&quot;'
    })[character]);
}

function setStatus(kind, html) {
    const wrap = statusWrap();
    const row = statusRow();
    wrap.hidden = false;
    row.className = 'tx-status-row' + (kind ? ' ' + kind : '');
    row.innerHTML = html;
}

function clearStatus() {
    statusWrap().hidden = true;
    statusRow().innerHTML = '';
}

// ---------- Beldex Wallet extension ----------

let bdxWallet = null;
let connectedAddress = '';

function setWalletConnectedUI(address) {
    document.getElementById('wallet-btn-label').textContent = 'Connected';
    document.getElementById('wallet-btn').title = 'Connected: ' + address + ' (click to disconnect)';

    connectedAddress = address;

    const chip = document.getElementById('address-chip');
    chip.classList.add('filled');
    chip.title = address;
    document.getElementById('address-text').textContent = address;

    // A successful connect makes any earlier connect error stale.
    clearStatus();
}

function setWalletDisconnectedUI() {
    document.getElementById('wallet-btn-label').textContent = 'Connect Wallet';
    document.getElementById('wallet-btn').title = '';

    connectedAddress = '';

    const chip = document.getElementById('address-chip');
    chip.classList.remove('filled');
    chip.title = '';
    document.getElementById('address-text').textContent = 'Connect your wallet to fill this in';
}

window.addEventListener('pageshow', function () {
    bdxWallet = null;
    setWalletDisconnectedUI();
    clearStatus();
});

async function connectWallet() {
    const btn = document.getElementById('wallet-btn');

    if (bdxWallet && bdxWallet.isConnected) {
        try {
            await bdxWallet.disconnect();
        } catch (err) {
            console.error('Wallet disconnect error:', err);
        }
        bdxWallet = null;
        setWalletDisconnectedUI();
        return;
    }

    clearStatus();

    if (typeof BdxWeb3 === 'undefined') {
        setStatus('error', 'Beldex Wallet SDK failed to load. Please refresh the page.');
        return;
    }

    btn.disabled = true;
    document.getElementById('wallet-btn-label').textContent = 'Connecting...';

    try {
        const provider = await BdxWeb3.detectProvider({ timeoutMs: 3000 });

        if (!provider) {
            setStatus('error', 'Beldex Wallet extension not found. Please install it and refresh the page.');
            setWalletDisconnectedUI();
            return;
        }

        bdxWallet = new BdxWeb3.BeldexWeb3(provider);

        bdxWallet.on('accountsChanged', account => {
            if (account && account.address) {
                setWalletConnectedUI(account.address);
            } else {
                setWalletDisconnectedUI();
            }
        });
        bdxWallet.on('disconnect', () => {
            bdxWallet = null;
            setWalletDisconnectedUI();
        });

        const { address } = await bdxWallet.connect();
        setWalletConnectedUI(address);
    } catch (err) {
        console.error('Wallet connect error:', err);
        bdxWallet = null;
        setWalletDisconnectedUI();

        if (BdxWeb3.BdxRpcError.isLocked(err)) {
            setStatus('error', 'Your Beldex Wallet is locked. Unlock the extension and try again.');
        } else if (BdxWeb3.BdxRpcError.isUserRejection(err)) {
            setStatus('error', 'Wallet connection request was rejected.');
        } else if (err.code === 4902) { // ERR.PANEL_CLOSED — wallet's side panel isn't open
            setStatus('error', err.message);
        } else {
            setStatus('error', `Could not connect to Beldex Wallet: ${err.message || err}`);
        }
    } finally {
        btn.disabled = false;
    }
}

function submit_key() {
    const address = connectedAddress.trim();

    if (!address) {
        setStatus('error', 'Please connect your Beldex Wallet first.');
        return;
    }

    const btn = submitBtn();
    btn.disabled = true;
    btn.textContent = 'Sending...';
    setStatus('pending', 'Your request is being processed. Please stand by...');

    const payload = { address: address };

    fetch('/transfer', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(payload)
    })
        .then(response => response.json())
        .then(data => {
            if (data.status) {
                const amount = escapeHtml(data.amount);
                const txHash = escapeHtml(data.tx_hash);
                const txUrl = `https://testnet.beldex.dev/tx/${encodeURIComponent(String(data.tx_hash))}`;
                const warning = data.warning
                    ? ` ${escapeHtml(data.warning)} Do not submit the transaction again.`
                    : '';
                setStatus('success', `Transaction successful! ${amount} BDX was sent. Reference: ${txHash}.${warning}`);
                setTimeout(() => {
                    setStatus('success', `Transaction successful! ${amount} BDX was sent. Reference: <a href="${txUrl}" target="_blank" rel="noopener noreferrer">${txHash}</a>.${warning}`);
                }, 3000);
            } else if (data['tx-error']) {
                setStatus('error', `${escapeHtml(data['tx-error'])}. Please try again later or <a href="https://testnet.support.beldex.io" target="_blank" rel="noopener noreferrer">contact support</a>.`);
            } else if (data.error) {
                setStatus('error', escapeHtml(data.error));
            } else {
                setStatus('error', 'Unexpected response.');
            }
        })
        .catch(error => {
            console.error('Fetch error: ', error);
            setStatus('error', `Fetch error: ${escapeHtml(error)}`);
        })
        .finally(() => {
            btn.disabled = false;
            btn.textContent = 'Get Faucet';
        });
}
