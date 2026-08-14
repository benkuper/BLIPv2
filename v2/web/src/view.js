import { coerceControlValue, filterControlModel } from "./model.js";

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
  constructor({ document, root, emptyTemplate, onSend, onError }) {
    this.document = document;
    this.root = root;
    this.emptyTemplate = emptyTemplate;
    this.onSend = onSend;
    this.onError = onError;
    this.model = null;
    this.query = "";
    this.rows = new Map();
  }

  setModel(model) {
    this.model = model;
    this.render();
  }

  setFilter(query) {
    this.query = query;
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
