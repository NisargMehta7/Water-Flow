#ifndef WEBPAGE_H
#define WEBPAGE_H

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Feedlot Water Monitor</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3/dist/chartjs-adapter-date-fns.bundle.min.js"></script>
<style>
  body{font-family:Arial,sans-serif;margin:0;padding:16px;background:#f4f4f4;color:#222;}
  h1{color:#2c5f2e;margin-bottom:4px;}
  h2{color:#2c5f2e;margin-top:24px;}
  .card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;
        box-shadow:0 1px 4px rgba(0,0,0,0.1);}
  table{width:100%;border-collapse:collapse;font-size:0.85em;}
  th{background:#2c5f2e;color:#fff;padding:8px;text-align:left;}
  td{padding:7px 8px;border-bottom:1px solid #eee;}
  tr:hover td{background:#f0f7f0;}
  .btn{display:inline-block;padding:9px 18px;border-radius:6px;text-decoration:none;
       font-weight:bold;margin-right:8px;cursor:pointer;border:none;font-size:0.95em;}
  .btn-green{background:#2c5f2e;color:#fff;}
  .btn-red{background:#c0392b;color:#fff;}
  .filter-btn{background:#ddd;color:#333;}
  .filter-btn.active{background:#2c5f2e;color:#fff;}
  .warning-box{background:#fdecea;border:1px solid #c0392b;color:#c0392b;
                border-radius:8px;padding:12px 16px;margin-bottom:12px;font-weight:bold;}
  .settings-form input[type=number]{width:70px;padding:6px;font-size:1em;
                border:1px solid #ccc;border-radius:6px;margin:0 8px;}
  .btn-gray{background:#888;color:#fff;}
  .pagination{display:flex;align-items:center;gap:8px;margin-top:12px;}
  canvas{max-height:320px;}
</style>
</head>
<body>
<h1>Feedlot Water Monitor</h1>
<p style="color:#666;font-size:0.85em;">Firmware v FW_VER &nbsp;|&nbsp; Refresh page to update.</p>

WARNING_BOXES_PLACEHOLDER

<div class='card'>
  <button class="btn filter-btn active" data-filter="all" onclick="filterTable('all',this)">All</button>
  SENDER_BUTTONS_PLACEHOLDER
</div>

<div class='card'><h2>Volume Over Time</h2>
<canvas id="volChart"></canvas>
</div>

<div class='card'>
  <h2>Settings</h2>
  <form class="settings-form" action="/setthreshold" method="GET">
    <div style="margin-bottom:4px;">
      No-flow warning after
      <input type="number" name="hours" min="1" max="1000" value="THRESHOLD_PLACEHOLDER">
      hours
    </div>
    <p style="color:#666;font-size:0.85em;margin:0 0 14px 0;">Current setting: THRESHOLD_PLACEHOLDER hours</p>
    <div style="margin-bottom:4px;">
      No-packet warning after
      <input type="number" name="nopacket" min="1" max="1000" value="NOPKT_PLACEHOLDER">
      missed packets
    </div>
    <p style="color:#666;font-size:0.85em;margin:0 0 14px 0;">Current setting: NOPKT_PLACEHOLDER missed packets</p>
    <button type="submit" class="btn btn-green">Save</button>
  </form>
</div>

<div class='card'><h2>All Data</h2>
<div style='overflow-x:auto'>
<table id="dataTable"><thead><tr>
<th>Timestamp</th><th>Sender</th>
<th>Flow (L/min)</th><th>Volume (L)</th>
<th>RSSI</th><th>SNR</th>
</tr></thead><tbody>
TABLE_ROWS_PLACEHOLDER
</tbody></table></div>
<div class="pagination">
  <button class="btn btn-gray" onclick="changePage(-1)">&#8592; Prev</button>
  <span id="pageInfo">Page 1</span>
  <button class="btn btn-gray" onclick="changePage(1)">Next &#8594;</button>
  <span style="color:#666;font-size:0.85em" id="rowInfo"></span>
</div>
</div>

<div class='card'>
<a href='/csv' class='btn btn-green'>Download CSV</a>
</div>

<script>
// ---- Pagination ----
const ROWS_PER_PAGE = 50;
let currentPage  = 1;
let filteredIdxs = [];

function getAllRows() {
  return Array.from(document.querySelectorAll('#dataTable tbody tr'));
}

function applyFilter(filter) {
  currentPage  = 1;
  const all    = getAllRows();
  filteredIdxs = [];
  all.forEach((r, i) => {
    if (filter === 'all' || r.getAttribute('data-sender') === filter)
      filteredIdxs.push(i);
  });
  renderPage();
}

function renderPage() {
  const all        = getAllRows();
  const totalPages = Math.max(1, Math.ceil(filteredIdxs.length / ROWS_PER_PAGE));
  currentPage      = Math.min(currentPage, totalPages);
  const start      = (currentPage - 1) * ROWS_PER_PAGE;
  const end        = start + ROWS_PER_PAGE;
  const pageSet    = new Set(filteredIdxs.slice(start, end));
  all.forEach((r, i) => { r.style.display = pageSet.has(i) ? '' : 'none'; });
  document.getElementById('pageInfo').textContent = 'Page ' + currentPage + ' of ' + totalPages;
  document.getElementById('rowInfo').textContent  = filteredIdxs.length + ' record(s)';
}

function changePage(dir) {
  const totalPages = Math.max(1, Math.ceil(filteredIdxs.length / ROWS_PER_PAGE));
  currentPage = Math.max(1, Math.min(totalPages, currentPage + dir));
  renderPage();
}

// ---- Chart ----
const northData = {
  label:   'Tap 1',
  labels:  [CHART_LABELS_1_PLACEHOLDER],
  volumes: [CHART_VOLUME_1_PLACEHOLDER]
};
const southData = {
  label:   'Tap 2',
  labels:  [CHART_LABELS_2_PLACEHOLDER],
  volumes: [CHART_VOLUME_2_PLACEHOLDER]
};

function toPoints(data) {
  return data.volumes.map((v, i) => ({ x: data.labels[i], y: parseFloat(v) }));
}

let volChart;

function buildChart(filter) {
  const ctx = document.getElementById('volChart').getContext('2d');
  if (volChart) volChart.destroy();
  let datasets = [];
  [northData, southData].forEach(sender => {
    if (filter === 'all' || filter === sender.label) {
      datasets.push({
        label: sender.label,
        data: toPoints(sender),
        backgroundColor: sender.label === 'Tap 1' ? 'rgba(44,95,46,0.8)' : 'rgba(230,126,34,0.8)',
        borderColor:     sender.label === 'Tap 1' ? '#2c5f2e' : '#e67e22',
        showLine: false, pointRadius: 5
      });
    }
  });
  volChart = new Chart(ctx, {
    type: 'scatter',
    data: { datasets: datasets },
    options: {
      responsive: true,
      showLine: false,
      plugins: {
        tooltip: {
          callbacks: {
            label: ctx => ctx.dataset.label + ': ' + ctx.raw.x + ' = ' + ctx.raw.y + ' L'
          }
        }
      },
      scales: {
        y: { beginAtZero: true, title: { display: true, text: 'Volume (L)' } },
        x: {
          type: 'time',
          time: {
            parser: 'yyyy-MM-dd HH:mm:ss',
            tooltipFormat: 'yyyy-MM-dd HH:mm',
            displayFormats: {
            minute: 'HH:mm',
            hour:   'MMM d HH:mm',
            day:    'MMM d',
            week:   'MMM d'
          }
          },
          title: { display: true, text: 'Timestamp' },
          ticks: { autoSkip: true, maxRotation: 60, minRotation: 45, maxTicksLimit: 10 }
        }
      }
    }
  });
}

function filterTable(sender, btn) {
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  applyFilter(sender);
  buildChart(sender);
}

buildChart('all');
applyFilter('all');
</script>
</body></html>
)rawliteral";

#endif