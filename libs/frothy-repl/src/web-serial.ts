import type { Transport } from "./types.ts";

/** The slice of a Web Serial `SerialPort` that TR uses. The consumer opens it. */
export interface WebSerialPort {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  close(): Promise<void>;
}

/** Wraps an already-open Web Serial `SerialPort` as a {@link Transport}. */
export class WebSerialTransport implements Transport {
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;

  constructor(private readonly port: WebSerialPort) {}

  async write(bytes: Uint8Array): Promise<void> {
    const writable = this.port.writable;
    if (!writable) throw new Error("serial port is not writable");
    const writer = writable.getWriter();
    try {
      await writer.write(bytes);
    } finally {
      writer.releaseLock();
    }
  }

  async *read(): AsyncIterable<Uint8Array> {
    const readable = this.port.readable;
    if (!readable) return;
    const reader = readable.getReader();
    this.reader = reader;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        if (value) yield value;
      }
    } finally {
      reader.releaseLock();
      this.reader = null;
    }
  }

  async close(): Promise<void> {
    // Cancel the parked read so the reader releases its lock before close.
    if (this.reader) await this.reader.cancel();
    await this.port.close();
  }
}
