import type { Transport } from "./types.ts";

/** The slice of a `serialport` SerialPort that TR uses. It is a Node Duplex. */
export interface NodeSerialPort extends AsyncIterable<Uint8Array> {
  write(data: Uint8Array, cb: (err?: Error | null) => void): boolean;
  close(cb: (err?: Error | null) => void): void;
}

/** Wraps an already-open `serialport` instance as a {@link Transport}. */
export class NodeSerialTransport implements Transport {
  constructor(private readonly port: NodeSerialPort) {}

  write(bytes: Uint8Array): Promise<void> {
    return new Promise((resolve, reject) => {
      this.port.write(bytes, (err) => (err ? reject(err) : resolve()));
    });
  }

  read(): AsyncIterable<Uint8Array> {
    return this.port;
  }

  close(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.port.close((err) => (err ? reject(err) : resolve()));
    });
  }
}
