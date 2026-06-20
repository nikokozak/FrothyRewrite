import type { ReplConnector, Response, Status, Transport, Unsubscribe } from "./types.ts";
import { TransportError, WireFormatError } from "./types.ts";

const PROMPT = "> ";
const ERROR_RE = /^error: (.+) \((\d+)\)$/;
const LEGACY_ERROR_RE = /^err (\d+)$/;

type Pending = {
  lines: string[];
  terminator: Response | null;
  resolve: (r: Response) => void;
  reject: (e: Error) => void;
};

class Connector implements ReplConnector {
  private readonly subscribers = new Set<(line: string) => void>();
  private readonly lines: string[] = [];
  private readonly decoder = new TextDecoder();
  private readonly encoder = new TextEncoder();
  private buf = "";
  private pending: Pending | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private applyBytes: number | null = null;
  private closed = false;

  constructor(private readonly transport: Transport) {
    void this.pump();
  }

  private async pump(): Promise<void> {
    try {
      for await (const chunk of this.transport.read()) {
        if (this.closed) break;
        this.feed(chunk);
      }
    } catch (err) {
      this.failPending(new TransportError("transport read failed", { cause: err }));
    }
  }

  private feed(chunk: Uint8Array): void {
    this.buf += this.decoder.decode(chunk, { stream: true });
    // buf[0] is always a line start: complete lines are consumed with their LF.
    // A bare "> " at a line start is the prompt; value text is quoted, so no
    // value line begins with a bare "> ".
    // ponytail: this also strips a leading "> " from words/see output if a word
    // is literally named ">". Those commands are out of TR's strict contract.
    for (;;) {
      if (this.buf.startsWith(PROMPT)) {
        this.buf = this.buf.slice(PROMPT.length);
        this.onPrompt();
        continue;
      }
      const nl = this.buf.indexOf("\n");
      if (nl < 0) break;
      const line = this.buf.slice(0, nl);
      this.buf = this.buf.slice(nl + 1);
      this.onLine(line);
    }
  }

  private onLine(line: string): void {
    this.lines.push(line);
    for (const cb of this.subscribers) cb(line);

    const p = this.pending;
    if (!p || p.terminator) return;
    const term = classify(line);
    if (term) p.terminator = term;
    else p.lines.push(line);
  }

  private onPrompt(): void {
    const p = this.pending;
    if (!p || !p.terminator) return; // banner / pre-request prompt
    this.pending = null;
    p.resolve(finalize(p));
  }

  private failPending(err: Error): void {
    const p = this.pending;
    if (!p) return;
    this.pending = null;
    p.reject(err);
  }

  private send(line: string): Promise<Response> {
    const run = (): Promise<Response> => {
      const result = new Promise<Response>((resolve, reject) => {
        this.pending = { lines: [], terminator: null, resolve, reject };
      });
      this.transport.write(this.encoder.encode(line + "\n")).catch((err) => {
        this.failPending(new TransportError("transport write failed", { cause: err }));
      });
      return result;
    };
    const next = this.queue.then(run, run);
    this.queue = next.catch(() => {});
    return next;
  }

  sendLine(line: string): Promise<Response> {
    if (/[\n\r]/.test(line)) {
      return Promise.reject(new RangeError("line must not contain LF or CR"));
    }
    return this.send(line);
  }

  onLine(cb: (line: string) => void): Unsubscribe {
    this.subscribers.add(cb);
    return () => {
      this.subscribers.delete(cb);
    };
  }

  async status(): Promise<Status> {
    const res = await this.send("status");
    if (res.kind !== "value") {
      throw new WireFormatError("status did not return a status line");
    }
    const status = parseStatus(res.lines);
    this.applyBytes = status.apply_bytes;
    return status;
  }

  apply(bytes: Uint8Array): Promise<Response> {
    return this.binary("apply", bytes);
  }

  run(bytes: Uint8Array): Promise<Response> {
    return this.binary("run", bytes);
  }

  private binary(cmd: "apply" | "run", bytes: Uint8Array): Promise<Response> {
    if (this.applyBytes !== null && bytes.length > this.applyBytes) {
      return Promise.reject(
        new RangeError(`payload of ${bytes.length} exceeds apply_bytes ${this.applyBytes}`),
      );
    }
    return this.send(`${cmd} ${toHex(bytes)}`);
  }

  transcript(): readonly string[] {
    return this.lines;
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    await this.transport.close();
  }
}

function classify(line: string): Response | null {
  if (line === "ok") return { kind: "ok" };
  const m = ERROR_RE.exec(line);
  if (m) return { kind: "error", code: Number(m[2]), phrase: m[1] ?? null };
  const legacy = LEGACY_ERROR_RE.exec(line);
  if (legacy) return { kind: "error", code: Number(legacy[1]), phrase: null };
  return null;
}

function finalize(p: Pending): Response {
  const term = p.terminator!;
  if (term.kind === "error") return term;
  return p.lines.length > 0 ? { kind: "value", lines: p.lines } : { kind: "ok" };
}

function toHex(bytes: Uint8Array): string {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}

function parseStatus(lines: string[]): Status {
  const line = lines.find((l) => l.startsWith("frothy status"));
  if (!line) throw new WireFormatError("no status line in response");
  const parts = line.split(" ");
  if (parts[2] !== "v1") {
    throw new WireFormatError(`unsupported status version: ${parts[2] ?? "<none>"}`);
  }
  const fields = new Map<string, string>();
  for (const part of parts.slice(3)) {
    const eq = part.indexOf("=");
    if (eq > 0) fields.set(part.slice(0, eq), part.slice(eq + 1));
  }
  const get = (key: string): string => {
    const v = fields.get(key);
    if (v === undefined) throw new WireFormatError(`status missing field: ${key}`);
    return v;
  };
  const wordSize = Number(get("word_size"));
  if (wordSize !== 16 && wordSize !== 32) {
    throw new WireFormatError(`bad word_size: ${get("word_size")}`);
  }
  return {
    version: 1,
    profile: get("profile"),
    profile_hash: get("profile_hash"),
    compiler: get("compiler") as Status["compiler"],
    names: get("names") as Status["names"],
    storage: get("storage") as Status["storage"],
    interrupt: get("interrupt"),
    word_size: wordSize,
    int_min: Number(get("int_min")),
    int_max: Number(get("int_max")),
    apply_bytes: Number(get("apply_bytes")),
  };
}

export function createConnector(transport: Transport): Promise<ReplConnector> {
  return Promise.resolve(new Connector(transport));
}
