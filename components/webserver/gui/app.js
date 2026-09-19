//-- These GUI assets are served from LittleFS and must never be deletable.
var PROTECTED_LITTLEFS_FILES = ["style.css", "index.html", "app.js"];

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

function refreshFileList()
{
  fetch("/api/files?store=" + currentStore() + "&refresh=" + Date.now(), { cache: "no-store" })
    .then(function (response) { return response.json(); })
    .then(function (files)
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
          window.location = "/api/download?store=" + currentStore() + "&name=" + encodeURIComponent(file.name);
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
    })
    .catch(function (error)
    {
      setStatus("Failed to load file list: " + error);
    });
}

function deleteFile(name)
{
  fetch("/api/delete?store=" + currentStore() + "&name=" + encodeURIComponent(name), { method: "DELETE" })
    .then(function ()
    {
      setStatus("Deleted " + name);
      refreshFileList();
    })
    .catch(function (error)
    {
      setStatus("Failed to delete " + name + ": " + error);
    });
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
  fetch("/api/upload?store=" + currentStore() + "&name=" + encodeURIComponent(file.name), {
    method: "POST",
    body: file,
  })
    .then(function ()
    {
      setStatus("Uploaded " + file.name);
      input.value = "";
      refreshFileList();
    })
    .catch(function (error)
    {
      setStatus("Failed to upload " + file.name + ": " + error);
    });
}

document.getElementById("uploadButton").addEventListener("click", uploadFile);
document.getElementById("refreshButton").addEventListener("click", refreshFileList);
document.querySelectorAll('input[name="store"]').forEach(function (radio)
{
  radio.addEventListener("change", refreshFileList);
});

refreshFileList();
