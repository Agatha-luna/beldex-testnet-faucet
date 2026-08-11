window.addEventListener('pageshow', function (event) {
    document.getElementById("key").value = "";
    document.getElementById('tx-hash').textContent = "";
});

function submit_key() {
    document.getElementById('tx-hash').textContent = "Your request is being processed. Please stand by...";
    address = document.getElementById('key').value;
    console.log(address);

    const payload = {
        address: address
    };

    fetch('http://localhost:5000/transfer', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(payload)
    })
    .then(response => {
        return response.json();
    })
    .then(data => {
        console.log(data);

        if(data.status) {
            document.getElementById('tx-hash').innerHTML = `Transaction Successful! ${data.amount} BDX was sent. Reference: ${data.tx_hash}.`;
            setTimeout(() => {
                document.getElementById('tx-hash').innerHTML = `Transaction Successful! ${data.amount} BDX was sent. Reference: <a href="https://testnet.beldex.dev/tx/${data.tx_hash}" target="_blank" rel="noopener noreferrer">${data.tx_hash}</a>.`;
            }, 3000);
        } else if (data['tx-error']) {
            document.getElementById('tx-hash').innerHTML = `${data['tx-error']}. Please try again later or <a href="https://testnet.support.beldex.io" target="_blank" rel="noopener noreferrer">contact support</a>.`;
        }else if (data.error) {
            document.getElementById('tx-hash').textContent = `${data.error}`;
        } else {
            document.getElementById('tx-hash').textContent = `Unexpected Response`;
        }
    })
    .catch(error => {
        console.error('Fetch error: ', error);
        document.getElementById('tx-hash').textContent = `Fetch error: ${error}`;
    });
}