import type { Transport } from "./types.ts";

/**
 * Deterministic in-memory transport for tests. It speaks the wire-protocol
 * byte shape: a responder maps each written request line to the raw device
 * output (response lines, terminator, and prompt).
 */
export class FakeTransport implements Transport {
  readonly writes: string[] = [];
  failWrite = false;
  closeCount = 0;

  private readonly queue: Uint8Array[] = [];
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();
  private resolveNext: (() => void) | null = null;
  private closed = false;
  private readonly responder?: (line: string) => string;

  constructor(responder?: (line: string) => string) {
    this.responder = responder;
  }

  /** Push raw device bytes for the connector to read. */
  emit(text: string): void {
    this.queue.push(this.encoder.encode(text));
    const wake = this.resolveNext;
    this.resolveNext = null;
    if (wake) wake();
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (this.failWrite) throw new Error("fake write failure");
    const line = this.decoder.decode(bytes).replace(/\n$/, "");
    this.writes.push(line);
    if (this.responder) this.emit(this.responder(line));
  }

  async *read(): AsyncIterable<Uint8Array> {
    for (;;) {
      while (this.queue.length > 0) yield this.queue.shift()!;
      if (this.closed) return;
      await new Promise<void>((resolve) => {
        this.resolveNext = resolve;
      });
    }
  }

  async close(): Promise<void> {
    this.closeCount += 1;
    this.closed = true;
    const wake = this.resolveNext;
    this.resolveNext = null;
    if (wake) wake();
  }
}
