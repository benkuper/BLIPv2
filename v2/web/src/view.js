import { coerceControlValue, filterControlModel } from "./model.js";
import { buildReassignment, filterPins, pinOutcome } from "./resources.js";

function element(document, tag, className, content) {
  const result = document.createElement(tag);
  if (className) result.className = className;
  if (content !== undefined) result.textContent = content;
  return result;
}

function displayValue(values, empty = "Done") {
  if (!Array.isArray(values) || values.length === 0) return empty;
  return values
    .map(({ value }) => (typeof value === "bigint" ? value.toString() : String(value)))
    .join(" · ");
}

function fieldInput(document, field, currentValue) {
  const input = element(document, "input");
  input.setAttribute("aria-label", field.id);
  input.dataset.valueType = field.type;
  input.required = field.required;
  if (field.type === "boolean") {
    input.type = "checkbox";
    input.checked = currentValue === true;
  } else if (field.type === "integer" || field.type === "number") {
    input.type = "number";
    input.step = field.type === "integer" ? "1" : "any";
    if (currentValue !== undefined) input.value = String(currentValue);
  } else {
    input.type = "text";
    if (currentValue !== undefined) input.value = String(currentValue);
  }
  return input;
}

function readInput(input) {
  const raw = input.type === "checkbox" ? input.checked : input.value;
  return coerceControlValue(input.dataset.valueType, raw);
}

export class ControlView {
  constructor({ document, root, emptyTemplate, onSend, onReassign, onError }) {
    this.document = document;
    this.root = root;
    this.emptyTemplate = emptyTemplate;
    this.onSend = onSend;
    this.onReassign = onReassign;
    this.onError = onError;
    this.model = null;
    this.query = "";
    this.rows = new Map();
    this.resources = null;
  }

  setModel(model) {
    this.model = model;
    this.render();
  }

  setFilter(query) {
    this.query = query;
    this.render();
  }

  setResources(resources) {
    this.resources = resources;
    this.render();
  }

  render() {
    this.root.replaceChildren();
    this.rows.clear();
    const components = this.model === null ? [] : filterControlModel(this.model, this.query);
    if (components.length === 0) {
      this.root.append(this.emptyTemplate.content.cloneNode(true));
      return;
    }
    for (const component of components) this.root.append(this.componentCard(component));
  }

  componentCard(component) {
    const card = element(this.document, "article", "component-card");
    const header = element(this.document, "header", "component-header");
    header.append(element(this.document, "h2", "", component.label));
    header.append(element(this.document, "p", "component-path", component.path));
    if (component.description) {
      header.append(element(this.document, "p", "component-description", component.description));
    }
    card.append(header);
    const list = element(this.document, "ul", "control-list");
    for (const control of component.controls) list.append(this.controlRow(control));
    card.append(list);
    return card;
  }

  controlRow(control) {
    const row = element(this.document, "li", "control-row");
    row.dataset.path = control.path;
    const copy = element(this.document, "div", "control-copy");
    copy.append(element(this.document, "span", "control-label", control.label));
    copy.append(element(this.document, "span", "control-hint", control.path));
    row.append(copy);
    const editor = element(this.document, "div", "control-editor");
    row.append(editor);
    this.renderEditor(editor, row, control);
    this.rows.set(control.path, { row, editor, control });
    return row;
  }

  renderEditor(editor, row, control) {
    if (control.editor === "pin") {
      this.renderPinEditor(editor, row, control);
      return;
    }
    if (control.editor === "readonly" || control.editor === "unsupported") {
      editor.append(
        element(
          this.document,
          "output",
          "value-output",
          displayValue(control.value === undefined ? [] : [{ value: control.value }], "—"),
        ),
      );
      return;
    }
    if (control.editor === "action") {
      const form = element(this.document, "form", "action-fields");
      const fields = control.fields.map((field) => {
        const input = fieldInput(this.document, field);
        form.append(input);
        return input;
      });
      const button = element(this.document, "button", "action-button", "Run");
      button.type = "submit";
      form.append(button);
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        this.trySubmit(row, control, fields);
      });
      editor.append(form);
      return;
    }

    let input;
    if (control.editor === "select") {
      input = element(this.document, "select");
      input.setAttribute("aria-label", control.label);
      input.dataset.valueType = control.type;
      for (const value of control.range.values) {
        const option = element(this.document, "option", "", String(value));
        option.value = String(value);
        option.selected = value === control.value;
        input.append(option);
      }
    } else {
      input = fieldInput(this.document, { id: control.label, type: control.type, required: true }, control.value);
      if (control.editor === "password") {
        input.type = "password";
        input.autocomplete = "new-password";
      }
      if (control.range.minimum !== undefined) input.min = String(control.range.minimum);
      if (control.range.maximum !== undefined) input.max = String(control.range.maximum);
      if (control.range.step !== undefined) input.step = String(control.range.step);
    }
    input.dataset.valueType = control.type;
    const form = element(this.document, "form", "control-editor");
    form.append(input);
    if (control.unit) form.append(element(this.document, "span", "unit", control.unit));
    if (control.editor !== "checkbox" && control.editor !== "select") {
      const apply = element(this.document, "button", "apply-button", "Apply");
      apply.type = "submit";
      form.append(apply);
    }
    const submit = (event) => {
      event?.preventDefault();
      this.trySubmit(row, control, [input]);
    };
    form.addEventListener("submit", submit);
    if (control.editor === "checkbox" || control.editor === "select") {
      input.addEventListener("change", submit);
    }
    editor.append(form);
  }

  renderPinEditor(editor, row, control) {
    if (this.resources === null) {
      editor.append(element(this.document, "output", "value-output", "Pin inventory unavailable"));
      return;
    }
    const select = element(this.document, "select", "pin-select");
    select.setAttribute("aria-label", control.label);
    for (const pin of this.resources.pins) {
      const outcome = pinOutcome(pin, control);
      const option = element(this.document, "option", "", `${pin.label} — ${outcome.state}: ${outcome.reason}`);
      option.value = pin.id;
      option.disabled = outcome.disabled;
      option.selected = pin.gpio === control.value;
      option.dataset.state = outcome.state;
      option.dataset.reason = outcome.reason;
      select.append(option);
    }
    const explanation = element(this.document, "p", "pin-explanation", "Choose a board-declared pin.");
    const confirmation = element(this.document, "div", "pin-confirmation");
    confirmation.hidden = true;
    const submitPin = async () => {
      const pin = this.resources.pins.find((candidate) => candidate.id === select.value);
      if (!pin) return;
      const outcome = pinOutcome(pin, control);
      explanation.textContent = outcome.reason;
      confirmation.replaceChildren();
      confirmation.hidden = true;
      if (outcome.state !== "conflict") {
        await this.submit(row, control, [{ type: "integer", value: pin.gpio }]);
        return;
      }
      const previous = outcome.owners[0];
      confirmation.hidden = false;
      confirmation.setAttribute("role", "alertdialog");
      confirmation.setAttribute("aria-label", "Confirm pin reassignment");
      confirmation.append(element(this.document, "p", "", `${control.resourceOwner} conflicts with ${previous.path}. Nothing will be changed until you choose an atomic operation.`));
      const run = async (operation) => {
        row.dataset.pending = "true";
        try {
          const request = buildReassignment({ snapshot: this.resources, control, pin, operation });
          const updated = await this.onReassign(request);
          this.resources = updated;
          this.render();
        } catch (error) {
          delete row.dataset.pending;
          this.onError(error);
        }
      };
      if (control.resourceSelector.supportsSwap) {
        const swap = element(this.document, "button", "apply-button", "Swap assignments");
        swap.type = "button";
        swap.addEventListener("click", () => run("swap"));
        confirmation.append(swap);
      }
      if (control.resourceSelector.supportsMove && previous.optional) {
        const move = element(this.document, "button", "apply-button", "Unassign previous and move");
        move.type = "button";
        move.addEventListener("click", () => run("unassign-and-move"));
        confirmation.append(move);
      } else {
        confirmation.append(element(this.document, "p", "pin-explanation", `${previous.path} is required and cannot be unassigned.`));
      }
      if (previous.rebootRequired) confirmation.append(element(this.document, "p", "pin-explanation", "Reboot required for the affected component."));
    };
    select.addEventListener("change", () => { void submitPin(); });
    editor.append(select, explanation, confirmation);
  }

  async submit(row, control, values) {
    row.dataset.pending = "true";
    try {
      await this.onSend(control, values);
    } catch (error) {
      delete row.dataset.pending;
      this.onError(error);
    }
  }

  trySubmit(row, control, inputs) {
    try {
      this.submit(row, control, inputs.map(readInput));
    } catch (error) {
      this.onError(error);
    }
  }

  applyMessage(message) {
    const entry = this.rows.get(message.address);
    if (entry === undefined) return;
    delete entry.row.dataset.pending;
    const output = entry.editor.querySelector("output");
    if (output !== null) output.textContent = displayValue(message.values);
    if (message.values.length !== 1) return;
    const value = message.values[0].value;
    if (entry.control.kind === "parameter") entry.control.value = value;
    const input = entry.editor.querySelector("input, select");
    if (input === null || entry.control.editor === "password") return;
    if (input.type === "checkbox") input.checked = value === true;
    else input.value = String(value);
  }
}

export class ReservationView {
  constructor({ document, root, onNavigate }) {
    this.document = document;
    this.root = root;
    this.onNavigate = onNavigate;
    this.snapshot = null;
    this.query = "";
  }

  setSnapshot(snapshot) { this.snapshot = snapshot; this.render(); }
  setFilter(query) { this.query = query; this.render(); }

  render() {
    this.root.replaceChildren();
    if (!this.snapshot) return;
    const list = element(this.document, "ul", "reservation-list");
    for (const pin of filterPins(this.snapshot, this.query)) {
      const item = element(this.document, "li", "reservation-row");
      item.dataset.pin = pin.id;
      item.append(element(this.document, "strong", "", `${pin.label} (${pin.id})`));
      item.append(element(this.document, "span", "reservation-state", `${pin.state} · ${pin.electrical}`));
      if (pin.reason) item.append(element(this.document, "span", "pin-explanation", pin.reason));
      if (pin.bus) item.append(element(this.document, "span", "pin-explanation", `Bus: ${pin.bus}`));
      for (const owner of pin.owners) {
        const button = element(this.document, "button", "owner-link", `${owner.path} — ${owner.mode}${owner.role ? ` — ${owner.role}` : ""}`);
        button.type = "button";
        button.disabled = !owner.path.includes(":");
        button.addEventListener("click", () => this.onNavigate(owner.path));
        item.append(button);
      }
      const effective = pin.owners.map((owner) => owner.path).join(", ") || "none";
      const configured = pin.configuredOwners.join(", ") || "none";
      item.append(element(this.document, "span", "pin-explanation", `Configured: ${configured}; effective: ${effective}`));
      list.append(item);
    }
    this.root.append(list);
  }
}
