// The device has four HTTP sessions. Load the module graph in dependency order
// so a browser's six parallel module requests cannot evict an active response.
try {
  for (const module of ["osc", "model", "resources", "firmware", "client", "view", "updates", "app"])
    await import(`./${module}.js`);
} catch {
  const notice = document.querySelector("#notice");
  notice.textContent = "The interface could not finish loading. Reload to try again.";
  notice.hidden = false;
  document.querySelector("#connection-status").textContent = "Loading error";
  document.querySelector("#connect-button").addEventListener("click", () => location.reload());
}
