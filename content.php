<?php
// No server-side state needed — all communication goes through FPP's command API.
?>

<div class="container-fluid">

    <!-- Overview -->
    <div class="card card-outline card-primary">
        <div class="card-header">
            <h3 class="card-title"><i class="fas fa-plug"></i> WLED Check Status</h3>
        </div>
        <div class="card-body">
            <p class="text-muted small mb-0">
                Check and recover WLED device power state from the browser or from FPP playlist entries.
                Use <strong>Ensure Power On</strong> as the first step in your show playlist to guarantee
                all WLED devices are on before sequences start.
            </p>
        </div>
    </div>

    <!-- Ensure Power On -->
    <div class="card card-outline card-warning mt-3">
        <div class="card-header">
            <h3 class="card-title"><i class="fas fa-power-off"></i> Ensure Power On</h3>
        </div>
        <div class="card-body">
            <div class="form-group row mb-2">
                <label class="col-sm-2 col-form-label col-form-label-sm"><strong>IP Address</strong></label>
                <div class="col-sm-10">
                    <input type="text" id="wledIP" class="form-control form-control-sm"
                           style="max-width:200px;display:inline-block"
                           placeholder="192.168.1.50">
                </div>
            </div>
            <div class="form-group row mb-2">
                <label class="col-sm-2 col-form-label col-form-label-sm"><strong>Brightness</strong></label>
                <div class="col-sm-10">
                    <input type="number" id="wledBri" class="form-control form-control-sm"
                           style="max-width:100px;display:inline-block"
                           min="-1" max="255" value="-1">
                    <span class="text-muted small ml-2">0–255, or -1 to leave unchanged</span>
                </div>
            </div>
            <div class="form-group row">
                <div class="col-sm-10 offset-sm-2">
                    <button class="btn btn-warning btn-sm" onclick="sendEnsureOn()">
                        <i class="fas fa-check-circle"></i> Ensure Power On
                    </button>
                    <button class="btn btn-secondary btn-sm ml-2" onclick="sendTurnOff()">
                        <i class="fas fa-times-circle"></i> Turn Off
                    </button>
                    <span id="wledStatus" class="ml-3 small"></span>
                </div>
            </div>
        </div>
    </div>

    <!-- How to use in playlists -->
    <div class="card card-outline card-secondary mt-3">
        <div class="card-header">
            <h3 class="card-title"><i class="fas fa-info-circle"></i> Using in Playlists &amp; Presets</h3>
            <div class="card-tools">
                <button type="button" class="btn btn-tool" data-card-widget="collapse">
                    <i class="fas fa-minus"></i>
                </button>
            </div>
        </div>
        <div class="card-body">
            <p class="mb-2">Two FPP commands are available under <strong>Sequences &rarr; Command Presets</strong>:</p>
            <table class="table table-sm table-bordered" style="max-width:720px">
                <thead class="thead-light">
                    <tr><th>Command</th><th>Arguments</th><th>Notes</th></tr>
                </thead>
                <tbody>
                    <tr>
                        <td><code>WLED - Ensure Power On</code></td>
                        <td>IP Address, Brightness (optional, default -1)</td>
                        <td>
                            Checks device state and corrects it if needed.
                            Handles three cases: device off (idle), device off while receiving live data,
                            and device on with brightness at 0.
                            Blocks until complete — safe to use as a Lead-In playlist step.
                        </td>
                    </tr>
                    <tr>
                        <td><code>WLED - Turn Off</code></td>
                        <td>IP Address</td>
                        <td>Sends a power-off command. Use at the end of a show playlist.</td>
                    </tr>
                </tbody>
            </table>
            <p class="text-muted small mb-0">
                <strong>Tip:</strong> Add one <em>Command</em> playlist entry per WLED device at the top
                of your Lead-In playlist, each firing <code>WLED - Ensure Power On</code> with the
                device's IP. They run sequentially, so all devices are confirmed on before the first
                sequence starts.
            </p>
        </div>
    </div>

</div>

<script>
function apiCommand(cmd, args) {
    const parts = ['/api/command', encodeURIComponent(cmd)].concat(args.map(encodeURIComponent));
    return fetch(parts.join('/'), { method: 'GET' })
        .then(r => r.ok ? r.json() : Promise.reject(r.status));
}

function setStatus(ok, msg) {
    $('#wledStatus').html(ok
        ? '<span class="text-success"><i class="fas fa-check"></i> ' + msg + '</span>'
        : '<span class="text-danger"><i class="fas fa-times"></i> ' + msg + '</span>'
    );
}

function getIP() {
    const ip = $('#wledIP').val().trim();
    if (!ip) { setStatus(false, 'Enter an IP address'); return null; }
    return ip;
}

function sendEnsureOn() {
    const ip = getIP(); if (!ip) return;
    const bri = $('#wledBri').val();
    setStatus(true, 'Checking ' + ip + '…');
    apiCommand('WLED - Ensure Power On', [ip, String(bri)])
        .then(r => setStatus(true, r.result || 'Done'))
        .catch(() => setStatus(false, 'Failed — is the plugin loaded?'));
}

function sendTurnOff() {
    const ip = getIP(); if (!ip) return;
    setStatus(true, 'Turning off ' + ip + '…');
    apiCommand('WLED - Turn Off', [ip])
        .then(() => setStatus(true, 'Turned off ' + ip))
        .catch(() => setStatus(false, 'Failed — is the plugin loaded?'));
}
</script>
