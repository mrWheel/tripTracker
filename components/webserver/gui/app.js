//-- These GUI assets are served from LittleFS and must never be deletable.
var PROTECTED_LITTLEFS_FILES = ["style.css", "index.html", "app.js"];

//-- Chunk size used when streaming an upload to the device over the WebSocket.
var UPLOAD_CHUNK_SIZE = 4096;

var ws = null;
var pendingDownload = null;

function currentStore()
{
  return document.querySelector('input[name="store"]:checked').value;
}

function isProtectedFile(store, name)
{
  return store === "fs" && PROTECTED_LITTLEFS_FILES.indexOf(name) !== -1;
}

function setStatus(message)
{
  document.getElementById("statusMessage").textContent = message;
}

function formatSize(bytes)
{
  if (bytes < 1024)
  {
    return bytes + " B";
  }
  return (bytes / 1024).toFixed(1) + " kB";
}

function formatDistance(meters)
{
  if (meters >= 1000)
  {
    return (meters / 1000).toFixed(1) + " km";
  }
  return Math.round(meters) + " m";
}

function formatSpeed(kmh)
{
  return kmh.toFixed(1) + " km/h";
}

function showOverlay(title)
{
  document.getElementById("wsOverlayTitle").textContent = title;
  document.getElementById("wsOverlay").hidden = false;
}

function hideOverlay()
{
  document.getElementById("wsOverlay").hidden = true;
}

function send(message)
{
  if (ws && ws.readyState === WebSocket.OPEN)
  {
    ws.send(JSON.stringify(message));
  }
}

function refreshFileList()
{
  send({ type: "list", store: currentStore() });
}

function renderFileList(files)
{
  var body = document.getElementById("fileTableBody");
  body.innerHTML = "";
  files.forEach(function (file)
  {
    var row = document.createElement("tr");

    var nameCell = document.createElement("td");
    nameCell.textContent = file.name;
    row.appendChild(nameCell);

    var sizeCell = document.createElement("td");
    sizeCell.textContent = formatSize(file.size);
    row.appendChild(sizeCell);

    var actionCell = document.createElement("td");

    var downloadButton = document.createElement("button");
    downloadButton.textContent = "Download";
    downloadButton.className = "btn";
    downloadButton.style.marginRight = "0.5em";
    downloadButton.onclick = function ()
    {
      downloadFile(file.name);
    };
    actionCell.appendChild(downloadButton);

    var deleteButton = document.createElement("button");
    deleteButton.textContent = "Delete";
    deleteButton.className = "btn btn-danger";
    if (isProtectedFile(currentStore(), file.name))
    {
      deleteButton.disabled = true;
    }
    else
    {
      deleteButton.onclick = function ()
      {
        deleteFile(file.name);
      };
    }
    actionCell.appendChild(deleteButton);

    row.appendChild(actionCell);

    var distanceCell = document.createElement("td");
    distanceCell.textContent = file.distance_m !== undefined ? formatDistance(file.distance_m) : "-";
    row.appendChild(distanceCell);

    var avgSpeedCell = document.createElement("td");
    avgSpeedCell.textContent = file.avg_speed_kmh !== undefined ? formatSpeed(file.avg_speed_kmh) : "-";
    row.appendChild(avgSpeedCell);

    body.appendChild(row);
  });
}

function deleteFile(name)
{
  send({ type: "delete", store: currentStore(), name: name });
}

function downloadFile(name)
{
  pendingDownload = { name: name, chunks: [] };
  send({ type: "download", store: currentStore(), name: name });
}

function finishDownload(name)
{
  if (!pendingDownload)
  {
    return;
  }
  var blob = new Blob(pendingDownload.chunks);
  var url = URL.createObjectURL(blob);
  var link = document.createElement("a");
  link.href = url;
  link.download = name;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
  pendingDownload = null;
  setStatus("Downloaded " + name);
}

function uploadFile()
{
  var input = document.getElementById("uploadFile");
  if (input.files.length === 0)
  {
    setStatus("Select a file first.");
    return;
  }

  var file = input.files[0];
  var reader = new FileReader();
  reader.onload = function ()
  {
    var data = reader.result;
    send({ type: "upload_start", store: currentStore(), name: file.name, size: data.byteLength });
    for (var offset = 0; offset < data.byteLength; offset += UPLOAD_CHUNK_SIZE)
    {
      ws.send(data.slice(offset, offset + UPLOAD_CHUNK_SIZE));
    }
    send({ type: "upload_end" });
    input.value = "";
  };
  reader.onerror = function ()
  {
    setStatus("Failed to read " + file.name + " for upload.");
  };
  reader.readAsArrayBuffer(file);
}

function handleWsMessage(event)
{
  if (event.data instanceof ArrayBuffer)
  {
    if (pendingDownload)
    {
      pendingDownload.chunks.push(event.data);
    }
    return;
  }

  var message = JSON.parse(event.data);
  switch (message.type)
  {
    case "hello":
      //-- Connection-confirmed ack; the file list is already requested from
      //-- ws.onopen, so no action needed here.
      break;
    case "taken_over":
      showOverlay("Connection lost or taken over");
      break;
    case "files":
      renderFileList(message.files);
      break;
    case "delete_ack":
      setStatus(message.ok ? "Deleted " + message.name : "Failed to delete " + message.name);
      refreshFileList();
      break;
    case "upload_ack":
      setStatus(message.ok ? "Uploaded " + message.name : "Upload failed");
      refreshFileList();
      break;
    case "download_end":
      finishDownload(message.name);
      break;
    case "error":
      setStatus("Error: " + message.message);
      break;
    default:
      break;
  }
}

function connectWebSocket()
{
  hideOverlay();
  var protocol = location.protocol === "https:" ? "wss:" : "ws:";
  ws = new WebSocket(protocol + "//" + location.host + "/ws");
  ws.binaryType = "arraybuffer";
  ws.onmessage = handleWsMessage;
  ws.onopen = function ()
  {
    //-- The server never pushes anything before it sees a data frame from
    //-- us (see check_takeover() in webserver_api.c), so we have to speak first.
    refreshFileList();
  };
  ws.onclose = function ()
  {
    showOverlay("Connection lost or taken over");
  };
}

document.getElementById("uploadButton").addEventListener("click", uploadFile);
document.getElementById("refreshButton").addEventListener("click", refreshFileList);
document.getElementById("wsReconnectButton").addEventListener("click", connectWebSocket);
document.querySelectorAll('input[name="store"]').forEach(function (radio)
{
  radio.addEventListener("change", refreshFileList);
});

connectWebSocket();

