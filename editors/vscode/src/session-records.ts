export const SESSION_STATES = [
  'syncing',
  'idle',
  'waiting',
  'interrupting',
  'stale',
  'error',
  'closed',
] as const;

export const MIRROR_STATES = ['none', 'clean', 'pending', 'stale'] as const;

export type SessionState = (typeof SESSION_STATES)[number];
export type MirrorState = (typeof MIRROR_STATES)[number];

export interface SessionRecord extends Record<string, unknown> {
  v: 1;
  session: string;
  seq: number;
  kind: string;
  state: SessionState;
  mirror: MirrorState;
}

export interface SessionIssue {
  kind: string;
  code?: string;
  status?: string;
  message?: string;
  text?: string;
}

export interface SessionSnapshot {
  state: SessionState;
  mirror: MirrorState;
  profile?: string;
  mode?: string;
  lastError?: SessionIssue;
  lastResultText?: string;
}

export function emptySessionSnapshot(): SessionSnapshot {
  return { state: 'closed', mirror: 'none' };
}

export function parseSessionRecord(line: string): SessionRecord {
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch {
    throw new Error('invalid session record JSON');
  }
  if (!isObject(value)) throw new Error('session record must be an object');
  if (value.v !== 1) throw new Error(`unsupported session record version ${String(value.v)}`);
  if (typeof value.session !== 'string' || value.session.length === 0) {
    throw new Error('session record requires a session id');
  }
  if (!Number.isInteger(value.seq) || (value.seq as number) <= 0) {
    throw new Error('session record requires a positive integer sequence');
  }
  if (typeof value.kind !== 'string' || value.kind.length === 0) {
    throw new Error('session record requires a kind');
  }
  if (!SESSION_STATES.includes(value.state as SessionState)) {
    throw new Error(`unknown session state ${String(value.state)}`);
  }
  if (!MIRROR_STATES.includes(value.mirror as MirrorState)) {
    throw new Error(`unknown mirror state ${String(value.mirror)}`);
  }
  return value as SessionRecord;
}

export function recordFailed(record: SessionRecord): boolean {
  return record.kind === 'compile_error' ||
    (record.kind === 'response' && record.ok === false);
}

export function reduceSessionRecord(
  snapshot: SessionSnapshot,
  record: SessionRecord,
): SessionSnapshot {
  const next: SessionSnapshot = {
    ...snapshot,
    state: record.state,
    mirror: record.mirror,
  };

  switch (record.kind) {
    case 'session_start':
      return { state: record.state, mirror: record.mirror };
    case 'status': {
      const device = isObject(record.device) ? record.device : undefined;
      return {
        ...next,
        profile: device ? stringField(device, 'profile') : undefined,
        mode: stringField(record, 'mode'),
        lastError: undefined,
      };
    }
    case 'send':
      return { ...next, lastError: undefined };
    case 'response':
      return {
        ...next,
        lastError: undefined,
        lastResultText: stringField(record, 'text') ?? next.lastResultText,
      };
    case 'compile_error':
    case 'session_error':
      return {
        ...next,
        lastError: issueFrom(record),
        lastResultText: stringField(record, 'text') ?? next.lastResultText,
      };
    case 'interrupt': {
      const code = stringField(record, 'code');
      return {
        ...next,
        lastError: code ? issueFrom(record) : next.lastError,
        lastResultText: stringField(record, 'text') ?? next.lastResultText,
      };
    }
    default:
      return next;
  }
}

function issueFrom(record: SessionRecord): SessionIssue {
  return {
    kind: record.kind,
    code: stringField(record, 'code'),
    status: stringField(record, 'status'),
    message: stringField(record, 'message'),
    text: stringField(record, 'text'),
  };
}

function stringField(value: Record<string, unknown>, key: string): string | undefined {
  return typeof value[key] === 'string' ? value[key] : undefined;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
