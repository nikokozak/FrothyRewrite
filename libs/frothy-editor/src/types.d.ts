// Web Serial isn't in the default DOM lib yet. Minimal shape used by
// frothy-editor and @frothy/repl's WebSerialTransport.

declare interface SerialPortRequestOptions {
  filters?: { usbVendorId?: number; usbProductId?: number }[];
}

declare interface SerialOptions {
  baudRate: number;
}

declare interface SerialPort {
  open(options: SerialOptions): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array>;
  writable: WritableStream<Uint8Array>;
}

declare interface Serial {
  requestPort(options?: SerialPortRequestOptions): Promise<SerialPort>;
}

declare interface Navigator {
  readonly serial?: Serial;
}
