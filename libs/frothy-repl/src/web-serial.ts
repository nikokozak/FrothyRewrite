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
  private released: Promise<void> | null = null;
  private writeChain: Promise<void> = Promise.resolve();
  private readonly port: WebSerialPort;

  constructor(port: WebSerialPort) {
    this.port = port;
  }

  // Serialize writes: an out-of-band interrupt (0x03) and a queued send can be
  // issued concurrently, and `writable.getWriter()` throws if the stream is
  // already locked to another writer. Chaining every write keeps the two from
  // racing for the lock; each write is a few bytes, so the wait is negligible.
  write(bytes: Uint8Array): Promise<void> {
    const run = async (): Promise<void> => {
      const writable = this.port.writable;
      if (!writable) throw new Error("serial port is not writable");
      const writer = writable.getWriter();
      try {
        await writer.write(bytes);
      } finally {
        writer.releaseLock();
      }
    };
    const next = this.writeChain.then(run, run);
    this.writeChain = next.catch(() => {});
    return next;
  }

  async *read(): AsyncIterable<Uint8Array> {
    const readable = this.port.readable;
    if (!readable) return;
    const reader = readable.getReader();
    this.reader = reader;
    let release!: () => void;
    this.released = new Promise<void>((r) => (release = r));
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        if (value) yield value;
      }
    } finally {
      reader.releaseLock();
      this.reader = null;
      release();
    }
  }

  async close(): Promise<void> {
    // Cancel the parked read, then wait for the generator's finally to run
    // releaseLock() before closing the port, so the port lock is provably free.
    if (this.reader) {
      await this.reader.cancel();
      await this.released;
    }
    await this.port.close();
  }
}
