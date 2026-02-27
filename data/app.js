// Current color selection
let selR = 255, selG = 60, selB = 10;

// ── Helpers ──────────────────────────────────────────────

function pad(n) {
  return String(n).padStart(2, '0');
}

function timeStrToMs(t) {
  const [h, m] = t.split(':').map(Number);
  return (h * 60 + m) * 60000;
}

function msDuration(ms) {
  const total = Math.round(ms / 60000);
  if (total <= 0) return '—';
  if (total < 60) return total + ' min';
  const h = Math.floor(total / 60), m = total % 60;
  return m ? `${h}h ${m}m` : `${h}h`;
}

function rgbToHex(r, g, b) {
  return '#' + [r, g, b].map(v => pad(v.toString(16))).join('');
}

// ── UI updates ────────────────────────────────────────────

function updateDurationLabel() {
  const start = document.getElementById('alarmTime').value;
  const end   = document.getElementById('endTime').value;
  if (!start || !end) return;
  let s = timeStrToMs(start), e = timeStrToMs(end);
  if (e <= s) e += 86400000;
  document.getElementById('durationLabel').textContent = 'Ramp: ' + msDuration(e - s);
}

function updateRampPreview() {
  // Always shows the fixed sunrise gradient — manual color never changes this
  document.getElementById('rampPreview').style.background =
    'linear-gradient(90deg, #000000 0%, #3d0000 30%, #ff3c0a 100%)';
}

function setStatus(state, text) {
  document.getElementById('statusDot').className = 'status-dot ' + state;
  document.getElementById('statusText').textContent = text;
}

// ── API ───────────────────────────────────────────────────

async function call(url) {
  try {
    const res = await fetch(url);
    return res.ok;
  } catch {
    setStatus('', 'Unreachable');
    return false;
  }
}

// ── Status polling ─────────────────────────────────────────

async function pollStatus() {
  try {
    const res = await fetch('/status');
    if (!res.ok) throw new Error();
    const d = await res.json();

    // Clock
    document.getElementById('serverTime').textContent = d.time;

    // Progress card
    const card = document.getElementById('progressCard');
    if (d.isFading) {
      card.style.display = 'block';
      const pct = Math.round(d.progress * 100);
      document.getElementById('progressPct').textContent = pct + '%';
      document.getElementById('progressFill').style.width = pct + '%';
      setStatus('fading', 'Ramping ' + pct + '%');
    } else {
      card.style.display = 'none';
      if (d.alarmEnabled) {
        setStatus('armed', `${pad(d.alarmHour)}:${pad(d.alarmMin)}`);
      } else {
        setStatus('idle', 'No alarm');
      }
    }

    // Sync alarm toggle (only if not mid-interaction)
    if (document.activeElement.type !== 'checkbox') {
      document.getElementById('alarmEnabled').checked = d.alarmEnabled;
    }

    // Sync alarm time fields if unfocused
    if (document.activeElement !== document.getElementById('alarmTime')) {
      document.getElementById('alarmTime').value = pad(d.alarmHour) + ':' + pad(d.alarmMin);
    }

    // Sync UTC offset if unfocused
    if (document.activeElement !== document.getElementById('utcOffset')) {
      document.getElementById('utcOffset').value = Math.round(d.utcOffset / 3600);
    }

  } catch {
    setStatus('', 'Connecting…');
  }
}

// ── Color selection ────────────────────────────────────────

function selectSwatch(swatch) {
  document.querySelectorAll('.color-swatch').forEach(s => s.classList.remove('active'));
  swatch.classList.add('active');
}

document.querySelectorAll('.color-swatch:not(#customSwatch)').forEach(sw => {
  sw.addEventListener('click', () => {
    selectSwatch(sw);
    selR = parseInt(sw.dataset.r);
    selG = parseInt(sw.dataset.g);
    selB = parseInt(sw.dataset.b);
    document.getElementById('customColorRow').style.display = 'none';
    updateRampPreview();
    call(`/on?r=${selR}&g=${selG}&b=${selB}`);
  });
});

document.getElementById('customSwatch').addEventListener('click', () => {
  selectSwatch(document.getElementById('customSwatch'));
  document.getElementById('customColorRow').style.display = 'flex';
  updateRampPreview();
});

document.getElementById('colorPicker').addEventListener('input', e => {
  const hex = e.target.value;
  selR = parseInt(hex.slice(1, 3), 16);
  selG = parseInt(hex.slice(3, 5), 16);
  selB = parseInt(hex.slice(5, 7), 16);
  document.getElementById('colorHex').textContent = hex;
  updateRampPreview();
});

// ── Alarm card ─────────────────────────────────────────────

document.getElementById('alarmTime').addEventListener('change', updateDurationLabel);
document.getElementById('endTime').addEventListener('change', updateDurationLabel);

document.getElementById('setAlarmBtn').addEventListener('click', async () => {
  const startStr = document.getElementById('alarmTime').value;
  const endStr   = document.getElementById('endTime').value;
  const enabled  = document.getElementById('alarmEnabled').checked ? 1 : 0;
  if (!startStr || !endStr) return;

  const [hour, min] = startStr.split(':').map(Number);
  let s = timeStrToMs(startStr), e = timeStrToMs(endStr);
  if (e <= s) e += 86400000;
  const duration = e - s;

  const ok = await call(
    `/setalarm?hour=${hour}&min=${min}&duration=${duration}&enabled=${enabled}`
  );
  if (ok) {
    const btn = document.getElementById('setAlarmBtn');
    const orig = btn.textContent;
    btn.textContent = 'Saved ✓';
    setTimeout(() => { btn.textContent = orig; }, 1800);
    pollStatus();
  }
});

// ── Manual controls ────────────────────────────────────────

document.getElementById('btnOn').addEventListener('click', () => {
  call(`/on?r=${selR}&g=${selG}&b=${selB}`);
});

document.getElementById('btnRamp').addEventListener('click', () => {
  const startStr = document.getElementById('alarmTime').value;
  const endStr   = document.getElementById('endTime').value;
  let duration = 3600000;
  if (startStr && endStr) {
    let s = timeStrToMs(startStr), e = timeStrToMs(endStr);
    if (e <= s) e += 86400000;
    duration = e - s;
  }
  call(`/start?time=${duration}`);
  setTimeout(pollStatus, 800);
});

document.getElementById('btnOff').addEventListener('click', () => {
  call('/stop');
  setTimeout(pollStatus, 500);
});

// ── Settings ────────────────────────────────────────────────

document.getElementById('saveSettingsBtn').addEventListener('click', async () => {
  const offset = parseInt(document.getElementById('utcOffset').value) || 0;
  const ok = await call(`/setalarm?utcoffset=${offset * 3600}`);
  if (ok) {
    const btn = document.getElementById('saveSettingsBtn');
    btn.textContent = 'Saved ✓';
    setTimeout(() => { btn.textContent = 'Save'; }, 1800);
  }
});

// ── Init ─────────────────────────────────────────────────────

updateDurationLabel();
updateRampPreview();
pollStatus();
setInterval(pollStatus, 5000);
