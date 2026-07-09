// The public vocabulary. Names and shapes track docs/wire-protocol.md 1:1.

/** A response terminator, classified from the wire. */
export type Response =
  | { kind: "ok" }
  | { kind: "value"; lines: string[] }
  | { kind: "error"; code: number; phrase: string | null };

/** The device's self-advertisement. Mirrors the v1 status line field order. */
export type Status = {
  version: 1;
  profile: string;
  profile_hash: string;
  compiler: "device" | "host-required" | "unknown";
  names: "device" | "host" | "none";
  storage: "eeprom" | "volatile";
  interrupt: string; // "cooperative" today; tolerate others
  word_size: 16 | 32;
  int_min: number;
  int_max: number;
  apply_bytes: number;
};

export type Unsubscribe = () => void;

export interface ReplConnector {
  sendLine(line: string): Promise<Response>;
  /** Send a raw Ctrl-C (0x03) out-of-band to interrupt a running command.
   *  Bypasses the send queue; does not wait for a response. */
  interrupt(): Promise<void>;
  onLine(cb: (line: string) => void): Unsubscribe;
  /** Fires once when the connector becomes closed, whether by read end/failure or close(). */
  onClose(cb: () => void): Unsubscribe;
  status(): Promise<Status>;
  apply(bytes: Uint8Array): Promise<Response>;
  run(bytes: Uint8Array): Promise<Response>;
  transcript(): readonly string[];
  close(): Promise<void>;
}

/** A byte sink/source. The library never assumes serial specifics. */
export interface Transport {
  write(bytes: Uint8Array): Promise<void>;
  read(): AsyncIterable<Uint8Array>;
  close(): Promise<void>;
}

/** The transport failed: port closed, write rejected, read threw. */
export class TransportError extends Error {
  readonly kind = "transport";
  constructor(message: string, options?: { cause?: unknown }) {
    super(message, options);
    this.name = "TransportError";
  }
}

/** The wire was malformed or announced an unsupported version. */
export class WireFormatError extends Error {
  readonly kind = "wire-format";
  constructor(message: string) {
    super(message);
    this.name = "WireFormatError";
  }
}
