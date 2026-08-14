const VALID_KINDS = new Set(["parameter", "action", "event"]);
const MAX_TREE_DEPTH = 16;

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function text(value, fallback = "") {
  return typeof value === "string" && value.length > 0 ? value : fallback;
}

function typeFromTag(tag) {
  switch (tag) {
    case "T":
    case "F":
    case "b":
      return "boolean";
    case "i":
      return "integer";
    case "f":
      return "number";
    case "s":
      return "string";
    case "I":
      return "impulse";
    case "r":
      return "rgba";
    case "m":
      return "midi";
    case "t":
      return "timetag";
    default:
      return "unsupported";
  }
}

function inferKind(node, writable) {
  if (VALID_KINDS.has(node.BLIP_KIND)) {
    return node.BLIP_KIND;
  }
  if (node.TYPE === "I" && writable) {
    return "action";
  }
  return "parameter";
}

function normalizeFields(node, typeTags) {
  if (Array.isArray(node.BLIP_FIELDS)) {
    return node.BLIP_FIELDS.map((field, index) => {
      if (!isRecord(field)) {
        throw new TypeError(`Invalid BLIP field at index ${index}`);
      }
      const tag = text(field.TYPE, typeTags[index] ?? "");
      return {
        id: text(field.ID, `value_${index + 1}`),
        type: typeFromTag(tag),
        tag,
        required: field.REQUIRED !== false,
      };
    });
  }
  return typeTags
    .filter((tag) => tag !== "I")
    .map((tag, index) => ({
      id: `value_${index + 1}`,
      type: typeFromTag(tag),
      tag,
      required: true,
    }));
}

function normalizeRange(node) {
  const source = Array.isArray(node.RANGE) && isRecord(node.RANGE[0]) ? node.RANGE[0] : {};
  const values = Array.isArray(source.VALS)
    ? source.VALS.filter((value) => ["boolean", "number", "string"].includes(typeof value))
    : [];
  const minimum = Number.isFinite(source.MIN) ? source.MIN : undefined;
  const maximum = Number.isFinite(source.MAX) ? source.MAX : undefined;
  const step = Number.isFinite(node.BLIP_STEP) && node.BLIP_STEP > 0 ? node.BLIP_STEP : undefined;
  return { values, minimum, maximum, step };
}

function editorFor(control) {
  if (control.kind === "action") return "action";
  if (control.kind === "event" || !control.writable) return "readonly";
  if (control.range.values.length > 0) return "select";
  if (control.type === "boolean") return "checkbox";
  if (control.type === "integer" || control.type === "number") return "number";
  if (control.type === "string") return control.readable ? "text" : "password";
  return "unsupported";
}

function normalizeControl(key, node, parentPath) {
  if (!isRecord(node)) {
    throw new TypeError(`Invalid OSCQuery node ${key}`);
  }
  const path = text(node.FULL_PATH, `${parentPath}/${key}`);
  if (!path.startsWith("/") || path.includes("\0")) {
    throw new TypeError(`Invalid OSC path ${path}`);
  }
  const access = Number.isInteger(node.ACCESS) ? node.ACCESS : 0;
  const readable = typeof node.BLIP_READABLE === "boolean" ? node.BLIP_READABLE : (access & 1) !== 0;
  const writable = typeof node.BLIP_WRITABLE === "boolean" ? node.BLIP_WRITABLE : (access & 2) !== 0;
  const tags = text(node.TYPE).split("");
  const kind = inferKind(node, writable);
  const value = Array.isArray(node.VALUE) ? node.VALUE[0] : undefined;
  const control = {
    id: key,
    path,
    label: text(node.DESCRIPTION, key),
    kind,
    access,
    readable,
    writable,
    persisted: node.BLIP_PERSISTED === true,
    type: typeFromTag(tags[0] ?? ""),
    tags,
    fields: normalizeFields(node, tags),
    value,
    range: normalizeRange(node),
    unit: text(node.BLIP_UNIT),
  };
  control.editor = editorFor(control);
  return control;
}

export function buildControlModel(tree) {
  if (!isRecord(tree) || !isRecord(tree.CONTENTS)) {
    throw new TypeError("OSCQuery root must contain CONTENTS");
  }
  const components = [];
  const index = new Map();

  function visit(key, node, depth) {
    if (depth > MAX_TREE_DEPTH || !isRecord(node) || !isRecord(node.CONTENTS)) {
      if (depth > MAX_TREE_DEPTH) throw new RangeError("OSCQuery tree exceeds maximum depth");
      throw new TypeError(`Invalid OSCQuery container ${key}`);
    }
    const path = text(node.FULL_PATH);
    const controls = [];
    for (const [childKey, child] of Object.entries(node.CONTENTS)) {
      if (isRecord(child) && isRecord(child.CONTENTS)) {
        visit(childKey, child, depth + 1);
        continue;
      }
      const control = normalizeControl(childKey, child, path);
      if (index.has(control.path)) {
        throw new TypeError(`Duplicate OSC path ${control.path}`);
      }
      controls.push(control);
      index.set(control.path, control);
    }
    if (controls.length > 0) {
      components.push({
        id: text(node.BLIP_COMPONENT_ID, path || key || "root"),
        path,
        label: text(node.DESCRIPTION, key || "Device"),
        schemaVersion: Number.isInteger(node.BLIP_SCHEMA_VERSION) ? node.BLIP_SCHEMA_VERSION : null,
        disablePolicy: text(node.BLIP_DISABLE_POLICY),
        controls,
      });
    }
  }

  visit("root", tree, 0);
  components.sort((left, right) => left.path.localeCompare(right.path));
  return { components, index };
}

export function filterControlModel(model, query) {
  const needle = query.trim().toLocaleLowerCase();
  if (needle.length === 0) return model.components;
  return model.components
    .map((component) => ({
      ...component,
      controls: component.controls.filter((control) =>
        `${component.label} ${component.path} ${control.label} ${control.path}`
          .toLocaleLowerCase()
          .includes(needle),
      ),
    }))
    .filter((component) => component.controls.length > 0);
}

export function coerceControlValue(type, raw) {
  switch (type) {
    case "boolean":
      if (typeof raw === "boolean") return { type, value: raw };
      if (raw === "true") return { type, value: true };
      if (raw === "false") return { type, value: false };
      break;
    case "integer": {
      const value = typeof raw === "number" ? raw : Number(raw);
      if (Number.isInteger(value) && value >= -2147483648 && value <= 2147483647) {
        return { type, value };
      }
      break;
    }
    case "number": {
      const value = typeof raw === "number" ? raw : Number(raw);
      if (Number.isFinite(value)) return { type, value };
      break;
    }
    case "string":
      return { type, value: String(raw) };
    default:
      break;
  }
  throw new TypeError(`Value does not match ${type}`);
}

export function describeHost(host) {
  if (!isRecord(host)) throw new TypeError("OSCQuery host info must be an object");
  return {
    name: text(host.NAME, "BLIP device"),
    id: text(host.DEVICE_ID, "unknown device"),
    type: text(host.DEVICE_TYPE, "BLIP"),
    version: text(host.VERSION, "unknown version"),
  };
}
