// Failed module imports remain cached in a document. A bounded page reload
// starts a fresh module graph; retrying import() in this page would not recover.
export async function startInterface({ document, storage, reload,
  loadModule = name => import(`./${name}.js`), wait = ms => new Promise(resolve => setTimeout(resolve, ms)),
} = {}) {
  const key = "blip.interface-startup.0.3.10";
  const recovery = document.querySelector("#startup-recovery");
  if (recovery) recovery.hidden = true;
  try {
    // Four device HTTP sessions: consume dependencies sequentially, including
    // files before client, rather than starting parallel module requests.
    for (const name of ["osc", "model", "resources", "firmware", "files", "client", "view", "updates", "app"])
      await loadModule(name);
    try { storage?.removeItem(key); } catch {}
    return true;
  } catch (error) {
    const notice = document.querySelector("#notice"), button = document.querySelector("#connect-button");
    notice.hidden = false;
    let retry = 0;
    // Without persistent page-session storage, automatic reloads could loop.
    if (error instanceof TypeError) try {
      const previous = Number(storage.getItem(key) ?? 0);
      if (Number.isInteger(previous) && previous >= 0 && previous < 2) {
        storage.setItem(key, String(previous + 1)); retry = previous + 1;
      }
    } catch {}
    if (retry) {
      notice.textContent = `The connection interrupted interface loading. Retrying (${retry}/2)…`;
      document.querySelector("#connection-status").textContent = "Retrying interface";
      button.disabled = true;
      await wait(retry * 1000); reload();
    } else {
      notice.textContent = "The interface could not finish loading. Choose Retry loading to try again.";
      document.querySelector("#connection-status").textContent = "Loading error";
      button.disabled = false; button.textContent = "Retry loading";
      button.addEventListener("click", event => { event.preventDefault(); try { storage?.removeItem(key); } catch {} reload(); });
    }
    return false;
  }
}

if (typeof document !== "undefined") {
  let storage;
  try { storage = globalThis.sessionStorage; } catch {}
  await startInterface({ document, storage, reload: () => location.reload() });
}
