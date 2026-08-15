const VALID_STATES = new Set(["free", "exclusive", "shared", "reserved"]);

function record(value, label) {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError(`${label} must be an object`);
  }
  return value;
}

export function normalizeResourceSnapshot(input) {
  const source = record(input, "Resource snapshot");
  if (source.schema_version !== 1 || !Number.isInteger(source.allocation_revision)) {
    throw new TypeError("Unsupported resource snapshot");
  }
  const board = record(source.board, "Board");
  if (!Array.isArray(source.pins)) throw new TypeError("Resource pins must be an array");
  const ids = new Set();
  const pins = source.pins.map((raw, index) => {
    const pin = record(raw, `Pin ${index}`);
    if (typeof pin.id !== "string" || ids.has(pin.id) || !Number.isInteger(pin.gpio)) {
      throw new TypeError(`Invalid resource pin at index ${index}`);
    }
    ids.add(pin.id);
    const owners = Array.isArray(pin.owners)
      ? pin.owners.map((owner, ownerIndex) => {
          const value = record(owner, `Owner ${ownerIndex}`);
          if (typeof value.path !== "string" || typeof value.mode !== "string") {
            throw new TypeError(`Invalid owner on ${pin.id}`);
          }
          return {
            path: value.path,
            role: typeof value.role === "string" ? value.role : "",
            mode: value.mode,
            memberKey: Number.isInteger(value.member_key) ? value.member_key : 0,
            configurable: value.configurable === true || value.path.includes(":"),
            optional: value.optional === true,
            rebootRequired: value.reboot_required === true,
          };
        })
      : [];
    return {
      id: pin.id,
      label: typeof pin.label === "string" ? pin.label : pin.id,
      gpio: pin.gpio,
      capabilities: Number.isInteger(pin.capabilities) ? pin.capabilities : 0,
      electrical: typeof pin.electrical === "string" ? pin.electrical : "",
      selectable: pin.selectable === true,
      state: VALID_STATES.has(pin.state) ? pin.state : "reserved",
      reason: typeof pin.reason === "string" ? pin.reason : "unknown-resource-state",
      bus: typeof pin.bus === "string" ? pin.bus : "",
      owners,
      configuredOwners: Array.isArray(pin.configured_owners)
        ? pin.configured_owners.filter((owner) => typeof owner === "string")
        : [],
    };
  });
  return {
    schemaVersion: 1,
    revision: source.allocation_revision,
    board: {
      id: typeof board.id === "string" ? board.id : "unknown-board",
      target: typeof board.target === "string" ? board.target : "unknown-target",
      antenna: typeof board.antenna === "string" ? board.antenna : "",
    },
    pins,
  };
}

export function pinOutcome(pin, control) {
  const selector = control.resourceSelector;
  const ownerPath = control.resourceOwner;
  const own = pin.owners.find((owner) => owner.path === ownerPath);
  if (own) return { state: "selected", disabled: false, reason: `Selected by ${ownerPath}`, owners: pin.owners };
  if (!pin.selectable || pin.state === "reserved") {
    return { state: "reserved", disabled: true, reason: pin.reason || "Board-critical reservation", owners: pin.owners };
  }
  const missing = selector.requiredCapabilities & ~pin.capabilities;
  if (missing !== 0) {
    return { state: "incompatible", disabled: true, reason: `Missing capabilities 0x${missing.toString(16)}`, owners: pin.owners };
  }
  if (pin.owners.length === 0) return { state: "free", disabled: false, reason: "Free and compatible", owners: [] };
  const compatibleShared = pin.owners.every((owner) => owner.mode === "shared-read" || owner.mode === "bus-member");
  if (compatibleShared) return { state: "shared", disabled: false, reason: `Shared with ${pin.owners.map((owner) => owner.path).join(", ")}`, owners: pin.owners };
  const configurable = pin.owners.filter((owner) => owner.configurable);
  if (configurable.length === pin.owners.length && configurable.length > 0) {
    return { state: "conflict", disabled: false, reason: `Assigned to ${configurable.map((owner) => owner.path).join(", ")}`, owners: configurable };
  }
  return { state: "reserved", disabled: true, reason: pin.reason || `Owned by ${pin.owners.map((owner) => owner.path).join(", ")}`, owners: pin.owners };
}

export function buildReassignment({ snapshot, control, pin, operation }) {
  const outcome = pinOutcome(pin, control);
  if (outcome.state !== "conflict" || outcome.owners.length !== 1) {
    throw new TypeError("Pin does not have one reassignable owner");
  }
  const previous = outcome.owners[0];
  if (operation === "swap" && !control.resourceSelector.supportsSwap) {
    throw new TypeError("Swap is not supported");
  }
  if (operation === "unassign-and-move" &&
      (!control.resourceSelector.supportsMove || !previous.optional)) {
    throw new TypeError("Previous assignment is required");
  }
  return {
    schema_version: 1,
    expected_revision: snapshot.revision,
    operation,
    requester: control.resourceOwner,
    previous_owner: previous.path,
    target_resource: pin.id,
  };
}

export function filterPins(snapshot, query) {
  const needle = query.trim().toLocaleLowerCase();
  if (!needle) return snapshot.pins;
  return snapshot.pins.filter((pin) =>
    `${pin.label} ${pin.id} ${pin.gpio} ${pin.electrical} ${pin.reason} ${pin.bus} ${pin.owners.map((owner) => owner.path).join(" ")}`
      .toLocaleLowerCase()
      .includes(needle),
  );
}
